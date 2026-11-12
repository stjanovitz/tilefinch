#ifndef TILEFINCH_MEDIA_MP3_H
#define TILEFINCH_MEDIA_MP3_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/budget.h"
#include "tilefinch/media_hls.h"
#include "tilefinch/media_source.h"

/*
 * MPEG-1/2/2.5 Layer III audio: frame parsing and a bounded streaming sample
 * source for page audio.
 *
 * The source reads one progressive HTTP response at a time through a small
 * transport seam and keeps a fixed byte ring, so an endless radio stream
 * occupies the same memory after an hour as after a second. Frames are found
 * by their sync word; acquiring sync requires two consecutive consistent
 * headers, and a frame that fails its header check drops the stream back
 * into a bounded resync scan. Finite files may seek when the server answers
 * byte ranges; the source then restarts its response at the estimated offset
 * and resynchronises there.
 */

/* MediaMp4TrackInfo.codec for a Layer III track. */
#define MEDIA_MP3_FOURCC MEDIA_MP4_FOURCC('.', 'm', 'p', '3')
/* The largest Layer III frame: MPEG-1 at 320 kbit/s and 32 kHz, padded. */
#define MEDIA_MP3_MAXIMUM_FRAME_BYTES 1441u
/* PCM samples per channel in one frame: MPEG-1 and MPEG-2/2.5. */
#define MEDIA_MP3_SAMPLES_MPEG1 1152u
#define MEDIA_MP3_SAMPLES_MPEG2 576u
/* The compressed ring: about 8 s of a 128 kbit/s station. */
#define MEDIA_MP3_RING_BYTES (128u * 1024u)
/* Bytes scanned without finding a confirmed frame before the stream is
   declared not to be MP3 (or hopelessly damaged). */
#define MEDIA_MP3_RESYNC_LIMIT_BYTES (64u * 1024u)
/* Consecutive reconnects without a delivered frame before a live stream
   fails; finite files resume at most this many times. */
#define MEDIA_MP3_RECONNECT_LIMIT 4u

typedef enum {
    MEDIA_MP3_VERSION_1 = 0,
    MEDIA_MP3_VERSION_2,
    MEDIA_MP3_VERSION_2_5
} MediaMp3Version;

typedef struct {
    MediaMp3Version version;
    uint32_t sample_rate;
    uint32_t bitrate;          /* bits per second */
    uint16_t channels;         /* 1 or 2 */
    uint16_t samples;          /* per channel, per frame */
    uint16_t frame_bytes;      /* header included */
    uint8_t channel_mode;      /* 0 stereo, 1 joint, 2 dual, 3 mono */
    bool padding;
    bool crc;
} MediaMp3FrameHeader;

/* Parses a 4-byte Layer III frame header. Free-format, reserved and
   non-Layer-III headers are refused. */
bool media_mp3_parse_header(const unsigned char header[4],
                            MediaMp3FrameHeader *frame);
/* Two headers belong to one stream: same version and sample rate. */
bool media_mp3_headers_consistent(const MediaMp3FrameHeader *left,
                                  const MediaMp3FrameHeader *right);
/* The length of an ID3v2 tag starting at bytes (header, body and optional
   footer), or 0 when bytes does not start one. Needs 10 bytes. */
size_t media_mp3_id3v2_length(const unsigned char *bytes, size_t length);

/* VBR metadata carried in a stream's first frame. */
typedef struct {
    bool present;          /* a Xing, Info or VBRI frame (not audio) */
    bool has_frames;
    bool has_bytes;
    bool has_toc;
    uint32_t frames;
    uint32_t bytes;
    unsigned char toc[100];
} MediaMp3VbrInfo;

/* Reads Xing/Info or VBRI metadata from one complete frame. */
bool media_mp3_vbr_info(const unsigned char *frame, size_t length,
                        const MediaMp3FrameHeader *header,
                        MediaMp3VbrInfo *info);

/* Whether a response looks like MPEG audio: an MPEG audio media type, or a
   body that starts with an ID3v2 tag or two consecutive Layer III frames. */
bool media_mp3_content_type(const char *content_type);
bool media_mp3_sniff(const unsigned char *bytes, size_t length);

/*
 * A station's self-description from an Icecast/SHOUTcast response header
 * (icy-name, icy-genre), made safe to show: surrounding and repeated
 * whitespace collapsed, control characters dropped, Latin-1 bytes (which
 * many servers send) converted to UTF-8, and the result cut at a character
 * boundary to fit `size`. False when nothing printable remains.
 */
bool media_mp3_station_text(const char *raw, char *output, size_t size);
/* The host (and a non-default port) an http(s) URL names, for a "where it
   comes from" line: no scheme, credentials, path or query. */
bool media_audio_source_label(const char *url, char *output, size_t size);

/*
 * SHOUTcast's stream address. A SHOUTcast DNAS server picks what its root
 * address answers from the User-Agent: players get the stream, and web
 * browsers get a redirect to its status page, "index.html?sid=N". Both
 * DNAS generations (v1 and v2) serve the first stream at "/;" to every
 * client; it is the address the server's own listen.pls and listen.m3u
 * playlists name. When page audio at a server root (`source`) ended at
 * that status page (`answered`, same host and port, either scheme), this
 * writes the stream address to try once instead: the source's scheme, host
 * and port with path "/;". False for any other answer.
 */
bool media_shoutcast_stream_url(const char *source, const char *answered,
                                char *output, size_t size);

/*
 * The response transport. start() opens the resource at byte `offset` (0:
 * from the beginning, otherwise a Range request whose response must begin
 * there). A zero handle with an empty error is transient backpressure. poll
 * and cancel follow MediaHlsTransport.
 *
 * declared_length (optional) returns the Content-Length the response on an
 * open `handle` declared, or 0 when it declared none; the source asks once
 * the response delivers its first bytes. A source routed as live (the probe
 * learned no length, as when a server ignores Range and its whole file
 * overflowed the probe) ends instead of reconnecting when such a response
 * completes with exactly that many bytes: it was a finite file.
 */
typedef struct {
    void *opaque;
    uint64_t (*start)(void *opaque, uint64_t offset,
                      char *error, size_t error_size);
    MediaHlsTransportPollResult (*poll)(
        void *opaque, uint64_t handle, unsigned char *destination,
        size_t capacity, size_t *length, char *error, size_t error_size);
    void (*cancel)(void *opaque, uint64_t handle);
    uint64_t (*declared_length)(void *opaque, uint64_t handle);
} MediaMp3Transport;

typedef struct {
    /* The resource length, or 0 when the server named none (a live
       stream). */
    uint64_t content_length;
    /* The server answers byte ranges, so seeks and resumed reads can
       restart the response at an offset. */
    bool seekable;
} MediaMp3SourceOptions;

typedef struct {
    uint64_t bytes_received;
    uint64_t frames;
    uint64_t skipped_bytes;
    uint64_t buffered_until_us;
    unsigned sync_losses;
    unsigned reconnects;
    unsigned id3_tags;
    size_t buffered_bytes;
    uint32_t sample_rate;
    uint16_t channels;
    bool live;
    bool ended;
    bool holding;
    /* A live stream's response broke and its successor has not yet
       delivered: playback may continue from the ring meanwhile. */
    bool reconnecting;
    bool vbr;
} MediaMp3Stats;

typedef struct MediaMp3Source MediaMp3Source;

MediaMp3Source *media_mp3_source_create(
    Budget *budget, const MediaMp3Transport *transport,
    const MediaMp3SourceOptions *options, char *error, size_t error_size);
/* READY once the first audio frame is confirmed, which fixes the track's
   rate and channel count. */
MediaHlsPrimeStatus media_mp3_source_prime(
    MediaMp3Source *source, char *error, size_t error_size);
/* One bounded delivery visit: pulls response bytes into free ring space and
   reconnects a dropped live stream after a short backoff. */
void media_mp3_source_pump(MediaMp3Source *source, uint64_t now_us);
bool media_mp3_source_sample_source(
    MediaMp3Source *source, MediaSampleSource *sample_source);
bool media_mp3_source_track_info(const MediaMp3Source *source,
                                 MediaMp4TrackInfo *info);
void media_mp3_source_stats(const MediaMp3Source *source,
                            MediaMp3Stats *stats);
bool media_mp3_source_failed(const MediaMp3Source *source);
/* Cancels the response and releases everything. */
void media_mp3_source_destroy(MediaMp3Source *source);

#endif
