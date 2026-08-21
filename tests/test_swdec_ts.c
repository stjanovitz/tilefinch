#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "swdec_ts.h"

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

typedef struct {
    unsigned video_count;
    unsigned audio_count;
    size_t video_bytes[2];
    uint64_t video_pts[2];
    uint64_t audio_pts[2];
} Capture;

static void capture_video(void *opaque, const uint8_t *data, size_t length,
                          uint64_t pts90k)
{
    Capture *capture = opaque;
    (void) data;
    if (capture->video_count < 2u) {
        capture->video_bytes[capture->video_count] = length;
        capture->video_pts[capture->video_count] = pts90k;
    }
    capture->video_count++;
}

static void capture_audio(void *opaque, const uint8_t *data, size_t length,
                          uint64_t pts90k)
{
    Capture *capture = opaque;
    (void) data;
    (void) length;
    if (capture->audio_count < 2u)
        capture->audio_pts[capture->audio_count] = pts90k;
    capture->audio_count++;
}

static void encode_pts(uint8_t output[5], uint64_t pts)
{
    output[0] = (uint8_t) (0x21u | ((pts >> 29) & 0x0eu));
    output[1] = (uint8_t) (pts >> 22);
    output[2] = (uint8_t) (1u | ((pts >> 14) & 0xfeu));
    output[3] = (uint8_t) (pts >> 7);
    output[4] = (uint8_t) (1u | ((pts << 1) & 0xfeu));
}

static void ts_packet(uint8_t packet[188], unsigned pid, bool pusi,
                      const uint8_t *payload, size_t payload_bytes)
{
    memset(packet, 0xff, 188u);
    packet[0] = 0x47u;
    packet[1] = (uint8_t) ((pusi ? 0x40u : 0u) | ((pid >> 8) & 0x1fu));
    packet[2] = (uint8_t) pid;
    if (payload_bytes > 183u) abort();
    packet[3] = 0x30u;
    size_t adaptation = 183u - payload_bytes;
    packet[4] = (uint8_t) adaptation;
    if (adaptation != 0) packet[5] = 0u;
    memcpy(packet + 5u + adaptation, payload, payload_bytes);
}

static void psi_packet(uint8_t packet[188], unsigned pid,
                       const uint8_t *section, size_t section_bytes)
{
    uint8_t payload[184] = {0};
    payload[0] = 0u;
    memcpy(payload + 1u, section, section_bytes);
    ts_packet(packet, pid, true, payload, section_bytes + 1u);
}

static size_t pes(uint8_t *output, uint8_t stream_id, uint64_t pts,
                  const uint8_t *payload, size_t payload_bytes)
{
    output[0] = 0u; output[1] = 0u; output[2] = 1u;
    output[3] = stream_id;
    output[4] = 0u; output[5] = 0u;
    output[6] = 0x80u; output[7] = 0x80u; output[8] = 5u;
    encode_pts(output + 9u, pts);
    memcpy(output + 14u, payload, payload_bytes);
    return payload_bytes + 14u;
}

static int test_valid_stream(void)
{
    static const uint8_t pat[] = {
        0x00, 0xb0, 0x0d, 0x00, 0x01, 0xc1, 0x00, 0x00,
        0x00, 0x01, 0xe1, 0x00, 0, 0, 0, 0
    };
    static const uint8_t pmt[] = {
        0x02, 0xb0, 0x17, 0x00, 0x01, 0xc1, 0x00, 0x00,
        0xe1, 0x01, 0xf0, 0x00,
        0x1b, 0xe1, 0x01, 0xf0, 0x00,
        0x0f, 0xe1, 0x02, 0xf0, 0x00,
        0, 0, 0, 0
    };
    static const uint8_t au[] = {
        0, 0, 1, 9, 0xf0, 0, 0, 1, 0x65, 0x80, 0x11
    };
    static const uint8_t adts_pair[] = {
        0xff, 0xf1, 0x50, 0x80, 0x00, 0xff, 0xfc,
        0xff, 0xf1, 0x50, 0x80, 0x00, 0xff, 0xfc
    };
    Capture capture = {0};
    SwdecTs *ts = calloc(1u, sizeof(*ts));
    CHECK(ts != NULL);
    swdec_ts_init(ts, capture_video, capture_audio, &capture);
    uint8_t packet[188];
    psi_packet(packet, 0u, pat, sizeof(pat));
    CHECK(swdec_ts_feed(ts, packet, sizeof(packet)) == 0);
    psi_packet(packet, 0x100u, pmt, sizeof(pmt));
    CHECK(swdec_ts_feed(ts, packet, sizeof(packet)) == 0);
    CHECK(ts->video_pid == 0x101 && ts->audio_pid == 0x102);

    uint8_t payload[184];
    size_t bytes = pes(payload, 0xe0u, 90000u, au, sizeof(au));
    ts_packet(packet, 0x101u, true, payload, bytes);
    CHECK(swdec_ts_feed(ts, packet, sizeof(packet)) == 0);
    bytes = pes(payload, 0xe0u, 93600u, au, sizeof(au));
    ts_packet(packet, 0x101u, true, payload, bytes);
    CHECK(swdec_ts_feed(ts, packet, sizeof(packet)) == 0);

    bytes = pes(payload, 0xc0u, 90000u, adts_pair, sizeof(adts_pair));
    ts_packet(packet, 0x102u, true, payload, bytes);
    CHECK(swdec_ts_feed(ts, packet, sizeof(packet)) == 0);
    swdec_ts_flush(ts);
    CHECK(capture.video_count == 2u);
    CHECK(capture.video_pts[0] == 90000u);
    CHECK(capture.video_pts[1] == 93600u);
    CHECK(capture.audio_count == 2u);
    CHECK(capture.audio_pts[0] == 90000u);
    CHECK(capture.audio_pts[1] == 92089u);
    CHECK(ts->stats.sync_losses == 0u);
    CHECK(ts->stats.malformed_packets == 0u);
    CHECK(ts->stats.malformed_psi == 0u);
    free(ts);
    return 0;
}

static int test_hostile_bounds(void)
{
    SwdecTs *ts = calloc(1u, sizeof(*ts));
    CHECK(ts != NULL);
    swdec_ts_init(ts, NULL, NULL, NULL);
    uint8_t packet[188] = {0};
    packet[0] = 0x47u; packet[1] = 0x40u; packet[3] = 0x10u;
    packet[4] = 0xffu;
    CHECK(swdec_ts_feed(ts, packet, sizeof(packet)) == 0);
    CHECK(ts->stats.malformed_psi == 1u);

    ts->pmt_pid = 0x100;
    packet[1] = 0x41u; packet[2] = 0u; packet[4] = 0u;
    packet[5] = 2u; packet[6] = 0u;
    CHECK(swdec_ts_feed(ts, packet, sizeof(packet)) == 0);
    CHECK(ts->stats.malformed_psi == 2u);

    memset(packet, 0, sizeof(packet));
    packet[0] = 0x47u; packet[3] = 0x30u; packet[4] = 0xffu;
    CHECK(swdec_ts_feed(ts, packet, sizeof(packet)) == 0);
    CHECK(ts->stats.malformed_packets == 1u);

    uint8_t with_tail[189] = {0};
    memset(with_tail, 0xff, sizeof(with_tail));
    with_tail[0] = 0x47u; with_tail[3] = 0x20u;
    CHECK(swdec_ts_feed(ts, with_tail, sizeof(with_tail)) == 1);

    uint32_t random = 0x12345678u;
    uint8_t fuzz[997];
    for (unsigned pass = 0; pass < 128u; pass++) {
        for (size_t i = 0; i < sizeof(fuzz); i++) {
            random = random * UINT32_C(1664525) + UINT32_C(1013904223);
            fuzz[i] = (uint8_t) (random >> 24);
        }
        (void) swdec_ts_feed(ts, fuzz, sizeof(fuzz));
    }
    free(ts);
    return 0;
}

int main(void)
{
    if (test_valid_stream() != 0) return 1;
    if (test_hostile_bounds() != 0) return 1;
    puts("swdec TS tests passed");
    return 0;
}
