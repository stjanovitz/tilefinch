#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tilefinch/budget.h"
#include "tilefinch/media_backend.h"
#include "tilefinch/media_failure_message.h"
#include "tilefinch/media_hls.h"

#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, \
                #condition);                                                 \
        return 1;                                                            \
    }                                                                        \
} while (0)

typedef struct {
    unsigned char bytes[64];
    size_t bit;
} SpsBits;

static void write_bits(SpsBits *bits, uint32_t value, unsigned count)
{
    for (unsigned i = 0; i < count; i++) {
        unsigned shift = count - i - 1u;
        if (((value >> shift) & 1u) != 0)
            bits->bytes[bits->bit / 8u] |=
                (unsigned char) (1u << (7u - bits->bit % 8u));
        bits->bit++;
    }
}

static void write_ue(SpsBits *bits, uint32_t value)
{
    uint32_t code = value + 1u;
    unsigned width = 0;
    for (uint32_t at = code; at != 0; at >>= 1u) width++;
    write_bits(bits, 0, width - 1u);
    write_bits(bits, code, width);
}

static size_t make_sps(unsigned char output[64])
{
    SpsBits bits = {0};
    write_bits(&bits, 0x67u, 8); /* SPS */
    write_bits(&bits, 100u, 8);  /* High profile */
    write_bits(&bits, 0, 8);
    write_bits(&bits, 30u, 8);
    write_ue(&bits, 0);          /* sps id */
    write_ue(&bits, 1);          /* 4:2:0 */
    write_ue(&bits, 0);          /* bit depth luma */
    write_ue(&bits, 0);          /* bit depth chroma */
    write_bits(&bits, 0, 1);     /* transform bypass */
    write_bits(&bits, 0, 1);     /* scaling matrices */
    write_ue(&bits, 0);          /* frame num */
    write_ue(&bits, 0);          /* POC type */
    write_ue(&bits, 0);          /* POC lsb */
    write_ue(&bits, 1);          /* refs */
    write_bits(&bits, 0, 1);     /* gaps */
    write_ue(&bits, 26u);        /* 432 pixels */
    write_ue(&bits, 14u);        /* 240 pixels */
    write_bits(&bits, 1, 1);     /* progressive */
    write_bits(&bits, 1, 1);     /* direct 8x8 */
    write_bits(&bits, 0, 1);     /* no crop */
    write_bits(&bits, 0, 1);     /* no VUI */
    write_bits(&bits, 1, 1);
    size_t length = (bits.bit + 7u) / 8u;
    memcpy(output, bits.bytes, length);
    return length;
}

static void encode_pts(unsigned char output[5], uint64_t pts)
{
    output[0] = (unsigned char) (0x21u | ((pts >> 29) & 0x0eu));
    output[1] = (unsigned char) (pts >> 22);
    output[2] = (unsigned char) (1u | ((pts >> 14) & 0xfeu));
    output[3] = (unsigned char) (pts >> 7);
    output[4] = (unsigned char) (1u | ((pts << 1) & 0xfeu));
}

static void ts_packet(unsigned char packet[188], unsigned pid, bool pusi,
                      const unsigned char *payload, size_t payload_bytes)
{
    memset(packet, 0xff, 188u);
    packet[0] = 0x47u;
    packet[1] = (unsigned char) ((pusi ? 0x40u : 0u) | (pid >> 8));
    packet[2] = (unsigned char) pid;
    packet[3] = 0x30u;
    size_t adaptation = 183u - payload_bytes;
    packet[4] = (unsigned char) adaptation;
    if (adaptation != 0) packet[5] = 0;
    memcpy(packet + 5u + adaptation, payload, payload_bytes);
}

static void psi_packet(unsigned char packet[188], unsigned pid,
                       const unsigned char *section, size_t length)
{
    unsigned char payload[184] = {0};
    payload[0] = 0;
    memcpy(payload + 1u, section, length);
    ts_packet(packet, pid, true, payload, length + 1u);
}

static size_t pes(unsigned char *output, unsigned stream, uint64_t pts,
                  const unsigned char *payload, size_t length)
{
    output[0] = 0; output[1] = 0; output[2] = 1;
    output[3] = (unsigned char) stream;
    output[4] = 0; output[5] = 0;
    output[6] = 0x80u; output[7] = 0x80u; output[8] = 5u;
    encode_pts(output + 9u, pts);
    memcpy(output + 14u, payload, length);
    return length + 14u;
}

typedef struct {
    unsigned char segment[8][188u * 40u];
    size_t segment_bytes[8];
    struct {
        bool active;
        size_t segment;
        size_t offset;
        uint64_t ready_us;
    } request[4];
    unsigned starts;
    unsigned start_calls;
    unsigned admission_deferrals;
    unsigned cancels;
    bool fail_segments;
    size_t read_quantum;
    uint64_t now_us;
    uint64_t following_delay_us;
    unsigned reject_following;
} MockTransport;

typedef struct {
    unsigned submits;
    bool wait_for_audio;
    bool bypass_allowed;
    unsigned audio_attempts;
    unsigned audio_accepted;
    unsigned video_accepted;
    size_t video_bytes;
    unsigned char video_copy[128];
} PlaybackBackend;

static MediaBackendResult playback_submit(
    void *opaque, const MediaMp4Sample *sample,
    const unsigned char *bytes, size_t length,
    char *error, size_t error_size)
{
    (void) error;
    (void) error_size;
    PlaybackBackend *backend = opaque;
    if (backend == NULL || sample == NULL || bytes == NULL
        || length != sample->size) return MEDIA_BACKEND_ERROR;
    backend->submits++;
    if (backend->wait_for_audio) {
        if (sample->kind == MEDIA_MP4_TRACK_VIDEO) {
            if (length > sizeof(backend->video_copy))
                return MEDIA_BACKEND_ERROR;
            if (backend->video_bytes == 0) {
                memcpy(backend->video_copy, bytes, length);
                backend->video_bytes = length;
            } else if (length != backend->video_bytes
                       || memcmp(backend->video_copy, bytes, length) != 0) {
                return MEDIA_BACKEND_ERROR;
            }
            if (backend->audio_accepted == 0)
                return MEDIA_BACKEND_WOULD_BLOCK;
            backend->video_accepted++;
        } else {
            if (backend->audio_attempts++ == 0)
                return MEDIA_BACKEND_WOULD_BLOCK;
            backend->audio_accepted++;
        }
    }
    return MEDIA_BACKEND_ACCEPTED;
}

static MediaBackendResult playback_drain(
    void *opaque, char *error, size_t error_size)
{
    (void) opaque;
    (void) error;
    (void) error_size;
    return MEDIA_BACKEND_END;
}

static bool playback_advance(
    void *opaque, uint64_t clock_us, char *error, size_t error_size)
{
    (void) opaque;
    (void) clock_us;
    (void) error;
    (void) error_size;
    return true;
}

static void playback_destroy(void *opaque)
{
    (void) opaque;
}

static bool playback_not_resident(const void *opaque, const MediaMp4Sample *sample)
{
    (void) opaque;
    (void) sample;
    /* A completed HTTP successor can still need a read to install it. */
    return false;
}

static bool playback_allow_bypass(const void *opaque, int kind)
{
    const PlaybackBackend *backend = opaque;
    return backend->bypass_allowed && kind == MEDIA_MP4_TRACK_VIDEO;
}

static void build_segment(MockTransport *mock, size_t segment, uint64_t pts)
{
    static const unsigned char pat[] = {
        0x00,0xb0,0x0d,0,1,0xc1,0,0,0,1,0xe1,0,0,0,0,0
    };
    static const unsigned char pmt[] = {
        0x02,0xb0,0x17,0,1,0xc1,0,0,0xe1,1,0xf0,0,
        0x1b,0xe1,1,0xf0,0,0x0f,0xe1,2,0xf0,0,0,0,0,0
    };
    unsigned char *at = mock->segment[segment];
    psi_packet(at, 0, pat, sizeof(pat)); at += 188u;
    psi_packet(at, 0x100u, pmt, sizeof(pmt)); at += 188u;
    unsigned char sps[64];
    size_t sps_length = make_sps(sps);
    unsigned char au[128] = {0,0,1};
    memcpy(au + 3u, sps, sps_length);
    size_t au_length = 3u + sps_length;
    static const unsigned char tail[] = {
        0,0,1,0x68,0xce,0x06,0xe2, 0,0,1,0x65,0x80,0x11
    };
    memcpy(au + au_length, tail, sizeof(tail));
    au_length += sizeof(tail);
    unsigned char payload[184];
    size_t bytes = pes(payload, 0xe0u, pts, au, au_length);
    ts_packet(at, 0x101u, true, payload, bytes); at += 188u;
    static const unsigned char adts[] = {
        0xff,0xf1,0x50,0x80,0x00,0xff,0xfc
    };
    bytes = pes(payload, 0xc0u, pts, adts, sizeof(adts));
    ts_packet(at, 0x102u, true, payload, bytes); at += 188u;
    mock->segment_bytes[segment] = (size_t) (at - mock->segment[segment]);
}

/* PAT, a video-only PMT, an IDR access unit at `pts` and one P picture a
   frame later. Returns the bytes written (four TS packets). */
static size_t video_only_ts(unsigned char *out, uint64_t pts)
{
    static const unsigned char pat[] = {
        0x00,0xb0,0x0d,0,1,0xc1,0,0,0,1,0xe1,0,0,0,0,0
    };
    static const unsigned char pmt[] = {
        0x02,0xb0,0x12,0,1,0xc1,0,0,0xe1,1,0xf0,0,
        0x1b,0xe1,1,0xf0,0,0,0,0,0
    };
    unsigned char *at = out;
    psi_packet(at, 0, pat, sizeof(pat)); at += 188u;
    psi_packet(at, 0x100u, pmt, sizeof(pmt)); at += 188u;
    unsigned char sps[64];
    size_t sps_length = make_sps(sps);
    unsigned char au[128] = {0,0,1};
    memcpy(au + 3u, sps, sps_length);
    size_t au_length = 3u + sps_length;
    static const unsigned char tail[] = {
        0,0,1,0x68,0xce,0x06,0xe2, 0,0,1,0x65,0x80,0x11
    };
    memcpy(au + au_length, tail, sizeof(tail));
    au_length += sizeof(tail);
    unsigned char payload[184];
    size_t bytes = pes(payload, 0xe0u, pts, au, au_length);
    ts_packet(at, 0x101u, true, payload, bytes); at += 188u;
    static const unsigned char second_au[] = {
        0,0,1,0x41,0x80,0x22
    };
    bytes = pes(payload, 0xe0u, pts + 3600u,
                second_au, sizeof(second_au));
    ts_packet(at, 0x101u, true, payload, bytes); at += 188u;
    return (size_t) (at - out);
}

static void build_video_only_segment(MockTransport *mock, size_t segment,
                                     uint64_t pts)
{
    mock->segment_bytes[segment] =
        video_only_ts(mock->segment[segment], pts);
}

/* A video-only segment of one IDR then `count - 1` small P pictures, one
   TS packet each, so a delivery visit can publish many samples. */
static void build_video_run_segment(MockTransport *mock, size_t segment,
                                    uint64_t pts, unsigned count)
{
    build_video_only_segment(mock, segment, pts);
    unsigned char *at = mock->segment[segment]
        + mock->segment_bytes[segment] - 188u;
    static const unsigned char picture[] = {0,0,1,0x41,0x80,0x22};
    unsigned char payload[184];
    for (unsigned i = 1; i < count; i++) {
        size_t bytes = pes(payload, 0xe0u, pts + 3600u * i,
                           picture, sizeof(picture));
        ts_packet(at, 0x101u, true, payload, bytes);
        at += 188u;
    }
    mock->segment_bytes[segment] = (size_t) (at - mock->segment[segment]);
}

/* A video-only segment whose pictures arrive in decode order with
   B-frames: one IDR, then units at the given frame offsets, `interval`
   ticks apart. */
static void build_reordered_segment(MockTransport *mock, size_t segment,
                                    uint64_t pts, const unsigned *frames,
                                    unsigned count, uint32_t interval)
{
    build_video_only_segment(mock, segment, pts);
    unsigned char *at = mock->segment[segment]
        + mock->segment_bytes[segment] - 188u;
    static const unsigned char picture[] = {0,0,1,0x41,0x80,0x22};
    unsigned char payload[184];
    for (unsigned i = 0; i < count; i++) {
        size_t bytes = pes(payload, 0xe0u, pts + (uint64_t) interval * frames[i],
                           picture, sizeof(picture));
        ts_packet(at, 0x101u, true, payload, bytes);
        at += 188u;
    }
    mock->segment_bytes[segment] = (size_t) (at - mock->segment[segment]);
}

static void build_raw_adts_segment(MockTransport *mock, size_t segment)
{
    static const unsigned char id3[] = {
        'I','D','3', 4,0,0, 0,0,0,4, 't','e','s','t'
    };
    static const unsigned char adts[] = {
        0xff,0xf1,0x4c,0x80,0x00,0xff,0xfc
    };
    unsigned char *at = mock->segment[segment];
    memcpy(at, id3, sizeof(id3));
    at += sizeof(id3);
    memcpy(at, adts, sizeof(adts));
    at += sizeof(adts);
    memcpy(at, adts, sizeof(adts));
    at += sizeof(adts);
    mock->segment_bytes[segment] = (size_t) (at - mock->segment[segment]);
}

static uint64_t mock_start(void *opaque, const char *url, size_t maximum,
                           char *error, size_t error_size)
{
    MockTransport *mock = opaque;
    (void) error; (void) error_size;
    mock->start_calls++;
    if (mock->admission_deferrals != 0) {
        mock->admission_deferrals--;
        return 0;
    }
    if (maximum != MEDIA_HLS_MAXIMUM_SEGMENT_BYTES) return 0;
    size_t segment = 0;
    static const char *const names[] = {
        "one.ts", "two.ts", "three.ts", "four.ts",
        "five.ts", "six.ts", "seven.ts", "eight.ts"
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (strstr(url, names[i]) != NULL) { segment = i; break; }
    if (segment != 0 && mock->reject_following != 0) {
        mock->reject_following--;
        snprintf(error, error_size, "optional request refused");
        return 0;
    }
    for (size_t i = 0; i < 4u; i++) {
        if (!mock->request[i].active) {
            mock->request[i].active = true;
            mock->request[i].segment = segment;
            mock->request[i].offset = 0;
            mock->request[i].ready_us = mock->now_us
                + (segment != 0 ? mock->following_delay_us : 0);
            mock->starts++;
            return i + 1u;
        }
    }
    return 0;
}

static MediaHlsTransportPollResult mock_poll(
    void *opaque, uint64_t handle, unsigned char *destination,
    size_t capacity, size_t *length, char *error, size_t error_size)
{
    MockTransport *mock = opaque;
    (void) error; (void) error_size;
    if (length != NULL) *length = 0;
    if (handle == 0 || handle > 4u || !mock->request[handle - 1u].active)
        return MEDIA_HLS_TRANSPORT_ERROR;
    size_t index = handle - 1u;
    if (mock->fail_segments) {
        mock->request[index].active = false;
        if (error != NULL && error_size != 0)
            snprintf(error, error_size, "segment rejected");
        return MEDIA_HLS_TRANSPORT_ERROR;
    }
    size_t segment = mock->request[index].segment;
    size_t offset = mock->request[index].offset;
    if (mock->now_us < mock->request[index].ready_us)
        return MEDIA_HLS_TRANSPORT_WAIT;
    if (offset == mock->segment_bytes[segment]) {
        mock->request[index].active = false;
        return MEDIA_HLS_TRANSPORT_COMPLETE;
    }
    size_t bytes = mock->segment_bytes[segment] - offset;
    size_t quantum = mock->read_quantum != 0 ? mock->read_quantum : 131u;
    if (bytes > quantum) bytes = quantum;
    if (bytes > capacity) bytes = capacity;
    memcpy(destination, mock->segment[segment] + offset, bytes);
    mock->request[index].offset += bytes;
    *length = bytes;
    return MEDIA_HLS_TRANSPORT_CHUNK;
}

static void mock_cancel(void *opaque, uint64_t handle)
{
    MockTransport *mock = opaque;
    if (handle != 0 && handle <= 4u) mock->request[handle - 1u].active = false;
    mock->cancels++;
}

static int test_playlists(void)
{
    Budget budget;
    budget_init(&budget, 2u * 1024u * 1024u);
    char error[160] = {0};
    static const char master[] =
        "#EXTM3U\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=300000,RESOLUTION=426x240\nlow.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=700000,RESOLUTION=640x360\nhigh.m3u8\n";
    MediaHlsPlaylist *playlist = media_hls_playlist_parse(
        &budget, "https://media.invalid/path/master.m3u8",
        (const unsigned char *) master, strlen(master), error, sizeof(error));
    CHECK(playlist != NULL
          && media_hls_playlist_kind(playlist) == MEDIA_HLS_PLAYLIST_MASTER);
    char url[256];
    CHECK(media_hls_playlist_select_variant(
              playlist, 432u, 240u, 240u, url, sizeof(url))
          && strcmp(url, "https://media.invalid/path/low.m3u8") == 0);
    media_hls_playlist_destroy(playlist);

    static const char demuxed[] =
        "#EXTM3U\n"
        "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"stereo\",NAME=\"English\","
        "DEFAULT=YES,URI=\"audio-en.m3u8\"\n"
        "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"stereo\",NAME=\"Alt\","
        "DEFAULT=NO,URI=\"audio-alt.m3u8\"\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=300000,RESOLUTION=426x240,"
        "CODECS=\"avc1.4d4015\",AUDIO=\"stereo\"\nvideo.m3u8\n";
    playlist = media_hls_playlist_parse(
        &budget, "https://media.invalid/live/master.m3u8",
        (const unsigned char *) demuxed, strlen(demuxed),
        error, sizeof(error));
    char audio_url[256];
    CHECK(playlist != NULL && media_hls_playlist_select_streams(
              playlist, 432u, 240u, 240u, url, sizeof(url),
              audio_url, sizeof(audio_url))
          && strcmp(url, "https://media.invalid/live/video.m3u8") == 0
          && strcmp(audio_url,
                    "https://media.invalid/live/audio-en.m3u8") == 0);
    media_hls_playlist_destroy(playlist);

    static const char aac_choice[] =
        "#EXTM3U\n"
        "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"he\",NAME=\"HE\","
        "DEFAULT=YES,URI=\"audio-he.m3u8\"\n"
        "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"lc\",NAME=\"LC\","
        "DEFAULT=YES,URI=\"audio-lc.m3u8\"\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=507418,RESOLUTION=426x240,"
        "CODECS=\"avc1.4d4015,mp4a.40.5\",AUDIO=\"he\"\nhe.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=591418,RESOLUTION=426x240,"
        "CODECS=\"avc1.4d4015,mp4a.40.2\",AUDIO=\"lc\"\nlc.m3u8\n";
    playlist = media_hls_playlist_parse(
        &budget, "https://media.invalid/live/master.m3u8",
        (const unsigned char *) aac_choice, strlen(aac_choice),
        error, sizeof(error));
    CHECK(playlist != NULL && media_hls_playlist_select_streams(
              playlist, 432u, 240u, 240u, url, sizeof(url),
              audio_url, sizeof(audio_url))
          && strcmp(url, "https://media.invalid/live/lc.m3u8") == 0
          && strcmp(audio_url,
                    "https://media.invalid/live/audio-lc.m3u8") == 0);
    media_hls_playlist_destroy(playlist);

    static const char missing_audio[] =
        "#EXTM3U\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=300000,RESOLUTION=426x240,"
        "CODECS=\"avc1.4d4015\",AUDIO=\"missing\"\nvideo.m3u8\n";
    playlist = media_hls_playlist_parse(
        &budget, "https://media.invalid/live/master.m3u8",
        (const unsigned char *) missing_audio, strlen(missing_audio),
        error, sizeof(error));
    CHECK(playlist != NULL
          && media_hls_playlist_select_variant(
              playlist, 432u, 240u, 240u, url, sizeof(url))
          && !media_hls_playlist_select_streams(
              playlist, 432u, 240u, 240u, url, sizeof(url),
              audio_url, sizeof(audio_url)));
    media_hls_playlist_destroy(playlist);

    static const char mixed_codecs[] =
        "#EXTM3U\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=100000,RESOLUTION=426x240,"
        "CODECS=\"vp09.00.10.08,opus\"\nvp9.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=300000,RESOLUTION=426x240,"
        "CODECS=\"avc1.4d4015,mp4a.40.2\"\navc.m3u8\n";
    playlist = media_hls_playlist_parse(
        &budget, "https://media.invalid/path/master.m3u8",
        (const unsigned char *) mixed_codecs, strlen(mixed_codecs),
        error, sizeof(error));
    CHECK(playlist != NULL && media_hls_playlist_select_variant(
              playlist, 432u, 240u, 240u, url, sizeof(url))
          && strcmp(url, "https://media.invalid/path/avc.m3u8") == 0);
    media_hls_playlist_destroy(playlist);

    static const char average_bandwidth[] =
        "#EXTM3U\n"
        "#EXT-X-STREAM-INF:AVERAGE-BANDWIDTH=900000,BANDWIDTH=100000,"
        "CODECS=\"avc1.4d4015,mp4a.40.2\"\nexact-low.m3u8\n"
        "#EXT-X-STREAM-INF:AVERAGE-BANDWIDTH=100000,BANDWIDTH=200000,"
        "CODECS=\"avc1.4d4015,mp4a.40.2\"\nexact-high.m3u8\n";
    playlist = media_hls_playlist_parse(
        &budget, "https://media.invalid/path/master.m3u8",
        (const unsigned char *) average_bandwidth,
        strlen(average_bandwidth), error, sizeof(error));
    CHECK(playlist != NULL && media_hls_playlist_select_variant(
              playlist, 432u, 240u, 240u, url, sizeof(url))
          && strcmp(url, "https://media.invalid/path/exact-low.m3u8") == 0);
    media_hls_playlist_destroy(playlist);

    static const char unsupported[] =
        "#EXTM3U\n#EXT-X-KEY:METHOD=AES-128,URI=\"key\"\n"
        "#EXTINF:4,\none.ts\n#EXT-X-ENDLIST\n";
    CHECK(media_hls_playlist_parse(
              &budget, "https://media.invalid/list.m3u8",
              (const unsigned char *) unsupported, strlen(unsupported),
              error, sizeof(error)) == NULL);
    static const char missing_duration[] =
        "#EXTM3U\none.ts\n#EXT-X-ENDLIST\n";
    CHECK(media_hls_playlist_parse(
              &budget, "https://media.invalid/list.m3u8",
              (const unsigned char *) missing_duration,
              strlen(missing_duration), error, sizeof(error)) == NULL);
    static const char live[] =
        "#EXTM3U\n#EXT-X-TARGETDURATION:5\n"
        "#EXT-X-MEDIA-SEQUENCE:42\n#EXTINF:5,\none.ts\n";
    playlist = media_hls_playlist_parse(
        &budget, "https://media.invalid/live.m3u8",
        (const unsigned char *) live, strlen(live), error, sizeof(error));
    CHECK(playlist != NULL && media_hls_playlist_is_live(playlist)
          && media_hls_playlist_duration_us(playlist) == 5000000u);
    media_hls_playlist_destroy(playlist);
    static const char live_without_target[] =
        "#EXTM3U\n#EXT-X-MEDIA-SEQUENCE:42\n#EXTINF:5,\none.ts\n";
    CHECK(media_hls_playlist_parse(
              &budget, "https://media.invalid/live.m3u8",
              (const unsigned char *) live_without_target,
              strlen(live_without_target), error, sizeof(error)) == NULL);
    CHECK(budget.current == 0);
    return 0;
}

static int test_oversized_live_playlist_stream(void)
{
    Budget budget;
    budget_init(&budget, 2u * 1024u * 1024u);
    char error[160] = {0};
    MediaHlsPlaylistStream *stream = media_hls_playlist_stream_create(
        &budget, "https://media.invalid/live/list.m3u8",
        error, sizeof(error));
    CHECK(stream != NULL);
    static const char header[] =
        "#EXTM3U\n#EXT-X-TARGETDURATION:4\n"
        "#EXT-X-MEDIA-SEQUENCE:1000\n";
    CHECK(media_hls_playlist_stream_feed(
        stream, (const unsigned char *) header, strlen(header),
        error, sizeof(error)));
    char line[3072];
    for (unsigned i = 0; i < 420u; i++) {
        int length = snprintf(
            line, sizeof(line),
            "#EXTINF:4.000,\nsegment-%u.ts?token=%02500u\n", i, i);
        CHECK(length > 0 && (size_t) length < sizeof(line)
              && media_hls_playlist_stream_feed(
                  stream, (const unsigned char *) line, (size_t) length,
                  error, sizeof(error)));
    }
    CHECK(media_hls_playlist_stream_bytes_seen(stream) > 900000u
          && media_hls_playlist_stream_was_compacted(stream));
    MediaHlsPlaylist *playlist = media_hls_playlist_stream_finish(
        stream, error, sizeof(error));
    CHECK(playlist != NULL && media_hls_playlist_is_live(playlist)
          && media_hls_playlist_segment_count(playlist)
               == MEDIA_HLS_RETAINED_LIVE_SEGMENTS);
    media_hls_playlist_destroy(playlist);
    media_hls_playlist_stream_destroy(stream);
    CHECK(budget.current == 0u);
    return 0;
}

/* A small live playlist streamed in pieces parses from its retained text,
   and the stream's retained buffer stays proportional to the playlist. */
static int test_small_live_playlist_stream(void)
{
    Budget budget;
    budget_init(&budget, 2u * 1024u * 1024u);
    char error[160] = {0};
    MediaHlsPlaylistStream *stream = media_hls_playlist_stream_create(
        &budget, "https://media.invalid/live/list.m3u8",
        error, sizeof(error));
    CHECK(stream != NULL);
    static const char first[] =
        "#EXTM3U\n#EXT-X-TARGETDURATION:4\n#EXT-X-MEDIA-SEQUENCE:7\n"
        "#EXTINF:4,\none.ts\n#EXTI";
    static const char second[] =
        "NF:4,\ntwo.ts\n#EXTINF:4,\nthree.ts\n";
    CHECK(media_hls_playlist_stream_feed(
              stream, (const unsigned char *) first, strlen(first),
              error, sizeof(error))
          && media_hls_playlist_stream_feed(
              stream, (const unsigned char *) second, strlen(second),
              error, sizeof(error)));
    CHECK(budget.current < 96u * 1024u);
    MediaHlsPlaylist *playlist = media_hls_playlist_stream_finish(
        stream, error, sizeof(error));
    CHECK(playlist != NULL && media_hls_playlist_is_live(playlist)
          && media_hls_playlist_segment_count(playlist) == 3u
          && media_hls_playlist_duration_us(playlist) == 12000000u);
    media_hls_playlist_destroy(playlist);
    media_hls_playlist_stream_destroy(stream);
    CHECK(budget.current == 0u);

    /* MD-10: a live window of more than 256 entries, in well under the
       text bound, is compacted to its tail like an oversized one; 256 are
       kept whole. The playlist bound (2,048) is not the live one. */
    for (unsigned entries = 256u; entries <= 257u; entries++) {
        stream = media_hls_playlist_stream_create(
            &budget, "https://media.invalid/live/list.m3u8",
            error, sizeof(error));
        CHECK(stream != NULL);
        static const char head[] =
            "#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:100\n";
        CHECK(media_hls_playlist_stream_feed(
            stream, (const unsigned char *) head, strlen(head),
            error, sizeof(error)));
        char line[64];
        for (unsigned i = 0; i < entries; i++) {
            int length = snprintf(line, sizeof(line),
                                  "#EXTINF:2,\n%u.ts\n", i);
            CHECK(media_hls_playlist_stream_feed(
                stream, (const unsigned char *) line, (size_t) length,
                error, sizeof(error)));
        }
        CHECK(media_hls_playlist_stream_bytes_seen(stream) < 64u * 1024u);
        bool compacted = media_hls_playlist_stream_was_compacted(stream);
        playlist = media_hls_playlist_stream_finish(
            stream, error, sizeof(error));
        size_t kept = media_hls_playlist_segment_count(playlist);
        printf("live window of %u short entries: %s, %zu kept\n", entries,
               playlist == NULL ? error
                   : compacted ? "compacted" : "whole", kept);
        CHECK(playlist != NULL && media_hls_playlist_is_live(playlist));
        if (entries <= 256u) {
            CHECK(!compacted && kept == entries);
        } else {
            CHECK(compacted && kept == MEDIA_HLS_RETAINED_LIVE_SEGMENTS);
        }
        media_hls_playlist_destroy(playlist);
        media_hls_playlist_stream_destroy(stream);
        CHECK(budget.current == 0u);
    }
    return 0;
}

/* The whole-text parser and the incremental stream share one tag reader:
   both refuse encryption, fMP4 maps and byte ranges with the same message,
   the stream as soon as the line arrives. Unencrypted keys pass. */
static int test_unsupported_tags_refused_by_both_parsers(void)
{
    static const struct {
        const char *tag;
        const char *message;
    } cases[] = {
        { "#EXT-X-KEY:METHOD=AES-128,URI=\"k\"",
          "encrypted HLS is unsupported" },
        { "#EXT-X-MAP:URI=\"init.mp4\"",
          "fragmented or byte-range HLS is unsupported" },
        { "#EXT-X-BYTERANGE:100@0",
          "fragmented or byte-range HLS is unsupported" },
        { "#EXT-X-KEY:METHOD=NONE", NULL }
    };
    Budget budget;
    budget_init(&budget, 2u * 1024u * 1024u);
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char text[256], error[160] = {0};
        int length = snprintf(
            text, sizeof(text),
            "#EXTM3U\n#EXT-X-TARGETDURATION:4\n%s\n#EXTINF:4,\none.ts\n",
            cases[i].tag);
        CHECK(length > 0 && (size_t) length < sizeof(text));
        MediaHlsPlaylist *playlist = media_hls_playlist_parse(
            &budget, "https://media.invalid/list.m3u8",
            (const unsigned char *) text, (size_t) length,
            error, sizeof(error));
        CHECK((playlist == NULL) == (cases[i].message != NULL));
        CHECK(cases[i].message == NULL
              || strcmp(error, cases[i].message) == 0);
        media_hls_playlist_destroy(playlist);

        MediaHlsPlaylistStream *stream = media_hls_playlist_stream_create(
            &budget, "https://media.invalid/list.m3u8",
            error, sizeof(error));
        CHECK(stream != NULL);
        bool fed = media_hls_playlist_stream_feed(
            stream, (const unsigned char *) text, (size_t) length,
            error, sizeof(error));
        CHECK(fed == (cases[i].message == NULL));
        CHECK(cases[i].message == NULL
              || strcmp(error, cases[i].message) == 0);
        media_hls_playlist_stream_destroy(stream);
    }
    CHECK(budget.current == 0u);
    return 0;
}

/* What an offline live stream's address can answer where a playlist was
   expected: a master with no variant, a playlist that ended with no
   segment, nothing at all, an HTML or JSON error page. Both parsers
   (the PSP reads the playlist incrementally) refuse each with text the
   failed-video panel shows as STREAM OFFLINE OR NOT AVAILABLE. */
static int test_offline_playlists(void)
{
    static const struct {
        const char *body;
        const char *message;
    } cases[] = {
        {"#EXTM3U\n#EXT-X-VERSION:3\n"
         "#EXT-X-INDEPENDENT-SEGMENTS\n", "HLS playlist is empty"},
        {"#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:40\n"
         "#EXT-X-ENDLIST\n", "HLS playlist is empty"},
        {"#EXTM3U\n#EXT-X-TARGETDURATION:2\n", "HLS playlist is empty"},
        {"", "HLS playlist is empty"},
        {"<!doctype html><title>Offline</title>",
         "HLS playlist lacks EXTM3U"},
        {"[{\"error\":\"Can not find channel\"}]",
         "HLS playlist lacks EXTM3U"},
    };
    Budget budget;
    budget_init(&budget, 2u * 1024u * 1024u);
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        size_t length = strlen(cases[i].body);
        char error[160] = {0};
        MediaHlsPlaylist *playlist = media_hls_playlist_parse(
            &budget, "https://media.invalid/live/channel.m3u8",
            (const unsigned char *) cases[i].body, length,
            error, sizeof(error));
        CHECK(playlist == NULL && strcmp(error, cases[i].message) == 0);
        MediaFailureMessage message;
        CHECK(media_failure_message(error, &message)
              && message.kind == MEDIA_FAILURE_STREAM_UNAVAILABLE
              && strcmp(message.headline,
                        "STREAM OFFLINE OR NOT AVAILABLE") == 0);

        error[0] = '\0';
        MediaHlsPlaylistStream *stream = media_hls_playlist_stream_create(
            &budget, "https://media.invalid/live/channel.m3u8",
            error, sizeof(error));
        CHECK(stream != NULL);
        bool fed = media_hls_playlist_stream_feed(
            stream, (const unsigned char *) cases[i].body, length,
            error, sizeof(error));
        if (fed) {
            playlist = media_hls_playlist_stream_finish(
                stream, error, sizeof(error));
            CHECK(playlist == NULL);
        }
        CHECK(strcmp(error, cases[i].message) == 0);
        media_hls_playlist_stream_destroy(stream);
    }
    /* A live master with variants still parses. */
    static const char live[] =
        "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=230000,RESOLUTION=284x160,"
        "CODECS=\"avc1.4D401F,mp4a.40.2\"\n160p30/index.m3u8\n";
    char error[160] = {0};
    MediaHlsPlaylist *playlist = media_hls_playlist_parse(
        &budget, "https://media.invalid/live/channel.m3u8",
        (const unsigned char *) live, strlen(live), error, sizeof(error));
    CHECK(playlist != NULL
          && media_hls_playlist_kind(playlist) == MEDIA_HLS_PLAYLIST_MASTER);
    media_hls_playlist_destroy(playlist);
    CHECK(budget.current == 0u);
    return 0;
}

static int test_live_window_refresh(void)
{
    Budget budget;
    budget_init(&budget, 3u * 1024u * 1024u);
    static const char initial[] =
        "#EXTM3U\n#EXT-X-TARGETDURATION:4\n"
        "#EXT-X-MEDIA-SEQUENCE:100\n"
        "#EXTINF:4,\none.ts\n#EXTINF:4,\ntwo.ts\n"
        "#EXTINF:4,\nthree.ts\n#EXTINF:4,\nfour.ts\n";
    static const char refreshed[] =
        "#EXTM3U\n#EXT-X-TARGETDURATION:4\n"
        "#EXT-X-MEDIA-SEQUENCE:102\n"
        "#EXTINF:4,\nthree.ts\n#EXTINF:4,\nfour.ts\n"
        "#EXTINF:4,\nfive.ts\n#EXTINF:4,\nsix.ts\n";
    char error[160] = {0};
    MediaHlsPlaylist *playlist = media_hls_playlist_parse(
        &budget, "https://media.invalid/live.m3u8",
        (const unsigned char *) initial, strlen(initial),
        error, sizeof(error));
    CHECK(playlist != NULL && media_hls_playlist_is_live(playlist));
    MockTransport mock = {0};
    for (size_t i = 0; i < 6u; i++) build_segment(
        &mock, i, UINT64_C(90000) + (uint64_t) i * UINT64_C(360000));
    MediaHlsTransport transport = {
        .opaque = &mock, .start = mock_start,
        .poll = mock_poll, .cancel = mock_cancel
    };
    MediaHlsSource *source = media_hls_source_create(
        &budget, playlist, &transport, error, sizeof(error));
    CHECK(source != NULL);
    MediaHlsPrimeStatus status = MEDIA_HLS_PRIME_PENDING;
    for (unsigned i = 0; i < 64u && status == MEDIA_HLS_PRIME_PENDING; i++)
        status = media_hls_source_prime(source, error, sizeof(error));
    CHECK(status == MEDIA_HLS_PRIME_READY);
    MediaSampleSource samples;
    CHECK(media_hls_source_sample_source(source, &samples));
    bool refreshed_once = false;
    unsigned consumed = 0;
    unsigned char payload[512];
    for (uint64_t tick = 1; tick < 500u; tick++) {
        uint64_t now_us = tick * UINT64_C(100000);
        media_hls_source_pump(source, now_us);
        if (!refreshed_once
            && media_hls_source_wants_playlist_refresh(source, now_us)) {
            MediaHlsPlaylist *next = media_hls_playlist_parse(
                &budget, "https://media.invalid/live.m3u8",
                (const unsigned char *) refreshed, strlen(refreshed),
                error, sizeof(error));
            CHECK(next != NULL && media_hls_source_update_playlist(
                source, next, now_us, error, sizeof(error)));
            refreshed_once = true;
        }
        MediaMp4Sample sample;
        if (samples.ops->next_sample(samples.opaque, &sample)) {
            CHECK(samples.ops->read_sample(
                samples.opaque, &sample, payload, sizeof(payload)));
            consumed++;
        }
        MediaHlsStats stats = {0};
        media_hls_source_stats(source, &stats);
        if (refreshed_once && stats.segments_completed >= 5u) break;
    }
    MediaHlsStats stats = {0};
    media_hls_source_stats(source, &stats);
    CHECK(refreshed_once && stats.live && !stats.ended
          && stats.playlist_refreshes == 1u
          && stats.segments_completed >= 5u && consumed != 0u);
    uint64_t actual = UINT64_MAX;
    CHECK(!samples.ops->seek_us(samples.opaque, 1000000u, &actual));
    media_hls_source_destroy(source);
    CHECK(budget.current == 0);
    return 0;
}

/*
 * A live stream in the shape YouTube serves (synthetic): a 15-minute DVR
 * window of 2-second segments with long signed addresses, ad cue tags
 * (EXT-X-DATERANGE / EXT-X-CUEPOINT) between them, a discontinuity
 * sequence, program dates and a discontinuity inside the window, reloaded
 * every target duration. Each reload is far over the parse bound, so the
 * incremental capture keeps only the newest window; the source starts near
 * the live edge and a reload two segments later continues with the next
 * segment, skipping none, with presentation times still rising.
 */
static MediaHlsPlaylist *dvr_window(Budget *budget, unsigned newest,
                                    char *error, size_t error_size)
{
    static const char *const names[] = {
        "one.ts", "two.ts", "three.ts", "four.ts",
        "five.ts", "six.ts", "seven.ts", "eight.ts"
    };
    MediaHlsPlaylistStream *stream = media_hls_playlist_stream_create(
        budget, "https://manifest.invalid/hls_playlist/itag/229/index.m3u8",
        error, error_size);
    if (stream == NULL) return NULL;
    char text[2048];
    unsigned first = newest + 1u - 450u;
    int length = snprintf(
        text, sizeof(text),
        "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:2\n"
        "#EXT-X-MEDIA-SEQUENCE:%u\n#EXT-X-DISCONTINUITY-SEQUENCE:20\n",
        first);
    bool fed = length > 0 && media_hls_playlist_stream_feed(
        stream, (const unsigned char *) text, (size_t) length,
        error, error_size);
    for (unsigned sequence = first; fed && sequence <= newest; sequence++) {
        /* Sequences 8800-8807 are the segments the mock serves; older
           ones never reach the transport. */
        const char *name = sequence >= 8800u && sequence <= 8807u
            ? names[sequence - 8800u] : "old.ts";
        length = snprintf(
            text, sizeof(text),
            "%s%s#EXT-X-DATERANGE:CLASS=\"CUEPOINT-AD\",DURATION=2.0,"
            "ID=\"%u-AD-CONTINUE\",START-DATE=\"2026-10-29T03:06:13.028Z\","
            "X-YTCP-CONTEXT=\"%0400u\"\n"
            "#EXT-X-CUEPOINT:CONTEXT=%0300u,DURATION=60.0,EVENT=CONTINUE,"
            "TYPE=AD\n#EXTINF:2.0,\nhttps://rr2.invalid/videoplayback/"
            "itag/229/sq/%u/sig/%0600u/%s\n",
            sequence == newest - 4u ? "#EXT-X-DISCONTINUITY\n" : "",
            sequence % 150u == 0u
                ? "#EXT-X-PROGRAM-DATE-TIME:2026-10-29T02:51:15.025Z\n" : "",
            sequence, sequence, sequence, sequence, sequence, name);
        fed = length > 0 && (size_t) length < sizeof(text)
            && media_hls_playlist_stream_feed(
                stream, (const unsigned char *) text, (size_t) length,
                error, error_size);
    }
    bool compacted = fed && media_hls_playlist_stream_was_compacted(stream);
    MediaHlsPlaylist *playlist = compacted
        ? media_hls_playlist_stream_finish(stream, error, error_size) : NULL;
    media_hls_playlist_stream_destroy(stream);
    return playlist;
}

static int test_dvr_live_window_reloads_at_the_edge(void)
{
    Budget budget;
    budget_init(&budget, 3u * 1024u * 1024u);
    char error[160] = {0};
    /* The first window ends at six.ts (8805); the reload adds seven and
       eight (8807). */
    MediaHlsPlaylist *playlist = dvr_window(
        &budget, 8805u, error, sizeof(error));
    CHECK(playlist != NULL && media_hls_playlist_is_live(playlist)
          && media_hls_playlist_segment_count(playlist)
               == MEDIA_HLS_RETAINED_LIVE_SEGMENTS);
    MockTransport mock = {0};
    for (size_t i = 0; i < 8u; i++)
        build_segment(&mock, i, UINT64_C(90000) + i * UINT64_C(180000));
    MediaHlsTransport transport = {
        .opaque = &mock, .start = mock_start,
        .poll = mock_poll, .cancel = mock_cancel
    };
    MediaHlsSource *source = media_hls_source_create(
        &budget, playlist, &transport, error, sizeof(error));
    CHECK(source != NULL);
    MediaHlsPrimeStatus status = MEDIA_HLS_PRIME_PENDING;
    for (unsigned i = 0; i < 64u && status == MEDIA_HLS_PRIME_PENDING; i++)
        status = media_hls_source_prime(source, error, sizeof(error));
    CHECK(status == MEDIA_HLS_PRIME_READY);
    MediaSampleSource samples;
    CHECK(media_hls_source_sample_source(source, &samples));
    bool reloaded = false;
    unsigned video = 0;
    uint64_t last_dts = 0;
    bool rising = true;
    unsigned char payload[512];
    for (uint64_t tick = 1; tick < 2000u; tick++) {
        uint64_t now_us = tick * UINT64_C(50000);
        media_hls_source_pump(source, now_us);
        if (!reloaded
            && media_hls_source_wants_playlist_refresh(source, now_us)) {
            MediaHlsPlaylist *next = dvr_window(
                &budget, 8807u, error, sizeof(error));
            CHECK(next != NULL && media_hls_source_update_playlist(
                source, next, now_us, error, sizeof(error)));
            reloaded = true;
        }
        MediaMp4Sample sample;
        if (samples.ops->next_sample(samples.opaque, &sample)) {
            CHECK(samples.ops->read_sample(
                samples.opaque, &sample, payload, sizeof(payload)));
            if (sample.kind == MEDIA_MP4_TRACK_VIDEO) {
                rising = rising && (video == 0 || sample.dts > last_dts);
                last_dts = sample.dts;
                video++;
            }
        }
        CHECK(!media_hls_source_failed(source));
        MediaHlsStats stats = {0};
        media_hls_source_stats(source, &stats);
        if (reloaded && stats.segments_completed >= 5u && video >= 5u)
            break;
    }
    MediaHlsStats stats = {0};
    media_hls_source_stats(source, &stats);
    CHECK(reloaded && stats.live && !stats.ended
          && stats.playlist_refreshes == 1u
          && stats.skipped_live_segments == 0u
          && stats.segments_completed >= 5u && video >= 5u && rising);
    media_hls_source_destroy(source);
    CHECK(budget.current == 0);
    return 0;
}

static int test_live_refresh_gap_failure_and_endlist(void)
{
    Budget budget;
    budget_init(&budget, 3u * 1024u * 1024u);
    static const char initial[] =
        "#EXTM3U\n#EXT-X-TARGETDURATION:4\n"
        "#EXT-X-MEDIA-SEQUENCE:100\n"
        "#EXTINF:4,\none.ts\n#EXTINF:4,\ntwo.ts\n";
    static const char final_window[] =
        "#EXTM3U\n#EXT-X-MEDIA-SEQUENCE:104\n"
        "#EXTINF:4,\nfive.ts\n#EXTINF:4,\nsix.ts\n#EXT-X-ENDLIST\n";
    char error[160] = {0};
    MediaHlsPlaylist *playlist = media_hls_playlist_parse(
        &budget, "https://media.invalid/live.m3u8",
        (const unsigned char *) initial, strlen(initial),
        error, sizeof(error));
    CHECK(playlist != NULL);
    MockTransport mock = {0};
    build_segment(&mock, 4u, 90000u);
    build_segment(&mock, 5u, 450000u);
    MediaHlsTransport transport = {
        .opaque = &mock, .start = mock_start,
        .poll = mock_poll, .cancel = mock_cancel
    };
    MediaHlsSource *source = media_hls_source_create(
        &budget, playlist, &transport, error, sizeof(error));
    CHECK(source != NULL);

    /* A failed refresh is retried no faster than the bounded backoff. */
    media_hls_source_note_playlist_refresh_failure(source, 100u);
    CHECK(!media_hls_source_wants_playlist_refresh(source, 1000000u));
    CHECK(media_hls_source_wants_playlist_refresh(source, 1000100u));

    /* The server may have advanced beyond every retained segment while Wi-Fi
       was unavailable. Continue at the oldest surviving sequence, record the
       exact gap, and make the first replacement segment a discontinuity. */
    MediaHlsPlaylist *replacement = media_hls_playlist_parse(
        &budget, "https://media.invalid/live.m3u8",
        (const unsigned char *) final_window, strlen(final_window),
        error, sizeof(error));
    CHECK(replacement != NULL && media_hls_source_update_playlist(
        source, replacement, 1000100u, error, sizeof(error)));
    CHECK(!media_hls_source_wants_playlist_refresh(source, UINT64_MAX));

    MediaSampleSource samples;
    CHECK(media_hls_source_sample_source(source, &samples));
    unsigned char payload[512];
    for (unsigned guard = 0; guard < 256u; guard++) {
        media_hls_source_pump(source, 2000000u + guard * 10000u);
        MediaMp4Sample sample;
        if (samples.ops->next_sample(samples.opaque, &sample)) {
            CHECK(samples.ops->read_sample(
                samples.opaque, &sample, payload, sizeof(payload)));
        }
        MediaHlsStats stats = {0};
        media_hls_source_stats(source, &stats);
        if (stats.ended) break;
    }
    MediaHlsStats stats = {0};
    media_hls_source_stats(source, &stats);
    CHECK(stats.live && stats.ended
          && stats.playlist_refreshes == 1u
          && stats.playlist_refresh_failures == 1u
          && stats.skipped_live_segments == 4u
          && stats.segments_completed == 2u);
    media_hls_source_destroy(source);
    CHECK(budget.current == 0);
    return 0;
}

static int test_live_segment_failure_survives_static_refresh(void)
{
    Budget budget;
    budget_init(&budget, 3u * 1024u * 1024u);
    static const char live[] =
        "#EXTM3U\n#EXT-X-TARGETDURATION:4\n"
        "#EXT-X-MEDIA-SEQUENCE:100\n"
        "#EXTINF:4,\none.ts\n#EXTINF:4,\ntwo.ts\n";
    char error[160] = {0};
    MediaHlsPlaylist *playlist = media_hls_playlist_parse(
        &budget, "https://media.invalid/live.m3u8",
        (const unsigned char *) live, strlen(live), error, sizeof(error));
    CHECK(playlist != NULL);
    MockTransport mock = {.fail_segments = true};
    MediaHlsTransport transport = {
        .opaque = &mock, .start = mock_start,
        .poll = mock_poll, .cancel = mock_cancel
    };
    MediaHlsSource *source = media_hls_source_create(
        &budget, playlist, &transport, error, sizeof(error));
    CHECK(source != NULL);
    for (unsigned attempt = 0; attempt < 4u; attempt++) {
        media_hls_source_pump(source, UINT64_C(1000000) * (attempt + 1u));
        if (media_hls_source_failed(source)) break;
        MediaHlsPlaylist *same = media_hls_playlist_parse(
            &budget, "https://media.invalid/live.m3u8",
            (const unsigned char *) live, strlen(live),
            error, sizeof(error));
        CHECK(same != NULL && media_hls_source_update_playlist(
            source, same, UINT64_C(1000000) * (attempt + 1u),
            error, sizeof(error)));
    }
    CHECK(media_hls_source_failed(source) && mock.starts >= 4u);
    media_hls_source_destroy(source);
    CHECK(budget.current == 0u);
    return 0;
}

static int test_streaming_source(void)
{
    Budget budget;
    budget_init(&budget, 2u * 1024u * 1024u);
    static const char media[] =
        "#EXTM3U\n#EXTINF:4.500,\none.ts\n"
        "#EXT-X-DISCONTINUITY\n#EXTINF:5.250,\ntwo.ts\n#EXT-X-ENDLIST\n";
    char error[160] = {0};
    MediaHlsPlaylist *playlist = media_hls_playlist_parse(
        &budget, "https://media.invalid/path/list.m3u8",
        (const unsigned char *) media, strlen(media), error, sizeof(error));
    CHECK(playlist != NULL && media_hls_playlist_segment_count(playlist) == 2u
          && media_hls_playlist_duration_us(playlist) == 9750000u);
    MockTransport mock = {0};
    mock.admission_deferrals = 2u;
    build_segment(&mock, 0u, 90000u);
    build_segment(&mock, 1u, 495000u);
    MediaHlsTransport transport = {
        .opaque = &mock,
        .start = mock_start,
        .poll = mock_poll,
        .cancel = mock_cancel
    };
    MediaHlsSource *source = media_hls_source_create(
        &budget, playlist, &transport, error, sizeof(error));
    CHECK(source != NULL);
    MediaHlsPrimeStatus status = MEDIA_HLS_PRIME_PENDING;
    for (unsigned i = 0; i < 32u && status == MEDIA_HLS_PRIME_PENDING; i++)
        status = media_hls_source_prime(source, error, sizeof(error));
    CHECK(status == MEDIA_HLS_PRIME_READY && mock.starts == 2u
          && mock.start_calls == 4u);
    MediaMp4TrackInfo video = {0}, audio = {0};
    CHECK(media_hls_source_stream_info(source, &video, &audio)
          && video.width == 432u && video.height == 240u
          && video.packet_format == MEDIA_PACKET_FORMAT_H264_ANNEX_B
          && audio.packet_format == MEDIA_PACKET_FORMAT_AAC_ADTS);
    MediaSampleSource samples;
    CHECK(media_hls_source_sample_source(source, &samples));
    MediaMp4Sample sample;
    unsigned video_samples = 0, audio_samples = 0;
    uint64_t video_pts[2] = {UINT64_MAX, UINT64_MAX};
    uint64_t audio_pts[2] = {UINT64_MAX, UINT64_MAX};
    unsigned char payload[512];
    for (unsigned guard = 0; guard < 128u; guard++) {
        if (samples.ops->next_sample(samples.opaque, &sample)) {
            CHECK(samples.ops->read_sample(
                samples.opaque, &sample, payload, sizeof(payload)));
            if (sample.kind == MEDIA_MP4_TRACK_VIDEO) {
                if (video_samples < 2u) video_pts[video_samples] = sample.dts;
                video_samples++;
            }
            if (sample.kind == MEDIA_MP4_TRACK_AUDIO) {
                if (audio_samples < 2u) audio_pts[audio_samples] = sample.dts;
                audio_samples++;
            }
        } else if (!samples.ops->would_block(samples.opaque)) break;
    }
    CHECK(video_samples == 2u && audio_samples == 2u);
    CHECK(video_pts[0] == 0u && video_pts[1] == 405000u);
    CHECK(audio_pts[0] == 0u && audio_pts[1] == 405000u);
    uint64_t actual = 0;
    /* Hold the restarted segment back so the probe below sees a transfer
       in flight; one prime visit may parse a whole delivered segment. */
    mock.following_delay_us = 1000000u;
    CHECK(samples.ops->seek_us(samples.opaque, 6000000u, &actual)
          && actual == 4500000u);
    /* Repeated readiness probes at an untouched seek target must not cancel
       and restart the same segment transfer. */
    status = media_hls_source_prime(source, error, sizeof(error));
    CHECK(status == MEDIA_HLS_PRIME_PENDING);
    unsigned cancels = mock.cancels;
    CHECK(samples.ops->seek_us(samples.opaque, 6000000u, &actual)
          && actual == 4500000u && mock.cancels == cancels);
    MediaHlsStats stats;
    media_hls_source_stats(source, &stats);
    CHECK(stats.segments_started >= 2u && stats.queue_overflows == 0u);
    media_hls_source_destroy(source);
    CHECK(budget.current == 0);
    return 0;
}

static int test_video_only_primes_before_segment_completion(void)
{
    Budget budget;
    budget_init(&budget, 2u * 1024u * 1024u);
    static const char media[] =
        "#EXTM3U\n#EXTINF:8.000,\none.ts\n#EXT-X-ENDLIST\n";
    char error[160] = {0};
    MediaHlsPlaylist *playlist = media_hls_playlist_parse(
        &budget, "https://media.invalid/path/list.m3u8",
        (const unsigned char *) media, strlen(media), error, sizeof(error));
    CHECK(playlist != NULL);
    MockTransport mock = {0};
    build_video_only_segment(&mock, 0u, 90000u);
    MediaHlsTransport transport = {
        .opaque = &mock,
        .start = mock_start,
        .poll = mock_poll,
        .cancel = mock_cancel
    };
    MediaHlsSource *source = media_hls_source_create(
        &budget, playlist, &transport, error, sizeof(error));
    CHECK(source != NULL);
    MediaHlsPrimeStatus status = MEDIA_HLS_PRIME_PENDING;
    for (unsigned i = 0; i < 32u && status == MEDIA_HLS_PRIME_PENDING; i++)
        status = media_hls_source_prime(source, error, sizeof(error));
    MediaHlsStats stats = {0};
    media_hls_source_stats(source, &stats);
    MediaMp4TrackInfo video = {0}, audio = {0};
    CHECK(status == MEDIA_HLS_PRIME_READY
          && media_hls_source_stream_info(source, &video, &audio)
          && video.width == 432u && audio.codec == 0u
          && stats.segments_completed == 0u
          && stats.queue_overflows == 0u);
    media_hls_source_destroy(source);
    CHECK(budget.current == 0);
    return 0;
}

/* Writes a finite playlist of `count` segments of `seconds` each into
   text; `line_bytes` pads each URI line to that length (0: short names
   "s<index>.ts"). Returns the length, or 0 when it does not fit. */
static size_t long_playlist_text(char *text, size_t capacity, unsigned count,
                                 unsigned seconds, size_t line_bytes)
{
    size_t at = (size_t) snprintf(
        text, capacity, "#EXTM3U\n#EXT-X-TARGETDURATION:%u\n", seconds);
    for (unsigned i = 0; i < count && at < capacity; i++) {
        int written;
        if (line_bytes == 0) {
            written = snprintf(text + at, capacity - at,
                               "#EXTINF:%u.000,\ns%u.ts\n", seconds, i);
        } else {
            /* A CDN-signed absolute URI: fixed prefix, then a signature
               padded so the line is line_bytes long. */
            static const char prefix[] =
                "https://cdn.invalid/vod/7f3a/seg-";
            size_t fixed = strlen(prefix) + 5u + strlen(".ts?sig=");
            if (line_bytes <= fixed) return 0;
            written = snprintf(text + at, capacity - at,
                               "#EXTINF:%u.000,\n%s%05u.ts?sig=%0*u\n",
                               seconds, prefix, i,
                               (int) (line_bytes - fixed), i);
        }
        if (written < 0 || (size_t) written >= capacity - at) return 0;
        at += (size_t) written;
    }
    int written = snprintf(text + at, capacity - at, "#EXT-X-ENDLIST\n");
    if (written < 0 || (size_t) written >= capacity - at) return 0;
    return at + (size_t) written;
}

/* Parses text both whole and through the incremental stream (fed in
   1,000-byte pieces, as a transport delivers it). Both must agree. */
static MediaHlsPlaylist *parse_both_ways(Budget *budget, const char *text,
                                         size_t length, char *error,
                                         size_t error_size)
{
    MediaHlsPlaylist *whole = media_hls_playlist_parse(
        budget, "https://media.invalid/path/list.m3u8",
        (const unsigned char *) text, length, error, error_size);
    char stream_error[160] = {0};
    MediaHlsPlaylistStream *stream = media_hls_playlist_stream_create(
        budget, "https://media.invalid/path/list.m3u8",
        stream_error, sizeof(stream_error));
    if (stream == NULL) {
        media_hls_playlist_destroy(whole);
        return NULL;
    }
    bool fed = true;
    for (size_t at = 0; fed && at < length; at += 1000u) {
        size_t piece = length - at < 1000u ? length - at : 1000u;
        fed = media_hls_playlist_stream_feed(
            stream, (const unsigned char *) text + at, piece,
            stream_error, sizeof(stream_error));
    }
    MediaHlsPlaylist *streamed = fed ? media_hls_playlist_stream_finish(
        stream, stream_error, sizeof(stream_error)) : NULL;
    media_hls_playlist_stream_destroy(stream);
    bool agree = (whole == NULL) == (streamed == NULL)
        && media_hls_playlist_segment_count(whole)
               == media_hls_playlist_segment_count(streamed);
    if (whole == NULL && streamed != NULL) whole = streamed;
    else media_hls_playlist_destroy(streamed);
    if (!agree) {
        fprintf(stderr, "whole and streamed parses disagree: %s / %s\n",
                error, stream_error);
        media_hls_playlist_destroy(whole);
        return NULL;
    }
    return whole;
}

/* MD-10: a finite playlist holds 2,048 segments, about 68 minutes at
   2-second segments; one more is refused, by both parsers, with the
   failure text the panel and the candidate list recognise. */
static int test_finite_playlist_entry_bound(void)
{
    static char text[128u * 1024u];
    for (unsigned count = 2048u; count <= 2049u; count++) {
        size_t length = long_playlist_text(text, sizeof(text), count, 2u, 0);
        CHECK(length != 0 && length < 64u * 1024u);
        Budget budget;
        budget_init(&budget, 2u * 1024u * 1024u);
        char error[160] = {0};
        MediaHlsPlaylist *playlist = parse_both_ways(
            &budget, text, length, error, sizeof(error));
        printf("finite playlist of %u segments: %s\n", count,
               playlist != NULL ? "accepted" : error);
        if (count == 2048u) {
            CHECK(playlist != NULL
                  && media_hls_playlist_segment_count(playlist) == count
                  && !media_hls_playlist_is_live(playlist)
                  && media_hls_playlist_duration_us(playlist)
                         == UINT64_C(4096000000));
            /* The playlist's record (the segment table and its base URL),
               apart from the retained text. */
            printf("2,048-segment playlist: %zu bytes of records, "
                   "%zu of text\n",
                   budget.categories[BUDGET_CATEGORY_NAVIGATION].current,
                   budget.categories[BUDGET_CATEGORY_RESOURCE].current);
            CHECK(budget.categories[BUDGET_CATEGORY_NAVIGATION].current
                  < 40u * 1024u);
            char url[160];
            uint32_t duration_ms = 0;
            CHECK(media_hls_playlist_segment(playlist, 2047u, url,
                                             sizeof(url), &duration_ms)
                  && strcmp(url, "https://media.invalid/path/s2047.ts") == 0
                  && duration_ms == 2000u);
        } else {
            CHECK(playlist == NULL
                  && strncmp(error, MEDIA_HLS_PLAYLIST_TOO_LONG,
                             strlen(MEDIA_HLS_PLAYLIST_TOO_LONG)) == 0
                  && strstr(error, "over 2048 entries") != NULL);
            MediaFailureMessage message;
            CHECK(media_failure_message(error, &message)
                  && message.kind == MEDIA_FAILURE_FORMAT_UNSUPPORTED);
        }
        media_hls_playlist_destroy(playlist);
        CHECK(budget.current == 0);
    }
    /* The table is charged to the Budget: a ceiling that admits the text
       but not the playlist record refuses cleanly. */
    size_t length = long_playlist_text(text, sizeof(text), 2048u, 2u, 0);
    Budget tight;
    budget_init(&tight, length + 16u * 1024u);
    char error[160] = {0};
    CHECK(media_hls_playlist_parse(
              &tight, "https://media.invalid/path/list.m3u8",
              (const unsigned char *) text, length,
              error, sizeof(error)) == NULL
          && strcmp(error, "HLS playlist exceeds memory budget") == 0
          && tight.current == 0u);
    return 0;
}

/* MD-10: long signed segment URIs (about 300 bytes a line) are accepted
   up to 128 KiB of playlist text, about 400 segments; past it a finite
   playlist is refused. */
static int test_signed_uri_playlist_text_bound(void)
{
    static char text[160u * 1024u];
    unsigned count = 0;
    size_t length = 0;
    /* The most 300-byte lines that fit in 128 KiB. */
    for (unsigned n = 380u; n < 480u; n++) {
        size_t candidate = long_playlist_text(text, sizeof(text), n, 6u,
                                              300u);
        CHECK(candidate != 0);
        if (candidate > 128u * 1024u) break;
        count = n;
        length = candidate;
    }
    CHECK(count >= 400u && length > 64u * 1024u);
    length = long_playlist_text(text, sizeof(text), count, 6u, 300u);
    Budget budget;
    budget_init(&budget, 2u * 1024u * 1024u);
    char error[160] = {0};
    MediaHlsPlaylist *playlist = parse_both_ways(
        &budget, text, length, error, sizeof(error));
    printf("%u signed 300-byte URIs in %zu bytes: %s\n", count, length,
           playlist != NULL ? "accepted" : error);
    CHECK(playlist != NULL
          && media_hls_playlist_segment_count(playlist) == count
          && !media_hls_playlist_is_live(playlist));
    char url[512], expected[512];
    uint32_t duration_ms = 0;
    CHECK(media_hls_playlist_segment(playlist, count - 1u, url, sizeof(url),
                                     &duration_ms)
          && duration_ms == 6000u);
    snprintf(expected, sizeof(expected),
             "https://cdn.invalid/vod/7f3a/seg-%05u.ts?sig=%0*u", count - 1u,
             (int) (300u - strlen("https://cdn.invalid/vod/7f3a/seg-")
                    - 5u - strlen(".ts?sig=")), count - 1u);
    CHECK(strlen(url) == 300u && strcmp(url, expected) == 0);
    media_hls_playlist_destroy(playlist);
    CHECK(budget.current == 0u);

    /* One more line passes 128 KiB. */
    length = long_playlist_text(text, sizeof(text), count + 1u, 6u, 300u);
    CHECK(length > 128u * 1024u);
    playlist = parse_both_ways(&budget, text, length, error, sizeof(error));
    printf("%u signed URIs in %zu bytes: %s\n", count + 1u, length,
           playlist != NULL ? "accepted" : error);
    CHECK(playlist == NULL);
    MediaHlsPlaylistStream *stream = media_hls_playlist_stream_create(
        &budget, "https://media.invalid/path/list.m3u8",
        error, sizeof(error));
    CHECK(stream != NULL && media_hls_playlist_stream_feed(
              stream, (const unsigned char *) text, length,
              error, sizeof(error)));
    CHECK(media_hls_playlist_stream_finish(stream, error, sizeof(error))
              == NULL
          && strcmp(error, MEDIA_HLS_PLAYLIST_TOO_LONG
                    " (VOD text over 128 KiB)") == 0);
    media_hls_playlist_stream_destroy(stream);
    CHECK(budget.current == 0u);

    /* A live window of the same lines is still compacted once its text
       passes 64 KiB, well under 256 segments. */
    length = long_playlist_text(text, sizeof(text), 230u, 6u, 300u);
    CHECK(length > 64u * 1024u && length < 128u * 1024u);
    length -= strlen("#EXT-X-ENDLIST\n");
    stream = media_hls_playlist_stream_create(
        &budget, "https://media.invalid/path/list.m3u8",
        error, sizeof(error));
    CHECK(stream != NULL && media_hls_playlist_stream_feed(
              stream, (const unsigned char *) text, length,
              error, sizeof(error))
          && media_hls_playlist_stream_was_compacted(stream));
    playlist = media_hls_playlist_stream_finish(stream, error, sizeof(error));
    CHECK(playlist != NULL && media_hls_playlist_is_live(playlist)
          && media_hls_playlist_segment_count(playlist)
                 == MEDIA_HLS_RETAINED_LIVE_SEGMENTS);
    media_hls_playlist_destroy(playlist);
    media_hls_playlist_stream_destroy(stream);
    CHECK(budget.current == 0u);
    return 0;
}

static int prime_source(MediaHlsSource *source);

typedef struct {
    MockTransport mock;
    /* The first URI asked for since `first_url` was cleared. */
    char first_url[96];
} NamedSegmentTransport;

/* Serves segment 0's bytes for every URI and records the first one asked
   for: the mock's segments carry their own timestamps, and a source
   places each segment on the timeline from its EXTINF start. */
static uint64_t named_segment_start(void *opaque, const char *url,
                                    size_t maximum, char *error,
                                    size_t error_size)
{
    NamedSegmentTransport *rig = opaque;
    if (rig->first_url[0] == '\0')
        snprintf(rig->first_url, sizeof(rig->first_url), "%s", url);
    return mock_start(&rig->mock, "one.ts", maximum, error, error_size);
}

static MediaHlsTransportPollResult named_segment_poll(
    void *opaque, uint64_t handle, unsigned char *destination,
    size_t capacity, size_t *length, char *error, size_t error_size)
{
    NamedSegmentTransport *rig = opaque;
    return mock_poll(&rig->mock, handle, destination, capacity, length,
                     error, error_size);
}

static void named_segment_cancel(void *opaque, uint64_t handle)
{
    NamedSegmentTransport *rig = opaque;
    mock_cancel(&rig->mock, handle);
}

/* Reads samples until one at or after at_or_after90k; returns its dts or
   UINT64_MAX. */
static uint64_t first_sample_from(MediaSampleSource *samples,
                                  uint64_t at_or_after90k)
{
    MediaMp4Sample sample;
    unsigned char payload[512];
    for (unsigned guard = 0; guard < 256u; guard++) {
        if (samples->ops->next_sample(samples->opaque, &sample)) {
            if (!samples->ops->read_sample(samples->opaque, &sample,
                                           payload, sizeof(payload)))
                return UINT64_MAX;
            if (sample.dts >= at_or_after90k) return sample.dts;
        } else if (!samples->ops->would_block(samples->opaque)) {
            break;
        }
    }
    return UINT64_MAX;
}

/* MD-10: a 2,048-segment VOD plays from its start, reports its whole
   duration, and seeks to its last segment and plays there. */
static int test_long_playlist_plays_and_seeks(void)
{
    puts("test: a 2,048-segment HLS VOD plays and seeks to its end");
    static char text[128u * 1024u];
    size_t length = long_playlist_text(text, sizeof(text), 2048u, 2u, 0);
    CHECK(length != 0);
    Budget budget;
    budget_init(&budget, 4u * 1024u * 1024u);
    char error[160] = {0};
    MediaHlsPlaylist *playlist = media_hls_playlist_parse(
        &budget, "https://media.invalid/path/list.m3u8",
        (const unsigned char *) text, length, error, sizeof(error));
    CHECK(playlist != NULL);
    static NamedSegmentTransport rig;
    memset(&rig, 0, sizeof(rig));
    build_segment(&rig.mock, 0u, 90000u);
    MediaHlsTransport transport = {
        .opaque = &rig, .start = named_segment_start,
        .poll = named_segment_poll, .cancel = named_segment_cancel
    };
    MediaHlsSource *source = media_hls_source_create(
        &budget, playlist, &transport, error, sizeof(error));
    CHECK(source != NULL && prime_source(source) == 0);
    CHECK(strcmp(rig.first_url, "https://media.invalid/path/s0.ts") == 0);
    MediaMp4TrackInfo video = {0}, audio = {0};
    CHECK(media_hls_source_stream_info(source, &video, &audio)
          && video.timescale == 90000u
          && video.duration == UINT64_C(4096) * 90000u);
    MediaSampleSource samples;
    CHECK(media_hls_source_sample_source(source, &samples));
    CHECK(first_sample_from(&samples, 0u) == 0u);

    /* The last segment starts at 4,094 s. */
    uint64_t actual = 0;
    CHECK(samples.ops->seek_us(samples.opaque, UINT64_C(4095000000),
                               &actual)
          && actual == UINT64_C(4094000000));
    rig.first_url[0] = '\0';
    CHECK(prime_source(source) == 0);
    CHECK(strcmp(rig.first_url, "https://media.invalid/path/s2047.ts") == 0);
    uint64_t dts = first_sample_from(&samples, 0u);
    printf("after the seek: %s, first sample at %llu ticks\n",
           rig.first_url, (unsigned long long) dts);
    CHECK(dts == UINT64_C(4094) * 90000u);
    /* And back into the middle. */
    CHECK(samples.ops->seek_us(samples.opaque, UINT64_C(2001000000),
                               &actual)
          && actual == UINT64_C(2000000000));
    rig.first_url[0] = '\0';
    CHECK(prime_source(source) == 0);
    CHECK(strcmp(rig.first_url, "https://media.invalid/path/s1000.ts") == 0
          && first_sample_from(&samples, 0u) == UINT64_C(2000) * 90000u);
    MediaHlsStats stats;
    media_hls_source_stats(source, &stats);
    CHECK(stats.queue_overflows == 0u && !stats.live);
    media_hls_source_destroy(source);
    CHECK(budget.current == 0u);
    return 0;
}

/* MD-09: a 33-bit PTS wrap inside a segment keeps counting forward; a
   picture shown before the segment's first (B-frame order) still maps to
   the segment start. */
static int test_pts_wrap_inside_a_segment(void)
{
    static const char media[] =
        "#EXTM3U\n#EXTINF:4.000,\none.ts\n#EXT-X-ENDLIST\n";
    Budget budget;
    budget_init(&budget, 4u * 1024u * 1024u);
    char error[160] = {0};
    MediaHlsPlaylist *playlist = media_hls_playlist_parse(
        &budget, "https://media.invalid/path/list.m3u8",
        (const unsigned char *) media, strlen(media), error, sizeof(error));
    CHECK(playlist != NULL);
    static MockTransport mock;
    memset(&mock, 0, sizeof(mock));
    /* IDR at 2^33 - 7200, then pictures at +3600 steps: the third and
       fourth carry PTS 0 and 3600 after the wrap. */
    const uint64_t first = (UINT64_C(1) << 33) - 7200u;
    build_video_run_segment(&mock, 0u, first, 5u);
    MediaHlsTransport transport = {
        .opaque = &mock, .start = mock_start,
        .poll = mock_poll, .cancel = mock_cancel
    };
    MediaHlsSource *source = media_hls_source_create(
        &budget, playlist, &transport, error, sizeof(error));
    CHECK(source != NULL);
    MediaHlsPrimeStatus status = MEDIA_HLS_PRIME_PENDING;
    for (unsigned i = 0; i < 32u && status == MEDIA_HLS_PRIME_PENDING; i++)
        status = media_hls_source_prime(source, error, sizeof(error));
    CHECK(status == MEDIA_HLS_PRIME_READY);
    MediaSampleSource samples;
    CHECK(media_hls_source_sample_source(source, &samples));
    MediaMp4Sample sample;
    unsigned char payload[512];
    uint64_t times[8];
    unsigned count = 0;
    for (unsigned guard = 0; guard < 64u && count < 8u; guard++) {
        if (samples.ops->next_sample(samples.opaque, &sample)) {
            CHECK(samples.ops->read_sample(
                samples.opaque, &sample, payload, sizeof(payload)));
            if (sample.kind == MEDIA_MP4_TRACK_VIDEO)
                times[count++] = sample.dts;
        } else if (!samples.ops->would_block(samples.opaque)) break;
    }
    printf("video times across a PTS wrap:");
    for (unsigned i = 0; i < count; i++)
        printf(" %llu", (unsigned long long) times[i]);
    printf("\n");
    CHECK(count >= 4u);
    for (unsigned i = 0; i < count; i++)
        CHECK(times[i] == 3600u * i);
    media_hls_source_destroy(source);
    CHECK(budget.current == 0);
    return 0;
}

/* MD-07: an audio rendition source does not carry the 512 KiB video
   access-unit buffer of the TS demuxer. */
static int test_audio_rendition_source_memory(void)
{
    static const char media[] =
        "#EXTM3U\n#EXTINF:4.000,\none.ts\n#EXT-X-ENDLIST\n";
    static const MediaHlsTrackSelection selections[] = {
        MEDIA_HLS_TRACK_AUDIO, MEDIA_HLS_TRACK_VIDEO
    };
    size_t charged[2] = {0};
    for (size_t at = 0; at < 2u; at++) {
        Budget budget;
        budget_init(&budget, 4u * 1024u * 1024u);
        char error[160] = {0};
        MediaHlsPlaylist *playlist = media_hls_playlist_parse(
            &budget, "https://media.invalid/path/list.m3u8",
            (const unsigned char *) media, strlen(media),
            error, sizeof(error));
        CHECK(playlist != NULL);
        size_t before = budget.current;
        MockTransport mock = {0};
        MediaHlsTransport transport = {
            .opaque = &mock, .start = mock_start,
            .poll = mock_poll, .cancel = mock_cancel
        };
        MediaHlsSource *source = media_hls_source_create_track(
            &budget, playlist, &transport, selections[at],
            error, sizeof(error));
        CHECK(source != NULL);
        charged[at] = budget.current - before;
        media_hls_source_destroy(source);
        CHECK(budget.current == 0);
    }
    printf("HLS source memory: audio rendition %zu KiB, video %zu KiB\n",
           charged[0] / 1024u, charged[1] / 1024u);
    CHECK(charged[0] < 160u * 1024u);
    CHECK(charged[1] >= 1024u * 1024u);
    return 0;
}

/* Writes `count` audio PES packets of twenty 7-byte ADTS frames each. */
static unsigned char *audio_run_packets(unsigned char *at, uint64_t pts,
                                        unsigned count)
{
    static const unsigned char adts[] = {
        0xff,0xf1,0x50,0x80,0x00,0xff,0xfc
    };
    unsigned char frames[20u * sizeof(adts)];
    for (size_t i = 0; i < 20u; i++)
        memcpy(frames + i * sizeof(adts), adts, sizeof(adts));
    unsigned char payload[184];
    for (unsigned i = 0; i < count; i++) {
        size_t bytes = pes(payload, 0xc0u, pts + 20u * 1920u * i,
                           frames, sizeof(frames));
        ts_packet(at, 0x102u, true, payload, bytes);
        at += 188u;
    }
    return at;
}

/* MD-06: a program with AAC and no video, 80 audio frames. */
static void build_audio_only_ts_segment(MockTransport *mock, size_t segment)
{
    static const unsigned char pat[] = {
        0x00,0xb0,0x0d,0,1,0xc1,0,0,0,1,0xe1,0,0,0,0,0
    };
    static const unsigned char pmt[] = {
        0x02,0xb0,0x12,0,1,0xc1,0,0,0xe1,2,0xf0,0,
        0x0f,0xe1,2,0xf0,0,0,0,0,0
    };
    unsigned char *at = mock->segment[segment];
    psi_packet(at, 0, pat, sizeof(pat)); at += 188u;
    psi_packet(at, 0x100u, pmt, sizeof(pmt)); at += 188u;
    at = audio_run_packets(at, 90000u, 4u);
    mock->segment_bytes[segment] = (size_t) (at - mock->segment[segment]);
}

/* MD-06: video and audio, with 80 audio frames before the first SPS. */
static void build_late_sps_segment(MockTransport *mock, size_t segment)
{
    static const unsigned char pat[] = {
        0x00,0xb0,0x0d,0,1,0xc1,0,0,0,1,0xe1,0,0,0,0,0
    };
    static const unsigned char pmt[] = {
        0x02,0xb0,0x17,0,1,0xc1,0,0,0xe1,1,0xf0,0,
        0x1b,0xe1,1,0xf0,0,0x0f,0xe1,2,0xf0,0,0,0,0,0
    };
    unsigned char *at = mock->segment[segment];
    psi_packet(at, 0, pat, sizeof(pat)); at += 188u;
    psi_packet(at, 0x100u, pmt, sizeof(pmt)); at += 188u;
    at = audio_run_packets(at, 90000u, 4u);
    unsigned char sps[64];
    size_t sps_length = make_sps(sps);
    unsigned char au[128] = {0,0,1};
    memcpy(au + 3u, sps, sps_length);
    size_t au_length = 3u + sps_length;
    static const unsigned char tail[] = {
        0,0,1,0x68,0xce,0x06,0xe2, 0,0,1,0x65,0x80,0x11
    };
    memcpy(au + au_length, tail, sizeof(tail));
    au_length += sizeof(tail);
    unsigned char payload[184];
    size_t bytes = pes(payload, 0xe0u, 90000u + 153600u, au, au_length);
    ts_packet(at, 0x101u, true, payload, bytes); at += 188u;
    static const unsigned char second_au[] = {0,0,1,0x41,0x80,0x22};
    bytes = pes(payload, 0xe0u, 90000u + 153600u + 3600u,
                second_au, sizeof(second_au));
    ts_packet(at, 0x101u, true, payload, bytes); at += 188u;
    mock->segment_bytes[segment] = (size_t) (at - mock->segment[segment]);
}

/* MD-06: a media playlist opened directly is a mixed source, ready only
   with video. Audio-only HLS and video with a late first SPS used to fill
   the 64-sample queue and fail with its bound. */
static int test_mixed_source_without_early_video(void)
{
    static const char media[] =
        "#EXTM3U\n#EXTINF:4.000,\none.ts\n#EXT-X-ENDLIST\n";
    for (unsigned variant = 0; variant < 3u; variant++) {
        Budget budget;
        budget_init(&budget, 2u * 1024u * 1024u);
        char error[160] = {0};
        MediaHlsPlaylist *playlist = media_hls_playlist_parse(
            &budget, "https://media.invalid/path/list.m3u8",
            (const unsigned char *) media, strlen(media),
            error, sizeof(error));
        CHECK(playlist != NULL);
        static MockTransport mock;
        memset(&mock, 0, sizeof(mock));
        if (variant == 0u) build_audio_only_ts_segment(&mock, 0u);
        else if (variant == 1u) build_raw_adts_segment(&mock, 0u);
        else build_late_sps_segment(&mock, 0u);
        MediaHlsTransport transport = {
            .opaque = &mock, .start = mock_start,
            .poll = mock_poll, .cancel = mock_cancel
        };
        MediaHlsSource *source = media_hls_source_create(
            &budget, playlist, &transport, error, sizeof(error));
        CHECK(source != NULL);
        MediaHlsPrimeStatus status = MEDIA_HLS_PRIME_PENDING;
        unsigned calls = 0;
        while (calls < 64u && status == MEDIA_HLS_PRIME_PENDING) {
            status = media_hls_source_prime(source, error, sizeof(error));
            calls++;
        }
        MediaHlsStats stats = {0};
        media_hls_source_stats(source, &stats);
        static const char *const names[] = {
            "audio-only TS", "packed audio", "late SPS"
        };
        printf("mixed %s: status=%d after %u calls, error=\"%s\", "
               "preroll audio dropped=%zu\n", names[variant], (int) status,
               calls, error, stats.preroll_audio_dropped);
        CHECK(stats.queue_overflows == 0u);
        if (variant < 2u) {
            CHECK(status == MEDIA_HLS_PRIME_FAILED
                  && strstr(error, "has no supported track") != NULL);
        } else {
            MediaMp4TrackInfo video = {0}, audio = {0};
            CHECK(status == MEDIA_HLS_PRIME_READY
                  && media_hls_source_stream_info(source, &video, &audio)
                  && video.width == 432u && audio.sample_rate != 0u
                  && stats.preroll_audio_dropped > 0u);
        }
        media_hls_source_destroy(source);
        CHECK(budget.current == 0);
    }
    return 0;
}

/* A video-only segment whose IDR access unit spans `packets` continuation
   TS packets after its first one, then one P picture that completes it. */
static void build_large_keyframe_segment(MockTransport *mock, size_t segment,
                                         uint64_t pts, unsigned packets)
{
    build_video_only_segment(mock, segment, pts);
    unsigned char *at = mock->segment[segment]
        + mock->segment_bytes[segment] - 188u;
    unsigned char payload[184];
    memset(payload, 0x55, sizeof(payload));
    for (unsigned i = 0; i < packets; i++) {
        ts_packet(at, 0x101u, false, payload, 183u);
        at += 188u;
    }
    static const unsigned char picture[] = {0,0,1,0x41,0x80,0x22};
    size_t bytes = pes(payload, 0xe0u, pts + 3600u, picture, sizeof(picture));
    ts_packet(at, 0x101u, true, payload, bytes);
    at += 188u;
    mock->segment_bytes[segment] = (size_t) (at - mock->segment[segment]);
}

/* Prime used to parse one transport slice (at most one TS packet) per call,
   and the open calls it once per main-loop frame: a first keyframe of S
   bytes took S/184 frames, about 2 s for 20 KB at 60 fps, whatever the
   network speed. A call now parses until the source is ready, the
   transport waits, or a bounded slice of bytes arrived. */
static int test_prime_parses_a_keyframe_in_few_visits(void)
{
    Budget budget;
    budget_init(&budget, 2u * 1024u * 1024u);
    static const char media[] =
        "#EXTM3U\n#EXTINF:8.000,\none.ts\n#EXT-X-ENDLIST\n";
    char error[160] = {0};
    MediaHlsPlaylist *playlist = media_hls_playlist_parse(
        &budget, "https://media.invalid/path/list.m3u8",
        (const unsigned char *) media, strlen(media), error, sizeof(error));
    CHECK(playlist != NULL);
    static MockTransport mock;
    memset(&mock, 0, sizeof(mock));
    build_large_keyframe_segment(&mock, 0u, 90000u, 35u);
    CHECK(mock.segment_bytes[0] == 188u * 39u);
    MediaHlsTransport transport = {
        .opaque = &mock,
        .start = mock_start,
        .poll = mock_poll,
        .cancel = mock_cancel
    };
    MediaHlsSource *source = media_hls_source_create(
        &budget, playlist, &transport, error, sizeof(error));
    CHECK(source != NULL);
    MediaHlsPrimeStatus status = MEDIA_HLS_PRIME_PENDING;
    unsigned calls = 0;
    while (calls < 256u && status == MEDIA_HLS_PRIME_PENDING) {
        status = media_hls_source_prime(source, error, sizeof(error));
        calls++;
    }
    MediaHlsStats stats = {0};
    media_hls_source_stats(source, &stats);
    printf("prime of a %zu-byte keyframe segment: %u calls\n",
           mock.segment_bytes[0], calls);
    CHECK(status == MEDIA_HLS_PRIME_READY);
    CHECK(calls <= 2u);
    CHECK(stats.queue_overflows == 0u && stats.segments_completed == 0u);
    /* The keyframe and nothing past the picture that completed it. */
    CHECK(stats.queued_samples == 1u);
    MediaSampleSource samples;
    CHECK(media_hls_source_sample_source(source, &samples));
    MediaMp4Sample sample;
    CHECK(samples.ops->next_sample(samples.opaque, &sample)
          && sample.keyframe && sample.size > 35u * 183u);
    media_hls_source_destroy(source);
    CHECK(budget.current == 0);
    return 0;
}

/* One frame-loop delivery visit must outpace one TS packet: at a ~37 KB/s
   ceiling a 240p stream starved, buffered, and never resumed. The visit stops
   once the queue is too full for proactive delivery and reports holding. */
static int test_delivery_visit_ingests_several_packets(void)
{
    Budget budget;
    budget_init(&budget, 2u * 1024u * 1024u);
    static const char media[] =
        "#EXTM3U\n#EXTINF:1.200,\none.ts\n#EXTINF:1.200,\ntwo.ts\n"
        "#EXT-X-ENDLIST\n";
    char error[160] = {0};
    MediaHlsPlaylist *playlist = media_hls_playlist_parse(
        &budget, "https://media.invalid/path/list.m3u8",
        (const unsigned char *) media, strlen(media), error, sizeof(error));
    CHECK(playlist != NULL);
    MockTransport mock = {0};
    build_video_run_segment(&mock, 0u, 90000u, 30u);
    build_video_run_segment(&mock, 1u, 90000u + 30u * 3600u, 30u);
    MediaHlsTransport transport = {
        .opaque = &mock,
        .start = mock_start,
        .poll = mock_poll,
        .cancel = mock_cancel
    };
    MediaHlsSource *source = media_hls_source_create(
        &budget, playlist, &transport, error, sizeof(error));
    CHECK(source != NULL);
    MediaHlsPrimeStatus status = MEDIA_HLS_PRIME_PENDING;
    for (unsigned i = 0; i < 64u && status == MEDIA_HLS_PRIME_PENDING; i++)
        status = media_hls_source_prime(source, error, sizeof(error));
    CHECK(status == MEDIA_HLS_PRIME_READY);
    MediaHlsStats before = {0}, after = {0};
    media_hls_source_stats(source, &before);
    CHECK(!before.holding && before.queued_samples <= 16u);
    media_hls_source_pump(source, 1000u);
    media_hls_source_stats(source, &after);
    CHECK(after.bytes_received - before.bytes_received > 188u);
    for (unsigned i = 0; i < 16u && !after.holding; i++) {
        media_hls_source_pump(source, 1000u);
        media_hls_source_stats(source, &after);
    }
    CHECK(after.holding && after.queued_samples == 17u
          && after.queue_overflows == 0u);
    /* A holding visit takes no bytes; taking a sample releases it. */
    size_t held_bytes = after.bytes_received;
    media_hls_source_pump(source, 1000u);
    media_hls_source_stats(source, &after);
    CHECK(after.bytes_received == held_bytes);
    MediaSampleSource samples;
    CHECK(media_hls_source_sample_source(source, &samples));
    MediaMp4Sample sample;
    unsigned char payload[512];
    CHECK(samples.ops->next_sample(samples.opaque, &sample)
          && samples.ops->read_sample(
                 samples.opaque, &sample, payload, sizeof(payload)));
    media_hls_source_stats(source, &after);
    CHECK(!after.holding);
    media_hls_source_destroy(source);
    CHECK(budget.current == 0);
    return 0;
}

/* Transport streams carry no durations and B-frames arrive out of order:
   the gap to the previous unit was a P picture's duration (three frames
   here), which made it due early and superseded the Bs before it. Every
   unit is one frame long once the interval has been seen. */
static int test_reordered_units_last_one_frame(void)
{
    puts("test: HLS units with B-frames last one frame, not a decode gap");
    Budget budget;
    budget_init(&budget, 2u * 1024u * 1024u);
    static const char media[] =
        "#EXTM3U\n#EXTINF:1.200,\none.ts\n#EXT-X-ENDLIST\n";
    char error[160] = {0};
    MediaHlsPlaylist *playlist = media_hls_playlist_parse(
        &budget, "https://media.invalid/path/list.m3u8",
        (const unsigned char *) media, strlen(media), error, sizeof(error));
    CHECK(playlist != NULL);
    MockTransport mock = {0};
    /* IDR 0, P 3, B 1, B 2, P 6, B 4, B 5, P 9, B 7, B 8 */
    static const unsigned frames[] = {3, 1, 2, 6, 4, 5, 9, 7, 8};
    build_reordered_segment(&mock, 0u, 90000u, frames,
                            sizeof(frames) / sizeof(frames[0]), 3750u);
    MediaHlsTransport transport = {
        .opaque = &mock,
        .start = mock_start,
        .poll = mock_poll,
        .cancel = mock_cancel
    };
    MediaHlsSource *source = media_hls_source_create(
        &budget, playlist, &transport, error, sizeof(error));
    CHECK(source != NULL);
    MediaHlsPrimeStatus status = MEDIA_HLS_PRIME_PENDING;
    for (unsigned i = 0; i < 64u && status == MEDIA_HLS_PRIME_PENDING; i++)
        status = media_hls_source_prime(source, error, sizeof(error));
    CHECK(status == MEDIA_HLS_PRIME_READY);
    for (unsigned i = 0; i < 16u; i++) media_hls_source_pump(source, 1000u);
    MediaSampleSource samples;
    CHECK(media_hls_source_sample_source(source, &samples));
    MediaMp4Sample sample;
    unsigned seen = 0;
    while (samples.ops->next_sample(samples.opaque, &sample)) {
        unsigned char payload[512];
        CHECK(samples.ops->read_sample(
            samples.opaque, &sample, payload, sizeof(payload)));
        /* The first P is seen before any one-frame gap. */
        if (seen >= 2u) CHECK(sample.duration == 3750u);
        seen++;
    }
    CHECK(seen == 1u + sizeof(frames) / sizeof(frames[0]));
    media_hls_source_destroy(source);
    CHECK(budget.current == 0);
    return 0;
}

/* Every video unit's duration after priming and pumping a playlist. */
static int read_video_durations(const char *media, MockTransport *mock,
                                uint32_t *durations, unsigned capacity,
                                unsigned *count, uint64_t seek_us)
{
    Budget budget;
    budget_init(&budget, 2u * 1024u * 1024u);
    char error[160] = {0};
    MediaHlsPlaylist *playlist = media_hls_playlist_parse(
        &budget, "https://media.invalid/path/list.m3u8",
        (const unsigned char *) media, strlen(media), error, sizeof(error));
    CHECK(playlist != NULL);
    MediaHlsTransport transport = {
        .opaque = mock,
        .start = mock_start,
        .poll = mock_poll,
        .cancel = mock_cancel
    };
    MediaHlsSource *source = media_hls_source_create(
        &budget, playlist, &transport, error, sizeof(error));
    CHECK(source != NULL);
    MediaHlsPrimeStatus status = MEDIA_HLS_PRIME_PENDING;
    for (unsigned i = 0; i < 64u && status == MEDIA_HLS_PRIME_PENDING; i++)
        status = media_hls_source_prime(source, error, sizeof(error));
    CHECK(status == MEDIA_HLS_PRIME_READY);
    MediaSampleSource samples;
    CHECK(media_hls_source_sample_source(source, &samples));
    *count = 0;
    bool seeked = seek_us == 0;
    for (unsigned round = 0; round < 64u; round++) {
        media_hls_source_pump(source, 1000u);
        MediaMp4Sample sample;
        while (samples.ops->next_sample(samples.opaque, &sample)) {
            unsigned char payload[512];
            CHECK(samples.ops->read_sample(
                samples.opaque, &sample, payload, sizeof(payload)));
            if (sample.kind != MEDIA_MP4_TRACK_VIDEO) continue;
            CHECK(*count < capacity);
            durations[(*count)++] = sample.duration;
            if (!seeked && *count == 4u) {
                uint64_t actual = 0;
                CHECK(samples.ops->seek_us(samples.opaque, seek_us, &actual));
                seeked = true;
                *count = 0;
                break;
            }
        }
    }
    media_hls_source_destroy(source);
    CHECK(budget.current == 0);
    return 0;
}

/* The interval is relearnt past a discontinuity (another frame rate), a
   segment placed by EXTINF a little after the last picture does not shrink
   it, and after a seek the landing segment's units are one frame long. */
static int test_reordered_interval_across_segments(void)
{
    puts("test: HLS frame interval across segments, discontinuities, seek");
    /* IDR 0, P 3, B 1, B 2, P 6, B 4, B 5 */
    static const unsigned frames[] = {3, 1, 2, 6, 4, 5};
    uint32_t durations[64];
    unsigned count = 0;
    {
        /* 30 fps then, past a discontinuity, 24 fps. */
        static const char media[] =
            "#EXTM3U\n#EXTINF:0.233,\none.ts\n#EXT-X-DISCONTINUITY\n"
            "#EXTINF:0.292,\ntwo.ts\n#EXT-X-ENDLIST\n";
        MockTransport mock = {0};
        build_reordered_segment(&mock, 0u, 90000u, frames, 6u, 3000u);
        build_reordered_segment(&mock, 1u, 900000u, frames, 6u, 3750u);
        CHECK(read_video_durations(media, &mock, durations, 64u, &count,
                                   0u) == 0);
        CHECK(count == 14u);
        for (unsigned i = 2; i < 7u; i++) CHECK(durations[i] == 3000u);
        for (unsigned i = 9; i < 14u; i++) CHECK(durations[i] == 3750u);
    }
    {
        /* EXTINF 22 ms short of the segment's pictures: the next
           segment's first picture lands 20 ms after the last one. */
        static const char media[] =
            "#EXTM3U\n#EXTINF:0.270,\none.ts\n#EXTINF:0.292,\ntwo.ts\n"
            "#EXT-X-ENDLIST\n";
        MockTransport mock = {0};
        build_reordered_segment(&mock, 0u, 90000u, frames, 6u, 3750u);
        build_reordered_segment(&mock, 1u, 90000u + 7u * 3750u, frames, 6u,
                                3750u);
        CHECK(read_video_durations(media, &mock, durations, 64u, &count,
                                   0u) == 0);
        CHECK(count == 14u);
        for (unsigned i = 2; i < 14u; i++) CHECK(durations[i] == 3750u);
    }
    {
        static const char media[] =
            "#EXTM3U\n#EXTINF:0.292,\none.ts\n#EXTINF:0.292,\ntwo.ts\n"
            "#EXT-X-ENDLIST\n";
        MockTransport mock = {0};
        build_reordered_segment(&mock, 0u, 90000u, frames, 6u, 3750u);
        build_reordered_segment(&mock, 1u, 90000u + 7u * 3750u, frames, 6u,
                                3750u);
        CHECK(read_video_durations(media, &mock, durations, 64u, &count,
                                   300000u) == 0);
        CHECK(count >= 7u);
        for (unsigned i = 0; i < count; i++) CHECK(durations[i] == 3750u);
    }
    {
        /* Learn 30 fps, then seek past the marked discontinuity directly
           into an unmarked 24 fps segment. Keep the first segment longer
           than the proactive queue so its estimate is still current at
           the seek; the discontinuity has not been parsed in advance. */
        static const char media[] =
            "#EXTM3U\n#EXTINF:1.033,\none.ts\n#EXT-X-DISCONTINUITY\n"
            "#EXTINF:0.292,\ntwo.ts\n#EXTINF:0.292,\nthree.ts\n"
            "#EXT-X-ENDLIST\n";
        static const unsigned long_run[] = {
            3, 1, 2, 6, 4, 5, 9, 7, 8, 12, 10, 11, 15, 13, 14,
            18, 16, 17, 21, 19, 20, 24, 22, 23, 27, 25, 26, 30, 28, 29
        };
        MockTransport mock = {0};
        build_reordered_segment(&mock, 0u, 90000u, long_run, 30u, 3000u);
        build_reordered_segment(&mock, 1u, 900000u, frames, 6u, 3750u);
        build_reordered_segment(&mock, 2u, 900000u + 7u * 3750u,
                                frames, 6u, 3750u);
        CHECK(read_video_durations(media, &mock, durations, 64u, &count,
                                   1400000u) == 0);
        CHECK(count == 7u);
        for (unsigned i = 2u; i < count; i++) CHECK(durations[i] == 3750u);
    }
    return 0;
}


static int test_demuxed_track_sources_prime_independently(void)
{
    Budget budget;
    budget_init(&budget, 3u * 1024u * 1024u);
    static const char media[] =
        "#EXTM3U\n#EXTINF:4.000,\none.ts\n#EXT-X-ENDLIST\n";
    char error[160] = {0};
    MediaHlsPlaylist *video_playlist = media_hls_playlist_parse(
        &budget, "https://media.invalid/video.m3u8",
        (const unsigned char *) media, strlen(media), error, sizeof(error));
    MediaHlsPlaylist *audio_playlist = media_hls_playlist_parse(
        &budget, "https://media.invalid/audio.m3u8",
        (const unsigned char *) media, strlen(media), error, sizeof(error));
    CHECK(video_playlist != NULL && audio_playlist != NULL);
    MockTransport video_transport = {0}, audio_transport = {0};
    build_segment(&video_transport, 0u, 90000u);
    build_segment(&audio_transport, 0u, 90000u);
    MediaHlsTransport video_ops = {
        .opaque = &video_transport, .start = mock_start,
        .poll = mock_poll, .cancel = mock_cancel
    };
    MediaHlsTransport audio_ops = {
        .opaque = &audio_transport, .start = mock_start,
        .poll = mock_poll, .cancel = mock_cancel
    };
    MediaHlsSource *video = media_hls_source_create_track(
        &budget, video_playlist, &video_ops, MEDIA_HLS_TRACK_VIDEO,
        error, sizeof(error));
    MediaHlsSource *audio = media_hls_source_create_track(
        &budget, audio_playlist, &audio_ops, MEDIA_HLS_TRACK_AUDIO,
        error, sizeof(error));
    CHECK(video != NULL && audio != NULL);
    MediaHlsPrimeStatus video_status = MEDIA_HLS_PRIME_PENDING;
    MediaHlsPrimeStatus audio_status = MEDIA_HLS_PRIME_PENDING;
    for (unsigned guard = 0; guard < 64u
         && (video_status == MEDIA_HLS_PRIME_PENDING
             || audio_status == MEDIA_HLS_PRIME_PENDING); guard++) {
        if (video_status == MEDIA_HLS_PRIME_PENDING)
            video_status = media_hls_source_prime(
                video, error, sizeof(error));
        if (audio_status == MEDIA_HLS_PRIME_PENDING)
            audio_status = media_hls_source_prime(
                audio, error, sizeof(error));
    }
    MediaMp4TrackInfo video_info = {0}, audio_info = {0};
    CHECK(video_status == MEDIA_HLS_PRIME_READY
          && audio_status == MEDIA_HLS_PRIME_READY
          && media_hls_source_stream_info(video, &video_info, NULL)
          && media_hls_source_stream_info(audio, NULL, &audio_info)
          && video_info.packet_format == MEDIA_PACKET_FORMAT_H264_ANNEX_B
          && audio_info.packet_format == MEDIA_PACKET_FORMAT_AAC_ADTS);
    MediaSampleSource video_samples = {0}, audio_samples = {0};
    CHECK(media_hls_source_sample_source(video, &video_samples)
          && media_hls_source_sample_source(audio, &audio_samples)
          && video_samples.ops->track_count(video_samples.opaque) == 1u
          && audio_samples.ops->track_count(audio_samples.opaque) == 1u);
    MediaMp4Sample sample = {0};
    CHECK(video_samples.ops->next_sample(video_samples.opaque, &sample)
          && sample.kind == MEDIA_MP4_TRACK_VIDEO
          && audio_samples.ops->next_sample(audio_samples.opaque, &sample)
          && sample.kind == MEDIA_MP4_TRACK_AUDIO);
    MediaSampleSourceOps audio_sample_ops = *audio_samples.ops;
    audio_sample_ops.sample_resident = playback_not_resident;
    audio_samples.ops = &audio_sample_ops;
    /* HLS reads retire their queue head.  A firmware backend can transiently
       refuse that copied packet, so playback must retry the retained bytes,
       not ask the streaming source to recreate an already-consumed AU. */
    /* Model the post-seek cycle: the video slots cannot retire until audio
       advances the clock. Also refuse audio once, proving that BOTH consumed
       source payloads survive a pending visit without being re-read. */
    PlaybackBackend playback_fixture = {.wait_for_audio = true};
    MediaBackend backend = {
        .opaque = &playback_fixture,
        .submit = playback_submit,
        .drain = playback_drain,
        .advance = playback_advance,
        .destroy = playback_destroy,
        .allow_submit_bypass = playback_allow_bypass
    };
    MediaPlaybackOptions options = {
        .decode_lead_us = UINT64_C(2000000),
        .maximum_packet_bytes = 1024u * 1024u
    };
    MediaPlayback *playback = media_playback_create_sources(
        &budget, &video_samples, &audio_samples, &backend, &options,
        error, sizeof(error));
    CHECK(playback != NULL);
    CHECK(media_playback_advance_bounded(
              playback, UINT64_C(1000000), 1u, error, sizeof(error))
              == MEDIA_PLAYBACK_ADVANCE_PENDING
          && playback_fixture.audio_attempts == 0u);
    playback_fixture.bypass_allowed = true;
    size_t owned_before_audio = budget.current;
    size_t saved_limit = budget.limit;
    budget.limit = budget.current;
    CHECK(media_playback_advance_bounded(
              playback, UINT64_C(1000000), 1u, error, sizeof(error))
              == MEDIA_PLAYBACK_ADVANCE_PENDING
          && error[0] == '\0'
          && budget.current == owned_before_audio
          && playback_fixture.audio_attempts == 0u);
    budget.limit = saved_limit;
    error[0] = '\0';
    CHECK(playback != NULL
          && media_playback_advance_bounded(
                 playback, UINT64_C(1000000), 1u, error, sizeof(error))
               == MEDIA_PLAYBACK_ADVANCE_PENDING
          && playback_fixture.audio_attempts == 1u
          && media_playback_advance_bounded(
                 playback, UINT64_C(1000000), 1u, error, sizeof(error))
               != MEDIA_PLAYBACK_ADVANCE_ERROR
          && playback_fixture.audio_accepted == 1u
          && playback_fixture.video_accepted == 0u
          && media_playback_advance_bounded(
                 playback, UINT64_C(1000000), 1u, error, sizeof(error))
               != MEDIA_PLAYBACK_ADVANCE_ERROR
          && playback_fixture.video_accepted == 1u);
    media_playback_destroy(playback);
    media_hls_source_destroy(video);
    media_hls_source_destroy(audio);
    CHECK(budget.current == 0u);
    return 0;
}

static int test_demuxed_raw_adts_audio_source(void)
{
    Budget budget;
    budget_init(&budget, 2u * 1024u * 1024u);
    static const char media[] =
        "#EXTM3U\n#EXTINF:4.000,\none.ts\n#EXT-X-ENDLIST\n";
    char error[160] = {0};
    MediaHlsPlaylist *playlist = media_hls_playlist_parse(
        &budget, "https://media.invalid/audio.m3u8",
        (const unsigned char *) media, strlen(media), error, sizeof(error));
    CHECK(playlist != NULL);
    MockTransport mock = {0};
    build_raw_adts_segment(&mock, 0u);
    MediaHlsTransport transport = {
        .opaque = &mock, .start = mock_start,
        .poll = mock_poll, .cancel = mock_cancel
    };
    MediaHlsSource *source = media_hls_source_create_track(
        &budget, playlist, &transport, MEDIA_HLS_TRACK_AUDIO,
        error, sizeof(error));
    CHECK(source != NULL);
    MediaHlsPrimeStatus status = MEDIA_HLS_PRIME_PENDING;
    for (unsigned guard = 0; guard < 32u
         && status == MEDIA_HLS_PRIME_PENDING; guard++) {
        status = media_hls_source_prime(source, error, sizeof(error));
    }
    MediaMp4TrackInfo audio = {0};
    CHECK(status == MEDIA_HLS_PRIME_READY
          && media_hls_source_stream_info(source, NULL, &audio)
          && audio.packet_format == MEDIA_PACKET_FORMAT_AAC_ADTS
          && audio.sample_rate == 48000u && audio.channels == 2u);
    MediaSampleSource samples = {0};
    MediaMp4Sample sample = {0};
    unsigned char payload[16] = {0};
    CHECK(media_hls_source_sample_source(source, &samples)
          && samples.ops->next_sample(samples.opaque, &sample)
          && sample.kind == MEDIA_MP4_TRACK_AUDIO
          && sample.size == 7u
          && samples.ops->read_sample(
              samples.opaque, &sample, payload, sizeof(payload))
          && payload[0] == 0xffu && payload[1] == 0xf1u);
    MediaHlsStats stats = {0};
    media_hls_source_stats(source, &stats);
    CHECK(stats.malformed_segments == 0u && stats.queue_overflows == 0u);
    media_hls_source_destroy(source);
    CHECK(budget.current == 0u);
    return 0;
}

/* Cross the 64 KiB audio payload ring twice, first exactly at a sample
   boundary and then inside a sample. Descriptor-wrap tests use tiny units
   and cannot detect corruption in either half of a split payload. */
static int test_audio_payload_ring_wrap(size_t frame_bytes)
{
    printf("test: audio payload ring wrap frame_bytes=%zu\n", frame_bytes);
    enum { SEGMENTS = 20, FRAMES_PER_SEGMENT = 14 };
    Budget budget;
    budget_init(&budget, 2u * 1024u * 1024u);
    char media[1024] = "#EXTM3U\n", error[160] = {0};
    size_t used = strlen(media);
    for (unsigned i = 0; i < SEGMENTS; i++) {
        int written = snprintf(media + used, sizeof(media) - used,
                               "#EXTINF:0.298,\none.ts\n");
        CHECK(written > 0 && (size_t) written < sizeof(media) - used);
        used += (size_t) written;
    }
    CHECK(sizeof("#EXT-X-ENDLIST\n") <= sizeof(media) - used);
    memcpy(media + used, "#EXT-X-ENDLIST\n", sizeof("#EXT-X-ENDLIST\n"));
    MediaHlsPlaylist *playlist = media_hls_playlist_parse(
        &budget, "https://media.invalid/audio.m3u8",
        (const unsigned char *) media, strlen(media), error, sizeof(error));
    CHECK(playlist != NULL);
    MockTransport mock = {.read_quantum = 131u};
    CHECK(frame_bytes >= 7u && frame_bytes <= 513u
          && frame_bytes * FRAMES_PER_SEGMENT <= sizeof(mock.segment[0]));
    for (unsigned i = 0; i < FRAMES_PER_SEGMENT; i++) {
        unsigned char *frame = mock.segment[0] + i * frame_bytes;
        frame[0] = 0xffu;
        frame[1] = 0xf1u;
        frame[2] = 0x4cu;
        frame[3] = (unsigned char) (0x80u | (frame_bytes >> 11u));
        frame[4] = (unsigned char) (frame_bytes >> 3u);
        frame[5] = (unsigned char) ((frame_bytes << 5u) | 0x1fu);
        frame[6] = 0xfcu;
        for (size_t byte = 7u; byte < frame_bytes; byte++)
            frame[byte] = (unsigned char) (i * 13u + byte);
    }
    mock.segment_bytes[0] = FRAMES_PER_SEGMENT * frame_bytes;
    MediaHlsTransport transport = {
        .opaque = &mock, .start = mock_start, .poll = mock_poll,
        .cancel = mock_cancel
    };
    MediaHlsSource *source = media_hls_source_create_track(
        &budget, playlist, &transport, MEDIA_HLS_TRACK_AUDIO,
        error, sizeof(error));
    CHECK(source != NULL);
    MediaHlsPrimeStatus status = MEDIA_HLS_PRIME_PENDING;
    for (unsigned i = 0; i < 64u && status == MEDIA_HLS_PRIME_PENDING; i++)
        status = media_hls_source_prime(source, error, sizeof(error));
    CHECK(status == MEDIA_HLS_PRIME_READY);
    MediaSampleSource samples = {0};
    CHECK(media_hls_source_sample_source(source, &samples));
    unsigned seen = 0;
    for (unsigned visit = 0; visit < SEGMENTS * FRAMES_PER_SEGMENT * 8u;
         visit++) {
        media_hls_source_pump(source, 0);
        MediaMp4Sample sample;
        if (!samples.ops->next_sample(samples.opaque, &sample)) {
            if (!samples.ops->would_block(samples.opaque)) break;
            continue;
        }
        CHECK(seen < SEGMENTS * FRAMES_PER_SEGMENT
              && sample.kind == MEDIA_MP4_TRACK_AUDIO
              && sample.size == frame_bytes);
        unsigned char actual[513];
        /* Both consumer entry points share the same byte-ring contract. */
        CHECK((seen & 1u)
            ? samples.ops->take_track_sample(
                  samples.opaque, &sample, actual, sizeof(actual))
            : samples.ops->read_sample(
                  samples.opaque, &sample, actual, sizeof(actual)));
        const unsigned char *expected = mock.segment[0]
            + (seen % FRAMES_PER_SEGMENT) * frame_bytes;
        CHECK(memcmp(actual, expected, frame_bytes) == 0);
        seen++;
    }
    MediaHlsStats stats;
    media_hls_source_stats(source, &stats);
    CHECK(seen == SEGMENTS * FRAMES_PER_SEGMENT
          && seen * frame_bytes > 2u * 64u * 1024u
          && stats.ended && stats.segments_completed == SEGMENTS
          && stats.queued_samples == 0u && stats.queued_bytes == 0u
          && stats.queue_overflows == 0u && !media_hls_source_failed(source));
    media_hls_source_destroy(source);
    CHECK(budget.current == 0u);
    return 0;
}

static int test_raw_audio_delivery(unsigned frames, bool delayed,
                                   bool refuse_prefetch)
{
    printf("test: raw audio delivery frames=%u delayed=%d refusal=%d\n",
           frames, delayed, refuse_prefetch);
    Budget budget;
    budget_init(&budget, 2u * 1024u * 1024u);
    char media[256], error[160] = {0};
    /* 48 kHz AAC frames are exactly 1920 ticks on the 90 kHz clock. */
    unsigned duration_ms = frames * 1024u / 48u;
    snprintf(media, sizeof(media),
             "#EXTM3U\n#EXTINF:%u.%03u,\none.ts\n"
             "#EXTINF:%u.%03u,\ntwo.ts\n#EXT-X-ENDLIST\n",
             duration_ms / 1000u, duration_ms % 1000u,
             duration_ms / 1000u, duration_ms % 1000u);
    MediaHlsPlaylist *playlist = media_hls_playlist_parse(
        &budget, "https://media.invalid/audio.m3u8",
        (const unsigned char *) media, strlen(media), error, sizeof(error));
    CHECK(playlist != NULL);
    MockTransport mock = {
        .read_quantum = delayed ? 131u : 4096u,
        .following_delay_us = delayed ? 1500000u : 0u,
        .reject_following = refuse_prefetch ? 1u : 0u
    };
    static const unsigned char frame[] = {
        0xff,0xf1,0x4c,0x80,0x00,0xff,0xfc
    };
    CHECK(frames <= sizeof(mock.segment[0]) / sizeof(frame));
    for (unsigned segment = 0; segment < 2u; segment++) {
        for (unsigned i = 0; i < frames; i++)
            memcpy(mock.segment[segment] + i * sizeof(frame),
                   frame, sizeof(frame));
        mock.segment_bytes[segment] = frames * sizeof(frame);
    }
    MediaHlsTransport transport = {
        .opaque = &mock, .start = mock_start,
        .poll = mock_poll, .cancel = mock_cancel
    };
    MediaHlsSource *source = media_hls_source_create_track(
        &budget, playlist, &transport, MEDIA_HLS_TRACK_AUDIO,
        error, sizeof(error));
    CHECK(source != NULL);
    size_t owned = budget.current;
    MediaHlsPrimeStatus status = MEDIA_HLS_PRIME_PENDING;
    for (unsigned i = 0; i < 64u && status == MEDIA_HLS_PRIME_PENDING; i++)
        status = media_hls_source_prime(source, error, sizeof(error));
    CHECK(status == MEDIA_HLS_PRIME_READY);
    if (refuse_prefetch) CHECK(mock.reject_following == 0u);
    MediaSampleSource samples = {0};
    CHECK(media_hls_source_sample_source(source, &samples));
    unsigned seen = 0, waits = 0, bad_timestamps = 0;
    for (unsigned visit = 0; visit < frames * 2u + 128u; visit++) {
        media_hls_source_pump(source, mock.now_us);
        MediaMp4Sample sample;
        if (samples.ops->next_sample(samples.opaque, &sample)) {
            unsigned char bytes[16];
            CHECK(samples.ops->read_sample(samples.opaque, &sample,
                                           bytes, sizeof(bytes)));
            CHECK(sample.size == sizeof(frame)
                  && memcmp(bytes, frame, sizeof(frame)) == 0);
            uint64_t expected = (seen / frames) * (uint64_t) duration_ms * 90u
                + (seen % frames) * UINT64_C(1920);
            if (sample.pts != (int64_t) expected || sample.duration != 1920u)
                bad_timestamps++;
            seen++;
        } else if (!samples.ops->would_block(samples.opaque)) break;
        else waits++;
        mock.now_us += 21334u;
    }
    MediaHlsStats stats;
    media_hls_source_stats(source, &stats);
    CHECK(!media_hls_source_failed(source) && stats.queue_overflows == 0u
          && stats.segments_completed == 2u && seen == 2u * frames);
    CHECK(bad_timestamps == 0u);
    CHECK(waits == 0u);
    CHECK(budget.current == owned);
    /* A cold seek cancels both current and prefetched requests; neither may
       publish stale samples into the restarted timeline. */
    uint64_t actual = UINT64_MAX;
    CHECK(samples.ops->seek_us(samples.opaque, 0u, &actual) && actual == 0u);
    media_hls_source_pump(source, mock.now_us);
    unsigned active = 0;
    for (unsigned i = 0; i < 4u; i++) active += mock.request[i].active;
    CHECK(active == 2u);
    unsigned cancels = mock.cancels;
    media_hls_source_destroy(source);
    CHECK(mock.cancels == cancels + 2u && budget.current == 0u);
    return 0;
}

/* The PSP routes Baseline/Main AVC to the firmware decoder (up to 640x360
   or 272x480) and High to the software decoder (up to 432x240). Master
   selection must admit each variant by the decoder its CODECS name. */
static int test_decoder_aware_master_selection(void)
{
    Budget budget;
    budget_init(&budget, 2u * 1024u * 1024u);
    char error[160] = {0};
    char url[256], audio_url[256];
    const MediaHlsDecoderLimits psp = {
        .software_width = 432u, .software_height = 240u,
        .target_height = 240u, .admit_undeclared_firmware = true
    };
    MediaHlsSelection selection;

    /* 480p and 360p Main: 480p exceeds the firmware geometry, 360p fits. */
    static const char main_master[] =
        "#EXTM3U\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=900000,RESOLUTION=854x480,"
        "CODECS=\"avc1.4d401f,mp4a.40.2\"\nmain-480.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=500000,RESOLUTION=640x360,"
        "CODECS=\"avc1.4d401e,mp4a.40.2\"\nmain-360.m3u8\n";
    MediaHlsPlaylist *playlist = media_hls_playlist_parse(
        &budget, "https://media.test/hls/master.m3u8",
        (const unsigned char *) main_master, strlen(main_master),
        error, sizeof(error));
    CHECK(playlist != NULL);
    CHECK(media_hls_playlist_select_decodable_streams(
              playlist, &psp, url, sizeof(url),
              audio_url, sizeof(audio_url), &selection)
          && strcmp(url, "https://media.test/hls/main-360.m3u8") == 0
          && audio_url[0] == '\0'
          && selection.video_selected && !selection.unverified
          && selection.width == 640u && selection.height == 360u);
    /* The software-only bound (the old PSP selection) finds nothing. */
    CHECK(!media_hls_playlist_select_streams(
              playlist, 432u, 240u, 240u, url, sizeof(url),
              audio_url, sizeof(audio_url)));
    media_hls_playlist_destroy(playlist);

    /* Baseline is the firmware route too, and portrait 272x480 fits it. */
    static const char baseline_master[] =
        "#EXTM3U\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=400000,RESOLUTION=272x480,"
        "CODECS=\"avc1.42e01e,mp4a.40.2\"\nportrait.m3u8\n";
    playlist = media_hls_playlist_parse(
        &budget, "https://media.test/hls/master.m3u8",
        (const unsigned char *) baseline_master, strlen(baseline_master),
        error, sizeof(error));
    CHECK(playlist != NULL && media_hls_playlist_select_decodable_streams(
              playlist, &psp, url, sizeof(url),
              audio_url, sizeof(audio_url), &selection)
          && strcmp(url, "https://media.test/hls/portrait.m3u8") == 0);
    media_hls_playlist_destroy(playlist);

    /* A High 360p variant stays behind the software bound even when it is
       the smallest at or above 240p; the 240p Main one plays instead. */
    static const char high_master[] =
        "#EXTM3U\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=500000,RESOLUTION=640x360,"
        "CODECS=\"avc1.64001e,mp4a.40.2\"\nhigh-360.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=900000,RESOLUTION=854x480,"
        "CODECS=\"avc1.4d401f,mp4a.40.2\"\nmain-480.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=200000,RESOLUTION=320x180,"
        "CODECS=\"avc1.4d400d,mp4a.40.2\"\nmain-180.m3u8\n";
    playlist = media_hls_playlist_parse(
        &budget, "https://media.test/hls/master.m3u8",
        (const unsigned char *) high_master, strlen(high_master),
        error, sizeof(error));
    CHECK(playlist != NULL && media_hls_playlist_select_decodable_streams(
              playlist, &psp, url, sizeof(url),
              audio_url, sizeof(audio_url), &selection)
          && strcmp(url, "https://media.test/hls/main-180.m3u8") == 0);
    media_hls_playlist_destroy(playlist);

    static const char high_only[] =
        "#EXTM3U\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=500000,RESOLUTION=640x360,"
        "CODECS=\"avc1.64001e,mp4a.40.2\"\nhigh-360.m3u8\n";
    playlist = media_hls_playlist_parse(
        &budget, "https://media.test/hls/master.m3u8",
        (const unsigned char *) high_only, strlen(high_only),
        error, sizeof(error));
    CHECK(playlist != NULL && !media_hls_playlist_select_decodable_streams(
              playlist, &psp, url, sizeof(url),
              audio_url, sizeof(audio_url), &selection)
          && !selection.video_selected);
    media_hls_playlist_destroy(playlist);

    /* Without CODECS the profile is unknown: the firmware bound is tried
       and flagged for an SPS check, and the software bound remains for the
       second selection. */
    static const char undeclared[] =
        "#EXTM3U\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=900000,RESOLUTION=854x480\nu-480.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=500000,RESOLUTION=640x360\nu-360.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=200000,RESOLUTION=320x180\nu-180.m3u8\n";
    playlist = media_hls_playlist_parse(
        &budget, "https://media.test/hls/master.m3u8",
        (const unsigned char *) undeclared, strlen(undeclared),
        error, sizeof(error));
    CHECK(playlist != NULL && media_hls_playlist_select_decodable_streams(
              playlist, &psp, url, sizeof(url),
              audio_url, sizeof(audio_url), &selection)
          && strcmp(url, "https://media.test/hls/u-360.m3u8") == 0
          && selection.unverified);
    const MediaHlsDecoderLimits software_only = {
        .software_width = 432u, .software_height = 240u,
        .target_height = 240u, .admit_undeclared_firmware = false
    };
    CHECK(media_hls_playlist_select_decodable_streams(
              playlist, &software_only, url, sizeof(url),
              audio_url, sizeof(audio_url), &selection)
          && strcmp(url, "https://media.test/hls/u-180.m3u8") == 0
          && !selection.unverified);
    media_hls_playlist_destroy(playlist);

    /* After the firmware decoder refused a variant's coding (a B-frame
       pyramid in its SPS), only the software bound remains, declared Main
       or not. */
    const MediaHlsDecoderLimits firmware_refused = {
        .software_width = 432u, .software_height = 240u,
        .target_height = 240u, .firmware_refused = true
    };
    playlist = media_hls_playlist_parse(
        &budget, "https://media.test/hls/master.m3u8",
        (const unsigned char *) high_master, strlen(high_master),
        error, sizeof(error));
    CHECK(playlist != NULL && media_hls_playlist_select_decodable_streams(
              playlist, &firmware_refused, url, sizeof(url),
              audio_url, sizeof(audio_url), &selection)
          && strcmp(url, "https://media.test/hls/main-180.m3u8") == 0);
    media_hls_playlist_destroy(playlist);
    playlist = media_hls_playlist_parse(
        &budget, "https://media.test/hls/master.m3u8",
        (const unsigned char *) main_master, strlen(main_master),
        error, sizeof(error));
    CHECK(playlist != NULL && !media_hls_playlist_select_decodable_streams(
              playlist, &firmware_refused, url, sizeof(url),
              audio_url, sizeof(audio_url), &selection)
          && !selection.video_selected);
    media_hls_playlist_destroy(playlist);
    static const char main_ladder[] =
        "#EXTM3U\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=500000,RESOLUTION=640x360,"
        "CODECS=\"avc1.4d401e,mp4a.40.2\"\nmain-360.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=200000,RESOLUTION=320x180,"
        "CODECS=\"avc1.4d400d,mp4a.40.2\"\nmain-180.m3u8\n";
    playlist = media_hls_playlist_parse(
        &budget, "https://media.test/hls/master.m3u8",
        (const unsigned char *) main_ladder, strlen(main_ladder),
        error, sizeof(error));
    CHECK(playlist != NULL && media_hls_playlist_select_decodable_streams(
              playlist, &psp, url, sizeof(url),
              audio_url, sizeof(audio_url), &selection)
          && strcmp(url, "https://media.test/hls/main-360.m3u8") == 0
          && media_hls_playlist_select_decodable_streams(
              playlist, &firmware_refused, url, sizeof(url),
              audio_url, sizeof(audio_url), &selection)
          && strcmp(url, "https://media.test/hls/main-180.m3u8") == 0);
    media_hls_playlist_destroy(playlist);
    return 0;
}

/*
 * A muxed TS where video runs ahead of its audio in transport order, played
 * through a fake two-surface decoder whose clock is the audio it accepted.
 *
 * The cycle: one surface is displayed, the other holds a decoded picture due
 * in the future, so the next video access unit has nowhere to decode and is
 * refused. The AAC that would advance the clock to that picture sits behind
 * the refused unit in the one shared source. Unless audio can pass the held
 * video head, the clock never moves, the picture is never shown, its surface
 * is never freed, and playback stops after two frames.
 */
#define CYCLE_VIDEO_UNITS 6u
#define CYCLE_AUDIO_UNITS 12u

typedef struct {
    uint64_t audio_end_us;
    uint64_t last_audio_us;
    uint64_t ready_us;
    bool ready;
    bool audio_out_of_order;
    unsigned video_accepted;
    unsigned video_refused;
    unsigned audio_accepted;
    unsigned presented;
} CycleBackend;

static uint64_t cycle_sample_us(const MediaMp4Sample *sample)
{
    return sample->dts * 1000000u / sample->timescale;
}

static MediaBackendResult cycle_submit(
    void *opaque, const MediaMp4Sample *sample,
    const unsigned char *bytes, size_t length,
    char *error, size_t error_size)
{
    (void) bytes; (void) error; (void) error_size;
    CycleBackend *backend = opaque;
    if (length != sample->size) return MEDIA_BACKEND_ERROR;
    uint64_t at = cycle_sample_us(sample);
    if (sample->kind == MEDIA_MP4_TRACK_VIDEO) {
        /* One surface is displayed; the other is free only when no decoded
           picture is waiting in it. */
        if (backend->ready) {
            backend->video_refused++;
            return MEDIA_BACKEND_WOULD_BLOCK;
        }
        backend->ready = true;
        backend->ready_us = at;
        backend->video_accepted++;
        return MEDIA_BACKEND_ACCEPTED;
    }
    if (backend->audio_accepted != 0 && at <= backend->last_audio_us)
        backend->audio_out_of_order = true;
    backend->last_audio_us = at;
    uint64_t end = at + sample->duration * 1000000u / sample->timescale;
    if (end > backend->audio_end_us) backend->audio_end_us = end;
    backend->audio_accepted++;
    return MEDIA_BACKEND_ACCEPTED;
}

static bool cycle_advance(void *opaque, uint64_t clock_us,
                          char *error, size_t error_size)
{
    (void) error; (void) error_size;
    CycleBackend *backend = opaque;
    /* Showing the waiting picture releases the previously displayed one. */
    if (backend->ready && backend->ready_us <= clock_us) {
        backend->ready = false;
        backend->presented++;
    }
    return true;
}

static bool cycle_reset(void *opaque, char *error, size_t error_size)
{
    (void) error; (void) error_size;
    CycleBackend *backend = opaque;
    *backend = (CycleBackend) {0};
    return true;
}

static bool cycle_allow_bypass(const void *opaque, int kind)
{
    /* The PSP policy: only a future picture can hold the surface/clock
       dependency, so only then may audio pass a refused video unit. */
    const CycleBackend *backend = opaque;
    return kind == MEDIA_MP4_TRACK_VIDEO && backend->ready
        && backend->ready_us > backend->audio_end_us;
}

static void build_video_ahead_segment(MockTransport *mock, size_t segment)
{
    build_video_run_segment(mock, segment, 0u, CYCLE_VIDEO_UNITS);
    /* build_video_run_segment wrote a video-only PMT; declare audio too. */
    static const unsigned char pmt[] = {
        0x02,0xb0,0x17,0,1,0xc1,0,0,0xe1,1,0xf0,0,
        0x1b,0xe1,1,0xf0,0,0x0f,0xe1,2,0xf0,0,0,0,0,0
    };
    psi_packet(mock->segment[segment] + 188u, 0x100u, pmt, sizeof(pmt));
    unsigned char *at = mock->segment[segment] + mock->segment_bytes[segment];
    static const unsigned char adts[] = {
        0xff,0xf1,0x50,0x80,0x00,0xff,0xfc
    };
    unsigned char payload[184];
    for (unsigned i = 0; i < CYCLE_AUDIO_UNITS; i++) {
        size_t bytes = pes(payload, 0xc0u, 2090u * i, adts, sizeof(adts));
        ts_packet(at, 0x102u, true, payload, bytes);
        at += 188u;
    }
    mock->segment_bytes[segment] = (size_t) (at - mock->segment[segment]);
}

static int test_muxed_held_video_does_not_strand_audio(void)
{
    puts("test: a held muxed video head does not strand the audio clock");
    Budget budget;
    budget_init(&budget, 2u * 1024u * 1024u);
    static const char media[] =
        "#EXTM3U\n#EXTINF:4.000,\none.ts\n#EXT-X-ENDLIST\n";
    char error[160] = {0};
    MediaHlsPlaylist *playlist = media_hls_playlist_parse(
        &budget, "https://media.invalid/muxed.m3u8",
        (const unsigned char *) media, strlen(media), error, sizeof(error));
    CHECK(playlist != NULL);
    MockTransport mock = {0};
    build_video_ahead_segment(&mock, 0u);
    MediaHlsTransport transport = {
        .opaque = &mock, .start = mock_start, .poll = mock_poll,
        .cancel = mock_cancel
    };
    MediaHlsSource *source = media_hls_source_create(
        &budget, playlist, &transport, error, sizeof(error));
    CHECK(source != NULL);
    MediaHlsPrimeStatus status = MEDIA_HLS_PRIME_PENDING;
    for (unsigned i = 0; i < 64u && status == MEDIA_HLS_PRIME_PENDING; i++)
        status = media_hls_source_prime(source, error, sizeof(error));
    CHECK(status == MEDIA_HLS_PRIME_READY);
    MediaSampleSource samples;
    CHECK(media_hls_source_sample_source(source, &samples));
    CycleBackend fixture = {0};
    MediaBackend backend = {
        .opaque = &fixture,
        .submit = cycle_submit,
        .drain = playback_drain,
        .advance = cycle_advance,
        .reset = cycle_reset,
        .destroy = playback_destroy,
        .allow_submit_bypass = cycle_allow_bypass
    };
    MediaPlaybackOptions options = {
        .decode_lead_us = 100000u,
        .maximum_packet_bytes = 1024u * 1024u
    };
    MediaPlayback *playback = media_playback_create_sources(
        &budget, &samples, NULL, &backend, &options, error, sizeof(error));
    CHECK(playback != NULL);
    for (unsigned visit = 0;
         visit < 256u && !media_playback_ended(playback); visit++) {
        media_hls_source_pump(source, 0);
        /* The audio clock: what the DAC was given. Starved audio freezes it. */
        CHECK(media_playback_advance_bounded(
                  playback, fixture.audio_end_us, 4u, error, sizeof(error))
              != MEDIA_PLAYBACK_ADVANCE_ERROR);
    }
    CHECK(fixture.video_refused != 0u);
    CHECK(fixture.video_accepted == CYCLE_VIDEO_UNITS);
    CHECK(fixture.audio_accepted == CYCLE_AUDIO_UNITS
          && !fixture.audio_out_of_order);
    CHECK(media_playback_ended(playback));
    MediaPlaybackJobStats stats = {0};
    media_playback_job_stats(playback, &stats);
    CHECK(stats.head_alt_bypasses != 0u && stats.head_alt_submitted != 0u);
    /* A seek inside the segment: video decodes forward from the segment's
       start, sound resumes at the target (0.1 s) through the same cycle. */
    uint64_t actual_us = 0;
    CHECK(media_playback_seek(playback, 100000u, &actual_us,
                              error, sizeof(error))
          && actual_us == 0u);
    for (unsigned visit = 0;
         visit < 256u && !media_playback_ended(playback); visit++) {
        media_hls_source_pump(source, 0);
        if (fixture.audio_end_us < 100000u && fixture.audio_accepted == 0u)
            fixture.audio_end_us = 100000u;
        CHECK(media_playback_advance_bounded(
                  playback, fixture.audio_end_us, 4u, error, sizeof(error))
              != MEDIA_PLAYBACK_ADVANCE_ERROR);
    }
    CHECK(media_playback_ended(playback));
    CHECK(fixture.video_accepted == CYCLE_VIDEO_UNITS);
    /* 1024-sample frames at 44.1 kHz: units 5..11 start at or after 0.1 s. */
    CHECK(fixture.audio_accepted == CYCLE_AUDIO_UNITS - 5u
          && !fixture.audio_out_of_order);
    media_playback_destroy(playback);
    media_hls_source_destroy(source);
    CHECK(budget.current == 0u);
    return 0;
}

/*
 * HLS next_sample only peeks the queue head, so dropping a sound unit from
 * before a seek target without telling the source handed the same unit back
 * on the next pass, forever: a seek inside a muxed segment played a few
 * pictures and then nothing, with no sound at all.
 */
static int test_muxed_seek_drops_preroll_audio(void)
{
    puts("test: a seek inside a muxed segment resumes sound at the target");
    Budget budget;
    budget_init(&budget, 2u * 1024u * 1024u);
    static const char media[] =
        "#EXTM3U\n#EXTINF:4.000,\none.ts\n#EXT-X-ENDLIST\n";
    char error[160] = {0};
    MediaHlsPlaylist *playlist = media_hls_playlist_parse(
        &budget, "https://media.invalid/muxed.m3u8",
        (const unsigned char *) media, strlen(media), error, sizeof(error));
    CHECK(playlist != NULL);
    MockTransport mock = {0};
    build_video_ahead_segment(&mock, 0u);
    MediaHlsTransport transport = {
        .opaque = &mock, .start = mock_start, .poll = mock_poll,
        .cancel = mock_cancel
    };
    MediaHlsSource *source = media_hls_source_create(
        &budget, playlist, &transport, error, sizeof(error));
    CHECK(source != NULL);
    MediaHlsPrimeStatus status = MEDIA_HLS_PRIME_PENDING;
    for (unsigned i = 0; i < 64u && status == MEDIA_HLS_PRIME_PENDING; i++)
        status = media_hls_source_prime(source, error, sizeof(error));
    CHECK(status == MEDIA_HLS_PRIME_READY);
    MediaSampleSource samples;
    CHECK(media_hls_source_sample_source(source, &samples));
    CycleBackend fixture = {0};
    /* No bypass: the decoder never holds a picture past this clock. */
    MediaBackend backend = {
        .opaque = &fixture, .submit = cycle_submit, .drain = playback_drain,
        .advance = cycle_advance, .reset = cycle_reset,
        .destroy = playback_destroy
    };
    MediaPlaybackOptions options = {
        .decode_lead_us = 100000u,
        .maximum_packet_bytes = 1024u * 1024u
    };
    MediaPlayback *playback = media_playback_create_sources(
        &budget, &samples, NULL, &backend, &options, error, sizeof(error));
    CHECK(playback != NULL);
    uint64_t actual_us = 0;
    CHECK(media_playback_seek(playback, 100000u, &actual_us,
                              error, sizeof(error))
          && actual_us == 0u);
    for (unsigned visit = 0;
         visit < 256u && !media_playback_ended(playback); visit++) {
        media_hls_source_pump(source, 0);
        CHECK(media_playback_advance_bounded(
                  playback, UINT64_C(10000000), 4u, error, sizeof(error))
              != MEDIA_PLAYBACK_ADVANCE_ERROR);
    }
    CHECK(media_playback_ended(playback)
          && fixture.video_accepted == CYCLE_VIDEO_UNITS);
    /* 1024-sample frames at 44.1 kHz: units 5..11 start at or after 0.1 s. */
    CHECK(fixture.audio_accepted == CYCLE_AUDIO_UNITS - 5u
          && !fixture.audio_out_of_order);
    media_playback_destroy(playback);
    media_hls_source_destroy(source);
    CHECK(budget.current == 0u);
    return 0;
}

/*
 * Lane coverage. A fake decoder that holds video on request, can refuse audio
 * for a number of attempts, and records every unit it accepted so loss,
 * duplication and reordering are all visible.
 */
#define LANE_MAX_UNITS 512u

typedef struct {
    bool hold_video;
    unsigned video_limit;       /* when nonzero, hold video past this many */
    bool allow_bypass;
    unsigned refuse_audio;      /* refuse this many audio attempts */
    unsigned video_accepted;
    unsigned video_refused;
    unsigned audio_accepted;
    unsigned audio_refused;
    bool out_of_order;
    bool duplicate;
    uint64_t last_video_us;
    uint64_t last_audio_us;
    uint64_t minimum_audio_us;  /* first audio accepted since the last reset */
    uint64_t identity[LANE_MAX_UNITS];
    size_t identities;
} LaneBackend;

static void lane_note(LaneBackend *backend, const MediaMp4Sample *sample)
{
    for (size_t i = 0; i < backend->identities; i++)
        if (backend->identity[i] == sample->offset) backend->duplicate = true;
    if (backend->identities < LANE_MAX_UNITS)
        backend->identity[backend->identities++] = sample->offset;
}

static MediaBackendResult lane_submit(
    void *opaque, const MediaMp4Sample *sample,
    const unsigned char *bytes, size_t length,
    char *error, size_t error_size)
{
    (void) bytes; (void) error; (void) error_size;
    LaneBackend *backend = opaque;
    if (length != sample->size) return MEDIA_BACKEND_ERROR;
    uint64_t at = cycle_sample_us(sample);
    if (sample->kind == MEDIA_MP4_TRACK_VIDEO) {
        if (backend->hold_video
            || (backend->video_limit != 0
                && backend->video_accepted >= backend->video_limit)) {
            backend->video_refused++;
            return MEDIA_BACKEND_WOULD_BLOCK;
        }
        if (backend->video_accepted != 0 && at <= backend->last_video_us)
            backend->out_of_order = true;
        backend->last_video_us = at;
        backend->video_accepted++;
        lane_note(backend, sample);
        return MEDIA_BACKEND_ACCEPTED;
    }
    if (backend->refuse_audio != 0) {
        backend->refuse_audio--;
        backend->audio_refused++;
        return MEDIA_BACKEND_WOULD_BLOCK;
    }
    if (backend->audio_accepted != 0 && at <= backend->last_audio_us)
        backend->out_of_order = true;
    if (backend->audio_accepted == 0) backend->minimum_audio_us = at;
    backend->last_audio_us = at;
    backend->audio_accepted++;
    lane_note(backend, sample);
    return MEDIA_BACKEND_ACCEPTED;
}

static bool lane_reset(void *opaque, char *error, size_t error_size)
{
    (void) error; (void) error_size;
    LaneBackend *backend = opaque;
    bool hold = backend->hold_video, bypass = backend->allow_bypass;
    unsigned limit = backend->video_limit;
    *backend = (LaneBackend) {0};
    backend->hold_video = hold;
    backend->video_limit = limit;
    backend->allow_bypass = bypass;
    return true;
}

static bool lane_allow_bypass(const void *opaque, int kind)
{
    const LaneBackend *backend = opaque;
    return backend->allow_bypass && kind == MEDIA_MP4_TRACK_VIDEO;
}

static const MediaBackend lane_backend_template = {
    .submit = lane_submit,
    .drain = playback_drain,
    .advance = playback_advance,
    .reset = lane_reset,
    .destroy = playback_destroy,
    .allow_submit_bypass = lane_allow_bypass
};

/* One segment of 38 media packets: an IDR (first segment only) or P picture,
   then four AAC frames, repeated. Audio runs ahead of video in time, so a
   held picture leaves many taken AAC units behind an untaken one. */
static void build_audio_heavy_segment(MockTransport *mock, size_t segment,
                                      bool first, unsigned *videos,
                                      unsigned *audios)
{
    static const unsigned char pat[] = {
        0x00,0xb0,0x0d,0,1,0xc1,0,0,0,1,0xe1,0,0,0,0,0
    };
    static const unsigned char pmt[] = {
        0x02,0xb0,0x17,0,1,0xc1,0,0,0xe1,1,0xf0,0,
        0x1b,0xe1,1,0xf0,0,0x0f,0xe1,2,0xf0,0,0,0,0,0
    };
    static const unsigned char adts[] = {
        0xff,0xf1,0x50,0x80,0x00,0xff,0xfc
    };
    static const unsigned char picture[] = {0,0,1,0x41,0x80,0x22};
    static const unsigned char tail[] = {
        0,0,1,0x68,0xce,0x06,0xe2, 0,0,1,0x65,0x80,0x11
    };
    unsigned char *at = mock->segment[segment];
    psi_packet(at, 0, pat, sizeof(pat)); at += 188u;
    psi_packet(at, 0x100u, pmt, sizeof(pmt)); at += 188u;
    unsigned char payload[184];
    unsigned video = 0, audio = 0;
    for (unsigned packet = 0; packet < 38u; packet++) {
        size_t bytes;
        if (packet % 5u == 0) {
            unsigned char au[128] = {0,0,1};
            size_t au_length;
            if (first && video == 0) {
                unsigned char sps[64];
                size_t sps_length = make_sps(sps);
                memcpy(au + 3u, sps, sps_length);
                memcpy(au + 3u + sps_length, tail, sizeof(tail));
                au_length = 3u + sps_length + sizeof(tail);
            } else {
                memcpy(au, picture, sizeof(picture));
                au_length = sizeof(picture);
            }
            bytes = pes(payload, 0xe0u, 3000u * video, au, au_length);
            ts_packet(at, 0x101u, true, payload, bytes);
            video++;
        } else {
            bytes = pes(payload, 0xc0u, 2090u * audio, adts, sizeof(adts));
            ts_packet(at, 0x102u, true, payload, bytes);
            audio++;
        }
        at += 188u;
    }
    mock->segment_bytes[segment] = (size_t) (at - mock->segment[segment]);
    *videos += video;
    *audios += audio;
}

typedef struct {
    Budget budget;
    MockTransport mock;
    MediaHlsSource *source;
    MediaSampleSource samples;
    LaneBackend fixture;
    MediaPlayback *playback;
    unsigned videos;
    unsigned audios;
} LaneRig;

static int lane_rig_open(LaneRig *rig, unsigned segments,
                         uint64_t decode_lead_us)
{
    static const char *const playlists[] = {
        "", "#EXTM3U\n#EXTINF:1.000,\none.ts\n#EXT-X-ENDLIST\n",
        "#EXTM3U\n#EXTINF:1.000,\none.ts\n#EXTINF:1.000,\ntwo.ts\n"
        "#EXT-X-ENDLIST\n",
        "#EXTM3U\n#EXTINF:1.000,\none.ts\n#EXTINF:1.000,\ntwo.ts\n"
        "#EXTINF:1.000,\nthree.ts\n#EXT-X-ENDLIST\n",
        "#EXTM3U\n#EXTINF:1.000,\none.ts\n#EXTINF:1.000,\ntwo.ts\n"
        "#EXTINF:1.000,\nthree.ts\n#EXTINF:1.000,\nfour.ts\n"
        "#EXT-X-ENDLIST\n"
    };
    CHECK(segments >= 1u && segments <= 4u);
    budget_init(&rig->budget, 2u * 1024u * 1024u);
    char error[160] = {0};
    const char *media = playlists[segments];
    MediaHlsPlaylist *playlist = media_hls_playlist_parse(
        &rig->budget, "https://media.invalid/lane.m3u8",
        (const unsigned char *) media, strlen(media), error, sizeof(error));
    CHECK(playlist != NULL);
    for (unsigned i = 0; i < segments; i++)
        build_audio_heavy_segment(&rig->mock, i, i == 0, &rig->videos,
                                  &rig->audios);
    MediaHlsTransport transport = {
        .opaque = &rig->mock, .start = mock_start, .poll = mock_poll,
        .cancel = mock_cancel
    };
    rig->source = media_hls_source_create(
        &rig->budget, playlist, &transport, error, sizeof(error));
    CHECK(rig->source != NULL);
    MediaHlsPrimeStatus status = MEDIA_HLS_PRIME_PENDING;
    for (unsigned i = 0; i < 64u && status == MEDIA_HLS_PRIME_PENDING; i++)
        status = media_hls_source_prime(rig->source, error, sizeof(error));
    CHECK(status == MEDIA_HLS_PRIME_READY);
    CHECK(media_hls_source_sample_source(rig->source, &rig->samples));
    MediaBackend backend = lane_backend_template;
    backend.opaque = &rig->fixture;
    MediaPlaybackOptions options = {
        .decode_lead_us = decode_lead_us,
        .maximum_packet_bytes = 1024u * 1024u
    };
    rig->playback = media_playback_create_sources(
        &rig->budget, &rig->samples, NULL, &backend, &options,
        error, sizeof(error));
    CHECK(rig->playback != NULL);
    return 0;
}

static int lane_rig_visits(LaneRig *rig, unsigned visits)
{
    char error[160] = {0};
    for (unsigned visit = 0;
         visit < visits && !media_playback_ended(rig->playback); visit++) {
        media_hls_source_pump(rig->source, 0);
        MediaPlaybackAdvanceResult result = media_playback_advance_bounded(
            rig->playback, UINT64_C(1000000000), 4u, error, sizeof(error));
        if (result == MEDIA_PLAYBACK_ADVANCE_ERROR)
            fprintf(stderr, "lane advance: %s\n", error);
        CHECK(result != MEDIA_PLAYBACK_ADVANCE_ERROR);
        CHECK(!media_hls_source_failed(rig->source));
    }
    return 0;
}

static int lane_rig_close(LaneRig *rig)
{
    media_playback_destroy(rig->playback);
    media_hls_source_destroy(rig->source);
    CHECK(rig->budget.current == 0u);
    return 0;
}

/*
 * The descriptor ring is 64 entries; delivery holds at 48 spanned or 16
 * waiting. A held picture with audio running far ahead leaves taken AAC
 * entries behind an untaken picture at the head, so the span bound -- not
 * the waiting count -- is what stops delivery. It must stop before the hard
 * bound fails the source, reclaim the taken entries once the head moves,
 * and deliver every unit exactly once.
 */
static int test_lane_queue_saturation(void)
{
    puts("test: taken entries saturate the HLS ring without loss");
    static LaneRig rig;
    memset(&rig, 0, sizeof(rig));
    CHECK(lane_rig_open(&rig, 4u, UINT64_C(100000000)) == 0);
    CHECK(rig.videos + rig.audios > 64u * 2u);
    rig.fixture.allow_bypass = true;
    /* Let the first picture through, then hold every later one. */
    rig.fixture.video_limit = 1u;
    CHECK(lane_rig_visits(&rig, 512u) == 0);
    MediaHlsStats stats;
    media_hls_source_stats(rig.source, &stats);
    /* Stalled by the span bound with few entries actually waiting. */
    CHECK(stats.holding && stats.queued_samples <= 16u
          && stats.queue_overflows == 0u && !stats.ended);
    CHECK(rig.fixture.video_accepted == 1u
          && rig.fixture.audio_accepted > 16u
          && rig.fixture.audio_accepted < rig.audios);
    rig.fixture.video_limit = 0u;
    CHECK(lane_rig_visits(&rig, 2048u) == 0);
    CHECK(media_playback_ended(rig.playback));
    CHECK(rig.fixture.video_accepted == rig.videos
          && rig.fixture.audio_accepted == rig.audios
          && !rig.fixture.out_of_order && !rig.fixture.duplicate);
    media_hls_source_stats(rig.source, &stats);
    CHECK(stats.queued_samples == 0u && stats.queued_bytes == 0u
          && stats.queue_overflows == 0u);
    return lane_rig_close(&rig);
}

/*
 * Memory refusals inside the lane. The lane's packet buffer cannot grow, so
 * the take must not happen at all; and the decoder refuses a taken unit, so
 * the lane must keep exactly that unit and offer it again.
 */
static int test_lane_refusals_roll_back(void)
{
    puts("test: lane allocation and submit refusals keep every unit");
    static LaneRig rig;
    memset(&rig, 0, sizeof(rig));
    CHECK(lane_rig_open(&rig, 1u, UINT64_C(100000000)) == 0);
    rig.fixture.allow_bypass = true;
    /* No room for the lane's packet: nothing is taken, nothing fails. The
       head's own packet was reserved at create, so only the lane is short. */
    rig.fixture.video_limit = 1u;
    size_t saved_limit = rig.budget.limit;
    rig.budget.limit = rig.budget.current;
    CHECK(lane_rig_visits(&rig, 8u) == 0);
    CHECK(rig.fixture.video_accepted == 1u
          && rig.fixture.video_refused != 0u);
    unsigned audio_before = rig.fixture.audio_accepted;
    size_t packet_bytes = media_playback_packet_bytes(rig.playback);
    CHECK(lane_rig_visits(&rig, 32u) == 0);
    /* The lane's buffer never grew, so nothing was taken. */
    CHECK(rig.fixture.audio_accepted == audio_before
          && media_playback_packet_bytes(rig.playback) == packet_bytes
          && rig.fixture.video_accepted == 1u);
    MediaPlaybackJobStats job = {0};
    media_playback_job_stats(rig.playback, &job);
    CHECK(job.head_alt_bypasses != 0u && job.head_alt_submitted == 0u);
    rig.budget.limit = saved_limit;
    /* The decoder refuses the first taken units: they stay in the lane. */
    rig.fixture.refuse_audio = 3u;
    CHECK(lane_rig_visits(&rig, 64u) == 0);
    CHECK(rig.fixture.audio_refused == 3u
          && rig.fixture.audio_accepted > audio_before
          && !rig.fixture.duplicate && !rig.fixture.out_of_order);
    rig.fixture.video_limit = 0u;
    CHECK(lane_rig_visits(&rig, 1024u) == 0);
    CHECK(media_playback_ended(rig.playback)
          && rig.fixture.video_accepted == rig.videos
          && rig.fixture.audio_accepted == rig.audios
          && !rig.fixture.out_of_order && !rig.fixture.duplicate);
    return lane_rig_close(&rig);
}

/*
 * A seek while the lane holds a taken unit the decoder refused. The unit is
 * from before the seek and must never be submitted afterwards; the seek
 * starts both tracks again from the target.
 */
static int test_lane_seek_discards_taken_audio(void)
{
    puts("test: a seek drops the lane's taken, refused unit");
    static LaneRig rig;
    memset(&rig, 0, sizeof(rig));
    CHECK(lane_rig_open(&rig, 2u, UINT64_C(100000000)) == 0);
    rig.fixture.allow_bypass = true;
    rig.fixture.video_limit = 1u;
    CHECK(lane_rig_visits(&rig, 8u) == 0);
    CHECK(rig.fixture.video_refused != 0u);
    unsigned lane_audio = rig.fixture.audio_accepted;
    CHECK(lane_audio > 4u);
    rig.fixture.refuse_audio = 1000u;
    CHECK(lane_rig_visits(&rig, 16u) == 0);
    CHECK(rig.fixture.audio_refused != 0u
          && rig.fixture.audio_accepted == lane_audio);
    rig.fixture.video_limit = 0u;
    rig.fixture.refuse_audio = 0u;
    /* Into the second segment, past its first AAC frames. */
    char error[160] = {0};
    uint64_t actual_us = 0;
    CHECK(media_playback_seek(rig.playback, 1100000u, &actual_us,
                              error, sizeof(error))
          && actual_us == 1000000u);
    CHECK(lane_rig_visits(&rig, 1024u) == 0);
    CHECK(media_playback_ended(rig.playback));
    /* Second segment only: 8 pictures from its start, AAC from 1.1 s on
       (frames 5.. of 30 at 23.2 ms). */
    CHECK(rig.fixture.video_accepted == 8u
          && rig.fixture.audio_accepted == 30u - 5u
          && rig.fixture.minimum_audio_us >= 1100000u
          && !rig.fixture.out_of_order && !rig.fixture.duplicate);
    return lane_rig_close(&rig);
}

/*
 * VOD duration and seek. The player's timeline, seek preview and resume
 * read the duration from stream_info; HLS left it 0 there, so a VOD page
 * video could never be seeked. Live keeps none (and refuses seeks).
 */
static int prime_source(MediaHlsSource *source)
{
    char error[160] = {0};
    MediaHlsPrimeStatus status = MEDIA_HLS_PRIME_PENDING;
    for (unsigned i = 0; i < 64u && status == MEDIA_HLS_PRIME_PENDING; i++)
        status = media_hls_source_prime(source, error, sizeof(error));
    return status == MEDIA_HLS_PRIME_READY ? 0 : 1;
}

static int check_source_duration(const char *text, bool live,
                                 uint64_t playlist_us, uint64_t track90k)
{
    Budget budget;
    budget_init(&budget, 2u * 1024u * 1024u);
    char error[160] = {0};
    MediaHlsPlaylist *playlist = media_hls_playlist_parse(
        &budget, "https://media.invalid/path/list.m3u8",
        (const unsigned char *) text, strlen(text), error, sizeof(error));
    CHECK(playlist != NULL);
    CHECK(media_hls_playlist_is_live(playlist) == live
          && media_hls_playlist_duration_us(playlist) == playlist_us);
    static MockTransport mock;
    memset(&mock, 0, sizeof(mock));
    for (size_t i = 0; i < 4u; i++)
        build_segment(&mock, i, 90000u + 360000u * i);
    MediaHlsTransport transport = {
        .opaque = &mock, .start = mock_start, .poll = mock_poll,
        .cancel = mock_cancel
    };
    MediaHlsSource *source = media_hls_source_create(
        &budget, playlist, &transport, error, sizeof(error));
    CHECK(source != NULL && prime_source(source) == 0);
    MediaMp4TrackInfo video = {0}, audio = {0};
    CHECK(media_hls_source_stream_info(source, &video, &audio)
          && video.timescale == 90000u && video.duration == track90k
          && audio.timescale == 90000u && audio.duration == track90k);
    MediaSampleSource samples;
    CHECK(media_hls_source_sample_source(source, &samples));
    MediaMp4TrackInfo info = {0};
    CHECK(samples.ops->track_info(samples.opaque, 0u, &info)
          && info.duration == track90k);
    MediaHlsStats stats = {0};
    media_hls_source_stats(source, &stats);
    CHECK(stats.live == live);
    uint64_t actual = UINT64_MAX;
    bool seeked = samples.ops->seek_us(samples.opaque, 4500000u, &actual);
    CHECK(live ? !seeked && actual == 0u : seeked && actual == 4000000u);
    media_hls_source_destroy(source);
    CHECK(budget.current == 0u);
    return 0;
}

static int test_vod_duration_and_liveness(void)
{
    puts("test: VOD HLS reports its duration; live and EVENT do not");
    /* 4 + 4 + 4.5 s across a discontinuity. */
    static const char endlist[] =
        "#EXTM3U\n#EXT-X-TARGETDURATION:5\n#EXTINF:4.000,\none.ts\n"
        "#EXTINF:4.000,\ntwo.ts\n#EXT-X-DISCONTINUITY\n"
        "#EXTINF:4.500,\nthree.ts\n#EXT-X-ENDLIST\n";
    CHECK(check_source_duration(endlist, false, 12500000u,
                                12u * 90000u + 45000u) == 0);
    /* PLAYLIST-TYPE:VOD cannot change: complete without ENDLIST. */
    static const char vod_type[] =
        "#EXTM3U\n#EXT-X-TARGETDURATION:5\n#EXT-X-PLAYLIST-TYPE:VOD\n"
        "#EXTINF:4.000,\none.ts\n#EXTINF:4.000,\ntwo.ts\n"
        "#EXTINF:4.500,\nthree.ts\n";
    CHECK(check_source_duration(vod_type, false, 12500000u,
                                12u * 90000u + 45000u) == 0);
    /* A finished EVENT is VOD; a growing one is live. */
    static const char event_ended[] =
        "#EXTM3U\n#EXT-X-TARGETDURATION:5\n#EXT-X-PLAYLIST-TYPE:EVENT\n"
        "#EXTINF:4.000,\none.ts\n#EXTINF:4.000,\ntwo.ts\n"
        "#EXTINF:4.500,\nthree.ts\n#EXT-X-ENDLIST\n";
    CHECK(check_source_duration(event_ended, false, 12500000u,
                                12u * 90000u + 45000u) == 0);
    static const char event_open[] =
        "#EXTM3U\n#EXT-X-TARGETDURATION:5\n#EXT-X-PLAYLIST-TYPE:EVENT\n"
        "#EXTINF:4.000,\none.ts\n#EXTINF:4.000,\ntwo.ts\n"
        "#EXTINF:4.500,\nthree.ts\n";
    CHECK(check_source_duration(event_open, true, 12500000u, 0u) == 0);
    /* No ENDLIST and no type: a live window, however long. */
    static const char live[] =
        "#EXTM3U\n#EXT-X-TARGETDURATION:5\n#EXT-X-MEDIA-SEQUENCE:7\n"
        "#EXTINF:4.000,\none.ts\n#EXT-X-DISCONTINUITY\n"
        "#EXTINF:4.000,\ntwo.ts\n#EXTINF:4.500,\nthree.ts\n";
    CHECK(check_source_duration(live, true, 12500000u, 0u) == 0);
    /* The incremental parser treats the VOD type like ENDLIST too: an
       oversized one is refused rather than compacted into a live tail. */
    Budget budget;
    budget_init(&budget, 2u * 1024u * 1024u);
    char error[160] = {0};
    MediaHlsPlaylistStream *stream = media_hls_playlist_stream_create(
        &budget, "https://media.invalid/path/list.m3u8",
        error, sizeof(error));
    CHECK(stream != NULL);
    static const char head[] =
        "#EXTM3U\n#EXT-X-TARGETDURATION:5\n#EXT-X-PLAYLIST-TYPE:VOD\n";
    CHECK(media_hls_playlist_stream_feed(
        stream, (const unsigned char *) head, strlen(head),
        error, sizeof(error)));
    char line[160];
    for (unsigned i = 0;
         media_hls_playlist_stream_bytes_seen(stream)
             <= MEDIA_HLS_MAXIMUM_PLAYLIST_BYTES; i++) {
        int length = snprintf(line, sizeof(line),
                              "#EXTINF:4.000,\nsegment-%06u.ts\n", i);
        CHECK(media_hls_playlist_stream_feed(
            stream, (const unsigned char *) line, (size_t) length,
            error, sizeof(error)));
    }
    CHECK(media_hls_playlist_stream_finish(
              stream, error, sizeof(error)) == NULL
          && strstr(error, "VOD") != NULL
          && strncmp(error, MEDIA_HLS_PLAYLIST_TOO_LONG,
                     strlen(MEDIA_HLS_PLAYLIST_TOO_LONG)) == 0);
    printf("oversized VOD text: %s\n", error);
    media_hls_playlist_stream_destroy(stream);
    CHECK(budget.current == 0u);
    return 0;
}

/* 0.2 s of muxed TS: 5 pictures (40 ms) and 8 AAC frames (23.2 ms), both
   starting at the raw `pts`. */
#define SEEK_VIDEO_UNITS 5u
#define SEEK_AUDIO_UNITS 8u

static void build_av_run_segment(MockTransport *mock, size_t segment,
                                 uint64_t pts)
{
    build_video_run_segment(mock, segment, pts, SEEK_VIDEO_UNITS);
    static const unsigned char pmt[] = {
        0x02,0xb0,0x17,0,1,0xc1,0,0,0xe1,1,0xf0,0,
        0x1b,0xe1,1,0xf0,0,0x0f,0xe1,2,0xf0,0,0,0,0,0
    };
    psi_packet(mock->segment[segment] + 188u, 0x100u, pmt, sizeof(pmt));
    unsigned char *at = mock->segment[segment] + mock->segment_bytes[segment];
    static const unsigned char adts[] = {
        0xff,0xf1,0x50,0x80,0x00,0xff,0xfc
    };
    unsigned char payload[184];
    for (unsigned i = 0; i < SEEK_AUDIO_UNITS; i++) {
        size_t bytes = pes(payload, 0xc0u, pts + 2090u * i,
                           adts, sizeof(adts));
        ts_packet(at, 0x102u, true, payload, bytes);
        at += 188u;
    }
    mock->segment_bytes[segment] = (size_t) (at - mock->segment[segment]);
}

typedef struct {
    uint64_t first_video_us;
    uint64_t first_audio_us;
    uint64_t last_video_us;
    uint64_t last_audio_us;
    unsigned video_accepted;
    unsigned audio_accepted;
    bool out_of_order;
} SeekBackend;

static MediaBackendResult seek_submit(
    void *opaque, const MediaMp4Sample *sample,
    const unsigned char *bytes, size_t length,
    char *error, size_t error_size)
{
    (void) bytes; (void) error; (void) error_size;
    SeekBackend *backend = opaque;
    if (length != sample->size) return MEDIA_BACKEND_ERROR;
    uint64_t at = cycle_sample_us(sample);
    if (sample->kind == MEDIA_MP4_TRACK_VIDEO) {
        if (backend->video_accepted++ == 0) backend->first_video_us = at;
        else if (at <= backend->last_video_us) backend->out_of_order = true;
        backend->last_video_us = at;
    } else {
        if (backend->audio_accepted++ == 0) backend->first_audio_us = at;
        else if (at <= backend->last_audio_us) backend->out_of_order = true;
        backend->last_audio_us = at;
    }
    return MEDIA_BACKEND_ACCEPTED;
}

static bool seek_reset(void *opaque, char *error, size_t error_size)
{
    (void) error; (void) error_size;
    *(SeekBackend *) opaque = (SeekBackend) {0};
    return true;
}

typedef struct {
    MockTransport video_mock;
    MockTransport audio_mock;
    MediaHlsTransport video_ops;
    MediaHlsTransport audio_ops;
    MediaHlsSource *video;
    MediaHlsSource *audio;
    MediaPlayback *playback;
    SeekBackend fixture;
    MediaBackend backend;
    Budget budget;
} SeekRig;

static void seek_rig_pump(SeekRig *rig)
{
    media_hls_source_pump(rig->video, 0);
    media_hls_source_pump(rig->audio, 0);
}

static int seek_rig_open(SeekRig *rig)
{
    budget_init(&rig->budget, 3u * 1024u * 1024u);
    /* The video and audio renditions' EXTINF differ slightly, as on the
       recorded stream (6.000 against 6.014 s). The third segment follows
       a discontinuity whose raw timestamps jump to 10 s. */
    static const char video[] =
        "#EXTM3U\n#EXT-X-TARGETDURATION:1\n#EXT-X-PLAYLIST-TYPE:VOD\n"
        "#EXTINF:0.200,\none.ts\n#EXTINF:0.200,\ntwo.ts\n"
        "#EXT-X-DISCONTINUITY\n#EXTINF:0.200,\nthree.ts\n"
        "#EXT-X-ENDLIST\n";
    static const char audio[] =
        "#EXTM3U\n#EXT-X-TARGETDURATION:1\n#EXT-X-PLAYLIST-TYPE:VOD\n"
        "#EXTINF:0.209,\none.ts\n#EXTINF:0.209,\ntwo.ts\n"
        "#EXT-X-DISCONTINUITY\n#EXTINF:0.209,\nthree.ts\n"
        "#EXT-X-ENDLIST\n";
    char error[160] = {0};
    MediaHlsPlaylist *video_playlist = media_hls_playlist_parse(
        &rig->budget, "https://media.invalid/video.m3u8",
        (const unsigned char *) video, strlen(video), error, sizeof(error));
    MediaHlsPlaylist *audio_playlist = media_hls_playlist_parse(
        &rig->budget, "https://media.invalid/audio.m3u8",
        (const unsigned char *) audio, strlen(audio), error, sizeof(error));
    CHECK(video_playlist != NULL && audio_playlist != NULL);
    static const uint64_t raw[] = {90000u, 108000u, 900000u};
    for (size_t i = 0; i < 3u; i++) {
        build_av_run_segment(&rig->video_mock, i, raw[i]);
        build_av_run_segment(&rig->audio_mock, i, raw[i]);
    }
    rig->video_ops = (MediaHlsTransport) {
        .opaque = &rig->video_mock, .start = mock_start,
        .poll = mock_poll, .cancel = mock_cancel
    };
    rig->audio_ops = (MediaHlsTransport) {
        .opaque = &rig->audio_mock, .start = mock_start,
        .poll = mock_poll, .cancel = mock_cancel
    };
    rig->video = media_hls_source_create_track(
        &rig->budget, video_playlist, &rig->video_ops,
        MEDIA_HLS_TRACK_VIDEO, error, sizeof(error));
    rig->audio = media_hls_source_create_track(
        &rig->budget, audio_playlist, &rig->audio_ops,
        MEDIA_HLS_TRACK_AUDIO, error, sizeof(error));
    CHECK(rig->video != NULL && rig->audio != NULL
          && prime_source(rig->video) == 0
          && prime_source(rig->audio) == 0);
    MediaMp4TrackInfo video_info = {0}, audio_info = {0};
    CHECK(media_hls_source_stream_info(rig->video, &video_info, NULL)
          && media_hls_source_stream_info(rig->audio, NULL, &audio_info)
          && video_info.duration == 54000u
          && audio_info.duration == 56430u);
    MediaSampleSource video_samples = {0}, audio_samples = {0};
    CHECK(media_hls_source_sample_source(rig->video, &video_samples)
          && media_hls_source_sample_source(rig->audio, &audio_samples));
    rig->backend = (MediaBackend) {
        .opaque = &rig->fixture, .submit = seek_submit,
        .drain = playback_drain, .advance = playback_advance,
        .reset = seek_reset, .destroy = playback_destroy
    };
    MediaPlaybackOptions options = {
        .decode_lead_us = UINT64_C(10000000),
        .maximum_packet_bytes = 1024u * 1024u
    };
    rig->playback = media_playback_create_sources(
        &rig->budget, &video_samples, &audio_samples, &rig->backend,
        &options, error, sizeof(error));
    CHECK(rig->playback != NULL);
    return 0;
}

static int seek_rig_close(SeekRig *rig)
{
    media_playback_destroy(rig->playback);
    media_hls_source_destroy(rig->video);
    media_hls_source_destroy(rig->audio);
    CHECK(rig->budget.current == 0u);
    return 0;
}

/* The PSP player's seek transaction (psp_media_seek_decode_pump): prepare,
   reposition (exact for the firmware decoder, keyframe for swdec), prime
   both sources, then play to the end. */
static int seek_rig_seek(SeekRig *rig, uint64_t target_us, bool keyframe,
                         uint64_t *actual_us)
{
    char error[160] = {0};
    MediaPlaybackSourcePrimeStatus status =
        MEDIA_PLAYBACK_SOURCE_PRIME_PENDING;
    for (unsigned i = 0; i < 64u
         && status == MEDIA_PLAYBACK_SOURCE_PRIME_PENDING; i++) {
        status = media_playback_prepare_seek_sources(
            rig->playback, target_us, error, sizeof(error));
        if (status == MEDIA_PLAYBACK_SOURCE_PRIME_PENDING)
            seek_rig_pump(rig);
    }
    CHECK(status == MEDIA_PLAYBACK_SOURCE_PRIME_READY);
    CHECK(keyframe
          ? media_playback_seek_keyframe(rig->playback, target_us,
                                         actual_us, error, sizeof(error))
          : media_playback_seek(rig->playback, target_us, actual_us,
                                error, sizeof(error)));
    /* A keyframe seek primes and resumes at the keyframe. */
    if (keyframe && *actual_us < target_us) target_us = *actual_us;
    MediaPlaybackSourcePrimeStatus video =
        MEDIA_PLAYBACK_SOURCE_PRIME_PENDING;
    MediaPlaybackSourcePrimeStatus audio =
        MEDIA_PLAYBACK_SOURCE_PRIME_PENDING;
    uint64_t audio_us = 0;
    for (unsigned i = 0; i < 256u
         && (video == MEDIA_PLAYBACK_SOURCE_PRIME_PENDING
             || audio == MEDIA_PLAYBACK_SOURCE_PRIME_PENDING); i++) {
        if (video == MEDIA_PLAYBACK_SOURCE_PRIME_PENDING)
            video = media_playback_prime_video_source(
                rig->playback, target_us, NULL, error, sizeof(error));
        if (audio == MEDIA_PLAYBACK_SOURCE_PRIME_PENDING)
            audio = media_playback_prime_audio_source(
                rig->playback, target_us, &audio_us, error, sizeof(error));
        seek_rig_pump(rig);
    }
    if (video != MEDIA_PLAYBACK_SOURCE_PRIME_READY
        || audio != MEDIA_PLAYBACK_SOURCE_PRIME_READY)
        fprintf(stderr, "seek prime video=%d audio=%d error=%s\n",
                (int) video, (int) audio, error);
    CHECK(video == MEDIA_PLAYBACK_SOURCE_PRIME_READY
          && audio == MEDIA_PLAYBACK_SOURCE_PRIME_READY);
    (void) audio_us;
    media_playback_set_playing(rig->playback, true);
    for (unsigned visit = 0;
         visit < 512u && !media_playback_ended(rig->playback); visit++) {
        seek_rig_pump(rig);
        CHECK(media_playback_advance_bounded(
                  rig->playback, UINT64_C(10000000), 4u,
                  error, sizeof(error))
              != MEDIA_PLAYBACK_ADVANCE_ERROR);
    }
    CHECK(media_playback_ended(rig->playback) && !rig->fixture.out_of_order);
    return 0;
}

static int test_vod_seek_lands_on_segments_with_audio_rendition(void)
{
    puts("test: a VOD seek lands on a segment and resyncs its audio rendition");
    static SeekRig rig;
    memset(&rig, 0, sizeof(rig));
    CHECK(seek_rig_open(&rig) == 0);
    /* Firmware decoder: exact. Video decodes from the second segment's
       start (0.2 s), sound starts at the target: the audio rendition's
       second segment starts at 0.209 s, its fifth frame at 0.3019 s. */
    uint64_t actual_us = 0;
    CHECK(seek_rig_seek(&rig, 300000u, false, &actual_us) == 0);
    CHECK(actual_us == 200000u
          && rig.fixture.first_video_us == 200000u
          && rig.fixture.video_accepted == 2u * SEEK_VIDEO_UNITS
          && rig.fixture.first_audio_us >= 300000u
          && rig.fixture.first_audio_us < 300000u + 23220u
          && rig.fixture.audio_accepted == 2u * SEEK_AUDIO_UNITS - 4u);
    /* Across the discontinuity: playlist time, not the raw 10 s. */
    CHECK(seek_rig_seek(&rig, 450000u, false, &actual_us) == 0);
    CHECK(actual_us == 400000u
          && rig.fixture.first_video_us == 400000u
          && rig.fixture.last_video_us == 400000u + 4u * 40000u
          && rig.fixture.video_accepted == SEEK_VIDEO_UNITS
          && rig.fixture.first_audio_us >= 450000u
          && rig.fixture.first_audio_us < 450000u + 23220u
          && rig.fixture.last_audio_us < 700000u);
    /* Software decoder: keyframe seek, sound restarts with the picture at
       the segment start. The audio segment holding 0.2 s ends 9 ms later,
       so the rendition starts at its next one (0.209 s) rather than fetch
       and drop a whole segment. */
    CHECK(seek_rig_seek(&rig, 300000u, true, &actual_us) == 0);
    CHECK(actual_us == 200000u
          && rig.fixture.first_video_us == 200000u
          && rig.fixture.first_audio_us == 209000u
          && rig.fixture.audio_accepted == 2u * SEEK_AUDIO_UNITS);
    /* Backward to the start. */
    CHECK(seek_rig_seek(&rig, 0u, false, &actual_us) == 0);
    CHECK(actual_us == 0u && rig.fixture.first_video_us == 0u
          && rig.fixture.first_audio_us == 0u
          && rig.fixture.video_accepted == 3u * SEEK_VIDEO_UNITS
          && rig.fixture.audio_accepted == 3u * SEEK_AUDIO_UNITS);
    /* That follow slack is 50 ms, for the audio rendition only. */
    MediaSampleSource audio = {0}, video = {0};
    CHECK(media_hls_source_sample_source(rig.audio, &audio)
          && media_hls_source_sample_source(rig.video, &video));
    CHECK(audio.ops->seek_us(audio.opaque, 160000u, &actual_us)
          && actual_us == 209000u);
    CHECK(audio.ops->seek_us(audio.opaque, 150000u, &actual_us)
          && actual_us == 0u);
    CHECK(video.ops->seek_us(video.opaque, 190000u, &actual_us)
          && actual_us == 0u);
    return seek_rig_close(&rig);
}

typedef struct {
    char first_url[96];
    unsigned calls;
} RecordingTransport;

/* Records the first URL a source asks for and refuses every start, so
   prime stays pending without bytes. */
static uint64_t recording_start(void *opaque, const char *url,
                                size_t maximum, char *error,
                                size_t error_size)
{
    RecordingTransport *rig = opaque;
    (void) maximum; (void) error; (void) error_size;
    if (rig->calls++ == 0u)
        snprintf(rig->first_url, sizeof(rig->first_url), "%s", url);
    return 0;
}

static MediaHlsTransportPollResult recording_poll(
    void *opaque, uint64_t handle, unsigned char *destination,
    size_t capacity, size_t *length, char *error, size_t error_size)
{
    (void) opaque; (void) handle; (void) destination; (void) capacity;
    (void) error; (void) error_size;
    if (length != NULL) *length = 0;
    return MEDIA_HLS_TRANSPORT_ERROR;
}

static void recording_cancel(void *opaque, uint64_t handle)
{
    (void) opaque; (void) handle;
}

static MediaHlsPlaylist *live_rendition_playlist(
    Budget *budget, const char *name, unsigned first_sequence)
{
    char text[1024];
    int at = snprintf(text, sizeof(text),
                      "#EXTM3U\n#EXT-X-TARGETDURATION:6\n"
                      "#EXT-X-MEDIA-SEQUENCE:%u\n", first_sequence);
    for (unsigned i = 0; i < 10u && at > 0 && (size_t) at < sizeof(text); i++)
        at += snprintf(text + at, sizeof(text) - (size_t) at,
                       "#EXTINF:6.000,\n%s%u.ts\n", name, first_sequence + i);
    char error[160] = {0};
    return media_hls_playlist_parse(
        budget, "https://media.invalid/live/rendition.m3u8",
        (const unsigned char *) text, strlen(text), error, sizeof(error));
}

/* Renditions number their segments independently: equal sequence numbers
   need not hold the same content. Before both are primed there is no
   timestamp to compare, so each live source starts three segments from
   the end of its own playlist. */
static int test_live_renditions_start_at_their_own_edges(void)
{
    static const struct {
        unsigned video_first, audio_first;
        const char *video_url, *audio_url;
    } cases[] = {
        {100u, 101u, "https://media.invalid/live/v107.ts",
         "https://media.invalid/live/a108.ts"},
        {101u, 100u, "https://media.invalid/live/v108.ts",
         "https://media.invalid/live/a107.ts"},
        {100u, 100u, "https://media.invalid/live/v107.ts",
         "https://media.invalid/live/a107.ts"},
        {100u, 500u, "https://media.invalid/live/v107.ts",
         "https://media.invalid/live/a507.ts"},
    };
    for (size_t at = 0; at < sizeof(cases) / sizeof(cases[0]); at++) {
        Budget budget;
        budget_init(&budget, 4u * 1024u * 1024u);
        RecordingTransport rig[2] = {{{0}}};
        MediaHlsTransport video_transport = {
            .opaque = &rig[0], .start = recording_start,
            .poll = recording_poll, .cancel = recording_cancel
        };
        MediaHlsTransport audio_transport = {
            .opaque = &rig[1], .start = recording_start,
            .poll = recording_poll, .cancel = recording_cancel
        };
        char error[160] = {0};
        MediaHlsPlaylist *video_playlist = live_rendition_playlist(
            &budget, "v", cases[at].video_first);
        MediaHlsPlaylist *audio_playlist = live_rendition_playlist(
            &budget, "a", cases[at].audio_first);
        CHECK(video_playlist != NULL && audio_playlist != NULL);
        MediaHlsSource *video = media_hls_source_create_track(
            &budget, video_playlist, &video_transport,
            MEDIA_HLS_TRACK_VIDEO, error, sizeof(error));
        MediaHlsSource *audio = media_hls_source_create_track(
            &budget, audio_playlist, &audio_transport,
            MEDIA_HLS_TRACK_AUDIO, error, sizeof(error));
        CHECK(video != NULL && audio != NULL);
        /* Where 820ec87f called it, before either source has a timestamp:
           it must change nothing. */
        CHECK(!media_hls_source_align_live_start(video, audio));
        CHECK(media_hls_source_prime(video, error, sizeof(error))
                  == MEDIA_HLS_PRIME_PENDING
              && media_hls_source_prime(audio, error, sizeof(error))
                  == MEDIA_HLS_PRIME_PENDING);
        printf("live renditions %u/%u start at %s and %s\n",
               cases[at].video_first, cases[at].audio_first,
               rig[0].first_url, rig[1].first_url);
        CHECK(strcmp(rig[0].first_url, cases[at].video_url) == 0);
        CHECK(strcmp(rig[1].first_url, cases[at].audio_url) == 0);
        CHECK(!media_hls_source_align_live_start(video, audio));
        media_hls_source_destroy(audio);
        media_hls_source_destroy(video);
        CHECK(budget.current == 0);
    }
    return 0;
}

#define TIMED_FRAMES 40u
#define TIMED_TS_FRAME_TICKS 2090u
/* What the source counts for one 1024-sample frame at 44.1 kHz. */
#define TIMED_ADTS_FRAME_TICKS 2089u
#define TIMED_PTS_MASK ((UINT64_C(1) << 33) - 1u)

/* A live rendition served by timestamp. Segment `n` of a playlist whose
   first segment is `first_sequence` starts at raw PTS
   first_pts + (n - first_sequence) * segment_ms * 90 (33-bit). Audio
   segments carry TIMED_FRAMES 9-byte ADTS frames whose two payload bytes
   are the segment number's low byte and the frame's index, so a test can
   tell which content it was handed. */
typedef struct {
    bool active;
    size_t length;
    size_t offset;
    unsigned char bytes[188u * (TIMED_FRAMES + 4u)];
} TimedSlot;

typedef struct {
    char prefix;
    bool audio;
    bool packed;        /* ID3 then ADTS, instead of MPEG-TS */
    bool priv;          /* packed: the ID3 tag has the timestamp PRIV */
    unsigned first_sequence;
    unsigned segment_ms;
    unsigned count;
    uint64_t first_pts;
    /* Numbers from first_sequence + renumber serve the content numbered
       renumber lower: the playlist was renumbered. */
    unsigned renumber;
    /* Content from segment restart_at on (0: none) carries a clock that
       restarted at restart_pts: the packager restarted. */
    unsigned restart_at;
    uint64_t restart_pts;
    /* Video from segment slow_from on (0: none) has half the pictures,
       twice as far apart, after an EXT-X-DISCONTINUITY: a new encoder. */
    unsigned slow_from;
    unsigned requested[32];
    unsigned request_count;
    TimedSlot slot[2];
    /* Segment reads wait while *clock_us lies in [stall_from_us,
       stall_to_us): the network stalls (clock_us NULL: never). */
    const uint64_t *clock_us;
    uint64_t stall_from_us, stall_to_us;
} TimedRendition;

/* Where content segment `sequence` starts, on the clock it began on. */
static uint64_t timed_content(const TimedRendition *rendition,
                              unsigned sequence)
{
    return (rendition->first_pts
            + (uint64_t) (sequence - rendition->first_sequence)
                * rendition->segment_ms * 90u) & TIMED_PTS_MASK;
}

/* The PTS segment `sequence` carries. */
static uint64_t timed_pts(const TimedRendition *rendition, unsigned sequence)
{
    if (rendition->restart_at == 0 || sequence < rendition->restart_at)
        return timed_content(rendition, sequence);
    return (rendition->restart_pts
            + (uint64_t) (sequence - rendition->restart_at)
                * rendition->segment_ms * 90u) & TIMED_PTS_MASK;
}

static void timed_adts_frame(unsigned char frame[9], unsigned sequence,
                             unsigned index)
{
    static const unsigned char header[] = {
        0xff,0xf1,0x50,0x80,0x01,0x3f,0xfc
    };
    memcpy(frame, header, sizeof(header));
    frame[7] = (unsigned char) sequence;
    frame[8] = (unsigned char) index;
}

static size_t timed_audio_segment(const TimedRendition *rendition,
                                  unsigned sequence, unsigned char *out)
{
    uint64_t pts = timed_pts(rendition, sequence);
    unsigned char *at = out;
    unsigned char frame[9];
    if (rendition->packed) {
        static const char owner[] =
            "com.apple.streaming.transportStreamTimestamp";
        static const unsigned char tag[] = {'I','D','3',4,0,0,0,0,0,63};
        static const unsigned char priv[] = {'P','R','I','V',0,0,0,53,0,0};
        static const unsigned char other[] = {
            'I','D','3',4,0,0,0,0,0,4,'t','e','s','t'
        };
        if (rendition->priv) {
            memcpy(at, tag, sizeof(tag)); at += sizeof(tag);
            memcpy(at, priv, sizeof(priv)); at += sizeof(priv);
            memcpy(at, owner, sizeof(owner)); at += sizeof(owner);
            for (unsigned i = 0; i < 8u; i++)
                *at++ = (unsigned char) (pts >> (56u - 8u * i));
        } else {
            memcpy(at, other, sizeof(other)); at += sizeof(other);
        }
        for (unsigned i = 0; i < TIMED_FRAMES; i++) {
            timed_adts_frame(at, sequence, i);
            at += 9u;
        }
        return (size_t) (at - out);
    }
    static const unsigned char pat[] = {
        0x00,0xb0,0x0d,0,1,0xc1,0,0,0,1,0xe1,0,0,0,0,0
    };
    static const unsigned char pmt[] = {
        0x02,0xb0,0x12,0,1,0xc1,0,0,0xe1,2,0xf0,0,
        0x0f,0xe1,2,0xf0,0,0,0,0,0
    };
    psi_packet(at, 0, pat, sizeof(pat)); at += 188u;
    psi_packet(at, 0x100u, pmt, sizeof(pmt)); at += 188u;
    unsigned char payload[184];
    for (unsigned i = 0; i < TIMED_FRAMES; i++) {
        timed_adts_frame(frame, sequence, i);
        size_t bytes = pes(payload, 0xc0u,
                           pts + (uint64_t) i * TIMED_TS_FRAME_TICKS,
                           frame, sizeof(frame));
        ts_packet(at, 0x102u, true, payload, bytes);
        at += 188u;
    }
    return (size_t) (at - out);
}

static unsigned timed_video_frames(const TimedRendition *rendition,
                                   unsigned sequence)
{
    return rendition->slow_from != 0 && sequence >= rendition->slow_from
        ? TIMED_FRAMES / 2u : TIMED_FRAMES;
}

static uint64_t timed_video_tick(const TimedRendition *rendition,
                                 unsigned sequence)
{
    return (uint64_t) rendition->segment_ms * 90u
        / timed_video_frames(rendition, sequence);
}

/* An IDR then TIMED_FRAMES - 1 P pictures spread over the segment, one TS
   packet each. Each picture's slice data ends with its index and the low
   byte of the segment's number, so a test can tell what it was handed. */
static size_t timed_video_segment(const TimedRendition *rendition,
                                  unsigned sequence, unsigned char *out)
{
    static const unsigned char pat[] = {
        0x00,0xb0,0x0d,0,1,0xc1,0,0,0,1,0xe1,0,0,0,0,0
    };
    static const unsigned char pmt[] = {
        0x02,0xb0,0x12,0,1,0xc1,0,0,0xe1,1,0xf0,0,
        0x1b,0xe1,1,0xf0,0,0,0,0,0
    };
    uint64_t pts = timed_pts(rendition, sequence);
    unsigned char *at = out;
    psi_packet(at, 0, pat, sizeof(pat)); at += 188u;
    psi_packet(at, 0x100u, pmt, sizeof(pmt)); at += 188u;
    unsigned char au[128] = {0,0,1};
    size_t length = 3u + make_sps(au + 3u);
    const unsigned char idr[] = {
        0,0,1,0x68,0xce,0x06,0xe2, 0,0,1,0x65,0x80,0,
        (unsigned char) sequence
    };
    memcpy(au + length, idr, sizeof(idr));
    length += sizeof(idr);
    unsigned char payload[184];
    for (unsigned i = 0; i < timed_video_frames(rendition, sequence); i++) {
        const unsigned char picture[] = {
            0,0,1,0x41,0x80,(unsigned char) i,(unsigned char) sequence
        };
        size_t bytes = i == 0
            ? pes(payload, 0xe0u, pts, au, length)
            : pes(payload, 0xe0u,
                  (pts + i * timed_video_tick(rendition, sequence))
                      & TIMED_PTS_MASK,
                  picture, sizeof(picture));
        ts_packet(at, 0x101u, true, payload, bytes);
        at += 188u;
    }
    return (size_t) (at - out);
}

static uint64_t timed_start(void *opaque, const char *url, size_t maximum,
                            char *error, size_t error_size)
{
    TimedRendition *rendition = opaque;
    (void) maximum; (void) error; (void) error_size;
    const char *name = strrchr(url, '/');
    if (name == NULL || name[1] != rendition->prefix) return 0;
    unsigned sequence = (unsigned) strtoul(name + 2, NULL, 10);
    if (rendition->renumber != 0
        && sequence >= rendition->first_sequence + rendition->renumber)
        sequence -= rendition->renumber;
    for (size_t i = 0; i < 2u; i++) {
        if (rendition->slot[i].active) continue;
        rendition->slot[i].active = true;
        rendition->slot[i].offset = 0;
        rendition->slot[i].length = rendition->audio
            ? timed_audio_segment(rendition, sequence,
                                  rendition->slot[i].bytes)
            : timed_video_segment(rendition, sequence,
                                  rendition->slot[i].bytes);
        if (rendition->request_count < 32u)
            rendition->requested[rendition->request_count] = sequence;
        rendition->request_count++;
        return i + 1u;
    }
    return 0;
}

static MediaHlsTransportPollResult timed_poll(
    void *opaque, uint64_t handle, unsigned char *destination,
    size_t capacity, size_t *length, char *error, size_t error_size)
{
    TimedRendition *rendition = opaque;
    (void) error; (void) error_size;
    *length = 0;
    if (handle == 0 || handle > 2u || !rendition->slot[handle - 1u].active)
        return MEDIA_HLS_TRANSPORT_ERROR;
    TimedSlot *slot = &rendition->slot[handle - 1u];
    if (rendition->clock_us != NULL
        && *rendition->clock_us >= rendition->stall_from_us
        && *rendition->clock_us < rendition->stall_to_us)
        return MEDIA_HLS_TRANSPORT_WAIT;
    if (slot->offset == slot->length) {
        slot->active = false;
        return MEDIA_HLS_TRANSPORT_COMPLETE;
    }
    size_t bytes = slot->length - slot->offset;
    /* Small reads: a packed segment's ID3 tag arrives in two. */
    if (bytes > 50u) bytes = 50u;
    if (bytes > capacity) bytes = capacity;
    memcpy(destination, slot->bytes + slot->offset, bytes);
    slot->offset += bytes;
    *length = bytes;
    return MEDIA_HLS_TRANSPORT_CHUNK;
}

static void timed_cancel(void *opaque, uint64_t handle)
{
    TimedRendition *rendition = opaque;
    if (handle != 0 && handle <= 2u) rendition->slot[handle - 1u].active = false;
}

/* The live window of `count` segments from `first`. */
static MediaHlsPlaylist *timed_window_playlist(
    Budget *budget, const TimedRendition *rendition, unsigned first,
    unsigned count)
{
    static char text[4096];
    int at = snprintf(text, sizeof(text),
                      "#EXTM3U\n#EXT-X-TARGETDURATION:%u\n"
                      "#EXT-X-MEDIA-SEQUENCE:%u\n",
                      (rendition->segment_ms + 999u) / 1000u, first);
    for (unsigned i = 0; i < count && at > 0
         && (size_t) at < sizeof(text); i++) {
        if (rendition->slow_from != 0
            && first + i == rendition->slow_from + rendition->renumber)
            at += snprintf(text + at, sizeof(text) - (size_t) at,
                           "#EXT-X-DISCONTINUITY\n");
        at += snprintf(text + at, sizeof(text) - (size_t) at,
                       "#EXTINF:%u.%03u,\n%c%u.%s\n",
                       rendition->segment_ms / 1000u,
                       rendition->segment_ms % 1000u, rendition->prefix,
                       first + i, rendition->packed ? "aac" : "ts");
    }
    char error[160] = {0};
    return media_hls_playlist_parse(
        budget, "https://media.invalid/live/rendition.m3u8",
        (const unsigned char *) text, strlen(text), error, sizeof(error));
}

static MediaHlsPlaylist *timed_playlist(Budget *budget,
                                        const TimedRendition *rendition)
{
    return timed_window_playlist(budget, rendition,
                                 rendition->first_sequence, rendition->count);
}

/* The open as psp_media_hls drives it: prime the video, then the audio;
   align; when that moved or emptied the audio, prime it again. The align
   call before the first prime is where 820ec87f made it. Counts the
   aligns that asked for another prime. */
static int timed_open(MediaHlsSource *video, MediaHlsSource *audio,
                      unsigned *reprimes)
{
    char error[160] = {0};
    (void) media_hls_source_align_live_start(video, audio);
    size_t priming = 0;
    *reprimes = 0;
    for (unsigned guard = 0; guard < 1024u; guard++) {
        MediaHlsPrimeStatus status = media_hls_source_prime(
            priming == 0 ? video : audio, error, sizeof(error));
        CHECK(status != MEDIA_HLS_PRIME_FAILED);
        if (status == MEDIA_HLS_PRIME_PENDING) continue;
        if (++priming < 2u) continue;
        if (!media_hls_source_align_live_start(video, audio)) return 0;
        (*reprimes)++;
        CHECK(*reprimes <= 2u);
        priming = 1;
    }
    CHECK(!"the open finished");
    return 1;
}

static int64_t timed_signed33(uint64_t ticks)
{
    ticks &= TIMED_PTS_MASK;
    return ticks > (TIMED_PTS_MASK >> 1)
        ? (int64_t) ticks - (int64_t) (TIMED_PTS_MASK + 1u)
        : (int64_t) ticks;
}

/* External P2 finding 1: a demuxed live open lines the audio up with the
   video by the first timestamps both sources primed, whatever their
   segment numbers and durations, and keeps the MD-04 race (the audio
   playlist fetched one segment later) closed. */
static int test_live_renditions_align_by_timestamp(void)
{
    /* Content time 600 s is raw PTS 90,000 * 600 (plus `bias` for the
       audio, its first frame's offset from the picture). */
    const uint64_t base = UINT64_C(54000000);
    const uint64_t wrap_base =
        ((UINT64_C(1) << 33) + 1000u - 7u * 540000u) & TIMED_PTS_MASK;
    static const struct {
        const char *name;
        unsigned video_first, video_ms, video_count;
        unsigned audio_first, audio_ms, audio_count;
        int64_t audio_skew;   /* audio window's first content - video's */
        int64_t bias;         /* audio segment start - picture start */
        bool packed, priv, wrap, related;
        unsigned audio_start, audio_moved;  /* 0: no second fetch */
        int64_t first_audio;  /* expected dts of the first audio unit */
    } cases[] = {
        {"MD-04 race, same numbering", 100, 6000, 10, 101, 6000, 10,
         540000, 0, false, false, false, true, 108, 107, 0},
        {"audio numbered one higher", 100, 6000, 10, 101, 6000, 10,
         0, 0, false, false, false, true, 108, 0, 0},
        {"audio numbered 1000 higher, and the race", 100, 6000, 10,
         1101, 6000, 10, 540000, 0, false, false, false, true, 1108, 1107, 0},
        {"2 s audio against 6 s video", 100, 6000, 10, 100, 2000, 30,
         0, 0, false, false, false, true, 127, 121, 0},
        {"6 s audio against 2 s video", 100, 2000, 30, 100, 6000, 10,
         0, 0, false, false, false, true, 107, 109, 0},
        {"audio 50 ms before the picture", 100, 6000, 10, 100, 6000, 10,
         0, -4500, false, false, false, true, 107, 0, 1770},
        {"audio 30 ms after the picture", 100, 6000, 10, 100, 6000, 10,
         0, 2700, false, false, false, true, 107, 0, 2700},
        {"clocks an hour apart", 100, 6000, 10, 100, 6000, 10,
         0, 324000000, false, false, false, false, 107, 0, 0},
        {"PTS wrap between audio and picture", 100, 6000, 10, 100, 6000, 10,
         0, -4500, false, false, true, true, 107, 0, 1770},
        {"packed audio, ID3 timestamp, renumbered and late", 100, 6000, 10,
         1101, 6000, 10, 540000, -4500, true, true, false, true,
         1108, 1107, 3u * TIMED_ADTS_FRAME_TICKS - 4500},
        {"packed audio without an ID3 timestamp", 100, 6000, 10,
         100, 6000, 10, 0, -4500, true, false, false, false, 107, 0, 0},
        /* No timestamp: matched by place on the playlist timeline, the
           time from each playlist's end (both end at the live edge). */
        {"packed audio without an ID3 timestamp, 2 s against 6 s video",
         100, 6000, 10, 100, 2000, 30, 0, 0, true, false, false, true,
         127, 121, 0},
        {"packed audio without an ID3 timestamp, 6 s against 2 s video",
         100, 2000, 30, 100, 6000, 10, 0, 0, true, false, false, true,
         107, 109, 0},
    };

    static TimedRendition video_rendition, audio_rendition;
    for (size_t at = 0; at < sizeof(cases) / sizeof(cases[0]); at++) {
        Budget budget;
        budget_init(&budget, 4u * 1024u * 1024u);
        uint64_t origin = cases[at].wrap ? wrap_base : base;
        memset(&video_rendition, 0, sizeof(video_rendition));
        memset(&audio_rendition, 0, sizeof(audio_rendition));
        video_rendition = (TimedRendition) {
            .prefix = 'v', .first_sequence = cases[at].video_first,
            .segment_ms = cases[at].video_ms,
            .count = cases[at].video_count, .first_pts = origin
        };
        audio_rendition = (TimedRendition) {
            .prefix = 'a', .audio = true, .packed = cases[at].packed,
            .priv = cases[at].priv,
            .first_sequence = cases[at].audio_first,
            .segment_ms = cases[at].audio_ms,
            .count = cases[at].audio_count,
            .first_pts = (origin + (uint64_t) cases[at].audio_skew
                          + (uint64_t) cases[at].bias) & TIMED_PTS_MASK
        };
        MediaHlsTransport video_ops = {
            .opaque = &video_rendition, .start = timed_start,
            .poll = timed_poll, .cancel = timed_cancel
        };
        MediaHlsTransport audio_ops = video_ops;
        audio_ops.opaque = &audio_rendition;
        char error[160] = {0};
        MediaHlsPlaylist *video_playlist =
            timed_playlist(&budget, &video_rendition);
        MediaHlsPlaylist *audio_playlist =
            timed_playlist(&budget, &audio_rendition);
        CHECK(video_playlist != NULL && audio_playlist != NULL);
        MediaHlsSource *video = media_hls_source_create_track(
            &budget, video_playlist, &video_ops, MEDIA_HLS_TRACK_VIDEO,
            error, sizeof(error));
        MediaHlsSource *audio = media_hls_source_create_track(
            &budget, audio_playlist, &audio_ops, MEDIA_HLS_TRACK_AUDIO,
            error, sizeof(error));
        CHECK(video != NULL && audio != NULL);
        unsigned reprimes = 0;
        CHECK(timed_open(video, audio, &reprimes) == 0);
        MediaSampleSource video_samples = {0}, audio_samples = {0};
        CHECK(media_hls_source_sample_source(video, &video_samples)
              && media_hls_source_sample_source(audio, &audio_samples));
        /* The player's construction probe still lands: a moved or trimmed
           audio source has given nothing out yet. */
        uint64_t landing = 1;
        CHECK(audio_samples.ops->seek_us(audio_samples.opaque, 0, &landing)
              && landing == 0
              && video_samples.ops->seek_us(video_samples.opaque, 0,
                                            &landing));
        MediaMp4Sample video_sample = {0}, audio_sample = {0};
        unsigned char payload[512];
        CHECK(video_samples.ops->next_sample(video_samples.opaque,
                                             &video_sample)
              && video_sample.keyframe
              && video_samples.ops->read_sample(
                     video_samples.opaque, &video_sample,
                     payload, sizeof(payload)));
        CHECK(audio_samples.ops->next_sample(audio_samples.opaque,
                                             &audio_sample)
              && audio_sample.size == 9u
              && audio_samples.ops->read_sample(
                     audio_samples.opaque, &audio_sample,
                     payload, sizeof(payload)));
        /* Which content the first audio unit holds, as a raw PTS. */
        unsigned sequence = audio_rendition.first_sequence
            + ((payload[7] - audio_rendition.first_sequence) & 0xffu);
        uint64_t audio_raw = timed_pts(&audio_rendition, sequence)
            + (uint64_t) payload[8] * (cases[at].packed
                  ? TIMED_ADTS_FRAME_TICKS : TIMED_TS_FRAME_TICKS);
        uint64_t video_raw =
            timed_pts(&video_rendition, video_rendition.requested[0]);
        /* Content apart minus clock apart: what a viewer hears off the
           picture, in 90 kHz ticks. */
        int64_t error_ticks = timed_signed33(audio_raw - video_raw)
            - (int64_t) (audio_sample.dts - video_sample.dts);
        char moved[16] = "";
        if (audio_rendition.request_count > 1u)
            snprintf(moved, sizeof(moved), " then a%u",
                     audio_rendition.requested[1]);
        printf("live renditions, %s: video v%u, audio a%u%s "
               "(%u fetches, %u reprimes); first audio unit %u:%u at "
               "%lld ticks, %lld ticks off the picture\n",
               cases[at].name, video_rendition.requested[0],
               audio_rendition.requested[0], moved,
               audio_rendition.request_count, reprimes,
               sequence, (unsigned) payload[8],
               (long long) audio_sample.dts, (long long) error_ticks);
        CHECK(video_sample.dts == 0);
        CHECK(audio_rendition.requested[0] == cases[at].audio_start);
        if (cases[at].audio_moved != 0) {
            CHECK(audio_rendition.request_count >= 2u
                  && audio_rendition.requested[1] == cases[at].audio_moved);
        } else {
            CHECK(audio_rendition.request_count == 1u);
        }
        CHECK(audio_sample.dts == (uint64_t) cases[at].first_audio);
        /* Same content, same clock time; but clocks an hour apart keep
           main's start at zero, and so does audio with no timestamp
           whose segments are as long as the video's (nothing tells the
           50 ms apart). */
        if (cases[at].related) CHECK(error_ticks == 0);
        CHECK(!media_hls_source_align_live_start(video, audio));
        media_hls_source_destroy(audio);
        media_hls_source_destroy(video);
        CHECK(budget.current == 0);
    }
    return 0;
}

/* What content a taken unit holds, as a raw PTS: the segment from its
   payload tag (the low byte of its number), then its place in it. */
static uint64_t timed_unit_raw(const TimedRendition *rendition,
                               const MediaMp4Sample *sample,
                               const unsigned char *payload)
{
    unsigned tag = rendition->audio ? payload[7] : payload[sample->size - 1u];
    unsigned sequence = rendition->first_sequence
        + ((tag - rendition->first_sequence) & 0xffu);
    uint64_t raw = timed_content(rendition, sequence);
    if (rendition->audio)
        return raw + (uint64_t) payload[8] * (rendition->packed
            ? TIMED_ADTS_FRAME_TICKS : TIMED_TS_FRAME_TICKS);
    return raw
        + payload[sample->size - 2u] * timed_video_tick(rendition, sequence);
}

typedef struct {
    unsigned taken;
    unsigned last_sequence;
    uint64_t last_dts;
    uint32_t last_duration;
    /* Units given out at or before the previous one's time. The audio
       output is the clock and counts what it played, so sound that runs
       back over itself puts the picture behind for good. */
    unsigned backwards;
    /* Content (ticks after the video's first segment) minus clock time.
       Equal for both tracks when sound and picture are in step. */
    int64_t offset;
} TimedTrack;

/* Takes units as the player does: always the earlier of the two heads,
   and only while both have one. */
static int timed_take(MediaSampleSource samples[2],
                      TimedRendition *rendition[2], TimedTrack track[2],
                      unsigned *taken)
{
    *taken = 0;
    for (unsigned take = 0; take < 64u; take++) {
        MediaMp4Sample head[2] = {{0}};
        if (!samples[0].ops->next_sample(samples[0].opaque, &head[0])
            || !samples[1].ops->next_sample(samples[1].opaque, &head[1]))
            break;
        size_t pick = head[1].dts < head[0].dts ? 1u : 0u;
        unsigned char payload[512];
        CHECK(head[pick].size <= sizeof(payload)
              && samples[pick].ops->read_sample(
                     samples[pick].opaque, &head[pick], payload,
                     sizeof(payload)));
        uint64_t raw = timed_unit_raw(rendition[pick], &head[pick], payload);
        if (track[pick].taken != 0 && head[pick].dts <= track[pick].last_dts)
            track[pick].backwards++;
        track[pick].last_dts = head[pick].dts;
        track[pick].last_duration = head[pick].duration;
        track[pick].taken++;
        track[pick].last_sequence = rendition[pick]->audio
            ? payload[7] : payload[head[pick].size - 1u];
        track[pick].offset = timed_signed33(raw - rendition[0]->first_pts)
            - (int64_t) head[pick].dts;
        (*taken)++;
    }
    return 0;
}

/* Plays both sources as the player does (always the earlier of the two
   heads, and only while both have one) and hands each rendition
   `refreshes` live refreshes as it asks for them, the windows of ten
   segments from window_first[i], then one later each time. With more than
   one the play runs for a simulated two and a half minutes (20 ms steps),
   past the 60 s stall limit, whatever is taken. */
static int timed_play(MediaHlsSource *source[2], TimedRendition *rendition[2],
                      const unsigned window_first[2], unsigned refreshes,
                      Budget *budget, TimedTrack track[2])
{
    MediaSampleSource samples[2] = {{0}};
    for (size_t i = 0; i < 2u; i++)
        CHECK(media_hls_source_sample_source(source[i], &samples[i]));
    unsigned served[2] = {0, 0};
    unsigned idle = 0;
    const unsigned steps = refreshes > 1u ? 7500u : 4000u;
    const unsigned idle_limit = refreshes > 1u ? steps : 50u;
    uint64_t now = 1;
    for (unsigned step = 0; step < steps && idle < idle_limit; step++) {
        now += 20000u;
        for (size_t i = 0; i < 2u; i++) {
            media_hls_source_pump(source[i], now);
            if (served[i] >= refreshes
                || !media_hls_source_wants_playlist_refresh(source[i], now))
                continue;
            char error[160] = {0};
            MediaHlsPlaylist *window = timed_window_playlist(
                budget, rendition[i], window_first[i] + served[i], 10u);
            CHECK(window != NULL);
            CHECK(media_hls_source_update_playlist(
                source[i], window, now, error, sizeof(error)));
            served[i]++;
        }
        idle++;
        unsigned taken = 0;
        CHECK(timed_take(samples, rendition, track, &taken) == 0);
        if (taken != 0) idle = 0;
        if (media_hls_source_failed(source[0])
            || media_hls_source_failed(source[1]))
            printf("source failed at simulated %llu ms\n",
                   (unsigned long long) (now / 1000u));
        CHECK(!media_hls_source_failed(source[0])
              && !media_hls_source_failed(source[1]));
    }
    CHECK(served[0] != 0 && served[1] != 0);
    return 0;
}

static bool timed_requested(const TimedRendition *rendition,
                            unsigned first, unsigned last)
{
    for (unsigned i = 0; i < rendition->request_count && i < 32u; i++)
        if (rendition->requested[i] >= first
            && rendition->requested[i] <= last) return true;
    return false;
}

/* Remaining-work item (live HLS): a live source that falls behind its
   window skips to the window's first segment. Each rendition used to
   continue its own clock past the skip, so when only one of them skipped,
   or they skipped by different amounts, sound and picture drifted apart by
   the difference for the rest of the session. */
static int test_live_window_skips_keep_renditions_together(void)
{
    /* Where content segment 115 starts on the video's original clock. */
    const uint64_t at115 = UINT64_C(54000000) + 15u * UINT64_C(540000);
    static const struct {
        const char *name;
        bool packed, priv;
        unsigned video_window, audio_window;
        unsigned video_renumber;
        /* The rendition that did not skip must not fetch these: both
           tracks drop that content, whole segments unfetched. */
        unsigned unfetched_first, unfetched_last;
        /* The video's clock restarts at content segment 115 (0: never). */
        unsigned restart_at;
        uint64_t restart_pts;
        /* Sound minus picture: 0, or for a skip nothing measured, the
           skipped content (the offset from before skips were shared). */
        int64_t apart;
    } cases[] = {
        {"both skip to 115", false, false, 115, 115, 0, 0, 0, 0, 0, 0},
        {"only the video skips", false, false, 115, 108, 0, 111, 114,
         0, 0, 0},
        {"only the audio skips", false, false, 108, 115, 0, 111, 114,
         0, 0, 0},
        {"they skip by different amounts", false, false, 115, 112, 0, 0, 0,
         0, 0, 0},
        /* The timestamps, not the count of skipped numbers, measure it. */
        {"only the video skips, renumbered 1000 higher", false, false,
         115, 108, 1000, 111, 114, 0, 0, 0},
        /* No usable timestamp: the numbers and EXTINF do. */
        {"only the video skips, its PTS clock restarts", false, false,
         115, 108, 0, 111, 114, 115, 900000u, 0},
        /* Neither measures it: the estimate (about 6000 s) is the numbers
           jumping, not content, so the audio keeps playing what it has,
           off by the skipped 42 s. Sharing it dropped all the audio, and
           the source failed 60 s later. */
        {"only the video skips, renumbered 1000 higher, PTS clock restarts",
         false, false, 115, 108, 1000, 0, 0, 115, 900000u, -3780000},
        /* A restarted clock that lands 1000 s on agrees with no count the
           playlists can account for. */
        {"only the video skips, renumbered 1000 higher, PTS 1000 s on",
         false, false, 115, 108, 1000, 0, 0, 115,
         at115 + UINT64_C(90000000), -3780000},
        {"packed audio with PRIV, only the audio skips", true, true,
         108, 115, 0, 111, 114, 0, 0, 0},
        {"packed audio without PRIV, both skip", true, false,
         115, 115, 0, 0, 0, 0, 0, 0},
        {"packed audio without PRIV, only the video skips", true, false,
         115, 108, 0, 111, 114, 0, 0, 0},
        {"packed audio without PRIV, only the audio skips", true, false,
         108, 115, 0, 111, 114, 0, 0, 0},
    };
    static TimedRendition video_rendition, audio_rendition;
    for (size_t at = 0; at < sizeof(cases) / sizeof(cases[0]); at++) {
        Budget budget;
        budget_init(&budget, 4u * 1024u * 1024u);
        memset(&video_rendition, 0, sizeof(video_rendition));
        memset(&audio_rendition, 0, sizeof(audio_rendition));
        video_rendition = (TimedRendition) {
            .prefix = 'v', .first_sequence = 100u, .segment_ms = 6000u,
            .count = 10u, .first_pts = UINT64_C(54000000),
            .renumber = cases[at].video_renumber,
            .restart_at = cases[at].restart_at,
            .restart_pts = cases[at].restart_pts
        };
        audio_rendition = (TimedRendition) {
            .prefix = 'a', .audio = true, .packed = cases[at].packed,
            .priv = cases[at].priv, .first_sequence = 100u,
            .segment_ms = 6000u, .count = 10u,
            .first_pts = UINT64_C(54000000)
        };
        MediaHlsTransport video_ops = {
            .opaque = &video_rendition, .start = timed_start,
            .poll = timed_poll, .cancel = timed_cancel
        };
        MediaHlsTransport audio_ops = video_ops;
        audio_ops.opaque = &audio_rendition;
        char error[160] = {0};
        MediaHlsPlaylist *video_playlist =
            timed_playlist(&budget, &video_rendition);
        MediaHlsPlaylist *audio_playlist =
            timed_playlist(&budget, &audio_rendition);
        CHECK(video_playlist != NULL && audio_playlist != NULL);
        MediaHlsSource *source[2] = {
            media_hls_source_create_track(
                &budget, video_playlist, &video_ops, MEDIA_HLS_TRACK_VIDEO,
                error, sizeof(error)),
            media_hls_source_create_track(
                &budget, audio_playlist, &audio_ops, MEDIA_HLS_TRACK_AUDIO,
                error, sizeof(error))
        };
        CHECK(source[0] != NULL && source[1] != NULL);
        unsigned reprimes = 0;
        CHECK(timed_open(source[0], source[1], &reprimes) == 0);
        TimedRendition *rendition[2] = {&video_rendition, &audio_rendition};
        const unsigned window[2] = {
            cases[at].video_window + cases[at].video_renumber,
            cases[at].audio_window
        };
        TimedTrack track[2] = {{0}};
        CHECK(timed_play(source, rendition, window, 1u, &budget, track)
              == 0);
        MediaHlsStats stats[2];
        media_hls_source_stats(source[0], &stats[0]);
        media_hls_source_stats(source[1], &stats[1]);
        int64_t apart = track[1].offset - track[0].offset;
        printf("live window skip, %s: skipped v%zu a%zu; last v%u a%u "
               "(%u and %u units, %u sound units back), sound %lld ticks "
               "off the picture\n",
               cases[at].name, stats[0].skipped_live_segments,
               stats[1].skipped_live_segments, track[0].last_sequence,
               track[1].last_sequence, track[0].taken, track[1].taken,
               track[1].backwards, (long long) apart);
        /* Both played past the skip, and in step. */
        CHECK(track[0].last_sequence >= 115u
              && track[1].last_sequence >= 115u);
        CHECK(apart == cases[at].apart && track[1].backwards == 0);
        if (cases[at].unfetched_first != 0) {
            const TimedRendition *follower =
                cases[at].video_window < cases[at].audio_window
                    ? &video_rendition : &audio_rendition;
            CHECK(!timed_requested(follower, cases[at].unfetched_first,
                                   cases[at].unfetched_last));
        }
        media_hls_source_destroy(source[1]);
        media_hls_source_destroy(source[0]);
        CHECK(budget.current == 0);
    }
    return 0;
}

/* Review finding MS-1: the rendition that did not skip passes the skipped
   segments unfetched, and when its playlist lags it passes them one
   refresh at a time, at the playlist's end each time. Those refreshes add
   a segment but do not move it past where it stood, so the stall limit
   (60 s at the playlist's end with no progress) ran across the whole
   pass. Here the audio's playlist is at the live edge and skips 26
   segments (156 s, measured by its timestamps); the video's playlist lags
   and adds one segment per refresh (every 3 s), so the video passes for
   about 75 s before it reaches content to fetch.
   Review finding MS-3: every pass there ends at the playlist's end, where
   no next entry can be flagged as a discontinuity, so the first segment
   fetched after the pass was parsed as if it followed the last one
   fetched before it. A new encoder starts at 120, inside the pass, with
   half the frame rate; the picture interval learnt before the pass (the
   smallest gap seen since the last discontinuity) stayed in force. */
static int test_live_partner_passes_a_long_skip(void)
{
    Budget budget;
    budget_init(&budget, 4u * 1024u * 1024u);
    static TimedRendition video_rendition, audio_rendition;
    memset(&video_rendition, 0, sizeof(video_rendition));
    memset(&audio_rendition, 0, sizeof(audio_rendition));
    video_rendition = (TimedRendition) {
        .prefix = 'v', .first_sequence = 100u, .segment_ms = 6000u,
        .count = 10u, .first_pts = UINT64_C(54000000), .slow_from = 120u
    };
    audio_rendition = (TimedRendition) {
        .prefix = 'a', .audio = true, .first_sequence = 100u,
        .segment_ms = 6000u, .count = 10u, .first_pts = UINT64_C(54000000)
    };
    MediaHlsTransport video_ops = {
        .opaque = &video_rendition, .start = timed_start,
        .poll = timed_poll, .cancel = timed_cancel
    };
    MediaHlsTransport audio_ops = video_ops;
    audio_ops.opaque = &audio_rendition;
    char error[160] = {0};
    MediaHlsPlaylist *video_playlist = timed_playlist(&budget, &video_rendition);
    MediaHlsPlaylist *audio_playlist = timed_playlist(&budget, &audio_rendition);
    CHECK(video_playlist != NULL && audio_playlist != NULL);
    MediaHlsSource *source[2] = {
        media_hls_source_create_track(
            &budget, video_playlist, &video_ops, MEDIA_HLS_TRACK_VIDEO,
            error, sizeof(error)),
        media_hls_source_create_track(
            &budget, audio_playlist, &audio_ops, MEDIA_HLS_TRACK_AUDIO,
            error, sizeof(error))
    };
    CHECK(source[0] != NULL && source[1] != NULL);
    unsigned reprimes = 0;
    CHECK(timed_open(source[0], source[1], &reprimes) == 0);
    TimedRendition *rendition[2] = {&video_rendition, &audio_rendition};
    const unsigned window[2] = {101u, 135u};
    TimedTrack track[2] = {{0}};
    int played = timed_play(source, rendition, window, 40u, &budget, track);
    MediaHlsStats stats[2];
    media_hls_source_stats(source[0], &stats[0]);
    media_hls_source_stats(source[1], &stats[1]);
    int64_t apart = track[1].offset - track[0].offset;
    printf("live long pass: skipped v%zu a%zu; last v%u a%u (%u and %u "
           "units), sound %lld ticks off the picture; last picture lasts "
           "%u ticks\n",
           stats[0].skipped_live_segments, stats[1].skipped_live_segments,
           track[0].last_sequence, track[1].last_sequence, track[0].taken,
           track[1].taken, (long long) apart, track[0].last_duration);
    CHECK(played == 0);
    CHECK(track[0].last_duration == 6000u * 90u / (TIMED_FRAMES / 2u));
    CHECK(track[0].last_sequence >= 135u && track[1].last_sequence >= 135u);
    CHECK(apart == 0 && track[1].backwards == 0);
    CHECK(!timed_requested(&video_rendition, 111u, 134u));
    media_hls_source_destroy(source[1]);
    media_hls_source_destroy(source[0]);
    CHECK(budget.current == 0);
    return 0;
}

/* A live packager on a wall clock: content segment k of a rendition
   (number first_sequence + k) is published once the clock passes its end,
   and each playlist lists the newest `window` of them, so every refresh
   after a segment boundary has slid. */
static MediaHlsPlaylist *live_clock_playlist(
    Budget *budget, const TimedRendition *rendition, unsigned window,
    uint64_t now_us)
{
    uint64_t published = now_us / ((uint64_t) rendition->segment_ms * 1000u);
    unsigned count = published < window ? (unsigned) published : window;
    return timed_window_playlist(
        budget, rendition,
        rendition->first_sequence + (unsigned) (published - count), count);
}

typedef struct {
    Budget *budget;
    uint64_t clock_us;
    TimedRendition *rendition[2];
    unsigned window[2];
    MediaHlsSource *source[2];
    TimedTrack track[2];
} LiveClockRig;

/* The open as psp_media_hls does it: both playlists fetched at the
   rig's clock, video then audio, then primed and aligned. */
static int live_clock_open(LiveClockRig *rig, TimedRendition *audio)
{
    rig->rendition[1] = audio;
    rig->window[1] = 36000u / audio->segment_ms;
    char error[160] = {0};
    MediaHlsTransport ops[2];
    for (size_t i = 0; i < 2u; i++) {
        rig->rendition[i]->clock_us = &rig->clock_us;
        rig->rendition[i]->request_count = 0;
        ops[i] = (MediaHlsTransport) {
            .opaque = rig->rendition[i], .start = timed_start,
            .poll = timed_poll, .cancel = timed_cancel
        };
        MediaHlsPlaylist *playlist = live_clock_playlist(
            rig->budget, rig->rendition[i], rig->window[i], rig->clock_us);
        CHECK(playlist != NULL);
        rig->source[i] = media_hls_source_create_track(
            rig->budget, playlist, &ops[i],
            i == 0 ? MEDIA_HLS_TRACK_VIDEO : MEDIA_HLS_TRACK_AUDIO,
            error, sizeof(error));
        CHECK(rig->source[i] != NULL);
    }
    unsigned reprimes = 0;
    CHECK(timed_open(rig->source[0], rig->source[1], &reprimes) == 0);
    /* The player's construction probe lands on both. */
    for (size_t i = 0; i < 2u; i++) {
        MediaSampleSource samples = {0};
        uint64_t landing = 1;
        CHECK(media_hls_source_sample_source(rig->source[i], &samples)
              && samples.ops->seek_us(samples.opaque, 0, &landing)
              && landing == 0);
    }
    memset(rig->track, 0, sizeof(rig->track));
    return 0;
}

/* Plays until the clock reaches until_us in 20 ms steps, each rendition
   refreshed whenever it asks. */
static int live_clock_play(LiveClockRig *rig, uint64_t until_us)
{
    MediaSampleSource samples[2] = {{0}};
    for (size_t i = 0; i < 2u; i++)
        CHECK(media_hls_source_sample_source(rig->source[i], &samples[i]));
    while (rig->clock_us < until_us) {
        rig->clock_us += 20000u;
        for (size_t i = 0; i < 2u; i++) {
            media_hls_source_pump(rig->source[i], rig->clock_us);
            if (!media_hls_source_wants_playlist_refresh(rig->source[i],
                                                         rig->clock_us))
                continue;
            char error[160] = {0};
            MediaHlsPlaylist *window = live_clock_playlist(
                rig->budget, rig->rendition[i], rig->window[i],
                rig->clock_us);
            CHECK(window != NULL);
            CHECK(media_hls_source_update_playlist(
                rig->source[i], window, rig->clock_us, error, sizeof(error)));
        }
        unsigned taken = 0;
        CHECK(timed_take(samples, rig->rendition, rig->track, &taken) == 0);
        for (size_t i = 0; i < 2u; i++) {
            if (!media_hls_source_failed(rig->source[i])) continue;
            printf("live clock: source %zu failed at %llu ms\n", i,
                   (unsigned long long) (rig->clock_us / 1000u));
            CHECK(!media_hls_source_failed(rig->source[i]));
        }
    }
    return 0;
}

/* The number of the segment a track's last unit came from. */
static unsigned live_clock_last(const LiveClockRig *rig, size_t i)
{
    unsigned first = rig->rendition[i]->first_sequence;
    return first + ((rig->track[i].last_sequence - first) & 0xffu);
}

/* Prints where the pair stands and returns sound minus picture, as
   test_live_window_skips_keep_renditions_together measures it. */
static int64_t live_clock_report(const LiveClockRig *rig, const char *phase)
{
    MediaHlsStats stats[2];
    media_hls_source_stats(rig->source[0], &stats[0]);
    media_hls_source_stats(rig->source[1], &stats[1]);
    int64_t apart = rig->track[1].offset - rig->track[0].offset;
    printf("live demuxed pair, %s at %llu ms: skipped v%zu a%zu, refreshes "
           "v%zu a%zu; last v%u a%u (%u and %u units, %u and %u back), last "
           "picture %u ticks; sound %lld ticks off the picture\n",
           phase, (unsigned long long) (rig->clock_us / 1000u),
           stats[0].skipped_live_segments, stats[1].skipped_live_segments,
           stats[0].playlist_refreshes, stats[1].playlist_refreshes,
           live_clock_last(rig, 0), live_clock_last(rig, 1),
           rig->track[0].taken, rig->track[1].taken,
           rig->track[0].backwards, rig->track[1].backwards,
           rig->track[0].last_duration, (long long) apart);
    return apart;
}

static size_t live_clock_skipped(const LiveClockRig *rig, size_t i)
{
    MediaHlsStats stats;
    media_hls_source_stats(rig->source[i], &stats);
    return stats.skipped_live_segments;
}

static void live_clock_close(LiveClockRig *rig)
{
    media_hls_source_destroy(rig->source[1]);
    media_hls_source_destroy(rig->source[0]);
    rig->source[0] = rig->source[1] = NULL;
}

/* Polish T9: a demuxed live pair whose playlists slide on every refresh,
   played as a session would: open, a network stall that drops both
   renditions out of their windows by different amounts, a refused seek,
   and an audio track change (a reopen of both at the then-current
   windows). The video gets a new encoder at 120 (EXT-X-DISCONTINUITY,
   a restarted clock, half the frame rate); the first audio rendition is
   packed audio whose ID3 tag has no PRIV timestamp, numbered on its own
   and in 2 s segments, so only playlist position places it, and only the
   numbers and EXTINF measure its skips. The second is TS audio in 4 s
   segments with timestamps on the video's clocks, numbered 1000 higher,
   which its timestamps move back a segment at the reopen. Sound and
   picture must name the same content throughout. */
static int test_live_demuxed_pair_through_open_skip_track_change_and_seek(void)
{
    Budget budget;
    budget_init(&budget, 4u * 1024u * 1024u);
    const uint64_t origin = UINT64_C(54000000);
    const uint64_t restart = UINT64_C(900000);
    static TimedRendition video, packed, timed;
    video = (TimedRendition) {
        .prefix = 'v', .first_sequence = 100u, .segment_ms = 6000u,
        .first_pts = origin, .slow_from = 120u, .restart_at = 120u,
        .restart_pts = restart
    };
    packed = (TimedRendition) {
        .prefix = 'a', .audio = true, .packed = true, .priv = false,
        .first_sequence = 500u, .segment_ms = 2000u, .first_pts = origin
    };
    timed = (TimedRendition) {
        .prefix = 'a', .audio = true, .first_sequence = 1100u,
        .segment_ms = 4000u, .first_pts = origin, .restart_at = 1130u,
        .restart_pts = restart
    };
    static LiveClockRig rig;
    memset(&rig, 0, sizeof(rig));
    rig.budget = &budget;
    rig.rendition[0] = &video;
    rig.window[0] = 6u;

    /* Open 66.1 s in: both windows end at content 66 s; the video starts
       at 108 (48 s), three from its end, and the audio, started at 530,
       moves back to 524 (48 s) by its distance from the end. */
    rig.clock_us = UINT64_C(66100000);
    CHECK(live_clock_open(&rig, &packed) == 0);
    printf("live demuxed pair, open: video v%u, audio a%u then a%u\n",
           video.requested[0], packed.requested[0],
           packed.request_count > 1u ? packed.requested[1] : 0u);
    CHECK(video.requested[0] == 108u && packed.requested[0] == 530u
          && packed.request_count > 1u && packed.requested[1] == 524u);
    CHECK(live_clock_play(&rig, UINT64_C(100000000)) == 0);
    CHECK(rig.track[0].taken != 0 && rig.track[1].taken != 0);
    CHECK(live_clock_report(&rig, "playing") == 0);
    CHECK(live_clock_skipped(&rig, 0) == 0 && live_clock_skipped(&rig, 1) == 0);

    /* The video's network stalls for a minute: its window and, behind the
       held picture, the audio's slide past both, and each skips when its
       next refresh lands. The video comes back past the new encoder. */
    video.stall_from_us = UINT64_C(100000000);
    video.stall_to_us = UINT64_C(160000000);
    CHECK(live_clock_play(&rig, UINT64_C(220000000)) == 0);
    int64_t apart = live_clock_report(&rig, "after the video stall");
    CHECK(live_clock_skipped(&rig, 0) != 0 && live_clock_skipped(&rig, 1) != 0);
    /* The video's skip landed on the new encoder: its restarted clock
       cannot measure the skip, so the numbers and EXTINF do. */
    CHECK(!timed_requested(&video, 117u, 119u)
          && timed_requested(&video, 120u, 120u));
    CHECK(live_clock_last(&rig, 0) >= 130u && live_clock_last(&rig, 1) >= 600u);
    CHECK(rig.track[0].last_duration == 6000u * 90u / (TIMED_FRAMES / 2u));
    CHECK(apart == 0
          && rig.track[0].backwards == 0 && rig.track[1].backwards == 0);

    /* A live window has no timeline: a seek is refused and changes
       nothing, and the probe's zero no longer lands once units are
       taken. */
    for (size_t i = 0; i < 2u; i++) {
        MediaSampleSource samples = {0};
        uint64_t landing = 1;
        CHECK(media_hls_source_sample_source(rig.source[i], &samples));
        CHECK(!samples.ops->seek_us(samples.opaque, UINT64_C(30000000),
                                    &landing));
        CHECK(!samples.ops->seek_after_us(samples.opaque, UINT64_C(30000000),
                                          &landing));
        CHECK(!samples.ops->seek_us(samples.opaque, 0, &landing));
        samples.ops->rewind(samples.opaque);
    }
    unsigned taken[2] = {rig.track[0].taken, rig.track[1].taken};
    CHECK(live_clock_play(&rig, UINT64_C(230000000)) == 0);
    CHECK(rig.track[0].taken > taken[0] && rig.track[1].taken > taken[1]);
    CHECK(live_clock_report(&rig, "after a refused seek") == 0
          && rig.track[0].backwards == 0 && rig.track[1].backwards == 0);

    /* The track menu picks the other audio rendition: the session reopens
       both at the windows now published (234.1 s, ending at 234 s). */
    live_clock_close(&rig);
    CHECK(budget.current == 0);
    rig.clock_us = UINT64_C(234100000);
    video.stall_from_us = video.stall_to_us = 0;
    CHECK(live_clock_open(&rig, &timed) == 0);
    printf("live demuxed pair, track change: video v%u, audio a%u then "
           "a%u\n", video.requested[0], timed.requested[0],
           timed.request_count > 1u ? timed.requested[1] : 0u);
    CHECK(video.requested[0] == 136u && timed.requested[0] == 1155u
          && timed.request_count > 1u && timed.requested[1] == 1154u);
    CHECK(live_clock_play(&rig, UINT64_C(250000000)) == 0);
    CHECK(rig.track[0].taken != 0 && rig.track[1].taken != 0);
    CHECK(live_clock_report(&rig, "after the track change") == 0);

    CHECK(live_clock_play(&rig, UINT64_C(270000000)) == 0);
    CHECK(live_clock_report(&rig, "20 s later") == 0
          && rig.track[0].backwards == 0 && rig.track[1].backwards == 0);
    CHECK(live_clock_last(&rig, 0) >= 143u
          && live_clock_last(&rig, 1) >= 1165u);
    live_clock_close(&rig);
    CHECK(budget.current == 0);
    return 0;
}

int main(void)
{
    int delivery_failed = 0;
    delivery_failed |= test_raw_audio_delivery(600u, false, false);
    delivery_failed |= test_raw_audio_delivery(100u, true, false);
    delivery_failed |= test_raw_audio_delivery(100u, false, true);
    delivery_failed |= test_audio_payload_ring_wrap(512u);
    delivery_failed |= test_audio_payload_ring_wrap(513u);
    if (delivery_failed) return 1;
    if (test_playlists() != 0) return 1;
    if (test_vod_duration_and_liveness() != 0) return 1;
    if (test_vod_seek_lands_on_segments_with_audio_rendition() != 0)
        return 1;
    if (test_decoder_aware_master_selection() != 0) return 1;
    if (test_oversized_live_playlist_stream() != 0) return 1;
    if (test_small_live_playlist_stream() != 0) return 1;
    if (test_unsupported_tags_refused_by_both_parsers() != 0) return 1;
    if (test_offline_playlists() != 0) return 1;
    if (test_streaming_source() != 0) return 1;
    if (test_muxed_held_video_does_not_strand_audio() != 0) return 1;
    if (test_muxed_seek_drops_preroll_audio() != 0) return 1;
    if (test_lane_queue_saturation() != 0) return 1;
    if (test_lane_refusals_roll_back() != 0) return 1;
    if (test_lane_seek_discards_taken_audio() != 0) return 1;
    if (test_video_only_primes_before_segment_completion() != 0) return 1;
    if (test_delivery_visit_ingests_several_packets() != 0) return 1;
    if (test_prime_parses_a_keyframe_in_few_visits() != 0) return 1;
    if (test_live_renditions_start_at_their_own_edges() != 0) return 1;
    if (test_live_renditions_align_by_timestamp() != 0) return 1;
    if (test_live_window_skips_keep_renditions_together() != 0) return 1;
    if (test_live_partner_passes_a_long_skip() != 0) return 1;
    if (test_live_demuxed_pair_through_open_skip_track_change_and_seek()
        != 0) return 1;
    if (test_mixed_source_without_early_video() != 0) return 1;
    if (test_audio_rendition_source_memory() != 0) return 1;
    if (test_pts_wrap_inside_a_segment() != 0) return 1;
    if (test_finite_playlist_entry_bound() != 0) return 1;
    if (test_signed_uri_playlist_text_bound() != 0) return 1;
    if (test_long_playlist_plays_and_seeks() != 0) return 1;
    if (test_reordered_units_last_one_frame() != 0) return 1;
    if (test_reordered_interval_across_segments() != 0) return 1;
    if (test_demuxed_track_sources_prime_independently() != 0) return 1;
    if (test_demuxed_raw_adts_audio_source() != 0) return 1;
    if (test_live_window_refresh() != 0) return 1;
    if (test_dvr_live_window_reloads_at_the_edge() != 0) return 1;
    if (test_live_refresh_gap_failure_and_endlist() != 0) return 1;
    if (test_live_segment_failure_survives_static_refresh() != 0) return 1;
    puts("media HLS tests passed");
    return 0;
}
