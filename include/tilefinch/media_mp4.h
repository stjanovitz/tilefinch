#ifndef TILEFINCH_MEDIA_MP4_H
#define TILEFINCH_MEDIA_MP4_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/budget.h"

#define MEDIA_MP4_FOURCC(a, b, c, d) \
    (((uint32_t) (a) << 24) | ((uint32_t) (b) << 16) \
     | ((uint32_t) (c) << 8) | (uint32_t) (d))

/* Web media uses much smaller clock rates (commonly 90 kHz video and
   sample-rate audio). Bounding hostile metadata here keeps all downstream
   cross-track products within uint64_t without constraining Tilefinch's
   supported web-media profiles. */
#define MEDIA_MP4_MAX_TIMESCALE UINT32_C(10000000)

typedef bool (*MediaRangeRead)(void *opaque, uint64_t offset,
                               void *destination, size_t length);
typedef bool (*MediaRangeDescribeFailure)(void *opaque, char *error,
                                          size_t error_size);

/*
 * The outcome of a read that is not allowed to wait. WOULD_BLOCK is not a
 * failure: the bytes are not buffered yet, the source has been told to fetch
 * them, and the caller must come back later. Only FAILED carries an error.
 */
typedef enum {
    MEDIA_RANGE_READ_COMPLETE = 0,
    MEDIA_RANGE_READ_WOULD_BLOCK,
    MEDIA_RANGE_READ_FAILED
} MediaRangeReadStatus;

typedef MediaRangeReadStatus (*MediaRangeReadPoll)(
    void *opaque, uint64_t offset, void *destination, size_t length);

/*
 * Whether [offset, offset + length) could be read right now without blocking,
 * asked without reading it.
 *
 * Purely an observation: it copies nothing, allocates nothing, and must never
 * start a fetch. It exists so the pump can count what it did NOT try -- a
 * source whose head sample blocked while the other source held one that was
 * due and already buffered -- without a speculative read whose side effects
 * would change the scheduling it is measuring.
 */
typedef bool (*MediaRangeResident)(
    void *opaque, uint64_t offset, size_t length);

/* floor(ticks * 1000000 / timescale), saturating at UINT64_MAX; a zero
   timescale also yields UINT64_MAX, so callers wanting another sentinel test
   it first. Values below 2^32 at ordinary clock rates take a 32-bit divide
   path (Allegrex has no 64-bit divider, and __udivdi3 runs per sample); it
   rounds identically. */
static inline uint64_t media_ticks_to_us(uint64_t ticks, uint32_t timescale)
{
    if (timescale == 0) return UINT64_MAX;
    uint32_t scale_remainder = UINT32_C(1000000) % timescale;
    if (ticks <= UINT32_MAX
        && (uint64_t) (timescale - 1u) * scale_remainder <= UINT32_MAX) {
        uint32_t small = (uint32_t) ticks;
        uint32_t whole = small / timescale;
        uint32_t remainder = small % timescale;
        uint32_t scale_whole = UINT32_C(1000000) / timescale;
        return (uint64_t) whole * UINT32_C(1000000)
             + (uint64_t) remainder * scale_whole
             + (remainder * scale_remainder) / timescale;
    }
    uint64_t whole = ticks / timescale;
    uint64_t remainder = ticks % timescale;
    if (whole > UINT64_MAX / UINT64_C(1000000)) return UINT64_MAX;
    uint64_t base = whole * UINT64_C(1000000);
    uint64_t fraction = remainder * UINT64_C(1000000) / timescale;
    return fraction > UINT64_MAX - base ? UINT64_MAX : base + fraction;
}

typedef struct {
    void *opaque;
    uint64_t length;
    /*
     * The blocking form, used while a stream is being opened: metadata is
     * parsed once, inside a job phase that owns its own deadline.
     */
    MediaRangeRead read;
    /*
     * The non-blocking form, used for everything a playing stream reads.
     * Optional: a source without one (an offline file, a fixture) is answered
     * through `read`, which for those sources never touches a network.
     */
    MediaRangeReadPoll poll;
    /*
     * Optional, and NULL means "reads from this source never block" -- which
     * is the truth for every source that has no `poll` either. A source that
     * can block and does not answer this is reported as resident, so the
     * counters built on it are an upper bound rather than a wrong answer.
     */
    MediaRangeResident resident;
    MediaRangeDescribeFailure describe_failure;
} MediaRangeReader;

typedef struct {
    size_t maximum_metadata_bytes;
    size_t maximum_track_table_bytes;
    uint32_t maximum_tracks;
    uint32_t maximum_samples_per_track;
    uint32_t maximum_chunks_per_track;
    uint32_t maximum_table_entries;
    uint32_t maximum_sample_bytes;
} MediaMp4Limits;

typedef enum {
    MEDIA_MP4_TRACK_OTHER = 0,
    MEDIA_MP4_TRACK_VIDEO,
    MEDIA_MP4_TRACK_AUDIO
} MediaMp4TrackKind;

typedef struct {
    MediaMp4TrackKind kind;
    uint32_t codec;
    uint32_t timescale;
    uint64_t duration;
    uint32_t sample_count;
    uint32_t largest_sample;
    uint16_t width;
    uint16_t height;
    uint16_t channels;
    uint32_t sample_rate;
    const unsigned char *codec_config;
    size_t codec_config_length;
    uint8_t nal_length_size;
    /* Container representation delivered by read_sample. Zero preserves the
       MP4 contract; streaming containers may deliver decoder-ready units. */
    uint8_t packet_format;
} MediaMp4TrackInfo;

typedef enum {
    MEDIA_PACKET_FORMAT_CONTAINER_NATIVE = 0,
    MEDIA_PACKET_FORMAT_H264_ANNEX_B,
    MEDIA_PACKET_FORMAT_AAC_ADTS
} MediaPacketFormat;

typedef struct {
    size_t track_index;
    MediaMp4TrackKind kind;
    /* File offset of the sample's bytes. Sources without one (HLS) store
       an opaque identity here that only their own read path interprets. */
    uint64_t offset;
    uint32_t size;
    uint64_t dts;
    int64_t pts;
    uint32_t duration;
    uint32_t timescale;
    bool keyframe;
    uint8_t packet_format;
} MediaMp4Sample;

typedef struct MediaMp4Demux MediaMp4Demux;

MediaMp4Limits media_mp4_default_limits(void);
MediaMp4Demux *media_mp4_open(
    Budget *budget, const MediaRangeReader *reader,
    const MediaMp4Limits *limits, char *error, size_t error_size);
size_t media_mp4_track_count(const MediaMp4Demux *demux);
bool media_mp4_track_info(const MediaMp4Demux *demux, size_t index,
                          MediaMp4TrackInfo *info);
/* Parse the coded, crop-adjusted dimensions which govern an AVC decoder's
   output writes. These helpers are allocation-free and reject malformed or
   pathologically large parameter sets. avcC requires every advertised SPS
   to agree on geometry. */
bool media_h264_sps_dimensions(
    const unsigned char *sps, size_t length,
    uint16_t *width, uint16_t *height);
bool media_h264_avcc_dimensions(
    const unsigned char *config, size_t length,
    uint16_t *width, uint16_t *height, uint8_t *nal_length_size);
typedef enum {
    MEDIA_H264_DECODER_ROUTE_UNSUPPORTED = 0,
    MEDIA_H264_DECODER_ROUTE_PSP_FIRMWARE,
    /* Extension seam for an Annex-B High-profile decoder. The current
       firmware backend reports this route distinctly instead of pretending
       the Baseline/Main program can accept it. */
    MEDIA_H264_DECODER_ROUTE_HIGH_EXTENSION
} MediaH264DecoderRoute;
MediaH264DecoderRoute media_h264_avcc_decoder_route(
    const unsigned char *config, size_t length, uint8_t *profile_idc);
MediaH264DecoderRoute media_h264_annexb_decoder_route(
    const unsigned char *config, size_t length, uint8_t *profile_idc);
MediaH264DecoderRoute media_h264_codec_string_decoder_route(
    const char *mime, uint8_t *profile_idc);
/*
 * The reference structure an SPS declares. bitstream_restriction says the
 * VUI carried max_num_reorder_frames and max_dec_frame_buffering; without it
 * both read as zero and only max_num_ref_frames is known.
 */
typedef struct {
    uint8_t max_num_ref_frames;
    bool bitstream_restriction;
    uint8_t max_num_reorder_frames;
    uint8_t max_dec_frame_buffering;
} MediaH264SpsReferences;
bool media_h264_sps_references(
    const unsigned char *sps, size_t length,
    MediaH264SpsReferences *references);
/*
 * The most decoded pictures any SPS asks a decoder to hold:
 * max(max_num_ref_frames, max_dec_frame_buffering when the VUI declares
 * it), over every SPS found. KNOWN when at least one SPS was read and none
 * was unreadable; UNREADABLE when some SPS used syntax this parser cannot
 * follow (`*frames` then covers only the readable ones); ABSENT without an
 * SPS. The config form reads an avcC record or, with `annex_b`, Annex-B
 * parameter sets; the sample form reads one access unit, Annex-B when
 * `nal_length_size` is 0, otherwise length-prefixed NAL units.
 */
/* The entropy coding the stream's picture parameter sets declare
   (entropy_coding_mode_flag): the optional software decoder reads CABAC
   only, so a CAVLC stream (every Baseline one) cannot go to it. */
typedef enum {
    MEDIA_H264_ENTROPY_UNKNOWN = 0,
    MEDIA_H264_ENTROPY_CABAC,
    MEDIA_H264_ENTROPY_CAVLC
} MediaH264Entropy;
MediaH264Entropy media_h264_config_entropy(
    const unsigned char *config, size_t length, bool annex_b);
/* Whether the optional software decoder can take the stream's coding:
   progressive (frame_mbs_only), 4:2:0, 8-bit. CABAC and CAVLC both are.
   UNKNOWN when the SPS cannot be read (the decoder then judges it). */
typedef enum {
    MEDIA_H264_SOFTWARE_CODING_UNKNOWN = 0,
    MEDIA_H264_SOFTWARE_CODING_SUPPORTED,
    MEDIA_H264_SOFTWARE_CODING_UNSUPPORTED
} MediaH264SoftwareCoding;
MediaH264SoftwareCoding media_h264_config_software_coding(
    const unsigned char *config, size_t length, bool annex_b);
/* True when the stream's SPS rules out picture reordering: Baseline, or a
   VUI declaring max_num_reorder_frames 0. Output order is then decode
   order. False when unknown. */
bool media_h264_config_reorder_free(
    const unsigned char *config, size_t length, bool annex_b);
typedef enum {
    MEDIA_H264_SPS_FRAMES_ABSENT = 0,
    MEDIA_H264_SPS_FRAMES_KNOWN,
    MEDIA_H264_SPS_FRAMES_UNREADABLE
} MediaH264SpsFramesStatus;
MediaH264SpsFramesStatus media_h264_config_sps_frames(
    const unsigned char *config, size_t length, bool annex_b,
    unsigned *frames);
MediaH264SpsFramesStatus media_h264_sample_sps_frames(
    const unsigned char *payload, size_t length,
    unsigned nal_length_size, unsigned *frames);
/*
 * Reference structures the PSP firmware AVC decoder refuses (PSP-3000,
 * sceMpegAvcDecode 0x80628002, 640x360 Main 3.0 on the wide program):
 *
 * - more than three reference frames: the first access unit is refused;
 * - a B-frame pyramid -- B pictures kept as references, which an encoder
 *   declares as max_num_reorder_frames > 1 -- decodes its opening pictures
 *   and then has every later reference picture refused.
 *
 * Two non-reference B-frames with three references (reorder 1) play. An SPS
 * whose VUI omits bitstream_restriction cannot show a pyramid; firmware stays
 * the judge of such a stream, as it is of syntax this parser cannot follow.
 */
#define MEDIA_H264_PSP_FIRMWARE_MAXIMUM_REFERENCES 3u
#define MEDIA_H264_PSP_FIRMWARE_MAXIMUM_REORDER 1u
typedef enum {
    MEDIA_H264_FIRMWARE_REFERENCES_SUPPORTED = 0,
    MEDIA_H264_FIRMWARE_REFERENCES_PYRAMID,
    MEDIA_H264_FIRMWARE_REFERENCES_TOO_MANY
} MediaH264FirmwareReferences;
MediaH264FirmwareReferences media_h264_psp_firmware_references(
    const MediaH264SpsReferences *references);
MediaH264FirmwareReferences media_h264_avcc_firmware_references(
    const unsigned char *config, size_t length);
MediaH264FirmwareReferences media_h264_annexb_firmware_references(
    const unsigned char *config, size_t length);
/*
 * Whether `verdict` may refuse a stream before firmware has seen it, rather
 * than only route it to the software decoder.
 *
 * More than three references is refused on the first access unit (and that
 * refusal latches the wide program off), so it is always conclusive. A
 * pyramid is read from the declared max_num_reorder_frames: x264 declares 2
 * exactly when b-pyramid is on, and those streams are device-confirmed
 * refused, but there is no device evidence about a stream that declares
 * more reorder than it uses, which firmware may well play. Page media with
 * a declared pyramid is refused, so the page's next rendition is tried;
 * provider and offline streams keep firmware as the judge when the
 * software decoder cannot take them, and a real pyramid is then caught by
 * its opening refusals (psp_media_avc_opening_refused).
 *
 * The signal that would separate the two -- a B slice with nal_ref_idc != 0
 * in the first GOP -- needs sample payload the open path has not fetched
 * when it routes, so it is not used here.
 */
bool media_h264_firmware_references_conclusive(
    MediaH264FirmwareReferences verdict, bool page_media);
/* Every decoder refusal of a stream's coding starts with this text, which is
   what media_candidate_refused recognises. */
#define MEDIA_H264_UNSUPPORTED_STREAM "unsupported H.264 stream"
#define MEDIA_H264_SOFTWARE_CODING_REFUSAL \
    MEDIA_H264_UNSUPPORTED_STREAM \
    " (the optional decoder plays progressive 8-bit 4:2:0 only)"
#define MEDIA_H264_FIRST_PICTURE_REFUSAL \
    MEDIA_H264_UNSUPPORTED_STREAM " (firmware refused its first picture)"
#define MEDIA_H264_OPENING_REFUSAL \
    MEDIA_H264_UNSUPPORTED_STREAM " (firmware refused its opening pictures)"
/* The open error for a refused verdict ("unsupported H.264 stream (...)"),
   or NULL when the firmware decoder admits the stream. */
const char *media_h264_firmware_references_refusal(
    MediaH264FirmwareReferences verdict);
/*
 * Whether the first SPS's level_idc is one the firmware decoder admits
 * (psp_video_avc_level_supported: up to Level 3.0). An unreadable config
 * answers true; the route and geometry checks judge it. A Baseline/Main
 * stream above the range needs the software decoder or another rendition,
 * and the open path reports MEDIA_H264_FIRMWARE_LEVEL_REFUSAL for it.
 */
bool media_h264_avcc_firmware_level_supported(
    const unsigned char *config, size_t length);
bool media_h264_annexb_firmware_level_supported(
    const unsigned char *config, size_t length);
#define MEDIA_H264_FIRMWARE_LEVEL_REFUSAL "PSP AVC profile/level unsupported"
/* Bounded firmware display geometries. Tall 240p-class pictures use the
   existing large decode pool, not an enlarged or uncharged allocation. */
bool media_h264_psp_dimensions_supported(unsigned width, unsigned height);
/*
 * Validates one length-prefixed AVC access unit using the same bounded
 * contract as the PSP firmware bridge. Every NAL must fit exactly. The
 * Baseline/Main subset rejects extension/reserved NAL types, and any in-band
 * SPS/PPS must exactly match a parameter set admitted by avcC.
 */
bool media_h264_avcc_sample_is_admitted(
    const unsigned char *payload, size_t length,
    uint8_t nal_length_size, uint16_t width, uint16_t height,
    const unsigned char *config, size_t config_length);
/* Decoder-ready Annex-B companion to the avcC admission rule. Every in-band
   SPS must preserve the geometry used to size downstream decoder and RGB
   surfaces. Access units without an SPS retain the previously admitted one. */
bool media_h264_annexb_sample_is_admitted(
    const unsigned char *payload, size_t length,
    uint16_t width, uint16_t height);
/* Firmware decoders retain the parameter sets admitted at open. In addition
   to geometry, reject any in-band SPS/PPS which differs from that fixed
   Annex-B configuration. Access units without parameter sets remain valid. */
bool media_h264_annexb_sample_matches_config(
    const unsigned char *payload, size_t length,
    uint16_t width, uint16_t height,
    const unsigned char *config, size_t config_length);
bool media_h264_annexb_parameter_sets(
    const unsigned char *payload, size_t length,
    const unsigned char **sps, size_t *sps_length,
    const unsigned char **pps, size_t *pps_length);
/* Convert a complete Annex-B access unit to four-byte length-prefixed AVC in
   place. The caller supplies bounded spare capacity for three-byte start
   codes; no allocation is performed. */
bool media_h264_annexb_to_avcc_in_place(
    unsigned char *payload, size_t length, size_t capacity,
    size_t *output_length, unsigned *nal_count);
typedef struct {
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t samples_per_frame;
} MediaAacStreamInfo;
/*
 * PSP's fixed 1024-sample PCM path admits AAC-LC only. Parse the bounded
 * DecoderSpecificInfo inside an esds box and reject explicit or sync-extension
 * SBR/HE-AAC before firmware decode can write a 2048-sample frame.
 */
bool media_aac_esds_stream_info(
    const unsigned char *config, size_t length, MediaAacStreamInfo *info);
bool media_aac_esds_is_low_complexity(
    const unsigned char *config, size_t length);
/*
 * Returns the earliest next sample across tracks in normalized decode-time
 * order and advances only that track. Payload remains outside the demuxer;
 * read_sample performs one exact bounded range read into caller storage.
 */
bool media_mp4_next_sample(MediaMp4Demux *demux, MediaMp4Sample *sample);
/* Distinguishes clean end-of-stream from a lazy fragment transport/parse
   failure after next_sample returns false. */
bool media_mp4_last_error(const MediaMp4Demux *demux,
                          char *error, size_t error_size);
/*
 * True when the last next_sample/read_sample returned false only because the
 * source has not buffered the bytes yet. It is neither end-of-stream nor a
 * failure, and media_mp4_last_error() reports nothing for it: the caller must
 * retry on a later pump.
 */
bool media_mp4_would_block(const MediaMp4Demux *demux);
/*
 * Whether media_mp4_read_sample would find this already-peeked sample's
 * payload buffered right now.
 *
 * Non-consuming and side-effect free: it moves no cursor, touches neither the
 * would-block flag nor the error, and cannot start a fetch. The sample must be
 * one this demuxer returned from media_mp4_next_sample -- residency is a
 * question about {offset, size}, which the sample carries.
 *
 * A source that cannot answer is reported resident, so a false here is
 * reliable and a true is a source's best claim. See MediaRangeResident.
 */
bool media_mp4_sample_resident(const MediaMp4Demux *demux,
                               const MediaMp4Sample *sample);
/*
 * Out-of-order access to one track, for MediaPlayback's muxed audio lane.
 * peek describes the next sample of the first track of `kind` that
 * media_mp4_next_sample has not returned, only when its payload is resident;
 * it changes nothing. take reads exactly that sample without waiting and
 * advances only its track, so next_sample never returns it; on false nothing
 * moved and the would-block record is untouched.
 */
bool media_mp4_peek_track_sample(const MediaMp4Demux *demux,
                                 MediaMp4TrackKind kind,
                                 MediaMp4Sample *sample);
bool media_mp4_take_track_sample(MediaMp4Demux *demux,
                                 const MediaMp4Sample *sample,
                                 void *destination, size_t capacity);
/* Same read, but may block (bounded by the range's own wait budget) to fetch
   the window rather than answering would-block. Used only to warm a
   connection during an open, never on the playing path. */
bool media_mp4_read_sample_waiting(MediaMp4Demux *demux,
                           const MediaMp4Sample *sample,
                           void *destination, size_t capacity);
bool media_mp4_read_sample(MediaMp4Demux *demux,
                           const MediaMp4Sample *sample,
                           void *destination, size_t capacity);
/*
 * Repositions every track to the video keyframe at or before target_us.
 * Audio is aligned to that decode time. This scans retained sample tables,
 * never media payload, and therefore has a fixed memory cost.
 */
/* Cooperatively load the target fragment's metadata before resetting the
   decoder. False with would_block is pending, not a failed seek. No new cache
   is retained, and the ordinary seek still validates the actual keyframe. */
bool media_mp4_prepare_seek_us(MediaMp4Demux *demux, uint64_t target_us);
bool media_mp4_seek_us(MediaMp4Demux *demux, uint64_t target_us,
                       uint64_t *actual_us);
/* Repositions to the first actual video keyframe strictly after target_us.
 * Unlike guessing a later target and using seek_us(), this never rounds back
 * into the damaged dependency chain. Lazy fragmented inputs scan only the
 * necessary sidx windows. Audio is aligned to the selected video time. */
bool media_mp4_seek_after_us(MediaMp4Demux *demux, uint64_t target_us,
                             uint64_t *actual_us);
void media_mp4_rewind(MediaMp4Demux *demux);
size_t media_mp4_retained_bytes(const MediaMp4Demux *demux);
void media_mp4_close(MediaMp4Demux *demux);

#endif
