#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tilefinch/budget.h"
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
    unsigned char segment[2][188u * 6u];
    size_t segment_bytes[2];
    struct { bool active; size_t segment; size_t offset; } request[4];
    unsigned starts;
    unsigned start_calls;
    unsigned admission_deferrals;
    unsigned cancels;
} MockTransport;

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

static void build_video_only_segment(MockTransport *mock, size_t segment,
                                     uint64_t pts)
{
    static const unsigned char pat[] = {
        0x00,0xb0,0x0d,0,1,0xc1,0,0,0,1,0xe1,0,0,0,0,0
    };
    static const unsigned char pmt[] = {
        0x02,0xb0,0x12,0,1,0xc1,0,0,0xe1,1,0xf0,0,
        0x1b,0xe1,1,0xf0,0,0,0,0,0
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
    static const unsigned char second_au[] = {
        0,0,1,0x41,0x80,0x22
    };
    bytes = pes(payload, 0xe0u, pts + 3600u,
                second_au, sizeof(second_au));
    ts_packet(at, 0x101u, true, payload, bytes); at += 188u;
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
    size_t segment = strstr(url, "two.ts") != NULL ? 1u : 0u;
    for (size_t i = 0; i < 4u; i++) {
        if (!mock->request[i].active) {
            mock->request[i].active = true;
            mock->request[i].segment = segment;
            mock->request[i].offset = 0;
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
    size_t segment = mock->request[index].segment;
    size_t offset = mock->request[index].offset;
    if (offset == mock->segment_bytes[segment]) {
        mock->request[index].active = false;
        return MEDIA_HLS_TRANSPORT_COMPLETE;
    }
    size_t bytes = mock->segment_bytes[segment] - offset;
    if (bytes > 131u) bytes = 131u;
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
    CHECK(budget.current == 0);
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

int main(void)
{
    if (test_playlists() != 0) return 1;
    if (test_streaming_source() != 0) return 1;
    if (test_video_only_primes_before_segment_completion() != 0) return 1;
    puts("media HLS tests passed");
    return 0;
}
