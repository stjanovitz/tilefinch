#include "tilefinch/media_hls.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "swdec/swdec_ts.h"
#include "tilefinch/url.h"

#define HLS_SAMPLE_LIMIT 64u
#define HLS_QUEUE_BYTES (576u * 1024u)
#define HLS_PARAMETER_SET_BYTES 4096u

typedef struct {
    uint32_t text_offset;
    uint32_t start_ms;
    uint16_t text_length;
    uint16_t width;
    uint16_t height;
    uint32_t duration_ms;
    uint32_t bandwidth;
    bool discontinuity;
} HlsEntry;

struct MediaHlsPlaylist {
    Budget *budget;
    char *text;
    size_t text_length;
    char base_url[4096];
    HlsEntry entries[MEDIA_HLS_MAXIMUM_SEGMENTS];
    size_t entry_count;
    uint64_t duration_us;
    MediaHlsPlaylistKind kind;
    bool end_list;
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

struct MediaHlsSource {
    Budget *budget;
    MediaHlsPlaylist *playlist;
    MediaHlsTransport transport;
    SwdecTs *ts;
    unsigned char *queue_bytes;
    unsigned char *transport_chunk;
    HlsQueuedSample queue[HLS_SAMPLE_LIMIT];
    size_t queue_head;
    size_t queue_count;
    size_t queue_read;
    size_t queue_write;
    size_t queue_used;
    uint64_t next_identity;
    HlsRequest requests[2];
    size_t segment_index;
    size_t prepared_segment;
    size_t transport_tail;
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
    uint64_t last_video_pts90k;
    uint64_t last_audio_pts90k;
    uint32_t audio_duration90k;
    MediaMp4TrackInfo video_info;
    MediaMp4TrackInfo audio_info;
    unsigned char parameter_sets[HLS_PARAMETER_SET_BYTES];
    size_t parameter_set_bytes;
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
    const char *at = strstr(line, name);
    if (at == NULL) return false;
    at += strlen(name);
    if (*at != '=') return false;
    at++;
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
            entry->bandwidth = pending_bandwidth;
            entry->width = pending_width;
            entry->height = pending_height;
            pending_variant = false;
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
            && !playlist->end_list)) {
        hls_error(error, error_size,
                  playlist->entry_count == 0
                      ? "HLS playlist is empty"
                      : "live HLS playlists are unsupported");
        media_hls_playlist_destroy(playlist);
        return NULL;
    }
    return playlist;
}

MediaHlsPlaylistKind media_hls_playlist_kind(const MediaHlsPlaylist *playlist)
{
    return playlist == NULL ? MEDIA_HLS_PLAYLIST_MEDIA : playlist->kind;
}

bool media_hls_playlist_select_variant(
    const MediaHlsPlaylist *playlist, unsigned maximum_width,
    unsigned maximum_height, unsigned target_height,
    char *url, size_t url_size)
{
    if (playlist == NULL || playlist->kind != MEDIA_HLS_PLAYLIST_MASTER
        || url == NULL || url_size == 0) return false;
    size_t selected = SIZE_MAX;
    unsigned selected_class = UINT32_MAX;
    uint32_t selected_cost = UINT32_MAX;
    for (size_t i = 0; i < playlist->entry_count; i++) {
        const HlsEntry *entry = &playlist->entries[i];
        if (entry->width != 0 && entry->height != 0
            && (entry->width > maximum_width
                || entry->height > maximum_height)) continue;
        unsigned candidate_class;
        uint32_t cost;
        if (entry->height == 0) {
            candidate_class = 2u;
            cost = entry->bandwidth;
        } else if (entry->height >= target_height) {
            candidate_class = 0u;
            cost = ((uint32_t) entry->height << 16u)
                 | (entry->bandwidth > 0xffffu ? 0xffffu
                                               : entry->bandwidth);
        } else {
            candidate_class = 1u;
            cost = ((uint32_t) (maximum_height - entry->height) << 16u)
                 | (entry->bandwidth > 0xffffu ? 0xffffu
                                               : entry->bandwidth);
        }
        if (selected == SIZE_MAX || candidate_class < selected_class
            || (candidate_class == selected_class && cost < selected_cost)) {
            selected = i;
            selected_class = candidate_class;
            selected_cost = cost;
        }
    }
    if (selected == SIZE_MAX) return false;
    const HlsEntry *entry = &playlist->entries[selected];
    const char *reference = playlist->text + entry->text_offset;
    return tilefinch_url_resolve(
        playlist->base_url, reference, url, url_size);
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

void media_hls_playlist_destroy(MediaHlsPlaylist *playlist)
{
    if (playlist == NULL) return;
    Budget *budget = playlist->budget;
    budget_free(budget, playlist->text);
    budget_free(budget, playlist);
}

static bool hls_queue_write(MediaHlsSource *source,
                            const unsigned char *data, size_t length,
                            uint32_t *offset)
{
    if (length > HLS_QUEUE_BYTES - source->queue_used) return false;
    *offset = (uint32_t) source->queue_write;
    size_t first = HLS_QUEUE_BYTES - source->queue_write;
    if (first > length) first = length;
    memcpy(source->queue_bytes + source->queue_write, data, first);
    memcpy(source->queue_bytes, data + first, length - first);
    source->queue_write = (source->queue_write + length) % HLS_QUEUE_BYTES;
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
    bool keyframe = false;
    (void) hls_annexb_info(source, data, length, &keyframe);
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
    uint32_t start_ms = source->playlist->entries[segment].start_ms;
    source->segment_base90k =
        (uint64_t) (start_ms / 1000u) * UINT64_C(90000)
        + (uint32_t) (start_ms % 1000u) * 90u;
    source->segment_origin90k = 0;
    source->segment_origin_valid = false;
    source->prepared_segment = segment;
}

static void hls_complete_segment(MediaHlsSource *source)
{
    /* A TS segment is an independent delivery boundary. Publish its final
       access unit while the current segment's timestamp base is still live;
       otherwise that AU is emitted only after prepare_segment installs the
       next base and appears to jump at every boundary. */
    swdec_ts_flush(source->ts);
    source->stats.segments_completed++;
    source->segment_index++;
    source->requests[0] = (HlsRequest) {0};
    if (source->requests[1].handle != 0
        && source->requests[1].segment == source->segment_index) {
        source->requests[0] = source->requests[1];
        source->requests[1] = (HlsRequest) {0};
    }
    if (source->segment_index >= source->playlist->entry_count) {
        source->ended = true;
    }
}

static void hls_pump(MediaHlsSource *source)
{
    if (source == NULL || source->failed || source->ended) return;
    if (source->requests[0].handle == 0) {
        (void) hls_start_request(source, 0u, source->segment_index);
        if (source->requests[0].handle == 0) return;
    }
    if (source->requests[1].handle == 0
        && source->segment_index + 1u < source->playlist->entry_count) {
        (void) hls_start_request(source, 1u, source->segment_index + 1u);
    }
    hls_prepare_segment(source, source->segment_index);
    size_t length = 0;
    char error[160] = {0};
    MediaHlsTransportPollResult result = source->transport.poll(
        source->transport.opaque, source->requests[0].handle,
        source->transport_chunk + source->transport_tail,
        MEDIA_HLS_TRANSPORT_CHUNK_BYTES, &length, error, sizeof(error));
    if (result == MEDIA_HLS_TRANSPORT_WAIT) return;
    if (result == MEDIA_HLS_TRANSPORT_ERROR) {
        hls_source_fail(source, "%s",
                        error[0] == '\0' ? "HLS segment fetch failed" : error);
        return;
    }
    if (result == MEDIA_HLS_TRANSPORT_CHUNK) {
        if (length == 0 || length > MEDIA_HLS_TRANSPORT_CHUNK_BYTES) {
            hls_source_fail(source, "HLS transport returned an invalid chunk");
            return;
        }
        source->stats.bytes_received += length;
        size_t total = source->transport_tail + length;
        int tail = swdec_ts_feed(source->ts, source->transport_chunk, total);
        if (tail < 0 || (size_t) tail >= 188u || (size_t) tail > total) {
            hls_source_fail(source, "HLS TS tail exceeded its bound");
            return;
        }
        source->transport_tail = (size_t) tail;
        if (source->transport_tail != 0) {
            memmove(source->transport_chunk,
                    source->transport_chunk + total - source->transport_tail,
                    source->transport_tail);
        }
    } else if (result == MEDIA_HLS_TRANSPORT_COMPLETE) {
        if (source->transport_tail != 0) {
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
    if (error != NULL && error_size != 0) error[0] = '\0';
    if (budget == NULL || playlist == NULL
        || playlist->kind != MEDIA_HLS_PLAYLIST_MEDIA
        || transport == NULL || transport->start == NULL
        || transport->poll == NULL || transport->cancel == NULL) {
        hls_error(error, error_size, "invalid HLS source");
        return NULL;
    }
    MediaHlsSource *source = budget_calloc_category(
        budget, BUDGET_CATEGORY_NAVIGATION, 1, sizeof(*source));
    SwdecTs *ts = budget_malloc_category(
        budget, BUDGET_CATEGORY_RESOURCE, sizeof(*ts));
    unsigned char *queue = budget_malloc_category(
        budget, BUDGET_CATEGORY_RESOURCE, HLS_QUEUE_BYTES);
    unsigned char *chunk = budget_malloc_category(
        budget, BUDGET_CATEGORY_RESOURCE,
        MEDIA_HLS_TRANSPORT_CHUNK_BYTES + 188u);
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
    source->transport_chunk = chunk;
    source->prepared_segment = SIZE_MAX;
    source->seek_segment = SIZE_MAX;
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
    bool layout_known = source->track_layout_known
        || source->audio_info_valid
        || (source->ts->video_pid >= 0 && source->ts->audio_pid < 0);
    if (source->video_info_valid && layout_known
        && hls_queue_has_kind(source, MEDIA_MP4_TRACK_VIDEO)) {
        source->track_layout_known = true;
        return MEDIA_HLS_PRIME_READY;
    }
    hls_pump(source);
    if (source->failed) {
        hls_error(error, error_size, "%s", source->error);
        return MEDIA_HLS_PRIME_FAILED;
    }
    layout_known = source->track_layout_known
        || source->audio_info_valid
        || (source->ts->video_pid >= 0 && source->ts->audio_pid < 0);
    bool ready = source->video_info_valid && layout_known
        && hls_queue_has_kind(source, MEDIA_MP4_TRACK_VIDEO);
    if (ready) {
        source->track_layout_known = true;
    }
    return ready ? MEDIA_HLS_PRIME_READY : MEDIA_HLS_PRIME_PENDING;
}

static size_t hls_track_count(const void *opaque)
{
    const MediaHlsSource *source = opaque;
    return source->audio_info_valid ? 2u : 1u;
}

static bool hls_track_info(const void *opaque, size_t index,
                           MediaMp4TrackInfo *info)
{
    const MediaHlsSource *source = opaque;
    if (source == NULL || info == NULL) return false;
    if (index == 0 && source->video_info_valid) {
        *info = source->video_info;
        info->duration = (source->playlist->duration_us / 1000000u) * 90000u
            + ((source->playlist->duration_us % 1000000u) * 90000u)
                / 1000000u;
        return true;
    }
    if (index == 1 && source->audio_info_valid) {
        *info = source->audio_info;
        info->duration = (source->playlist->duration_us / 1000000u) * 90000u
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
    if (source->queue_count == 0) hls_pump(source);
    if (source->queue_count == 0) return false;
    const HlsQueuedSample *queued = &source->queue[source->queue_head];
    uint64_t dts = queued->raw_pts90k;
    *sample = (MediaMp4Sample) {
        .track_index = queued->kind == MEDIA_MP4_TRACK_AUDIO ? 1u : 0u,
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
    size_t first = HLS_QUEUE_BYTES - queued->payload_offset;
    if (first > queued->payload_length) first = queued->payload_length;
    memcpy(destination, source->queue_bytes + queued->payload_offset, first);
    memcpy((unsigned char *) destination + first, source->queue_bytes,
           queued->payload_length - first);
    source->queue_read = (queued->payload_offset + queued->payload_length)
        % HLS_QUEUE_BYTES;
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
    (void) hls_seek_common(opaque, 0, false, NULL);
}

static size_t hls_retained_bytes(const void *opaque)
{
    const MediaHlsSource *source = opaque;
    if (source == NULL) return 0;
    return sizeof(*source) + sizeof(*source->ts) + HLS_QUEUE_BYTES
        + MEDIA_HLS_TRANSPORT_CHUNK_BYTES + 188u
        + sizeof(*source->playlist) + source->playlist->text_length + 1u;
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
    if (source == NULL || video == NULL || !source->video_info_valid)
        return false;
    *video = source->video_info;
    if (audio != NULL) {
        *audio = source->audio_info_valid
            ? source->audio_info : (MediaMp4TrackInfo) {0};
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
        stats->ended = source->ended;
    }
}

void media_hls_source_destroy(MediaHlsSource *source)
{
    if (source == NULL) return;
    hls_cancel_requests(source);
    Budget *budget = source->budget;
    media_hls_playlist_destroy(source->playlist);
    budget_free(budget, source->transport_chunk);
    budget_free(budget, source->queue_bytes);
    budget_free(budget, source->ts);
    budget_free(budget, source);
}
