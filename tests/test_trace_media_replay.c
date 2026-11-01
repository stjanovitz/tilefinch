/*
 * Recorded page video, replayed with no network (src/fetch/trace_media_lane.inc
 * and the media rules in src/fetch/trace_replay.inc).
 *
 * A small HLS stream (master, variant, three TS segments, all generated here)
 * and an MP4 are written as a response-keyed HTTP trace. The stream is then
 * opened through the PSP's own media path in the host build
 * (src/media_replay_check.c: psp_media_hls.c over the background-transport
 * API, and the page-media probe plus media_http range source under the MP4
 * demuxer) and every sample is read. The trace is the only source: the host
 * has no background worker, so before the replay lane every HLS request was
 * refused, and a Range request was answered with the whole stored body.
 *
 * The recording deliberately differs from what the player asks for in the
 * two ways real recordings do: the master playlist was reached through a
 * redirect (the lane answers the page hop with that redirect and the
 * playlist rebases onto the CDN), and the segments were recorded with a
 * different signing token in their query than the replayed playlist names.
 *
 * argv[1]: a writable directory for the trace; argv[2]: an MP4 fixture.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "tilefinch/budget.h"
#include "tilefinch/fetch.h"
#include "tilefinch/media_replay_check.h"
#include "tilefinch/session.h"

static int failures;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "trace-media-replay failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        failures++; \
    } \
} while (0)

#define DOCUMENT "https://media.test/watch/clip"
#define MASTER "https://media.test/r/master.m3u8"
#define CDN "https://cdn.media.test/v/"
#define CLIP "https://media.test/clip.mp4"
#define PART "https://media.test/part.mp4"

/* ---- A minimal H.264 + AAC transport stream (as tests/test_media_hls.c) */
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
    write_bits(&bits, 0x67u, 8);
    write_bits(&bits, 100u, 8);  /* High profile */
    write_bits(&bits, 0, 8);
    write_bits(&bits, 30u, 8);
    write_ue(&bits, 0);
    write_ue(&bits, 1);
    write_ue(&bits, 0);
    write_ue(&bits, 0);
    write_bits(&bits, 0, 1);
    write_bits(&bits, 0, 1);
    write_ue(&bits, 0);
    write_ue(&bits, 0);
    write_ue(&bits, 0);
    write_ue(&bits, 1);
    write_bits(&bits, 0, 1);
    write_ue(&bits, 26u);        /* 432 pixels */
    write_ue(&bits, 14u);        /* 240 pixels */
    write_bits(&bits, 1, 1);
    write_bits(&bits, 1, 1);
    write_bits(&bits, 0, 1);
    write_bits(&bits, 0, 1);
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

/* PAT, PMT, one IDR access unit (SPS, PPS, slice) and one ADTS frame. */
static size_t build_segment(unsigned char *segment, uint64_t pts)
{
    static const unsigned char pat[] = {
        0x00,0xb0,0x0d,0,1,0xc1,0,0,0,1,0xe1,0,0,0,0,0
    };
    static const unsigned char pmt[] = {
        0x02,0xb0,0x17,0,1,0xc1,0,0,0xe1,1,0xf0,0,
        0x1b,0xe1,1,0xf0,0,0x0f,0xe1,2,0xf0,0,0,0,0,0
    };
    unsigned char *at = segment;
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
    return (size_t) (at - segment);
}

/* ---- The trace ------------------------------------------------------- */
static char trace_dir[1024];
static size_t record_count;

static bool write_file(const char *path, const void *bytes, size_t length)
{
    FILE *file = fopen(path, "wb");
    if (file == NULL) return false;
    bool ok = fwrite(bytes, 1, length, file) == length;
    return fclose(file) == 0 && ok;
}

/* A minimal (version 1) response record, as tests/
   make_declarative_refresh_fixture.py writes them. */
static bool add_record(const char *url, const char *effective_url,
                       long status, const char *content_type,
                       const char *content_range,
                       const void *body, size_t length)
{
    char path[1200];
    snprintf(path, sizeof(path), "%s/%04zu.body", trace_dir, record_count);
    if (!write_file(path, body, length)) return false;
    char meta[8192];
    int used = snprintf(
        meta, sizeof(meta),
        "psp-http-trace=1\nmethod=GET\nurl=%s\nsuccess=1\n"
        "async-delay-pumps=0\nexternal-cancel=0\ntransport-timeout=0\n"
        "error=\nstatus=%ld\nlength=%zu\neffective-url=%s\n"
        "content-type=%s\nserver=fixture\nset-cookie-count=0\n"
        "response-header-count=%d\nresponse-header-0=content-type: %s\n",
        url, status, length, effective_url, content_type,
        content_range == NULL ? 1 : 2, content_type);
    if (content_range != NULL) {
        used += snprintf(meta + used, sizeof(meta) - (size_t) used,
                         "response-header-1=content-range: %s\n",
                         content_range);
    }
    snprintf(path, sizeof(path), "%s/%04zu.meta", trace_dir, record_count);
    record_count++;
    return write_file(path, meta, (size_t) used);
}

/* A full (version 10) record carrying its body hash. hash_body is what the
   recorded hash covers, so a mismatch stands for a corrupt stored body;
   trailing bytes are written after the body. */
static bool add_hashed_record(const char *url, const void *body,
                              size_t length, const void *hash_body,
                              size_t trailing)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t at = 0; at < length; at++) {
        hash ^= ((const unsigned char *) hash_body)[at];
        hash *= UINT64_C(1099511628211);
    }
    char path[1200];
    snprintf(path, sizeof(path), "%s/%04zu.body", trace_dir, record_count);
    FILE *file = fopen(path, "wb");
    if (file == NULL) return false;
    bool ok = fwrite(body, 1, length, file) == length;
    for (size_t i = 0; ok && i < trailing; i++) ok = fputc('x', file) != EOF;
    if (fclose(file) != 0 || !ok) return false;
    char meta[8192];
    int used = snprintf(meta, sizeof(meta),
        "psp-http-trace=10\ncookie-values=redacted\nmethod=GET\nurl=%s\n"
        "logical-request-url=%s\nsuccess=1\nasync-delay-pumps=0\n"
        "external-cancel=0\ntransport-timeout=0\n"
        "redirect-origin-tainted=0\nerror=\nrequest-body-length=0\n"
        "request-body-hash=cbf29ce484222325\nrequest-content-type=\n"
        "request-cookie-bytes=0\nrequest-has-cf-clearance=0\n"
        "request-extra-header-bytes=0\nrequest-extra-header-shape=\n"
        "request-allow-http-errors=1\nrequest-enforce-cors=0\n"
        "request-redirect-same-origin-only=0\n"
        "request-cors-cached-response-validated=0\nrequest-if-none-match=\n"
        "request-if-modified-since=\nrequest-referer=\nrequest-origin=\n"
        "request-accept=\nrequest-sec-fetch-dest=\nrequest-sec-fetch-mode=\n"
        "request-sec-fetch-site=\nrequest-send-client-hints=0\n"
        "request-client-hint-tokens=\nrequest-client-hint-origin=\n"
        "request-send-low-client-hints=0\nrequest-sec-fetch-user=0\n"
        "request-upgrade-insecure=0\nrequest-user-agent=\n"
        "request-diagnostic-mobile-safari=0\nrequest-credentials=0\n"
        "request-credential-origin=\nrequest-initiator-url=\n"
        "request-referrer-source=\nrequest-referrer-policy=\n"
        "status=200\nlength=%zu\nresponse-body-hash=%016llx\n"
        "effective-url=%s\ncontent-type=video/mp2t\netag=\n"
        "last-modified=\ncf-mitigated=\naccept-ch=\ncritical-ch=\n"
        "server=fixture\ncf-ray=\n"
        "response-referrer-policy-metadata-valid=1\n"
        "response-referrer-policy-present=0\nresponse-referrer-policy=\n"
        "response-security-headers-truncated=0\nset-cookie-count=0\n"
        "response-header-count=1\n"
        "response-header-0=content-type: video/mp2t\n",
        url, url, length, (unsigned long long) hash, url);
    snprintf(path, sizeof(path), "%s/%04zu.meta", trace_dir, record_count);
    record_count++;
    return used > 0 && (size_t) used < sizeof(meta)
        && write_file(path, meta, (size_t) used);
}

#define HASHED_BYTES 40000u
#define HASHED_GOOD CDN "hashed/good.ts"
#define HASHED_BAD CDN "hashed/bad.ts"
#define HASHED_LONG CDN "hashed/long.ts"
static unsigned char hashed_body[HASHED_BYTES];
static unsigned char hashed_other[HASHED_BYTES];

static bool finish_trace(void)
{
    char info[256];
    int used = snprintf(info, sizeof(info),
                        "psp-http-trace-clock=1\norigin-ms=1700000000000\n"
                        "capture-complete=yes\nrecord-count=%zu\n",
                        record_count);
    char path[1200];
    snprintf(path, sizeof(path), "%s/trace.meta", trace_dir);
    return write_file(path, info, (size_t) used);
}

static unsigned char *read_fixture(const char *path, size_t *length)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) return NULL;
    unsigned char *bytes = NULL;
    if (fseek(file, 0, SEEK_END) == 0) {
        long size = ftell(file);
        if (size > 0 && fseek(file, 0, SEEK_SET) == 0) {
            bytes = malloc((size_t) size);
            if (bytes != NULL
                && fread(bytes, 1, (size_t) size, file) != (size_t) size) {
                free(bytes);
                bytes = NULL;
            }
            *length = (size_t) size;
        }
    }
    fclose(file);
    return bytes;
}

static bool build_trace(const unsigned char *mp4, size_t mp4_length)
{
    static const char master[] =
        "#EXTM3U\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=3000000,RESOLUTION=1280x720,"
        "CODECS=\"avc1.640028,mp4a.40.2\"\n"
        "high/index.m3u8?token=live\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=300000,RESOLUTION=432x240,"
        "CODECS=\"avc1.64001e,mp4a.40.2\"\n"
        "low/index.m3u8?token=live\n";
    static const char variant[] =
        "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:2\n"
        "#EXT-X-MEDIA-SEQUENCE:0\n"
        "#EXTINF:2.0,\nseg0.ts?token=live\n"
        "#EXTINF:2.0,\nseg1.ts?token=live\n"
        "#EXTINF:2.0,\nseg2.ts?token=live\n"
        "#EXT-X-ENDLIST\n";
    bool ok = add_record(MASTER, CDN "master.m3u8", 200,
                         "application/vnd.apple.mpegurl", NULL,
                         master, strlen(master))
        && add_record(CDN "low/index.m3u8?token=live",
                      CDN "low/index.m3u8?token=live", 200,
                      "application/vnd.apple.mpegurl", NULL,
                      variant, strlen(variant));
    for (unsigned i = 0; ok && i < 3; i++) {
        unsigned char segment[188u * 8u];
        size_t length = build_segment(segment, (uint64_t) i * 180000u);
        char url[256];
        snprintf(url, sizeof(url), CDN "low/seg%u.ts?token=recorded", i);
        ok = add_record(url, url, 200, "video/mp2t", NULL, segment, length);
    }
    /* The whole movie, and a recorded 206 holding only its first half. */
    char part_range[96];
    size_t half = mp4_length / 2u;
    snprintf(part_range, sizeof(part_range), "bytes 0-%zu/%zu", half - 1u,
             mp4_length);
    for (size_t i = 0; i < HASHED_BYTES; i++) {
        hashed_body[i] = (unsigned char) (i * 7u + (i >> 8));
        hashed_other[i] = hashed_body[i];
    }
    hashed_other[HASHED_BYTES - 1u] ^= 1u;
    ok = ok
        && add_hashed_record(HASHED_GOOD, hashed_body, HASHED_BYTES,
                             hashed_body, 0)
        && add_hashed_record(HASHED_BAD, hashed_body, HASHED_BYTES,
                             hashed_other, 0)
        && add_hashed_record(HASHED_LONG, hashed_body, HASHED_BYTES,
                             hashed_body, 1);
    ok = ok
        && add_record(CLIP, CLIP, 200, "video/mp4", NULL, mp4, mp4_length)
        && add_record(PART, PART, 206, "video/mp4", part_range, mp4, half);
    return ok && finish_trace();
}

/* ---- Direct lane requests -------------------------------------------- */
/* Streams a hashed record one TS packet at a time, as HLS takes it. Returns
   the bytes taken; *completed says whether the request finished cleanly. */
static size_t lane_stream(const char *url, unsigned char *copy,
                          bool *completed, Budget *budget)
{
    FetchRequest request = {.allow_http_errors = true, .accept = "*/*"};
    FetchBackgroundEnqueueStatus status = FETCH_BACKGROUND_ENQUEUE_UNAVAILABLE;
    uint64_t id = fetch_background_transport_enqueue_media_stream_sized_diagnosed(
        url, &request, 1024u * 1024u, 15000, 16384u, &status);
    *completed = false;
    if (id == 0 || status != FETCH_BACKGROUND_ENQUEUE_ADMITTED) return 0;
    FetchResult *headers = fetch_result_create(budget);
    bool admitted = headers != NULL
        && fetch_background_transport_take_headers(id, headers);
    fetch_result_free(headers);
    size_t taken = 0, length = 0;
    while (admitted && taken + 188u <= HASHED_BYTES + 188u
           && fetch_background_transport_take_chunk(
                  id, copy + taken, 188u, &length)) {
        taken += length;
    }
    FetchResult *result = fetch_result_create(budget);
    *completed = result != NULL
        && fetch_background_transport_take_fetch_result_consumed(
               id, budget, result, taken);
    fetch_result_free(result);
    if (!*completed) (void) fetch_background_transport_cancel(id, "test");
    return taken;
}

/* The lane checks a record's hash while the body is read (not by reading
   it all first at enqueue), through a read-ahead slice: the bytes arrive
   intact, a corrupt body fails before its last bytes are handed out, and a
   stored file longer than its record is refused at once. */
static void test_lane_hash_on_delivery(Budget *budget)
{
    static unsigned char copy[HASHED_BYTES + 188u];
    bool completed = false;
    size_t taken = lane_stream(HASHED_GOOD, copy, &completed, budget);
    CHECK(completed && taken == HASHED_BYTES);
    CHECK(memcmp(copy, hashed_body, HASHED_BYTES) == 0);
    taken = lane_stream(HASHED_BAD, copy, &completed, budget);
    CHECK(!completed && taken < HASHED_BYTES && taken > HASHED_BYTES / 2u);
    taken = lane_stream(HASHED_LONG, copy, &completed, budget);
    CHECK(!completed && taken == 0);
}

static bool lane_window(const char *url, const char *range,
                        unsigned char *destination, size_t capacity,
                        FetchBackgroundMediaResponse *response)
{
    char header[96];
    snprintf(header, sizeof(header), "Range: %s", range);
    FetchRequest request = {
        .allow_http_errors = true,
        .accept = "*/*",
        .extra_headers = header
    };
    FetchBackgroundEnqueueStatus status = FETCH_BACKGROUND_ENQUEUE_UNAVAILABLE;
    uint64_t id = fetch_background_transport_enqueue_media_diagnosed(
        url, &request, capacity, 15000, &status);
    memset(response, 0, sizeof(*response));
    return id != 0 && status == FETCH_BACKGROUND_ENQUEUE_ADMITTED
        && fetch_background_transport_take_media(
               id, destination, capacity, response)
        && response->success;
}

static void test_lane_ranges(const unsigned char *mp4, size_t mp4_length)
{
    unsigned char window[4096];
    FetchBackgroundMediaResponse response;
    char expected[128];
    /* A window inside a stored 200. */
    CHECK(lane_window(CLIP, "bytes=100-1123", window, sizeof(window),
                      &response));
    CHECK(response.status_code == 206 && response.length == 1024u);
    CHECK(memcmp(window, mp4 + 100, 1024u) == 0);
    snprintf(expected, sizeof(expected), "bytes 100-1123/%zu", mp4_length);
    CHECK(strcmp(response.content_range, expected) == 0);
    CHECK(strcmp(response.content_length, "1024") == 0);
    /* Open-ended and suffix windows are clamped to the resource. */
    CHECK(lane_window(CLIP, "bytes=-50", window, sizeof(window), &response));
    CHECK(response.status_code == 206 && response.length == 50u);
    CHECK(memcmp(window, mp4 + mp4_length - 50u, 50u) == 0);
    char open_range[64];
    snprintf(open_range, sizeof(open_range), "bytes=%zu-", mp4_length - 10u);
    CHECK(lane_window(CLIP, open_range, window, sizeof(window), &response));
    CHECK(response.length == 10u
          && memcmp(window, mp4 + mp4_length - 10u, 10u) == 0);
    /* A stored 206 answers inside its span and refuses past it. */
    size_t half = mp4_length / 2u;
    char inside[64], outside[64];
    snprintf(inside, sizeof(inside), "bytes=%zu-%zu", half - 200u,
             half - 1u);
    snprintf(outside, sizeof(outside), "bytes=%zu-%zu", half, half + 99u);
    CHECK(lane_window(PART, inside, window, sizeof(window), &response));
    CHECK(response.length == 200u
          && memcmp(window, mp4 + half - 200u, 200u) == 0);
    CHECK(!lane_window(PART, outside, window, sizeof(window), &response));
    /* An unrecorded address fails without reaching any network. */
    CHECK(!lane_window("https://media.test/missing.mp4", "bytes=0-99",
                       window, sizeof(window), &response));
    CHECK(fetch_background_transport_active_operations() == 0);
}

/* Parameter sets in avcC or Annex-B form around one SPS (no PPS needed by
   the verdict, but present as an encoder writes it). */
static size_t decoder_config(unsigned char *out, const unsigned char *sps,
                             size_t sps_length, bool annexb)
{
    static const unsigned char pps[] = {0x68, 0xce, 0x3c, 0x80};
    size_t at = 0;
    if (annexb) {
        static const unsigned char start[] = {0, 0, 0, 1};
        memcpy(out + at, start, 4); at += 4;
        memcpy(out + at, sps, sps_length); at += sps_length;
        memcpy(out + at, start, 4); at += 4;
        memcpy(out + at, pps, sizeof(pps));
        return at + sizeof(pps);
    }
    out[at++] = 1;
    out[at++] = sps[1];
    out[at++] = sps[2];
    out[at++] = sps[3];
    out[at++] = 0xff;
    out[at++] = 0xe1;
    out[at++] = (unsigned char) (sps_length >> 8u);
    out[at++] = (unsigned char) sps_length;
    memcpy(out + at, sps, sps_length); at += sps_length;
    out[at++] = 1;
    out[at++] = 0;
    out[at++] = (unsigned char) sizeof(pps);
    memcpy(out + at, pps, sizeof(pps));
    return at + sizeof(pps);
}

/* The decoder the PSP open path picks for page video, by profile, level
   and picture shape. */
static void test_decoder_verdicts(void)
{
    /* A device-refused rendition: Constrained Baseline, Level 1.3, three
       references, 202x360 portrait (208x368 coded, cropped). */
    static const unsigned char baseline_13_portrait[] = {
        0x67, 0x42, 0xc0, 0x0d, 0xd9, 0x03, 0x42, 0xfe, 0x49, 0x70, 0x16,
        0xa0, 0x20, 0x20, 0x28, 0x00, 0x00, 0x03, 0x00, 0x08, 0x00, 0x00,
        0x03, 0x01, 0x94, 0x78, 0xa1, 0x52
    };
    /* Baseline Level 3.1 at 320x180; Main 3.0 and High 3.0 at 640x360. */
    static const unsigned char baseline_31_small[] = {
        0x67, 0x42, 0xc0, 0x1f, 0xda, 0x05, 0x06, 0x7e, 0x74
    };
    static const unsigned char main_30_wide[] = {
        0x67, 0x4d, 0x40, 0x1e, 0xda, 0x02, 0x80, 0xbf, 0xe5, 0x40
    };
    static const unsigned char high_30_wide[] = {
        0x67, 0x64, 0x00, 0x1e, 0xac, 0xb4, 0x05, 0x01, 0x7f, 0xca, 0x80
    };
    unsigned char config[128], sps[64];
    char refusal[128];
    unsigned profile = 0, level = 0;
    for (int annexb = 0; annexb <= 1; annexb++) {
        size_t length = decoder_config(
            config, baseline_13_portrait, sizeof(baseline_13_portrait),
            annexb != 0);
        CHECK(media_replay_check_decoder(
                  config, length, annexb != 0, &profile, &level, refusal,
                  sizeof(refusal)) == MEDIA_REPLAY_DECODER_FIRMWARE
              && profile == 66u && level == 13u && refusal[0] == '\0');
        /* The same picture above Level 3.0: too tall for the software
           decoder, so refused with the level reason, which lets the page's
           next candidate be tried. */
        memcpy(sps, baseline_13_portrait, sizeof(baseline_13_portrait));
        sps[3] = 31u;
        length = decoder_config(config, sps, sizeof(baseline_13_portrait),
                                annexb != 0);
        CHECK(media_replay_check_decoder(
                  config, length, annexb != 0, &profile, &level, refusal,
                  sizeof(refusal)) == MEDIA_REPLAY_DECODER_REFUSED
              && level == 31u
              && strstr(refusal, "profile/level unsupported") != NULL);
        /* A small picture above Level 3.0 goes to the software decoder. */
        length = decoder_config(config, baseline_31_small,
                                sizeof(baseline_31_small), annexb != 0);
        CHECK(media_replay_check_decoder(
                  config, length, annexb != 0, NULL, NULL, refusal,
                  sizeof(refusal)) == MEDIA_REPLAY_DECODER_SOFTWARE);
        length = decoder_config(config, main_30_wide, sizeof(main_30_wide),
                                annexb != 0);
        CHECK(media_replay_check_decoder(
                  config, length, annexb != 0, NULL, NULL, refusal,
                  sizeof(refusal)) == MEDIA_REPLAY_DECODER_FIRMWARE);
        length = decoder_config(config, high_30_wide, sizeof(high_30_wide),
                                annexb != 0);
        CHECK(media_replay_check_decoder(
                  config, length, annexb != 0, NULL, NULL, refusal,
                  sizeof(refusal)) == MEDIA_REPLAY_DECODER_REFUSED
              && strstr(refusal, "software H.264 is limited") != NULL);
    }
    CHECK(media_replay_check_decoder(NULL, 0, false, NULL, NULL, refusal,
                                     sizeof(refusal))
          == MEDIA_REPLAY_DECODER_REFUSED);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s TRACE-DIR MP4\n", argv[0]);
        return EXIT_FAILURE;
    }
    snprintf(trace_dir, sizeof(trace_dir), "%s", argv[1]);
    (void) mkdir(trace_dir, 0755);
    size_t mp4_length = 0;
    unsigned char *mp4 = read_fixture(argv[2], &mp4_length);
    CHECK(mp4 != NULL && mp4_length > 4096u);
    if (mp4 == NULL || !build_trace(mp4, mp4_length)) {
        fprintf(stderr, "trace-media-replay: fixture setup failed\n");
        free(mp4);
        return EXIT_FAILURE;
    }

    test_decoder_verdicts();

    /* Without a replay the host has no media transport at all. */
    CHECK(!fetch_background_transport_available());
    char error[256] = {0};
    CHECK(fetch_trace_replay_begin_response_keyed(trace_dir, error,
                                                  sizeof(error)));
    CHECK(fetch_background_transport_available());

    Budget budget;
    budget_init(&budget, 48u * 1024u * 1024u);
    BrowserSession session;
    CHECK(browser_session_init(&session, &budget, 128u * 1024u));

    /* HLS: redirected master, query-aliased segments, every sample read. */
    MediaReplayCheckReport report;
    bool played = media_replay_check(
        &budget, &session, MASTER, DOCUMENT, PSP_MEDIA_HLS_BOUND_FIRMWARE,
        UINT64_C(60000000), 0, &report);
    if (!played) fprintf(stderr, "HLS check: %s\n", report.error);
    CHECK(played);
    CHECK(report.hls && report.opened && report.ended);
    CHECK(report.width == 432u && report.height == 240u);
    CHECK(report.hls_segments == 3u);
    CHECK(report.video_samples == 3u && report.audio_samples == 3u);
    CHECK(report.video_until_us >= UINT64_C(4000000));
    FetchTraceReplayStats stats;
    CHECK(fetch_trace_replay_stats(&stats));
    /* Master (its redirect, then the CDN copy), variant, three segments. */
    CHECK(stats.media_lane_request_count == 6u);
    CHECK(stats.media_lane_served_count == 6u);
    CHECK(stats.media_alias_count == 4u);
    CHECK(stats.unmatched_request_count == 0u);
    CHECK(stats.invalid_route_request_count == 0u);
    /* The VOD stream has its playlist's 6 s, and seeks: 3 s lands on the
       second segment, where both tracks resume. */
    CHECK(report.duration_us == UINT64_C(6000000));
    played = media_replay_check_seek(
        &budget, &session, MASTER, DOCUMENT, PSP_MEDIA_HLS_BOUND_FIRMWARE,
        UINT64_C(60000000), UINT64_C(3000000), &report);
    if (!played) fprintf(stderr, "HLS seek check: %s\n", report.error);
    CHECK(played && report.seek_ran && report.seek_ok);
    CHECK(report.seek_actual_us == UINT64_C(2000000)
          && report.seek_video_us == UINT64_C(2000000)
          && report.seek_audio_us == UINT64_C(2000000));
    CHECK(fetch_trace_replay_stats(&stats));
    CHECK(stats.unmatched_request_count == 0u);

    /* MP4: the probe and the range source read windows of stored bodies.
       They run on the player's private schedulers, so a page network held
       for the open player (browser_engine_hold_page_network) does not
       reach them: the page's own requests are refused meanwhile. */
    FetchSchedulerDomain *page_domain = fetch_scheduler_domain_create(
        &budget, 4u);
    FetchScheduler *page_view = fetch_scheduler_create_in_domain(
        page_domain, 1u, 4096u);
    fetch_scheduler_domain_set_held(page_domain, true);
    FetchRequest page_request = {.method = "GET"};
    CHECK(page_view != NULL && fetch_scheduler_held(page_view)
          && fetch_scheduler_enqueue(page_view, DOCUMENT, &page_request,
                                     4096u, 1000) == 0);
    played = media_replay_check(
        &budget, &session, CLIP, DOCUMENT, PSP_MEDIA_HLS_BOUND_FIRMWARE,
        UINT64_C(60000000), 0, &report);
    if (!played) fprintf(stderr, "MP4 check: %s\n", report.error);
    CHECK(played);
    CHECK(!report.hls && report.opened && report.ended);
    CHECK(report.content_length == mp4_length);
    CHECK(report.video_samples > 0u);
    CHECK(report.width == 320u && report.height == 240u);
    CHECK(report.decoder == MEDIA_REPLAY_DECODER_FIRMWARE
          && report.profile_idc == 66u);
    CHECK(fetch_trace_replay_stats(&stats));
    CHECK(stats.range_slice_count >= 2u);
    CHECK(stats.unmatched_request_count == 0u);
    CHECK(stats.invalid_route_request_count == 0u);

    CHECK(fetch_scheduler_held(page_view));
    fetch_scheduler_destroy(page_view);
    fetch_scheduler_domain_release(page_domain);

    test_lane_ranges(mp4, mp4_length);
    test_lane_hash_on_delivery(&budget);

    browser_session_destroy(&session);
    fetch_trace_end();
    CHECK(!fetch_background_transport_available());
    CHECK(budget.current == 0u);
    free(mp4);
    if (failures != 0) {
        fprintf(stderr, "%d trace-media-replay check(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    printf("trace-media-replay tests passed\n");
    return EXIT_SUCCESS;
}
