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
#include "tilefinch/platform.h"
#include "tilefinch/session.h"
#include "../src/tilefinch_test_faults.h"

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

/* One recorded hop of an updater request, in the shape the hop machine asks
   for (scripts/record-glyph-pack-replay.py writes the same record). A record
   is matched on every non-blank request field, so the wrong Accept or
   User-Agent would not be served. */
static bool add_hop_record(const char *url, long status, const char *location,
                           const void *body, size_t length)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t at = 0; at < length; at++) {
        hash ^= ((const unsigned char *) body)[at];
        hash *= UINT64_C(1099511628211);
    }
    char path[1200];
    snprintf(path, sizeof(path), "%s/%04zu.body", trace_dir, record_count);
    if (!write_file(path, body, length)) return false;
    char meta[8192];
    int used = snprintf(meta, sizeof(meta),
        "psp-http-trace=10\ncookie-values=redacted\nmethod=GET\nurl=%s\n"
        "logical-request-url=%s\nsuccess=1\nasync-delay-pumps=0\n"
        "external-cancel=0\ntransport-timeout=0\n"
        "redirect-origin-tainted=0\nerror=\nrequest-body-length=0\n"
        "request-body-hash=cbf29ce484222325\nrequest-content-type=\n"
        "request-cookie-bytes=0\nrequest-has-cf-clearance=0\n"
        "request-extra-header-bytes=0\nrequest-extra-header-shape=\n"
        "request-allow-http-errors=0\nrequest-enforce-cors=0\n"
        "request-redirect-same-origin-only=0\n"
        "request-cors-cached-response-validated=0\nrequest-if-none-match=\n"
        "request-if-modified-since=\nrequest-referer=\nrequest-origin=\n"
        "request-accept=application/octet-stream\n"
        "request-sec-fetch-dest=\nrequest-sec-fetch-mode=\n"
        "request-sec-fetch-site=\nrequest-send-client-hints=0\n"
        "request-client-hint-tokens=\nrequest-client-hint-origin=\n"
        "request-send-low-client-hints=0\nrequest-sec-fetch-user=0\n"
        "request-upgrade-insecure=0\nrequest-user-agent=Tilefinch-Updater/1\n"
        "request-diagnostic-mobile-safari=0\nrequest-credentials=0\n"
        "request-credential-origin=\nrequest-initiator-url=\n"
        "request-referrer-source=\nrequest-referrer-policy=\n"
        "status=%ld\nlength=%zu\nresponse-body-hash=%016llx\n"
        "effective-url=%s\ncontent-type=application/octet-stream\netag=\n"
        "last-modified=\ncf-mitigated=\naccept-ch=\ncritical-ch=\n"
        "server=fixture\ncf-ray=\n"
        "response-referrer-policy-metadata-valid=1\n"
        "response-referrer-policy-present=0\nresponse-referrer-policy=\n"
        "response-security-headers-truncated=0\nset-cookie-count=0\n"
        "response-header-count=2\n"
        "response-header-0=content-type: application/octet-stream\n"
        "response-header-1=%s%s\n",
        url, url, status, length, (unsigned long long) hash, url,
        location == NULL ? "x-fixture: 1" : "location: ",
        location == NULL ? "" : location);
    snprintf(path, sizeof(path), "%s/%04zu.meta", trace_dir, record_count);
    record_count++;
    return used > 0 && (size_t) used < sizeof(meta)
        && write_file(path, meta, (size_t) used);
}

#define RELEASE_LATEST \
    "https://github.com/owner/models/releases/latest/download/pack.tfgm"
#define RELEASE_TAGGED \
    "https://github.com/owner/models/releases/download/v8/pack.tfgm"
#define RELEASE_ASSET \
    "https://release-assets.githubusercontent.com/asset/1/pack.tfgm?sig=a"
#define RELEASE_STRAY \
    "https://github.com/owner/models/releases/latest/download/stray.tfgm"
#define RELEASE_OUTSIDE "https://media.test/outside.tfgm"
static const char release_body[] = "the signed bytes";

/* Two release downloads larger than one pump's body quota. */
#define QUOTA_BYTES 40000u
#define QUOTA_START_A \
    "https://github.com/owner/models/releases/download/v8/a.tfgm"
#define QUOTA_ASSET_A \
    "https://release-assets.githubusercontent.com/asset/2/a.tfgm?sig=a"
#define QUOTA_START_B \
    "https://github.com/owner/models/releases/download/v8/b.tfgm"
#define QUOTA_ASSET_B \
    "https://release-assets.githubusercontent.com/asset/3/b.tfgm?sig=b"
static unsigned char quota_body[QUOTA_BYTES];
/* A streamed release download for a consumer with a small buffer. */
#define CAPACITY_START \
    "https://github.com/owner/models/releases/download/v8/c.tfgm"
#define CAPACITY_ASSET \
    "https://release-assets.githubusercontent.com/asset/4/c.tfgm?sig=c"
static unsigned char capacity_body[QUOTA_BYTES];

static bool build_release_chain(void)
{
    memset(quota_body, 'q', sizeof(quota_body));
    for (size_t i = 0; i < sizeof(capacity_body); i++)
        capacity_body[i] = (unsigned char) ('a' + (i * 7u + i / 251u) % 26u);
    return add_hop_record(QUOTA_START_A, 302, QUOTA_ASSET_A, "", 0)
        && add_hop_record(CAPACITY_START, 302, CAPACITY_ASSET, "", 0)
        && add_hop_record(CAPACITY_ASSET, 200, NULL, capacity_body,
                          QUOTA_BYTES)
        && add_hop_record(QUOTA_ASSET_A, 200, NULL, quota_body, QUOTA_BYTES)
        && add_hop_record(QUOTA_START_B, 302, QUOTA_ASSET_B, "", 0)
        && add_hop_record(QUOTA_ASSET_B, 200, NULL, quota_body, QUOTA_BYTES)
        && add_hop_record(RELEASE_LATEST, 302, RELEASE_TAGGED, "", 0)
        && add_hop_record(RELEASE_TAGGED, 302, RELEASE_ASSET, "", 0)
        && add_hop_record(RELEASE_ASSET, 200, NULL, release_body,
                          sizeof(release_body) - 1u)
        && add_hop_record(RELEASE_STRAY, 302, RELEASE_OUTSIDE, "", 0)
        && add_hop_record(RELEASE_OUTSIDE, 200, NULL, release_body,
                          sizeof(release_body) - 1u);
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
    return ok && build_release_chain() && finish_trace();
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
/* ---- The updater's redirect chain ------------------------------------- */
static bool release_fetch(Budget *budget, const char *url, FetchResult *result,
                          bool *success)
{
    FetchScheduler *scheduler = fetch_scheduler_create(budget, 1u, 65536u);
    if (scheduler == NULL
        || !fetch_scheduler_enable_background_transport(scheduler, true)) {
        fetch_scheduler_destroy(scheduler);
        return false;
    }
    FetchRequest request = {
        .method = "GET",
        .accept = "application/octet-stream",
        .credentials = FETCH_CREDENTIALS_OMIT,
        .identity_encoding = true,
        .redirect_policy = FETCH_REDIRECT_GITHUB_RELEASE,
        .user_agent = "Tilefinch-Updater/1"
    };
    uint64_t id = fetch_scheduler_enqueue(scheduler, url, &request, 4096u,
                                          30000);
    bool taken = false;
    for (unsigned pump = 0; id != 0 && pump < 64u && !taken; pump++) {
        (void) fetch_scheduler_pump(scheduler, 1u, 0u);
        taken = fetch_scheduler_take(scheduler, id, success, result);
    }
    fetch_scheduler_destroy(scheduler);
    return taken;
}

/* A recorded release download runs the hop machine: one recorded response per
   hop, so the redirects are really followed and really policed. */
static void test_release_redirect_chain(Budget *budget)
{
    FetchTraceReplayStats before, after;
    CHECK(fetch_trace_replay_stats(&before));
    FetchResult *result = fetch_result_create(budget);
    CHECK(result != NULL);
    if (result == NULL) return;
    bool success = false;
    CHECK(release_fetch(budget, RELEASE_LATEST, result, &success));
    CHECK(success);
    CHECK(result->status_code == 200);
    CHECK(result->length == sizeof(release_body) - 1u
          && memcmp(result->data, release_body, result->length) == 0);
    CHECK(strcmp(result->effective_url, RELEASE_ASSET) == 0);
    CHECK(fetch_trace_replay_stats(&after));
    /* 302, 302, 200: three lane answers, not one whole-response replay. */
    CHECK(after.media_lane_request_count
          == before.media_lane_request_count + 3u);
    CHECK(after.media_lane_served_count
          == before.media_lane_served_count + 3u);
    fetch_result_destroy(result);

    /* The recording cannot talk the client off the release hosts, although
       it holds a response for the forbidden URL. */
    success = true;
    before = after;
    CHECK(release_fetch(budget, RELEASE_STRAY, result, &success));
    CHECK(!success);
    CHECK(strstr(result->error, "redirect target") != NULL);
    CHECK(fetch_trace_replay_stats(&after));
    CHECK(after.media_lane_request_count
          == before.media_lane_request_count + 1u);
    fetch_result_destroy(result);
    fetch_result_free(result);
}

static const FetchRequest release_request = {
    .method = "GET",
    .accept = "application/octet-stream",
    .credentials = FETCH_CREDENTIALS_OMIT,
    .identity_encoding = true,
    .redirect_policy = FETCH_REDIRECT_GITHUB_RELEASE,
    .user_agent = "Tilefinch-Updater/1"
};

/* Enqueues a release download and reports whether its first hop went to
   the worker (here the replay lane). One that did not is on the host's
   browser-thread curl path: it is cancelled before any pump, so it never
   reaches the network. */
static uint64_t release_enqueue_on_worker(FetchScheduler *scheduler,
                                          bool *on_worker)
{
    FetchTraceReplayStats before, after;
    CHECK(fetch_trace_replay_stats(&before));
    uint64_t id = fetch_scheduler_enqueue(scheduler, RELEASE_LATEST,
                                          &release_request, 4096u, 30000);
    CHECK(fetch_trace_replay_stats(&after));
    *on_worker = id != 0 && after.media_lane_request_count
                                == before.media_lane_request_count + 1u;
    if (id != 0 && !*on_worker) {
        (void) fetch_scheduler_cancel(scheduler, id, "test: not on worker");
        bool ignored = false;
        FetchResult result = {0};
        if (fetch_scheduler_take(scheduler, id, &ignored, &result))
            fetch_result_destroy(&result);
        return 0;
    }
    return id;
}

static bool release_finish(FetchScheduler *scheduler, uint64_t id)
{
    bool success = false, taken = false;
    FetchResult result = {0};
    for (unsigned pump = 0; id != 0 && pump < 64u && !taken; pump++) {
        (void) fetch_scheduler_pump(scheduler, 1u, 0u);
        taken = fetch_scheduler_take(scheduler, id, &success, &result);
    }
    bool whole = taken && success && result.status_code == 200
        && result.length == sizeof(release_body) - 1u
        && memcmp(result.data, release_body, result.length) == 0;
    if (taken) fetch_result_destroy(&result);
    return whole;
}

/* NW-04 question 3. A scheduler whose 64 KiB worker chunk was refused
   once is not demoted for life: the next request attaches again and runs
   on the worker. Under the PSP rule (fail_worker_refused_hops) a request
   the worker is unavailable to fails with a named refusal ("worker-chunk",
   "worker-off") instead of running curl on the browser thread. */
static void test_worker_attach_is_retried(Budget *budget)
{
    TilefinchTestFaults *faults = tilefinch_test_faults();
    FetchScheduler *scheduler = fetch_scheduler_create(budget, 1u, 65536u);
    CHECK(scheduler != NULL);
    if (scheduler == NULL) return;
    /* The chunk is the attach's only Budget allocation. */
    budget_inject_failure_after(budget, 0);
    CHECK(!fetch_scheduler_enable_background_transport(scheduler, true));
    budget_clear_failure_injection(budget);

    /* The same refusal at the first request fails only that request. */
    faults->fail_worker_refused_hops = true;
    budget_inject_failure_after(budget, 0);
    bool on_worker = false;
    uint64_t id = fetch_scheduler_enqueue(scheduler, RELEASE_LATEST,
                                          &release_request, 4096u, 30000);
    budget_clear_failure_injection(budget);
    if (id != 0) {
        fprintf(stderr, "chunk refusal at enqueue admitted request %llu "
                "(%s)\n", (unsigned long long) id,
                fetch_scheduler_last_error(scheduler));
        (void) fetch_scheduler_cancel(scheduler, id, "test cleanup");
        bool ignored = false;
        FetchResult result = {0};
        if (fetch_scheduler_take(scheduler, id, &ignored, &result))
            fetch_result_destroy(&result);
    }
    CHECK(id == 0
          && strstr(fetch_scheduler_last_error(scheduler),
                    "worker unavailable (worker-chunk)") != NULL);

    id = release_enqueue_on_worker(scheduler, &on_worker);
    if (!on_worker)
        fprintf(stderr, "request after a refused worker chunk ran off the "
                "worker\n");
    CHECK(on_worker && release_finish(scheduler, id));
    fetch_scheduler_destroy(scheduler);

    /* A scheduler that never asked for the worker: refused by name under
       the PSP rule, the host's browser-thread path otherwise. */
    scheduler = fetch_scheduler_create(budget, 1u, 65536u);
    CHECK(scheduler != NULL);
    if (scheduler == NULL) return;
    id = fetch_scheduler_enqueue(scheduler, RELEASE_LATEST,
                                 &release_request, 4096u, 30000);
    CHECK(id == 0
          && strstr(fetch_scheduler_last_error(scheduler),
                    "worker unavailable (worker-off)") != NULL);
    if (id != 0) {
        (void) fetch_scheduler_cancel(scheduler, id, "test cleanup");
        bool ignored = false;
        FetchResult result = {0};
        if (fetch_scheduler_take(scheduler, id, &ignored, &result))
            fetch_result_destroy(&result);
    }
    faults->fail_worker_refused_hops = false;
    id = release_enqueue_on_worker(scheduler, &on_worker);
    CHECK(!on_worker && id == 0);
    CHECK(fetch_scheduler_pending(scheduler) == 0);
    fetch_scheduler_destroy(scheduler);
}

/* NW-04 question 1. A blocking request (sync XHR, module import) waits on
   the thread that would pump everything else, so nothing it waits behind
   frees while it waits. Here a page scheduler's six resource downloads,
   never pumped, hold six of the domain's seven slots and every descriptor
   the worker (the replay lane with the PSP worker's six descriptors) gives
   page requests; the realm's own ordinary request holds the seventh slot
   and its only one. The blocking request still runs, on the slot past each
   cap and the descriptor kept for it; without them it waited out its
   deadline. */
static void test_blocking_request_beside_page_hops(Budget *budget)
{
    enum { PAGE_HOPS = 6 };
    TilefinchTestFaults *faults = tilefinch_test_faults();
    faults->replay_lane_worker_descriptors = true;
    FetchSchedulerDomain *domain = fetch_scheduler_domain_create(
        budget, PAGE_HOPS + 1u);
    FetchScheduler *page = fetch_scheduler_create_in_domain(
        domain, PAGE_HOPS, PAGE_HOPS * 4096u);
    FetchScheduler *realm = fetch_scheduler_create_in_domain(
        domain, 1u, 2u * 4096u);
    fetch_scheduler_domain_release(domain);
    CHECK(page != NULL && realm != NULL
          && fetch_scheduler_enable_background_transport(page, true)
          && fetch_scheduler_enable_background_transport(realm, true));
    if (page == NULL || realm == NULL) {
        fetch_scheduler_destroy(page);
        fetch_scheduler_destroy(realm);
        faults->replay_lane_worker_descriptors = false;
        return;
    }
    FetchTraceReplayStats before, after;
    CHECK(fetch_trace_replay_stats(&before));
    uint64_t hops[PAGE_HOPS] = {0};
    for (unsigned i = 0; i < PAGE_HOPS; i++) {
        hops[i] = fetch_scheduler_enqueue(page, RELEASE_LATEST,
                                          &release_request, 4096u, 30000);
        CHECK(hops[i] != 0);
    }
    uint64_t realm_ordinary = fetch_scheduler_enqueue(
        realm, RELEASE_LATEST, &release_request, 4096u, 30000);
    CHECK(realm_ordinary != 0);
    CHECK(fetch_scheduler_enqueue_would_block(realm, 4096u)
          || fetch_scheduler_enqueue(realm, RELEASE_LATEST, &release_request,
                                     4096u, 30000) == 0);
    CHECK(fetch_trace_replay_stats(&after));
    size_t started = after.media_lane_request_count
        - before.media_lane_request_count;
    fprintf(stderr, "page and realm requests on worker descriptors: %zu of "
            "%u\n", started, PAGE_HOPS + 1u);
    CHECK(started == PAGE_HOPS - 1u);

    FetchResult result = {0};
    uint64_t started_us = tilefinch_platform_monotonic_time_us();
    bool ok = fetch_scheduler_request_blocking(
        realm, RELEASE_LATEST, &release_request, 4096u, 3000, &result);
    uint64_t elapsed_us = tilefinch_platform_monotonic_time_us() - started_us;
    fprintf(stderr, "blocking request beside %u page hops: %s in %llu us "
            "(%s)\n", PAGE_HOPS, ok ? "ok" : "failed",
            (unsigned long long) elapsed_us, result.error);
    CHECK(ok && result.status_code == 200
          && result.length == sizeof(release_body) - 1u
          && memcmp(result.data, release_body, result.length) == 0);
    fetch_result_destroy(&result);
    CHECK(elapsed_us < UINT64_C(1000000));

    /* The slots and the descriptor return; the page's requests, the sixth
       included, finish once their owner pumps again. */
    for (unsigned i = 0; i < PAGE_HOPS; i++)
        CHECK(release_finish(page, hops[i]));
    CHECK(release_finish(realm, realm_ordinary));
    CHECK(fetch_scheduler_pending(page) == 0
          && fetch_scheduler_pending(realm) == 0);
    fetch_scheduler_destroy(page);
    fetch_scheduler_destroy(realm);
    faults->replay_lane_worker_descriptors = false;
}

/* Worker chunks follow the pump's body quota as the browser-thread path
   does (NW-06): with two downloads in flight and a quota of 16 KiB in one
   callback, no pump hands more than that to consumers, a pump that used it
   reports the yield, and both bodies still arrive whole. */
static void test_worker_chunks_follow_pump_quota(Budget *budget)
{
    FetchScheduler *scheduler = fetch_scheduler_create(budget, 2u, 131072u);
    CHECK(scheduler != NULL
          && fetch_scheduler_enable_background_transport(scheduler, true));
    if (scheduler == NULL) return;
    FetchRequest request = {
        .method = "GET",
        .accept = "application/octet-stream",
        .credentials = FETCH_CREDENTIALS_OMIT,
        .identity_encoding = true,
        .redirect_policy = FETCH_REDIRECT_GITHUB_RELEASE,
        .user_agent = "Tilefinch-Updater/1"
    };
    const char *urls[2] = { QUOTA_START_A, QUOTA_START_B };
    uint64_t ids[2];
    for (unsigned i = 0; i < 2u; i++)
        ids[i] = fetch_scheduler_enqueue(scheduler, urls[i], &request,
                                         65536u, 30000);
    CHECK(ids[0] != 0 && ids[1] != 0);
    const FetchPumpQuota quota = {
        .maximum_body_callbacks = 1u, .maximum_body_bytes = 16u * 1024u
    };
    FetchResult *results[2] = {
        fetch_result_create(budget), fetch_result_create(budget)
    };
    bool taken[2] = {false, false}, success[2] = {false, false};
    size_t most_callbacks = 0, most_bytes = 0, body_pumps = 0;
    bool unyielded = false;
    for (unsigned pump = 0; pump < 256u && !(taken[0] && taken[1]);
         pump++) {
        FetchPumpMetrics metrics;
        (void) fetch_scheduler_pump_bounded(scheduler, 2u, 0u, &quota,
                                            &metrics);
        if (metrics.body_callbacks > most_callbacks)
            most_callbacks = metrics.body_callbacks;
        if (metrics.body_bytes > most_bytes) most_bytes = metrics.body_bytes;
        if (metrics.body_bytes != 0) body_pumps++;
        for (unsigned i = 0; i < 2u; i++) {
            if (!taken[i] && results[i] != NULL)
                taken[i] = fetch_scheduler_take(scheduler, ids[i],
                                                &success[i], results[i]);
        }
        /* A pump that spent its callback with work left reports it. */
        if (metrics.body_callbacks >= quota.maximum_body_callbacks
            && !(taken[0] && taken[1]) && !metrics.quota_yielded)
            unyielded = true;
    }
    if (most_callbacks > 1u || most_bytes > quota.maximum_body_bytes
        || unyielded)
        fprintf(stderr, "worker pump quota: callbacks=%zu bytes=%zu "
                "pumps=%zu unyielded=%d\n", most_callbacks, most_bytes,
                body_pumps, unyielded);
    CHECK(most_callbacks == 1u && most_bytes <= quota.maximum_body_bytes
          && !unyielded);
    CHECK(body_pumps >= (2u * QUOTA_BYTES) / quota.maximum_body_bytes);
    for (unsigned i = 0; i < 2u; i++) {
        CHECK(taken[i] && success[i] && results[i] != NULL
              && results[i]->length == QUOTA_BYTES
              && memcmp(results[i]->data, quota_body, QUOTA_BYTES) == 0);
        if (results[i] != NULL) {
            fetch_result_destroy(results[i]);
            fetch_result_free(results[i]);
        }
    }
    fetch_scheduler_destroy(scheduler);
}

/* A stream consumer that holds at most 4 KiB until the test empties it,
   and fails a chunk that does not fit, as an EventSource does. */
typedef struct {
    unsigned char held[4096];
    size_t held_length;
    unsigned char copied[QUOTA_BYTES];
    size_t copied_length;
    size_t full_reports;
    bool overflowed;
} SmallConsumer;

static bool small_consumer_headers(void *opaque, const FetchResult *metadata)
{
    (void) opaque;
    return metadata != NULL && metadata->status_code == 200;
}

static bool small_consumer_body(void *opaque, const unsigned char *data,
                                size_t length)
{
    SmallConsumer *consumer = opaque;
    if (length > sizeof(consumer->held) - consumer->held_length) {
        consumer->overflowed = true;
        return false;
    }
    memcpy(consumer->held + consumer->held_length, data, length);
    consumer->held_length += length;
    return true;
}

static size_t small_consumer_capacity(void *opaque)
{
    SmallConsumer *consumer = opaque;
    size_t room = sizeof(consumer->held) - consumer->held_length;
    if (room == 0) consumer->full_reports++;
    return room;
}

static void small_consumer_drain(SmallConsumer *consumer)
{
    size_t room = sizeof(consumer->copied) - consumer->copied_length;
    size_t length = consumer->held_length < room
        ? consumer->held_length : room;
    memcpy(consumer->copied + consumer->copied_length, consumer->held,
           length);
    consumer->copied_length += length;
    consumer->held_length = 0;
}

/* A worker stream hands its consumer no more than on_capacity reports and
   keeps the rest for a later pump (NW-09): the consumer is emptied only
   every third pump, never overflows, and gets the whole body in order. */
static void test_worker_stream_waits_for_consumer(Budget *budget)
{
    FetchScheduler *scheduler = fetch_scheduler_create(budget, 1u, 131072u);
    CHECK(scheduler != NULL
          && fetch_scheduler_enable_background_transport(scheduler, true));
    if (scheduler == NULL) return;
    static SmallConsumer consumer;
    memset(&consumer, 0, sizeof(consumer));
    FetchStreamOptions stream = {
        .on_headers = small_consumer_headers,
        .on_body = small_consumer_body,
        .on_capacity = small_consumer_capacity,
        .opaque = &consumer
    };
    FetchRequest request = {
        .method = "GET",
        .accept = "application/octet-stream",
        .credentials = FETCH_CREDENTIALS_OMIT,
        .identity_encoding = true,
        .redirect_policy = FETCH_REDIRECT_GITHUB_RELEASE,
        .user_agent = "Tilefinch-Updater/1"
    };
    FetchTraceReplayStats before, after;
    CHECK(fetch_trace_replay_stats(&before));
    uint64_t id = fetch_scheduler_enqueue_stream(
        scheduler, CAPACITY_START, &request, 65536u, 30000, &stream);
    CHECK(id != 0);
    FetchResult *result = fetch_result_create(budget);
    bool taken = false, success = false;
    FetchStreamMetrics metrics;
    for (unsigned pump = 0; id != 0 && result != NULL && pump < 512u
         && !taken; pump++) {
        (void) fetch_scheduler_pump(scheduler, 1u, 0u);
        if (pump % 3u == 2u) small_consumer_drain(&consumer);
        taken = fetch_scheduler_take_stream(scheduler, id, &success,
                                            &metrics, result);
    }
    small_consumer_drain(&consumer);
    CHECK(fetch_trace_replay_stats(&after));
    if (!taken || !success || consumer.overflowed
        || consumer.copied_length != QUOTA_BYTES)
        fprintf(stderr, "worker stream capacity: taken=%d success=%d "
                "overflowed=%d bytes=%zu full=%zu error=%s\n", taken,
                success, consumer.overflowed, consumer.copied_length,
                consumer.full_reports, result == NULL ? "" : result->error);
    CHECK(taken && success && !consumer.overflowed);
    CHECK(consumer.full_reports != 0);
    CHECK(consumer.copied_length == QUOTA_BYTES
          && memcmp(consumer.copied, capacity_body, QUOTA_BYTES) == 0);
    /* Both hops went through the worker lane. */
    CHECK(after.media_lane_served_count
          == before.media_lane_served_count + 2u);
    if (result != NULL) {
        fetch_result_destroy(result);
        fetch_result_free(result);
    }
    fetch_scheduler_destroy(scheduler);
}

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
    test_release_redirect_chain(&budget);
    test_worker_attach_is_retried(&budget);
    test_blocking_request_beside_page_hops(&budget);
    test_worker_chunks_follow_pump_quota(&budget);
    test_worker_stream_waits_for_consumer(&budget);

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
