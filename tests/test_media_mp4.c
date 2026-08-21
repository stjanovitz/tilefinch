#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tilefinch/budget.h"
#include "tilefinch/media_backend.h"
#include "tilefinch/media_http.h"
#include "tilefinch/media_mp4.h"
#include "../src/media_h264_psp_compat.h"
#include "../src/media_backend_psp_policy.h"
#include "../src/media_backend_psp_pool.h"
#include "../src/media_mp4_policy.h"

#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, \
                #condition);                                                 \
        return 1;                                                            \
    }                                                                        \
} while (0)

typedef struct {
    unsigned char bytes[8192];
    size_t length;
} Fixture;

typedef struct {
    const unsigned char *bytes;
    size_t length;
    size_t reads;
    size_t bytes_read;
    size_t one_shot_offset;
    size_t one_shot_length;
    size_t one_shot_reads;
    size_t watch_offset;
    size_t watch_length;
    size_t watch_reads;
    size_t fail_offset;
    bool fail_enabled;
    size_t cancel_offset;
    TilefinchCancellation *cancel_on_read;
    /* Answer this many polls with would-block before serving bytes, the way a
       real range source behaves while its next window is on the wire. */
    size_t block_polls;
    size_t polls;
    /* A one-window nonblocking source split at an arbitrary byte. Entering
       the other half installs it only after one WOULD_BLOCK, evicting the
       previous half just like MediaHttpRange. */
    uint64_t poll_window_split;
    unsigned poll_window;
    bool poll_window_valid;
    size_t poll_window_switches;
} FixtureReader;

typedef struct {
    const unsigned char *bytes;
    size_t length;
    size_t requests;
    size_t failures_remaining;
    size_t truncate_success_by;
    /* Requested by the transport itself, so a cancellation can be observed
       exactly where a real one arrives: while an attempt is in flight. */
    TilefinchCancellation *cancel_on_request;
} HttpFixture;

static bool http_fixture_cancelled(void *opaque)
{
    return tilefinch_cancellation_requested(opaque);
}

typedef struct {
    size_t submitted;
    size_t drains;
    size_t advanced;
    size_t resets;
    size_t destroyed;
    size_t discarded_before;
    uint64_t discard_floor_us;
    bool drain_emit_once;
    bool drain_tail_pending;
    bool queue_once;
    size_t presentation_borrows;
    size_t presentation_releases;
    size_t presentation_staged;
    size_t presentation_displayed;
    TilefinchCancellation *cancel_on_submit;
    TilefinchCancellation *cancel_on_drain;
} BackendFixture;

static bool backend_presentation_borrow(
    void *opaque, unsigned slot, uint32_t generation)
{
    BackendFixture *fixture = opaque;
    if (fixture == NULL || slot != 7u || generation != 9u) return false;
    fixture->presentation_borrows++;
    return true;
}

static void backend_presentation_release(void *opaque, unsigned slot)
{
    BackendFixture *fixture = opaque;
    if (fixture != NULL && slot == 7u) fixture->presentation_releases++;
}

static void backend_presentation_note_staged(
    void *opaque, const MediaVideoFrame *frame)
{
    BackendFixture *fixture = opaque;
    if (fixture != NULL && frame != NULL && frame->slot == 7)
        fixture->presentation_staged++;
}

static void backend_presentation_note_displayed(
    void *opaque, const MediaVideoFrame *frame, int present_path)
{
    BackendFixture *fixture = opaque;
    if (fixture != NULL && frame != NULL && frame->slot == 7
        && present_path == 3) fixture->presentation_displayed++;
}

static const MediaBackendPresentationOps backend_presentation_ops = {
    .borrow = backend_presentation_borrow,
    .release = backend_presentation_release,
    .note_staged = backend_presentation_note_staged,
    .note_displayed = backend_presentation_note_displayed
};

static MediaBackendResult backend_submit(
    void *opaque, const MediaMp4Sample *sample,
    const unsigned char *bytes, size_t length,
    char *error, size_t error_size)
{
    (void) error;
    (void) error_size;
    BackendFixture *fixture = opaque;
    if (fixture == NULL || sample == NULL || bytes == NULL
        || length != sample->size) return MEDIA_BACKEND_ERROR;
    fixture->submitted++;
    if (fixture->queue_once) {
        fixture->queue_once = false;
        return MEDIA_BACKEND_QUEUED;
    }
    if (fixture->cancel_on_submit != NULL)
        tilefinch_cancellation_request(fixture->cancel_on_submit);
    return MEDIA_BACKEND_ACCEPTED;
}

static MediaBackendResult backend_drain(
    void *opaque, char *error, size_t error_size)
{
    (void) error;
    (void) error_size;
    BackendFixture *fixture = opaque;
    if (fixture == NULL) return MEDIA_BACKEND_ERROR;
    fixture->drains++;
    if (fixture->cancel_on_drain != NULL)
        tilefinch_cancellation_request(fixture->cancel_on_drain);
    if (fixture->drain_tail_pending)
        return MEDIA_BACKEND_WOULD_BLOCK;
    if (fixture->drain_emit_once) {
        fixture->drain_emit_once = false;
        fixture->drain_tail_pending = true;
        return MEDIA_BACKEND_ACCEPTED;
    }
    return MEDIA_BACKEND_END;
}

static bool backend_take_video_frame(
    void *opaque, MediaVideoFrame *frame)
{
    BackendFixture *fixture = opaque;
    if (fixture == NULL || frame == NULL
        || !fixture->drain_tail_pending) return false;
    fixture->drain_tail_pending = false;
    *frame = (MediaVideoFrame) {
        .pixels = (const void *) (uintptr_t) 1,
        .width = 1,
        .height = 1,
        .stride_pixels = 1,
        .duration_us = UINT64_C(40000),
        .identity = 1,
        /* A backend with no decoded-output slots to name. */
        .slot = -1
    };
    return true;
}

static size_t backend_discard_video_before(void *opaque, uint64_t floor_us)
{
    BackendFixture *fixture = opaque;
    if (fixture == NULL) return 0;
    fixture->discard_floor_us = floor_us;
    return fixture->discarded_before;
}

static bool backend_advance(void *opaque, uint64_t clock_us,
                            char *error, size_t error_size)
{
    (void) clock_us;
    (void) error;
    (void) error_size;
    BackendFixture *fixture = opaque;
    if (fixture == NULL) return false;
    fixture->advanced++;
    return true;
}

static bool backend_reset(void *opaque, char *error, size_t error_size)
{
    (void) error;
    (void) error_size;
    BackendFixture *fixture = opaque;
    if (fixture == NULL) return false;
    fixture->resets++;
    return true;
}

static void backend_destroy(void *opaque)
{
    BackendFixture *fixture = opaque;
    if (fixture != NULL) fixture->destroyed++;
}

static bool http_fixture_range(
    void *opaque, uint64_t first_byte, uint64_t last_byte,
    unsigned char *destination, size_t capacity, size_t *length,
    uint64_t *complete_length, char *error, size_t error_size)
{
    (void) error;
    (void) error_size;
    HttpFixture *fixture = opaque;
    if (fixture != NULL) fixture->requests++;
    if (fixture != NULL && fixture->cancel_on_request != NULL) {
        tilefinch_cancellation_request(fixture->cancel_on_request);
        snprintf(error, error_size, "fixture cancelled mid-attempt");
        return false;
    }
    if (fixture == NULL || last_byte < first_byte
        || last_byte >= fixture->length
        || last_byte - first_byte + 1u > capacity) {
        return false;
    }
    if (fixture->failures_remaining != 0) {
        fixture->failures_remaining--;
        snprintf(error, error_size, "fixture transient failure");
        return false;
    }
    *length = (size_t) (last_byte - first_byte + 1u);
    if (fixture->truncate_success_by != 0
        && fixture->truncate_success_by < *length) {
        *length -= fixture->truncate_success_by;
    }
    memcpy(destination, fixture->bytes + (size_t) first_byte, *length);
    *complete_length = fixture->length;
    return true;
}

static bool http_fixture_url_admitted(const char *url)
{
    return url != NULL
        && strncmp(url, "https://media.invalid/", 22u) == 0;
}

static void put_u32(unsigned char *output, uint32_t value)
{
    output[0] = (unsigned char) (value >> 24);
    output[1] = (unsigned char) (value >> 16);
    output[2] = (unsigned char) (value >> 8);
    output[3] = (unsigned char) value;
}

typedef struct {
    unsigned char bytes[64];
    size_t bit;
} SpsFixture;

static void sps_bits(SpsFixture *sps, uint32_t value, unsigned count)
{
    for (unsigned i = 0; i < count; i++) {
        unsigned shift = count - i - 1u;
        if (((value >> shift) & 1u) != 0) {
            sps->bytes[sps->bit / 8u] |=
                (unsigned char) (1u << (7u - sps->bit % 8u));
        }
        sps->bit++;
    }
}

static void sps_ue(SpsFixture *sps, uint32_t value)
{
    uint32_t code = value + 1u;
    unsigned bits = 0;
    for (uint32_t at = code; at != 0; at >>= 1u) bits++;
    sps_bits(sps, 0, bits - 1u);
    sps_bits(sps, code, bits);
}

static size_t make_baseline_sps(
    unsigned char output[64], uint32_t width_mbs_minus_one,
    uint32_t height_map_minus_one)
{
    SpsFixture sps = {0};
    sps_bits(&sps, 0x67, 8); /* nal_ref_idc=3, nal_unit_type=SPS */
    sps_bits(&sps, 66, 8);   /* baseline profile */
    sps_bits(&sps, 0, 8);    /* constraints */
    sps_bits(&sps, 30, 8);   /* level */
    sps_ue(&sps, 0);         /* seq_parameter_set_id */
    sps_ue(&sps, 0);         /* log2_max_frame_num_minus4 */
    sps_ue(&sps, 0);         /* pic_order_cnt_type */
    sps_ue(&sps, 0);         /* log2_max_pic_order_cnt_lsb_minus4 */
    sps_ue(&sps, 1);         /* max_num_ref_frames */
    sps_bits(&sps, 0, 1);    /* gaps_in_frame_num_value_allowed_flag */
    sps_ue(&sps, width_mbs_minus_one);
    sps_ue(&sps, height_map_minus_one);
    sps_bits(&sps, 1, 1);    /* frame_mbs_only_flag */
    sps_bits(&sps, 1, 1);    /* direct_8x8_inference_flag */
    sps_bits(&sps, 0, 1);    /* frame_cropping_flag */
    sps_bits(&sps, 0, 1);    /* vui_parameters_present_flag */
    sps_bits(&sps, 1, 1);    /* rbsp_stop_one_bit */
    size_t length = (sps.bit + 7u) / 8u;
    memcpy(output, sps.bytes, length);
    return length;
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
    put_u32(fixture->bytes + start,
            (uint32_t) (fixture->length - start));
}

static void append_u32(Fixture *fixture, uint32_t value)
{
    put_u32(fixture->bytes + fixture->length, value);
    fixture->length += 4;
}

static bool fixture_set_first_mdhd_timescale(
    Fixture *fixture, uint32_t timescale)
{
    static const unsigned char mdhd[] = {'m', 'd', 'h', 'd'};
    for (size_t at = 4; at + sizeof(mdhd) <= fixture->length; at++) {
        if (memcmp(fixture->bytes + at, mdhd, sizeof(mdhd)) != 0)
            continue;
        size_t payload = at + sizeof(mdhd);
        if (payload >= fixture->length) return false;
        unsigned version = fixture->bytes[payload];
        size_t offset = version == 1 ? 20u : 12u;
        if (version > 1 || offset > fixture->length - payload
            || fixture->length - payload - offset < 4u) return false;
        put_u32(fixture->bytes + payload + offset, timescale);
        return true;
    }
    return false;
}

static bool fixture_set_first_box_payload_u32(
    Fixture *fixture, uint32_t type, size_t payload_offset, uint32_t value)
{
    if (fixture == NULL) return false;
    unsigned char encoded_type[4];
    put_u32(encoded_type, type);
    for (size_t at = 4u; at + 4u <= fixture->length; at++) {
        if (memcmp(fixture->bytes + at, encoded_type, 4u) != 0)
            continue;
        size_t payload = at + 4u;
        if (payload > fixture->length
            || payload_offset > fixture->length - payload
            || fixture->length - payload - payload_offset < 4u) {
            return false;
        }
        put_u32(fixture->bytes + payload + payload_offset, value);
        return true;
    }
    return false;
}

static size_t make_fixture(Fixture *fixture)
{
    memset(fixture, 0, sizeof(*fixture));
    size_t ftyp = begin_box(
        fixture, MEDIA_MP4_FOURCC('f','t','y','p'));
    append_u32(fixture, MEDIA_MP4_FOURCC('i','s','o','m'));
    append_u32(fixture, 0);
    end_box(fixture, ftyp);

    size_t moov = begin_box(
        fixture, MEDIA_MP4_FOURCC('m','o','o','v'));
    size_t trak = begin_box(
        fixture, MEDIA_MP4_FOURCC('t','r','a','k'));
    size_t mdia = begin_box(
        fixture, MEDIA_MP4_FOURCC('m','d','i','a'));
    size_t mdhd = begin_box(
        fixture, MEDIA_MP4_FOURCC('m','d','h','d'));
    append_u32(fixture, 0);
    append_u32(fixture, 0);
    append_u32(fixture, 0);
    append_u32(fixture, 1000);
    append_u32(fixture, 2000);
    append_u32(fixture, 0);
    end_box(fixture, mdhd);
    size_t hdlr = begin_box(
        fixture, MEDIA_MP4_FOURCC('h','d','l','r'));
    append_u32(fixture, 0);
    append_u32(fixture, 0);
    append_u32(fixture, MEDIA_MP4_FOURCC('v','i','d','e'));
    end_box(fixture, hdlr);
    size_t minf = begin_box(
        fixture, MEDIA_MP4_FOURCC('m','i','n','f'));
    size_t stbl = begin_box(
        fixture, MEDIA_MP4_FOURCC('s','t','b','l'));

    size_t stsd = begin_box(
        fixture, MEDIA_MP4_FOURCC('s','t','s','d'));
    append_u32(fixture, 0);
    append_u32(fixture, 1);
    size_t avc1 = begin_box(
        fixture, MEDIA_MP4_FOURCC('a','v','c','1'));
    fixture->length += 78;
    fixture->bytes[avc1 + 32] = 0x01;
    fixture->bytes[avc1 + 33] = 0x40;
    fixture->bytes[avc1 + 34] = 0x00;
    fixture->bytes[avc1 + 35] = 0xf0;
    size_t avcc = begin_box(
        fixture, MEDIA_MP4_FOURCC('a','v','c','C'));
    static const unsigned char config[] = {
        1, 0x42, 0, 0x1e, 0xff, 0xe1, 0
    };
    memcpy(fixture->bytes + fixture->length, config, sizeof(config));
    fixture->length += sizeof(config);
    end_box(fixture, avcc);
    end_box(fixture, avc1);
    end_box(fixture, stsd);

    size_t stts = begin_box(
        fixture, MEDIA_MP4_FOURCC('s','t','t','s'));
    append_u32(fixture, 0);
    append_u32(fixture, 1);
    append_u32(fixture, 2);
    append_u32(fixture, 1000);
    end_box(fixture, stts);

    size_t stsc = begin_box(
        fixture, MEDIA_MP4_FOURCC('s','t','s','c'));
    append_u32(fixture, 0);
    append_u32(fixture, 1);
    append_u32(fixture, 1);
    append_u32(fixture, 2);
    append_u32(fixture, 1);
    end_box(fixture, stsc);

    size_t stsz = begin_box(
        fixture, MEDIA_MP4_FOURCC('s','t','s','z'));
    append_u32(fixture, 0);
    append_u32(fixture, 0);
    append_u32(fixture, 2);
    append_u32(fixture, 3);
    append_u32(fixture, 4);
    end_box(fixture, stsz);

    size_t stco = begin_box(
        fixture, MEDIA_MP4_FOURCC('s','t','c','o'));
    append_u32(fixture, 0);
    append_u32(fixture, 1);
    size_t chunk_offset = fixture->length;
    append_u32(fixture, 0);
    end_box(fixture, stco);

    size_t stss = begin_box(
        fixture, MEDIA_MP4_FOURCC('s','t','s','s'));
    append_u32(fixture, 0);
    append_u32(fixture, 1);
    append_u32(fixture, 1);
    end_box(fixture, stss);

    end_box(fixture, stbl);
    end_box(fixture, minf);
    end_box(fixture, mdia);
    end_box(fixture, trak);
    end_box(fixture, moov);

    size_t mdat = begin_box(
        fixture, MEDIA_MP4_FOURCC('m','d','a','t'));
    put_u32(fixture->bytes + chunk_offset, (uint32_t) fixture->length);
    static const unsigned char payload[] = {1, 2, 3, 4, 5, 6, 7};
    memcpy(fixture->bytes + fixture->length, payload, sizeof(payload));
    fixture->length += sizeof(payload);
    end_box(fixture, mdat);
    return mdat + 8;
}

static size_t make_fragmented_fixture_with_duration(
    Fixture *fixture, uint32_t decode_time,
    uint32_t sample_duration,
    size_t *moof_offset_out, size_t *moof_length_out)
{
    memset(fixture, 0, sizeof(*fixture));
    size_t ftyp = begin_box(
        fixture, MEDIA_MP4_FOURCC('f','t','y','p'));
    append_u32(fixture, MEDIA_MP4_FOURCC('i','s','o','m'));
    append_u32(fixture, 0);
    end_box(fixture, ftyp);

    size_t moov = begin_box(
        fixture, MEDIA_MP4_FOURCC('m','o','o','v'));
    size_t mvex = begin_box(
        fixture, MEDIA_MP4_FOURCC('m','v','e','x'));
    size_t trex = begin_box(
        fixture, MEDIA_MP4_FOURCC('t','r','e','x'));
    append_u32(fixture, 0);
    append_u32(fixture, 1);
    append_u32(fixture, 1);
    append_u32(fixture, sample_duration);
    append_u32(fixture, 0);
    append_u32(fixture, 0);
    end_box(fixture, trex);
    end_box(fixture, mvex);

    size_t trak = begin_box(
        fixture, MEDIA_MP4_FOURCC('t','r','a','k'));
    size_t tkhd = begin_box(
        fixture, MEDIA_MP4_FOURCC('t','k','h','d'));
    append_u32(fixture, 0);
    append_u32(fixture, 0);
    append_u32(fixture, 0);
    append_u32(fixture, 1);
    append_u32(fixture, 0);
    end_box(fixture, tkhd);
    size_t mdia = begin_box(
        fixture, MEDIA_MP4_FOURCC('m','d','i','a'));
    size_t mdhd = begin_box(
        fixture, MEDIA_MP4_FOURCC('m','d','h','d'));
    append_u32(fixture, 0);
    append_u32(fixture, 0);
    append_u32(fixture, 0);
    append_u32(fixture, 1000);
    append_u32(fixture, sample_duration * 2u);
    append_u32(fixture, 0);
    end_box(fixture, mdhd);
    size_t hdlr = begin_box(
        fixture, MEDIA_MP4_FOURCC('h','d','l','r'));
    append_u32(fixture, 0);
    append_u32(fixture, 0);
    append_u32(fixture, MEDIA_MP4_FOURCC('v','i','d','e'));
    end_box(fixture, hdlr);
    size_t minf = begin_box(
        fixture, MEDIA_MP4_FOURCC('m','i','n','f'));
    size_t stbl = begin_box(
        fixture, MEDIA_MP4_FOURCC('s','t','b','l'));

    size_t stsd = begin_box(
        fixture, MEDIA_MP4_FOURCC('s','t','s','d'));
    append_u32(fixture, 0);
    append_u32(fixture, 1);
    size_t avc1 = begin_box(
        fixture, MEDIA_MP4_FOURCC('a','v','c','1'));
    fixture->length += 78;
    fixture->bytes[avc1 + 32] = 0x01;
    fixture->bytes[avc1 + 33] = 0x40;
    fixture->bytes[avc1 + 34] = 0x00;
    fixture->bytes[avc1 + 35] = 0xf0;
    size_t avcc = begin_box(
        fixture, MEDIA_MP4_FOURCC('a','v','c','C'));
    static const unsigned char config[] = {
        1, 0x42, 0, 0x1e, 0xff, 0xe1, 0
    };
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
    for (size_t at = 0;
         at < sizeof(empty_tables) / sizeof(empty_tables[0]); at++) {
        size_t table = begin_box(fixture, empty_tables[at]);
        append_u32(fixture, 0);
        append_u32(fixture, 0);
        end_box(fixture, table);
    }
    size_t stsz = begin_box(
        fixture, MEDIA_MP4_FOURCC('s','t','s','z'));
    append_u32(fixture, 0);
    append_u32(fixture, 0);
    append_u32(fixture, 0);
    end_box(fixture, stsz);
    end_box(fixture, stbl);
    end_box(fixture, minf);
    end_box(fixture, mdia);
    end_box(fixture, trak);
    end_box(fixture, moov);

    size_t moof = begin_box(
        fixture, MEDIA_MP4_FOURCC('m','o','o','f'));
    size_t mfhd = begin_box(
        fixture, MEDIA_MP4_FOURCC('m','f','h','d'));
    append_u32(fixture, 0);
    append_u32(fixture, 1);
    end_box(fixture, mfhd);
    size_t traf = begin_box(
        fixture, MEDIA_MP4_FOURCC('t','r','a','f'));
    size_t tfhd = begin_box(
        fixture, MEDIA_MP4_FOURCC('t','f','h','d'));
    append_u32(fixture, UINT32_C(0x0002000a));
    append_u32(fixture, 1);
    append_u32(fixture, 1);
    append_u32(fixture, sample_duration);
    end_box(fixture, tfhd);
    size_t tfdt = begin_box(
        fixture, MEDIA_MP4_FOURCC('t','f','d','t'));
    append_u32(fixture, 0);
    append_u32(fixture, decode_time);
    end_box(fixture, tfdt);
    size_t trun = begin_box(
        fixture, MEDIA_MP4_FOURCC('t','r','u','n'));
    append_u32(fixture, UINT32_C(0x00000701));
    append_u32(fixture, 2);
    size_t data_offset = fixture->length;
    append_u32(fixture, 0);
    append_u32(fixture, sample_duration);
    append_u32(fixture, 3);
    append_u32(fixture, 0);
    append_u32(fixture, sample_duration);
    append_u32(fixture, 4);
    append_u32(fixture, UINT32_C(0x00010000));
    end_box(fixture, trun);
    end_box(fixture, traf);
    end_box(fixture, moof);
    if (moof_offset_out != NULL) *moof_offset_out = moof;
    if (moof_length_out != NULL)
        *moof_length_out = fixture->length - moof;

    size_t mdat = begin_box(
        fixture, MEDIA_MP4_FOURCC('m','d','a','t'));
    put_u32(fixture->bytes + data_offset,
            (uint32_t) (fixture->length - moof));
    static const unsigned char payload[] = {1, 2, 3, 4, 5, 6, 7};
    memcpy(fixture->bytes + fixture->length, payload, sizeof(payload));
    fixture->length += sizeof(payload);
    end_box(fixture, mdat);
    return mdat + 8;
}

static size_t make_fragmented_fixture(
    Fixture *fixture, uint32_t decode_time,
    size_t *moof_offset_out, size_t *moof_length_out)
{
    return make_fragmented_fixture_with_duration(
        fixture, decode_time, 1000,
        moof_offset_out, moof_length_out);
}

static bool make_sidx_fragmented_fixture(
    Fixture *fixture, size_t *second_moof_offset,
    size_t *second_moof_length, size_t *first_payload,
    size_t *second_payload)
{
    Fixture first;
    Fixture second;
    size_t first_moof = 0;
    size_t first_moof_bytes = 0;
    size_t second_moof = 0;
    size_t second_moof_bytes = 0;
    size_t first_payload_source = make_fragmented_fixture(
        &first, 0, &first_moof, &first_moof_bytes);
    size_t second_payload_source = make_fragmented_fixture(
        &second, 2000, &second_moof, &second_moof_bytes);
    memset(fixture, 0, sizeof(*fixture));
    memcpy(fixture->bytes, first.bytes, first_moof);
    fixture->length = first_moof;

    size_t sidx = begin_box(
        fixture, MEDIA_MP4_FOURCC('s','i','d','x'));
    append_u32(fixture, 0);       /* version + flags */
    append_u32(fixture, 1);       /* reference ID */
    append_u32(fixture, 1000);    /* timescale */
    append_u32(fixture, 0);       /* earliest presentation time */
    append_u32(fixture, 0);       /* first offset */
    append_u32(fixture, 2);       /* reserved + reference count */
    size_t first_size_field = fixture->length;
    append_u32(fixture, 0);
    append_u32(fixture, 2000);
    append_u32(fixture, UINT32_C(0x90000000));
    size_t second_size_field = fixture->length;
    append_u32(fixture, 0);
    append_u32(fixture, 2000);
    append_u32(fixture, UINT32_C(0x90000000));
    end_box(fixture, sidx);

    size_t first_segment = fixture->length;
    size_t first_segment_bytes = first.length - first_moof;
    size_t second_segment_bytes = second.length - second_moof;
    if (first_segment_bytes > sizeof(fixture->bytes) - fixture->length
        || second_segment_bytes
           > sizeof(fixture->bytes)
             - fixture->length - first_segment_bytes) {
        return false;
    }
    memcpy(
        fixture->bytes + fixture->length,
        first.bytes + first_moof, first_segment_bytes);
    fixture->length += first_segment_bytes;
    size_t second_segment = fixture->length;
    memcpy(
        fixture->bytes + fixture->length,
        second.bytes + second_moof, second_segment_bytes);
    fixture->length += second_segment_bytes;
    put_u32(
        fixture->bytes + first_size_field,
        (uint32_t) first_segment_bytes);
    put_u32(
        fixture->bytes + second_size_field,
        (uint32_t) second_segment_bytes);
    if (second_moof_offset != NULL)
        *second_moof_offset = second_segment;
    if (second_moof_length != NULL)
        *second_moof_length = second_moof_bytes;
    if (first_payload != NULL)
        *first_payload =
            first_segment + first_payload_source - first_moof;
    if (second_payload != NULL)
        *second_payload =
            second_segment + second_payload_source - second_moof;
    return true;
}

static bool make_long_sidx_fragmented_fixture(
    Fixture *fixture, size_t *last_moof_offset,
    size_t *last_moof_length)
{
    enum { FRAGMENT_COUNT = 21 };
    const uint32_t sample_duration = 30000;
    const uint32_t fragment_duration = sample_duration * 2u;
    Fixture fragment;
    size_t moof = 0;
    size_t moof_length = 0;
    (void) make_fragmented_fixture_with_duration(
        &fragment, 0, sample_duration, &moof, &moof_length);
    memset(fixture, 0, sizeof(*fixture));
    memcpy(fixture->bytes, fragment.bytes, moof);
    fixture->length = moof;

    size_t sidx = begin_box(
        fixture, MEDIA_MP4_FOURCC('s','i','d','x'));
    append_u32(fixture, 0);
    append_u32(fixture, 1);
    append_u32(fixture, 1000);
    append_u32(fixture, 0);
    append_u32(fixture, 0);
    append_u32(fixture, FRAGMENT_COUNT);
    size_t size_fields[FRAGMENT_COUNT];
    for (size_t i = 0; i < FRAGMENT_COUNT; i++) {
        size_fields[i] = fixture->length;
        append_u32(fixture, 0);
        append_u32(fixture, fragment_duration);
        append_u32(fixture, UINT32_C(0x90000000));
    }
    end_box(fixture, sidx);

    for (size_t i = 0; i < FRAGMENT_COUNT; i++) {
        size_t payload = make_fragmented_fixture_with_duration(
            &fragment, (uint32_t) i * fragment_duration,
            sample_duration, &moof, &moof_length);
        (void) payload;
        size_t segment_bytes = fragment.length - moof;
        if (segment_bytes > sizeof(fixture->bytes) - fixture->length)
            return false;
        size_t destination = fixture->length;
        memcpy(
            fixture->bytes + destination,
            fragment.bytes + moof, segment_bytes);
        fixture->length += segment_bytes;
        put_u32(
            fixture->bytes + size_fields[i],
            (uint32_t) segment_bytes);
        if (i + 1u == FRAGMENT_COUNT) {
            if (last_moof_offset != NULL)
                *last_moof_offset = destination;
            if (last_moof_length != NULL)
                *last_moof_length = moof_length;
        }
    }
    return true;
}

static bool fixture_set_trun_first_sample_flags(
    Fixture *fixture, size_t occurrence, uint32_t flags)
{
    static const unsigned char trun[] = {'t', 'r', 'u', 'n'};
    size_t seen = 0;
    for (size_t at = 4; at + 28u <= fixture->length; at++) {
        if (memcmp(fixture->bytes + at, trun, sizeof(trun)) != 0)
            continue;
        if (seen++ != occurrence) continue;
        put_u32(fixture->bytes + at + 24u, flags);
        return true;
    }
    return false;
}

static bool fixture_collapse_sidx_to_one_reference(Fixture *fixture)
{
    static const unsigned char sidx[] = {'s', 'i', 'd', 'x'};
    for (size_t at = 4; at + sizeof(sidx) <= fixture->length; at++) {
        if (memcmp(fixture->bytes + at, sidx, sizeof(sidx)) != 0)
            continue;
        size_t box_start = at - 4u;
        uint32_t box_size =
            ((uint32_t) fixture->bytes[box_start] << 24u)
            | ((uint32_t) fixture->bytes[box_start + 1u] << 16u)
            | ((uint32_t) fixture->bytes[box_start + 2u] << 8u)
            | fixture->bytes[box_start + 3u];
        if (box_size < 44u || box_size > fixture->length - box_start)
            return false;
        size_t segment_start = box_start + box_size;
        size_t segment_bytes = fixture->length - segment_start;
        if (segment_bytes == 0 || segment_bytes > UINT32_C(0x7fffffff))
            return false;
        put_u32(fixture->bytes + box_start + 28u, 1);
        put_u32(
            fixture->bytes + box_start + 32u,
            (uint32_t) segment_bytes);
        return true;
    }
    return false;
}

static uint64_t media_test_random(uint64_t *state)
{
    uint64_t value = *state;
    value ^= value << 13u;
    value ^= value >> 7u;
    value ^= value << 17u;
    *state = value;
    return value;
}

static bool fixture_read(void *opaque, uint64_t offset,
                         void *destination, size_t length)
{
    FixtureReader *reader = opaque;
    if (offset > reader->length || length > reader->length - offset)
        return false;
    if (reader->watch_length != 0
        && offset == reader->watch_offset
        && length == reader->watch_length) {
        reader->watch_reads++;
    }
    if (reader->one_shot_length != 0
        && offset == reader->one_shot_offset
        && length == reader->one_shot_length
        && reader->one_shot_reads++ != 0) {
        return false;
    }
    if (reader->fail_enabled && offset == reader->fail_offset)
        return false;
    if (reader->cancel_on_read != NULL
        && offset == reader->cancel_offset) {
        tilefinch_cancellation_request(reader->cancel_on_read);
        return false;
    }
    memcpy(destination, reader->bytes + (size_t) offset, length);
    reader->reads++;
    reader->bytes_read += length;
    return true;
}

static MediaRangeReadStatus fixture_poll(void *opaque, uint64_t offset,
                                         void *destination, size_t length)
{
    FixtureReader *reader = opaque;
    reader->polls++;
    if (reader->block_polls != 0) {
        reader->block_polls--;
        return MEDIA_RANGE_READ_WOULD_BLOCK;
    }
    if (reader->poll_window_split != 0) {
        unsigned wanted = offset >= reader->poll_window_split ? 1u : 0u;
        if (!reader->poll_window_valid || reader->poll_window != wanted) {
            reader->poll_window = wanted;
            reader->poll_window_valid = true;
            reader->poll_window_switches++;
            return MEDIA_RANGE_READ_WOULD_BLOCK;
        }
    }
    return fixture_read(opaque, offset, destination, length)
        ? MEDIA_RANGE_READ_COMPLETE : MEDIA_RANGE_READ_FAILED;
}

/*
 * The observation form of the same answer, and it has to agree with
 * fixture_poll or the instrumentation built on it would be told bytes are in
 * hand at the exact moments the next poll is going to refuse them.
 */
static bool fixture_resident(void *opaque, uint64_t offset, size_t length)
{
    const FixtureReader *reader = opaque;
    (void) length;
    if (reader == NULL || reader->block_polls != 0) return false;
    if (reader->poll_window_split == 0) return true;
    unsigned wanted = offset >= reader->poll_window_split ? 1u : 0u;
    return reader->poll_window_valid && reader->poll_window == wanted;
}

static bool validate_device_media_fixture(
    const char *path, uint16_t expected_width, uint16_t expected_height)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL || fseek(file, 0, SEEK_END) != 0) {
        if (file != NULL) fclose(file);
        return false;
    }
    long end = ftell(file);
    if (end <= 0 || end > 512 * 1024 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }
    unsigned char *bytes = malloc((size_t) end);
    bool loaded = bytes != NULL
        && fread(bytes, 1, (size_t) end, file) == (size_t) end;
    loaded = fclose(file) == 0 && loaded;
    if (!loaded) {
        free(bytes);
        return false;
    }
    Budget budget;
    budget_init(&budget, 4 * 1024 * 1024);
    FixtureReader fixture_reader = {
        .bytes = bytes,
        .length = (size_t) end
    };
    MediaRangeReader reader = {
        .opaque = &fixture_reader,
        .length = (size_t) end,
        .read = fixture_read,
        .describe_failure = NULL
    };
    char error[256] = {0};
    MediaMp4Demux *demux = media_mp4_open(
        &budget, &reader, NULL, error, sizeof(error));
    bool video = false, audio = false, admitted_video_sample = false;
    MediaMp4TrackInfo video_info = {0};
    if (demux != NULL) {
        for (size_t at = 0; at < media_mp4_track_count(demux); at++) {
            MediaMp4TrackInfo info;
            if (!media_mp4_track_info(demux, at, &info)) continue;
            uint16_t coded_width = 0, coded_height = 0;
            uint8_t nal_length_size = 0;
            if (info.kind == MEDIA_MP4_TRACK_VIDEO
                && info.codec == MEDIA_MP4_FOURCC('a','v','c','1')
                && info.width == expected_width
                && info.height == expected_height
                && media_h264_avcc_dimensions(
                       info.codec_config, info.codec_config_length,
                       &coded_width, &coded_height, &nal_length_size)
                && coded_width == expected_width
                && coded_height == expected_height
                && nal_length_size == info.nal_length_size) {
                video = true;
                video_info = info;
            } else if (info.kind == MEDIA_MP4_TRACK_AUDIO
                       && info.codec == MEDIA_MP4_FOURCC('m','p','4','a')
                       && info.sample_rate == 48000u) {
                MediaAacStreamInfo stream = {0};
                audio = media_aac_esds_stream_info(
                            info.codec_config,
                            info.codec_config_length, &stream)
                     && stream.sample_rate == info.sample_rate
                     && stream.channels == info.channels
                     && stream.samples_per_frame
                            == PSP_MEDIA_AUDIO_SAMPLES;
            }
        }
        unsigned char *sample_bytes = malloc(1024u * 1024u);
        for (unsigned samples = 0; sample_bytes != NULL && samples < 32u;
             samples++) {
            MediaMp4Sample sample;
            if (!media_mp4_next_sample(demux, &sample)) break;
            if (sample.kind != MEDIA_MP4_TRACK_VIDEO) continue;
            if (sample.size > 1024u * 1024u
                || !media_mp4_read_sample(
                       demux, &sample, sample_bytes, 1024u * 1024u)) break;
            admitted_video_sample = media_h264_avcc_sample_is_admitted(
                sample_bytes, sample.size, video_info.nal_length_size,
                expected_width, expected_height,
                video_info.codec_config, video_info.codec_config_length);
            break;
        }
        free(sample_bytes);
        media_mp4_close(demux);
    }
    bool okay = video && audio && admitted_video_sample
        && budget.current == 0;
    free(bytes);
    return okay;
}

static bool fixture_describe_failure(void *opaque, char *error,
                                     size_t error_size)
{
    FixtureReader *reader = opaque;
    if (reader == NULL || !reader->fail_enabled
        || error == NULL || error_size == 0) return false;
    snprintf(error, error_size, "fixture transport timeout");
    return true;
}

int main(void)
{
    puts("test: embedded PSP decoder fixtures are admissible");
    CHECK(validate_device_media_fixture(
              TILEFINCH_TEST_MEDIA_FIXTURE_240, 320, 240));
    CHECK(validate_device_media_fixture(
              TILEFINCH_TEST_MEDIA_FIXTURE_360, 640, 360));
    puts("test: lazy sidx is confined to globally ordered single-track files");
    CHECK(!media_mp4_lazy_sidx_admitted(0)
          && media_mp4_lazy_sidx_admitted(1)
          && !media_mp4_lazy_sidx_admitted(2));

    MediaMp4Limits default_limits = media_mp4_default_limits();
    CHECK(default_limits.maximum_table_entries == 131072
          && default_limits.maximum_track_table_bytes
              == 2u * 1024u * 1024u);
    static const unsigned char aac_lc_esds[] = {
        0, 0, 0, 0,
        0x03, 0x19, 0, 1, 0,
        0x04, 0x11, 0x40, 0x15, 0, 0, 0,
        0, 1, 0xf4, 0, 0, 1, 0xf4, 0,
        0x05, 0x02, 0x12, 0x10,
        0x06, 0x01, 0x02
    };
    unsigned char he_aac_esds[sizeof(aac_lc_esds)];
    unsigned char short_frame_aac_esds[sizeof(aac_lc_esds)];
    memcpy(he_aac_esds, aac_lc_esds, sizeof(he_aac_esds));
    memcpy(short_frame_aac_esds, aac_lc_esds, sizeof(aac_lc_esds));
    he_aac_esds[26] = 0x28;
    short_frame_aac_esds[27] |= 0x04u;
    MediaAacStreamInfo aac_info = {0};
    CHECK(media_aac_esds_stream_info(
              aac_lc_esds, sizeof(aac_lc_esds), &aac_info)
          && aac_info.sample_rate == 44100u
          && aac_info.channels == 2u
          && aac_info.samples_per_frame == 1024u
          && media_aac_esds_is_low_complexity(
              aac_lc_esds, sizeof(aac_lc_esds))
          && !media_aac_esds_is_low_complexity(
              he_aac_esds, sizeof(he_aac_esds))
          && media_aac_esds_stream_info(
              short_frame_aac_esds, sizeof(short_frame_aac_esds),
              &aac_info)
          && aac_info.samples_per_frame == 960u
          && !media_aac_esds_is_low_complexity(
              short_frame_aac_esds, sizeof(short_frame_aac_esds))
          && !media_aac_esds_is_low_complexity(
              aac_lc_esds, 8));

    puts("test: AVC SPS geometry governs decoder admission");
    unsigned char admitted_sps[64] = {0};
    unsigned char oversized_sps[64] = {0};
    size_t admitted_length = make_baseline_sps(
        admitted_sps, 29u, 16u);
    size_t oversized_length = make_baseline_sps(
        oversized_sps, 79u, 44u);
    uint16_t coded_width = 0, coded_height = 0;
    CHECK(media_h264_sps_dimensions(
              admitted_sps, admitted_length,
              &coded_width, &coded_height)
          && coded_width == 480 && coded_height == 272
          && media_h264_sps_dimensions(
              oversized_sps, oversized_length,
              &coded_width, &coded_height)
          && coded_width == 1280 && coded_height == 720);
    unsigned char avcc[160] = {1, 66, 0, 30, 0xff, 0xe1};
    avcc[6] = (unsigned char) (admitted_length >> 8u);
    avcc[7] = (unsigned char) admitted_length;
    memcpy(avcc + 8, admitted_sps, admitted_length);
    size_t avcc_length = 8u + admitted_length;
    avcc[avcc_length++] = 1;
    avcc[avcc_length++] = 0;
    avcc[avcc_length++] = 1;
    avcc[avcc_length++] = 0x68;
    uint8_t nal_length_size = 0;
    CHECK(media_h264_avcc_dimensions(
              avcc, avcc_length, &coded_width, &coded_height,
              &nal_length_size)
          && coded_width == 480 && coded_height == 272
          && nal_length_size == 4);
    uint8_t profile_idc = 0;
    CHECK(media_h264_avcc_decoder_route(
              avcc, avcc_length, &profile_idc)
              == MEDIA_H264_DECODER_ROUTE_PSP_FIRMWARE
          && profile_idc == 66u);
    unsigned char high_avcc[sizeof(avcc)];
    memcpy(high_avcc, avcc, avcc_length);
    high_avcc[1] = 100u;
    high_avcc[9] = 100u;
    CHECK(media_h264_avcc_decoder_route(
              high_avcc, avcc_length, &profile_idc)
              == MEDIA_H264_DECODER_ROUTE_HIGH_EXTENSION
          && profile_idc == 100u);
    CHECK(media_h264_avcc_decoder_route(
              high_avcc, 9u, &profile_idc)
              == MEDIA_H264_DECODER_ROUTE_UNSUPPORTED);
    CHECK(media_h264_codec_string_decoder_route(
              "video/mp4; codecs=\"avc1.64000d\"", &profile_idc)
              == MEDIA_H264_DECODER_ROUTE_HIGH_EXTENSION
          && profile_idc == 100u);
    CHECK(media_h264_codec_string_decoder_route(
              "video/mp4; codecs=\"avc1.4d401e\"", &profile_idc)
              == MEDIA_H264_DECODER_ROUTE_PSP_FIRMWARE
          && profile_idc == 77u);
    CHECK(media_h264_codec_string_decoder_route(
              "video/mp4; codecs=\"vp09.00.10.08\"", &profile_idc)
              == MEDIA_H264_DECODER_ROUTE_UNSUPPORTED);
    unsigned char mismatched_avcc[sizeof(avcc)];
    memcpy(mismatched_avcc, avcc, avcc_length);
    mismatched_avcc[1] = 77u;
    CHECK(!media_h264_avcc_dimensions(
        mismatched_avcc, avcc_length,
        &coded_width, &coded_height, &nal_length_size));
    memcpy(mismatched_avcc, avcc, avcc_length);
    mismatched_avcc[8] |= 0x80u;
    CHECK(!media_h264_avcc_dimensions(
        mismatched_avcc, avcc_length,
        &coded_width, &coded_height, &nal_length_size));
    CHECK(!media_h264_avcc_dimensions(
        avcc, avcc_length - 4u, &coded_width, &coded_height,
        &nal_length_size));
    unsigned char admitted_access_unit[80] = {0};
    put_u32(admitted_access_unit, (uint32_t) admitted_length);
    memcpy(
        admitted_access_unit + 4u, admitted_sps, admitted_length);
    CHECK(media_h264_avcc_sample_is_admitted(
              admitted_access_unit, admitted_length + 4u,
              4, 480, 272, avcc, avcc_length)
          && !media_h264_avcc_sample_is_admitted(
              admitted_access_unit, admitted_length + 3u,
              4, 480, 272, avcc, avcc_length)
          && !media_h264_avcc_sample_is_admitted(
              admitted_access_unit, admitted_length + 4u,
              3, 480, 272, avcc, avcc_length));
    put_u32(admitted_access_unit, (uint32_t) oversized_length);
    memcpy(
        admitted_access_unit + 4u, oversized_sps, oversized_length);
    CHECK(!media_h264_avcc_sample_is_admitted(
        admitted_access_unit, oversized_length + 4u,
        4, 480, 272, avcc, avcc_length));
    unsigned char annexb_access_unit[96] = {0, 0, 0, 1};
    memcpy(annexb_access_unit + 4u, admitted_sps, admitted_length);
    CHECK(media_h264_annexb_sample_is_admitted(
              annexb_access_unit, admitted_length + 4u, 480, 272)
          && !media_h264_annexb_sample_is_admitted(
              annexb_access_unit, admitted_length + 4u, 432, 240));
    memcpy(annexb_access_unit + 4u, oversized_sps, oversized_length);
    CHECK(!media_h264_annexb_sample_is_admitted(
              annexb_access_unit, oversized_length + 4u, 480, 272)
          && !media_h264_annexb_sample_is_admitted(
              annexb_access_unit + 4u, oversized_length, 480, 272));
    static const unsigned char annexb_slice[] = {
        0, 0, 1, 0x65, 0x88, 0x84
    };
    CHECK(media_h264_annexb_sample_is_admitted(
              annexb_slice, sizeof(annexb_slice), 432, 240));
    unsigned char changed_sps[sizeof(admitted_sps)];
    memcpy(changed_sps, admitted_sps, admitted_length);
    changed_sps[2] ^= 0x40u;
    put_u32(admitted_access_unit, (uint32_t) admitted_length);
    memcpy(
        admitted_access_unit + 4u, changed_sps, admitted_length);
    CHECK(!media_h264_avcc_sample_is_admitted(
        admitted_access_unit, admitted_length + 4u,
        4, 480, 272, avcc, avcc_length));
    put_u32(admitted_access_unit, 1u);
    admitted_access_unit[4] = 0x68u;
    CHECK(media_h264_avcc_sample_is_admitted(
              admitted_access_unit, 5u, 4, 480, 272,
              avcc, avcc_length));
    admitted_access_unit[4] = 0x48u;
    CHECK(!media_h264_avcc_sample_is_admitted(
              admitted_access_unit, 5u, 4, 480, 272,
              avcc, avcc_length));
    admitted_access_unit[4] = 0xe8u;
    CHECK(!media_h264_avcc_sample_is_admitted(
              admitted_access_unit, 5u, 4, 480, 272,
              avcc, avcc_length));
    admitted_access_unit[4] = 0x6du;
    CHECK(!media_h264_avcc_sample_is_admitted(
              admitted_access_unit, 5u, 4, 480, 272,
              avcc, avcc_length));

    unsigned char reserved_nal_length_avcc[sizeof(avcc)];
    memcpy(reserved_nal_length_avcc, avcc, avcc_length);
    reserved_nal_length_avcc[4] =
        (unsigned char) ((reserved_nal_length_avcc[4] & 0xfcu) | 2u);
    CHECK(!media_h264_avcc_dimensions(
        reserved_nal_length_avcc, avcc_length,
        &coded_width, &coded_height, &nal_length_size));

    unsigned char http_bytes[12288];
    for (size_t i = 0; i < sizeof(http_bytes); i++)
        http_bytes[i] = (unsigned char) (i * 31u + 7u);
    HttpFixture http_fixture = {
        .bytes = http_bytes,
        .length = sizeof(http_bytes)
    };
    Budget http_budget;
    budget_init(&http_budget, 64u * 1024u);
    char range_url[256];
    CHECK(media_http_build_range_url(
              "https://media.invalid/video.mp4?foo=1&range=0-9&bar=2#part",
              100, 199, range_url, sizeof(range_url))
          && strcmp(
              range_url,
              "https://media.invalid/video.mp4?foo=1&bar=2&range=100-199#part")
                 == 0);
    CHECK(media_http_build_range_url(
              "https://media.invalid/video.mp4?range=0-9",
              0, 4095, range_url, sizeof(range_url))
          && strcmp(
              range_url,
              "https://media.invalid/video.mp4?range=0-4095") == 0);
    CHECK(!media_http_build_range_url(
              "https://media.invalid/video.mp4", 9, 8,
              range_url, sizeof(range_url)));
    MediaHttpRangeOptions http_options = {
        .cache_bytes = 4096,
        .url_validator = http_fixture_url_admitted,
        .transport = http_fixture_range,
        .transport_opaque = &http_fixture
    };
    char http_error[256];
    MediaHttpRange *http = media_http_range_create(
        &http_budget, NULL, "https://media.invalid/video.mp4",
        sizeof(http_bytes), &http_options, http_error, sizeof(http_error));
    CHECK(http != NULL);
    size_t admitted_http_bytes = http_budget.current;
    CHECK(media_http_range_create(
              &http_budget, NULL, "https://redirect.invalid/video.mp4",
              sizeof(http_bytes), &http_options,
              http_error, sizeof(http_error)) == NULL
          && http_budget.current == admitted_http_bytes);
    MediaRangeReader http_reader = media_http_range_reader(http);
    unsigned char http_output[256];
    CHECK(http_reader.read(http_reader.opaque, 100, http_output, 100)
          && memcmp(http_output, http_bytes + 100, 100) == 0);
    CHECK(http_reader.read(http_reader.opaque, 150, http_output, 100)
          && http_fixture.requests == 1);
    CHECK(http_reader.read(http_reader.opaque, 4050, http_output, 100)
          && memcmp(http_output, http_bytes + 4050, 100) == 0
          && http_fixture.requests == 2);
    MediaHttpRangeStats http_stats;
    CHECK(media_http_range_stats(http, &http_stats)
          && http_stats.requests == 2
          && http_stats.cache_hits >= 1
          /* The opaque range object owns bounded scheduler, streamed-header,
             and diagnostic state in addition to this 4 KiB byte window. */
          && http_stats.retained_bytes < 6u * 1024u);
    media_http_range_destroy(http);
    CHECK(http_budget.current == 0);

    HttpFixture retry_fixture = {
        .bytes = http_bytes,
        .length = sizeof(http_bytes),
        .failures_remaining = 1
    };
    http_options.transport_opaque = &retry_fixture;
    http = media_http_range_create(
        &http_budget, NULL, "https://media.invalid/retry.mp4",
        sizeof(http_bytes), &http_options, http_error, sizeof(http_error));
    CHECK(http != NULL);
    http_reader = media_http_range_reader(http);
    CHECK(http_reader.read(http_reader.opaque, 0, http_output, 100)
          && retry_fixture.requests == 2
          && media_http_range_stats(http, &http_stats)
          && http_stats.requests == 2
          && http_stats.retry_attempts == 1
          && http_stats.failures == 0);
    media_http_range_destroy(http);
    CHECK(http_budget.current == 0);

    puts("test: HTTP ranges reject successful short bodies without looping");
    HttpFixture short_fixture = {
        .bytes = http_bytes,
        .length = sizeof(http_bytes),
        .truncate_success_by = 1
    };
    http_options.transport_opaque = &short_fixture;
    http = media_http_range_create(
        &http_budget, NULL, "https://media.invalid/short.mp4",
        sizeof(http_bytes), &http_options, http_error, sizeof(http_error));
    CHECK(http != NULL);
    http_reader = media_http_range_reader(http);
    CHECK(!http_reader.read(
              http_reader.opaque, 0, http_output, sizeof(http_output))
          && short_fixture.requests == 2
          && media_http_range_stats(http, &http_stats)
          && http_stats.requests == 2
          && http_stats.retry_attempts == 1
          && http_stats.failures == 1
          && http_reader.describe_failure(
              http_reader.opaque, http_error, sizeof(http_error))
          && strstr(http_error, "invalid bounded response") != NULL);
    media_http_range_destroy(http);
    CHECK(http_budget.current == 0);

    puts("test: cancelled HTTP ranges stop without a retry attempt");
    /*
     * The cancellation token is what gets Circle out of a media open: the
     * range reader must consult it before it starts an attempt and again
     * before it spends a second one. A cancel that only takes effect at the
     * request deadline is what left the player up for tens of seconds.
     */
    TilefinchCancellation range_cancellation;
    tilefinch_cancellation_init(&range_cancellation);
    HttpFixture cancel_fixture = {
        .bytes = http_bytes,
        .length = sizeof(http_bytes)
    };
    MediaHttpRangeOptions cancel_options = {
        .cache_bytes = 4096,
        .timeout_ms = 15000,
        .connect_timeout_ms = 3000,
        .url_validator = http_fixture_url_admitted,
        .transport = http_fixture_range,
        .transport_opaque = &cancel_fixture,
        .cancel = http_fixture_cancelled,
        .cancel_opaque = &range_cancellation
    };
    http = media_http_range_create(
        &http_budget, NULL, "https://media.invalid/cancel.mp4",
        sizeof(http_bytes), &cancel_options, http_error, sizeof(http_error));
    CHECK(http != NULL);
    http_reader = media_http_range_reader(http);
    tilefinch_cancellation_request(&range_cancellation);
    CHECK(!http_reader.read(http_reader.opaque, 0, http_output, 100)
          && cancel_fixture.requests == 0
          && media_http_range_stats(http, &http_stats)
          && http_stats.requests == 0
          && http_stats.failures == 1
          && http_reader.describe_failure(
              http_reader.opaque, http_error, sizeof(http_error))
          && strstr(http_error, "cancelled") != NULL);
    media_http_range_destroy(http);
    CHECK(http_budget.current == 0);

    tilefinch_cancellation_init(&range_cancellation);
    cancel_fixture.requests = 0;
    cancel_fixture.cancel_on_request = &range_cancellation;
    http = media_http_range_create(
        &http_budget, NULL, "https://media.invalid/cancel-inflight.mp4",
        sizeof(http_bytes), &cancel_options, http_error, sizeof(http_error));
    CHECK(http != NULL);
    http_reader = media_http_range_reader(http);
    CHECK(!http_reader.read(http_reader.opaque, 0, http_output, 100)
          && cancel_fixture.requests == 1
          && media_http_range_stats(http, &http_stats)
          && http_stats.requests == 1
          && http_stats.retry_attempts == 0
          && http_stats.failures == 1);
    media_http_range_destroy(http);
    CHECK(http_budget.current == 0);

    Fixture fixture;
    size_t payload_offset = make_fixture(&fixture);
    FixtureReader fixture_reader = {
        .bytes = fixture.bytes, .length = fixture.length
    };
    MediaRangeReader reader = {
        .opaque = &fixture_reader,
        .length = fixture.length,
        .read = fixture_read,
        .describe_failure = fixture_describe_failure
    };
    Budget budget;
    budget_init(&budget, 1024u * 1024u);
    char error[256];

    puts("test: caller limits cannot exceed fixed parser scratch bounds");
    MediaMp4Limits hostile_limits = media_mp4_default_limits();
    hostile_limits.maximum_tracks = 5;
    CHECK(media_mp4_open(
              &budget, &reader, &hostile_limits,
              error, sizeof(error)) == NULL
          && strstr(error, "invalid limits") != NULL
          && budget.current == 0);

    puts("test: top-level discovery and sample descriptions fail closed");
    Fixture excessive_top_level = {0};
    for (size_t at = 0; at < 513u; at++) {
        append_u32(&excessive_top_level, 8u);
        append_u32(
            &excessive_top_level, MEDIA_MP4_FOURCC('f','r','e','e'));
    }
    FixtureReader excessive_reader = {
        .bytes = excessive_top_level.bytes,
        .length = excessive_top_level.length
    };
    MediaRangeReader excessive_source = {
        .opaque = &excessive_reader,
        .length = excessive_top_level.length,
        .read = fixture_read
    };
    CHECK(media_mp4_open(
              &budget, &excessive_source, NULL,
              error, sizeof(error)) == NULL
          && excessive_reader.reads == 512u
          && budget.current == 0);
    Fixture multiple_descriptions = fixture;
    CHECK(fixture_set_first_box_payload_u32(
              &multiple_descriptions,
              MEDIA_MP4_FOURCC('s','t','s','d'), 4u, 2u));
    FixtureReader description_reader = {
        .bytes = multiple_descriptions.bytes,
        .length = multiple_descriptions.length
    };
    MediaRangeReader description_source = {
        .opaque = &description_reader,
        .length = multiple_descriptions.length,
        .read = fixture_read
    };
    CHECK(media_mp4_open(
              &budget, &description_source, NULL,
              error, sizeof(error)) == NULL
          && budget.current == 0);
    Fixture wrong_chunk_description = fixture;
    CHECK(fixture_set_first_box_payload_u32(
              &wrong_chunk_description,
              MEDIA_MP4_FOURCC('s','t','s','c'), 16u, 2u));
    description_reader = (FixtureReader) {
        .bytes = wrong_chunk_description.bytes,
        .length = wrong_chunk_description.length
    };
    description_source = (MediaRangeReader) {
        .opaque = &description_reader,
        .length = wrong_chunk_description.length,
        .read = fixture_read
    };
    CHECK(media_mp4_open(
              &budget, &description_source, NULL,
              error, sizeof(error)) == NULL
          && budget.current == 0);

    MediaMp4Demux *demux = media_mp4_open(
        &budget, &reader, NULL, error, sizeof(error));
    if (demux == NULL) fprintf(stderr, "MP4 fixture: %s\n", error);
    CHECK(demux != NULL && media_mp4_track_count(demux) == 1);
    MediaMp4TrackInfo track;
    CHECK(media_mp4_track_info(demux, 0, &track));
    if (track.width != 320 || track.height != 240
        || track.codec_config_length != 7
        || media_mp4_retained_bytes(demux) >= 2048) {
        fprintf(stderr,
                "track width=%u height=%u config=%zu retained=%zu\n",
                track.width, track.height, track.codec_config_length,
                media_mp4_retained_bytes(demux));
    }
    CHECK(track.kind == MEDIA_MP4_TRACK_VIDEO
          && track.codec == MEDIA_MP4_FOURCC('a','v','c','1')
          && track.width == 320 && track.height == 240
          && track.sample_count == 2 && track.largest_sample == 4
          && track.timescale == 1000 && track.duration == 2000
          && track.codec_config_length == 7
          && track.nal_length_size == 4
          && media_mp4_retained_bytes(demux) < 2048);
    MediaMp4Sample first;
    MediaMp4Sample second;
    unsigned char payload[4] = {0};
    CHECK(media_mp4_next_sample(demux, &first)
          && first.offset == payload_offset && first.size == 3
          && first.dts == 0 && first.pts == 0
          && first.duration == 1000 && first.keyframe
          && media_mp4_read_sample(demux, &first, payload, sizeof(payload))
          && memcmp(payload, "\1\2\3", 3) == 0);
    CHECK(media_mp4_next_sample(demux, &second)
          && second.offset == payload_offset + 3 && second.size == 4
          && second.dts == 1000 && second.pts == 1000
          && second.duration == 1000 && !second.keyframe
          && !media_mp4_next_sample(demux, &first));
    uint64_t seek_time = UINT64_MAX;
    CHECK(media_mp4_seek_us(demux, 1500000, &seek_time)
          && seek_time == 0
          && media_mp4_next_sample(demux, &first)
          && first.offset == payload_offset && first.keyframe);
    CHECK(!media_mp4_seek_after_us(demux, 0, &seek_time));
    fixture_reader.fail_offset = payload_offset;
    fixture_reader.fail_enabled = true;
    char sample_error[256] = {0};
    CHECK(!media_mp4_read_sample(
              demux, &first, payload, sizeof(payload))
          && media_mp4_last_error(
              demux, sample_error, sizeof(sample_error))
          && strstr(sample_error, "sample range read failed") != NULL
          && strstr(sample_error, "fixture transport timeout") != NULL);
    fixture_reader.fail_enabled = false;
    CHECK(fixture_reader.bytes_read < fixture.length + 16);
    media_mp4_close(demux);

    puts("test: MP4 timescale admission bounds cross-track arithmetic");
    Fixture boundary_fixture = fixture;
    CHECK(fixture_set_first_mdhd_timescale(
              &boundary_fixture, MEDIA_MP4_MAX_TIMESCALE));
    FixtureReader boundary_reader = {
        .bytes = boundary_fixture.bytes,
        .length = boundary_fixture.length
    };
    MediaRangeReader boundary_source = {
        .opaque = &boundary_reader,
        .length = boundary_fixture.length,
        .read = fixture_read
    };
    demux = media_mp4_open(
        &budget, &boundary_source, NULL, error, sizeof(error));
    CHECK(demux != NULL
          && media_mp4_track_info(demux, 0, &track)
          && track.timescale == MEDIA_MP4_MAX_TIMESCALE);
    media_mp4_close(demux);

    Fixture hostile_timescale_fixture = fixture;
    CHECK(fixture_set_first_mdhd_timescale(
              &hostile_timescale_fixture,
              MEDIA_MP4_MAX_TIMESCALE + UINT32_C(1)));
    FixtureReader hostile_timescale_reader = {
        .bytes = hostile_timescale_fixture.bytes,
        .length = hostile_timescale_fixture.length
    };
    MediaRangeReader hostile_timescale_source = {
        .opaque = &hostile_timescale_reader,
        .length = hostile_timescale_fixture.length,
        .read = fixture_read
    };
    CHECK(media_mp4_open(
              &budget, &hostile_timescale_source, NULL,
              error, sizeof(error)) == NULL
          && budget.current == 0);

    puts("test: fragmented MP4 synthesizes bounded sample tables");
    Fixture fragmented_fixture;
    size_t fragment_moof_offset = 0;
    size_t fragment_moof_length = 0;
    size_t fragmented_payload =
        make_fragmented_fixture(
            &fragmented_fixture, 0,
            &fragment_moof_offset, &fragment_moof_length);
    Fixture wrong_fragment_description = fragmented_fixture;
    CHECK(fixture_set_first_box_payload_u32(
              &wrong_fragment_description,
              MEDIA_MP4_FOURCC('t','r','e','x'), 8u, 2u));
    FixtureReader wrong_fragment_reader = {
        .bytes = wrong_fragment_description.bytes,
        .length = wrong_fragment_description.length
    };
    MediaRangeReader wrong_fragment_source = {
        .opaque = &wrong_fragment_reader,
        .length = wrong_fragment_description.length,
        .read = fixture_read
    };
    CHECK(media_mp4_open(
              &budget, &wrong_fragment_source, NULL,
              error, sizeof(error)) == NULL
          && budget.current == 0);
    wrong_fragment_description = fragmented_fixture;
    CHECK(fixture_set_first_box_payload_u32(
              &wrong_fragment_description,
              MEDIA_MP4_FOURCC('t','f','h','d'), 8u, 2u));
    wrong_fragment_reader = (FixtureReader) {
        .bytes = wrong_fragment_description.bytes,
        .length = wrong_fragment_description.length
    };
    wrong_fragment_source = (MediaRangeReader) {
        .opaque = &wrong_fragment_reader,
        .length = wrong_fragment_description.length,
        .read = fixture_read
    };
    CHECK(media_mp4_open(
              &budget, &wrong_fragment_source, NULL,
              error, sizeof(error)) == NULL
          && budget.current == 0);
    FixtureReader fragmented_reader = {
        .bytes = fragmented_fixture.bytes,
        .length = fragmented_fixture.length,
        .one_shot_offset = fragment_moof_offset,
        .one_shot_length = fragment_moof_length
    };
    MediaRangeReader fragmented_source = {
        .opaque = &fragmented_reader,
        .length = fragmented_fixture.length,
        .read = fixture_read
    };
    demux = media_mp4_open(
        &budget, &fragmented_source, NULL, error, sizeof(error));
    if (demux == NULL)
        fprintf(stderr, "fragmented MP4 fixture: %s\n", error);
    CHECK(demux != NULL && media_mp4_track_count(demux) == 1);
    CHECK(media_mp4_track_info(demux, 0, &track)
          && track.kind == MEDIA_MP4_TRACK_VIDEO
          && track.sample_count == 2
          && track.largest_sample == 4
          && track.duration == 2000);
    CHECK(media_mp4_next_sample(demux, &first)
          && first.offset == fragmented_payload
          && first.size == 3 && first.dts == 0
          && first.duration == 1000 && first.keyframe);
    CHECK(media_mp4_next_sample(demux, &second)
          && second.offset == fragmented_payload + 3
          && second.size == 4 && second.dts == 1000
          && second.duration == 1000 && !second.keyframe
          && !media_mp4_next_sample(demux, &first));
    /* Fragment headers are retained across the plan/fill boundary. A second
       reader walk would make HTTP-backed media re-download every moof. */
    CHECK(fragmented_reader.bytes_read
          < fragmented_fixture.length * 2u
          && fragmented_reader.one_shot_reads == 1u);
    media_mp4_close(demux);

    puts("test: sidx fragmented MP4 loads one bounded window at a time");
    Fixture sidx_fixture;
    size_t second_moof_offset = 0;
    size_t second_moof_length = 0;
    size_t first_window_payload = 0;
    size_t second_window_payload = 0;
    CHECK(make_sidx_fragmented_fixture(
        &sidx_fixture, &second_moof_offset, &second_moof_length,
        &first_window_payload, &second_window_payload));
    FixtureReader sidx_reader = {
        .bytes = sidx_fixture.bytes,
        .length = sidx_fixture.length,
        .watch_offset = second_moof_offset,
        .watch_length = second_moof_length
    };
    MediaRangeReader sidx_source = {
        .opaque = &sidx_reader,
        .length = sidx_fixture.length,
        .read = fixture_read
    };
    demux = media_mp4_open(
        &budget, &sidx_source, NULL, error, sizeof(error));
    if (demux == NULL)
        fprintf(stderr, "sidx MP4 fixture: %s\n", error);
    CHECK(demux != NULL
          && media_mp4_track_info(demux, 0, &track)
          && track.duration == 4000
          && track.sample_count == 0
          && sidx_reader.watch_reads == 0);
    size_t first_window_retained = media_mp4_retained_bytes(demux);
    MediaMp4Sample third;
    MediaMp4Sample fourth;
    CHECK(media_mp4_next_sample(demux, &first)
          && first.offset == first_window_payload
          && first.dts == 0
          && media_mp4_next_sample(demux, &second)
          && second.offset == first_window_payload + 3
          && second.dts == 1000
          && sidx_reader.watch_reads == 0);
    CHECK(media_mp4_next_sample(demux, &third)
          && third.offset == second_window_payload
          && third.dts == 2000
          && sidx_reader.watch_reads == 1
          && media_mp4_retained_bytes(demux)
             <= first_window_retained);
    CHECK(media_mp4_next_sample(demux, &fourth)
          && fourth.offset == second_window_payload + 3
          && fourth.dts == 3000
          && !media_mp4_next_sample(demux, &first));
    char lazy_error[256] = {0};
    CHECK(!media_mp4_last_error(
        demux, lazy_error, sizeof(lazy_error)));
    uint64_t actual_seek_us = 0;
    CHECK(media_mp4_seek_us(
              demux, UINT64_C(2500000), &actual_seek_us)
          && actual_seek_us == UINT64_C(2000000)
          && media_mp4_next_sample(demux, &third)
          && third.offset == second_window_payload
          && third.dts == 2000);
    CHECK(media_mp4_seek_after_us(
              demux, UINT64_C(1500000), &actual_seek_us)
          && actual_seek_us == UINT64_C(2000000)
          && media_mp4_next_sample(demux, &third)
          && third.offset == second_window_payload
          && third.dts == 2000);
    CHECK(!media_mp4_seek_after_us(
              demux, UINT64_C(2000000), &actual_seek_us));
    media_mp4_rewind(demux);
    CHECK(media_mp4_next_sample(demux, &first)
          && first.offset == first_window_payload
          && first.dts == 0);
    media_mp4_close(demux);

    puts("test: a lazy segment scan resumes across cache eviction");
    FixtureReader crossing_sidx_reader = {
        .bytes = sidx_fixture.bytes,
        .length = sidx_fixture.length,
        /* Put the synthetic cache boundary exactly between the second moof
           and its mdat header. The scan needs both; the later full-moof read
           needs the first half again. Restarting either phase oscillates. */
        .poll_window_split = second_moof_offset + second_moof_length,
        .poll_window = 0,
        .poll_window_valid = true
    };
    MediaRangeReader crossing_sidx_source = {
        .opaque = &crossing_sidx_reader,
        .length = sidx_fixture.length,
        .read = fixture_read,
        .poll = fixture_poll,
        .resident = fixture_resident,
        .describe_failure = fixture_describe_failure
    };
    demux = media_mp4_open(
        &budget, &crossing_sidx_source, NULL, error, sizeof(error));
    CHECK(demux != NULL);
    size_t crossing_samples = 0;
    for (size_t step = 0; demux != NULL && step < 64u; step++) {
        if (media_mp4_next_sample(demux, &first)) {
            crossing_samples++;
            continue;
        }
        if (media_mp4_would_block(demux)) continue;
        break;
    }
    CHECK(crossing_samples == 4
          && crossing_sidx_reader.poll_window_switches <= 4u
          && !media_mp4_last_error(
              demux, lazy_error, sizeof(lazy_error)));
    media_mp4_close(demux);

    puts("test: misleading SAP metadata backtracks to an actual keyframe");
    Fixture non_sap_window_fixture = sidx_fixture;
    CHECK(fixture_set_trun_first_sample_flags(
        &non_sap_window_fixture, 1, UINT32_C(0x00010000)));
    FixtureReader non_sap_window_reader = {
        .bytes = non_sap_window_fixture.bytes,
        .length = non_sap_window_fixture.length
    };
    MediaRangeReader non_sap_window_source = {
        .opaque = &non_sap_window_reader,
        .length = non_sap_window_fixture.length,
        .read = fixture_read
    };
    demux = media_mp4_open(
        &budget, &non_sap_window_source, NULL, error, sizeof(error));
    CHECK(demux != NULL
          && media_mp4_next_sample(demux, &first)
          && first.keyframe
          && media_mp4_next_sample(demux, &second)
          && !second.keyframe
          && media_mp4_next_sample(demux, &third)
          && !third.keyframe
          && media_mp4_next_sample(demux, &fourth)
          && !fourth.keyframe);
    CHECK(media_mp4_seek_us(
              demux, UINT64_C(2500000), &actual_seek_us)
          && actual_seek_us == 0
          && media_mp4_next_sample(demux, &first)
          && first.offset == first_window_payload
          && first.keyframe);
    media_mp4_close(demux);

    puts("test: multi-moof sidx references fall back without skipping media");
    Fixture multi_moof_reference_fixture = sidx_fixture;
    CHECK(fixture_collapse_sidx_to_one_reference(
        &multi_moof_reference_fixture));
    FixtureReader multi_moof_reference_reader = {
        .bytes = multi_moof_reference_fixture.bytes,
        .length = multi_moof_reference_fixture.length
    };
    MediaRangeReader multi_moof_reference_source = {
        .opaque = &multi_moof_reference_reader,
        .length = multi_moof_reference_fixture.length,
        .read = fixture_read
    };
    demux = media_mp4_open(
        &budget, &multi_moof_reference_source, NULL,
        error, sizeof(error));
    CHECK(demux != NULL
          && media_mp4_track_info(demux, 0, &track)
          && track.sample_count == 4);
    size_t multi_moof_samples = 0;
    while (media_mp4_next_sample(demux, &first))
        multi_moof_samples++;
    CHECK(multi_moof_samples == 4
          && !media_mp4_last_error(
              demux, lazy_error, sizeof(lazy_error)));
    media_mp4_close(demux);

    puts("test: lazy-window allocation failures remain recoverable");
    for (size_t failure_at = 0; failure_at < 12u; failure_at++) {
        FixtureReader fault_reader = {
            .bytes = sidx_fixture.bytes,
            .length = sidx_fixture.length
        };
        MediaRangeReader fault_source = {
            .opaque = &fault_reader,
            .length = sidx_fixture.length,
            .read = fixture_read
        };
        Budget fault_budget;
        budget_init(&fault_budget, 1024u * 1024u);
        MediaMp4Demux *fault_demux = media_mp4_open(
            &fault_budget, &fault_source, NULL, error, sizeof(error));
        CHECK(fault_demux != NULL);
        budget_inject_failure_after(&fault_budget, failure_at);
        bool sought = media_mp4_seek_us(
            fault_demux, UINT64_C(2500000), NULL);
        budget_clear_failure_injection(&fault_budget);
        if (!sought) {
            media_mp4_rewind(fault_demux);
            CHECK(!media_mp4_last_error(
                      fault_demux, lazy_error, sizeof(lazy_error))
                  && media_mp4_next_sample(fault_demux, &first)
                  && first.offset == first_window_payload);
        }
        media_mp4_close(fault_demux);
        CHECK(fault_budget.current == 0
              && fault_budget.allocation_head == NULL
              && fault_budget.accounting_repair_count == 0);
    }

    puts("test: structured MP4 truncation and mutation sweep is leak-free");
    for (size_t truncated = 0;
         truncated <= sidx_fixture.length; truncated++) {
        FixtureReader truncated_reader = {
            .bytes = sidx_fixture.bytes,
            .length = truncated
        };
        MediaRangeReader truncated_source = {
            .opaque = &truncated_reader,
            .length = truncated,
            .read = fixture_read
        };
        Budget mutation_budget;
        budget_init(&mutation_budget, 4u * 1024u * 1024u);
        MediaMp4Demux *candidate = media_mp4_open(
            &mutation_budget, &truncated_source, NULL,
            error, sizeof(error));
        media_mp4_close(candidate);
        CHECK(mutation_budget.current == 0
              && mutation_budget.allocation_head == NULL
              && mutation_budget.accounting_repair_count == 0);
    }
    uint64_t mutation_state = UINT64_C(0x74696c6566696e63);
    for (size_t iteration = 0; iteration < 2048u; iteration++) {
        Fixture mutated = sidx_fixture;
        size_t changes =
            1u + (size_t) (media_test_random(&mutation_state) % 4u);
        for (size_t change = 0; change < changes; change++) {
            size_t at = (size_t) (
                media_test_random(&mutation_state) % mutated.length);
            mutated.bytes[at] ^= (unsigned char) (
                1u + media_test_random(&mutation_state) % 255u);
        }
        size_t exposed = mutated.length;
        if ((iteration & 7u) == 0) {
            exposed = (size_t) (
                media_test_random(&mutation_state)
                % (mutated.length + 1u));
        }
        FixtureReader mutation_reader = {
            .bytes = mutated.bytes,
            .length = exposed
        };
        MediaRangeReader mutation_source = {
            .opaque = &mutation_reader,
            .length = exposed,
            .read = fixture_read
        };
        Budget mutation_budget;
        budget_init(&mutation_budget, 4u * 1024u * 1024u);
        MediaMp4Demux *candidate = media_mp4_open(
            &mutation_budget, &mutation_source, NULL,
            error, sizeof(error));
        if (candidate != NULL) {
            for (size_t sample = 0; sample < 64u; sample++) {
                if (!media_mp4_next_sample(candidate, &first)) break;
            }
            (void) media_mp4_seek_us(
                candidate,
                media_test_random(&mutation_state)
                    % UINT64_C(10000000),
                NULL);
            media_mp4_rewind(candidate);
        }
        media_mp4_close(candidate);
        CHECK(mutation_budget.current == 0
              && mutation_budget.allocation_head == NULL
              && mutation_budget.accounting_repair_count == 0);
    }

    puts("test: 21-minute sidx schedule remains window-bounded");
    Fixture long_sidx_fixture;
    size_t last_moof_offset = 0;
    size_t last_moof_length = 0;
    CHECK(make_long_sidx_fragmented_fixture(
        &long_sidx_fixture, &last_moof_offset, &last_moof_length));
    FixtureReader long_sidx_reader = {
        .bytes = long_sidx_fixture.bytes,
        .length = long_sidx_fixture.length,
        .watch_offset = last_moof_offset,
        .watch_length = last_moof_length
    };
    MediaRangeReader long_sidx_source = {
        .opaque = &long_sidx_reader,
        .length = long_sidx_fixture.length,
        .read = fixture_read
    };
    demux = media_mp4_open(
        &budget, &long_sidx_source, NULL, error, sizeof(error));
    CHECK(demux != NULL
          && media_mp4_track_info(demux, 0, &track)
          && track.duration == UINT64_C(1260000)
          && track.sample_count == 0
          && long_sidx_reader.watch_reads == 0);
    size_t long_window_retained = media_mp4_retained_bytes(demux);
    size_t long_samples = 0;
    uint64_t last_dts = 0;
    while (media_mp4_next_sample(demux, &first)) {
        long_samples++;
        last_dts = first.dts;
        CHECK(media_mp4_retained_bytes(demux)
              <= long_window_retained);
    }
    CHECK(long_samples == 42
          && last_dts == UINT64_C(1230000)
          && long_sidx_reader.watch_reads == 1
          && !media_mp4_last_error(
              demux, lazy_error, sizeof(lazy_error)));
    CHECK(media_mp4_seek_us(
              demux, UINT64_C(1200000000), &actual_seek_us)
          && actual_seek_us == UINT64_C(1200000000)
          && media_mp4_next_sample(demux, &first)
          && first.dts == UINT64_C(1200000));
    media_mp4_close(demux);

    puts("test: lazy fragment transport failure is not reported as EOF");
    FixtureReader failed_lazy_reader = {
        .bytes = sidx_fixture.bytes,
        .length = sidx_fixture.length,
        .fail_offset = second_moof_offset,
        .fail_enabled = true
    };
    sidx_source = (MediaRangeReader) {
        .opaque = &failed_lazy_reader,
        .length = sidx_fixture.length,
        .read = fixture_read,
        .describe_failure = fixture_describe_failure
    };
    demux = media_mp4_open(
        &budget, &sidx_source, NULL, error, sizeof(error));
    CHECK(demux != NULL
          && media_mp4_next_sample(demux, &first)
          && media_mp4_next_sample(demux, &second)
          && !media_mp4_next_sample(demux, &third)
          && media_mp4_last_error(
              demux, lazy_error, sizeof(lazy_error))
          && strstr(lazy_error, "range read failed") != NULL
          && strstr(lazy_error, "fixture transport timeout") != NULL);
    media_mp4_close(demux);

    FixtureReader failed_fragment_reader = {
        .bytes = fragmented_fixture.bytes,
        .length = fragmented_fixture.length,
        .fail_offset = fragment_moof_offset,
        .fail_enabled = true
    };
    fragmented_source = (MediaRangeReader) {
        .opaque = &failed_fragment_reader,
        .length = fragmented_fixture.length,
        .read = fixture_read,
        .describe_failure = fixture_describe_failure
    };
    CHECK(media_mp4_open(
              &budget, &fragmented_source, NULL,
              error, sizeof(error)) == NULL
          && strstr(error, "range read failed") != NULL
          && strstr(error, "fixture transport timeout") != NULL);

    puts("test: fragmented MP4 normalizes a non-zero tfdt origin");
    Fixture offset_fragmented_fixture;
    size_t offset_fragmented_payload =
        make_fragmented_fixture(
            &offset_fragmented_fixture, 5000, NULL, NULL);
    FixtureReader offset_fragmented_reader = {
        .bytes = offset_fragmented_fixture.bytes,
        .length = offset_fragmented_fixture.length
    };
    MediaRangeReader offset_fragmented_source = {
        .opaque = &offset_fragmented_reader,
        .length = offset_fragmented_fixture.length,
        .read = fixture_read
    };
    demux = media_mp4_open(
        &budget, &offset_fragmented_source, NULL, error, sizeof(error));
    CHECK(demux != NULL
          && media_mp4_track_info(demux, 0, &track)
          && track.duration == 2000
          && media_mp4_next_sample(demux, &first)
          && first.offset == offset_fragmented_payload
          && first.dts == 0);
    media_mp4_close(demux);

    puts("test: bounded media playback yields between packets");
    fixture_reader.reads = 0;
    fixture_reader.bytes_read = 0;
    demux = media_mp4_open(
        &budget, &reader, NULL, error, sizeof(error));
    CHECK(demux != NULL);
    BackendFixture backend_fixture = {.queue_once = true};
    MediaBackend backend = {
        .opaque = &backend_fixture,
        .presentation = &backend_presentation_ops,
        .submit = backend_submit,
        .drain = backend_drain,
        .advance = backend_advance,
        .take_video_frame = backend_take_video_frame,
        .discard_video_before = backend_discard_video_before,
        .reset = backend_reset,
        .destroy = backend_destroy
    };
    MediaPlaybackOptions playback_options = {
        .decode_lead_us = UINT64_C(2000000),
        .maximum_packet_bytes = 16,
        .preallocate_maximum_packet_bytes = true
    };
    MediaPlayback *playback = media_playback_create(
        &budget, demux, &backend, &playback_options,
        error, sizeof(error));
    CHECK(playback != NULL
          && !media_playback_has_audio(playback)
          && !media_playback_set_audio_submission_blocked(playback, true)
          && media_playback_discard_video_before(playback, 0) == 0
          && media_playback_packet_bytes(playback) == 16u
          && media_playback_advance_bounded(
                 playback, 0, 1, error, sizeof(error))
               == MEDIA_PLAYBACK_ADVANCE_PENDING
          && backend_fixture.submitted == 1
          && backend_fixture.advanced == 1);
    MediaVideoFrame presentation_frame = {
        .slot = 7,
        .generation = 9
    };
    CHECK(media_playback_borrow_video_slot(playback, 7u, 9u));
    media_playback_note_frame_staged(playback, &presentation_frame);
    media_playback_note_frame_displayed(
        playback, &presentation_frame, 3);
    media_playback_release_video_read(playback, 7u);
    CHECK(backend_fixture.presentation_borrows == 1
          && backend_fixture.presentation_releases == 1
          && backend_fixture.presentation_staged == 1
          && backend_fixture.presentation_displayed == 1);
    backend_fixture.discarded_before = 2;
    CHECK(media_playback_discard_video_before(
              playback, UINT64_C(1500000)) == 2
          && backend_fixture.discard_floor_us == UINT64_C(1500000));
    MediaPlaybackJobStats job_stats = {0};
    media_playback_job_stats(playback, &job_stats);
    CHECK(job_stats.calls == 1 && job_stats.yielded_calls == 1
          && job_stats.packets_submitted == 1);
    CHECK(media_playback_advance_bounded(
              playback, UINT64_C(2000000), 1, error, sizeof(error))
              == MEDIA_PLAYBACK_ADVANCE_PENDING
          && backend_fixture.submitted == 2);
    backend_fixture.drain_emit_once = true;
    CHECK(media_playback_advance_bounded(
              playback, UINT64_C(2000000), 1, error, sizeof(error))
              == MEDIA_PLAYBACK_ADVANCE_PENDING
          && !media_playback_ended(playback)
          && backend_fixture.drains == 1
          && backend_fixture.drain_tail_pending);
    CHECK(media_playback_advance_bounded(
              playback, UINT64_C(2000000), 1, error, sizeof(error))
              == MEDIA_PLAYBACK_ADVANCE_PENDING
          && !media_playback_ended(playback)
          && backend_fixture.drains == 2);
    MediaVideoFrame drained_frame = {0};
    CHECK(media_playback_take_video_frame(playback, &drained_frame)
          && drained_frame.pixels != NULL
          && !backend_fixture.drain_tail_pending);
    CHECK(media_playback_advance_bounded(
              playback, UINT64_C(2000000), 1, error, sizeof(error))
              == MEDIA_PLAYBACK_ADVANCE_COMPLETE
          && media_playback_ended(playback)
          && backend_fixture.drains == 3);
    media_playback_job_stats(playback, &job_stats);
    CHECK(job_stats.calls == 5 && job_stats.yielded_calls == 4
          && job_stats.packets_submitted == 2
          && job_stats.would_block_calls == 1
          && job_stats.drain_calls == 3);
    media_playback_destroy(playback);
    CHECK(backend_fixture.destroyed == 1);
    media_mp4_close(demux);

    /*
     * A media unit must never wait on the network. When the range source has
     * not buffered a payload it answers would-block, and that has to arrive at
     * the pump as a pending pipeline: not end-of-stream (which stops playback)
     * and not an error (which raises a failed player). Device cycle truth2
     * measured single units blocked for 1.64 s because this path did not exist.
     */
    puts("test: an unbuffered payload yields instead of ending the stream");
    FixtureReader blocking_reader = {
        .bytes = fixture.bytes, .length = fixture.length
    };
    MediaRangeReader blocking_range = {
        .opaque = &blocking_reader,
        .length = fixture.length,
        .read = fixture_read,
        .poll = fixture_poll,
        .resident = fixture_resident,
        .describe_failure = fixture_describe_failure
    };
    MediaMp4Demux *blocking_demux = media_mp4_open(
        &budget, &blocking_range, NULL, error, sizeof(error));
    CHECK(blocking_demux != NULL);
    BackendFixture blocking_backend_fixture = {0};
    MediaBackend blocking_backend = {
        .opaque = &blocking_backend_fixture,
        .submit = backend_submit,
        .drain = backend_drain,
        .advance = backend_advance,
        .take_video_frame = backend_take_video_frame,
        .reset = backend_reset,
        .destroy = backend_destroy
    };
    MediaPlayback *blocking_playback = media_playback_create(
        &budget, blocking_demux, &blocking_backend, &playback_options,
        error, sizeof(error));
    CHECK(blocking_playback != NULL);
    blocking_reader.block_polls = 2;
    error[0] = '\0';
    CHECK(media_playback_advance_bounded(
              blocking_playback, 0, 4, error, sizeof(error))
              == MEDIA_PLAYBACK_ADVANCE_PENDING
          && error[0] == '\0'
          && !media_playback_ended(blocking_playback)
          && blocking_backend_fixture.submitted == 0);
    MediaPlaybackJobStats blocked_stats = {0};
    media_playback_job_stats(blocking_playback, &blocked_stats);
    CHECK(blocked_stats.source_block_calls == 1
          && blocked_stats.would_block_calls == 1
          && blocked_stats.source_ended_breaks == 0
          && blocking_backend_fixture.drains == 0
          && !media_mp4_last_error(blocking_demux, error, sizeof(error)));
    /* The same sample is still selected, so the retry reads exactly it. */
    CHECK(media_playback_advance_bounded(
              blocking_playback, 0, 4, error, sizeof(error))
              == MEDIA_PLAYBACK_ADVANCE_PENDING
          && blocking_backend_fixture.submitted == 0);
    media_playback_job_stats(blocking_playback, &blocked_stats);
    CHECK(blocked_stats.source_block_calls == 2
          && blocking_reader.block_polls == 0);
    CHECK(media_playback_advance_bounded(
              blocking_playback, 0, 4, error, sizeof(error))
              != MEDIA_PLAYBACK_ADVANCE_ERROR
          && blocking_backend_fixture.submitted != 0);
    media_playback_destroy(blocking_playback);
    media_mp4_close(blocking_demux);

    puts("test: split MP4 sources retain one-packet orchestration");
    FixtureReader split_video_reader = {
        .bytes = fixture.bytes, .length = fixture.length
    };
    FixtureReader split_audio_reader = {
        .bytes = fixture.bytes, .length = fixture.length
    };
    MediaRangeReader split_video_source = {
        .opaque = &split_video_reader,
        .length = fixture.length,
        .read = fixture_read,
        .poll = fixture_poll,
        .resident = fixture_resident
    };
    MediaRangeReader split_audio_source = {
        .opaque = &split_audio_reader,
        .length = fixture.length,
        .read = fixture_read,
        .poll = fixture_poll,
        .resident = fixture_resident
    };
    MediaMp4Demux *split_video = media_mp4_open(
        &budget, &split_video_source, NULL, error, sizeof(error));
    MediaMp4Demux *split_audio = media_mp4_open(
        &budget, &split_audio_source, NULL, error, sizeof(error));
    CHECK(split_video != NULL && split_audio != NULL);
    backend_fixture = (BackendFixture) {0};
    playback = media_playback_create_split(
        &budget, split_video, split_audio, &backend, &playback_options,
        error, sizeof(error));
    CHECK(playback != NULL && media_playback_has_audio(playback)
          && media_playback_set_audio_submission_blocked(playback, true));
    /* The independent audio head stays pending while source zero advances.
       Once video is caught up, another visit remains pending rather than
       falsely draining the backend. */
    for (size_t packet = 0; packet < 2; packet++) {
        CHECK(media_playback_advance_bounded(
                  playback, UINT64_C(2000000), 1, error, sizeof(error))
                  == MEDIA_PLAYBACK_ADVANCE_PENDING);
    }
    CHECK(media_playback_advance_bounded(
              playback, UINT64_C(2000000), 1, error, sizeof(error))
              == MEDIA_PLAYBACK_ADVANCE_PENDING
          && backend_fixture.submitted == 2
          && !media_playback_ended(playback)
          && media_playback_set_audio_submission_blocked(playback, false));
    for (size_t packet = 0; packet < 2; packet++) {
        CHECK(media_playback_advance_bounded(
                  playback, UINT64_C(2000000), 1, error, sizeof(error))
                  == MEDIA_PLAYBACK_ADVANCE_PENDING);
    }
    CHECK(backend_fixture.submitted == 4
          && media_playback_advance_bounded(
                 playback, UINT64_C(2000000), 1, error, sizeof(error))
                 == MEDIA_PLAYBACK_ADVANCE_COMPLETE
          && media_playback_ended(playback));
    media_playback_destroy(playback);
    CHECK(backend_fixture.destroyed == 1);

    /*
     * A blocked split-track head may not strand the independent other track
     * when its sample is already resident and inside the same clock horizon.
     * The original head remains pending and is reconsidered by the next
     * bounded call; this call advances only the other source.
     */
    puts("test: a blocked head yields to a resident split-track alternate");
    FixtureReader head_video_reader = {
        .bytes = fixture.bytes, .length = fixture.length, .block_polls = 1
    };
    FixtureReader head_audio_reader = {
        .bytes = fixture.bytes, .length = fixture.length
    };
    MediaRangeReader head_video_source = {
        .opaque = &head_video_reader,
        .length = fixture.length,
        .read = fixture_read,
        .poll = fixture_poll,
        .resident = fixture_resident,
        .describe_failure = fixture_describe_failure
    };
    MediaRangeReader head_audio_source = {
        .opaque = &head_audio_reader,
        .length = fixture.length,
        .read = fixture_read
    };
    MediaMp4Demux *head_video = media_mp4_open(
        &budget, &head_video_source, NULL, error, sizeof(error));
    MediaMp4Demux *head_audio = media_mp4_open(
        &budget, &head_audio_source, NULL, error, sizeof(error));
    CHECK(head_video != NULL && head_audio != NULL);
    backend_fixture = (BackendFixture) {0};
    playback = media_playback_create_split(
        &budget, head_video, head_audio, &backend, &playback_options,
        error, sizeof(error));
    CHECK(playback != NULL);
    /* Source zero blocks on its payload; source one is holding a sample of
       the same instant whose bytes are already in memory. */
    CHECK(media_playback_advance_bounded(
              playback, 0, 4, error, sizeof(error))
              == MEDIA_PLAYBACK_ADVANCE_PENDING
          && backend_fixture.submitted == 2);
    MediaPlaybackJobStats head = {0};
    media_playback_job_stats(playback, &head);
    CHECK(head.source_block_calls == 1
          && head.head_blocks == 1
          && head.head_block_video + head.head_block_audio == 1
          && head.head_alt_pending == 1
          && head.head_alt_in_horizon == 1
          /* The whole point: a sample that existed, was due, and whose bytes
             were already there, that the visit went home without offering. */
          && head.head_alt_resident == 1
          && head.head_alt_bypasses == 1
          && head.head_alt_submitted == 2
          && head.head_alt_blocked == 0
          /* Both sources read one fixture, so the alternate sits level with
             the head rather than behind it. */
          && head.head_alt_lead_total_us == 0
          && head.head_alt_lead_samples == 1
          && head.head_alt_behind == 0);
    media_playback_destroy(playback);
    media_mp4_close(head_video);
    media_mp4_close(head_audio);

    /*
     * And the residency arm discriminates. Same block, but the alternate's
     * bytes are not buffered either -- so it was pending and due and still
     * could not have been submitted, which is the case that would make the
     * counter above a lie if it were not asked separately.
     */
    puts("test: an unbuffered alternate is counted as due but not resident");
    head_video_reader = (FixtureReader) {
        .bytes = fixture.bytes, .length = fixture.length, .block_polls = 1
    };
    head_audio_reader = (FixtureReader) {
        .bytes = fixture.bytes, .length = fixture.length, .block_polls = 8
    };
    head_audio_source.poll = fixture_poll;
    head_audio_source.resident = fixture_resident;
    head_video = media_mp4_open(
        &budget, &head_video_source, NULL, error, sizeof(error));
    head_audio = media_mp4_open(
        &budget, &head_audio_source, NULL, error, sizeof(error));
    CHECK(head_video != NULL && head_audio != NULL);
    backend_fixture = (BackendFixture) {0};
    playback = media_playback_create_split(
        &budget, head_video, head_audio, &backend, &playback_options,
        error, sizeof(error));
    CHECK(playback != NULL);
    CHECK(media_playback_advance_bounded(
              playback, 0, 4, error, sizeof(error))
              == MEDIA_PLAYBACK_ADVANCE_PENDING);
    media_playback_job_stats(playback, &head);
    CHECK(head.head_blocks == 1
          && head.head_alt_pending == 1
          && head.head_alt_in_horizon == 1
          && head.head_alt_resident == 0
          && head.head_alt_bypasses == 0
          && head.head_alt_submitted == 0);
    /* Asking never fetched: the alternate's polls are untouched, so the
       measurement did not move the schedule it was measuring. */
    CHECK(head_audio_reader.block_polls == 8
          && head_audio_reader.polls == 0);
    media_playback_destroy(playback);
    media_mp4_close(head_video);
    media_mp4_close(head_audio);

    puts("test: split seek decodes video preroll without queuing old audio");
    MediaVideoFrame before_seek = {
        .pts_us = UINT64_C(1450000),
        .duration_us = UINT64_C(40000)
    };
    MediaVideoFrame nearest_past = {
        .pts_us = UINT64_C(1485000),
        .duration_us = UINT64_C(40000)
    };
    MediaVideoFrame nearest_future = {
        .pts_us = UINT64_C(1515000),
        .duration_us = UINT64_C(40000)
    };
    CHECK(!media_video_frame_matches_seek(
              &before_seek, UINT64_C(1500000))
          && media_video_frame_matches_seek(
              &nearest_past, UINT64_C(1500000))
          && media_video_frame_matches_seek(
              &nearest_future, UINT64_C(1500000)));
    CHECK(!media_video_frame_is_due(
              &nearest_future, UINT64_C(1490000))
          && media_video_frame_is_due(
              &nearest_future, UINT64_C(1495000)));
    CHECK(psp_media_seek_take_clock_us(
              UINT64_C(1490000), UINT64_C(1490000), true,
              UINT64_C(1515000))
          == UINT64_C(1515000));
    CHECK(psp_media_seek_take_clock_us(
              UINT64_C(1490000), 0, true, UINT64_C(1515000))
          == UINT64_C(1490000));
    CHECK(psp_media_seek_take_clock_us(
              UINT64_C(1515000), UINT64_C(1490000), true,
              UINT64_C(1490000))
          == UINT64_C(1515000));
    CHECK(psp_media_seek_take_clock_us(
              UINT64_C(1490000), UINT64_C(1490000), false,
              UINT64_C(1515000))
          == UINT64_C(1490000));
    CHECK(media_video_seek_decide(
              &before_seek, UINT64_C(1500000), true)
          == MEDIA_VIDEO_SEEK_WAIT);
    before_seek.pixels = (unsigned char *) (uintptr_t) 1;
    CHECK(media_video_seek_decide(
              &before_seek, UINT64_C(1500000), false)
          == MEDIA_VIDEO_SEEK_FINAL_FALLBACK);
    MediaVideoFrame missing_seek_frame = {0};
    CHECK(media_video_seek_decide(
              &missing_seek_frame, UINT64_C(1500000), false)
          == MEDIA_VIDEO_SEEK_UNAVAILABLE);
    CHECK(media_video_seek_decide(
              &nearest_past, UINT64_C(1500000), true)
          == MEDIA_VIDEO_SEEK_MATCH);
    PspMediaVideoTimestampQueue timestamp_queue = {0};
    PspMediaSurfacePolicy surface_policy = {0};
    PspMediaDecoderPolicy decoder_policy = {0};
    CHECK(psp_utility_net_module_load_disposition(0)
              == PSP_UTILITY_MODULE_LOAD_ACQUIRED
          && psp_utility_net_module_load_disposition(
                 (int) PSP_MODULE_NET_ALREADY_LOADED)
              == PSP_UTILITY_MODULE_LOAD_RESIDENT
          && psp_utility_net_module_load_disposition(
                 (int) UINT32_C(0x80111103))
              == PSP_UTILITY_MODULE_LOAD_FAILED
          && psp_utility_net_module_load_disposition(
                 (int) PSP_MODULE_AV_ALREADY_LOADED)
              == PSP_UTILITY_MODULE_LOAD_FAILED
          && psp_utility_av_module_load_disposition(0)
              == PSP_UTILITY_MODULE_LOAD_ACQUIRED
          && psp_utility_av_module_load_disposition(
                 (int) PSP_MODULE_AV_ALREADY_LOADED)
              == PSP_UTILITY_MODULE_LOAD_RESIDENT
          && psp_utility_av_module_load_disposition(
                 (int) PSP_MODULE_NET_ALREADY_LOADED)
              == PSP_UTILITY_MODULE_LOAD_RESIDENT
          && psp_utility_av_module_load_disposition(
                 (int) UINT32_C(0x80110f03))
              == PSP_UTILITY_MODULE_LOAD_FAILED
          && psp_utility_module_load_owned(
                 PSP_UTILITY_MODULE_LOAD_ACQUIRED)
          && !psp_utility_module_load_owned(
                 PSP_UTILITY_MODULE_LOAD_RESIDENT)
          && psp_kernel_module_already_resident(
              (int) PSP_MODULE_KERNEL_EXCLUSIVE_LOAD)
          && !psp_kernel_module_already_resident(-1)
          && psp_kernel_module_start_succeeded(7, 0)
          && psp_kernel_module_start_succeeded(
              (int) PSP_MODULE_KERNEL_ALREADY_STARTED, -1)
          && !psp_kernel_module_start_succeeded(7, -1)
          && !psp_kernel_module_start_succeeded(-1, 0)
          && psp_media_module_failure_retryable(
              (int) PSP_MEDIA_ERROR_OUT_OF_MEMORY)
          && psp_media_module_failure_retryable(
              (int) PSP_MEDIA_ERROR_MODULE_MGR_BUSY)
          && psp_media_module_failure_retryable(
              (int) PSP_MEDIA_ERROR_KERNEL_NOMEM)
          && psp_media_module_failure_retryable(
              (int) PSP_MODULE_ERROR_THREAD_TERMINATED)
          && !psp_media_module_failure_retryable(
              (int) UINT32_C(0x80020146))
          && psp_module_worker_poll_disposition(0, UINT32_C(0x1))
              == PSP_MODULE_WORKER_POLL_PENDING
          && psp_module_worker_poll_disposition(0, UINT32_C(0x4))
              == PSP_MODULE_WORKER_POLL_PENDING
          && psp_module_worker_poll_disposition(
                 0, PSP_MODULE_THREAD_STATUS_STOPPED)
              == PSP_MODULE_WORKER_POLL_TERMINAL
          && psp_module_worker_poll_disposition(
                 0, PSP_MODULE_THREAD_STATUS_KILLED)
              == PSP_MODULE_WORKER_POLL_TERMINAL
          && psp_module_worker_poll_disposition(-1, 0)
              == PSP_MODULE_WORKER_POLL_ERROR
          && !psp_module_worker_was_killed(
              PSP_MODULE_THREAD_STATUS_STOPPED)
          && psp_module_worker_was_killed(
              PSP_MODULE_THREAD_STATUS_KILLED)
          && psp_unexpected_worker_exit_status(
                 PSP_MODULE_THREAD_STATUS_STOPPED, -7) == -7
          && (uint32_t) psp_unexpected_worker_exit_status(
                 PSP_MODULE_THREAD_STATUS_STOPPED, 0)
              == PSP_MODULE_ERROR_THREAD_TERMINATED
          && (uint32_t) psp_unexpected_worker_exit_status(
                 PSP_MODULE_THREAD_STATUS_KILLED, 0)
              == PSP_MODULE_ERROR_THREAD_TERMINATED
          && psp_kernel_callable_address(UINT32_C(0x88000000))
          && psp_kernel_callable_address(UINT32_C(0x8ffffffc))
          && !psp_kernel_callable_address(UINT32_C(0x88000002))
          && !psp_kernel_callable_address(UINT32_C(0x800200d2))
          && !psp_kernel_callable_address(UINT32_C(0xffffffff))
          && !psp_kernel_callable_address(0));
    CHECK(psp_media_decoder_policy(
              PSP_MEDIA_AVC_PROFILE_MAIN, 30, 640, 360, true, false,
              &decoder_policy)
          && decoder_policy.mpeg_mode == 5
          && decoder_policy.me_boot_type == 1
          && psp_media_decoder_policy(
              PSP_MEDIA_AVC_PROFILE_MAIN, 21, 426, 240, false, false,
              &decoder_policy)
          && decoder_policy.mpeg_mode == 4
          && decoder_policy.me_boot_type == 3
          && !psp_media_decoder_policy(
              PSP_MEDIA_AVC_PROFILE_BASELINE, 30, 640, 360, true, false,
              &decoder_policy)
          && !psp_media_decoder_policy(
              0x64, 30, 640, 360, true, false, &decoder_policy)
          && !psp_media_decoder_policy(
              PSP_MEDIA_AVC_PROFILE_MAIN, 31, 640, 360, true, false,
              &decoder_policy));
    /*
     * Baseline that fits the screen now defaults to the program which makes
     * no Media Engine boot call at all (create mode 4 / boot type 3). Three
     * device runs rejected every program that did make the call, including
     * this stream's historical type 4; type 3 is the one never attempted.
     * The `boot4` knob restores type 4 for the A/B, and changes nothing else
     * -- small Main profile keeps type 3 either way, and a wide picture stays
     * inadmissible because `boot4` does not enable the wide program.
     */
    CHECK(psp_media_decoder_policy(
              PSP_MEDIA_AVC_PROFILE_BASELINE, 21, 480, 272, false, false,
              &decoder_policy)
          && decoder_policy.mpeg_mode == 4
          && decoder_policy.me_boot_type == 3
          && psp_media_decoder_policy(
              PSP_MEDIA_AVC_PROFILE_BASELINE, 21, 426, 240, false, false,
              &decoder_policy)
          && decoder_policy.mpeg_mode == 4
          && decoder_policy.me_boot_type
                 == PSP_MEDIA_DEFAULT_ME_BOOT_TYPE
          && psp_media_decoder_policy(
              PSP_MEDIA_AVC_PROFILE_BASELINE, 21, 426, 240, false, true,
              &decoder_policy)
          && decoder_policy.mpeg_mode == 4
          && decoder_policy.me_boot_type == 4
          && psp_media_decoder_policy(
              PSP_MEDIA_AVC_PROFILE_MAIN, 21, 426, 240, false, true,
              &decoder_policy)
          && decoder_policy.mpeg_mode == 4
          && decoder_policy.me_boot_type == 3
          && !psp_media_decoder_policy(
              PSP_MEDIA_AVC_PROFILE_BASELINE, 21, 640, 360, false, true,
              &decoder_policy)
          && !psp_media_decoder_policy(
              PSP_MEDIA_AVC_PROFILE_MAIN, 30, 640, 360, false, true,
              &decoder_policy));
    /* The knob spellings, and which decision each one moves. Only `boot4`
       restores the Baseline boot call, and only the two wide spellings admit
       the wide program; neither leaks into the other. */
    CHECK(psp_media_wide_program_from_name("boot4")
              == PSP_MEDIA_WIDE_PROGRAM_BASELINE_BOOT4
          && psp_media_wide_program_name_valid("boot4")
          && !psp_media_wide_program_name_valid("boot3")
          && !psp_media_wide_program_name_valid("Boot4")
          && strcmp(psp_media_wide_program_name(
                        PSP_MEDIA_WIDE_PROGRAM_BASELINE_BOOT4),
                    "boot4") == 0
          && !psp_media_wide_program_enabled(
                 PSP_MEDIA_WIDE_PROGRAM_BASELINE_BOOT4)
          && !psp_media_wide_program_annexb(
                 PSP_MEDIA_WIDE_PROGRAM_BASELINE_BOOT4)
          && psp_media_baseline_boot_call_enabled(
                 PSP_MEDIA_WIDE_PROGRAM_BASELINE_BOOT4)
          && !psp_media_baseline_boot_call_enabled(
                 PSP_MEDIA_WIDE_PROGRAM_OFF)
          && !psp_media_baseline_boot_call_enabled(
                 PSP_MEDIA_WIDE_PROGRAM_WIDE)
          && !psp_media_baseline_boot_call_enabled(
                 PSP_MEDIA_WIDE_PROGRAM_WIDE_ANNEXB)
          /* A Baseline attempt on either program is not evidence about the
             wide one, so neither may trip the wide latch. */
          && !psp_media_wide_program_rejected_by(4, 4, true, false)
          && !psp_media_wide_program_rejected_by(4, 3, true, false)
          /* `boot4` leaves the wide clamp exactly where the default has it. */
          && psp_media_admitted_quality(
                 360u, false,
                 psp_media_wide_program_enabled(
                     PSP_MEDIA_WIDE_PROGRAM_BASELINE_BOOT4)) == 240u);
    /*
     * The two PMPlayer-faithful defaults and the spellings that restore what
     * they replaced. `edram-real` puts the firmware EDRAM grant back;
     * `no-boot` puts back the assumption that the Media Engine is already
     * running program type 3 at process start. Each moves exactly one
     * decision, and neither is a wide or Baseline program selection.
     */
    CHECK(psp_media_wide_program_from_name("edram-real")
              == PSP_MEDIA_WIDE_PROGRAM_EDRAM_REAL
          && psp_media_wide_program_from_name("no-boot")
              == PSP_MEDIA_WIDE_PROGRAM_NO_BOOT
          && strcmp(psp_media_wide_program_name(
                        PSP_MEDIA_WIDE_PROGRAM_EDRAM_REAL),
                    "edram-real") == 0
          && strcmp(psp_media_wide_program_name(
                        PSP_MEDIA_WIDE_PROGRAM_NO_BOOT),
                    "no-boot") == 0
          && psp_media_wide_program_name_valid("edram-real")
          && psp_media_wide_program_name_valid("no-boot")
          /* Typos near the new spellings must halt, not read as off. */
          && !psp_media_wide_program_name_valid("edram")
          && !psp_media_wide_program_name_valid("edram-fake")
          && !psp_media_wide_program_name_valid("noboot")
          && !psp_media_wide_program_name_valid("no-boot4")
          && psp_media_wide_program_from_name("edram")
                 == PSP_MEDIA_WIDE_PROGRAM_OFF
          /* The pool-backed AAC work buffer is the default: only the one
             spelling asks firmware for the real grant. */
          && psp_media_real_edram_enabled(
                 PSP_MEDIA_WIDE_PROGRAM_EDRAM_REAL)
          && !psp_media_real_edram_enabled(PSP_MEDIA_WIDE_PROGRAM_OFF)
          && !psp_media_real_edram_enabled(PSP_MEDIA_WIDE_PROGRAM_WIDE)
          && !psp_media_real_edram_enabled(
                 PSP_MEDIA_WIDE_PROGRAM_WIDE_ANNEXB)
          && !psp_media_real_edram_enabled(
                 PSP_MEDIA_WIDE_PROGRAM_BASELINE_BOOT4)
          && !psp_media_real_edram_enabled(PSP_MEDIA_WIDE_PROGRAM_NO_BOOT)
          /* And the first open of a process really boots the engine unless
             exactly that one spelling says not to. */
          && psp_media_cold_boot_call_enabled(PSP_MEDIA_WIDE_PROGRAM_OFF)
          && psp_media_cold_boot_call_enabled(PSP_MEDIA_WIDE_PROGRAM_WIDE)
          && psp_media_cold_boot_call_enabled(
                 PSP_MEDIA_WIDE_PROGRAM_WIDE_ANNEXB)
          && psp_media_cold_boot_call_enabled(
                 PSP_MEDIA_WIDE_PROGRAM_BASELINE_BOOT4)
          && psp_media_cold_boot_call_enabled(
                 PSP_MEDIA_WIDE_PROGRAM_EDRAM_REAL)
          && !psp_media_cold_boot_call_enabled(
                 PSP_MEDIA_WIDE_PROGRAM_NO_BOOT)
          /* Neither spelling drags a program selection along with it. */
          && !psp_media_wide_program_enabled(
                 PSP_MEDIA_WIDE_PROGRAM_EDRAM_REAL)
          && !psp_media_wide_program_enabled(PSP_MEDIA_WIDE_PROGRAM_NO_BOOT)
          && !psp_media_wide_program_annexb(
                 PSP_MEDIA_WIDE_PROGRAM_EDRAM_REAL)
          && !psp_media_wide_program_annexb(PSP_MEDIA_WIDE_PROGRAM_NO_BOOT)
          && !psp_media_baseline_boot_call_enabled(
                 PSP_MEDIA_WIDE_PROGRAM_EDRAM_REAL)
          && !psp_media_baseline_boot_call_enabled(
                 PSP_MEDIA_WIDE_PROGRAM_NO_BOOT));
    /*
     * The in-app picker's table. Every spelling it can offer must be one the
     * boot config gate accepts, or the UI could write a value that halts the
     * next boot; and a stored spelling must come back as its own row so the
     * picker opens on what the next boot will really use.
     */
    for (unsigned choice = 0;
         choice < PSP_MEDIA_WIDE_PROGRAM_CHOICE_COUNT; choice++) {
        const char *name = psp_media_wide_program_choice(choice);
        CHECK(name != NULL
              && psp_media_wide_program_name_valid(name)
              && psp_media_wide_program_choice_index(name) == choice
              && strcmp(psp_media_wide_program_name(
                            (int) psp_media_wide_program_from_name(name)),
                        name) == 0);
    }
    CHECK(PSP_MEDIA_WIDE_PROGRAM_CHOICE_COUNT == 6u
          && psp_media_wide_program_choice(
                 PSP_MEDIA_WIDE_PROGRAM_CHOICE_COUNT) == NULL
          && strcmp(psp_media_wide_program_choice(0), "off") == 0
          /* An absent setting opens on the promoted wide default; malformed
             input still fails closed to the explicit compatibility row. */
          && psp_media_wide_program_choice_index("") == 1u
          && psp_media_wide_program_choice_index(NULL) == 1u
          && psp_media_wide_program_choice_index("boot3") == 0u
          && psp_media_wide_program_choice_index("edram-real") == 4u
          && psp_media_wide_program_choice_index("no-boot") == 5u);
    /* The admission primitive still requires an enabled wide program. Boot
       configuration now supplies that for an absent setting; explicit off
       keeps this compatibility behavior. */
    CHECK(!psp_media_decoder_policy(
              PSP_MEDIA_AVC_PROFILE_MAIN, 30, 640, 360, false, false,
              &decoder_policy)
          && !psp_media_decoder_policy(
              PSP_MEDIA_AVC_PROFILE_MAIN, 30, 426, 273, false, false,
              &decoder_policy)
          && !psp_media_decoder_policy(
              PSP_MEDIA_AVC_PROFILE_MAIN, 30, 481, 240, false, false,
              &decoder_policy)
          && psp_media_wide_program_required(640, 360)
          && psp_media_wide_program_required(481, 240)
          && psp_media_wide_program_required(426, 273)
          && !psp_media_wide_program_required(480, 272)
          && psp_media_wide_program_from_name(NULL)
                 == PSP_MEDIA_WIDE_PROGRAM_OFF
          && psp_media_wide_program_from_name("")
                 == PSP_MEDIA_WIDE_PROGRAM_OFF
          && psp_media_wide_program_configured("")
                 == PSP_MEDIA_WIDE_PROGRAM_WIDE
          && psp_media_wide_program_configured(NULL)
                 == PSP_MEDIA_WIDE_PROGRAM_WIDE
          && psp_media_wide_program_configured("off")
                 == PSP_MEDIA_WIDE_PROGRAM_OFF
          && psp_media_wide_program_configured("invalid")
                 == PSP_MEDIA_WIDE_PROGRAM_OFF
          && psp_media_wide_program_from_name("off")
                 == PSP_MEDIA_WIDE_PROGRAM_OFF
          && psp_media_wide_program_from_name("Wide")
                 == PSP_MEDIA_WIDE_PROGRAM_OFF
          && psp_media_wide_program_from_name("wide")
                 == PSP_MEDIA_WIDE_PROGRAM_WIDE
          && psp_media_wide_program_from_name("wide-annexb")
                 == PSP_MEDIA_WIDE_PROGRAM_WIDE_ANNEXB
          && psp_media_wide_program_enabled(PSP_MEDIA_WIDE_PROGRAM_WIDE)
          && psp_media_wide_program_enabled(
                 PSP_MEDIA_WIDE_PROGRAM_WIDE_ANNEXB)
          && !psp_media_wide_program_enabled(PSP_MEDIA_WIDE_PROGRAM_OFF)
          && psp_media_wide_program_annexb(
                 PSP_MEDIA_WIDE_PROGRAM_WIDE_ANNEXB)
          && !psp_media_wide_program_annexb(PSP_MEDIA_WIDE_PROGRAM_WIDE)
          && psp_media_wide_program_name_valid("")
          && psp_media_wide_program_name_valid("off")
          && psp_media_wide_program_name_valid("wide")
          && psp_media_wide_program_name_valid("wide-annexb")
          && !psp_media_wide_program_name_valid("annexb")
          && !psp_media_wide_program_name_valid(NULL)
          && strcmp(psp_media_wide_program_name(
                        PSP_MEDIA_WIDE_PROGRAM_OFF), "off") == 0
          && strcmp(psp_media_wide_program_name(
                        PSP_MEDIA_WIDE_PROGRAM_WIDE), "wide") == 0
          && strcmp(psp_media_wide_program_name(
                        PSP_MEDIA_WIDE_PROGRAM_WIDE_ANNEXB),
                    "wide-annexb") == 0);
    /*
     * The Annex-B experiment rewrites the staging copy of an access unit in
     * place: every four-byte AVCC length becomes 00 00 00 01 and the buffer
     * keeps its size. Malformed framing must be refused, because the walk
     * cannot know where the next unit starts; the caller fails the submission
     * so a partially rewritten buffer never reaches firmware.
     */
    {
        unsigned char unit[] = {
            0x00, 0x00, 0x00, 0x03, 0x65, 0xb8, 0x40,
            0x00, 0x00, 0x00, 0x02, 0x41, 0x9a
        };
        static const unsigned char expected[] = {
            0x00, 0x00, 0x00, 0x01, 0x65, 0xb8, 0x40,
            0x00, 0x00, 0x00, 0x01, 0x41, 0x9a
        };
        unsigned nals = 99u;
        CHECK(psp_media_annexb_rewrite(
                  unit, sizeof(unit), 4u, &nals)
              && nals == 2u
              && memcmp(unit, expected, sizeof(unit)) == 0);
        /* The conversion is one-way: its own output is no longer AVCC, so a
           second pass must refuse rather than corrupt an already converted
           buffer. Only the submission path calls this, exactly once per
           access unit, but the property is worth pinning. */
        CHECK(!psp_media_annexb_rewrite(unit, sizeof(unit), 4u, &nals)
              && nals == 0u);

        unsigned char single[] = {0x00, 0x00, 0x00, 0x01, 0x67};
        CHECK(psp_media_annexb_rewrite(single, sizeof(single), 4u, NULL)
              && single[3] == 0x01 && single[4] == 0x67);

        /* A length that runs past the end of the sample. */
        unsigned char overrun[] = {0x00, 0x00, 0x00, 0x09, 0x65, 0xb8};
        nals = 99u;
        CHECK(!psp_media_annexb_rewrite(
                  overrun, sizeof(overrun), 4u, &nals)
              && nals == 0u
              && overrun[3] == 0x09);

        /* A zero-length unit, and a tail too short to hold another prefix. */
        unsigned char empty[] = {0x00, 0x00, 0x00, 0x00, 0x65};
        unsigned char tail[] = {
            0x00, 0x00, 0x00, 0x01, 0x65, 0x00, 0x00
        };
        CHECK(!psp_media_annexb_rewrite(empty, sizeof(empty), 4u, NULL)
              && !psp_media_annexb_rewrite(tail, sizeof(tail), 4u, NULL));

        /* Only a four-byte prefix can be replaced without moving bytes, and
           there is nothing to convert in an absent or empty sample. */
        unsigned char two_byte[] = {0x00, 0x01, 0x65};
        CHECK(!psp_media_annexb_rewrite(two_byte, sizeof(two_byte), 2u, NULL)
              && !psp_media_annexb_rewrite(two_byte, sizeof(two_byte), 1u,
                                           NULL)
              && !psp_media_annexb_rewrite(NULL, 4u, 4u, NULL)
              && !psp_media_annexb_rewrite(two_byte, 0, 4u, NULL));
    }
    /* YouTube's AVC renditions use exact recovery points carried by a
       non-IDR I picture. The PSP decoder accepts that picture and fatally
       rejects its first dependent reference picture. Normalize the recovery
       group without changing decoded pixels: recovery I -> IDR, frame numbers
       rebased inside the GOP, and the now-obsolete first-P MMCO removed. */
    {
        /* In-place frame-number rewriting must not synthesize an unescaped
           00 00 00..03 sequence across either edge of the edited field.
           Ordinary payload and the two removable unmark MMCO kinds remain
           admitted; long-term assignment/reset operations fail closed. */
        static const unsigned char safe_edit[] =
            {0x41, 0x80, 0x00, 0x04, 0x80};
        static const unsigned char unsafe_edit[] =
            {0x41, 0x80, 0x00, 0x00, 0x02, 0x80};
        CHECK(media_h264_psp_compat_ebsp_edit_safe(
                  safe_edit, sizeof(safe_edit), 24u, 4u)
              && !media_h264_psp_compat_ebsp_edit_safe(
                  unsafe_edit, sizeof(unsafe_edit), 24u, 4u)
              && media_h264_psp_compat_mmco_removable(1u)
              && media_h264_psp_compat_mmco_removable(2u)
              && !media_h264_psp_compat_mmco_removable(0u)
              && !media_h264_psp_compat_mmco_removable(3u)
              && !media_h264_psp_compat_mmco_removable(4u)
              && !media_h264_psp_compat_mmco_removable(5u)
              && !media_h264_psp_compat_mmco_removable(6u));
        static const unsigned char avcc[] = {
            0x01,0x4d,0x40,0x15,0xff,0xe1,0x00,0x1b,
            0x67,0x4d,0x40,0x15,0xe8,0x80,0xd8,0xff,
            0x27,0x80,0xb5,0x01,0x01,0x01,0x40,0x00,
            0x00,0xfa,0x40,0x00,0x2e,0xe0,0x03,0xc5,
            0x8b,0x44,0x80,0x01,0x00,0x04,0x68,0xeb,
            0xef,0x20
        };
        MediaH264PspCompat compat;
        CHECK(media_h264_psp_compat_init(
            &compat, avcc, sizeof(avcc)) && compat.enabled);
        unsigned char recovery[64] = {
            0x00,0x00,0x00,0x05, 0x06,0x06,0x01,0xc4,0x80,
            0x00,0x00,0x00,0x0c,
            0x41,0x88,0x82,0x02,0xbf,0x23,0x5c,0xf0,0x51,0x1f,0xab,0x3c
        };
        static const unsigned char recovery_expected[] = {
            0x00,0x00,0x00,0x05, 0x06,0x06,0x01,0xc4,0x80,
            0x00,0x00,0x00,0x0c,
            0x45,0x88,0x85,0x00,0xaf,0x23,0x5c,0xf0,0x51,0x1f,0xab,0x3c
        };
        size_t recovery_length = sizeof(recovery_expected);
        CHECK(media_h264_psp_compat_transform(
                  &compat, recovery, &recovery_length, sizeof(recovery))
                  == MEDIA_H264_PSP_COMPAT_REWRITTEN
              && recovery_length == sizeof(recovery_expected)
              && memcmp(recovery, recovery_expected, recovery_length) == 0
              && compat.active_gop && compat.first_reference_pending
              && compat.recovery_points_rewritten == 1u);
        unsigned char first_p[64] = {
            0x00,0x00,0x00,0x0c,
            0x41,0x9a,0x2c,0xf9,0x32,0x9a,0x58,0x4b,0xff,0xfb,0xb1,0xfe
        };
        static const unsigned char first_p_expected[] = {
            0x00,0x00,0x00,0x0a,
            0x41,0x9a,0x2c,0xf9,0x31,0x09,0x7f,0xfb,0xb1,0xfe
        };
        size_t first_p_length = 16u;
        MediaH264PspCompatResult first_p_result =
            media_h264_psp_compat_transform(
                &compat, first_p, &first_p_length, sizeof(first_p));
        if (first_p_result != MEDIA_H264_PSP_COMPAT_REWRITTEN
            || first_p_length != sizeof(first_p_expected)
            || memcmp(first_p, first_p_expected, first_p_length) != 0) {
            fprintf(stderr, "compat result=%d length=%zu expected=%zu bytes=",
                    first_p_result, first_p_length,
                    sizeof(first_p_expected));
            for (size_t at = 0; at < first_p_length; at++)
                fprintf(stderr, "%02x", first_p[at]);
            fputc('\n', stderr);
        }
        CHECK(first_p_result == MEDIA_H264_PSP_COMPAT_REWRITTEN
              && first_p_length == sizeof(first_p_expected)
              && memcmp(first_p, first_p_expected, first_p_length) == 0
              && !compat.first_reference_pending
              && compat.reference_markings_removed == 1u);

        /* Some current YouTube renditions put recovery_point and
           user_data_unregistered in one SEI NAL. The extra message must not
           hide the closed recovery point, nor be modified by the adapter. */
        media_h264_psp_compat_reset(&compat);
        unsigned char combined_recovery[96] = {
            0x00,0x00,0x00,0x17,
            0x06, 0x06,0x01,0xc4,
            0x05,0x10,
            0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,
            0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff,0x00,
            0x80,
            0x00,0x00,0x00,0x0c,
            0x41,0x88,0x82,0x02,0xbf,0x23,0x5c,0xf0,0x51,0x1f,0xab,0x3c
        };
        static const unsigned char combined_expected[] = {
            0x00,0x00,0x00,0x17,
            0x06, 0x06,0x01,0xc4,
            0x05,0x10,
            0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,
            0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff,0x00,
            0x80,
            0x00,0x00,0x00,0x0c,
            0x45,0x88,0x85,0x00,0xaf,0x23,0x5c,0xf0,0x51,0x1f,0xab,0x3c
        };
        size_t combined_length = sizeof(combined_expected);
        CHECK(media_h264_psp_compat_transform(
                  &compat, combined_recovery, &combined_length,
                  sizeof(combined_recovery))
                  == MEDIA_H264_PSP_COMPAT_REWRITTEN
              && combined_length == sizeof(combined_expected)
              && memcmp(combined_recovery, combined_expected,
                        combined_length) == 0
              && compat.recovery_points_rewritten == 2u);

        /* A recovery point that is not exact/closed must not arm a rewrite. */
        media_h264_psp_compat_reset(&compat);
        unsigned char broken_recovery[] = {
            0x00,0x00,0x00,0x05, 0x06,0x06,0x01,0xe4,0x80
        };
        size_t broken_length = sizeof(broken_recovery);
        CHECK(media_h264_psp_compat_transform(
                  &compat, broken_recovery, &broken_length,
                  sizeof(broken_recovery))
                  == MEDIA_H264_PSP_COMPAT_PASSTHROUGH
              && broken_length == sizeof(broken_recovery));
        media_h264_psp_compat_reset(&compat);
        CHECK(!compat.active_gop && !compat.first_reference_pending
              && compat.enabled);
        /* Unsupported NAL prefix widths bypass the adapter instead of making
           otherwise valid browser media fail admission. */
        unsigned char avcc_two[sizeof(avcc)];
        memcpy(avcc_two, avcc, sizeof(avcc_two));
        avcc_two[4] = 0xfdu;
        CHECK(!media_h264_psp_compat_init(
            &compat, avcc_two, sizeof(avcc_two)) && !compat.enabled);
    }
    /* The wide program is only latched off when it rejects the very first
       access unit it is given. Anything the decoder primed on is an ordinary
       stream failure and must keep the per-stream retry ladder. */
    CHECK(psp_media_wide_program_rejected_by(5, 1, true, false)
          && !psp_media_wide_program_rejected_by(5, 1, true, true)
          && !psp_media_wide_program_rejected_by(5, 1, false, false)
          && !psp_media_wide_program_rejected_by(4, 3, true, false)
          && !psp_media_wide_program_rejected_by(4, 1, true, false)
          && !psp_media_wide_program_rejected_by(5, 3, true, false)
          && psp_media_admitted_quality(360u, false, true) == 360u
          && psp_media_admitted_quality(360u, true, true) == 240u
          && psp_media_admitted_quality(240u, true, true) == 240u
          && psp_media_admitted_quality(240u, false, true) == 240u
          && psp_media_admitted_quality(0u, true, true) == 0u
          /* The knob being off clamps on its own, whether or not the wide
             program has had the chance to be rejected. */
          && psp_media_admitted_quality(360u, false, false) == 240u
          && psp_media_admitted_quality(360u, true, false) == 240u
          && psp_media_admitted_quality(240u, false, false) == 240u);
    CHECK(PSP_MEDIA_DDR_BYTES == 2u * 1024u * 1024u
          && PSP_MEDIA_DDR_ALIGNMENT == 4u * 1024u * 1024u
          && PSP_MEDIA_DEFAULT_ME_BOOT_TYPE == 3
          && PSP_MEDIA_AUDIO_QUEUE_SLOTS == 24u
          && psp_media_decode_clock_us(0, false) == 0
          && psp_media_decode_clock_us(0, true)
                 == PSP_MEDIA_FIRST_FRAME_PREROLL_US
                    - PSP_MEDIA_DECODE_LEAD_US
          && psp_media_decode_clock_us(UINT64_C(700000), true)
                 == UINT64_C(700000)
          && !psp_media_deadline_reached(0, UINT64_C(9000000), 1)
          && !psp_media_deadline_reached(
                 UINT64_C(100), UINT64_C(5099), UINT64_C(5000))
          && psp_media_deadline_reached(
                 UINT64_C(100), UINT64_C(5100), UINT64_C(5000))
          && psp_media_deadline_reached(
                 UINT64_C(100),
                 UINT64_C(100) + PSP_MEDIA_MODULE_PREPARE_TIMEOUT_US,
                 PSP_MEDIA_MODULE_PREPARE_TIMEOUT_US)
          && !psp_media_deadline_reached(
                 UINT64_C(5100), UINT64_C(100), UINT64_C(5000))
          && !psp_media_retry_should_resume(false, false)
          && psp_media_retry_should_resume(true, false)
          && psp_media_retry_should_resume(false, true)
          && !psp_media_retry_preview_should_resume(
                 false, false, true, false)
          && psp_media_retry_preview_should_resume(
                 false, false, true, true)
          && psp_media_retry_preview_should_resume(
                 true, false, false, false)
          && !psp_media_seek_reopens_backend(
                 UINT64_C(100000000), UINT64_C(70000001), false)
          && psp_media_seek_reopens_backend(
                 UINT64_C(100000000), UINT64_C(70000000), false)
          && !psp_media_seek_reopens_backend(
                 UINT64_C(100000000), UINT64_C(70000000), true)
          && !psp_media_seek_reopens_backend(
                 UINT64_C(70000000), UINT64_C(100000000), false)
          && psp_media_audio_prefer_standard_channel(44100u)
          && !psp_media_audio_prefer_standard_channel(48000u)
          && !psp_media_audio_prefer_standard_channel(32000u)
          && psp_media_audio_reset_action(false, 0)
                 == PSP_MEDIA_AUDIO_RESET_READY
          && psp_media_audio_reset_action(true, 0)
                 == PSP_MEDIA_AUDIO_RESET_WAIT
          && psp_media_audio_reset_action(
                 true, PSP_MEDIA_AUDIO_RESET_WAIT_US)
                 == PSP_MEDIA_AUDIO_RESET_TIMEOUT
          && psp_media_audio_stream_admitted(
                 48000u, 2u, 48000u, 2u, 1024u)
          && !psp_media_audio_stream_admitted(
                 48000u, 1u, 48000u, 1u, 1024u)
          && !psp_media_audio_stream_admitted(
                 48000u, 2u, 44100u, 2u, 1024u)
          && !psp_media_audio_stream_admitted(
                 48000u, 2u, 48000u, 2u, 960u)
          && psp_media_audio_stream_admitted(
                 8000u, 2u, 8000u, 2u, 1024u)
          && !psp_media_audio_stream_admitted(
                 7350u, 2u, 7350u, 2u, 1024u)
          && !psp_media_audio_stream_admitted(
                 12345u, 2u, 12345u, 2u, 1024u)
          && !psp_media_runtime_reset_after_failure(0, true)
          && !psp_media_runtime_reset_after_failure(-1, false)
          && psp_media_runtime_reset_after_failure(-1, true)
          && psp_media_surface_policy(320, 240, &surface_policy)
          && surface_policy.stride_pixels == 512
          && surface_policy.surface_rows == 272
          && surface_policy.surface_bytes
                 == (size_t) 512 * 272 * sizeof(uint32_t)
          && surface_policy.external_reserve_bytes
                 == 3584u * 1024u + 557056u);
    CHECK(psp_media_surface_policy(640, 360, &surface_policy)
          && surface_policy.stride_pixels == 768
          && surface_policy.surface_rows == 368
          && surface_policy.surface_bytes
                 == (size_t) 768 * 368 * sizeof(uint32_t)
          && surface_policy.external_reserve_bytes
                 == 4u * 1024u * 1024u + 1130496u
          && !psp_media_surface_policy(641, 360, &surface_policy)
          && !psp_media_surface_policy(640, 361, &surface_policy));
    CHECK(psp_media_macroblock_align(426) == 432u
          && psp_media_macroblock_align(432) == 432u
          && psp_media_macroblock_align(240) == 240u
          && psp_media_macroblock_align(250) == 256u
          && psp_media_macroblock_align(272) == 272u
          && psp_media_macroblock_align(0) == 0u
          && psp_media_macroblock_align(0xfff0u) == 0xfff0u
          && psp_media_macroblock_align(0xfff1u) == 0u
          && psp_media_macroblock_align(0xffffu) == 0u);
    /* Firmware reports the CODED size. A 426x240 stream is coded 432x240, and
       that is agreement, not a mid-stream geometry change. */
    CHECK(psp_media_decoded_geometry_admitted(432, 240, 426, 240)
          && psp_media_decoded_geometry_admitted(480, 272, 480, 272)
          && psp_media_decoded_geometry_admitted(640, 368, 640, 360)
          /* A live 360p rendition used by the device gate is authored at
             638 visible pixels inside the same 640x368 coded picture. */
          && psp_media_decoded_geometry_admitted(640, 368, 638, 360)
          && psp_media_decoded_geometry_admitted(256, 240, 250, 240));
    /* 400 and 432 are both whole macroblock widths, so a 432-wide picture
       arriving for a 400-wide source is a real change and must still fail --
       as must a picture firmware could not describe at all. */
    CHECK(!psp_media_decoded_geometry_admitted(432, 240, 400, 240)
          && !psp_media_decoded_geometry_admitted(432, 256, 426, 240)
          && !psp_media_decoded_geometry_admitted(426, 240, 426, 240)
          && !psp_media_decoded_geometry_admitted(0, 0, 426, 240)
          && !psp_media_decoded_geometry_admitted(-432, 240, 426, 240)
          && !psp_media_decoded_geometry_admitted(432, 240, 0, 0));
    /* Every admissible source must pad inside its own protected surface. */
    CHECK(psp_media_surface_policy(426, 240, &surface_policy)
          && surface_policy.stride_pixels == 512
          && psp_media_surface_covers_decoded(&surface_policy, 426, 240)
          && psp_media_surface_policy(480, 272, &surface_policy)
          && psp_media_surface_covers_decoded(&surface_policy, 480, 272)
          && psp_media_surface_policy(640, 360, &surface_policy)
          && psp_media_surface_covers_decoded(&surface_policy, 640, 360)
          && psp_media_surface_policy(480, 250, &surface_policy)
          && psp_media_surface_covers_decoded(&surface_policy, 480, 250)
          && !psp_media_surface_covers_decoded(&surface_policy, 0, 240)
          && !psp_media_surface_covers_decoded(NULL, 426, 240));
    for (unsigned probe_width = 1; probe_width <= 640u; probe_width++) {
        for (unsigned probe_height = 1; probe_height <= 360u;
             probe_height++) {
            PspMediaSurfacePolicy probe = {0};
            if (!psp_media_surface_policy(
                    probe_width, probe_height, &probe)) return 1;
            CHECK(psp_media_surface_covers_decoded(
                &probe, probe_width, probe_height));
        }
    }
    CHECK(psp_media_transport_refresh_policy(
              0u, false, 403, 200, 0, 1000)
          && psp_media_transport_refresh_policy(
              1u, false, 403, 200, 0, 1000)
          && psp_media_transport_refresh_policy(
              0u, false, 200, 200, 1029, 1000)
          && !psp_media_transport_refresh_policy(
              0u, false, 200, 200, 1031, 1000)
          && !psp_media_transport_refresh_policy(
              PSP_MEDIA_TRANSPORT_REFRESH_MAXIMUM_ATTEMPTS,
              false, 403, 200, 0, 1000)
          && !psp_media_transport_refresh_policy(
              0u, true, 403, 200, 0, 1000));
    CHECK(!psp_media_transport_recovery_stable(
              1u, UINT64_C(5000000), UINT64_C(4999999), true)
          && !psp_media_transport_recovery_stable(
              1u, UINT64_C(5000000), UINT64_C(5000000), false)
          && !psp_media_transport_recovery_stable(
              0u, UINT64_C(5000000), UINT64_C(5000000), true)
          && psp_media_transport_recovery_stable(
              2u, UINT64_C(5000000), UINT64_C(5000000), true));
    uint32_t surface_canary[8] = {0};
    psp_media_surface_canary_fill(
        surface_canary,
        (unsigned) (sizeof(surface_canary) / sizeof(surface_canary[0])));
    CHECK(!psp_media_surface_canary_was_overwritten(
              surface_canary,
              (unsigned) (sizeof(surface_canary)
                          / sizeof(surface_canary[0])))
          && surface_canary[0] == PSP_MEDIA_SURFACE_CANARY_A
          && surface_canary[1] == PSP_MEDIA_SURFACE_CANARY_B);
    surface_canary[7] = 0;
    CHECK(!psp_media_surface_canary_was_overwritten(
        surface_canary,
        (unsigned) (sizeof(surface_canary) / sizeof(surface_canary[0]))));
    memset(surface_canary, 0, sizeof(surface_canary));
    CHECK(psp_media_surface_canary_was_overwritten(
        surface_canary,
        (unsigned) (sizeof(surface_canary) / sizeof(surface_canary[0]))));
    CHECK(!psp_media_raw_nal_probe_exhausted(
              PSP_MEDIA_RAW_NAL_PROBE_PACKETS - 1u, false)
          && psp_media_raw_nal_probe_exhausted(
              PSP_MEDIA_RAW_NAL_PROBE_PACKETS, false)
          && !psp_media_raw_nal_probe_exhausted(
              PSP_MEDIA_RAW_NAL_PROBE_PACKETS, true));
    /* The raw-NAL diagnostic must not impose a smaller, invented reorder
       limit than the timestamp queue. In particular, neither an integer
       24-fps minimum nor an eight-frame DPB is enforced at admission. */
    CHECK(PSP_MEDIA_RAW_NAL_PROBE_PACKETS == 16u
          && PSP_MEDIA_RAW_NAL_PROBE_PACKETS
                 == PSP_MEDIA_VIDEO_TIMESTAMP_SLOTS);
    /*
     * The paused preroll must never let decoded audio fill the PCM queue
     * before a video packet can be selected: a full queue answers
     * WOULD_BLOCK, and because the paused audio worker never drains, the
     * pump would then re-select the same earlier audio timestamp forever.
     * The horizon has to stop admitting audio before the queue is full.
     */
    for (unsigned rate = 8000u; rate <= PSP_MEDIA_AUDIO_MAXIMUM_RATE;
         rate += 8000u) {
        uint64_t horizon = PSP_MEDIA_FIRST_FRAME_PREROLL_US;
        uint64_t unit_us =
            (UINT64_C(1000000) * PSP_MEDIA_AUDIO_SAMPLES) / rate;
        uint64_t admitted = 1u + horizon / unit_us;
        CHECK(admitted <= PSP_MEDIA_AUDIO_QUEUE_SLOTS);
    }
    /*
     * The first-frame watchdog is a progress watchdog. Genuine progress
     * rearms it, the absolute cap still fires, and the classification decides
     * whether the one-shot 240p fallback is spent.
     */
    uint64_t opened = UINT64_C(1000000);
    /* Idle short of the window is not a stall. */
    CHECK(psp_media_first_frame_verdict(
              opened, opened, 0,
              opened + PSP_MEDIA_FIRST_FRAME_TIMEOUT_US - 1u, false)
          == PSP_MEDIA_FIRST_FRAME_WAITING);
    /* Progress observed on this pump rearms the window instead of expiring. */
    CHECK(psp_media_first_frame_verdict(
              opened, opened, 0,
              opened + PSP_MEDIA_FIRST_FRAME_TIMEOUT_US, true)
          == PSP_MEDIA_FIRST_FRAME_WAITING);
    /* Five idle seconds spent outside the pump is a stalled decoder. */
    CHECK(psp_media_first_frame_verdict(
              opened, opened, 0,
              opened + PSP_MEDIA_FIRST_FRAME_TIMEOUT_US, false)
          == PSP_MEDIA_FIRST_FRAME_DECODER_STALLED);
    /* The same window spent blocked in range reads is a starved network. */
    CHECK(psp_media_first_frame_verdict(
              opened, opened, UINT64_C(4900000),
              opened + PSP_MEDIA_FIRST_FRAME_TIMEOUT_US, false)
          == PSP_MEDIA_FIRST_FRAME_NETWORK_STALLED);
    /* A brief blocking read inside a long idle window is not the cause. */
    CHECK(psp_media_first_frame_verdict(
              opened, opened,
              PSP_MEDIA_FIRST_FRAME_NETWORK_PUMP_US - 1u,
              opened + PSP_MEDIA_FIRST_FRAME_TIMEOUT_US, false)
          == PSP_MEDIA_FIRST_FRAME_DECODER_STALLED);
    /* Nor is a read that covered less than half of it. */
    CHECK(psp_media_first_frame_verdict(
              opened, opened, UINT64_C(2000000),
              opened + PSP_MEDIA_FIRST_FRAME_TIMEOUT_US, false)
          == PSP_MEDIA_FIRST_FRAME_DECODER_STALLED);
    /* Steady progress never expires the idle window ... */
    for (unsigned step = 1u; step <= 20u; step++) {
        uint64_t progress = opened
            + (uint64_t) step * (PSP_MEDIA_FIRST_FRAME_TIMEOUT_US - 1u);
        CHECK(psp_media_first_frame_verdict(
                  opened, progress, UINT64_C(4000000),
                  progress + PSP_MEDIA_FIRST_FRAME_TIMEOUT_US - 1u, false)
              == (progress + PSP_MEDIA_FIRST_FRAME_TIMEOUT_US - 1u
                      >= opened + PSP_MEDIA_FIRST_FRAME_ABSOLUTE_US
                  ? PSP_MEDIA_FIRST_FRAME_NETWORK_STALLED
                  : PSP_MEDIA_FIRST_FRAME_WAITING));
    }
    /* ... but the absolute cap fires through progress, and a pipeline that
       keeps receiving bytes without producing a picture is still the network's
       fault, not the decoder's. */
    CHECK(psp_media_first_frame_verdict(
              opened, opened + PSP_MEDIA_FIRST_FRAME_ABSOLUTE_US
                  - UINT64_C(4000000),
              UINT64_C(4000000),
              opened + PSP_MEDIA_FIRST_FRAME_ABSOLUTE_US, true)
          == PSP_MEDIA_FIRST_FRAME_NETWORK_STALLED);
    /* A decoder cycling quickly without a picture caps out as a decoder
       stall even though it makes packet progress every pump. */
    CHECK(psp_media_first_frame_verdict(
              opened, opened + PSP_MEDIA_FIRST_FRAME_ABSOLUTE_US - 1000u,
              1000u, opened + PSP_MEDIA_FIRST_FRAME_ABSOLUTE_US, true)
          == PSP_MEDIA_FIRST_FRAME_DECODER_STALLED);
    /* A retired watchdog never reports a stall. */
    CHECK(psp_media_first_frame_verdict(
              0, 0, 0, UINT64_C(600000000), false)
          == PSP_MEDIA_FIRST_FRAME_WAITING);
    CHECK(psp_media_timestamp_push(
              &timestamp_queue, UINT64_C(80000), UINT64_C(40000))
          && psp_media_timestamp_push(
              &timestamp_queue, 0, UINT64_C(40000))
          && psp_media_timestamp_push(
              &timestamp_queue, UINT64_C(40000), UINT64_C(40000)));
    PspMediaVideoTimestamp timestamp;
    CHECK(psp_media_timestamp_pop(&timestamp_queue, &timestamp)
          && timestamp.pts_us == 0
          && psp_media_timestamp_pop(&timestamp_queue, &timestamp)
          && timestamp.pts_us == UINT64_C(40000)
          && psp_media_timestamp_pop(&timestamp_queue, &timestamp)
          && timestamp.pts_us == UINT64_C(80000)
          && !psp_media_timestamp_pop(&timestamp_queue, &timestamp));
    for (unsigned at = 0; at < PSP_MEDIA_VIDEO_TIMESTAMP_SLOTS; at++)
        CHECK(psp_media_timestamp_push(
            &timestamp_queue, at, UINT64_C(40000)));
    CHECK(!psp_media_timestamp_push(
        &timestamp_queue, UINT64_C(999), UINT64_C(40000)));
    timestamp_queue = (PspMediaVideoTimestampQueue) {0};
    CHECK(psp_media_timestamp_push(
              &timestamp_queue, UINT64_C(40000), UINT64_C(41708))
          /* Timestamp admission precedes the native decode call so a
             zero-reorder stream may return its first picture immediately. */
          && psp_media_output_batch_admitted(
                 1, timestamp_queue.count)
          && psp_media_video_drain_action(
                 timestamp_queue.count, 0, PSP_MEDIA_SURFACE_SLOTS, 0, 0)
               == PSP_MEDIA_VIDEO_DRAIN_CALL_NATIVE
          && !psp_media_output_batch_admitted(
                 0, timestamp_queue.count)
          && timestamp_queue.count == 1
          && psp_media_video_drain_action(
                 timestamp_queue.count, 0, 0, 0, 0)
               == PSP_MEDIA_VIDEO_DRAIN_WAIT_FOR_SURFACE
          && psp_media_output_batch_admitted(
                 1, timestamp_queue.count)
          && psp_media_timestamp_pop(
                 &timestamp_queue, &timestamp)
          && timestamp.pts_us == UINT64_C(40000)
          && timestamp.duration_us == UINT64_C(41708)
          && timestamp_queue.count == 0
          && psp_media_video_drain_action(
                 timestamp_queue.count, 0, 0,
                 PSP_MEDIA_VIDEO_DRAIN_CALLS,
                 PSP_MEDIA_VIDEO_DRAIN_SURFACE_POLLS - 1u)
               == PSP_MEDIA_VIDEO_DRAIN_WAIT_FOR_SURFACE
          && psp_media_video_drain_action(
                 timestamp_queue.count, 0, 0,
                 PSP_MEDIA_VIDEO_DRAIN_CALLS,
                 PSP_MEDIA_VIDEO_DRAIN_SURFACE_POLLS)
               == PSP_MEDIA_VIDEO_DRAIN_DROP_SURFACE
          && psp_media_video_drain_action(
                 timestamp_queue.count, 1, 1,
                 PSP_MEDIA_VIDEO_DRAIN_CALLS, 0)
               == PSP_MEDIA_VIDEO_DRAIN_EMIT_PENDING
          && psp_media_video_drain_action(
                 timestamp_queue.count, 0, 1,
                 PSP_MEDIA_VIDEO_DRAIN_CALLS, 0)
               == PSP_MEDIA_VIDEO_DRAIN_COMPLETE);
    CHECK(!psp_media_output_batch_admitted(-1, 1)
          && !psp_media_output_batch_admitted(2, 1)
          && psp_media_output_count_sane(0)
          && psp_media_output_count_sane(
              PSP_MEDIA_VIDEO_TIMESTAMP_SLOTS)
          && !psp_media_output_count_sane(-1)
          && !psp_media_output_count_sane(
              PSP_MEDIA_VIDEO_TIMESTAMP_SLOTS + 1)
          && psp_media_output_batch_admitted(
              PSP_MEDIA_VIDEO_TIMESTAMP_SLOTS,
              PSP_MEDIA_VIDEO_TIMESTAMP_SLOTS)
          && !psp_media_output_batch_admitted(
              PSP_MEDIA_VIDEO_TIMESTAMP_SLOTS + 1,
              PSP_MEDIA_VIDEO_TIMESTAMP_SLOTS + 1));
    media_mp4_rewind(split_video);
    media_mp4_rewind(split_audio);
    backend_fixture = (BackendFixture) {0};
    playback = media_playback_create_split(
        &budget, split_video, split_audio, &backend, &playback_options,
        error, sizeof(error));
    uint64_t split_seek_actual = UINT64_MAX;
    CHECK(playback != NULL
          && media_playback_seek(
                 playback, UINT64_C(1500000), &split_seek_actual,
                 error, sizeof(error))
          && split_seek_actual == 0);
    uint64_t primed_video_us = UINT64_MAX;
    uint64_t primed_audio_us = UINT64_MAX;
    split_video_reader.block_polls = 1;
    split_audio_reader.block_polls = 1;
    CHECK(media_playback_prime_video_source(
              playback, UINT64_C(1500000), &primed_video_us,
              error, sizeof(error)) == MEDIA_PLAYBACK_SOURCE_PRIME_PENDING);
    /* This compact synthetic second source ends before the target; known EOF
       is ready because it cannot produce a later transport stall. */
    CHECK(media_playback_prime_audio_source(
              playback, UINT64_C(1500000), &primed_audio_us,
              error, sizeof(error)) == MEDIA_PLAYBACK_SOURCE_PRIME_READY);
    split_audio_reader.block_polls = 0;
    CHECK(media_playback_prime_video_source(
              playback, UINT64_C(1500000), &primed_video_us,
              error, sizeof(error)) == MEDIA_PLAYBACK_SOURCE_PRIME_READY
          && media_playback_prime_audio_source(
              playback, UINT64_C(1500000), &primed_audio_us,
              error, sizeof(error)) == MEDIA_PLAYBACK_SOURCE_PRIME_READY
          && primed_video_us <= UINT64_C(1500000)
          && primed_audio_us >= UINT64_C(1500000));
    /* Priming is a readiness proof, not consumption: the decode sequence
       below remains byte-for-byte the same as a plain committed seek. */
    CHECK(media_playback_advance_bounded(
              playback, UINT64_C(2000000), 1,
              error, sizeof(error))
              == MEDIA_PLAYBACK_ADVANCE_PENDING
          && backend_fixture.submitted == 1);
    /* The skipped pre-target secondary sample consumes this entire bounded
       pump even though it is intentionally never submitted to the backend. */
    CHECK(media_playback_advance_bounded(
              playback, UINT64_C(2000000), 1,
              error, sizeof(error))
              == MEDIA_PLAYBACK_ADVANCE_PENDING
          && backend_fixture.submitted == 1);
    CHECK(media_playback_advance_bounded(
              playback, UINT64_C(2000000), 1,
              error, sizeof(error))
              == MEDIA_PLAYBACK_ADVANCE_PENDING
          && backend_fixture.submitted == 2);
    CHECK(media_playback_advance_bounded(
              playback, UINT64_C(2000000), 1,
              error, sizeof(error))
              == MEDIA_PLAYBACK_ADVANCE_PENDING
          && backend_fixture.submitted == 2);
    CHECK(media_playback_advance_bounded(
              playback, UINT64_C(2000000), 1,
              error, sizeof(error))
              == MEDIA_PLAYBACK_ADVANCE_COMPLETE
          && backend_fixture.resets == 1
          && backend_fixture.submitted == 2);
    media_playback_destroy(playback);
    CHECK(backend_fixture.destroyed == 1);

    puts("test: warming the video connection leaves the decode where it was");
    /* A resume seek warms the video connection with a blocking read of its
       first sample, then must put the cursor back so the decode that follows
       begins at the target rather than one sample past it -- the device pays
       a 497ms TLS handshake here instead of on the first playing frame. The
       correctness a host can check is that the warm is transparent: advance
       after it reproduces exactly the submission sequence the un-warmed seek
       above produced. */
    media_mp4_rewind(split_video);
    media_mp4_rewind(split_audio);
    backend_fixture = (BackendFixture) {0};
    playback = media_playback_create_split(
        &budget, split_video, split_audio, &backend, &playback_options,
        error, sizeof(error));
    CHECK(playback != NULL
          && media_playback_seek(
              playback, UINT64_C(1500000), NULL, error, sizeof(error)));
    CHECK(media_playback_warm_video(
        playback, UINT64_C(1500000), error, sizeof(error)));
    CHECK(media_playback_advance_bounded(
              playback, UINT64_C(2000000), 1, error, sizeof(error))
              == MEDIA_PLAYBACK_ADVANCE_PENDING
          && backend_fixture.submitted == 1);
    CHECK(media_playback_advance_bounded(
              playback, UINT64_C(2000000), 1, error, sizeof(error))
              == MEDIA_PLAYBACK_ADVANCE_PENDING
          && backend_fixture.submitted == 1);
    CHECK(media_playback_advance_bounded(
              playback, UINT64_C(2000000), 1, error, sizeof(error))
              == MEDIA_PLAYBACK_ADVANCE_PENDING
          && backend_fixture.submitted == 2);
    media_playback_destroy(playback);

    puts("test: a sole AAC source primes and seeks as source zero");
    media_mp4_rewind(split_audio);
    backend_fixture = (BackendFixture) {0};
    playback = media_playback_create(
        &budget, split_audio, &backend, &playback_options,
        error, sizeof(error));
    CHECK(playback != NULL
          && media_playback_seek(
              playback, UINT64_C(1500000), NULL, error, sizeof(error))
          && media_playback_prime_audio_source(
              playback, UINT64_C(1500000), &primed_audio_us,
              error, sizeof(error)) == MEDIA_PLAYBACK_SOURCE_PRIME_READY
          && primed_audio_us >= UINT64_C(1500000));
    media_playback_destroy(playback);

    media_mp4_close(split_audio);
    media_mp4_close(split_video);

    puts("test: split seek failure stops instead of half-repositioning");
    FixtureReader failed_seek_video_reader = {
        .bytes = sidx_fixture.bytes,
        .length = sidx_fixture.length
    };
    FixtureReader failed_seek_audio_reader = {
        .bytes = sidx_fixture.bytes,
        .length = sidx_fixture.length,
        .fail_offset = second_moof_offset,
        .fail_enabled = true
    };
    MediaRangeReader failed_seek_video_source = {
        .opaque = &failed_seek_video_reader,
        .length = sidx_fixture.length,
        .read = fixture_read,
        .describe_failure = fixture_describe_failure
    };
    MediaRangeReader failed_seek_audio_source = {
        .opaque = &failed_seek_audio_reader,
        .length = sidx_fixture.length,
        .read = fixture_read,
        .describe_failure = fixture_describe_failure
    };
    split_video = media_mp4_open(
        &budget, &failed_seek_video_source, NULL,
        error, sizeof(error));
    split_audio = media_mp4_open(
        &budget, &failed_seek_audio_source, NULL,
        error, sizeof(error));
    CHECK(split_video != NULL && split_audio != NULL);
    backend_fixture = (BackendFixture) {0};
    playback = media_playback_create_split(
        &budget, split_video, split_audio, &backend, &playback_options,
        error, sizeof(error));
    CHECK(playback != NULL
          && !media_playback_seek(
              playback, UINT64_C(2500000), NULL,
              error, sizeof(error))
          && strstr(error, "range read failed") != NULL
          && media_playback_ended(playback)
          && backend_fixture.resets == 1
          && media_playback_advance_bounded(
              playback, UINT64_C(3000000), 1,
              error, sizeof(error))
             == MEDIA_PLAYBACK_ADVANCE_COMPLETE
          && backend_fixture.submitted == 0);
    media_playback_destroy(playback);
    media_mp4_close(split_audio);
    media_mp4_close(split_video);

    puts("test: a failed seek's clock adopts where the seek left the source");
    /*
     * The freeze the reconciliation rule exists for, reproduced at the layer
     * it lives on. A seek repositions the demuxer first and reaches a picture
     * second. When the second step fails and the clock is left where playback
     * was, the eligibility horizon (clock + lead) sits far behind everything
     * the source can now serve: every later pump exits on the horizon, nothing
     * is ever submitted again, and the session is dead until it is closed.
     * The device log recorded clock=2947000us against buffered=19783401us.
     */
    FixtureReader diverged_reader = {
        .bytes = long_sidx_fixture.bytes,
        .length = long_sidx_fixture.length
    };
    MediaRangeReader diverged_source = {
        .opaque = &diverged_reader,
        .length = long_sidx_fixture.length,
        .read = fixture_read
    };
    demux = media_mp4_open(
        &budget, &diverged_source, NULL, error, sizeof(error));
    CHECK(demux != NULL);
    MediaPlaybackOptions diverged_options = {
        /* The shipping horizon, so the arithmetic under test is the device's. */
        .decode_lead_us = PSP_MEDIA_DECODE_LEAD_US,
        .maximum_packet_bytes = 4096,
        .preallocate_maximum_packet_bytes = true
    };
    backend_fixture = (BackendFixture) {0};
    playback = media_playback_create(
        &budget, demux, &backend, &diverged_options, error, sizeof(error));
    uint64_t diverged_target_us = UINT64_C(1200000000);
    uint64_t diverged_stale_us = UINT64_C(10000000);
    CHECK(playback != NULL
          && media_playback_seek(
              playback, diverged_target_us, NULL, error, sizeof(error))
          && media_playback_buffered_until_us(playback)
              == diverged_target_us);
    for (size_t pump = 0; pump < 8u; pump++) {
        CHECK(media_playback_advance_bounded(
                  playback, diverged_stale_us, 1, error, sizeof(error))
              != MEDIA_PLAYBACK_ADVANCE_ERROR);
    }
    CHECK(backend_fixture.submitted == 0 && !media_playback_ended(playback));
    /* The stall line must name that state rather than the window it left
       pending: nothing asks the demuxer for the bytes that would retire it, so
       the pending window is the consequence, not the cause. */
    CHECK(psp_media_stall_suspect(
              diverged_stale_us, diverged_target_us, 5261u, 0u, true, true)
              == PSP_MEDIA_STALL_SUSPECT_CLOCK_DIVERGENCE);
    /* The rule: the clock adopts the seek's own target, and the same pipeline
       starts moving again without being torn down or reopened. */
    uint64_t diverged_reconciled_us = psp_media_seek_failure_clock_us(
        false, diverged_target_us, diverged_stale_us);
    CHECK(diverged_reconciled_us == diverged_target_us
          && media_playback_advance_bounded(
                 playback, diverged_reconciled_us, 1, error, sizeof(error))
              == MEDIA_PLAYBACK_ADVANCE_PENDING
          && backend_fixture.submitted == 1);
    /* A preview restore is the same rule read the other way: its seek pointed
       the demuxer back at the position it is restoring. */
    CHECK(psp_media_seek_failure_clock_us(
              true, diverged_target_us, diverged_stale_us)
          == diverged_stale_us);
    media_playback_destroy(playback);
    media_mp4_close(demux);

    puts("test: a seek warms on its own clock, not the open's");
    /* The budget a seek hands a blocking read is what its decode leg used to
       spend failing, and it only ever shrinks. Spent in full it reaches zero
       -- "no budget left" -- which the source turns into an immediate refusal
       rather than one more per-window wait; the warm is best effort, so that
       costs the first playing frames a fetch and never costs the seek. */
    CHECK(psp_media_seek_wait_budget_us(0, UINT64_C(9000000))
              == PSP_MEDIA_SEEK_TIMEOUT_US
          && psp_media_seek_wait_budget_us(
                 UINT64_C(1000000), UINT64_C(1002000))
              == PSP_MEDIA_SEEK_TIMEOUT_US - UINT64_C(2000)
          && psp_media_seek_wait_budget_us(
                 UINT64_C(1000000),
                 UINT64_C(1000000) + PSP_MEDIA_SEEK_TIMEOUT_US) == 0
          && psp_media_seek_wait_budget_us(
                 UINT64_C(9000000), UINT64_C(1000000))
              == PSP_MEDIA_SEEK_TIMEOUT_US);

    puts("test: the soak's seek target follows its configured fraction");
    /* 667 reproduces the two thirds the fraction was hardcoded to, on the
       exact duration the soak measured: 252261000us -> 168174087us, the
       target both wedged runs sought to. A third of the same stream lands
       before the region those runs died in, so the soak plays continuously
       into it instead of seeking past it. */
    CHECK(psp_media_stability_seek_target_us(UINT64_C(252261000), 667)
              == UINT64_C(168258087)
          && psp_media_stability_seek_target_us(UINT64_C(252261000), 333)
              == UINT64_C(84002913)
          /* A stream too short to seek into keeps the midpoint, and an
             unusable fraction falls back rather than landing outside. */
          && psp_media_stability_seek_target_us(UINT64_C(8000000), 333)
              == UINT64_C(4000000)
          && psp_media_stability_seek_target_us(UINT64_C(252261000), 0)
              == psp_media_stability_seek_target_us(
                     UINT64_C(252261000), 667));

    puts("test: a refill on the wire is not a stalled decoder");
    /*
     * The no-packet ceiling is a statement about a pipeline that has what it
     * needs, and a cold source has not. Two 256 KiB windows over a link that
     * delivers 150-250 KB/s are three to four seconds before the first access
     * unit can be accepted -- a device run died at two, with the refill still
     * on the wire and 134 source-blocked reads to prove it. The longer budget
     * is bought by an outstanding refill and by nothing else, so a window that
     * never lands still fails.
     */
    CHECK(psp_media_decode_no_progress_budget_ms(false)
              == PSP_MEDIA_DECODE_NO_PROGRESS_MS
          && psp_media_decode_no_progress_budget_ms(true)
              == PSP_MEDIA_DECODE_NO_PROGRESS_REFILL_MS
          && PSP_MEDIA_DECODE_NO_PROGRESS_REFILL_MS
              > PSP_MEDIA_DECODE_NO_PROGRESS_MS
          /* Above the worst cold fetch the device measured, and still bounded
             well inside the seek budget's scale. */
          && PSP_MEDIA_DECODE_NO_PROGRESS_REFILL_MS * 1000u
              >= PSP_MEDIA_SEEK_TIMEOUT_US);

    puts("test: intentional media holds do not trip the decoder watchdog");
    CHECK(psp_media_decode_no_progress_watchdog_active(false, false)
          && !psp_media_decode_no_progress_watchdog_active(true, false)
          && !psp_media_decode_no_progress_watchdog_active(false, true)
          && !psp_media_decode_no_progress_watchdog_active(true, true));

    puts("test: what an advance may afford depends on where it runs");
    /*
     * The frame's own advance must never block for the codec worker: the wait
     * gives the codec worker a bounded completion window, which is free
     * where the thread was already waiting on hardware and is frame time
     * anywhere else -- a device soak measured 8.8ms of every 46.4ms playing
     * frame inside it. Both dead-time pumps may take it.
     *
     * Only the pump that overlaps the stage copy holds video back, because
     * only there is something reading the surface a newly emitted picture is
     * converted into. Audio owns none of that memory and is never held.
     */
    CHECK(!psp_media_advance_may_wait(PSP_MEDIA_ADVANCE_FRAME)
          && psp_media_advance_may_wait(PSP_MEDIA_ADVANCE_DRAW)
          && psp_media_advance_may_wait(PSP_MEDIA_ADVANCE_STAGE_COPY)
          && psp_media_advance_may_submit_video(PSP_MEDIA_ADVANCE_FRAME)
          && psp_media_advance_may_submit_video(PSP_MEDIA_ADVANCE_DRAW)
          && !psp_media_advance_may_submit_video(
                 PSP_MEDIA_ADVANCE_STAGE_COPY));

    puts("test: a stalled pipeline names the cause above the consequence");
    /* A clock the source has not outrun is an ordinary stall, and the older
       three answers keep their order among themselves. */
    CHECK(psp_media_stall_suspect(
              UINT64_C(2000000),
              UINT64_C(2000000) + PSP_MEDIA_CLOCK_DIVERGENCE_US,
              9u, 0u, true, true)
              == PSP_MEDIA_STALL_SUSPECT_SOURCE_WINDOW
          /* A horizon that has never refused anything is not the diagnosis
             even when the source is far ahead -- that is a source which has
             legitimately run on, not a clock that stopped. */
          && psp_media_stall_suspect(
                 UINT64_C(2000000), UINT64_C(19000000), 0u, 0u, true, true)
              == PSP_MEDIA_STALL_SUSPECT_SOURCE_WINDOW
          && psp_media_stall_suspect(
                 UINT64_C(2000000), UINT64_C(2000000), 9u, 3u, false, false)
              == PSP_MEDIA_STALL_SUSPECT_DECODER_STAGING
          && psp_media_stall_suspect(
                 UINT64_C(2000000), UINT64_C(2000000), 9u, 3u, false, true)
              == PSP_MEDIA_STALL_SUSPECT_ELIGIBILITY_HORIZON
          && psp_media_stall_suspect(
                 UINT64_C(2000000), UINT64_C(2000000), 9u, 0u, false, false)
              == PSP_MEDIA_STALL_SUSPECT_ELIGIBILITY_HORIZON
          && strcmp(
                 psp_media_stall_suspect_name(
                     PSP_MEDIA_STALL_SUSPECT_CLOCK_DIVERGENCE),
                 "clock-divergence") == 0
          && strcmp(
                 psp_media_stall_suspect_name(
                     PSP_MEDIA_STALL_SUSPECT_SOURCE_WINDOW),
                 "source-window") == 0
          && strcmp(
                 psp_media_stall_suspect_name(
                     PSP_MEDIA_STALL_SUSPECT_DECODER_STAGING),
                 "decoder-staging") == 0
          && strcmp(
                 psp_media_stall_suspect_name(
                     PSP_MEDIA_STALL_SUSPECT_ELIGIBILITY_HORIZON),
                 "eligibility-horizon") == 0);

    puts("test: an open transaction is bounded and answers a stop request");
    /* The device run this exists for: modules ready, then more than ten
       minutes of nothing, no deadline, and a CIRCLE press that was never
       answered. Only module preparation had ever been deadlined; every other
       phase inherited the bound of whatever it called into, and per-read
       bounds do not compose into a per-phase one. */
    CHECK(psp_media_open_watch(false, 1000u, 1000u, 1000u)
              == PSP_MEDIA_OPEN_WATCH_CONTINUE
          /* Cancellation outranks both clocks, and is answered even on the
             first unit of the first phase. */
          && psp_media_open_watch(true, 1000u, 1000u, 1000u)
              == PSP_MEDIA_OPEN_WATCH_CANCELLED
          && psp_media_open_watch(
                 true, 1000u, 1000u,
                 1000u + PSP_MEDIA_OPEN_TOTAL_TIMEOUT_US)
              == PSP_MEDIA_OPEN_WATCH_CANCELLED
          /* A phase that stops progressing fails the open before the total
             does, so the log names the stage rather than the transaction. */
          && psp_media_open_watch(
                 false, 1000u, 1000u,
                 1000u + PSP_MEDIA_OPEN_PHASE_TIMEOUT_US)
              == PSP_MEDIA_OPEN_WATCH_PHASE_TIMEOUT
          /* Phases that each stay inside their own bound still cannot add up
             to an unbounded open. */
          && psp_media_open_watch(
                 false, 1000u,
                 1000u + PSP_MEDIA_OPEN_TOTAL_TIMEOUT_US,
                 1000u + PSP_MEDIA_OPEN_TOTAL_TIMEOUT_US)
              == PSP_MEDIA_OPEN_WATCH_TOTAL_TIMEOUT
          /* A phase clock that has not been armed leaves only the total. */
          && psp_media_open_watch(
                 false, 1000u, 0u,
                 1000u + PSP_MEDIA_OPEN_PHASE_TIMEOUT_US)
              == PSP_MEDIA_OPEN_WATCH_CONTINUE
          /* A healthy open is twenty to twenty-five seconds on a PSP-3000, so
             neither bound may fire inside that. */
          && psp_media_open_watch(false, 0u, 0u, UINT64_C(25000000))
              == PSP_MEDIA_OPEN_WATCH_CONTINUE);
    /* What the phase hands to its blocking reads: the tighter of the two
       clocks, and zero -- not "unbounded" -- once it is spent. */
    CHECK(psp_media_open_wait_budget_us(1000u, 1000u, 1000u)
              == PSP_MEDIA_OPEN_PHASE_TIMEOUT_US
          && psp_media_open_wait_budget_us(
                 1000u, 1000u, 1000u + PSP_MEDIA_OPEN_PHASE_TIMEOUT_US) == 0
          && psp_media_open_wait_budget_us(
                 1000u,
                 1000u + PSP_MEDIA_OPEN_TOTAL_TIMEOUT_US
                     - PSP_MEDIA_OPEN_PHASE_TIMEOUT_US / 2u,
                 1000u + PSP_MEDIA_OPEN_TOTAL_TIMEOUT_US
                     - PSP_MEDIA_OPEN_PHASE_TIMEOUT_US / 2u)
              == PSP_MEDIA_OPEN_PHASE_TIMEOUT_US / 2u
          && psp_media_open_wait_budget_us(0u, 0u, 1000u)
              == PSP_MEDIA_OPEN_PHASE_TIMEOUT_US);

    puts("test: a refused access unit is survivable until a run of them is not");
    /* The device proved the bridge builds valid access units and firmware
       refuses particular content: au-before carried esSize=0x132, exactly the
       306-byte packet, and sceMpegAvcDecode still answered 0x80628002. One
       refusal costs a frame; ending the session costs the session. */
    CHECK(psp_media_avc_refusal_survivable(1u, 1u)
          && psp_media_avc_refusal_survivable(
                 PSP_MEDIA_AVC_REFUSAL_CONSECUTIVE_LIMIT - 1u, 1u)
          /* A short consecutive run is a stream the decoder will not take. */
          && !psp_media_avc_refusal_survivable(
                 PSP_MEDIA_AVC_REFUSAL_CONSECUTIVE_LIMIT, 1u)
          /* And a stream that only dribbles them still has to give up. */
          && !psp_media_avc_refusal_survivable(
                 1u, PSP_MEDIA_AVC_REFUSAL_WINDOW_LIMIT)
          && psp_media_avc_refusal_survivable(
                 1u, PSP_MEDIA_AVC_REFUSAL_WINDOW_LIMIT - 1u)
          /* The observed shape -- isolated refusals inside a healthy stream --
             must survive at 30fps for a full minute. */
          && psp_media_avc_refusal_survivable(1u, 30u));

    puts("test: a refusal reset resumes past the group of pictures it broke");
    {
        CHECK(psp_media_refusal_resume_has_room(
                  UINT64_C(167000000), UINT64_C(252261000)));
        CHECK(psp_media_refusal_resume_has_room(
                  UINT64_C(250000000), UINT64_C(250500000)));
        CHECK(!psp_media_refusal_resume_has_room(
                  UINT64_C(167000000), 0u));
        CHECK(!psp_media_refusal_resume_has_room(
                  UINT64_C(252261000), UINT64_C(252261000)));
        CHECK(!psp_media_refusal_resume_has_room(
                  UINT64_C(252261001), UINT64_C(252261000)));
    }

    puts("test: a no-touch reposition skips the pictures firmware still held");
    /* Mode 2 tells firmware nothing, so the engine's reorder pipeline is still
       holding pictures when the stream moves under it. Our queues are cleared,
       so a batch of three arriving against the one timestamp the first
       post-reposition unit pushed would be refused outright by the admission
       rule and end the session -- the run would measure that rule instead of
       the wedge. Inside the drain window the excess is read as the held
       pictures and skipped. */
    CHECK(psp_media_reposition_stale_pictures(16u, 3, 1u) == 2u
          && psp_media_reposition_stale_pictures(16u, 2, 1u) == 1u
          /* A batch that pairs needs no help, in or out of the window. */
          && psp_media_reposition_stale_pictures(16u, 1, 1u) == 0u
          && psp_media_reposition_stale_pictures(16u, 1, 3u) == 0u
          /* Outside the window the strict rule is back: an over-long batch is
             a decoder returning nonsense, not a reposition draining. */
          && psp_media_reposition_stale_pictures(0u, 3, 1u) == 0u
          /* A count firmware could not have meant is never a skip. */
          && psp_media_reposition_stale_pictures(
                 16u, (int) PSP_MEDIA_VIDEO_TIMESTAMP_SLOTS + 1, 1u) == 0u
          && psp_media_reposition_stale_pictures(16u, 0, 0u) == 0u
          && psp_media_reposition_stale_pictures(16u, -1, 1u) == 0u);
    /* And the skip always leaves at least one picture to pair, so the batch it
       hands on is one the admission rule accepts. */
    CHECK(psp_media_output_batch_admitted(
              3 - (int) psp_media_reposition_stale_pictures(16u, 3, 1u), 1u));

    puts("test: an unpairable picture is bounded like a refusal, not fatal");
    /* The device produced exactly this: a refused access unit whose picture
       firmware kept while the refusal path discarded its timestamp, surfacing
       two units later as a four-picture batch against three timestamps. That
       is fixed at the source, but one unpairable picture must never again end
       a run the Media Engine had not failed. */
    CHECK(psp_media_unpaired_pictures(4, 3u) == 1u
          && psp_media_unpaired_pictures(3, 3u) == 0u
          && psp_media_unpaired_pictures(1, 3u) == 0u
          && psp_media_unpaired_pictures(0, 0u) == 0u
          && psp_media_unpaired_pictures(-1, 3u) == 0u
          /* A count firmware could not have meant is not an overrun to
             recover from -- it is the nonsense the sanity rule catches. */
          && psp_media_unpaired_pictures(
                 (int) PSP_MEDIA_VIDEO_TIMESTAMP_SLOTS + 1, 3u) == 0u);
    /* Bounded by the same rule refusals use: isolated ones survive, a run or a
       rate does not. */
    CHECK(psp_media_avc_refusal_survivable(1u, 1u)
          && !psp_media_avc_refusal_survivable(
                 PSP_MEDIA_AVC_REFUSAL_CONSECUTIVE_LIMIT, 1u)
          && !psp_media_avc_refusal_survivable(
                 1u, PSP_MEDIA_AVC_REFUSAL_WINDOW_LIMIT));
    /* Skipping the unpaired head is what makes the batch admissible. */
    CHECK(psp_media_output_batch_admitted(
              4 - (int) psp_media_unpaired_pictures(4, 3u), 3u));

    puts("test: media cancellation stops before and after native submit");
    fixture_reader.reads = 0;
    fixture_reader.bytes_read = 0;
    demux = media_mp4_open(
        &budget, &reader, NULL, error, sizeof(error));
    CHECK(demux != NULL);
    backend_fixture = (BackendFixture) {0};
    playback = media_playback_create(
        &budget, demux, &backend, &playback_options,
        error, sizeof(error));
    CHECK(playback != NULL);
    TilefinchCancellation cancellation;
    tilefinch_cancellation_init(&cancellation);
    tilefinch_cancellation_request(&cancellation);
    CHECK(media_playback_advance_bounded_cancelable(
              playback, 0, 1, &cancellation, error, sizeof(error))
              == MEDIA_PLAYBACK_ADVANCE_CANCELLED
          && backend_fixture.submitted == 0
          && backend_fixture.advanced == 0);
    tilefinch_cancellation_init(&cancellation);
    fixture_reader.cancel_offset = payload_offset;
    fixture_reader.cancel_on_read = &cancellation;
    CHECK(media_playback_advance_bounded_cancelable(
              playback, 0, 1, &cancellation, error, sizeof(error))
              == MEDIA_PLAYBACK_ADVANCE_CANCELLED
          && backend_fixture.submitted == 0
          && backend_fixture.advanced == 0);
    fixture_reader.cancel_on_read = NULL;
    tilefinch_cancellation_init(&cancellation);
    backend_fixture.cancel_on_submit = &cancellation;
    CHECK(media_playback_advance_bounded_cancelable(
              playback, 0, 1, &cancellation, error, sizeof(error))
              == MEDIA_PLAYBACK_ADVANCE_CANCELLED
          && backend_fixture.submitted == 1
          && backend_fixture.advanced == 0);
    backend_fixture.cancel_on_submit = NULL;
    tilefinch_cancellation_init(&cancellation);
    CHECK(media_playback_advance_bounded_cancelable(
              playback, UINT64_C(2000000), 1, &cancellation,
              error, sizeof(error))
              == MEDIA_PLAYBACK_ADVANCE_PENDING
          && backend_fixture.submitted == 2
          && backend_fixture.advanced == 1);
    backend_fixture.drain_emit_once = true;
    backend_fixture.cancel_on_drain = &cancellation;
    CHECK(media_playback_advance_bounded_cancelable(
              playback, UINT64_C(2000000), 1, &cancellation,
              error, sizeof(error))
              == MEDIA_PLAYBACK_ADVANCE_CANCELLED
          && backend_fixture.drains == 1
          && !media_playback_ended(playback)
          && backend_fixture.drain_tail_pending
          && backend_fixture.advanced == 1);
    backend_fixture.cancel_on_drain = NULL;
    tilefinch_cancellation_init(&cancellation);
    CHECK(media_playback_advance_bounded_cancelable(
              playback, UINT64_C(2000000), 1, &cancellation,
              error, sizeof(error))
              == MEDIA_PLAYBACK_ADVANCE_PENDING
          && backend_fixture.drains == 2
          && !media_playback_ended(playback)
          && backend_fixture.advanced == 2);
    CHECK(media_playback_take_video_frame(playback, &drained_frame)
          && media_playback_advance_bounded_cancelable(
                 playback, UINT64_C(2000000), 1, &cancellation,
                 error, sizeof(error))
               == MEDIA_PLAYBACK_ADVANCE_COMPLETE
          && backend_fixture.drains == 3
          && media_playback_ended(playback)
          && backend_fixture.advanced == 3);
    media_playback_destroy(playback);
    CHECK(backend_fixture.destroyed == 1);
    media_mp4_close(demux);

    puts("test: cancellation during a lazy fragment read is retryable");
    FixtureReader lazy_cancel_reader = {
        .bytes = sidx_fixture.bytes,
        .length = sidx_fixture.length
    };
    MediaRangeReader lazy_cancel_source = {
        .opaque = &lazy_cancel_reader,
        .length = sidx_fixture.length,
        .read = fixture_read,
        .describe_failure = fixture_describe_failure
    };
    demux = media_mp4_open(
        &budget, &lazy_cancel_source, NULL, error, sizeof(error));
    backend_fixture = (BackendFixture) {0};
    playback = media_playback_create(
        &budget, demux, &backend, &playback_options,
        error, sizeof(error));
    tilefinch_cancellation_init(&cancellation);
    CHECK(playback != NULL
          && media_playback_advance_bounded_cancelable(
              playback, UINT64_C(4000000), 1, &cancellation,
              error, sizeof(error))
             == MEDIA_PLAYBACK_ADVANCE_PENDING
          && media_playback_advance_bounded_cancelable(
              playback, UINT64_C(4000000), 1, &cancellation,
              error, sizeof(error))
             == MEDIA_PLAYBACK_ADVANCE_PENDING
          && backend_fixture.submitted == 2);
    lazy_cancel_reader.cancel_offset = second_moof_offset;
    lazy_cancel_reader.cancel_on_read = &cancellation;
    CHECK(media_playback_advance_bounded_cancelable(
              playback, UINT64_C(4000000), 1, &cancellation,
              error, sizeof(error))
             == MEDIA_PLAYBACK_ADVANCE_CANCELLED
          && backend_fixture.submitted == 2);
    lazy_cancel_reader.cancel_on_read = NULL;
    tilefinch_cancellation_init(&cancellation);
    CHECK(media_playback_advance_bounded_cancelable(
              playback, UINT64_C(4000000), 1, &cancellation,
              error, sizeof(error))
             == MEDIA_PLAYBACK_ADVANCE_PENDING
          && backend_fixture.submitted == 3);
    media_playback_destroy(playback);
    media_mp4_close(demux);
    CHECK(budget.current == 0);

    puts("test: the Media Engine pool hands out visible, aligned storage");
    {
        /*
         * The Media Engine cannot address the PSP-2000/3000 extra RAM bank at
         * 0x0A000000 and above, so every buffer firmware DMAs from has to come
         * out of one reservation taken while the heap cursor is still low. The
         * policy under test is the bump allocator over that reservation: what
         * it guarantees about alignment, what it does when it runs out, and
         * what it refuses to reuse.
         */
        CHECK(psp_media_me_invisible(PSP_MEDIA_ME_VISIBLE_LIMIT)
              && psp_media_me_invisible(PSP_MEDIA_ME_VISIBLE_LIMIT + 64u)
              && !psp_media_me_invisible(PSP_MEDIA_ME_VISIBLE_LIMIT - 1u)
              /* A failed allocation is a null pointer, not a low address. */
              && !psp_media_me_invisible(0));
        const void *const mixed[] = {
            (const void *) (uintptr_t) 0x08810000u,
            (const void *) (uintptr_t) 0x0A400000u,
            NULL,
            (const void *) (uintptr_t) 0x0BFFFFC0u
        };
        CHECK(psp_media_me_invisible_count(
                  mixed, sizeof(mixed) / sizeof(mixed[0])) == 2u
              && psp_media_me_invisible_count(NULL, 4) == 0u);

        /* The reservation has to hold everything a 360p stream with audio
           asks for, with the DDR arena's 4 MiB alignment free at offset 0. */
        CHECK(PSP_MEDIA_POOL_BYTES
                  > (size_t) PSP_MEDIA_DDR_BYTES
                    + PSP_MEDIA_POOL_MAX_SURFACE_BYTES
                        * PSP_MEDIA_SURFACE_SLOTS
                    + (size_t) PSP_MEDIA_MAXIMUM_PACKET_BYTES
              && PSP_MEDIA_POOL_MAX_SURFACE_BYTES
                  >= (size_t) PSP_MEDIA_360P_FRAME_STRIDE
                     * PSP_MEDIA_360P_FRAME_ROWS * sizeof(uint32_t));

        /* Every slot is a full-size CSC target, and the reservation carries
           all of them. Pinned as an equality against the term rather than a
           bound, because the whole cost of double buffering is here and a
           silent change to it is the one thing this file exists to catch. */
        CHECK(PSP_MEDIA_SURFACE_SLOTS == 2u);
        /* The Budget charge and the carve move together or not at all: the
           charge must cover every slot the pool actually reserves. */
        CHECK(PSP_MEDIA_360P_EXTERNAL_RESERVE
                  >= PSP_MEDIA_POOL_MAX_SURFACE_BYTES
                     * PSP_MEDIA_SURFACE_SLOTS
              && PSP_MEDIA_240P_EXTERNAL_RESERVE
                  >= PSP_MEDIA_240P_SURFACE_BYTES
                     * PSP_MEDIA_SURFACE_SLOTS);
        CHECK(PSP_MEDIA_POOL_MAX_SURFACE_BYTES == 1130496u);
        /* 5,513,800 before the audio staging joined it: two compressed AAC
           access units at the 4,096-byte ceiling, so one worker job can carry
           a batch without the shared packet buffer -- which the next video
           submission overwrites -- being asked to hold it. */
        CHECK(PSP_MEDIA_AUDIO_STAGING_BYTES == 8192u);
        CHECK(PSP_MEDIA_AUDIO_PENDING_BYTES == 16384u);
        CHECK(PSP_MEDIA_POOL_BYTES == 5538392u);

        /*
         * The slot ownership machine's decisions, which are pure and so are
         * checked here rather than inferred from a soak.
         *
         * The epoch first. Zero is reserved for "never stamped", so the
         * advance must skip it on wrap -- and a device would take longer than
         * the hardware has existed to reach that wrap, which is exactly why
         * the width is a parameter and this forces it in four steps.
         */
        CHECK(!psp_media_epoch_valid(PSP_MEDIA_EPOCH_INVALID)
              && psp_media_epoch_valid(PSP_MEDIA_EPOCH_FIRST)
              && psp_media_epoch_advance(
                     PSP_MEDIA_EPOCH_INVALID, PSP_MEDIA_EPOCH_WIDTH_MASK)
                 == PSP_MEDIA_EPOCH_FIRST);
        {
            /* A three-bit epoch: 1,2,3,4,5,6,7 then back to 1, never 0. */
            uint64_t narrow = PSP_MEDIA_EPOCH_FIRST;
            bool skipped_reserved = false;
            for (unsigned step = 0; step < 32u; step++) {
                narrow = psp_media_epoch_advance(narrow, UINT64_C(7));
                CHECK(psp_media_epoch_valid(narrow));
                if (narrow == PSP_MEDIA_EPOCH_FIRST) skipped_reserved = true;
            }
            CHECK(skipped_reserved);
            /* And a width with no room to represent one fails closed rather
               than answering the reserved value as though it were live. */
            CHECK(psp_media_epoch_advance(PSP_MEDIA_EPOCH_FIRST, 0)
                      == PSP_MEDIA_EPOCH_INVALID
                  && !psp_media_epoch_current(
                         PSP_MEDIA_EPOCH_INVALID, PSP_MEDIA_EPOCH_INVALID)
                  && !psp_media_epoch_current(
                         PSP_MEDIA_EPOCH_FIRST, PSP_MEDIA_EPOCH_INVALID)
                  && !psp_media_epoch_current(
                         PSP_MEDIA_EPOCH_INVALID, PSP_MEDIA_EPOCH_FIRST)
                  && !psp_media_epoch_current(1u, 2u)
                  && psp_media_epoch_current(2u, 2u));
        }

        {
            /*
             * Slot selection. Earliest presentation time wins; a tie and an
             * unset timestamp both fall back to decode order, because zero is
             * the timestamp queue's absent value rather than a position in the
             * stream and sorting on it would put an unnamed picture first.
             */
            PspMediaSurfaceSlot slots[PSP_MEDIA_SURFACE_SLOTS] = {0};
            const uint64_t epoch = 7u;
            CHECK(psp_media_slot_free_count(
                      slots, PSP_MEDIA_SURFACE_SLOTS)
                      == PSP_MEDIA_SURFACE_SLOTS
                  && psp_media_slot_free_index(
                         slots, PSP_MEDIA_SURFACE_SLOTS) == 0
                  && psp_media_slot_ready_count(
                         slots, PSP_MEDIA_SURFACE_SLOTS) == 0
                  && psp_media_slot_take_index(
                         slots, PSP_MEDIA_SURFACE_SLOTS, epoch) == -1);
            /* Slot 1 holds the earlier picture, so it is claimed first even
               though slot 0 was converted first: firmware reorders. */
            slots[0] = (PspMediaSurfaceSlot) {
                .state = PSP_MEDIA_SLOT_READY, .epoch = epoch,
                .pts_us = 80000u, .sequence = 1u, .identity = 1u
            };
            slots[1] = (PspMediaSurfaceSlot) {
                .state = PSP_MEDIA_SLOT_READY, .epoch = epoch,
                .pts_us = 40000u, .sequence = 2u, .identity = 2u
            };
            CHECK(psp_media_slot_take_index(
                      slots, PSP_MEDIA_SURFACE_SLOTS, epoch) == 1
                  && psp_media_slot_free_index(
                         slots, PSP_MEDIA_SURFACE_SLOTS) == -1
                  && psp_media_slot_free_count(
                         slots, PSP_MEDIA_SURFACE_SLOTS) == 0
                  && psp_media_slot_ready_count(
                         slots, PSP_MEDIA_SURFACE_SLOTS) == 2);
            /* A/V catch-up may drop only a genuinely late head with another
               due picture already present. It never drops for a merely early
               successor or an out-of-order pair. The caller separately proves
               that both entries are READY, so a lone picture has no successor
               to pass here. */
            CHECK(psp_media_video_should_drop_late(
                      200000u, &slots[1], &slots[0], false)
                  && !psp_media_video_should_drop_late(
                      100000u, &slots[1], &slots[0], false));
            CHECK(!psp_media_video_should_drop_late(
                      200000u, &slots[1], &slots[0], true)
                  && psp_media_video_should_drop_late(
                      600000u, &slots[1], &slots[0], true));
            CHECK(psp_media_startup_catchup_settled(
                      PSP_MEDIA_VIDEO_STARTUP_CATCHUP_FRAMES)
                  && !psp_media_startup_catchup_settled(
                      PSP_MEDIA_VIDEO_STARTUP_CATCHUP_FRAMES - 1u));
            slots[0].pts_us = 400000u;
            slots[0].duration_us = 40000u;
            CHECK(!psp_media_video_should_drop_late(
                      200000u, &slots[1], &slots[0],
                      false));
            slots[0].pts_us = 80000u;
            slots[0].duration_us = 0;
            /* Equal timestamps: decode order decides. */
            slots[1].pts_us = 80000u;
            CHECK(psp_media_slot_take_index(
                      slots, PSP_MEDIA_SURFACE_SLOTS, epoch) == 0);
            /* An unset timestamp does not sort ahead of a named one. */
            slots[1].pts_us = 0;
            slots[1].sequence = 3u;
            CHECK(psp_media_slot_take_index(
                      slots, PSP_MEDIA_SURFACE_SLOTS, epoch) == 0);
            slots[0].pts_us = 0;
            slots[0].sequence = 4u;
            CHECK(psp_media_slot_take_index(
                      slots, PSP_MEDIA_SURFACE_SLOTS, epoch) == 1);
            /* A picture of a stream the session has left is not claimable,
               and it is not free either -- the bytes are still the old
               writer's until a reset says otherwise. */
            slots[0].epoch = epoch + 1u;
            slots[1].epoch = epoch + 1u;
            CHECK(psp_media_slot_take_index(
                      slots, PSP_MEDIA_SURFACE_SLOTS, epoch) == -1
                  && psp_media_slot_free_count(
                         slots, PSP_MEDIA_SURFACE_SLOTS) == 0
                  && psp_media_slot_ready_count(
                         slots, PSP_MEDIA_SURFACE_SLOTS) == 2);
            /* Neither a slot being written nor one being read is claimable or
               free. Claimed and writable are disjoint by construction. */
            slots[0] = (PspMediaSurfaceSlot) {
                .state = PSP_MEDIA_SLOT_ME_WRITING, .epoch = epoch};
            slots[1] = (PspMediaSurfaceSlot) {
                .state = PSP_MEDIA_SLOT_READING, .epoch = epoch,
                .pts_us = 40000u, .identity = 3u};
            CHECK(psp_media_slot_take_index(
                      slots, PSP_MEDIA_SURFACE_SLOTS, epoch) == -1
                  && psp_media_slot_free_count(
                         slots, PSP_MEDIA_SURFACE_SLOTS) == 0);
            /* The claim that supersedes the reader frees exactly one. */
            slots[1].state = PSP_MEDIA_SLOT_FREE;
            CHECK(psp_media_slot_free_index(
                      slots, PSP_MEDIA_SURFACE_SLOTS) == 1
                  && psp_media_slot_free_count(
                         slots, PSP_MEDIA_SURFACE_SLOTS) == 1);
        }

        {
            /*
             * Slot dwell, which is how the occupancy line is assembled.
             *
             * The charge names the state being LEFT, so a caller that runs it
             * after the transition would attribute every interval to the
             * wrong state -- and on device, where the intervals are tens of
             * milliseconds and the states alternate, the result would look
             * entirely plausible. Pin the direction here, where it is
             * checkable, rather than trusting the call sites to read right.
             */
            PspMediaSlotDwell dwell = {.since_us = 1000u};
            psp_media_slot_dwell_charge(
                &dwell, PSP_MEDIA_SLOT_FREE, 1500u);
            psp_media_slot_dwell_charge(
                &dwell, PSP_MEDIA_SLOT_ME_WRITING, 3500u);
            psp_media_slot_dwell_charge(
                &dwell, PSP_MEDIA_SLOT_READY, 4000u);
            psp_media_slot_dwell_charge(
                &dwell, PSP_MEDIA_SLOT_READING, 4100u);
            CHECK(dwell.state_us[PSP_MEDIA_SLOT_FREE] == 500u
                  && dwell.state_us[PSP_MEDIA_SLOT_ME_WRITING] == 2000u
                  && dwell.state_us[PSP_MEDIA_SLOT_READY] == 500u
                  && dwell.state_us[PSP_MEDIA_SLOT_READING] == 100u
                  && dwell.since_us == 4100u);
            /* A state outside the four charges nothing and still re-arms, so
               one bad reading cannot make the next interval enormous. */
            psp_media_slot_dwell_charge(&dwell, 9, 5100u);
            CHECK(dwell.state_us[PSP_MEDIA_SLOT_READING] == 100u
                  && dwell.since_us == 5100u);
            /*
             * And the wrap. sceKernelGetSystemTimeLow is 32 bits of
             * microseconds, so it returns to zero every 71 minutes -- inside
             * an ordinary soak. The subtraction is modular, so an interval
             * that straddles the wrap is still the interval; a signed or
             * widened one would charge 71 minutes to a slot.
             */
            PspMediaSlotDwell wrapped = {.since_us = UINT32_MAX - 100u};
            psp_media_slot_dwell_charge(
                &wrapped, PSP_MEDIA_SLOT_READY, 150u);
            CHECK(wrapped.state_us[PSP_MEDIA_SLOT_READY] == 251u);
        }

        {
            /*
             * Bounded AAC batching. Four conditions decide whether a second
             * access unit may join a job, and three of them are refusals that
             * matter more than the optimisation does -- so they are checked
             * here rather than inferred from a soak that would only show the
             * aggregate effect.
             */
            /* The ceiling is two, and it is a ceiling: 2 x 1024 samples is
               about one video frame of media, and a job that holds the one
               shared slot longer than that refuses video for more than a
               frame -- the cost this exists to remove. */
            CHECK(PSP_MEDIA_AUDIO_BATCH_MAXIMUM == 2u);
            CHECK(PSP_MEDIA_AUDIO_PENDING_SLOTS == 4u);
            CHECK(PSP_MEDIA_AUDIO_PENDING_HOLD_US == 30000u);
            CHECK(PSP_MEDIA_AUDIO_PENDING_LOW_WATER == 4u);
            CHECK(PSP_MEDIA_AUDIO_PENDING_BYTES
                  == PSP_MEDIA_AUDIO_AU_BYTES
                     * PSP_MEDIA_AUDIO_PENDING_SLOTS);
            CHECK(psp_media_audio_pending_should_wait(
                      1u, PSP_MEDIA_AUDIO_PENDING_LOW_WATER, true, 0u));
            CHECK(!psp_media_audio_pending_should_wait(
                      0u, PSP_MEDIA_AUDIO_PENDING_LOW_WATER, true, 0u)
                  && !psp_media_audio_pending_should_wait(
                      2u, PSP_MEDIA_AUDIO_PENDING_LOW_WATER, true, 0u)
                  && !psp_media_audio_pending_should_wait(
                      1u, PSP_MEDIA_AUDIO_PENDING_LOW_WATER - 1u, true, 0u)
                  && !psp_media_audio_pending_should_wait(
                      1u, PSP_MEDIA_AUDIO_PENDING_LOW_WATER, false, 0u)
                  && !psp_media_audio_pending_should_wait(
                      1u, PSP_MEDIA_AUDIO_PENDING_LOW_WATER, true,
                      PSP_MEDIA_AUDIO_PENDING_HOLD_US));
            /* And the staging holds exactly what the batch may carry. */
            CHECK(PSP_MEDIA_AUDIO_STAGING_BYTES
                  == PSP_MEDIA_AUDIO_AU_BYTES * PSP_MEDIA_AUDIO_BATCH_MAXIMUM);
            /* The per-unit ceiling is above anything AAC-LC can emit: the
               format's own maximum is 768 bytes per channel, 1,536 for the
               admitted stereo configuration. */
            CHECK(PSP_MEDIA_AUDIO_AU_BYTES >= 1536u * 2u);
        }

        {
            /*
             * The audio job-duration histogram. Percentiles are reported as
             * bucket ceilings -- an upper bound, never a sample -- because
             * this target cannot keep several thousand durations to sort.
             */
            CHECK(psp_media_job_bucket(0u) == 0
                  && psp_media_job_bucket(500u) == 0
                  && psp_media_job_bucket(501u) == 1
                  && psp_media_job_bucket(64000u)
                         == PSP_MEDIA_JOB_HISTOGRAM_BUCKETS - 2u
                  && psp_media_job_bucket(1000000u)
                         == PSP_MEDIA_JOB_HISTOGRAM_BUCKETS - 1u);
            uint32_t buckets[PSP_MEDIA_JOB_HISTOGRAM_BUCKETS] = {0};
            /* Nothing recorded is zero, not a bucket ceiling: an empty
               histogram must not report a 500us median. */
            CHECK(psp_media_job_percentile_us(
                      buckets, PSP_MEDIA_JOB_HISTOGRAM_BUCKETS, 500u) == 0);
            /* One sample: its own bucket at every percentile, which is what
               the rounding-up exists for. */
            buckets[psp_media_job_bucket(1500u)] = 1u;
            CHECK(psp_media_job_percentile_us(
                      buckets, PSP_MEDIA_JOB_HISTOGRAM_BUCKETS, 500u) == 2000u
                  && psp_media_job_percentile_us(
                         buckets, PSP_MEDIA_JOB_HISTOGRAM_BUCKETS, 900u)
                     == 2000u);
            /* Ninety short jobs and ten long ones: the shape an audio codec
               sharing one job slot actually produces, and the case a mean
               cannot describe. The median is short, the ninetieth is not. */
            memset(buckets, 0, sizeof(buckets));
            buckets[psp_media_job_bucket(900u)] = 90u;
            buckets[psp_media_job_bucket(30000u)] = 10u;
            CHECK(psp_media_job_percentile_us(
                      buckets, PSP_MEDIA_JOB_HISTOGRAM_BUCKETS, 500u) == 1000u
                  && psp_media_job_percentile_us(
                         buckets, PSP_MEDIA_JOB_HISTOGRAM_BUCKETS, 900u)
                     == 1000u
                  && psp_media_job_percentile_us(
                         buckets, PSP_MEDIA_JOB_HISTOGRAM_BUCKETS, 1000u)
                     == 32000u);
            CHECK(psp_media_job_percentile_us(
                      NULL, PSP_MEDIA_JOB_HISTOGRAM_BUCKETS, 500u) == 0
                  && psp_media_job_percentile_us(buckets, 0, 500u) == 0);
        }

        {
            /*
             * The conversion's phase tag. The present window is derived from
             * the advance mode rather than published a second time, so the
             * two cannot disagree -- and an out-of-range phase folds to OTHER
             * rather than indexing past the array it counts into.
             */
            CHECK(psp_media_loop_phase_resolve(
                      PSP_MEDIA_LOOP_PHASE_PUMP, PSP_MEDIA_ADVANCE_FRAME)
                      == PSP_MEDIA_LOOP_PHASE_PUMP
                  && psp_media_loop_phase_resolve(
                         PSP_MEDIA_LOOP_PHASE_VBLANK, PSP_MEDIA_ADVANCE_FRAME)
                     == PSP_MEDIA_LOOP_PHASE_VBLANK
                  /* A pump that runs inside a present is a present. */
                  && psp_media_loop_phase_resolve(
                         PSP_MEDIA_LOOP_PHASE_PUMP, PSP_MEDIA_ADVANCE_DRAW)
                     == PSP_MEDIA_LOOP_PHASE_PRESENT
                  && psp_media_loop_phase_resolve(
                         PSP_MEDIA_LOOP_PHASE_VBLANK,
                         PSP_MEDIA_ADVANCE_STAGE_COPY)
                     == PSP_MEDIA_LOOP_PHASE_PRESENT
                  && psp_media_loop_phase_resolve(
                         99, PSP_MEDIA_ADVANCE_FRAME)
                     == PSP_MEDIA_LOOP_PHASE_OTHER
                  && psp_media_loop_phase_resolve(
                         -1, PSP_MEDIA_ADVANCE_FRAME)
                     == PSP_MEDIA_LOOP_PHASE_OTHER);
        }

        {
            /*
             * The pixel signature. Right metadata over the wrong pixels is the
             * failure two slots make possible, and this is the only check that
             * looks at the picture rather than at the bookkeeping -- so it has
             * to actually differ when one pixel does.
             */
            enum { SIGNATURE_STRIDE = 64, SIGNATURE_ROWS = 48 };
            static uint32_t first[SIGNATURE_STRIDE * SIGNATURE_ROWS];
            static uint32_t second[SIGNATURE_STRIDE * SIGNATURE_ROWS];
            for (unsigned at = 0;
                 at < SIGNATURE_STRIDE * SIGNATURE_ROWS; at++) {
                first[at] = 0x11223344u + at;
                second[at] = first[at];
            }
            uint32_t signature = psp_media_surface_signature(
                first, 60u, 40u, SIGNATURE_STRIDE);
            CHECK(signature != 0
                  && signature == psp_media_surface_signature(
                         second, 60u, 40u, SIGNATURE_STRIDE)
                  /* Absent rather than wrong when it cannot sample. */
                  && psp_media_surface_signature(
                         NULL, 60u, 40u, SIGNATURE_STRIDE) == 0
                  && psp_media_surface_signature(
                         first, 0, 40u, SIGNATURE_STRIDE) == 0
                  && psp_media_surface_signature(
                         first, 60u, 40u, 32u) == 0);
            /* Every sample point is inside the picture, and changing any one
               of them changes the answer -- which is what makes a wrong
               surface detectable rather than merely improbable. */
            for (unsigned index = 0;
                 index < PSP_MEDIA_SIGNATURE_SAMPLES; index++) {
                unsigned x = 0;
                unsigned y = 0;
                psp_media_signature_point(index, 60u, 40u, &x, &y);
                CHECK(x < 60u && y < 40u);
                size_t at = (size_t) y * SIGNATURE_STRIDE + x;
                second[at] = ~first[at];
                CHECK(psp_media_surface_signature(
                          second, 60u, 40u, SIGNATURE_STRIDE) != signature);
                second[at] = first[at];
            }
            /* Two different pictures at the same geometry differ. */
            for (unsigned at = 0;
                 at < SIGNATURE_STRIDE * SIGNATURE_ROWS; at++)
                second[at] = 0x55667788u + at;
            CHECK(psp_media_surface_signature(
                      second, 60u, 40u, SIGNATURE_STRIDE) != signature);
        }

        /*
         * The AAC work buffer PMPlayer allocates instead of calling
         * sceAudiocodecGetEDRAM comes out of the same reservation, and its
         * size is a runtime firmware answer. It is reserved explicitly rather
         * than taken from the slack, so an unexpectedly large answer still
         * comes from Media Engine visible storage instead of the heap, and
         * the slack stays available for alignment padding as documented.
         */
        CHECK(PSP_MEDIA_POOL_AUDIO_EDRAM_BYTES >= 16u * 1024u
              && PSP_MEDIA_POOL_BYTES
                  > (size_t) PSP_MEDIA_DDR_BYTES
                    + PSP_MEDIA_POOL_MAX_SURFACE_BYTES
                    + (size_t) PSP_MEDIA_MAXIMUM_PACKET_BYTES
                    + PSP_MEDIA_POOL_MPEG_WORKSPACE_BYTES
                    + PSP_MEDIA_POOL_AUDIO_EDRAM_BYTES
                    + (size_t) PSP_MEDIA_AUDIO_QUEUE_BYTES
                    + PSP_MEDIA_POOL_SLACK_BYTES);

        /*
         * The reservation is only taken where it is both affordable and
         * necessary, and the quantity that decides it is newlib heap
         * capacity. It is emphatically NOT sceKernelTotalFreeMemSize: PSPSDK
         * gives newlib the whole partition bar the 2 MiB late-module
         * threshold before main() runs, so every device -- extra-memory
         * unlock or not -- reports about 2.2 MB free there and would refuse
         * the reservation forever. A 22 MB heap, which is what a stock
         * partition leaves, lies entirely below the Media Engine's limit and
         * keeps the pre-pool behaviour; the unlocked 40 MB heap is the one
         * that needs the pool and can pay for it.
         */
        CHECK(!psp_media_pool_reservation_admitted(0)
              /* The device's partition reading. */
              && !psp_media_pool_reservation_admitted(2326528u)
              /* A stock-partition heap. */
              && !psp_media_pool_reservation_admitted(
                     22u * 1024u * 1024u)
              && !psp_media_pool_reservation_admitted(
                     24u * 1024u * 1024u)
              && !psp_media_pool_reservation_admitted(
                     PSP_MEDIA_POOL_CONTENT_HEADROOM_BYTES
                     + PSP_MEDIA_POOL_BYTES - 1u)
              && psp_media_pool_reservation_admitted(
                     PSP_MEDIA_POOL_CONTENT_HEADROOM_BYTES
                     + PSP_MEDIA_POOL_BYTES)
              && psp_media_pool_reservation_admitted(40u * 1024u * 1024u));

        /*
         * The probe that samples that capacity is bounded, and its bound is
         * the verdict itself: enough 1 MiB blocks to satisfy the predicate
         * and not one more, so boot never walks the whole heap and a "yes"
         * is never given for memory the probe did not actually hold.
         */
        CHECK(PSP_MEDIA_POOL_PROBE_BLOCK_BYTES == 1024u * 1024u
              && psp_media_pool_reservation_admitted(
                     PSP_MEDIA_POOL_PROBE_BLOCKS
                     * PSP_MEDIA_POOL_PROBE_BLOCK_BYTES)
              && !psp_media_pool_reservation_admitted(
                     (PSP_MEDIA_POOL_PROBE_BLOCKS - 1u)
                     * PSP_MEDIA_POOL_PROBE_BLOCK_BYTES));

        enum { POOL_TEST_BYTES = 1u << 16 };
        static unsigned char storage[POOL_TEST_BYTES + 4096u];
        /* Stand in for the boot memalign: the production reservation is made
           on a PSP_MEDIA_DDR_ALIGNMENT boundary so the arena costs nothing. */
        unsigned char *base = storage
            + ((4096u - ((uintptr_t) storage & 4095u)) & 4095u);
        PspMediaPool pool;
        psp_media_pool_init(&pool, base, POOL_TEST_BYTES);
        CHECK(psp_media_pool_available(&pool)
              && psp_media_pool_remaining(&pool) == POOL_TEST_BYTES
              && pool.cursor == 0);

        /* The strictest alignment comes out at offset 0, so it costs nothing
           when the reservation itself is aligned. */
        void *arena = psp_media_pool_alloc(&pool, 2048u, 4096u, NULL, NULL);
        CHECK(arena == (void *) base && pool.cursor == 2048u);

        /* Every other block is 64-byte aligned in both address and length,
           which is what a pure dcache invalidate requires on real firmware. */
        size_t needed = 0;
        size_t remaining = 0;
        void *odd = psp_media_pool_alloc(&pool, 100u, 0, &needed, &remaining);
        CHECK(odd != NULL
              && ((uintptr_t) odd % PSP_MEDIA_POOL_ALIGNMENT) == 0
              && needed == 128u
              && pool.cursor == 2048u + 128u
              && remaining == POOL_TEST_BYTES - pool.cursor
              && psp_media_pool_owns(&pool, odd)
              && psp_media_pool_owns(&pool, (unsigned char *) odd + 99)
              && !psp_media_pool_owns(&pool, base + POOL_TEST_BYTES)
              && !psp_media_pool_owns(&pool, NULL));

        /* Realigning to a boundary the cursor has already passed reports the
           padding it would have burned, not just the payload. */
        size_t before_padded = pool.cursor;
        void *padded = psp_media_pool_alloc(&pool, 64u, 4096u, NULL, NULL);
        CHECK(padded != NULL
              && ((uintptr_t) padded % 4096u) == 0
              && (uintptr_t) padded > (uintptr_t) base + before_padded);

        /* Exhaustion is a normal outcome: the caller falls back to the heap,
           and the request that did not fit is counted and described. */
        size_t cursor_before = pool.cursor;
        unsigned exhaustions_before = pool.exhaustions;
        void *too_large = psp_media_pool_alloc(
            &pool, POOL_TEST_BYTES, 64u, &needed, &remaining);
        CHECK(too_large == NULL
              && pool.cursor == cursor_before
              && pool.exhaustions == exhaustions_before + 1u
              && needed >= POOL_TEST_BYTES
              && remaining == POOL_TEST_BYTES - cursor_before);
        /* A refusal must not close the pool to a request that still fits. */
        CHECK(psp_media_pool_alloc(&pool, 64u, 64u, NULL, NULL) != NULL);

        /* Media buffers are released together, so destroy rewinds the whole
           pool and the next stream reuses the same low storage. */
        size_t high_water = pool.high_water;
        CHECK(high_water == pool.cursor
              && psp_media_pool_reset(&pool)
              && pool.cursor == 0
              && pool.high_water == high_water
              && psp_media_pool_alloc(&pool, 2048u, 4096u, NULL, NULL)
                     == (void *) base);

        /* A quarantined teardown deliberately leaks every firmware-visible
           buffer, so its storage may never be handed out again -- but the
           reservation stays owned rather than being freed underneath a
           firmware call that may still be reading it. */
        psp_media_pool_poison(&pool);
        CHECK(!psp_media_pool_available(&pool)
              && !psp_media_pool_reset(&pool)
              && psp_media_pool_remaining(&pool) == 0
              && psp_media_pool_alloc(&pool, 64u, 64u, NULL, NULL) == NULL
              /* Still owns what it handed out, so nothing double-frees. */
              && psp_media_pool_owns(&pool, base));

        /* No reservation at all is the boot-allocation-failure case: every
           request declines so the backend falls back to the ordinary heap. */
        PspMediaPool absent;
        psp_media_pool_init(&absent, NULL, POOL_TEST_BYTES);
        CHECK(!psp_media_pool_available(&absent)
              && absent.bytes == 0
              && psp_media_pool_alloc(&absent, 64u, 64u, NULL, NULL) == NULL
              && !psp_media_pool_reset(&absent)
              && !psp_media_pool_owns(&absent, base));
    }
    puts("media-mp4-tests: all checks passed");
    return 0;
}
