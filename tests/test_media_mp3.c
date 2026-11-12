#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tilefinch/budget.h"
#include "tilefinch/media_backend.h"
#include "tilefinch/media_mp3.h"

#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, \
                #condition);                                                 \
        return 1;                                                            \
    }                                                                        \
} while (0)

#ifndef TILEFINCH_MP3_FIXTURE_DIR
#error "TILEFINCH_MP3_FIXTURE_DIR must name tests/fixtures/media-mp3"
#endif

/* A scripted response transport over one in-memory resource. */
typedef struct {
    const unsigned char *bytes;
    size_t length;
    size_t position;
    size_t chunk;              /* bytes per poll */
    unsigned wait_every;       /* a WAIT after this many chunks (0: never) */
    unsigned polls;
    unsigned waits_after_start; /* WAITs before each response's first byte */
    uint64_t declared;         /* each response's Content-Length (0: none) */
    unsigned start_waits;
    bool ranges;               /* start(offset != 0) allowed */
    size_t drop_at;            /* ERROR once position reaches it (0: never) */
    uint64_t handle;
    unsigned starts;
    uint64_t last_offset;
    /* Endless mode: generated CBR frames, never COMPLETE. */
    bool endless;
    unsigned char frame[1441];
    size_t frame_bytes;
    uint64_t generated;
} ScriptTransport;

static uint64_t script_start(void *opaque, uint64_t offset, char *error,
                             size_t error_size)
{
    ScriptTransport *script = opaque;
    if (offset != 0 && !script->ranges) {
        snprintf(error, error_size, "%s", "ranges refused");
        return 0;
    }
    if (!script->endless && offset > script->length) {
        snprintf(error, error_size, "%s", "range past end");
        return 0;
    }
    script->position = (size_t) offset;
    script->start_waits = script->waits_after_start;
    script->starts++;
    script->last_offset = offset;
    script->handle++;
    return script->handle;
}

static MediaHlsTransportPollResult script_poll(
    void *opaque, uint64_t handle, unsigned char *destination,
    size_t capacity, size_t *length, char *error, size_t error_size)
{
    ScriptTransport *script = opaque;
    *length = 0;
    if (handle != script->handle) {
        snprintf(error, error_size, "%s", "stale handle");
        return MEDIA_HLS_TRANSPORT_ERROR;
    }
    script->polls++;
    if (script->start_waits != 0) {
        /* Headers have not arrived yet, as on a real connection. */
        script->start_waits--;
        return MEDIA_HLS_TRANSPORT_WAIT;
    }
    if (script->wait_every != 0 && script->polls % script->wait_every == 0)
        return MEDIA_HLS_TRANSPORT_WAIT;
    if (script->drop_at != 0 && script->position >= script->drop_at) {
        script->drop_at = 0;
        snprintf(error, error_size, "%s", "connection reset");
        return MEDIA_HLS_TRANSPORT_ERROR;
    }
    size_t want = script->chunk < capacity ? script->chunk : capacity;
    if (script->drop_at != 0 && script->position + want > script->drop_at)
        want = script->drop_at - script->position;
    if (script->endless) {
        for (size_t i = 0; i < want; i++) {
            destination[i] =
                script->frame[script->generated % script->frame_bytes];
            script->generated++;
        }
        script->position += want;
        *length = want;
        return MEDIA_HLS_TRANSPORT_CHUNK;
    }
    if (script->position >= script->length) return MEDIA_HLS_TRANSPORT_COMPLETE;
    size_t left = script->length - script->position;
    if (want > left) want = left;
    memcpy(destination, script->bytes + script->position, want);
    script->position += want;
    *length = want;
    return MEDIA_HLS_TRANSPORT_CHUNK;
}

static void script_cancel(void *opaque, uint64_t handle)
{
    ScriptTransport *script = opaque;
    if (handle == script->handle) script->handle++;
}

static uint64_t script_declared_length(void *opaque, uint64_t handle)
{
    ScriptTransport *script = opaque;
    return handle == script->handle ? script->declared : 0;
}

static MediaMp3Transport script_transport(ScriptTransport *script)
{
    return (MediaMp3Transport) {
        .opaque = script,
        .start = script_start,
        .poll = script_poll,
        .cancel = script_cancel,
        .declared_length = script_declared_length
    };
}

static void make_header(unsigned char out[4], unsigned version_bits,
                        unsigned bitrate_index, unsigned rate_index,
                        bool padding, unsigned mode)
{
    out[0] = 0xffu;
    out[1] = (unsigned char) (0xe0u | (version_bits << 3) | (1u << 1) | 1u);
    out[2] = (unsigned char) ((bitrate_index << 4) | (rate_index << 2)
                              | (padding ? 2u : 0u));
    out[3] = (unsigned char) (mode << 6);
}

/* Appends one synthetic frame (header plus a filler payload free of sync
   bytes) and returns its length. */
static size_t append_frame(unsigned char *out, unsigned version_bits,
                           unsigned bitrate_index, unsigned rate_index,
                           bool padding, unsigned mode)
{
    MediaMp3FrameHeader header;
    make_header(out, version_bits, bitrate_index, rate_index, padding, mode);
    if (!media_mp3_parse_header(out, &header)) return 0;
    for (size_t i = 4; i < header.frame_bytes; i++)
        out[i] = (unsigned char) (0x11u + i % 0x60u);
    return header.frame_bytes;
}

static unsigned char *read_fixture(const char *name, size_t *length)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", TILEFINCH_MP3_FIXTURE_DIR, name);
    FILE *file = fopen(path, "rb");
    if (file == NULL) return NULL;
    unsigned char *bytes = malloc(64u * 1024u);
    *length = bytes == NULL ? 0 : fread(bytes, 1, 64u * 1024u, file);
    fclose(file);
    return bytes;
}

typedef struct {
    uint64_t frames;
    uint64_t last_pts;
    uint64_t duration_sum;
    bool failed;
    char error[160];
} DrainResult;

/* Drains a source as the player does: pump, peek, read. */
static int drain(MediaMp3Source *source, uint64_t frame_limit,
                 DrainResult *result)
{
    memset(result, 0, sizeof(*result));
    MediaSampleSource samples;
    if (!media_mp3_source_sample_source(source, &samples)) return 1;
    uint64_t now = 0;
    for (unsigned spins = 0; spins < 2000000u; spins++) {
        if (result->frames >= frame_limit) return 0;
        media_mp3_source_pump(source, now);
        now += 10000u;
        MediaMp4Sample sample;
        if (!samples.ops->next_sample(samples.opaque, &sample)) {
            if (samples.ops->would_block(samples.opaque)) continue;
            result->failed = samples.ops->last_error(
                samples.opaque, result->error, sizeof(result->error));
            return 0;
        }
        unsigned char packet[MEDIA_MP3_MAXIMUM_FRAME_BYTES];
        if (sample.size < 4u || sample.size > sizeof(packet)) return 1;
        if (!samples.ops->sample_resident(samples.opaque, &sample)) return 1;
        if (!samples.ops->read_sample(samples.opaque, &sample, packet,
                                      sizeof(packet)))
            return 1;
        MediaMp3FrameHeader header;
        if (!media_mp3_parse_header(packet, &header)) return 1;
        if (header.frame_bytes != sample.size) return 1;
        if (result->frames != 0 && sample.dts != result->duration_sum)
            return 1;
        result->last_pts = sample.dts;
        result->duration_sum = sample.dts + sample.duration;
        result->frames++;
    }
    return 1;
}

static int test_header_parsing(void)
{
    MediaMp3FrameHeader header;
    /* MPEG-1 Layer III, 128 kbit/s, 44.1 kHz, joint stereo, no CRC. */
    static const unsigned char mpeg1[4] = {0xff, 0xfb, 0x90, 0x44};
    CHECK(media_mp3_parse_header(mpeg1, &header));
    CHECK(header.version == MEDIA_MP3_VERSION_1);
    CHECK(header.sample_rate == 44100u && header.bitrate == 128000u);
    CHECK(header.frame_bytes == 417u && header.samples == 1152u);
    CHECK(header.channels == 2u && !header.crc && !header.padding);
    /* Padded. */
    static const unsigned char padded[4] = {0xff, 0xfb, 0x92, 0x44};
    CHECK(media_mp3_parse_header(padded, &header) && header.frame_bytes == 418u);
    /* MPEG-2, 64 kbit/s, 22.05 kHz mono, with CRC. */
    static const unsigned char mpeg2[4] = {0xff, 0xf2, 0x80, 0xc0};
    CHECK(media_mp3_parse_header(mpeg2, &header));
    CHECK(header.version == MEDIA_MP3_VERSION_2 && header.crc);
    CHECK(header.sample_rate == 22050u && header.channels == 1u);
    CHECK(header.samples == 576u && header.frame_bytes == 208u);
    /* MPEG-2.5, 8 kbit/s, 8 kHz. */
    unsigned char bytes[4];
    make_header(bytes, 0u, 1u, 2u, false, 3u);
    CHECK(media_mp3_parse_header(bytes, &header));
    CHECK(header.version == MEDIA_MP3_VERSION_2_5);
    CHECK(header.sample_rate == 8000u && header.frame_bytes == 72u);
    /* The largest frame: MPEG-1 320 kbit/s at 32 kHz, padded. */
    make_header(bytes, 3u, 14u, 2u, true, 0u);
    CHECK(media_mp3_parse_header(bytes, &header));
    CHECK(header.frame_bytes == MEDIA_MP3_MAXIMUM_FRAME_BYTES);
    /* Refused: Layer II, reserved version, free format, bad bitrate,
       reserved rate, reserved emphasis, broken sync. */
    static const unsigned char layer2[4] = {0xff, 0xfd, 0x90, 0x44};
    static const unsigned char reserved_version[4] = {0xff, 0xeb, 0x90, 0x44};
    static const unsigned char free_format[4] = {0xff, 0xfb, 0x00, 0x44};
    static const unsigned char bad_bitrate[4] = {0xff, 0xfb, 0xf0, 0x44};
    static const unsigned char bad_rate[4] = {0xff, 0xfb, 0x9c, 0x44};
    static const unsigned char bad_emphasis[4] = {0xff, 0xfb, 0x90, 0x46};
    static const unsigned char no_sync[4] = {0xff, 0x7b, 0x90, 0x44};
    CHECK(!media_mp3_parse_header(layer2, &header));
    CHECK(!media_mp3_parse_header(reserved_version, &header));
    CHECK(!media_mp3_parse_header(free_format, &header));
    CHECK(!media_mp3_parse_header(bad_bitrate, &header));
    CHECK(!media_mp3_parse_header(bad_rate, &header));
    CHECK(!media_mp3_parse_header(bad_emphasis, &header));
    CHECK(!media_mp3_parse_header(no_sync, &header));
    /* Every legal header yields a frame that fits the packet bound. */
    for (unsigned version = 0; version < 4u; version++) {
        for (unsigned bitrate = 0; bitrate < 16u; bitrate++) {
            for (unsigned rate = 0; rate < 4u; rate++) {
                make_header(bytes, version, bitrate, rate, true, 1u);
                if (media_mp3_parse_header(bytes, &header))
                    CHECK(header.frame_bytes <= MEDIA_MP3_MAXIMUM_FRAME_BYTES);
            }
        }
    }
    return 0;
}

static int test_id3_and_sniff(void)
{
    unsigned char tag[16] = {'I', 'D', '3', 4, 0, 0, 0, 0, 0x02, 0x01};
    CHECK(media_mp3_id3v2_length(tag, sizeof(tag)) == 10u + 257u);
    tag[5] = 0x10u; /* footer present */
    CHECK(media_mp3_id3v2_length(tag, sizeof(tag)) == 20u + 257u);
    tag[8] = 0x80u; /* not synchsafe */
    CHECK(media_mp3_id3v2_length(tag, sizeof(tag)) == 0);
    CHECK(media_mp3_id3v2_length(tag, 9u) == 0);
    CHECK(media_mp3_content_type("audio/mpeg"));
    CHECK(media_mp3_content_type("Audio/MPEG; charset=binary"));
    CHECK(media_mp3_content_type("audio/mp3"));
    CHECK(!media_mp3_content_type("audio/mpegurl"));
    CHECK(!media_mp3_content_type("audio/aacp"));
    CHECK(!media_mp3_content_type("video/mp4"));
    CHECK(!media_mp3_content_type(NULL));

    unsigned char frames[3000];
    size_t first = append_frame(frames, 3u, 9u, 0u, false, 1u);
    size_t second = append_frame(frames + first, 3u, 9u, 0u, true, 1u);
    CHECK(first == 417u && second == 418u);
    CHECK(media_mp3_sniff(frames, first + second));
    CHECK(!media_mp3_sniff(frames, first + 2u));
    CHECK(media_mp3_sniff(tag, sizeof(tag)) == false);
    tag[8] = 0;
    CHECK(media_mp3_sniff(tag, sizeof(tag)));
    static const unsigned char html[] = "<!doctype html><html>";
    CHECK(!media_mp3_sniff(html, sizeof(html) - 1u));
    return 0;
}

typedef struct {
    const char *name;
    uint32_t rate;
    uint16_t channels;
    uint64_t frames;   /* audio frames as FFmpeg counts them */
    bool vbr;
    unsigned id3;
} FixtureCase;

static int test_fixtures(void)
{
    static const FixtureCase cases[] = {
        {"cbr-44100-stereo.mp3", 44100u, 2u, 40u, false, 0u},
        {"cbr-32000-stereo.mp3", 32000u, 2u, 29u, false, 0u},
        {"cbr-22050-mono-mpeg2.mp3", 22050u, 1u, 41u, false, 0u},
        {"vbr-48000-mono-id3.mp3", 48000u, 1u, 43u, true, 1u}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        size_t length = 0;
        unsigned char *bytes = read_fixture(cases[i].name, &length);
        CHECK(bytes != NULL && length > 1000u);
        for (size_t chunk = 1; chunk <= 8192u; chunk *= 8u) {
            Budget budget;
            budget_init(&budget, 1024u * 1024u);
            ScriptTransport script = {
                .bytes = bytes, .length = length, .chunk = chunk,
                .wait_every = 3u, .ranges = true
            };
            MediaMp3Transport transport = script_transport(&script);
            MediaMp3SourceOptions options = {
                .content_length = length, .seekable = true
            };
            char error[160];
            MediaMp3Source *source = media_mp3_source_create(
                &budget, &transport, &options, error, sizeof(error));
            CHECK(source != NULL);
            MediaHlsPrimeStatus prime = MEDIA_HLS_PRIME_PENDING;
            for (unsigned spin = 0; spin < 100000u
                     && prime == MEDIA_HLS_PRIME_PENDING; spin++) {
                media_mp3_source_pump(source, spin);
                prime = media_mp3_source_prime(source, error, sizeof(error));
            }
            CHECK(prime == MEDIA_HLS_PRIME_READY);
            MediaMp4TrackInfo info;
            CHECK(media_mp3_source_track_info(source, &info));
            CHECK(info.codec == MEDIA_MP3_FOURCC);
            CHECK(info.kind == MEDIA_MP4_TRACK_AUDIO);
            CHECK(info.sample_rate == cases[i].rate);
            CHECK(info.timescale == cases[i].rate);
            CHECK(info.channels == cases[i].channels);
            CHECK(info.duration != 0);
            DrainResult result;
            CHECK(drain(source, UINT64_MAX, &result) == 0);
            if (result.failed) fprintf(stderr, "%s: %s\n", cases[i].name,
                                       result.error);
            CHECK(!result.failed);
            CHECK(result.frames == cases[i].frames);
            MediaMp3Stats stats;
            media_mp3_source_stats(source, &stats);
            CHECK(stats.ended && !stats.live);
            CHECK(stats.vbr == cases[i].vbr);
            CHECK(stats.id3_tags == cases[i].id3);
            CHECK(stats.sync_losses == 0);
            uint32_t frame_samples = cases[i].rate >= 32000u ? 1152u : 576u;
            CHECK(result.duration_sum == cases[i].frames * frame_samples);
            if (cases[i].vbr) CHECK(info.duration == result.duration_sum);
            media_mp3_source_destroy(source);
            CHECK(budget.current == 0);
        }
        free(bytes);
    }
    return 0;
}

static int test_resync_and_damage(void)
{
    /* Ten frames, then garbage holding false sync bytes, then ten more. */
    unsigned char stream[16384];
    size_t at = 0;
    for (int i = 0; i < 10; i++)
        at += append_frame(stream + at, 3u, 9u, 0u, i % 3 == 0, 1u);
    static const unsigned char noise[] = {
        0xff, 0xfb, 0x90, 0x00, 0x13, 0xff, 0xe0, 0xff, 0xff, 0xfb, 0x12,
        0x77, 0xff, 0xfd, 0x90, 0x44, 0x00, 0x01
    };
    memcpy(stream + at, noise, sizeof(noise));
    at += sizeof(noise);
    for (int i = 0; i < 10; i++)
        at += append_frame(stream + at, 3u, 9u, 0u, false, 1u);
    /* An ID3v1 trailer. */
    memcpy(stream + at, "TAG", 3u);
    memset(stream + at + 3u, ' ', 125u);
    at += 128u;

    Budget budget;
    budget_init(&budget, 1024u * 1024u);
    ScriptTransport script = {.bytes = stream, .length = at, .chunk = 700u};
    MediaMp3Transport transport = script_transport(&script);
    MediaMp3SourceOptions options = {.content_length = at};
    MediaMp3Source *source = media_mp3_source_create(
        &budget, &transport, &options, NULL, 0);
    CHECK(source != NULL);
    DrainResult result;
    CHECK(drain(source, UINT64_MAX, &result) == 0);
    CHECK(!result.failed);
    CHECK(result.frames == 20u);
    MediaMp3Stats stats;
    media_mp3_source_stats(source, &stats);
    CHECK(stats.sync_losses >= 1u && stats.skipped_bytes >= sizeof(noise));
    CHECK(stats.ended);
    media_mp3_source_destroy(source);

    /* Truncated mid-frame: every whole frame plays, then a clean end. */
    size_t truncated = 417u * 3u + 100u;
    script = (ScriptTransport) {.bytes = stream, .length = truncated,
                                .chunk = 4096u};
    transport = script_transport(&script);
    options.content_length = truncated;
    source = media_mp3_source_create(&budget, &transport, &options, NULL, 0);
    CHECK(source != NULL);
    CHECK(drain(source, UINT64_MAX, &result) == 0);
    CHECK(!result.failed && result.frames == 3u);
    media_mp3_source_destroy(source);

    /* Garbage: a bounded scan, then a failure naming the problem. */
    unsigned char *garbage = malloc(200u * 1024u);
    CHECK(garbage != NULL);
    uint32_t seed = 12345u;
    for (size_t i = 0; i < 200u * 1024u; i++) {
        seed = seed * 1103515245u + 12345u;
        garbage[i] = (unsigned char) (seed >> 16);
    }
    script = (ScriptTransport) {.bytes = garbage, .length = 200u * 1024u,
                                .chunk = 4096u};
    transport = script_transport(&script);
    options.content_length = 200u * 1024u;
    source = media_mp3_source_create(&budget, &transport, &options, NULL, 0);
    CHECK(source != NULL);
    char error[160];
    MediaHlsPrimeStatus prime = MEDIA_HLS_PRIME_PENDING;
    for (unsigned spin = 0; spin < 10000u && prime == MEDIA_HLS_PRIME_PENDING;
         spin++) {
        media_mp3_source_pump(source, spin);
        prime = media_mp3_source_prime(source, error, sizeof(error));
    }
    CHECK(prime == MEDIA_HLS_PRIME_FAILED);
    CHECK(strstr(error, "not an MP3") != NULL);
    media_mp3_source_stats(source, &stats);
    CHECK(stats.skipped_bytes <= MEDIA_MP3_RESYNC_LIMIT_BYTES + 4096u);
    media_mp3_source_destroy(source);

    /* An HTML error page is refused before any frame. */
    static const char page[] = "<html><body>Station offline</body></html>";
    script = (ScriptTransport) {.bytes = (const unsigned char *) page,
                                .length = sizeof(page) - 1u, .chunk = 64u};
    transport = script_transport(&script);
    options.content_length = sizeof(page) - 1u;
    source = media_mp3_source_create(&budget, &transport, &options, NULL, 0);
    prime = MEDIA_HLS_PRIME_PENDING;
    for (unsigned spin = 0; spin < 100u && prime == MEDIA_HLS_PRIME_PENDING;
         spin++) {
        media_mp3_source_pump(source, spin);
        prime = media_mp3_source_prime(source, error, sizeof(error));
    }
    CHECK(prime == MEDIA_HLS_PRIME_FAILED);
    media_mp3_source_destroy(source);
    free(garbage);
    CHECK(budget.current == 0);
    return 0;
}

static int test_large_id3_streamed_and_ranged(void)
{
    /* A 200 KiB tag: larger than the ring. */
    size_t tag = 200u * 1024u;
    size_t total = tag + 10u * 418u;
    unsigned char *bytes = calloc(1, total);
    CHECK(bytes != NULL);
    size_t body = tag - 10u;
    unsigned char header[10] = {'I', 'D', '3', 3, 0, 0,
        (unsigned char) ((body >> 21) & 0x7fu),
        (unsigned char) ((body >> 14) & 0x7fu),
        (unsigned char) ((body >> 7) & 0x7fu),
        (unsigned char) (body & 0x7fu)};
    memcpy(bytes, header, sizeof(header));
    /* Tag body full of sync-looking bytes that must not be parsed. */
    for (size_t i = 10; i < tag; i++) bytes[i] = (i % 7u) == 0 ? 0xffu : 0xfbu;
    size_t at = tag;
    for (int i = 0; i < 10; i++)
        at += append_frame(bytes + at, 3u, 9u, 0u, true, 1u);
    for (int ranged = 0; ranged < 2; ranged++) {
        Budget budget;
        budget_init(&budget, 1024u * 1024u);
        ScriptTransport script = {
            .bytes = bytes, .length = at, .chunk = 8192u,
            .ranges = ranged != 0
        };
        MediaMp3Transport transport = script_transport(&script);
        MediaMp3SourceOptions options = {
            .content_length = at, .seekable = ranged != 0
        };
        MediaMp3Source *source = media_mp3_source_create(
            &budget, &transport, &options, NULL, 0);
        CHECK(source != NULL);
        DrainResult result;
        CHECK(drain(source, UINT64_MAX, &result) == 0);
        CHECK(!result.failed && result.frames == 10u);
        MediaMp3Stats stats;
        media_mp3_source_stats(source, &stats);
        CHECK(stats.id3_tags == 1u && stats.skipped_bytes == 0);
        if (ranged) {
            /* The tag was jumped with a range request, not downloaded. */
            CHECK(script.starts == 2u && script.last_offset == tag);
            CHECK(stats.bytes_received < tag);
        } else {
            CHECK(script.starts == 1u);
        }
        media_mp3_source_destroy(source);
        CHECK(budget.current == 0);
    }
    free(bytes);
    return 0;
}

/* Writes an ID3v2 header claiming `tag` bytes in all (header included). */
static void write_id3_header(unsigned char out[10], size_t tag)
{
    size_t body = tag - 10u;
    unsigned char header[10] = {'I', 'D', '3', 3, 0, 0,
        (unsigned char) ((body >> 21) & 0x7fu),
        (unsigned char) ((body >> 14) & 0x7fu),
        (unsigned char) ((body >> 7) & 0x7fu),
        (unsigned char) (body & 0x7fu)};
    memcpy(out, header, sizeof(header));
}

/* An ID3 tag that claims more bytes than the response still holds. */
static int test_id3_past_end(void)
{
    size_t total = 64u * 1024u;
    unsigned char *bytes = calloc(1, total);
    CHECK(bytes != NULL);
    size_t at = 0;
    for (int i = 0; i < 10; i++)
        at += append_frame(bytes + at, 3u, 9u, 0u, true, 1u);
    write_id3_header(bytes + at, 200u * 1024u);
    at += 10u;
    memset(bytes + at, 0x22, 1000u);
    at += 1000u;
    Budget budget;
    budget_init(&budget, 1024u * 1024u);
    /* A finite file from a server without ranges: the ten frames play and
       the source ends at the end of the response. It used to stay pending
       with the skip outstanding, so the player buffered forever. */
    ScriptTransport script = {.bytes = bytes, .length = at, .chunk = 700u};
    MediaMp3Transport transport = script_transport(&script);
    MediaMp3SourceOptions options = {.content_length = at};
    MediaMp3Source *source = media_mp3_source_create(
        &budget, &transport, &options, NULL, 0);
    CHECK(source != NULL);
    DrainResult result;
    CHECK(drain(source, UINT64_MAX, &result) == 0);
    CHECK(!result.failed && result.frames == 10u);
    MediaMp3Stats stats;
    media_mp3_source_stats(source, &stats);
    CHECK(stats.ended && stats.id3_tags == 1u);
    CHECK(script.starts == 1u);
    media_mp3_source_destroy(source);

    /* Nothing but such a tag: the open fails instead of waiting. */
    size_t tag_only = 10u + 500u;
    script = (ScriptTransport) {.bytes = bytes + at - tag_only,
                                .length = tag_only, .chunk = 700u};
    transport = script_transport(&script);
    options.content_length = tag_only;
    source = media_mp3_source_create(&budget, &transport, &options, NULL, 0);
    CHECK(source != NULL);
    char error[160] = {0};
    MediaHlsPrimeStatus prime = MEDIA_HLS_PRIME_PENDING;
    for (unsigned spin = 0; spin < 1000u && prime == MEDIA_HLS_PRIME_PENDING;
         spin++) {
        media_mp3_source_pump(source, spin * 10000u);
        prime = media_mp3_source_prime(source, error, sizeof(error));
    }
    CHECK(prime == MEDIA_HLS_PRIME_FAILED);
    CHECK(strstr(error, "no MP3 frames") != NULL);
    media_mp3_source_destroy(source);

    /* A live stream does not believe a tag that claims 100 MiB: the frames
       after the forged header play. Before, the skip swallowed them, and
       survived the reconnect, so the station stayed silent. */
    at = 0;
    for (int i = 0; i < 10; i++)
        at += append_frame(bytes + at, 3u, 9u, 0u, true, 1u);
    write_id3_header(bytes + at, 100u * 1024u * 1024u);
    at += 10u;
    for (int i = 0; i < 30; i++)
        at += append_frame(bytes + at, 3u, 9u, 0u, true, 1u);
    script = (ScriptTransport) {.bytes = bytes, .length = at, .chunk = 700u};
    transport = script_transport(&script);
    options.content_length = 0;
    source = media_mp3_source_create(&budget, &transport, &options, NULL, 0);
    CHECK(source != NULL);
    CHECK(drain(source, 40u, &result) == 0);
    CHECK(!result.failed && result.frames == 40u);
    CHECK(script.starts == 1u);
    media_mp3_source_destroy(source);
    CHECK(budget.current == 0);
    free(bytes);
    return 0;
}

static int test_seek(void)
{
    size_t length = 0;
    unsigned char *bytes = read_fixture("cbr-44100-stereo.mp3", &length);
    CHECK(bytes != NULL);
    Budget budget;
    budget_init(&budget, 1024u * 1024u);
    ScriptTransport script = {
        .bytes = bytes, .length = length, .chunk = 512u, .ranges = true
    };
    MediaMp3Transport transport = script_transport(&script);
    MediaMp3SourceOptions options = {.content_length = length,
                                     .seekable = true};
    MediaMp3Source *source = media_mp3_source_create(
        &budget, &transport, &options, NULL, 0);
    CHECK(source != NULL);
    MediaSampleSource samples;
    CHECK(media_mp3_source_sample_source(source, &samples));
    char error[160];
    MediaHlsPrimeStatus prime = MEDIA_HLS_PRIME_PENDING;
    for (unsigned spin = 0; spin < 1000u && prime == MEDIA_HLS_PRIME_PENDING;
         spin++)
        prime = media_mp3_source_prime(source, error, sizeof(error));
    CHECK(prime == MEDIA_HLS_PRIME_READY);
    /* The player's prime-time proof of zero leaves the response alone. */
    uint64_t actual = 1;
    CHECK(samples.ops->seek_us(samples.opaque, 0, &actual) && actual == 0);
    CHECK(script.starts == 1u);
    DrainResult result;
    CHECK(drain(source, 5u, &result) == 0 && result.frames == 5u);
    /* Half way: a range restart near 0.5 s, then resync. */
    CHECK(samples.ops->seek_us(samples.opaque, 500000u, &actual));
    CHECK(actual >= 490000u && actual <= 510000u);
    MediaMp4Sample sample;
    unsigned spins = 0;
    while (!samples.ops->next_sample(samples.opaque, &sample) && spins < 1000u)
        spins++;
    CHECK(script.starts == 2u);
    CHECK(script.last_offset > length / 3u && script.last_offset < length);
    CHECK(sample.dts >= 21600u && sample.dts <= 22500u);
    CHECK(drain(source, UINT64_MAX, &result) == 0 && !result.failed);
    CHECK(result.frames > 10u && result.frames < 30u);
    /* Back to the start. */
    CHECK(samples.ops->seek_us(samples.opaque, 0, &actual) && actual == 0);
    CHECK(drain(source, UINT64_MAX, &result) == 0 && result.frames == 40u);
    media_mp3_source_destroy(source);

    /* No ranges: only the untouched zero seek succeeds. */
    script = (ScriptTransport) {.bytes = bytes, .length = length,
                                .chunk = 512u};
    transport = script_transport(&script);
    options.seekable = false;
    source = media_mp3_source_create(&budget, &transport, &options, NULL, 0);
    CHECK(media_mp3_source_sample_source(source, &samples));
    for (unsigned spin = 0; spin < 1000u
             && media_mp3_source_prime(source, error, sizeof(error))
                    == MEDIA_HLS_PRIME_PENDING; spin++) {}
    CHECK(samples.ops->seek_us(samples.opaque, 0, &actual));
    CHECK(drain(source, 2u, &result) == 0);
    CHECK(!samples.ops->seek_us(samples.opaque, 500000u, &actual));
    CHECK(!samples.ops->seek_us(samples.opaque, 0, &actual));
    media_mp3_source_destroy(source);
    CHECK(budget.current == 0);
    free(bytes);
    return 0;
}

static MediaBackendResult backend_accept(
    void *opaque, const MediaMp4Sample *sample, const unsigned char *payload,
    size_t length, char *error, size_t error_size)
{
    (void) opaque; (void) sample; (void) payload; (void) length;
    (void) error; (void) error_size;
    return MEDIA_BACKEND_ACCEPTED;
}

static bool backend_advance(void *opaque, uint64_t clock_us, char *error,
                            size_t error_size)
{
    (void) opaque; (void) clock_us; (void) error; (void) error_size;
    return true;
}

static void backend_destroy(void *opaque) { (void) opaque; }

/* The player's seek prime (playback_prime_source) proves the target with
   seek_us up to three times a poll, and a real response delivers nothing
   on its first polls. Each proof used to restart the request, so a seek in
   a seekable MP3 cancelled its own response every poll and never primed. */
static int test_seek_prime_over_slow_response(void)
{
    size_t length = 0;
    unsigned char *bytes = read_fixture("cbr-44100-stereo.mp3", &length);
    CHECK(bytes != NULL);
    Budget budget;
    budget_init(&budget, 2u * 1024u * 1024u);
    ScriptTransport script = {
        .bytes = bytes, .length = length, .chunk = 512u, .ranges = true,
        .waits_after_start = 2u
    };
    MediaMp3Transport transport = script_transport(&script);
    MediaMp3SourceOptions options = {.content_length = length,
                                     .seekable = true};
    MediaMp3Source *source = media_mp3_source_create(
        &budget, &transport, &options, NULL, 0);
    CHECK(source != NULL);
    char error[256] = {0};
    MediaHlsPrimeStatus prime = MEDIA_HLS_PRIME_PENDING;
    for (unsigned spin = 0; spin < 1000u && prime == MEDIA_HLS_PRIME_PENDING;
         spin++) {
        media_mp3_source_pump(source, spin * 16000u);
        prime = media_mp3_source_prime(source, error, sizeof(error));
    }
    CHECK(prime == MEDIA_HLS_PRIME_READY);
    MediaSampleSource samples;
    CHECK(media_mp3_source_sample_source(source, &samples));
    MediaBackend backend = {
        .submit = backend_accept, .advance = backend_advance,
        .destroy = backend_destroy
    };
    MediaPlayback *playback = media_playback_create_sources(
        &budget, &samples, NULL, &backend, NULL, error, sizeof(error));
    CHECK(playback != NULL);
    unsigned starts = script.starts;
    MediaPlaybackSourcePrimeStatus status = MEDIA_PLAYBACK_SOURCE_PRIME_PENDING;
    uint64_t primed_us = 0;
    unsigned polls = 0;
    for (; polls < 64u && status == MEDIA_PLAYBACK_SOURCE_PRIME_PENDING;
         polls++) {
        media_mp3_source_pump(source, UINT64_C(20000000) + polls * 16000u);
        status = media_playback_prime_audio_source(
            playback, 500000u, &primed_us, error, sizeof(error));
    }
    if (status != MEDIA_PLAYBACK_SOURCE_PRIME_READY)
        fprintf(stderr, "seek prime: status %d after %u polls, %u starts: %s\n",
                (int) status, polls, script.starts - starts, error);
    CHECK(status == MEDIA_PLAYBACK_SOURCE_PRIME_READY);
    /* One range request for the seek, not one per poll. */
    CHECK(script.starts == starts + 1u);
    CHECK(script.last_offset > length / 3u);
    CHECK(primed_us >= 490000u && primed_us <= 510000u);

    /* Zero right after an untouched seek elsewhere is a real seek: it used
       to return at once and leave the audio at the earlier target. */
    uint64_t actual = 1;
    CHECK(samples.ops->seek_us(samples.opaque, 0, &actual) && actual == 0);
    MediaMp4Sample sample = {0};
    bool found = false;
    for (unsigned spin = 0; spin < 1000u && !found; spin++) {
        media_mp3_source_pump(source, UINT64_C(30000000) + spin * 16000u);
        found = samples.ops->next_sample(samples.opaque, &sample);
    }
    CHECK(found && sample.dts == 0);
    CHECK(script.starts == starts + 2u && script.last_offset < length / 3u);
    media_playback_destroy(playback);
    media_mp3_source_destroy(source);
    CHECK(budget.current == 0);
    free(bytes);
    return 0;
}

static int test_endless_stream(void)
{
    Budget budget;
    budget_init(&budget, 1024u * 1024u);
    ScriptTransport script = {.endless = true, .chunk = 1500u};
    script.frame_bytes = append_frame(script.frame, 3u, 9u, 0u, false, 0u);
    CHECK(script.frame_bytes == 417u);
    MediaMp3Transport transport = script_transport(&script);
    MediaMp3SourceOptions options = {0};
    MediaMp3Source *source = media_mp3_source_create(
        &budget, &transport, &options, NULL, 0);
    CHECK(source != NULL);
    size_t used = budget.current;
    MediaSampleSource samples;
    CHECK(media_mp3_source_sample_source(source, &samples));
    size_t retained = samples.ops->retained_bytes(samples.opaque);
    /* About ten minutes of a 128 kbit/s station. */
    DrainResult result;
    CHECK(drain(source, 23000u, &result) == 0);
    CHECK(!result.failed && result.frames == 23000u);
    MediaMp3Stats stats;
    media_mp3_source_stats(source, &stats);
    CHECK(stats.live && !stats.ended);
    CHECK(stats.buffered_bytes <= MEDIA_MP3_RING_BYTES);
    CHECK(stats.bytes_received >= 23000u * 417u);
    CHECK(budget.current == used);
    CHECK(samples.ops->retained_bytes(samples.opaque) == retained);
    MediaMp4TrackInfo info;
    CHECK(media_mp3_source_track_info(source, &info) && info.duration == 0);
    /* A live stream cannot seek away from its untouched start. */
    uint64_t actual;
    CHECK(!samples.ops->seek_us(samples.opaque, 1000000u, &actual));
    media_mp3_source_destroy(source);
    CHECK(script.handle != 0);
    CHECK(budget.current == 0);
    return 0;
}

static int test_live_reconnect(void)
{
    Budget budget;
    budget_init(&budget, 1024u * 1024u);
    /* The connection drops mid-frame; the reconnect starts a fresh response
       whose frames must not be glued onto the torn one. */
    ScriptTransport script = {.endless = true, .chunk = 1000u,
                              .drop_at = 417u * 20u + 200u};
    script.frame_bytes = append_frame(script.frame, 3u, 9u, 0u, false, 0u);
    MediaMp3Transport transport = script_transport(&script);
    MediaMp3SourceOptions options = {0};
    MediaMp3Source *source = media_mp3_source_create(
        &budget, &transport, &options, NULL, 0);
    CHECK(source != NULL);
    DrainResult result;
    MediaMp3Stats stats;
    /* The player names the gap: reconnecting from the break until the new
       response's first bytes, and never again once audio flows. */
    bool saw_reconnecting = false;
    for (unsigned frame = 0; frame < 60u; frame++) {
        CHECK(drain(source, 1u, &result) == 0);
        CHECK(!result.failed && result.frames == 1u);
        media_mp3_source_stats(source, &stats);
        saw_reconnecting = saw_reconnecting || stats.reconnecting;
    }
    CHECK(saw_reconnecting && !stats.reconnecting);
    CHECK(stats.reconnects == 1u && script.starts == 2u);
    CHECK(script.last_offset == 0);
    media_mp3_source_destroy(source);

    /* A station that keeps refusing gives up after the reconnect limit. */
    static const char refusal[] = "<html>busy</html>";
    script = (ScriptTransport) {.bytes = (const unsigned char *) refusal,
                                .length = sizeof(refusal) - 1u,
                                .chunk = 64u};
    transport = script_transport(&script);
    source = media_mp3_source_create(&budget, &transport, &options, NULL, 0);
    CHECK(drain(source, UINT64_MAX, &result) == 0);
    CHECK(result.failed && result.frames == 0);
    CHECK(script.starts <= MEDIA_MP3_RECONNECT_LIMIT + 1u);
    /* A failed station is failed, not reconnecting. */
    media_mp3_source_stats(source, &stats);
    CHECK(!stats.reconnecting);
    media_mp3_source_destroy(source);

    /* A finite file over ranges resumes where the response broke. */
    size_t length = 0;
    unsigned char *bytes = read_fixture("cbr-32000-stereo.mp3", &length);
    CHECK(bytes != NULL);
    script = (ScriptTransport) {.bytes = bytes, .length = length,
                                .chunk = 300u, .ranges = true,
                                .drop_at = length / 2u + 7u};
    transport = script_transport(&script);
    MediaMp3SourceOptions finite = {.content_length = length,
                                    .seekable = true};
    source = media_mp3_source_create(&budget, &transport, &finite, NULL, 0);
    CHECK(drain(source, UINT64_MAX, &result) == 0);
    CHECK(!result.failed && result.frames == 29u);
    CHECK(script.starts == 2u && script.last_offset == length / 2u + 7u);
    media_mp3_source_destroy(source);
    free(bytes);
    CHECK(budget.current == 0);
    return 0;
}

/* A finite file from a server that ignores Range overflows the probe and
   opens as live (no length). Its response declares a Content-Length and
   completes with all of it: that is the end of the file, not a dropped
   station, so the source ends instead of replaying it forever. */
static int test_live_routed_finite_file(void)
{
    size_t length = 0;
    unsigned char *bytes = read_fixture("cbr-44100-stereo.mp3", &length);
    CHECK(bytes != NULL);
    Budget budget;
    budget_init(&budget, 1024u * 1024u);
    ScriptTransport script = {.bytes = bytes, .length = length,
                              .chunk = 512u, .declared = length};
    MediaMp3Transport transport = script_transport(&script);
    MediaMp3SourceOptions live = {0};
    MediaMp3Source *source = media_mp3_source_create(
        &budget, &transport, &live, NULL, 0);
    CHECK(source != NULL);
    DrainResult result;
    CHECK(drain(source, 200u, &result) == 0);
    CHECK(!result.failed && result.frames == 40u);
    MediaMp3Stats stats;
    media_mp3_source_stats(source, &stats);
    CHECK(stats.ended && stats.reconnects == 0 && script.starts == 1u);
    media_mp3_source_destroy(source);

    /* No declared length, or fewer bytes than declared: a station's
       connection ended, and it reconnects as before. */
    for (int declared = 0; declared < 2; declared++) {
        script = (ScriptTransport) {
            .bytes = bytes, .length = length, .chunk = 512u,
            .declared = declared != 0 ? length + 100u : 0
        };
        transport = script_transport(&script);
        source = media_mp3_source_create(&budget, &transport, &live, NULL, 0);
        CHECK(source != NULL);
        CHECK(drain(source, 60u, &result) == 0);
        CHECK(!result.failed && result.frames == 60u);
        media_mp3_source_stats(source, &stats);
        CHECK(!stats.ended && stats.reconnects >= 1u && script.starts >= 2u);
        media_mp3_source_destroy(source);
    }
    CHECK(budget.current == 0);
    free(bytes);
    return 0;
}

static int test_station_text(void)
{
    char text[32];
    /* UTF-8 passes through; Latin-1 (what most stations send) is widened. */
    CHECK(media_mp3_station_text("Radio Rom\xc3\xa2nia", text, sizeof(text)));
    CHECK(strcmp(text, "Radio Rom\xc3\xa2nia") == 0);
    CHECK(media_mp3_station_text("Radio Rom\xe2nia", text, sizeof(text)));
    CHECK(strcmp(text, "Radio Rom\xc3\xa2nia") == 0);
    /* Controls go; whitespace runs collapse and never lead or trail. */
    CHECK(media_mp3_station_text("  DEEP\t\tFM\r\n \x1b[0m ", text,
                                 sizeof(text)));
    CHECK(strcmp(text, "DEEP FM [0m") == 0);
    /* C1 controls in UTF-8 are controls too. */
    CHECK(media_mp3_station_text("A\xc2\x85" "B", text, sizeof(text)));
    CHECK(strcmp(text, "AB") == 0);
    /* Nothing printable: no name. */
    CHECK(!media_mp3_station_text(" \t\r\n", text, sizeof(text)));
    CHECK(text[0] == '\0');
    CHECK(!media_mp3_station_text(NULL, text, sizeof(text)));
    /* The bound cuts on a character, never inside one. */
    char small[8];
    CHECK(media_mp3_station_text("abcde\xc3\xa2\xc3\xa2", small,
                                 sizeof(small)));
    CHECK(strcmp(small, "abcde\xc3\xa2") == 0);
    CHECK(media_mp3_station_text("abcdef\xe2\x82\xac", small, sizeof(small)));
    CHECK(strcmp(small, "abcdef") == 0);
    return 0;
}

static int test_source_label(void)
{
    char label[40];
    CHECK(media_audio_source_label("http://141.94.96.97:8888/stream?x=1",
                                   label, sizeof(label)));
    CHECK(strcmp(label, "141.94.96.97:8888") == 0);
    CHECK(media_audio_source_label("HTTPS://User:pw@Stream.Example.NET/live",
                                   label, sizeof(label)));
    CHECK(strcmp(label, "stream.example.net") == 0);
    /* The default port says nothing; another one is part of the place. */
    CHECK(media_audio_source_label("http://radio.example:80/;", label,
                                   sizeof(label)));
    CHECK(strcmp(label, "radio.example") == 0);
    CHECK(media_audio_source_label("https://radio.example:443#a", label,
                                   sizeof(label)));
    CHECK(strcmp(label, "radio.example") == 0);
    CHECK(media_audio_source_label("https://radio.example:8443", label,
                                   sizeof(label)));
    CHECK(strcmp(label, "radio.example:8443") == 0);
    CHECK(media_audio_source_label("http://[2001:db8::1]:8000/a", label,
                                   sizeof(label)));
    CHECK(strcmp(label, "[2001:db8::1]:8000") == 0);
    /* Only web addresses with a host have a source. */
    CHECK(!media_audio_source_label("blob:https://a.example/1", label,
                                    sizeof(label)));
    CHECK(!media_audio_source_label("http:///path", label, sizeof(label)));
    CHECK(!media_audio_source_label(NULL, label, sizeof(label)));
    return 0;
}

/* A SHOUTcast DNAS root that sent a browser to its status page plays at
   "/;" on the same server; nothing else is rewritten. */
static int test_shoutcast_stream_url(void)
{
    char url[96];
    static const char *const roots[][3] = {
        /* source, where its probe ended, the stream address */
        {"http://shoutcast.example:8000/",
         "http://shoutcast.example:8000/index.html?sid=1",
         "http://shoutcast.example:8000/;"},
        /* The page's own address carried a tracking query and no path. */
        {"http://shoutcast.example:8000?fromyp=true",
         "http://shoutcast.example:8000/index.html?sid=1",
         "http://shoutcast.example:8000/;"},
        /* The once-upgraded HTTPS probe: the source's scheme is kept, so
           the next probe is upgraded the same way. */
        {"http://Shoutcast.Example:8000/",
         "https://shoutcast.example:8000/index.html?sid=12",
         "http://Shoutcast.Example:8000/;"},
        {"http://radio.example/", "https://radio.example/index.html?sid=1",
         "http://radio.example/;"},
        {"https://radio.example:8443/",
         "https://radio.example:8443/index.html?sid=2",
         "https://radio.example:8443/;"}
    };
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
        CHECK(media_shoutcast_stream_url(roots[i][0], roots[i][1], url,
                                         sizeof(url)));
        CHECK(strcmp(url, roots[i][2]) == 0);
    }
    static const char *const others[][2] = {
        /* Not the server root. */
        {"http://shoutcast.example:8000/live",
         "http://shoutcast.example:8000/index.html?sid=1"},
        {"http://shoutcast.example:8000/;",
         "http://shoutcast.example:8000/index.html?sid=1"},
        /* Another server, or another port of it. */
        {"http://shoutcast.example:8000/",
         "http://elsewhere.example:8000/index.html?sid=1"},
        {"http://shoutcast.example:8000/",
         "http://shoutcast.example:8001/index.html?sid=1"},
        /* Not the DNAS status page. */
        {"http://shoutcast.example:8000/",
         "http://shoutcast.example:8000/index.html"},
        {"http://shoutcast.example:8000/",
         "http://shoutcast.example:8000/index.html?sid=1&x=2"},
        {"http://shoutcast.example:8000/",
         "http://shoutcast.example:8000/status.html?sid=1"},
        {"http://shoutcast.example:8000/",
         "http://shoutcast.example:8000/"},
        {"blob:http://a.example/1", "http://a.example/index.html?sid=1"}
    };
    for (size_t i = 0; i < sizeof(others) / sizeof(others[0]); i++) {
        CHECK(!media_shoutcast_stream_url(others[i][0], others[i][1], url,
                                          sizeof(url)));
        CHECK(url[0] == '\0');
    }
    CHECK(!media_shoutcast_stream_url(
        "http://shoutcast.example:8000/",
        "http://shoutcast.example:8000/index.html?sid=1", url, 12));
    CHECK(!media_shoutcast_stream_url(NULL, NULL, url, sizeof(url)));
    return 0;
}

static int test_budget_refusal(void)
{
    Budget budget;
    budget_init(&budget, 64u * 1024u);
    ScriptTransport script = {.chunk = 1u};
    MediaMp3Transport transport = script_transport(&script);
    char error[160];
    CHECK(media_mp3_source_create(&budget, &transport, NULL, error,
                                  sizeof(error)) == NULL);
    CHECK(strstr(error, "memory budget") != NULL);
    CHECK(budget.current == 0);
    return 0;
}

int main(void)
{
    if (test_header_parsing() != 0) return 1;
    if (test_id3_and_sniff() != 0) return 1;
    if (test_fixtures() != 0) return 1;
    if (test_resync_and_damage() != 0) return 1;
    if (test_large_id3_streamed_and_ranged() != 0) return 1;
    if (test_id3_past_end() != 0) return 1;
    if (test_seek() != 0) return 1;
    if (test_seek_prime_over_slow_response() != 0) return 1;
    if (test_endless_stream() != 0) return 1;
    if (test_live_reconnect() != 0) return 1;
    if (test_live_routed_finite_file() != 0) return 1;
    if (test_station_text() != 0) return 1;
    if (test_source_label() != 0) return 1;
    if (test_shoutcast_stream_url() != 0) return 1;
    if (test_budget_refusal() != 0) return 1;
    puts("media mp3 tests passed");
    return 0;
}
