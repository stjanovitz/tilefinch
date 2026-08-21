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
MediaH264DecoderRoute media_h264_codec_string_decoder_route(
    const char *mime, uint8_t *profile_idc);
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
