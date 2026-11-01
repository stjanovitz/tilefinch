/* The software decoder's packet and video-queue sizing against a fragmented
 * MP4: its moov is empty, so at open the demuxer knows only the first
 * fragment's samples, and a later fragment brings larger ones (a sample DASH
 * download: 1,737 bytes at open, a 3,532-byte keyframe at 2 s). The
 * backend sizes both buffers from the open-time largest sample and grows
 * them through swdec_packet_grow and swdec_au_queue_grow
 * (src/swdec/swdec_grow.h). This runs a real fragmented file through the
 * demuxer and those helpers on a Budget, rewriting every sample the way the
 * backend does, and checks what they do when the Budget refuses. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "swdec_bounds.h"
#include "swdec_grow.h"
#include "tilefinch/budget.h"
#include "tilefinch/media_mp4.h"

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

typedef struct {
    unsigned char *bytes;
    size_t length;
    size_t capacity;
} Fixture;

static void put_u32(unsigned char *output, uint32_t value)
{
    output[0] = (unsigned char) (value >> 24);
    output[1] = (unsigned char) (value >> 16);
    output[2] = (unsigned char) (value >> 8);
    output[3] = (unsigned char) value;
}

static size_t begin_box(Fixture *fixture, uint32_t type)
{
    size_t start = fixture->length;
    fixture->length += 8;
    put_u32(fixture->bytes + start + 4, type);
    return start;
}

static void end_box(Fixture *fixture, size_t start)
{
    put_u32(fixture->bytes + start, (uint32_t) (fixture->length - start));
}

static void append_u32(Fixture *fixture, uint32_t value)
{
    put_u32(fixture->bytes + fixture->length, value);
    fixture->length += 4;
}

/* A sample of length-prefixed (four-byte) NAL units: one big slice plus a
   few small ones, as an encoder writes them. */
static void fill_sample(unsigned char *at, uint32_t size, bool keyframe)
{
    uint32_t small = 6u;
    uint32_t smalls = size > 64u ? 3u : 0u;
    uint32_t first = size - 4u - smalls * (4u + small);
    put_u32(at, first);
    at[4] = keyframe ? 0x65 : 0x41;
    memset(at + 5, 0xa5, first - 1u);
    at += 4u + first;
    for (uint32_t i = 0; i < smalls; i++) {
        put_u32(at, small);
        at[4] = 0x06;
        memset(at + 5, 0x5a, small - 1u);
        at += 4u + small;
    }
}

typedef struct {
    uint32_t size;
    bool keyframe;
} SampleSpec;

static void append_fragment(Fixture *fixture, uint32_t sequence,
                            uint32_t decode_time,
                            const SampleSpec *samples, size_t count)
{
    size_t moof = begin_box(fixture, MEDIA_MP4_FOURCC('m','o','o','f'));
    size_t mfhd = begin_box(fixture, MEDIA_MP4_FOURCC('m','f','h','d'));
    append_u32(fixture, 0);
    append_u32(fixture, sequence);
    end_box(fixture, mfhd);
    size_t traf = begin_box(fixture, MEDIA_MP4_FOURCC('t','r','a','f'));
    size_t tfhd = begin_box(fixture, MEDIA_MP4_FOURCC('t','f','h','d'));
    append_u32(fixture, UINT32_C(0x0002000a));
    append_u32(fixture, 1);
    append_u32(fixture, 1);
    append_u32(fixture, 1000);
    end_box(fixture, tfhd);
    size_t tfdt = begin_box(fixture, MEDIA_MP4_FOURCC('t','f','d','t'));
    append_u32(fixture, 0);
    append_u32(fixture, decode_time);
    end_box(fixture, tfdt);
    size_t trun = begin_box(fixture, MEDIA_MP4_FOURCC('t','r','u','n'));
    append_u32(fixture, UINT32_C(0x00000701));
    append_u32(fixture, (uint32_t) count);
    size_t data_offset = fixture->length;
    append_u32(fixture, 0);
    for (size_t i = 0; i < count; i++) {
        append_u32(fixture, 1000);
        append_u32(fixture, samples[i].size);
        append_u32(fixture, samples[i].keyframe ? 0 : UINT32_C(0x00010000));
    }
    end_box(fixture, trun);
    end_box(fixture, traf);
    end_box(fixture, moof);
    size_t mdat = begin_box(fixture, MEDIA_MP4_FOURCC('m','d','a','t'));
    put_u32(fixture->bytes + data_offset, (uint32_t) (fixture->length - moof));
    for (size_t i = 0; i < count; i++) {
        fill_sample(fixture->bytes + fixture->length, samples[i].size,
                    samples[i].keyframe);
        fixture->length += samples[i].size;
    }
    end_box(fixture, mdat);
}

/* An empty-moov H.264 file in two indexed fragments of two seconds each. */
static void make_fragmented_file(Fixture *fixture,
                                 const SampleSpec *first, size_t first_count,
                                 const SampleSpec *second, size_t second_count)
{
    memset(fixture->bytes, 0, fixture->capacity);
    fixture->length = 0;
    size_t ftyp = begin_box(fixture, MEDIA_MP4_FOURCC('f','t','y','p'));
    append_u32(fixture, MEDIA_MP4_FOURCC('i','s','o','m'));
    append_u32(fixture, 0);
    end_box(fixture, ftyp);
    size_t moov = begin_box(fixture, MEDIA_MP4_FOURCC('m','o','o','v'));
    size_t mvex = begin_box(fixture, MEDIA_MP4_FOURCC('m','v','e','x'));
    size_t trex = begin_box(fixture, MEDIA_MP4_FOURCC('t','r','e','x'));
    append_u32(fixture, 0);
    append_u32(fixture, 1);
    append_u32(fixture, 1);
    append_u32(fixture, 1000);
    append_u32(fixture, 0);
    append_u32(fixture, 0);
    end_box(fixture, trex);
    end_box(fixture, mvex);
    size_t trak = begin_box(fixture, MEDIA_MP4_FOURCC('t','r','a','k'));
    size_t tkhd = begin_box(fixture, MEDIA_MP4_FOURCC('t','k','h','d'));
    append_u32(fixture, 0);
    append_u32(fixture, 0);
    append_u32(fixture, 0);
    append_u32(fixture, 1);
    append_u32(fixture, 0);
    end_box(fixture, tkhd);
    size_t mdia = begin_box(fixture, MEDIA_MP4_FOURCC('m','d','i','a'));
    size_t mdhd = begin_box(fixture, MEDIA_MP4_FOURCC('m','d','h','d'));
    append_u32(fixture, 0);
    append_u32(fixture, 0);
    append_u32(fixture, 0);
    append_u32(fixture, 1000);
    append_u32(fixture, 4000);
    append_u32(fixture, 0);
    end_box(fixture, mdhd);
    size_t hdlr = begin_box(fixture, MEDIA_MP4_FOURCC('h','d','l','r'));
    append_u32(fixture, 0);
    append_u32(fixture, 0);
    append_u32(fixture, MEDIA_MP4_FOURCC('v','i','d','e'));
    end_box(fixture, hdlr);
    size_t minf = begin_box(fixture, MEDIA_MP4_FOURCC('m','i','n','f'));
    size_t stbl = begin_box(fixture, MEDIA_MP4_FOURCC('s','t','b','l'));
    size_t stsd = begin_box(fixture, MEDIA_MP4_FOURCC('s','t','s','d'));
    append_u32(fixture, 0);
    append_u32(fixture, 1);
    size_t avc1 = begin_box(fixture, MEDIA_MP4_FOURCC('a','v','c','1'));
    fixture->length += 78;
    fixture->bytes[avc1 + 32] = 0x01; /* 320 */
    fixture->bytes[avc1 + 33] = 0x40;
    fixture->bytes[avc1 + 34] = 0x00; /* 240 */
    fixture->bytes[avc1 + 35] = 0xf0;
    size_t avcc = begin_box(fixture, MEDIA_MP4_FOURCC('a','v','c','C'));
    /* Four-byte NAL lengths (0xff); no parameter sets needed here. */
    static const unsigned char config[] = {1, 0x42, 0, 0x1e, 0xff, 0xe1, 0};
    memcpy(fixture->bytes + fixture->length, config, sizeof(config));
    fixture->length += sizeof(config);
    end_box(fixture, avcc);
    end_box(fixture, avc1);
    end_box(fixture, stsd);
    const uint32_t empty_tables[] = {
        MEDIA_MP4_FOURCC('s','t','t','s'),
        MEDIA_MP4_FOURCC('s','t','s','c'),
        MEDIA_MP4_FOURCC('s','t','c','o')
    };
    for (size_t at = 0; at < 3; at++) {
        size_t table = begin_box(fixture, empty_tables[at]);
        append_u32(fixture, 0);
        append_u32(fixture, 0);
        end_box(fixture, table);
    }
    size_t stsz = begin_box(fixture, MEDIA_MP4_FOURCC('s','t','s','z'));
    append_u32(fixture, 0);
    append_u32(fixture, 0);
    append_u32(fixture, 0);
    end_box(fixture, stsz);
    end_box(fixture, stbl);
    end_box(fixture, minf);
    end_box(fixture, mdia);
    end_box(fixture, trak);
    end_box(fixture, moov);
    /* A segment index, as DASH writes it: the demuxer then loads one
       fragment at a time, so its open reports only the first's samples. */
    size_t sidx = begin_box(fixture, MEDIA_MP4_FOURCC('s','i','d','x'));
    append_u32(fixture, 0);       /* version + flags */
    append_u32(fixture, 1);       /* reference ID */
    append_u32(fixture, 1000);    /* timescale */
    append_u32(fixture, 0);       /* earliest presentation time */
    append_u32(fixture, 0);       /* first offset */
    append_u32(fixture, 2);       /* reserved + reference count */
    size_t first_size = fixture->length;
    append_u32(fixture, 0);
    append_u32(fixture, 2000);
    append_u32(fixture, UINT32_C(0x90000000));
    size_t second_size = fixture->length;
    append_u32(fixture, 0);
    append_u32(fixture, 2000);
    append_u32(fixture, UINT32_C(0x90000000));
    end_box(fixture, sidx);
    size_t at = fixture->length;
    append_fragment(fixture, 1, 0, first, first_count);
    put_u32(fixture->bytes + first_size, (uint32_t) (fixture->length - at));
    at = fixture->length;
    append_fragment(fixture, 2, 2000, second, second_count);
    put_u32(fixture->bytes + second_size, (uint32_t) (fixture->length - at));
}

typedef struct {
    const unsigned char *bytes;
    size_t length;
} Reader;

static bool reader_read(void *opaque, uint64_t offset, void *destination,
                        size_t length)
{
    const Reader *reader = opaque;
    if (offset > reader->length || length > reader->length - offset)
        return false;
    memcpy(destination, reader->bytes + offset, length);
    return true;
}

/* swdec_convert_avcc's rewrite: four-byte start codes, parameter sets in
   front of a keyframe, never past `capacity`. */
static bool rewrite_avcc(unsigned char *packet, size_t capacity,
                         const unsigned char *payload, size_t length,
                         unsigned nal_length_size, size_t parameter_sets,
                         bool keyframe, size_t *written)
{
    size_t used = 0;
    if (keyframe) {
        if (parameter_sets > capacity) return false;
        memset(packet, 0x67, parameter_sets);
        used = parameter_sets;
    }
    size_t at = 0;
    while (at < length) {
        if (nal_length_size > length - at) return false;
        size_t bytes = 0;
        for (unsigned i = 0; i < nal_length_size; i++)
            bytes = (bytes << 8) | payload[at + i];
        at += nal_length_size;
        if (bytes == 0 || bytes > length - at || capacity - used < 4u
            || bytes > capacity - used - 4u) return false;
        packet[used] = 0;
        packet[used + 1u] = 0;
        packet[used + 2u] = 0;
        packet[used + 3u] = 1u;
        memcpy(packet + used + 4u, payload + at, bytes);
        used += 4u + bytes;
        at += bytes;
    }
    *written = used;
    return true;
}

typedef struct {
    uint32_t open_largest;
    unsigned samples;
    unsigned over_open;
    unsigned packet_grows;
    unsigned queue_grows;
    unsigned unfixed_failures;
    size_t packet_capacity;
    size_t queue_capacity;
} SizingRun;

/* Open the file and play it through the backend's sizing: the packet and
   queue planned at open from the track's largest sample, then grown for
   each sample as swdec_job_packet_fit and swdec_au_queue_grow do. */
static int run_sizing(const Fixture *fixture, bool have_audio,
                      SizingRun *run)
{
    memset(run, 0, sizeof(*run));
    Budget budget;
    budget_init(&budget, 4u * 1024u * 1024u);
    Reader source = {fixture->bytes, fixture->length};
    MediaRangeReader reader = {
        .opaque = &source, .length = fixture->length, .read = reader_read
    };
    char error[256] = "";
    MediaMp4Demux *demux = media_mp4_open(
        &budget, &reader, NULL, error, sizeof(error));
    if (demux == NULL) fprintf(stderr, "open: %s\n", error);
    CHECK(demux != NULL);
    MediaMp4TrackInfo video;
    CHECK(media_mp4_track_count(demux) == 1u);
    CHECK(media_mp4_track_info(demux, 0, &video));
    CHECK(video.kind == MEDIA_MP4_TRACK_VIDEO);
    CHECK(video.nal_length_size == 4u);
    run->open_largest = video.largest_sample;

    /* As media_psp_swdec_backend_create_sources sizes them. */
    const size_t parameter_capacity = video.codec_config_length + 1024u;
    const size_t parameter_sets = 40u;
    CHECK(swdec_packet_bytes(video.largest_sample, video.nal_length_size,
                             parameter_capacity, &run->packet_capacity));
    const size_t unfixed_packet = run->packet_capacity;
    size_t queue_floor = 0;
    CHECK(swdec_au_queue_plan(video.largest_sample, have_audio,
                              &run->queue_capacity, &queue_floor));
    const size_t unfixed_queue = run->queue_capacity;
    size_t ceiling = 0;
    CHECK(swdec_packet_bytes(SWDEC_SAMPLE_MAX_BYTES, video.nal_length_size,
                             parameter_sets, &ceiling));

    static unsigned char payload[SWDEC_SAMPLE_MAX_BYTES];
    unsigned char *packet = budget_malloc_category(
        &budget, BUDGET_CATEGORY_RESOURCE, run->packet_capacity);
    unsigned char *queue = budget_malloc_category(
        &budget, BUDGET_CATEGORY_RESOURCE, run->queue_capacity);
    CHECK(packet != NULL && queue != NULL);
    MediaMp4Sample sample;
    while (media_mp4_next_sample(demux, &sample)) {
        CHECK(sample.size <= sizeof(payload));
        CHECK(media_mp4_read_sample(demux, &sample, payload,
                                    sizeof(payload)));
        run->samples++;
        if (sample.size > run->open_largest) run->over_open++;

        /* swdec_submit: the queue takes the unit, growing first. */
        SwdecGrowResult queued = swdec_au_queue_grow(
            &budget, &queue, &run->queue_capacity, sample.size, have_audio);
        CHECK(queued == SWDEC_GROW_FITS || queued == SWDEC_GROW_GROWN);
        if (queued == SWDEC_GROW_GROWN) run->queue_grows++;
        CHECK(sample.size < run->queue_capacity);
        memcpy(queue, payload, sample.size);

        /* swdec_dispatch_queued: the job's packet holds the rewrite. */
        size_t needed = 0;
        CHECK(swdec_packet_bytes(sample.size, video.nal_length_size,
                                 parameter_sets, &needed));
        SwdecGrowResult fit = swdec_packet_grow(
            &budget, &packet, &run->packet_capacity, needed, ceiling);
        CHECK(fit == SWDEC_GROW_FITS || fit == SWDEC_GROW_GROWN);
        if (fit == SWDEC_GROW_GROWN) run->packet_grows++;
        CHECK(run->packet_capacity >= needed
              && run->packet_capacity <= ceiling);
        size_t written = 0;
        CHECK(rewrite_avcc(packet, run->packet_capacity, queue,
                           sample.size, video.nal_length_size,
                           parameter_sets, sample.keyframe, &written));
        CHECK(written == sample.size + (sample.keyframe ? parameter_sets : 0));

        /* Before the fix: buffers stayed at their open-time size. */
        size_t unfixed_written = 0;
        if (sample.size >= unfixed_queue
            || needed > unfixed_packet
            || !rewrite_avcc(packet, unfixed_packet, queue, sample.size,
                             video.nal_length_size, parameter_sets,
                             sample.keyframe, &unfixed_written))
            run->unfixed_failures++;
    }
    error[0] = '\0';
    CHECK(!media_mp4_last_error(demux, error, sizeof(error)));
    budget_free(&budget, packet);
    budget_free(&budget, queue);
    media_mp4_close(demux);
    CHECK(budget_active_allocations(&budget, NULL) == 0);
    CHECK(budget.current == 0);
    return 0;
}

static void fill_pattern(unsigned char *bytes, size_t length, unsigned seed)
{
    for (size_t i = 0; i < length; i++)
        bytes[i] = (unsigned char) ((i * 31u + seed) & 0xffu);
}

static bool has_pattern(const unsigned char *bytes, size_t length,
                        unsigned seed)
{
    for (size_t i = 0; i < length; i++)
        if (bytes[i] != (unsigned char) ((i * 31u + seed) & 0xffu))
            return false;
    return true;
}

/* swdec_packet_grow and swdec_au_queue_grow on a Budget that refuses: by
   its limit, and by the Budget's own one-shot fault injection. */
static int test_refusals(void)
{
    const size_t ceiling = SWDEC_SAMPLE_MAX_BYTES + 64u;
    Budget budget;
    budget_init(&budget, 4u * 1024u * 1024u);

    puts("test: a refused half-again packet falls back to the exact size");
    size_t capacity = 2768u;
    unsigned char *packet = budget_malloc_category(
        &budget, BUDGET_CATEGORY_RESOURCE, capacity);
    CHECK(packet != NULL);
    fill_pattern(packet, capacity, 1u);
    /* Room for exactly 3,572 bytes (the fixture keyframe's), not 4,152. */
    size_t limit = budget.limit;
    budget.limit = budget.current + (3572u - capacity);
    CHECK(swdec_packet_regrow(capacity, 3572u, ceiling) == 4152u);
    CHECK(swdec_packet_grow(&budget, &packet, &capacity, 3572u, ceiling)
          == SWDEC_GROW_GROWN);
    CHECK(capacity == 3572u);
    CHECK(has_pattern(packet, 2768u, 1u));
    budget.limit = limit;
    /* The Budget's fault injection refuses the first request only. */
    budget_inject_failure_after(&budget, 0);
    size_t failures = budget.injected_failure_count;
    CHECK(swdec_packet_grow(&budget, &packet, &capacity, 4000u, ceiling)
          == SWDEC_GROW_GROWN);
    CHECK(budget.injected_failure_count == failures + 1u);
    CHECK(capacity == 4000u);
    CHECK(has_pattern(packet, 2768u, 1u));

    puts("test: a packet refused both sizes is left exactly as it was");
    fill_pattern(packet, capacity, 2u);
    unsigned char *before = packet;
    size_t used = budget.current;
    budget.limit = budget.current; /* no room at all */
    CHECK(swdec_packet_grow(&budget, &packet, &capacity, 4100u, ceiling)
          == SWDEC_GROW_REFUSED);
    CHECK(packet == before && capacity == 4000u);
    CHECK(has_pattern(packet, capacity, 2u));
    CHECK(budget.current == used);
    budget.limit = limit;
    /* Exact size equal to the half-again one: a single refused request. */
    budget_inject_failure_after(&budget, 0);
    CHECK(swdec_packet_grow(&budget, &packet, &capacity, 6000u, ceiling)
          == SWDEC_GROW_REFUSED);
    CHECK(packet == before && capacity == 4000u);
    CHECK(has_pattern(packet, capacity, 2u));
    CHECK(swdec_packet_grow(&budget, &packet, &capacity, 3000u, ceiling)
          == SWDEC_GROW_FITS);
    CHECK(swdec_packet_grow(&budget, &packet, &capacity, ceiling + 1u,
                            ceiling) == SWDEC_GROW_OVER_BOUND);
    CHECK(packet == before && capacity == 4000u);
    budget_free(&budget, packet);

    puts("test: a grown queue keeps its queued units where they were");
    size_t queue_capacity = 65536u;
    unsigned char *queue = budget_malloc_category(
        &budget, BUDGET_CATEGORY_RESOURCE, queue_capacity);
    CHECK(queue != NULL);
    /* A wrapped ring: units at the top and at the start. */
    fill_pattern(queue + 40000u, 25000u, 3u);
    fill_pattern(queue, 12000u, 4u);
    CHECK(swdec_au_queue_grow(&budget, &queue, &queue_capacity, 100000u,
                              false) == SWDEC_GROW_GROWN);
    CHECK(queue_capacity == 200000u);
    CHECK(has_pattern(queue + 40000u, 25000u, 3u));
    CHECK(has_pattern(queue, 12000u, 4u));

    puts("test: a refused queue steps down to the least that holds the unit");
    /* Room for 300,001 bytes but not the 600,000 the plan prefers. */
    budget.limit = budget.current + (300001u - queue_capacity);
    CHECK(swdec_au_queue_grow(&budget, &queue, &queue_capacity, 300000u,
                              false) == SWDEC_GROW_GROWN);
    CHECK(queue_capacity == 300001u);
    CHECK(has_pattern(queue + 40000u, 25000u, 3u));
    CHECK(has_pattern(queue, 12000u, 4u));

    puts("test: a queue refused every size is left exactly as it was");
    before = queue;
    used = budget.current;
    budget.limit = budget.current;
    CHECK(swdec_au_queue_grow(&budget, &queue, &queue_capacity, 400000u,
                              false) == SWDEC_GROW_REFUSED);
    CHECK(queue == before && queue_capacity == 300001u);
    CHECK(has_pattern(queue + 40000u, 25000u, 3u));
    CHECK(has_pattern(queue, 12000u, 4u));
    CHECK(budget.current == used);
    budget.limit = limit;
    CHECK(swdec_au_queue_grow(&budget, &queue, &queue_capacity, 1000u,
                              false) == SWDEC_GROW_FITS);
    CHECK(swdec_au_queue_grow(&budget, &queue, &queue_capacity,
                              SWDEC_SAMPLE_MAX_BYTES + 1u, true)
          == SWDEC_GROW_OVER_BOUND);
    CHECK(queue == before && queue_capacity == 300001u);
    budget_free(&budget, queue);

    CHECK(budget_active_allocations(&budget, NULL) == 0);
    CHECK(budget.current == 0);
    return 0;
}

static int test_helpers(void)
{
    puts("test: packet bytes cover the start-code rewrite");
    size_t bytes = 0;
    CHECK(swdec_packet_bytes(1000, 4, 64, &bytes) && bytes == 1064u);
    CHECK(swdec_packet_bytes(1000, 0, 64, &bytes) && bytes == 1064u);
    /* Two-byte prefixes: at most 333 NAL units, two bytes more each. */
    CHECK(swdec_packet_bytes(1000, 2, 0, &bytes) && bytes == 1666u);
    CHECK(swdec_packet_bytes(1000, 1, 0, &bytes) && bytes == 2500u);
    CHECK(!swdec_packet_bytes(1000, 5, 0, &bytes));
    CHECK(!swdec_packet_bytes(SIZE_MAX - 10u, 4, 64, &bytes));
    CHECK(!swdec_packet_bytes(SIZE_MAX / 2u, 1, 0, &bytes));
    CHECK(!swdec_packet_bytes(1000, 4, 64, NULL));

    puts("test: the packet grows by half again, to the bound and no further");
    CHECK(swdec_packet_regrow(4000, 4000, 1u << 20) == 4000u);
    CHECK(swdec_packet_regrow(4000, 10, 1u << 20) == 4000u);
    CHECK(swdec_packet_regrow(4000, 4001, 1u << 20) == 6000u);
    CHECK(swdec_packet_regrow(4000, 9000, 1u << 20) == 9000u);
    CHECK(swdec_packet_regrow(900000, 900001, 1u << 20) == 1u << 20);
    CHECK(swdec_packet_regrow(4000, (1u << 20) + 1u, 1u << 20) == 0u);
    CHECK(swdec_packet_regrow(0, 100, 1u << 20) == 100u);
    /* Even a fit past the bound is left alone (open sized it so). */
    CHECK(swdec_packet_regrow(2u << 20, 4000, 1u << 20) == 2u << 20);

    puts("test: a queue grows only for a unit it can never hold");
    CHECK(!swdec_au_queue_needs_growth(65536, 65535));
    CHECK(swdec_au_queue_needs_growth(65536, 65536));
    CHECK(swdec_au_queue_needs_growth(0, 1));
    size_t preferred = 0, floor = 0;
    CHECK(swdec_au_queue_plan(SWDEC_SAMPLE_MAX_BYTES, true,
                              &preferred, &floor));
    CHECK(floor == SWDEC_SAMPLE_MAX_BYTES + 1u
          && preferred == SWDEC_AU_QUEUE_MAX_BYTES);

    puts("test: AAC slots hold the codec's largest frame, not the open's");
    CHECK(swdec_aac_sample_bound(571, 2) == 1536u);
    CHECK(swdec_aac_sample_bound(571, 1) == 768u);
    CHECK(swdec_aac_sample_bound(571, 0) == 1536u);
    CHECK(swdec_aac_sample_bound(571, 6) == 1536u);
    CHECK(swdec_aac_sample_bound(670, 2) >= 670u);
    CHECK(swdec_aac_sample_bound(8192, 2) == 8192u);
    return 0;
}

int main(void)
{
    if (test_helpers() != 0) return 1;
    if (test_refusals() != 0) return 1;

    static unsigned char storage[256u * 1024u];
    Fixture fixture = {storage, 0, sizeof(storage)};

    puts("test: a later fragment's larger keyframe grows the packet");
    {
        const SampleSpec first[] = {{1737, true}, {900, false}};
        const SampleSpec second[] = {{3532, true}, {1200, false}};
        make_fragmented_file(&fixture, first, 2, second, 2);
        SizingRun run;
        if (run_sizing(&fixture, true, &run) != 0) return 1;
        CHECK(run.open_largest == 1737u);
        CHECK(run.samples == 4u);
        CHECK(run.over_open == 1u);
        CHECK(run.packet_grows == 1u);
        CHECK(run.queue_grows == 0u);
        CHECK(run.unfixed_failures == 1u);
    }

    puts("test: a later unit past the whole queue grows the queue");
    {
        /* Video only: a 64 KiB queue at open; a 100 KB keyframe later. */
        const SampleSpec first[] = {{2000, true}, {700, false}};
        const SampleSpec second[] = {
            {100000, true}, {5000, false}, {120000, false}
        };
        make_fragmented_file(&fixture, first, 2, second, 3);
        SizingRun run;
        if (run_sizing(&fixture, false, &run) != 0) return 1;
        CHECK(run.open_largest == 2000u);
        CHECK(run.samples == 5u);
        CHECK(run.over_open == 3u);
        CHECK(run.queue_grows == 1u);
        CHECK(run.queue_capacity >= 200000u);
        CHECK(run.packet_grows == 2u);
        CHECK(run.unfixed_failures == 3u);
    }

    puts("test: a file whose open knows its largest sample never grows");
    {
        const SampleSpec first[] = {{8000, true}, {900, false}};
        const SampleSpec second[] = {{6000, true}, {7999, false}};
        make_fragmented_file(&fixture, first, 2, second, 2);
        SizingRun run;
        if (run_sizing(&fixture, true, &run) != 0) return 1;
        CHECK(run.open_largest == 8000u);
        CHECK(run.packet_grows == 0u && run.queue_grows == 0u);
        CHECK(run.unfixed_failures == 0u);
    }
    puts("ok");
    return 0;
}
