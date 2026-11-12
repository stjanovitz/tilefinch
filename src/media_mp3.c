#include "tilefinch/media_mp3.h"
#include "tilefinch/url.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

/* Bytes one delivery visit moves from the transport into the ring. */
#define MP3_VISIT_BYTES (32u * 1024u)
/* Ring space below which delivery waits for the consumer. */
#define MP3_HOLD_FREE_BYTES (4u * 1024u)
/* Live reconnect backoff: 500 ms doubling to 4 s. */
#define MP3_RECONNECT_BASE_US UINT64_C(500000)
#define MP3_RECONNECT_MAXIMUM_US UINT64_C(4000000)
/* The longest ID3v2 tag a live stream is believed to carry (cover art
   included). Its size field allows 256 MiB; past this the header is
   scanned over like other junk, so one forged tag cannot mute a station
   for that long. */
#define MP3_LIVE_TAG_LIMIT_BYTES (512u * 1024u)

static const uint16_t mp3_bitrates[2][16] = {
    /* MPEG-1 Layer III */
    {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0},
    /* MPEG-2 and 2.5 Layer III */
    {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0}
};

static const uint32_t mp3_sample_rates[3][3] = {
    {44100u, 48000u, 32000u},
    {22050u, 24000u, 16000u},
    {11025u, 12000u, 8000u}
};

bool media_mp3_parse_header(const unsigned char header[4],
                            MediaMp3FrameHeader *frame)
{
    if (header == NULL || header[0] != 0xffu
        || (header[1] & 0xe0u) != 0xe0u) return false;
    unsigned version_bits = (header[1] >> 3) & 3u;
    unsigned layer_bits = (header[1] >> 1) & 3u;
    unsigned bitrate_index = header[2] >> 4;
    unsigned rate_index = (header[2] >> 2) & 3u;
    /* 01 is a reserved version; 01 is the Layer III code; emphasis 10 is
       reserved. Free format (bitrate 0) has no derivable frame length. */
    if (version_bits == 1u || layer_bits != 1u || rate_index == 3u
        || bitrate_index == 0u || bitrate_index == 15u
        || (header[3] & 3u) == 2u) return false;
    MediaMp3Version version = version_bits == 3u ? MEDIA_MP3_VERSION_1
        : version_bits == 2u ? MEDIA_MP3_VERSION_2 : MEDIA_MP3_VERSION_2_5;
    bool mpeg1 = version == MEDIA_MP3_VERSION_1;
    uint32_t bitrate =
        (uint32_t) mp3_bitrates[mpeg1 ? 0 : 1][bitrate_index] * 1000u;
    uint32_t rate = mp3_sample_rates[version][rate_index];
    bool padding = (header[2] & 2u) != 0;
    uint32_t bytes = (mpeg1 ? 144u : 72u) * bitrate / rate
        + (padding ? 1u : 0u);
    if (frame != NULL) {
        *frame = (MediaMp3FrameHeader) {
            .version = version,
            .sample_rate = rate,
            .bitrate = bitrate,
            .channel_mode = (uint8_t) (header[3] >> 6),
            .channels = (header[3] >> 6) == 3u ? 1u : 2u,
            .samples = mpeg1 ? MEDIA_MP3_SAMPLES_MPEG1
                             : MEDIA_MP3_SAMPLES_MPEG2,
            .frame_bytes = (uint16_t) bytes,
            .padding = padding,
            .crc = (header[1] & 1u) == 0
        };
    }
    return bytes > 4u && bytes <= MEDIA_MP3_MAXIMUM_FRAME_BYTES;
}

bool media_mp3_headers_consistent(const MediaMp3FrameHeader *left,
                                  const MediaMp3FrameHeader *right)
{
    return left != NULL && right != NULL && left->version == right->version
        && left->sample_rate == right->sample_rate;
}

size_t media_mp3_id3v2_length(const unsigned char *bytes, size_t length)
{
    if (bytes == NULL || length < 10u || bytes[0] != 'I' || bytes[1] != 'D'
        || bytes[2] != '3' || bytes[3] == 0xffu || bytes[4] == 0xffu
        || ((bytes[6] | bytes[7] | bytes[8] | bytes[9]) & 0x80u) != 0)
        return 0;
    size_t size = ((size_t) bytes[6] << 21) | ((size_t) bytes[7] << 14)
        | ((size_t) bytes[8] << 7) | (size_t) bytes[9];
    return 10u + size + ((bytes[5] & 0x10u) != 0 ? 10u : 0u);
}

static uint32_t mp3_be32(const unsigned char *bytes)
{
    return ((uint32_t) bytes[0] << 24) | ((uint32_t) bytes[1] << 16)
        | ((uint32_t) bytes[2] << 8) | (uint32_t) bytes[3];
}

static size_t mp3_side_info_bytes(const MediaMp3FrameHeader *header)
{
    bool mono = header->channels == 1u;
    return header->version == MEDIA_MP3_VERSION_1 ? (mono ? 17u : 32u)
                                                  : (mono ? 9u : 17u);
}

bool media_mp3_vbr_info(const unsigned char *frame, size_t length,
                        const MediaMp3FrameHeader *header,
                        MediaMp3VbrInfo *info)
{
    if (info != NULL) memset(info, 0, sizeof(*info));
    if (frame == NULL || header == NULL || info == NULL) return false;
    size_t xing = 4u + (header->crc ? 2u : 0u) + mp3_side_info_bytes(header);
    if (xing + 8u <= length
        && (memcmp(frame + xing, "Xing", 4u) == 0
            || memcmp(frame + xing, "Info", 4u) == 0)) {
        uint32_t flags = mp3_be32(frame + xing + 4u);
        size_t at = xing + 8u;
        info->present = true;
        if ((flags & 1u) != 0 && at + 4u <= length) {
            info->frames = mp3_be32(frame + at);
            info->has_frames = info->frames != 0;
            at += 4u;
        }
        if ((flags & 2u) != 0 && at + 4u <= length) {
            info->bytes = mp3_be32(frame + at);
            info->has_bytes = info->bytes != 0;
            at += 4u;
        }
        if ((flags & 4u) != 0 && at + 100u <= length) {
            memcpy(info->toc, frame + at, 100u);
            info->has_toc = true;
        }
        return true;
    }
    /* Fraunhofer's VBRI header sits 32 bytes after the frame header. */
    if (36u + 18u <= length && memcmp(frame + 36u, "VBRI", 4u) == 0) {
        info->present = true;
        info->bytes = mp3_be32(frame + 36u + 10u);
        info->frames = mp3_be32(frame + 36u + 14u);
        info->has_bytes = info->bytes != 0;
        info->has_frames = info->frames != 0;
        return true;
    }
    return false;
}

bool media_mp3_content_type(const char *content_type)
{
    if (content_type == NULL) return false;
    while (*content_type == ' ' || *content_type == '\t') content_type++;
    static const char *const types[] = {
        "audio/mpeg", "audio/mp3", "audio/mpeg3", "audio/x-mpeg",
        "audio/x-mp3", "audio/mpg"
    };
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        size_t length = strlen(types[i]);
        if (strncasecmp(content_type, types[i], length) == 0
            && (content_type[length] == '\0' || content_type[length] == ';'
                || content_type[length] == ' '))
            return true;
    }
    return false;
}

bool media_mp3_sniff(const unsigned char *bytes, size_t length)
{
    if (media_mp3_id3v2_length(bytes, length) != 0) return true;
    MediaMp3FrameHeader first, second;
    return bytes != NULL && length >= 4u
        && media_mp3_parse_header(bytes, &first)
        && first.frame_bytes + 4u <= length
        && media_mp3_parse_header(bytes + first.frame_bytes, &second)
        && media_mp3_headers_consistent(&first, &second);
}

struct MediaMp3Source {
    Budget *budget;
    MediaMp3Transport transport;
    uint64_t content_length;
    bool seekable;
    bool live;
    unsigned char *ring;
    size_t ring_head;
    size_t ring_length;
    /* Resource offset of ring_head. A live stream's offsets keep counting
       across reconnects; they are identities, not file positions. */
    uint64_t ring_offset;
    /* Response bytes still to discard (an ID3 tag larger than the ring). */
    uint64_t skip_bytes;
    /* A live reconnect joins two unrelated responses here: no frame may
       span it. UINT64_MAX when none is pending. */
    uint64_t junction;
    uint64_t handle;
    /* The open response's bytes so far, and the Content-Length it declared
       (0: none, or not asked yet; asked once it delivers bytes). */
    uint64_t response_bytes;
    uint64_t response_declared_length;
    bool response_length_asked;
    /* The resource's last byte arrived (finite files, and a live-routed
       response that turned out to be one). */
    bool eof;
    bool ended;
    bool failed;
    bool synced;
    bool locked_valid;
    bool head_valid;
    bool first_frame_checked;
    /* No frame was consumed since the last restart (or since the start),
       so the source still stands at seek_target_samples. */
    bool seek_pristine;
    MediaMp3FrameHeader locked;
    MediaMp3FrameHeader head;
    uint32_t first_channels;
    uint64_t pts_samples;
    /* The sample position the last restart (or the start) placed the
       source at; meaningful while seek_pristine. */
    uint64_t seek_target_samples;
    uint64_t duration_samples;
    uint64_t audio_start;
    uint64_t scanned_without_frame;
    uint64_t reconnect_at_us;
    uint64_t last_pump_us;
    unsigned reconnects_without_frame;
    /* A live response broke and its successor has not delivered a byte. */
    bool reconnecting;
    MediaMp3VbrInfo vbr;
    MediaMp3Stats stats;
    char error[160];
};

static void mp3_fail(MediaMp3Source *source, const char *message)
{
    if (source->failed) return;
    source->failed = true;
    snprintf(source->error, sizeof(source->error), "%s", message);
}

static unsigned char mp3_ring_byte(const MediaMp3Source *source, size_t at)
{
    return source->ring[(source->ring_head + at) % MEDIA_MP3_RING_BYTES];
}

static void mp3_ring_copy(const MediaMp3Source *source, size_t at,
                          void *destination, size_t length)
{
    size_t start = (source->ring_head + at) % MEDIA_MP3_RING_BYTES;
    size_t first = MEDIA_MP3_RING_BYTES - start;
    if (first > length) first = length;
    memcpy(destination, source->ring + start, first);
    memcpy((unsigned char *) destination + first, source->ring,
           length - first);
}

static void mp3_ring_drop(MediaMp3Source *source, size_t length)
{
    if (length > source->ring_length) length = source->ring_length;
    source->ring_head = (source->ring_head + length) % MEDIA_MP3_RING_BYTES;
    source->ring_length -= length;
    source->ring_offset += length;
}

/* Throws away bytes that are not a frame, ending any established sync. */
static void mp3_skip(MediaMp3Source *source, size_t length)
{
    mp3_ring_drop(source, length);
    source->stats.skipped_bytes += length;
    source->scanned_without_frame += length;
    if (source->synced) {
        source->synced = false;
        source->stats.sync_losses++;
    }
    if (source->scanned_without_frame > MEDIA_MP3_RESYNC_LIMIT_BYTES)
        mp3_fail(source, source->stats.frames == 0
            ? "page audio is not an MP3 stream"
            : "MP3 stream lost sync");
}

static void mp3_cancel(MediaMp3Source *source)
{
    if (source->handle != 0 && source->transport.cancel != NULL)
        source->transport.cancel(source->transport.opaque, source->handle);
    source->handle = 0;
}

/* Restarts the response at `offset` with an empty ring (seek, a large ID3
   tag, a resumed finite read). */
static void mp3_restart_at(MediaMp3Source *source, uint64_t offset)
{
    mp3_cancel(source);
    source->ring_head = 0;
    source->ring_length = 0;
    source->ring_offset = offset;
    source->skip_bytes = 0;
    source->junction = UINT64_MAX;
    source->eof = false;
    source->ended = false;
    source->synced = false;
    source->head_valid = false;
    source->scanned_without_frame = 0;
}

/* Establishes the frame at the ring head. False means more bytes are
   needed, the stream ended, or it failed. */
static bool mp3_find_head(MediaMp3Source *source)
{
    while (!source->head_valid && !source->failed && !source->ended) {
        bool final = source->eof && source->handle == 0;
        if (source->skip_bytes != 0) {
            /* A tag that claimed more than the rest of a finished resource:
               nothing follows it, so the resource has ended. */
            if (final) {
                source->skip_bytes = 0;
                mp3_ring_drop(source, source->ring_length);
                source->ended = true;
            }
            return false;
        }
        size_t available = source->ring_length;
        if (available < 4u) {
            if (final) {
                if (available != 0) mp3_ring_drop(source, available);
                source->ended = true;
            }
            return false;
        }
        unsigned char bytes[4];
        mp3_ring_copy(source, 0, bytes, 4u);
        if (bytes[0] == 'I' && bytes[1] == 'D' && bytes[2] == '3') {
            if (available < 10u && !final) return false;
            unsigned char tag[10] = {0};
            mp3_ring_copy(source, 0, tag, available < 10u ? available : 10u);
            size_t length = media_mp3_id3v2_length(tag, sizeof(tag));
            if (source->live && length > MP3_LIVE_TAG_LIMIT_BYTES)
                length = 0;
            if (length != 0) {
                source->stats.id3_tags++;
                if (length <= available) {
                    mp3_ring_drop(source, length);
                } else if (source->seekable && !source->live) {
                    mp3_restart_at(source, source->ring_offset + length);
                    return false;
                } else {
                    source->skip_bytes = length - available;
                    mp3_ring_drop(source, available);
                    return false;
                }
                continue;
            }
        }
        MediaMp3FrameHeader frame;
        if (!media_mp3_parse_header(bytes, &frame)
            || (source->locked_valid
                && !media_mp3_headers_consistent(&source->locked, &frame))) {
            /* Step to the next possible sync byte. */
            size_t step = 1u;
            while (step < available && mp3_ring_byte(source, step) != 0xffu)
                step++;
            mp3_skip(source, step);
            continue;
        }
        if (source->junction != UINT64_MAX
            && source->ring_offset < source->junction
            && source->ring_offset + frame.frame_bytes > source->junction) {
            mp3_skip(source, (size_t) (source->junction - source->ring_offset));
            continue;
        }
        if (source->junction != UINT64_MAX
            && source->ring_offset >= source->junction)
            source->junction = UINT64_MAX;
        if (frame.frame_bytes > available) {
            if (final) mp3_skip(source, available);
            return false;
        }
        if (!source->synced) {
            /* Acquiring sync needs the next header to agree, except for the
               last frame of a finished resource. */
            if (available < (size_t) frame.frame_bytes + 4u) {
                if (!final) return false;
            } else {
                unsigned char next[4];
                MediaMp3FrameHeader following;
                mp3_ring_copy(source, frame.frame_bytes, next, 4u);
                bool id3 = next[0] == 'I' && next[1] == 'D' && next[2] == '3';
                bool tag = next[0] == 'T' && next[1] == 'A' && next[2] == 'G';
                if (!id3 && !(tag && source->eof)
                    && (!media_mp3_parse_header(next, &following)
                        || !media_mp3_headers_consistent(
                               &frame, &following))) {
                    mp3_skip(source, 1u);
                    continue;
                }
            }
        }
        source->synced = true;
        source->scanned_without_frame = 0;
        if (!source->first_frame_checked) {
            source->first_frame_checked = true;
            unsigned char first[MEDIA_MP3_MAXIMUM_FRAME_BYTES];
            mp3_ring_copy(source, 0, first, frame.frame_bytes);
            if (media_mp3_vbr_info(first, frame.frame_bytes, &frame,
                                   &source->vbr)) {
                /* The metadata frame is not audio. */
                mp3_ring_drop(source, frame.frame_bytes);
                continue;
            }
        }
        if (!source->locked_valid) {
            source->locked = frame;
            source->locked_valid = true;
            source->first_channels = frame.channels;
            source->audio_start = source->ring_offset;
            uint64_t span = source->content_length > source->audio_start
                ? source->content_length - source->audio_start : 0;
            if (source->live) {
                source->duration_samples = 0;
            } else if (source->vbr.has_frames) {
                source->duration_samples =
                    (uint64_t) source->vbr.frames * frame.samples;
            } else if (frame.bitrate != 0) {
                source->duration_samples = span * 8u
                    * frame.sample_rate / frame.bitrate;
            }
        }
        source->head = frame;
        source->head_valid = true;
    }
    return source->head_valid;
}

/* Pulls response bytes into the ring; one bounded visit. */
static void mp3_pull(MediaMp3Source *source, uint64_t now_us)
{
    if (source->failed || source->ended) return;
    if (source->handle == 0) {
        if (source->eof || now_us < source->reconnect_at_us) return;
        uint64_t offset = source->ring_offset + source->ring_length
            + source->skip_bytes;
        if (source->live) offset = 0;
        char error[160] = {0};
        source->handle = source->transport.start(
            source->transport.opaque, offset, error, sizeof(error));
        if (source->handle == 0) {
            if (error[0] != '\0') mp3_fail(source, error);
            return;
        }
        source->response_bytes = 0;
        source->response_declared_length = 0;
        source->response_length_asked = false;
    }
    size_t moved = 0;
    while (moved < MP3_VISIT_BYTES && source->handle != 0) {
        size_t free_bytes = MEDIA_MP3_RING_BYTES - source->ring_length;
        if (free_bytes == 0) break;
        size_t write = (source->ring_head + source->ring_length)
            % MEDIA_MP3_RING_BYTES;
        size_t capacity = MEDIA_MP3_RING_BYTES - write;
        if (capacity > free_bytes) capacity = free_bytes;
        size_t length = 0;
        char error[160] = {0};
        MediaHlsTransportPollResult result = source->transport.poll(
            source->transport.opaque, source->handle, source->ring + write,
            capacity, &length, error, sizeof(error));
        if (result == MEDIA_HLS_TRANSPORT_CHUNK) {
            if (length > capacity) {
                mp3_fail(source, "MP3 transport overran its chunk");
                return;
            }
            source->ring_length += length;
            source->stats.bytes_received += length;
            source->response_bytes += length;
            moved += length;
            if (length != 0) source->reconnecting = false;
            if (length != 0 && !source->response_length_asked) {
                source->response_length_asked = true;
                if (source->transport.declared_length != NULL)
                    source->response_declared_length =
                        source->transport.declared_length(
                            source->transport.opaque, source->handle);
            }
            if (source->skip_bytes != 0) {
                size_t drop = source->skip_bytes < source->ring_length
                    ? (size_t) source->skip_bytes : source->ring_length;
                mp3_ring_drop(source, drop);
                source->skip_bytes -= drop;
            }
            if (length == 0) break;
            continue;
        }
        if (result == MEDIA_HLS_TRANSPORT_WAIT) break;
        source->handle = 0;
        bool complete = result == MEDIA_HLS_TRANSPORT_COMPLETE;
        /* A live-routed response that declared its length and delivered
           all of it was a finite file from a server without ranges (its
           whole body overflowed the probe); reconnecting would replay it
           forever and the page would never see it end. */
        bool finite = complete && source->live
            && source->response_declared_length != 0
            && source->response_bytes == source->response_declared_length;
        if ((!source->live && complete) || finite) {
            source->eof = true;
            break;
        }
        /* A live stream that ended or broke reconnects; a finite file whose
           server answers ranges resumes where it stopped. */
        if ((!source->live && !source->seekable)
            || ++source->reconnects_without_frame
                   > MEDIA_MP3_RECONNECT_LIMIT) {
            mp3_fail(source, error[0] != '\0' ? error
                     : source->live ? "MP3 stream stopped"
                                    : "MP3 download failed");
            return;
        }
        source->stats.reconnects++;
        if (source->live) {
            source->junction = source->ring_offset + source->ring_length;
            source->reconnecting = true;
            /* The rest of a tag belonged to the broken response; the new
               one starts with its own bytes. */
            source->skip_bytes = 0;
        }
        uint64_t delay = MP3_RECONNECT_BASE_US
            << (source->reconnects_without_frame - 1u);
        if (delay > MP3_RECONNECT_MAXIMUM_US)
            delay = MP3_RECONNECT_MAXIMUM_US;
        source->reconnect_at_us = now_us > UINT64_MAX - delay
            ? UINT64_MAX : now_us + delay;
        break;
    }
}

MediaMp3Source *media_mp3_source_create(
    Budget *budget, const MediaMp3Transport *transport,
    const MediaMp3SourceOptions *options, char *error, size_t error_size)
{
    if (error != NULL && error_size != 0) error[0] = '\0';
    if (budget == NULL || transport == NULL || transport->start == NULL
        || transport->poll == NULL || transport->cancel == NULL) {
        if (error != NULL) snprintf(error, error_size, "%s",
                                    "invalid MP3 source");
        return NULL;
    }
    MediaMp3Source *source = budget_calloc_category(
        budget, BUDGET_CATEGORY_RESOURCE, 1, sizeof(*source));
    unsigned char *ring = budget_malloc_category(
        budget, BUDGET_CATEGORY_RESOURCE, MEDIA_MP3_RING_BYTES);
    if (source == NULL || ring == NULL) {
        budget_free(budget, ring);
        budget_free(budget, source);
        if (error != NULL) snprintf(error, error_size, "%s",
                                    "MP3 source exceeds memory budget");
        return NULL;
    }
    source->budget = budget;
    source->transport = *transport;
    source->ring = ring;
    source->content_length = options == NULL ? 0 : options->content_length;
    source->live = source->content_length == 0;
    source->seekable = options != NULL && options->seekable && !source->live;
    source->junction = UINT64_MAX;
    source->seek_pristine = true;
    return source;
}

MediaHlsPrimeStatus media_mp3_source_prime(
    MediaMp3Source *source, char *error, size_t error_size)
{
    if (error != NULL && error_size != 0) error[0] = '\0';
    if (source == NULL) return MEDIA_HLS_PRIME_FAILED;
    if (!mp3_find_head(source)) {
        mp3_pull(source, source->last_pump_us);
        (void) mp3_find_head(source);
    }
    if (source->head_valid) return MEDIA_HLS_PRIME_READY;
    if (source->ended && !source->failed)
        mp3_fail(source, "page audio has no MP3 frames");
    if (source->failed) {
        if (error != NULL) snprintf(error, error_size, "%s", source->error);
        return MEDIA_HLS_PRIME_FAILED;
    }
    return MEDIA_HLS_PRIME_PENDING;
}

void media_mp3_source_pump(MediaMp3Source *source, uint64_t now_us)
{
    if (source == NULL) return;
    source->last_pump_us = now_us;
    mp3_pull(source, now_us);
}

static size_t mp3_track_count(const void *opaque)
{
    const MediaMp3Source *source = opaque;
    return source != NULL && source->locked_valid ? 1u : 0u;
}

bool media_mp3_source_track_info(const MediaMp3Source *source,
                                 MediaMp4TrackInfo *info)
{
    if (source == NULL || info == NULL || !source->locked_valid) return false;
    *info = (MediaMp4TrackInfo) {
        .kind = MEDIA_MP4_TRACK_AUDIO,
        .codec = MEDIA_MP3_FOURCC,
        .timescale = source->locked.sample_rate,
        .duration = source->duration_samples,
        .largest_sample = MEDIA_MP3_MAXIMUM_FRAME_BYTES,
        .channels = (uint16_t) source->first_channels,
        .sample_rate = source->locked.sample_rate
    };
    return true;
}

static bool mp3_track_info(const void *opaque, size_t index,
                           MediaMp4TrackInfo *info)
{
    return index == 0 && media_mp3_source_track_info(opaque, info);
}

static bool mp3_next_sample(void *opaque, MediaMp4Sample *sample)
{
    MediaMp3Source *source = opaque;
    if (source == NULL || sample == NULL) return false;
    if (!mp3_find_head(source)) {
        mp3_pull(source, source->last_pump_us);
        if (!mp3_find_head(source)) return false;
    }
    *sample = (MediaMp4Sample) {
        .track_index = 0,
        .kind = MEDIA_MP4_TRACK_AUDIO,
        .offset = source->ring_offset,
        .size = source->head.frame_bytes,
        .dts = source->pts_samples,
        .pts = source->pts_samples > INT64_MAX
            ? INT64_MAX : (int64_t) source->pts_samples,
        .duration = source->head.samples,
        .timescale = source->locked.sample_rate,
        .keyframe = true
    };
    return true;
}

static bool mp3_last_error(const void *opaque, char *error,
                           size_t error_size)
{
    const MediaMp3Source *source = opaque;
    if (source == NULL || !source->failed) return false;
    if (error != NULL && error_size != 0)
        snprintf(error, error_size, "%s", source->error);
    return true;
}

static bool mp3_would_block(const void *opaque)
{
    const MediaMp3Source *source = opaque;
    return source != NULL && !source->failed && !source->ended
        && !source->head_valid;
}

static bool mp3_sample_resident(const void *opaque,
                                const MediaMp4Sample *sample)
{
    const MediaMp3Source *source = opaque;
    return source != NULL && sample != NULL && source->head_valid
        && sample->offset == source->ring_offset;
}

static void mp3_release_head(MediaMp3Source *source)
{
    mp3_ring_drop(source, source->head.frame_bytes);
    source->pts_samples += source->head.samples;
    source->head_valid = false;
    source->seek_pristine = false;
    source->reconnects_without_frame = 0;
    source->stats.frames++;
}

static bool mp3_read_sample(void *opaque, const MediaMp4Sample *sample,
                            void *destination, size_t capacity)
{
    MediaMp3Source *source = opaque;
    if (!mp3_sample_resident(source, sample) || destination == NULL)
        return false;
    if (source->head.frame_bytes > capacity) {
        mp3_fail(source, "MP3 frame exceeds packet buffer");
        return false;
    }
    mp3_ring_copy(source, 0, destination, source->head.frame_bytes);
    mp3_release_head(source);
    return true;
}

static void mp3_discard_sample(void *opaque, const MediaMp4Sample *sample)
{
    MediaMp3Source *source = opaque;
    if (mp3_sample_resident(source, sample)) mp3_release_head(source);
}

static bool mp3_seek_us(void *opaque, uint64_t target_us,
                        uint64_t *actual_us)
{
    MediaMp3Source *source = opaque;
    if (actual_us != NULL) *actual_us = 0;
    if (source == NULL || source->failed || !source->locked_valid)
        return false;
    /* The player proves its position while priming, up to three times a
       poll (playback_prime_source). While no frame has been consumed since
       the last restart, the source already stands there; restarting again
       would cancel the response before its headers arrive, every poll. An
       untouched start at zero is also all a live or rangeless source can
       do. */
    if (target_us == 0 && source->seek_pristine
        && source->seek_target_samples == 0) return true;
    if (!source->seekable || source->duration_samples == 0) return false;
    uint32_t rate = source->locked.sample_rate;
    uint64_t target = target_us / 1000000u * rate
        + target_us % 1000000u * rate / 1000000u;
    if (target > source->duration_samples) target = source->duration_samples;
    if (source->seek_pristine && target == source->seek_target_samples) {
        if (actual_us != NULL)
            *actual_us = target / rate * 1000000u
                + target % rate * 1000000u / rate;
        return true;
    }
    uint64_t span = source->content_length > source->audio_start
        ? source->content_length - source->audio_start : 0;
    if (source->vbr.has_toc && source->vbr.has_bytes
        && source->vbr.bytes < span)
        span = source->vbr.bytes;
    /* Position in thousandths of a percent of the duration. */
    uint64_t milli = target * 100000u / source->duration_samples;
    uint64_t scaled;  /* offset in 1/256000ths of the span */
    if (source->vbr.has_toc) {
        size_t index = (size_t) (milli / 1000u);
        if (index > 99u) index = 99u;
        uint64_t low = source->vbr.toc[index];
        uint64_t high = index < 99u ? source->vbr.toc[index + 1u] : 256u;
        if (high < low) high = low;
        scaled = low * 1000u + (high - low) * (milli - index * 1000u);
    } else {
        scaled = milli * 256u / 100u;
    }
    uint64_t offset = source->audio_start + span * scaled / 256000u;
    if (target == 0) offset = source->audio_start;
    mp3_restart_at(source, offset);
    source->pts_samples = target;
    source->seek_target_samples = target;
    source->seek_pristine = true;
    source->reconnects_without_frame = 0;
    source->reconnect_at_us = 0;
    if (actual_us != NULL)
        *actual_us = target / rate * 1000000u
            + target % rate * 1000000u / rate;
    return true;
}

static void mp3_rewind(void *opaque)
{
    (void) mp3_seek_us(opaque, 0, NULL);
}

static size_t mp3_retained_bytes(const void *opaque)
{
    return opaque == NULL ? 0
        : sizeof(MediaMp3Source) + MEDIA_MP3_RING_BYTES;
}

static const MediaSampleSourceOps mp3_source_ops = {
    .track_count = mp3_track_count,
    .track_info = mp3_track_info,
    .next_sample = mp3_next_sample,
    .last_error = mp3_last_error,
    .would_block = mp3_would_block,
    .sample_resident = mp3_sample_resident,
    .read_sample_waiting = mp3_read_sample,
    .read_sample = mp3_read_sample,
    .seek_us = mp3_seek_us,
    .seek_after_us = mp3_seek_us,
    .rewind = mp3_rewind,
    .retained_bytes = mp3_retained_bytes,
    .discard_sample = mp3_discard_sample
};

bool media_mp3_source_sample_source(
    MediaMp3Source *source, MediaSampleSource *sample_source)
{
    if (source == NULL || sample_source == NULL) return false;
    *sample_source = (MediaSampleSource) {
        .opaque = source,
        .ops = &mp3_source_ops
    };
    return true;
}

void media_mp3_source_stats(const MediaMp3Source *source,
                            MediaMp3Stats *stats)
{
    if (stats == NULL) return;
    if (source == NULL) {
        memset(stats, 0, sizeof(*stats));
        return;
    }
    *stats = source->stats;
    stats->buffered_bytes = source->ring_length;
    stats->live = source->live;
    stats->ended = source->ended;
    stats->reconnecting = source->reconnecting && !source->failed;
    stats->vbr = source->vbr.present;
    stats->holding = !source->failed
        && MEDIA_MP3_RING_BYTES - source->ring_length < MP3_HOLD_FREE_BYTES;
    if (source->locked_valid) {
        uint32_t rate = source->locked.sample_rate;
        uint64_t head_us = source->pts_samples / rate * 1000000u
            + source->pts_samples % rate * 1000000u / rate;
        uint32_t bitrate = source->head_valid ? source->head.bitrate
                                              : source->locked.bitrate;
        stats->buffered_until_us = head_us + (bitrate == 0 ? 0
            : (uint64_t) source->ring_length * 8000000u / bitrate);
        stats->sample_rate = rate;
        stats->channels = (uint16_t) source->first_channels;
    }
}

bool media_mp3_source_failed(const MediaMp3Source *source)
{
    return source != NULL && source->failed;
}

void media_mp3_source_destroy(MediaMp3Source *source)
{
    if (source == NULL) return;
    mp3_cancel(source);
    Budget *budget = source->budget;
    budget_free(budget, source->ring);
    budget_free(budget, source);
}

/* The length of one well-formed UTF-8 sequence at text, or 0. */
static size_t mp3_utf8_sequence(const unsigned char *text, size_t length)
{
    unsigned char lead = text[0];
    size_t need = lead < 0x80u ? 1u : (lead & 0xE0u) == 0xC0u ? 2u
        : (lead & 0xF0u) == 0xE0u ? 3u : (lead & 0xF8u) == 0xF0u ? 4u : 0u;
    if (need == 0u || need > length || (need == 2u && lead < 0xC2u))
        return 0u;
    for (size_t at = 1; at < need; at++)
        if ((text[at] & 0xC0u) != 0x80u) return 0u;
    return need;
}

bool media_mp3_station_text(const char *raw, char *output, size_t size)
{
    if (output == NULL || size == 0) return false;
    output[0] = '\0';
    if (raw == NULL || size < 2u) return false;
    const unsigned char *text = (const unsigned char *) raw;
    size_t length = strlen(raw);
    /* Decide the encoding once: a header that is not entirely well-formed
       UTF-8 is read as Latin-1 throughout. */
    bool utf8 = true;
    for (size_t at = 0; at < length;) {
        size_t used = mp3_utf8_sequence(text + at, length - at);
        if (used == 0u) { utf8 = false; break; }
        at += used;
    }
    size_t used_out = 0;
    bool space = false;
    for (size_t at = 0; at < length;) {
        unsigned char sequence[4];
        size_t sequence_length;
        if (utf8) {
            sequence_length = mp3_utf8_sequence(text + at, length - at);
            memcpy(sequence, text + at, sequence_length);
            at += sequence_length;
        } else if (text[at] < 0x80u) {
            sequence[0] = text[at++];
            sequence_length = 1u;
        } else {
            sequence[0] = (unsigned char) (0xC0u | (text[at] >> 6));
            sequence[1] = (unsigned char) (0x80u | (text[at] & 0x3Fu));
            at++;
            sequence_length = 2u;
        }
        if (sequence_length == 1u
            && (sequence[0] <= 0x20u || sequence[0] == 0x7Fu)) {
            /* Whitespace and controls separate words; a run of them is
               one space, and none leads or trails. */
            if (used_out != 0u) space = true;
            continue;
        }
        /* C1 controls (U+0080..U+009F) are not text either. */
        if (sequence_length == 2u && sequence[0] == 0xC2u
            && sequence[1] < 0xA0u) continue;
        size_t extra = space ? 1u : 0u;
        if (used_out + extra + sequence_length >= size) break;
        if (space) output[used_out++] = ' ';
        space = false;
        memcpy(output + used_out, sequence, sequence_length);
        used_out += sequence_length;
    }
    output[used_out] = '\0';
    return used_out != 0u;
}

bool media_audio_source_label(const char *url, char *output, size_t size)
{
    if (output == NULL || size == 0) return false;
    output[0] = '\0';
    if (url == NULL) return false;
    const char *host;
    const char *default_port;
    if (strncasecmp(url, "https://", 8) == 0) {
        host = url + 8;
        default_port = "443";
    } else if (strncasecmp(url, "http://", 7) == 0) {
        host = url + 7;
        default_port = "80";
    } else {
        return false;
    }
    size_t authority = strcspn(host, "/?#");
    const char *at_sign = NULL;
    for (size_t i = 0; i < authority; i++)
        if (host[i] == '@') at_sign = host + i;
    if (at_sign != NULL) {
        authority -= (size_t) (at_sign + 1 - host);
        host = at_sign + 1;
    }
    /* The port is whatever follows the last colon outside brackets. */
    size_t name_length = authority;
    for (size_t i = authority; i > 0; i--) {
        if (host[i - 1] == ']') break;
        if (host[i - 1] == ':') { name_length = i - 1; break; }
    }
    const char *port = name_length < authority ? host + name_length + 1 : NULL;
    size_t port_length = port == NULL ? 0 : authority - name_length - 1;
    bool show_port = port_length != 0
        && !(port_length == strlen(default_port)
             && strncmp(port, default_port, port_length) == 0);
    size_t total = name_length + (show_port ? port_length + 1 : 0);
    if (name_length == 0 || total >= size) return false;
    for (size_t i = 0; i < name_length; i++) {
        char c = host[i];
        output[i] = c >= 'A' && c <= 'Z' ? (char) (c - 'A' + 'a') : c;
    }
    size_t used = name_length;
    if (show_port) {
        output[used++] = ':';
        memcpy(output + used, port, port_length);
        used += port_length;
    }
    output[used] = '\0';
    return true;
}

/* A URL's port, with each scheme's default spelled out, so an HTTPS
   upgrade of http://host/ (80 to 443) still names the same server. */
static bool shoutcast_same_server(const TilefinchUrl *left,
                                  const TilefinchUrl *right)
{
    if (left->host_length != right->host_length
        || strncasecmp(left->value + left->host_offset,
                       right->value + right->host_offset,
                       left->host_length) != 0) return false;
    if (left->port == right->port) return true;
    return !left->explicit_port && !right->explicit_port;
}

bool media_shoutcast_stream_url(const char *source, const char *answered,
                                char *output, size_t size)
{
    if (output == NULL || size == 0) return false;
    output[0] = '\0';
    TilefinchUrl from, to;
    if (source == NULL || answered == NULL
        || !tilefinch_url_parse(source, &from)
        || !tilefinch_url_parse(answered, &to)
        || (from.scheme != TILEFINCH_URL_SCHEME_HTTP
            && from.scheme != TILEFINCH_URL_SCHEME_HTTPS)
        || (to.scheme != TILEFINCH_URL_SCHEME_HTTP
            && to.scheme != TILEFINCH_URL_SCHEME_HTTPS)
        || from.path_length > 1u
        || (from.path_length == 1u && source[from.path_offset] != '/')
        || !shoutcast_same_server(&from, &to)) return false;
    static const char page[] = "/index.html";
    static const char sid[] = "sid=";
    const char *query = answered + to.query_offset;
    if (to.path_length != sizeof(page) - 1u
        || strncmp(answered + to.path_offset, page, sizeof(page) - 1u) != 0
        || !to.has_query || to.query_length <= sizeof(sid) - 1u
        || strncmp(query, sid, sizeof(sid) - 1u) != 0) return false;
    for (size_t at = sizeof(sid) - 1u; at < to.query_length; at++)
        if (query[at] < '0' || query[at] > '9') return false;
    int written = snprintf(output, size, "%.*s/;",
                           (int) from.path_offset, source);
    if (written < 0 || (size_t) written >= size) {
        output[0] = '\0';
        return false;
    }
    return true;
}
