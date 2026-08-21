#include "tilefinch/media_mp4.h"

#include "media_mp4_policy.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define MEDIA_MP4_INTERNAL_MAX_TRACKS 4u
#define MEDIA_MP4_MAX_SAMPLES_PER_TRUN UINT32_C(32768)
#define MEDIA_MP4_MAX_TOP_LEVEL_BOXES 512u

typedef struct {
    size_t start;
    size_t payload;
    size_t end;
    uint32_t type;
} Mp4Box;

typedef struct {
    uint32_t first_chunk;
    uint32_t samples_per_chunk;
} Mp4Stsc;

typedef struct {
    uint32_t count;
    uint32_t delta;
} Mp4Stts;

typedef struct {
    uint32_t count;
    int32_t offset;
} Mp4Ctts;

typedef struct {
    MediaMp4TrackInfo info;
    uint32_t track_id;
    uint32_t default_duration;
    uint32_t default_size;
    uint32_t default_flags;
    bool fragmented;
    uint32_t *sizes;
    uint64_t *chunks;
    Mp4Stsc *stsc;
    Mp4Stts *stts;
    Mp4Ctts *ctts;
    uint32_t *sync_samples;
    uint32_t chunk_count;
    uint32_t stsc_count;
    uint32_t stts_count;
    uint32_t ctts_count;
    uint32_t sync_count;
    uint64_t decode_duration;
    uint32_t maximum_positive_ctts;
    unsigned char *codec_config;
    size_t retained_bytes;
    size_t window_table_bytes;
    uint32_t window_sample_count;
    uint64_t window_dts_base;
    uint64_t fragment_decode_origin;
    bool have_fragment_decode_origin;
    bool lazy_fragmented;

    uint32_t sample;
    uint32_t chunk;
    uint32_t sample_in_chunk;
    uint32_t stsc_index;
    uint32_t stts_index;
    uint32_t stts_remaining;
    uint32_t ctts_index;
    uint32_t ctts_remaining;
    uint32_t sync_index;
    uint64_t byte_offset;
    uint64_t dts;
} Mp4Track;

typedef struct {
    uint64_t offset;
    uint64_t length;
    uint64_t start_time;
    uint64_t duration;
} Mp4SidxEntry;

struct MediaMp4Demux {
    Budget *budget;
    MediaRangeReader reader;
    MediaMp4Limits limits;
    Mp4Track *tracks;
    size_t track_count;
    size_t retained_bytes;
    char fragment_error[256];
    Mp4SidxEntry *fragment_index;
    size_t fragment_index_count;
    size_t fragment_index_at;
    uint32_t fragment_index_timescale;
    bool lazy_fragmented;
    /* A lazy sidx segment can cross the HTTP cache's arbitrary window
       boundary. Keep the already-validated box cursor across WOULD_BLOCK so
       the next pump resumes instead of rereading the segment from its first
       box and evicting the window it just requested. */
    size_t sidx_scan_index;
    uint64_t sidx_scan_cursor;
    uint64_t sidx_scan_end;
    uint64_t sidx_scan_moof_offset;
    size_t sidx_scan_moof_length;
    size_t sidx_scan_boxes;
    bool sidx_scan_found_moof;
    bool runtime_failed;
    /* The last playback read found its bytes unbuffered. Not a failure: no
       runtime_error is written for it and the caller retries later. */
    bool runtime_would_block;
    char runtime_error[256];
};

typedef struct {
    uint32_t samples;
    uint32_t stts_runs;
    uint32_t ctts_runs;
    uint32_t sync_samples;
    uint64_t duration;
    uint32_t previous_duration;
    int32_t previous_ctts;
    uint32_t maximum_positive_ctts;
    uint64_t decode_time_origin;
    bool have_duration;
    bool have_ctts;
    bool any_ctts;
    bool have_decode_time_origin;
} Mp4FragmentPlan;

typedef struct {
    uint32_t sample;
    uint32_t stts;
    uint32_t ctts;
    uint32_t sync;
    uint64_t duration;
} Mp4FragmentFill;

typedef struct Mp4FragmentBlob {
    struct Mp4FragmentBlob *next;
    uint64_t offset;
    size_t length;
    unsigned char *data;
} Mp4FragmentBlob;

typedef struct {
    Mp4FragmentBlob *first;
    Mp4FragmentBlob *last;
    size_t bytes;
    size_t index_bytes;
} Mp4FragmentCache;

static void mp4_track_rewind(Mp4Track *track);

static uint32_t mp4_u32(const unsigned char *bytes)
{
    return ((uint32_t) bytes[0] << 24)
         | ((uint32_t) bytes[1] << 16)
         | ((uint32_t) bytes[2] << 8)
         | (uint32_t) bytes[3];
}

static uint64_t mp4_u64(const unsigned char *bytes)
{
    return ((uint64_t) mp4_u32(bytes) << 32) | mp4_u32(bytes + 4);
}

static void mp4_error(char *error, size_t error_size,
                      const char *format, ...)
{
    if (error == NULL || error_size == 0) return;
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(error, error_size, format, arguments);
    va_end(arguments);
}

static void mp4_reader_error(
    const MediaRangeReader *reader, char *error, size_t error_size,
    const char *operation, uint64_t offset, size_t length)
{
    char detail[160] = {0};
    if (reader != NULL && reader->describe_failure != NULL) {
        (void) reader->describe_failure(
            reader->opaque, detail, sizeof(detail));
    }
    mp4_error(
        error, error_size, "MP4 %s range read failed at %llu (%zuB)%s%s",
        operation == NULL ? "source" : operation,
        (unsigned long long) offset, length,
        detail[0] == '\0' ? "" : ": ",
        detail[0] == '\0' ? "" : detail);
}

static void mp4_fragment_error(MediaMp4Demux *demux,
                               const char *format, ...)
{
    if (demux == NULL || demux->fragment_error[0] != '\0') return;
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(demux->fragment_error, sizeof(demux->fragment_error),
              format, arguments);
    va_end(arguments);
}

/*
 * Every read this demuxer performs goes through here.
 *
 * A range source that reaches the network must never make a *playing* stream
 * wait: the caller is inside a bounded pump on the interactive thread, and one
 * synchronous range read is what turned single feed units into 1.6-second
 * stalls on real hardware. With may_wait false, a source that has a poll entry
 * point answers WOULD_BLOCK instead, and this demuxer records that separately
 * from failure so the pump can retire the unit and come back next frame.
 *
 * Open, seek and rewind pass may_wait true and keep the blocking form they
 * always had: each is a transactional job phase which owns a deadline and a
 * cancellation token, and none of them can leave a sample half-read.
 */
static MediaRangeReadStatus mp4_reader_read(
    MediaMp4Demux *demux, uint64_t offset, void *destination, size_t length,
    bool may_wait)
{
    if (demux == NULL) return MEDIA_RANGE_READ_FAILED;
    if (!may_wait && demux->reader.poll != NULL) {
        MediaRangeReadStatus status = demux->reader.poll(
            demux->reader.opaque, offset, destination, length);
        if (status == MEDIA_RANGE_READ_WOULD_BLOCK)
            demux->runtime_would_block = true;
        return status;
    }
    return demux->reader.read(demux->reader.opaque, offset, destination, length)
        ? MEDIA_RANGE_READ_COMPLETE : MEDIA_RANGE_READ_FAILED;
}

static void mp4_fragment_read_error(MediaMp4Demux *demux,
                                    uint64_t offset, size_t length)
{
    char detail[160] = {0};
    if (demux != NULL && demux->reader.describe_failure != NULL) {
        (void) demux->reader.describe_failure(
            demux->reader.opaque, detail, sizeof(detail));
    }
    mp4_fragment_error(
        demux, "MP4 range read failed at offset %llu (%zuB)%s%s",
        (unsigned long long) offset, length,
        detail[0] == '\0' ? "" : ": ",
        detail[0] == '\0' ? "" : detail);
}

static bool mp4_box_next(const unsigned char *data, size_t limit,
                         size_t *cursor, Mp4Box *box)
{
    size_t start = *cursor;
    if (start > limit || limit - start < 8u) return false;
    uint64_t size = mp4_u32(data + start);
    size_t header = 8;
    box->type = mp4_u32(data + start + 4);
    if (size == 1) {
        if (limit - start < 16u) return false;
        size = mp4_u64(data + start + 8);
        header = 16;
    } else if (size == 0) {
        size = limit - start;
    }
    if (size < header || size > limit - start || size > SIZE_MAX)
        return false;
    box->start = start;
    box->payload = start + header;
    box->end = start + (size_t) size;
    *cursor = box->end;
    return true;
}

static bool mp4_box_find(const unsigned char *data, size_t start,
                         size_t end, uint32_t type, Mp4Box *box)
{
    size_t cursor = start;
    Mp4Box candidate;
    while (mp4_box_next(data, end, &cursor, &candidate)) {
        if (candidate.type == type) {
            *box = candidate;
            return true;
        }
    }
    return false;
}

static void *mp4_array(MediaMp4Demux *demux, size_t count,
                       size_t element_size, size_t *retained)
{
    if (count == 0 || element_size > SIZE_MAX / count) return NULL;
    size_t bytes = count * element_size;
    if (retained != NULL
        && (bytes > demux->limits.maximum_track_table_bytes
            || *retained
                > demux->limits.maximum_track_table_bytes - bytes)) {
        return NULL;
    }
    void *allocation = budget_calloc_category(
        demux->budget, BUDGET_CATEGORY_RESOURCE, count, element_size);
    if (allocation != NULL) {
        demux->retained_bytes += bytes;
        if (retained != NULL) *retained += bytes;
    }
    return allocation;
}

static bool mp4_size_accumulate(size_t *total, size_t count,
                                size_t element_size)
{
    if (total == NULL || (count != 0 && element_size > SIZE_MAX / count))
        return false;
    size_t bytes = count * element_size;
    if (bytes > SIZE_MAX - *total) return false;
    *total += bytes;
    return true;
}

static bool mp4_parse_stsz(MediaMp4Demux *demux, const unsigned char *data,
                           Mp4Box box, Mp4Track *track)
{
    if (box.end - box.payload < 12u) return false;
    uint32_t fixed = mp4_u32(data + box.payload + 4);
    uint32_t count = mp4_u32(data + box.payload + 8);
    if (count == 0 || count > demux->limits.maximum_samples_per_track
        || (!fixed
            && (uint64_t) count * 4u > box.end - box.payload - 12u))
        return false;
    track->sizes = mp4_array(
        demux, count, sizeof(*track->sizes), &track->retained_bytes);
    if (track->sizes == NULL) return false;
    track->info.sample_count = count;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t size = fixed ? fixed
            : mp4_u32(data + box.payload + 12u + (size_t) i * 4u);
        if (size == 0 || size > demux->limits.maximum_sample_bytes)
            return false;
        track->sizes[i] = size;
        if (size > track->info.largest_sample)
            track->info.largest_sample = size;
    }
    return true;
}

static bool mp4_parse_chunks(MediaMp4Demux *demux,
                             const unsigned char *data,
                             Mp4Box box, Mp4Track *track)
{
    if (box.end - box.payload < 8u) return false;
    uint32_t count = mp4_u32(data + box.payload + 4);
    size_t stride = box.type == MEDIA_MP4_FOURCC('c','o','6','4') ? 8u : 4u;
    if (count == 0 || count > demux->limits.maximum_chunks_per_track
        || (uint64_t) count * stride > box.end - box.payload - 8u)
        return false;
    track->chunks = mp4_array(
        demux, count, sizeof(*track->chunks), &track->retained_bytes);
    if (track->chunks == NULL) return false;
    track->chunk_count = count;
    for (uint32_t i = 0; i < count; i++) {
        const unsigned char *value =
            data + box.payload + 8u + (size_t) i * stride;
        track->chunks[i] = stride == 8 ? mp4_u64(value) : mp4_u32(value);
    }
    return true;
}

static bool mp4_parse_stsc(MediaMp4Demux *demux,
                           const unsigned char *data,
                           Mp4Box box, Mp4Track *track)
{
    if (box.end - box.payload < 8u) return false;
    uint32_t count = mp4_u32(data + box.payload + 4);
    if (count == 0 || count > demux->limits.maximum_table_entries
        || (uint64_t) count * 12u > box.end - box.payload - 8u)
        return false;
    track->stsc = mp4_array(
        demux, count, sizeof(*track->stsc), &track->retained_bytes);
    if (track->stsc == NULL) return false;
    track->stsc_count = count;
    uint32_t previous = 0;
    for (uint32_t i = 0; i < count; i++) {
        const unsigned char *entry =
            data + box.payload + 8u + (size_t) i * 12u;
        track->stsc[i].first_chunk = mp4_u32(entry);
        track->stsc[i].samples_per_chunk = mp4_u32(entry + 4);
        uint32_t sample_description_index = mp4_u32(entry + 8);
        if (track->stsc[i].first_chunk == 0
            || track->stsc[i].first_chunk <= previous
            || track->stsc[i].samples_per_chunk == 0
            /*
             * Tilefinch retains one bounded sample entry. Silently ignoring a
             * different description can feed payload to the wrong decoder
             * configuration, so reject it at the table boundary.
             */
            || sample_description_index != 1u) return false;
        previous = track->stsc[i].first_chunk;
    }
    return track->stsc[0].first_chunk == 1;
}

static bool mp4_parse_stts(MediaMp4Demux *demux,
                           const unsigned char *data,
                           Mp4Box box, Mp4Track *track)
{
    if (box.end - box.payload < 8u) return false;
    uint32_t count = mp4_u32(data + box.payload + 4);
    if (count == 0 || count > demux->limits.maximum_table_entries
        || (uint64_t) count * 8u > box.end - box.payload - 8u)
        return false;
    track->stts = mp4_array(
        demux, count, sizeof(*track->stts), &track->retained_bytes);
    if (track->stts == NULL) return false;
    track->stts_count = count;
    uint64_t samples = 0;
    uint64_t duration = 0;
    for (uint32_t i = 0; i < count; i++) {
        const unsigned char *entry =
            data + box.payload + 8u + (size_t) i * 8u;
        track->stts[i].count = mp4_u32(entry);
        track->stts[i].delta = mp4_u32(entry + 4);
        if (track->stts[i].count == 0 || track->stts[i].delta == 0)
            return false;
        uint64_t run_duration =
            (uint64_t) track->stts[i].count * track->stts[i].delta;
        if (run_duration > (uint64_t) INT64_MAX - duration)
            return false;
        duration += run_duration;
        samples += track->stts[i].count;
    }
    track->decode_duration = duration;
    return samples == track->info.sample_count;
}

static bool mp4_parse_ctts(MediaMp4Demux *demux,
                           const unsigned char *data,
                           Mp4Box box, Mp4Track *track)
{
    if (box.end - box.payload < 8u) return false;
    unsigned version = data[box.payload];
    uint32_t count = mp4_u32(data + box.payload + 4);
    if (version > 1 || count == 0
        || count > demux->limits.maximum_table_entries
        || (uint64_t) count * 8u > box.end - box.payload - 8u)
        return false;
    track->ctts = mp4_array(
        demux, count, sizeof(*track->ctts), &track->retained_bytes);
    if (track->ctts == NULL) return false;
    track->ctts_count = count;
    uint64_t samples = 0;
    for (uint32_t i = 0; i < count; i++) {
        const unsigned char *entry =
            data + box.payload + 8u + (size_t) i * 8u;
        track->ctts[i].count = mp4_u32(entry);
        uint32_t encoded = mp4_u32(entry + 4);
        if (version == 0 && encoded > INT32_MAX) return false;
        track->ctts[i].offset = (int32_t) encoded;
        if (track->ctts[i].count == 0) return false;
        if (track->ctts[i].offset > 0
            && (uint32_t) track->ctts[i].offset
               > track->maximum_positive_ctts) {
            track->maximum_positive_ctts =
                (uint32_t) track->ctts[i].offset;
        }
        samples += track->ctts[i].count;
    }
    return samples == track->info.sample_count
        && track->maximum_positive_ctts
           <= (uint64_t) INT64_MAX - track->decode_duration;
}

static bool mp4_parse_stss(MediaMp4Demux *demux,
                           const unsigned char *data,
                           Mp4Box box, Mp4Track *track)
{
    if (box.end - box.payload < 8u) return false;
    uint32_t count = mp4_u32(data + box.payload + 4);
    if (count == 0 || count > demux->limits.maximum_table_entries
        || (uint64_t) count * 4u > box.end - box.payload - 8u)
        return false;
    track->sync_samples = mp4_array(
        demux, count, sizeof(*track->sync_samples),
        &track->retained_bytes);
    if (track->sync_samples == NULL) return false;
    track->sync_count = count;
    uint32_t previous = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t sample = mp4_u32(
            data + box.payload + 8u + (size_t) i * 4u);
        if (sample == 0 || sample <= previous
            || sample > track->info.sample_count) return false;
        track->sync_samples[i] = sample;
        previous = sample;
    }
    return true;
}

static bool mp4_copy_config(MediaMp4Demux *demux,
                            const unsigned char *data,
                            Mp4Box box, Mp4Track *track)
{
    size_t length = box.end - box.payload;
    if (length == 0 || length > 4096u) return false;
    track->codec_config = mp4_array(
        demux, length, 1, &track->retained_bytes);
    if (track->codec_config == NULL) return false;
    memcpy(track->codec_config, data + box.payload, length);
    track->info.codec_config = track->codec_config;
    track->info.codec_config_length = length;
    if (box.type == MEDIA_MP4_FOURCC('a','v','c','C')) {
        if (length < 5u || track->codec_config[0] != 1) return false;
        track->info.nal_length_size =
            (uint8_t) ((track->codec_config[4] & 3u) + 1u);
    }
    return true;
}

static bool mp4_descriptor(
    const unsigned char *data, size_t length, size_t *cursor,
    uint8_t wanted_tag, size_t *payload, size_t *payload_length)
{
    if (data == NULL || cursor == NULL || *cursor >= length
        || data[(*cursor)++] != wanted_tag) return false;
    size_t value = 0;
    bool complete = false;
    for (unsigned byte = 0; byte < 4; byte++) {
        if (*cursor >= length || value > (SIZE_MAX >> 7u)) return false;
        uint8_t encoded = data[(*cursor)++];
        value = (value << 7u) | (encoded & 0x7fu);
        if ((encoded & 0x80u) == 0) {
            complete = true;
            break;
        }
    }
    if (!complete || value > length - *cursor) return false;
    if (payload != NULL) *payload = *cursor;
    if (payload_length != NULL) *payload_length = value;
    return true;
}

static bool mp4_bits(
    const unsigned char *data, size_t length, size_t *bit_cursor,
    unsigned count, uint32_t *value)
{
    if (data == NULL || bit_cursor == NULL || value == NULL
        || count > 32u || *bit_cursor > length * 8u
        || count > length * 8u - *bit_cursor) return false;
    uint32_t result = 0;
    for (unsigned bit = 0; bit < count; bit++) {
        size_t at = *bit_cursor + bit;
        result = (result << 1u)
            | ((data[at / 8u] >> (7u - at % 8u)) & 1u);
    }
    *bit_cursor += count;
    *value = result;
    return true;
}

static bool mp4_aac_audio_specific_config_info(
    const unsigned char *data, size_t length, MediaAacStreamInfo *info)
{
    static const uint32_t sample_rates[] = {
        96000u, 88200u, 64000u, 48000u, 44100u, 32000u, 24000u,
        22050u, 16000u, 12000u, 11025u, 8000u, 7350u
    };
    if (info == NULL) return false;
    if (length > SIZE_MAX / 8u) return false;
    size_t bit = 0;
    uint32_t object_type = 0;
    if (!mp4_bits(data, length, &bit, 5, &object_type)) return false;
    if (object_type == 31u) {
        uint32_t extension = 0;
        if (!mp4_bits(data, length, &bit, 6, &extension)) return false;
        object_type = 32u + extension;
    }
    if (object_type == 5u || object_type == 29u || object_type != 2u) {
        return false;
    }
    uint32_t frequency_index = 0;
    if (!mp4_bits(data, length, &bit, 4, &frequency_index)) return false;
    uint32_t sample_rate = 0;
    if (frequency_index == 15u) {
        if (!mp4_bits(
                data, length, &bit, 24, &sample_rate)
            || sample_rate == 0) return false;
    } else if (frequency_index
               < sizeof(sample_rates) / sizeof(sample_rates[0])) {
        sample_rate = sample_rates[frequency_index];
    } else return false;
    uint32_t channel_configuration = 0;
    if (!mp4_bits(
            data, length, &bit, 4,
            &channel_configuration)) return false;
    static const uint8_t channel_counts[8] = {0, 1, 2, 3, 4, 5, 6, 8};
    if (channel_configuration == 0
        || channel_configuration
             >= sizeof(channel_counts) / sizeof(channel_counts[0])) {
        return false;
    }
    uint32_t frame_length_flag = 0;
    uint32_t depends_on_core_coder = 0;
    uint32_t extension_flag = 0;
    if (!mp4_bits(data, length, &bit, 1, &frame_length_flag)
        || !mp4_bits(data, length, &bit, 1, &depends_on_core_coder)) {
        return false;
    }
    if (depends_on_core_coder != 0) {
        uint32_t core_delay = 0;
        if (!mp4_bits(data, length, &bit, 14, &core_delay)) return false;
    }
    if (!mp4_bits(data, length, &bit, 1, &extension_flag)) return false;
    (void) extension_flag;

    /*
     * Backward-compatible HE-AAC advertises an AAC-LC core first, followed
     * by the 11-bit sync extension 0x2b7 and object type 5. Scan the small,
     * already bounded ASC bit string conservatively; a match with SBR present
     * is enough to reject it.
     */
    for (size_t scan = bit; scan + 17u <= length * 8u; scan++) {
        size_t cursor = scan;
        uint32_t sync = 0, extension_type = 0, sbr = 0;
        if (!mp4_bits(data, length, &cursor, 11, &sync)
            || sync != 0x2b7u
            || !mp4_bits(data, length, &cursor, 5, &extension_type)
            || extension_type != 5u
            || !mp4_bits(data, length, &cursor, 1, &sbr)) continue;
        if (sbr != 0) return false;
    }
    *info = (MediaAacStreamInfo) {
        .sample_rate = sample_rate,
        .channels = channel_counts[channel_configuration],
        .samples_per_frame = frame_length_flag != 0 ? 960u : 1024u
    };
    return true;
}

bool media_aac_esds_stream_info(
    const unsigned char *config, size_t length, MediaAacStreamInfo *info)
{
    if (config == NULL || length < 8u || info == NULL) return false;
    size_t cursor = 4u;
    size_t es_payload = 0, es_length = 0;
    if (!mp4_descriptor(
            config, length, &cursor, 0x03u,
            &es_payload, &es_length)) return false;
    size_t es_end = es_payload + es_length;
    cursor = es_payload;
    if (es_end > length || es_end - cursor < 3u) return false;
    cursor += 2u;
    uint8_t flags = config[cursor++];
    if ((flags & 0x80u) != 0) {
        if (es_end - cursor < 2u) return false;
        cursor += 2u;
    }
    if ((flags & 0x40u) != 0) {
        if (cursor >= es_end) return false;
        size_t url_length = config[cursor++];
        if (url_length > es_end - cursor) return false;
        cursor += url_length;
    }
    if ((flags & 0x20u) != 0) {
        if (es_end - cursor < 2u) return false;
        cursor += 2u;
    }
    size_t decoder_payload = 0, decoder_length = 0;
    if (!mp4_descriptor(
            config, es_end, &cursor, 0x04u,
            &decoder_payload, &decoder_length)) return false;
    size_t decoder_end = decoder_payload + decoder_length;
    if (decoder_end > es_end || decoder_length < 13u) return false;
    cursor = decoder_payload + 13u;
    size_t asc_payload = 0, asc_length = 0;
    if (!mp4_descriptor(
            config, decoder_end, &cursor, 0x05u,
            &asc_payload, &asc_length)
        || asc_length == 0 || asc_payload + asc_length > decoder_end) {
        return false;
    }
    MediaAacStreamInfo parsed = {0};
    if (!mp4_aac_audio_specific_config_info(
            config + asc_payload, asc_length, &parsed)) return false;
    *info = parsed;
    return true;
}

bool media_aac_esds_is_low_complexity(
    const unsigned char *config, size_t length)
{
    MediaAacStreamInfo info = {0};
    return media_aac_esds_stream_info(config, length, &info)
        && info.samples_per_frame == 1024u;
}

static bool mp4_parse_stsd(MediaMp4Demux *demux,
                           const unsigned char *data,
                           Mp4Box box, Mp4Track *track)
{
    if (box.end - box.payload < 16u
        || mp4_u32(data + box.payload + 4) != 1u) return false;
    size_t entry = box.payload + 8u;
    uint32_t entry_size = mp4_u32(data + entry);
    if (entry_size < 16u || entry_size > box.end - entry) return false;
    track->info.codec = mp4_u32(data + entry + 4);
    size_t children = 0;
    if (track->info.kind == MEDIA_MP4_TRACK_VIDEO) {
        if (entry_size < 86u) return false;
        track->info.width = (uint16_t) (
            (data[entry + 32] << 8) | data[entry + 33]);
        track->info.height = (uint16_t) (
            (data[entry + 34] << 8) | data[entry + 35]);
        children = entry + 86u;
    } else if (track->info.kind == MEDIA_MP4_TRACK_AUDIO) {
        if (entry_size < 36u) return false;
        track->info.channels = (uint16_t) (
            (data[entry + 24] << 8) | data[entry + 25]);
        track->info.sample_rate = mp4_u32(data + entry + 32) >> 16;
        children = entry + 36u;
    }
    if (children == 0 || children >= entry + entry_size) return true;
    size_t cursor = children;
    Mp4Box child;
    while (mp4_box_next(data, entry + entry_size, &cursor, &child)) {
        if (child.type == MEDIA_MP4_FOURCC('a','v','c','C')
            || child.type == MEDIA_MP4_FOURCC('e','s','d','s')) {
            return mp4_copy_config(demux, data, child, track);
        }
    }
    return true;
}

static bool mp4_validate_chunks(const MediaMp4Demux *demux,
                                const Mp4Track *track)
{
    uint32_t sample_count = track->lazy_fragmented
        ? track->window_sample_count : track->info.sample_count;
    uint32_t sample = 0;
    uint32_t stsc = 0;
    for (uint32_t chunk = 0; chunk < track->chunk_count; chunk++) {
        while (stsc + 1u < track->stsc_count
               && chunk + 1u >= track->stsc[stsc + 1u].first_chunk)
            stsc++;
        uint64_t offset = track->chunks[chunk];
        for (uint32_t within = 0;
             within < track->stsc[stsc].samples_per_chunk; within++) {
            if (sample >= sample_count
                || offset > demux->reader.length
                || track->sizes[sample] > demux->reader.length - offset)
                return false;
            offset += track->sizes[sample++];
        }
    }
    return sample == sample_count;
}

static bool mp4_parse_track(MediaMp4Demux *demux,
                            const unsigned char *data,
                            Mp4Box trak, Mp4Track *track)
{
    Mp4Box tkhd, mdia, mdhd, hdlr, minf, stbl;
    Mp4Box stsd, stsz, stsc, chunks, stts;
    memset(track, 0, sizeof(*track));
    bool have_tkhd = mp4_box_find(
        data, trak.payload, trak.end,
        MEDIA_MP4_FOURCC('t','k','h','d'), &tkhd);
    if (!mp4_box_find(data, trak.payload, trak.end,
                      MEDIA_MP4_FOURCC('m','d','i','a'), &mdia)
        || !mp4_box_find(data, mdia.payload, mdia.end,
                         MEDIA_MP4_FOURCC('m','d','h','d'), &mdhd)
        || !mp4_box_find(data, mdia.payload, mdia.end,
                         MEDIA_MP4_FOURCC('h','d','l','r'), &hdlr)
        || !mp4_box_find(data, mdia.payload, mdia.end,
                         MEDIA_MP4_FOURCC('m','i','n','f'), &minf)
        || !mp4_box_find(data, minf.payload, minf.end,
                         MEDIA_MP4_FOURCC('s','t','b','l'), &stbl)
        || !mp4_box_find(data, stbl.payload, stbl.end,
                         MEDIA_MP4_FOURCC('s','t','s','d'), &stsd)
        || !mp4_box_find(data, stbl.payload, stbl.end,
                         MEDIA_MP4_FOURCC('s','t','s','z'), &stsz)
        || !mp4_box_find(data, stbl.payload, stbl.end,
                         MEDIA_MP4_FOURCC('s','t','s','c'), &stsc)
        || !mp4_box_find(data, stbl.payload, stbl.end,
                         MEDIA_MP4_FOURCC('s','t','t','s'), &stts))
        return false;
    if (!mp4_box_find(data, stbl.payload, stbl.end,
                      MEDIA_MP4_FOURCC('s','t','c','o'), &chunks)
        && !mp4_box_find(data, stbl.payload, stbl.end,
                         MEDIA_MP4_FOURCC('c','o','6','4'), &chunks))
        return false;
    if (mdhd.end - mdhd.payload < 20u
        || hdlr.end - hdlr.payload < 12u)
        return false;
    if (have_tkhd) {
        unsigned tkhd_version = data[tkhd.payload];
        size_t track_id_offset = tkhd_version == 1 ? 20u : 12u;
        if (tkhd_version > 1
            || tkhd.end - tkhd.payload < track_id_offset + 4u)
            return false;
        track->track_id = mp4_u32(
            data + tkhd.payload + track_id_offset);
        if (track->track_id == 0) return false;
    }
    unsigned version = data[mdhd.payload];
    size_t timescale_offset = version == 1 ? 20u : 12u;
    size_t duration_offset = version == 1 ? 24u : 16u;
    size_t duration_bytes = version == 1 ? 8u : 4u;
    if (version > 1 || mdhd.end - mdhd.payload
        < duration_offset + duration_bytes) return false;
    track->info.timescale = mp4_u32(data + mdhd.payload + timescale_offset);
    if (track->info.timescale == 0
        || track->info.timescale > MEDIA_MP4_MAX_TIMESCALE) return false;
    track->info.duration = duration_bytes == 8
        ? mp4_u64(data + mdhd.payload + duration_offset)
        : mp4_u32(data + mdhd.payload + duration_offset);
    uint32_t handler = mp4_u32(data + hdlr.payload + 8);
    track->info.kind = handler == MEDIA_MP4_FOURCC('v','i','d','e')
        ? MEDIA_MP4_TRACK_VIDEO
        : handler == MEDIA_MP4_FOURCC('s','o','u','n')
            ? MEDIA_MP4_TRACK_AUDIO : MEDIA_MP4_TRACK_OTHER;
    uint32_t stsz_count = stsz.end - stsz.payload >= 12u
        ? mp4_u32(data + stsz.payload + 8u) : UINT32_MAX;
    uint32_t stsc_count = stsc.end - stsc.payload >= 8u
        ? mp4_u32(data + stsc.payload + 4u) : UINT32_MAX;
    uint32_t chunk_count = chunks.end - chunks.payload >= 8u
        ? mp4_u32(data + chunks.payload + 4u) : UINT32_MAX;
    uint32_t stts_count = stts.end - stts.payload >= 8u
        ? mp4_u32(data + stts.payload + 4u) : UINT32_MAX;
    if (stsz_count == 0 && stsc_count == 0
        && chunk_count == 0 && stts_count == 0) {
        if (track->track_id == 0) return false;
        track->fragmented = true;
        return mp4_parse_stsd(demux, data, stsd, track);
    }
    if (!mp4_parse_stsz(demux, data, stsz, track)
        || !mp4_parse_chunks(demux, data, chunks, track)
        || !mp4_parse_stsc(demux, data, stsc, track)
        || !mp4_parse_stts(demux, data, stts, track)
        || !mp4_parse_stsd(demux, data, stsd, track))
        return false;
    Mp4Box optional;
    if (mp4_box_find(data, stbl.payload, stbl.end,
                     MEDIA_MP4_FOURCC('c','t','t','s'), &optional)
        && !mp4_parse_ctts(demux, data, optional, track)) return false;
    if (mp4_box_find(data, stbl.payload, stbl.end,
                     MEDIA_MP4_FOURCC('s','t','s','s'), &optional)
        && !mp4_parse_stss(demux, data, optional, track)) return false;
    return mp4_validate_chunks(demux, track);
}

static Mp4Track *mp4_track_by_id(MediaMp4Demux *demux, uint32_t track_id)
{
    for (size_t at = 0; at < demux->track_count; at++) {
        if (demux->tracks[at].track_id == track_id)
            return &demux->tracks[at];
    }
    return NULL;
}

static size_t mp4_track_index(const MediaMp4Demux *demux,
                              const Mp4Track *track)
{
    return (size_t) (track - demux->tracks);
}

static bool mp4_parse_fragment_defaults(
    MediaMp4Demux *demux, const unsigned char *data, Mp4Box moov)
{
    Mp4Box mvex;
    if (!mp4_box_find(data, moov.payload, moov.end,
                      MEDIA_MP4_FOURCC('m','v','e','x'), &mvex)) {
        for (size_t at = 0; at < demux->track_count; at++) {
            if (demux->tracks[at].fragmented) return false;
        }
        return true;
    }
    size_t cursor = mvex.payload;
    Mp4Box child;
    while (mp4_box_next(data, mvex.end, &cursor, &child)) {
        if (child.type != MEDIA_MP4_FOURCC('t','r','e','x')) continue;
        if (child.end - child.payload < 24u) return false;
        Mp4Track *track = mp4_track_by_id(
            demux, mp4_u32(data + child.payload + 4u));
        if (track == NULL) continue;
        if (mp4_u32(data + child.payload + 8u) != 1u) return false;
        track->default_duration = mp4_u32(data + child.payload + 12u);
        track->default_size = mp4_u32(data + child.payload + 16u);
        track->default_flags = mp4_u32(data + child.payload + 20u);
    }
    for (size_t at = 0; at < demux->track_count; at++) {
        if (demux->tracks[at].fragmented
            && demux->tracks[at].track_id == 0) return false;
    }
    return true;
}

static bool mp4_add_signed_offset(uint64_t base, int32_t delta,
                                  uint64_t *result)
{
    if (delta >= 0) {
        if ((uint64_t) delta > UINT64_MAX - base) return false;
        *result = base + (uint32_t) delta;
        return true;
    }
    uint64_t magnitude = (uint64_t) (-(int64_t) delta);
    if (magnitude > base) return false;
    *result = base - magnitude;
    return true;
}

static bool mp4_fragment_record(
    MediaMp4Demux *demux, Mp4Track *track,
    Mp4FragmentPlan *plan, Mp4FragmentFill *fill,
    uint64_t offset, uint32_t size, uint32_t duration,
    int32_t composition_offset, uint32_t flags)
{
    if (size == 0 || size > demux->limits.maximum_sample_bytes) {
        mp4_fragment_error(
            demux, "MP4 fragment sample size %u outside 1-%uB",
            size, demux->limits.maximum_sample_bytes);
        return false;
    }
    if (duration == 0 || offset > demux->reader.length
        || size > demux->reader.length - offset) {
        mp4_fragment_error(
            demux, "MP4 fragment sample range invalid at %llu (%uB)",
            (unsigned long long) offset, size);
        return false;
    }
    if (plan->samples >= demux->limits.maximum_samples_per_track) {
        mp4_fragment_error(
            demux, "MP4 fragment sample limit reached: %u/%u",
            plan->samples, demux->limits.maximum_samples_per_track);
        return false;
    }
    if (plan->samples >= demux->limits.maximum_chunks_per_track) {
        mp4_fragment_error(
            demux, "MP4 fragment chunk limit reached: %u/%u",
            plan->samples, demux->limits.maximum_chunks_per_track);
        return false;
    }
    bool sync = track->info.kind == MEDIA_MP4_TRACK_VIDEO
        && (flags & UINT32_C(0x00010000)) == 0;
    if (fill == NULL) {
        if (!plan->have_duration
            || duration != plan->previous_duration) {
            if (plan->stts_runs >= demux->limits.maximum_table_entries) {
                mp4_fragment_error(
                    demux, "MP4 fragment stts limit reached: %u/%u",
                    plan->stts_runs,
                    demux->limits.maximum_table_entries);
                return false;
            }
            plan->stts_runs++;
            plan->previous_duration = duration;
            plan->have_duration = true;
        }
        if (!plan->have_ctts
            || composition_offset != plan->previous_ctts) {
            if (plan->ctts_runs >= demux->limits.maximum_table_entries) {
                mp4_fragment_error(
                    demux, "MP4 fragment ctts limit reached: %u/%u",
                    plan->ctts_runs,
                    demux->limits.maximum_table_entries);
                return false;
            }
            plan->ctts_runs++;
            plan->previous_ctts = composition_offset;
            plan->have_ctts = true;
        }
        if (composition_offset != 0) plan->any_ctts = true;
        if (composition_offset > 0
            && (uint32_t) composition_offset
               > plan->maximum_positive_ctts) {
            plan->maximum_positive_ctts =
                (uint32_t) composition_offset;
        }
        if (sync) plan->sync_samples++;
        plan->samples++;
        if (duration > (uint64_t) INT64_MAX - plan->duration) {
            mp4_fragment_error(
                demux, "MP4 fragment decode timestamp exceeds signed range");
            return false;
        }
        plan->duration += duration;
        if (plan->maximum_positive_ctts
            > (uint64_t) INT64_MAX - plan->duration) {
            mp4_fragment_error(
                demux,
                "MP4 fragment presentation timestamp exceeds signed range");
            return false;
        }
        return true;
    }
    if (fill->sample >= plan->samples) return false;
    uint32_t sample_number = fill->sample + 1u;
    track->sizes[fill->sample] = size;
    track->chunks[fill->sample] = offset;
    if (size > track->info.largest_sample)
        track->info.largest_sample = size;
    fill->sample++;
    if (fill->stts == 0
        || track->stts[fill->stts - 1u].delta != duration) {
        if (fill->stts >= plan->stts_runs) return false;
        track->stts[fill->stts++] = (Mp4Stts) {
            .count = 1, .delta = duration
        };
    } else {
        track->stts[fill->stts - 1u].count++;
    }
    if (plan->any_ctts) {
        if (fill->ctts == 0
            || track->ctts[fill->ctts - 1u].offset
                   != composition_offset) {
            if (fill->ctts >= plan->ctts_runs) return false;
            track->ctts[fill->ctts++] = (Mp4Ctts) {
                .count = 1, .offset = composition_offset
            };
        } else {
            track->ctts[fill->ctts - 1u].count++;
        }
    }
    if (sync) {
        if (fill->sync >= plan->sync_samples) return false;
        track->sync_samples[fill->sync++] = sample_number;
    }
    if (duration > UINT64_MAX - fill->duration) return false;
    fill->duration += duration;
    return true;
}

static bool mp4_parse_fragment_traf(
    MediaMp4Demux *demux, const unsigned char *data, Mp4Box traf,
    uint64_t moof_offset, Mp4FragmentPlan *plans,
    Mp4FragmentFill *fills)
{
    Mp4Box tfhd;
    if (!mp4_box_find(data, traf.payload, traf.end,
                      MEDIA_MP4_FOURCC('t','f','h','d'), &tfhd)
        || tfhd.end - tfhd.payload < 8u) return false;
    const unsigned char *header = data + tfhd.payload;
    uint32_t tfhd_flags = mp4_u32(header) & UINT32_C(0x00ffffff);
    Mp4Track *track = mp4_track_by_id(demux, mp4_u32(header + 4u));
    if (track == NULL || !track->fragmented) return false;
    size_t track_at = mp4_track_index(demux, track);
    Mp4FragmentPlan *plan = &plans[track_at];
    Mp4FragmentFill *fill = fills == NULL ? NULL : &fills[track_at];
    size_t cursor = tfhd.payload + 8u;
    uint64_t base_data_offset = moof_offset;
    if ((tfhd_flags & UINT32_C(0x000001)) != 0) {
        if (tfhd.end - cursor < 8u) return false;
        base_data_offset = mp4_u64(data + cursor);
        cursor += 8u;
    }
    if ((tfhd_flags & UINT32_C(0x000002)) != 0) {
        if (tfhd.end - cursor < 4u) return false;
        if (mp4_u32(data + cursor) != 1u) return false;
        cursor += 4u;
    }
    uint32_t default_duration = track->default_duration;
    uint32_t default_size = track->default_size;
    uint32_t default_flags = track->default_flags;
    if ((tfhd_flags & UINT32_C(0x000008)) != 0) {
        if (tfhd.end - cursor < 4u) return false;
        default_duration = mp4_u32(data + cursor);
        cursor += 4u;
    }
    if ((tfhd_flags & UINT32_C(0x000010)) != 0) {
        if (tfhd.end - cursor < 4u) return false;
        default_size = mp4_u32(data + cursor);
        cursor += 4u;
    }
    if ((tfhd_flags & UINT32_C(0x000020)) != 0) {
        if (tfhd.end - cursor < 4u) return false;
        default_flags = mp4_u32(data + cursor);
    }
    Mp4Box tfdt;
    if (mp4_box_find(data, traf.payload, traf.end,
                     MEDIA_MP4_FOURCC('t','f','d','t'), &tfdt)) {
        if (tfdt.end - tfdt.payload < 8u) return false;
        unsigned version = data[tfdt.payload];
        uint64_t decode_time;
        if (version == 1) {
            if (tfdt.end - tfdt.payload < 12u) return false;
            decode_time = mp4_u64(data + tfdt.payload + 4u);
        } else if (version == 0) {
            decode_time = mp4_u32(data + tfdt.payload + 4u);
        } else {
            return false;
        }
        uint64_t elapsed = fill == NULL ? plan->duration : fill->duration;
        if (!plan->have_decode_time_origin) {
            if (fill != NULL || decode_time < elapsed) return false;
            plan->decode_time_origin = decode_time - elapsed;
            plan->have_decode_time_origin = true;
        }
        if (decode_time < plan->decode_time_origin
            || decode_time - plan->decode_time_origin != elapsed) {
            return false;
        }
    }
    uint64_t data_offset = base_data_offset;
    size_t child_cursor = traf.payload;
    Mp4Box child;
    bool found_trun = false;
    while (mp4_box_next(data, traf.end, &child_cursor, &child)) {
        if (child.type != MEDIA_MP4_FOURCC('t','r','u','n')) continue;
        found_trun = true;
        if (child.end - child.payload < 8u) return false;
        const unsigned char *run = data + child.payload;
        unsigned version = run[0];
        uint32_t run_flags = mp4_u32(run) & UINT32_C(0x00ffffff);
        uint32_t sample_count = mp4_u32(run + 4u);
        if (version > 1
            || sample_count > MEDIA_MP4_MAX_SAMPLES_PER_TRUN) {
            if (sample_count > MEDIA_MP4_MAX_SAMPLES_PER_TRUN) {
                mp4_fragment_error(
                    demux, "MP4 trun sample limit reached: %u/%u",
                    sample_count, MEDIA_MP4_MAX_SAMPLES_PER_TRUN);
            }
            return false;
        }
        size_t at = child.payload + 8u;
        if ((run_flags & UINT32_C(0x000001)) != 0) {
            if (child.end - at < 4u
                || !mp4_add_signed_offset(
                    base_data_offset, (int32_t) mp4_u32(data + at),
                    &data_offset)) return false;
            at += 4u;
        }
        uint32_t first_flags = default_flags;
        bool have_first_flags =
            (run_flags & UINT32_C(0x000004)) != 0;
        if (have_first_flags) {
            if (child.end - at < 4u) return false;
            first_flags = mp4_u32(data + at);
            at += 4u;
        }
        for (uint32_t sample = 0; sample < sample_count; sample++) {
            uint32_t duration = default_duration;
            uint32_t size = default_size;
            uint32_t sample_flags =
                sample == 0 && have_first_flags
                    ? first_flags : default_flags;
            int32_t composition_offset = 0;
            if ((run_flags & UINT32_C(0x000100)) != 0) {
                if (child.end - at < 4u) return false;
                duration = mp4_u32(data + at);
                at += 4u;
            }
            if ((run_flags & UINT32_C(0x000200)) != 0) {
                if (child.end - at < 4u) return false;
                size = mp4_u32(data + at);
                at += 4u;
            }
            if ((run_flags & UINT32_C(0x000400)) != 0) {
                if (child.end - at < 4u) return false;
                sample_flags = mp4_u32(data + at);
                at += 4u;
            }
            if ((run_flags & UINT32_C(0x000800)) != 0) {
                if (child.end - at < 4u) return false;
                uint32_t encoded = mp4_u32(data + at);
                if (version == 0 && encoded > INT32_MAX) return false;
                composition_offset = (int32_t) encoded;
                at += 4u;
            }
            if (!mp4_fragment_record(
                    demux, track, plan, fill, data_offset, size,
                    duration, composition_offset, sample_flags)
                || size > UINT64_MAX - data_offset) return false;
            data_offset += size;
        }
        if (at > child.end) return false;
    }
    /* ISO BMFF permits an explicitly empty fragment.  It advances no media
       time and contributes no sample-table entries, but is not malformed. */
    return found_trun
        || (tfhd_flags & UINT32_C(0x010000)) != 0;
}

static bool mp4_parse_fragment_moof(
    MediaMp4Demux *demux, const unsigned char *data, size_t length,
    uint64_t moof_offset, Mp4FragmentPlan *plans,
    Mp4FragmentFill *fills)
{
    size_t cursor = 0;
    Mp4Box moof;
    if (!mp4_box_next(data, length, &cursor, &moof)
        || moof.type != MEDIA_MP4_FOURCC('m','o','o','f')
        || moof.end != length) return false;
    size_t child_cursor = moof.payload;
    Mp4Box child;
    bool found_traf = false;
    while (mp4_box_next(data, moof.end, &child_cursor, &child)) {
        if (child.type != MEDIA_MP4_FOURCC('t','r','a','f')) continue;
        found_traf = true;
        if (!mp4_parse_fragment_traf(
                demux, data, child, moof_offset, plans, fills))
            return false;
    }
    return found_traf;
}

static void mp4_track_clear_fragment_window(
    MediaMp4Demux *demux, Mp4Track *track)
{
    if (demux == NULL || track == NULL) return;
    budget_free(demux->budget, track->sync_samples);
    budget_free(demux->budget, track->ctts);
    budget_free(demux->budget, track->stts);
    budget_free(demux->budget, track->stsc);
    budget_free(demux->budget, track->chunks);
    budget_free(demux->budget, track->sizes);
    track->sync_samples = NULL;
    track->ctts = NULL;
    track->stts = NULL;
    track->stsc = NULL;
    track->chunks = NULL;
    track->sizes = NULL;
    track->chunk_count = 0;
    track->stsc_count = 0;
    track->stts_count = 0;
    track->ctts_count = 0;
    track->sync_count = 0;
    track->window_sample_count = 0;
    if (track->window_table_bytes <= track->retained_bytes)
        track->retained_bytes -= track->window_table_bytes;
    else
        track->retained_bytes = 0;
    if (track->window_table_bytes <= demux->retained_bytes)
        demux->retained_bytes -= track->window_table_bytes;
    else
        demux->retained_bytes = 0;
    track->window_table_bytes = 0;
}

static bool mp4_fragment_table_bytes(
    const Mp4Track *track, const Mp4FragmentPlan *plan,
    size_t *table_bytes)
{
    if (track == NULL || plan == NULL || table_bytes == NULL)
        return false;
    size_t total = 0;
    if (!mp4_size_accumulate(
            &total, plan->samples, sizeof(*track->sizes))
        || !mp4_size_accumulate(
            &total, plan->samples, sizeof(*track->chunks))
        || !mp4_size_accumulate(&total, 1, sizeof(*track->stsc))
        || !mp4_size_accumulate(
            &total, plan->stts_runs, sizeof(*track->stts))
        || (plan->any_ctts
            && !mp4_size_accumulate(
                &total, plan->ctts_runs, sizeof(*track->ctts)))
        || !mp4_size_accumulate(
            &total, plan->sync_samples,
            sizeof(*track->sync_samples))) {
        return false;
    }
    *table_bytes = total;
    return true;
}

static bool mp4_fragment_window_allocate(
    MediaMp4Demux *demux, Mp4Track *track,
    const Mp4FragmentPlan *plan, size_t track_at)
{
    size_t table_bytes = 0;
    size_t persistent = track->retained_bytes;
    if (!mp4_fragment_table_bytes(track, plan, &table_bytes)
        || table_bytes > demux->limits.maximum_track_table_bytes
        || persistent
            > demux->limits.maximum_track_table_bytes - table_bytes) {
        mp4_fragment_error(
            demux,
            "MP4 fragment window track %zu exceeds table budget "
            "%zu+%zu/%zuB",
            track_at, persistent, table_bytes,
            demux->limits.maximum_track_table_bytes);
        return false;
    }
    track->sizes = mp4_array(
        demux, plan->samples, sizeof(*track->sizes),
        &track->retained_bytes);
    track->chunks = mp4_array(
        demux, plan->samples, sizeof(*track->chunks),
        &track->retained_bytes);
    track->stsc = mp4_array(
        demux, 1, sizeof(*track->stsc), &track->retained_bytes);
    track->stts = mp4_array(
        demux, plan->stts_runs, sizeof(*track->stts),
        &track->retained_bytes);
    if (plan->any_ctts) {
        track->ctts = mp4_array(
            demux, plan->ctts_runs, sizeof(*track->ctts),
            &track->retained_bytes);
    }
    if (plan->sync_samples != 0) {
        track->sync_samples = mp4_array(
            demux, plan->sync_samples, sizeof(*track->sync_samples),
            &track->retained_bytes);
    }
    if (track->sizes == NULL || track->chunks == NULL
        || track->stsc == NULL || track->stts == NULL
        || (plan->any_ctts && track->ctts == NULL)
        || (plan->sync_samples != 0 && track->sync_samples == NULL)) {
        mp4_fragment_error(
            demux, "MP4 fragment window allocation failed for track %zu",
            track_at);
        track->window_table_bytes =
            track->retained_bytes - persistent;
        return false;
    }
    track->window_table_bytes =
        track->retained_bytes - persistent;
    track->window_sample_count = plan->samples;
    track->chunk_count = plan->samples;
    track->stsc_count = 1;
    track->stsc[0] = (Mp4Stsc) {
        .first_chunk = 1, .samples_per_chunk = 1
    };
    track->stts_count = plan->stts_runs;
    track->ctts_count = plan->any_ctts ? plan->ctts_runs : 0;
    track->sync_count = plan->sync_samples;
    return true;
}

static void mp4_sidx_scan_reset(MediaMp4Demux *demux)
{
    demux->sidx_scan_index = SIZE_MAX;
    demux->sidx_scan_cursor = 0;
    demux->sidx_scan_end = 0;
    demux->sidx_scan_moof_offset = 0;
    demux->sidx_scan_moof_length = 0;
    demux->sidx_scan_boxes = 0;
    demux->sidx_scan_found_moof = false;
}

static bool mp4_sidx_segment_moof(
    MediaMp4Demux *demux, size_t index, const Mp4SidxEntry *entry,
    uint64_t *moof_offset, size_t *moof_length, bool may_wait)
{
    if (demux->sidx_scan_index != index) {
        if (entry->length > UINT64_MAX - entry->offset) {
            mp4_fragment_error(demux, "MP4 sidx segment range overflow");
            mp4_sidx_scan_reset(demux);
            return false;
        }
        demux->sidx_scan_index = index;
        demux->sidx_scan_cursor = entry->offset;
        demux->sidx_scan_end = entry->offset + entry->length;
        demux->sidx_scan_moof_offset = 0;
        demux->sidx_scan_moof_length = 0;
        demux->sidx_scan_boxes = 0;
        demux->sidx_scan_found_moof = false;
    }
    while (demux->sidx_scan_cursor <= demux->sidx_scan_end
           && demux->sidx_scan_end - demux->sidx_scan_cursor >= 8u
           && demux->sidx_scan_boxes != 64u) {
        uint64_t cursor = demux->sidx_scan_cursor;
        uint64_t end = demux->sidx_scan_end;
        unsigned char header[16] = {0};
        MediaRangeReadStatus status =
            mp4_reader_read(demux, cursor, header, 8u, may_wait);
        if (status != MEDIA_RANGE_READ_COMPLETE) {
            if (status == MEDIA_RANGE_READ_FAILED)
                mp4_fragment_read_error(demux, cursor, 8u);
            if (status == MEDIA_RANGE_READ_FAILED)
                mp4_sidx_scan_reset(demux);
            return false;
        }
        uint64_t size = mp4_u32(header);
        size_t header_size = 8u;
        if (size == 1) {
            status = end - cursor < 16u
                ? MEDIA_RANGE_READ_FAILED
                : mp4_reader_read(
                      demux, cursor + 8u, header + 8u, 8u, may_wait);
            if (status != MEDIA_RANGE_READ_COMPLETE) {
                if (status == MEDIA_RANGE_READ_FAILED)
                    mp4_fragment_read_error(demux, cursor + 8u, 8u);
                if (status == MEDIA_RANGE_READ_FAILED)
                    mp4_sidx_scan_reset(demux);
                return false;
            }
            size = mp4_u64(header + 8u);
            header_size = 16u;
        } else if (size == 0) {
            size = end - cursor;
        }
        if (size < header_size || size > end - cursor) {
            mp4_fragment_error(
                demux, "MP4 sidx segment box invalid at %llu",
                (unsigned long long) cursor);
            mp4_sidx_scan_reset(demux);
            return false;
        }
        if (mp4_u32(header + 4u)
                == MEDIA_MP4_FOURCC('m','o','o','f')) {
            if (demux->sidx_scan_found_moof) {
                mp4_fragment_error(
                    demux,
                    "MP4 sidx reference contains multiple moof boxes");
                mp4_sidx_scan_reset(demux);
                return false;
            }
            if (size > demux->limits.maximum_metadata_bytes
                || size > SIZE_MAX) {
                mp4_fragment_error(
                    demux, "MP4 lazy moof too large: %llu/%zuB",
                    (unsigned long long) size,
                    demux->limits.maximum_metadata_bytes);
                mp4_sidx_scan_reset(demux);
                return false;
            }
            demux->sidx_scan_moof_offset = cursor;
            demux->sidx_scan_moof_length = (size_t) size;
            demux->sidx_scan_found_moof = true;
        }
        demux->sidx_scan_cursor += size;
        demux->sidx_scan_boxes++;
    }
    if (demux->sidx_scan_cursor != demux->sidx_scan_end) {
        mp4_fragment_error(
            demux, "MP4 sidx segment box scan incomplete at %llu/%llu",
            (unsigned long long) demux->sidx_scan_cursor,
            (unsigned long long) demux->sidx_scan_end);
        mp4_sidx_scan_reset(demux);
        return false;
    }
    if (demux->sidx_scan_found_moof) {
        *moof_offset = demux->sidx_scan_moof_offset;
        *moof_length = demux->sidx_scan_moof_length;
        /* Keep the resolved location across a yielded full-moof read. The
           segment scan and the moof payload can occupy different HTTP cache
           windows; forgetting this result would restart the scan and recreate
           the same A/B eviction cycle one layer later. */
        return true;
    }
    mp4_fragment_error(
        demux, "MP4 sidx segment has no bounded moof at %llu",
        (unsigned long long) entry->offset);
    mp4_sidx_scan_reset(demux);
    return false;
}

/*
 * `may_wait` separates the two callers. Open, seek and rewind are transactional
 * job phases that own a deadline and a cancellation token, and they keep the
 * blocking read they always had. The playing pump does not: it asks, and comes
 * back on the next frame.
 */
static bool mp4_load_sidx_window(
    MediaMp4Demux *demux, size_t index, bool may_wait)
{
    if (demux == NULL || !demux->lazy_fragmented
        || index >= demux->fragment_index_count) return false;
    demux->fragment_error[0] = '\0';
    const Mp4SidxEntry *entry = &demux->fragment_index[index];
    uint64_t moof_offset = 0;
    size_t moof_length = 0;
    if (!mp4_sidx_segment_moof(
            demux, index, entry, &moof_offset, &moof_length,
            may_wait)) return false;
    unsigned char *moof = budget_malloc_category(
        demux->budget, BUDGET_CATEGORY_RESOURCE, moof_length);
    if (moof == NULL) {
        mp4_fragment_error(
            demux, "MP4 lazy moof scratch allocation failed: %zuB",
            moof_length);
        mp4_sidx_scan_reset(demux);
        return false;
    }
    MediaRangeReadStatus moof_status =
        mp4_reader_read(demux, moof_offset, moof, moof_length, may_wait);
    if (moof_status != MEDIA_RANGE_READ_COMPLETE) {
        if (moof_status == MEDIA_RANGE_READ_FAILED)
            mp4_fragment_read_error(demux, moof_offset, moof_length);
        if (moof_status == MEDIA_RANGE_READ_FAILED)
            mp4_sidx_scan_reset(demux);
        budget_free(demux->budget, moof);
        return false;
    }
    mp4_sidx_scan_reset(demux);
    Mp4FragmentPlan plans[MEDIA_MP4_INTERNAL_MAX_TRACKS] = {{0}};
    Mp4FragmentFill fills[MEDIA_MP4_INTERNAL_MAX_TRACKS] = {{0}};
    if (!mp4_parse_fragment_moof(
            demux, moof, moof_length, moof_offset, plans, NULL)) {
        mp4_fragment_error(
            demux, "MP4 lazy fragment plan invalid at %llu",
            (unsigned long long) moof_offset);
        budget_free(demux->budget, moof);
        return false;
    }
    for (size_t at = 0; at < demux->track_count; at++) {
        Mp4Track *track = &demux->tracks[at];
        if (!track->fragmented) continue;
        Mp4FragmentPlan *plan = &plans[at];
        if (plan->samples == 0 || plan->stts_runs == 0
            || !plan->have_decode_time_origin) {
            mp4_fragment_error(
                demux, "MP4 lazy fragment track %zu is incomplete", at);
            budget_free(demux->budget, moof);
            return false;
        }
        uint64_t origin = track->have_fragment_decode_origin
            ? track->fragment_decode_origin
            : plan->decode_time_origin;
        if (plan->decode_time_origin < origin) {
            mp4_fragment_error(
                demux, "MP4 lazy fragment decode time moved backward");
            budget_free(demux->budget, moof);
            return false;
        }
        uint64_t normalized =
            plan->decode_time_origin - origin;
        if (normalized > INT64_MAX
            || plan->duration > (uint64_t) INT64_MAX - normalized
            || plan->maximum_positive_ctts
               > (uint64_t) INT64_MAX
                 - normalized - plan->duration) {
            mp4_fragment_error(
                demux, "MP4 lazy fragment timestamp exceeds signed range");
            budget_free(demux->budget, moof);
            return false;
        }
    }
    /*
     * From this point the old window is no longer recoverable if allocation
     * or the second parse pass fails. Mark it invalid until every table has
     * been filled and checked so callers cannot consume a partial window.
     * rewind/seek can still retry by loading a complete window.
     */
    demux->fragment_index_at = SIZE_MAX;
    for (size_t at = 0; at < demux->track_count; at++) {
        Mp4Track *track = &demux->tracks[at];
        if (!track->fragmented) continue;
        mp4_track_clear_fragment_window(demux, track);
        if (!mp4_fragment_window_allocate(
                demux, track, &plans[at], at)) {
            budget_free(demux->budget, moof);
            return false;
        }
        if (!track->have_fragment_decode_origin) {
            track->fragment_decode_origin =
                plans[at].decode_time_origin;
            track->have_fragment_decode_origin = true;
        }
        track->window_dts_base =
            plans[at].decode_time_origin
            - track->fragment_decode_origin;
    }
    if (!mp4_parse_fragment_moof(
            demux, moof, moof_length, moof_offset, plans, fills)) {
        mp4_fragment_error(
            demux, "MP4 lazy fragment fill invalid at %llu",
            (unsigned long long) moof_offset);
        budget_free(demux->budget, moof);
        return false;
    }
    budget_free(demux->budget, moof);
    for (size_t at = 0; at < demux->track_count; at++) {
        Mp4Track *track = &demux->tracks[at];
        if (!track->fragmented) continue;
        if (fills[at].sample != plans[at].samples
            || fills[at].stts != plans[at].stts_runs
            || fills[at].ctts
                 != (plans[at].any_ctts ? plans[at].ctts_runs : 0)
            || fills[at].sync != plans[at].sync_samples
            || !mp4_validate_chunks(demux, track)) {
            mp4_fragment_error(
                demux, "MP4 lazy fragment table mismatch for track %zu",
                at);
            return false;
        }
    }
    demux->fragment_index_at = index;
    for (size_t at = 0; at < demux->track_count; at++) {
        if (demux->tracks[at].fragmented)
            mp4_track_rewind(&demux->tracks[at]);
    }
    return true;
}

static uint64_t mp4_rescale_time(
    uint64_t value, uint32_t from_scale, uint32_t to_scale)
{
    if (from_scale == 0 || to_scale == 0) return UINT64_MAX;
    uint64_t whole = value / from_scale;
    uint64_t remainder = value % from_scale;
    if (whole > UINT64_MAX / to_scale) return UINT64_MAX;
    if (remainder != 0 && to_scale > UINT64_MAX / remainder)
        return UINT64_MAX;
    uint64_t base = whole * to_scale;
    uint64_t fraction = remainder * to_scale / from_scale;
    return fraction > UINT64_MAX - base
        ? UINT64_MAX : base + fraction;
}

/*
 * Returns 1 when a bounded flat sidx was installed, 0 when no usable sidx is
 * present (the caller may use the eager compatibility path), and -1 for an
 * authenticated but malformed index.
 */
static int mp4_prepare_lazy_sidx(MediaMp4Demux *demux)
{
    uint64_t cursor = 0;
    uint64_t sidx_offset = 0;
    uint64_t sidx_size = 0;
    for (size_t boxes = 0;
         cursor <= demux->reader.length
         && demux->reader.length - cursor >= 8u
         && boxes != 64u;
         boxes++) {
        unsigned char header[16] = {0};
        if (!demux->reader.read(
                demux->reader.opaque, cursor, header, 8u)) {
            mp4_fragment_read_error(demux, cursor, 8u);
            return -1;
        }
        uint64_t size = mp4_u32(header);
        size_t header_size = 8u;
        if (size == 1) {
            if (demux->reader.length - cursor < 16u
                || !demux->reader.read(
                    demux->reader.opaque, cursor + 8u,
                    header + 8u, 8u)) {
                if (demux->reader.length - cursor >= 16u) {
                    mp4_fragment_read_error(
                        demux, cursor + 8u, 8u);
                }
                return -1;
            }
            size = mp4_u64(header + 8u);
            header_size = 16u;
        } else if (size == 0) {
            size = demux->reader.length - cursor;
        }
        if (size < header_size
            || size > demux->reader.length - cursor) return -1;
        uint32_t type = mp4_u32(header + 4u);
        if (type == MEDIA_MP4_FOURCC('s','i','d','x')) {
            sidx_offset = cursor;
            sidx_size = size;
            break;
        }
        if (type == MEDIA_MP4_FOURCC('m','o','o','f')) return 0;
        cursor += size;
    }
    if (sidx_size == 0) return 0;
    if (sidx_size > demux->limits.maximum_metadata_bytes
        || sidx_size > SIZE_MAX) return -1;
    unsigned char *bytes = budget_malloc_category(
        demux->budget, BUDGET_CATEGORY_RESOURCE, (size_t) sidx_size);
    if (bytes == NULL) {
        mp4_fragment_error(
            demux, "MP4 sidx scratch allocation failed: %lluB",
            (unsigned long long) sidx_size);
        return -1;
    }
    if (!demux->reader.read(
            demux->reader.opaque, sidx_offset,
            bytes, (size_t) sidx_size)) {
        mp4_fragment_read_error(demux, sidx_offset, (size_t) sidx_size);
        budget_free(demux->budget, bytes);
        return -1;
    }
    size_t root_cursor = 0;
    Mp4Box box;
    if (!mp4_box_next(
            bytes, (size_t) sidx_size, &root_cursor, &box)
        || box.type != MEDIA_MP4_FOURCC('s','i','d','x')
        || box.end != sidx_size
        || box.end - box.payload < 24u) {
        budget_free(demux->budget, bytes);
        return -1;
    }
    size_t at = box.payload;
    unsigned version = bytes[at];
    if (version > 1) {
        budget_free(demux->budget, bytes);
        return -1;
    }
    at += 8u; /* full box + reference_ID */
    uint32_t timescale = mp4_u32(bytes + at);
    at += 4u;
    uint64_t earliest;
    uint64_t first_offset;
    if (version == 0) {
        if (box.end - at < 12u) {
            budget_free(demux->budget, bytes);
            return -1;
        }
        earliest = mp4_u32(bytes + at);
        first_offset = mp4_u32(bytes + at + 4u);
        at += 8u;
    } else {
        if (box.end - at < 20u) {
            budget_free(demux->budget, bytes);
            return -1;
        }
        earliest = mp4_u64(bytes + at);
        first_offset = mp4_u64(bytes + at + 8u);
        at += 16u;
    }
    if (timescale == 0 || timescale > MEDIA_MP4_MAX_TIMESCALE
        || box.end - at < 4u) {
        budget_free(demux->budget, bytes);
        return -1;
    }
    at += 2u;
    uint16_t count =
        (uint16_t) (((uint16_t) bytes[at] << 8u) | bytes[at + 1u]);
    at += 2u;
    if (count == 0 || count > demux->limits.maximum_table_entries
        || (size_t) count > demux->limits.maximum_metadata_bytes
               / sizeof(Mp4SidxEntry)
        || (size_t) count * 12u > box.end - at) {
        budget_free(demux->budget, bytes);
        return -1;
    }
    size_t index_bytes = (size_t) count * sizeof(Mp4SidxEntry);
    Mp4SidxEntry *entries = budget_calloc_category(
        demux->budget, BUDGET_CATEGORY_RESOURCE,
        count, sizeof(*entries));
    if (entries == NULL) {
        budget_free(demux->budget, bytes);
        return -1;
    }
    uint64_t segment_offset = sidx_offset + sidx_size;
    if (first_offset > demux->reader.length - segment_offset) {
        budget_free(demux->budget, entries);
        budget_free(demux->budget, bytes);
        return -1;
    }
    segment_offset += first_offset;
    uint64_t time = earliest;
    bool valid = true;
    for (uint16_t i = 0; i < count; i++, at += 12u) {
        uint32_t reference = mp4_u32(bytes + at);
        uint32_t length = reference & UINT32_C(0x7fffffff);
        uint32_t duration = mp4_u32(bytes + at + 4u);
        if ((reference & UINT32_C(0x80000000)) != 0
            || length == 0 || duration == 0
            || segment_offset > demux->reader.length
            || length > demux->reader.length - segment_offset
            || duration > UINT64_MAX - time) {
            valid = false;
            break;
        }
        entries[i] = (Mp4SidxEntry) {
            .offset = segment_offset,
            .length = length,
            .start_time = time,
            .duration = duration
        };
        segment_offset += length;
        time += duration;
    }
    budget_free(demux->budget, bytes);
    if (!valid) {
        budget_free(demux->budget, entries);
        return -1;
    }
    demux->fragment_index = entries;
    demux->fragment_index_count = count;
    demux->fragment_index_timescale = timescale;
    demux->retained_bytes += index_bytes;
    demux->lazy_fragmented = true;
    for (size_t i = 0; i < demux->track_count; i++) {
        Mp4Track *track = &demux->tracks[i];
        if (!track->fragmented) continue;
        track->lazy_fragmented = true;
        uint64_t duration = mp4_rescale_time(
            time - earliest, timescale, track->info.timescale);
        if (duration != UINT64_MAX) track->info.duration = duration;
        track->info.sample_count = 0;
    }
    if (!mp4_load_sidx_window(demux, 0, true)) return -1;
    return 1;
}

static void mp4_abandon_lazy_sidx(MediaMp4Demux *demux)
{
    if (demux == NULL) return;
    for (size_t i = 0; i < demux->track_count; i++) {
        Mp4Track *track = &demux->tracks[i];
        if (track->lazy_fragmented)
            mp4_track_clear_fragment_window(demux, track);
        track->lazy_fragmented = false;
        track->have_fragment_decode_origin = false;
        track->fragment_decode_origin = 0;
        track->window_dts_base = 0;
    }
    size_t index_bytes =
        demux->fragment_index_count * sizeof(*demux->fragment_index);
    budget_free(demux->budget, demux->fragment_index);
    if (index_bytes <= demux->retained_bytes)
        demux->retained_bytes -= index_bytes;
    demux->fragment_index = NULL;
    demux->fragment_index_count = 0;
    demux->fragment_index_at = 0;
    demux->fragment_index_timescale = 0;
    demux->lazy_fragmented = false;
    mp4_sidx_scan_reset(demux);
    demux->fragment_error[0] = '\0';
}

static void mp4_fragment_cache_destroy(
    MediaMp4Demux *demux, Mp4FragmentCache *cache)
{
    if (demux == NULL || cache == NULL) return;
    Mp4FragmentBlob *blob = cache->first;
    while (blob != NULL) {
        Mp4FragmentBlob *next = blob->next;
        budget_free(demux->budget, blob->data);
        budget_free(demux->budget, blob);
        blob = next;
    }
    memset(cache, 0, sizeof(*cache));
}

static bool mp4_scan_fragments(
    MediaMp4Demux *demux, Mp4FragmentPlan *plans,
    Mp4FragmentCache *cache)
{
    demux->fragment_error[0] = '\0';
    uint64_t cursor = 0;
    size_t boxes = 0;
    size_t fragments = 0;
    size_t maximum_boxes =
        (size_t) demux->limits.maximum_table_entries * 4u;
    while (cursor <= demux->reader.length
           && demux->reader.length - cursor >= 8u
           && boxes++ < maximum_boxes) {
        unsigned char header[16];
        if (!demux->reader.read(demux->reader.opaque, cursor, header, 8u)) {
            mp4_fragment_read_error(demux, cursor, 8u);
            return false;
        }
        uint64_t size = mp4_u32(header);
        size_t header_size = 8u;
        if (size == 1) {
            if (demux->reader.length - cursor < 16u
                || !demux->reader.read(
                    demux->reader.opaque, cursor + 8u, header + 8u, 8u)) {
                mp4_fragment_read_error(demux, cursor + 8u, 8u);
                return false;
            }
            size = mp4_u64(header + 8u);
            header_size = 16u;
        } else if (size == 0) {
            size = demux->reader.length - cursor;
        }
        if (size < header_size || size > demux->reader.length - cursor)
            return false;
        if (mp4_u32(header + 4u)
                == MEDIA_MP4_FOURCC('m','o','o','f')) {
            ++fragments;
            if (fragments > demux->limits.maximum_table_entries) {
                mp4_fragment_error(
                    demux, "MP4 fragment count limit reached: %zu/%u",
                    fragments,
                    demux->limits.maximum_table_entries);
                return false;
            }
            if (size > demux->limits.maximum_metadata_bytes
                || size > SIZE_MAX
                || cache == NULL
                || demux->limits.maximum_metadata_bytes
                     < sizeof(Mp4FragmentBlob)
                || cache->index_bytes
                     > demux->limits.maximum_metadata_bytes
                         - sizeof(Mp4FragmentBlob)) {
                mp4_fragment_error(
                    demux,
                    "MP4 fragment metadata/index limit reached: "
                    "box=%lluB index=%zu/%zuB",
                    (unsigned long long) size,
                    cache == NULL ? 0 : cache->index_bytes,
                    demux->limits.maximum_metadata_bytes);
                return false;
            }
            Mp4FragmentBlob *blob = budget_calloc_category(
                demux->budget, BUDGET_CATEGORY_RESOURCE,
                1, sizeof(*blob));
            if (blob == NULL) {
                mp4_fragment_error(
                    demux, "MP4 fragment index allocation failed: %zuB",
                    sizeof(*blob));
                return false;
            }
            bool retain_blob =
                cache->bytes
                    <= demux->limits.maximum_metadata_bytes - (size_t) size;
            unsigned char *fragment = budget_malloc_category(
                demux->budget, BUDGET_CATEGORY_RESOURCE, (size_t) size);
            if (fragment == NULL) {
                mp4_fragment_error(
                    demux, "MP4 fragment scratch allocation failed: %lluB",
                    (unsigned long long) size);
                budget_free(demux->budget, blob);
                return false;
            }
            if (!demux->reader.read(
                    demux->reader.opaque, cursor, fragment, (size_t) size)) {
                mp4_fragment_read_error(demux, cursor, (size_t) size);
                budget_free(demux->budget, fragment);
                budget_free(demux->budget, blob);
                return false;
            }
            bool parsed = mp4_parse_fragment_moof(
                demux, fragment, (size_t) size, cursor, plans, NULL);
            if (!parsed) {
                mp4_fragment_error(
                    demux, "MP4 fragment structure invalid at offset %llu",
                    (unsigned long long) cursor);
                budget_free(demux->budget, fragment);
                budget_free(demux->budget, blob);
                return false;
            }
            blob->next = NULL;
            blob->offset = cursor;
            blob->length = (size_t) size;
            blob->data = retain_blob ? fragment : NULL;
            if (!retain_blob) budget_free(demux->budget, fragment);
            if (cache->last == NULL)
                cache->first = blob;
            else
                cache->last->next = blob;
            cache->last = blob;
            cache->index_bytes += sizeof(*blob);
            if (retain_blob) cache->bytes += (size_t) size;
        }
        cursor += size;
    }
    if (fragments == 0 || cursor != demux->reader.length) {
        mp4_fragment_error(
            demux,
            "MP4 fragment top-level scan incomplete: fragments=%zu "
            "offset=%llu/%llu boxes=%zu/%zu",
            fragments, (unsigned long long) cursor,
            (unsigned long long) demux->reader.length,
            boxes, maximum_boxes);
        return false;
    }
    return true;
}

static bool mp4_fill_cached_fragments(
    MediaMp4Demux *demux, Mp4FragmentPlan *plans,
    Mp4FragmentFill *fills, const Mp4FragmentCache *cache)
{
    if (cache == NULL || cache->first == NULL) return false;
    for (const Mp4FragmentBlob *blob = cache->first;
         blob != NULL; blob = blob->next) {
        unsigned char *temporary = NULL;
        const unsigned char *data = blob->data;
        if (data == NULL) {
            temporary = budget_malloc_category(
                demux->budget, BUDGET_CATEGORY_RESOURCE, blob->length);
            if (temporary == NULL) {
                mp4_fragment_error(
                    demux, "MP4 fragment fill scratch allocation failed: %zuB",
                    blob->length);
                return false;
            }
            if (!demux->reader.read(
                    demux->reader.opaque, blob->offset,
                    temporary, blob->length)) {
                mp4_fragment_read_error(
                    demux, blob->offset, blob->length);
                budget_free(demux->budget, temporary);
                return false;
            }
            data = temporary;
        }
        if (!mp4_parse_fragment_moof(
                demux, data, blob->length, blob->offset,
                plans, fills)) {
            mp4_fragment_error(
                demux, "MP4 fragment fill invalid at offset %llu",
                (unsigned long long) blob->offset);
            budget_free(demux->budget, temporary);
            return false;
        }
        budget_free(demux->budget, temporary);
    }
    return true;
}

static bool mp4_prepare_fragmented_tracks(
    MediaMp4Demux *demux, char *error, size_t error_size)
{
    Mp4FragmentPlan plans[MEDIA_MP4_INTERNAL_MAX_TRACKS] = {{0}};
    Mp4FragmentFill fills[MEDIA_MP4_INTERNAL_MAX_TRACKS] = {{0}};
    Mp4FragmentCache cache = {0};
    bool ok = false;
    if (demux->track_count > MEDIA_MP4_INTERNAL_MAX_TRACKS) {
        mp4_error(error, error_size, "MP4 fragments: too many tracks");
        goto done;
    }
    if (!mp4_scan_fragments(demux, plans, &cache)) {
        mp4_error(
            error, error_size, "%s",
            demux->fragment_error[0] == '\0'
                ? "MP4 fragments: plan scan failed"
                : demux->fragment_error);
        goto done;
    }
    for (size_t at = 0; at < demux->track_count; at++) {
        Mp4Track *track = &demux->tracks[at];
        Mp4FragmentPlan *plan = &plans[at];
        if (!track->fragmented) continue;
        if (plan->samples == 0
            || plan->stts_runs == 0
            || (track->info.kind == MEDIA_MP4_TRACK_VIDEO
                && plan->sync_samples == 0)
            || plan->samples > demux->limits.maximum_chunks_per_track
            || plan->duration > INT64_MAX
            || plan->maximum_positive_ctts
               > (uint64_t) INT64_MAX - plan->duration)
        {
            mp4_error(
                error, error_size,
                "MP4 fragments: invalid track plan (%zu)", at);
            goto done;
        }
        size_t table_bytes = 0;
        if (!mp4_fragment_table_bytes(track, plan, &table_bytes)
            || table_bytes > demux->limits.maximum_track_table_bytes
            || track->retained_bytes
                > demux->limits.maximum_track_table_bytes - table_bytes) {
            mp4_error(
                error, error_size,
                "MP4 fragments: track %zu table budget "
                "%zu+%zu/%zuB (%u samples)",
                at, track->retained_bytes, table_bytes,
                demux->limits.maximum_track_table_bytes,
                plan->samples);
            goto done;
        }
        track->sizes = mp4_array(
            demux, plan->samples, sizeof(*track->sizes),
            &track->retained_bytes);
        track->chunks = mp4_array(
            demux, plan->samples, sizeof(*track->chunks),
            &track->retained_bytes);
        track->stsc = mp4_array(
            demux, 1, sizeof(*track->stsc), &track->retained_bytes);
        track->stts = mp4_array(
            demux, plan->stts_runs, sizeof(*track->stts),
            &track->retained_bytes);
        if (plan->any_ctts) {
            track->ctts = mp4_array(
                demux, plan->ctts_runs, sizeof(*track->ctts),
                &track->retained_bytes);
        }
        if (plan->sync_samples != 0) {
            track->sync_samples = mp4_array(
                demux, plan->sync_samples,
                sizeof(*track->sync_samples), &track->retained_bytes);
        }
        if (track->sizes == NULL || track->chunks == NULL
            || track->stsc == NULL || track->stts == NULL
            || (plan->any_ctts && track->ctts == NULL)
            || (plan->sync_samples != 0
                && track->sync_samples == NULL)) {
            mp4_error(
                error, error_size,
                "MP4 fragments: sample-table allocation (%zu)", at);
            goto done;
        }
        track->info.sample_count = plan->samples;
        track->info.duration = plan->duration;
        track->chunk_count = plan->samples;
        track->stsc_count = 1;
        track->stsc[0] = (Mp4Stsc) {
            .first_chunk = 1, .samples_per_chunk = 1
        };
        track->stts_count = plan->stts_runs;
        track->ctts_count = plan->any_ctts ? plan->ctts_runs : 0;
        track->sync_count = plan->sync_samples;
    }
    if (!mp4_fill_cached_fragments(demux, plans, fills, &cache)) {
        mp4_error(
            error, error_size, "%s",
            demux->fragment_error[0] == '\0'
                ? "MP4 fragments: fill scan failed"
                : demux->fragment_error);
        goto done;
    }
    for (size_t at = 0; at < demux->track_count; at++) {
        Mp4Track *track = &demux->tracks[at];
        if (!track->fragmented) continue;
        if (fills[at].sample != plans[at].samples
            || fills[at].stts != plans[at].stts_runs
            || fills[at].ctts
                 != (plans[at].any_ctts ? plans[at].ctts_runs : 0)
            || fills[at].sync != plans[at].sync_samples
            || !mp4_validate_chunks(demux, track)) {
            mp4_error(
                error, error_size,
                "MP4 fragments: filled table mismatch (%zu)", at);
            goto done;
        }
    }
    ok = true;
done:
    mp4_fragment_cache_destroy(demux, &cache);
    return ok;
}

static void mp4_track_rewind(Mp4Track *track)
{
    track->sample = 0;
    track->chunk = 0;
    track->sample_in_chunk = 0;
    track->stsc_index = 0;
    track->stts_index = 0;
    track->stts_remaining = track->stts_count == 0
        ? 0 : track->stts[0].count;
    track->ctts_index = 0;
    track->ctts_remaining = track->ctts_count == 0
        ? 0 : track->ctts[0].count;
    track->sync_index = 0;
    track->byte_offset = track->chunk_count == 0 ? 0 : track->chunks[0];
    track->dts = track->lazy_fragmented
        ? track->window_dts_base : 0;
}

static void mp4_track_free(MediaMp4Demux *demux, Mp4Track *track)
{
    budget_free(demux->budget, track->codec_config);
    budget_free(demux->budget, track->sync_samples);
    budget_free(demux->budget, track->ctts);
    budget_free(demux->budget, track->stts);
    budget_free(demux->budget, track->stsc);
    budget_free(demux->budget, track->chunks);
    budget_free(demux->budget, track->sizes);
    memset(track, 0, sizeof(*track));
}

MediaMp4Limits media_mp4_default_limits(void)
{
    return (MediaMp4Limits) {
        .maximum_metadata_bytes = 1024u * 1024u,
        .maximum_track_table_bytes = 2u * 1024u * 1024u,
        .maximum_tracks = 4,
        .maximum_samples_per_track = 174000,
        .maximum_chunks_per_track = 174000,
        /*
         * Fragmented H.264 with B-frames can carry nearly one composition
         * offset run per sample. An observed 213-second 360p rendition
         * contains 4,228 valid runs, so the former 4,096 ceiling rejected it
         * despite using only about 34 KiB for that table. Allocations remain
         * sized to observed runs, not this admission ceiling.
         */
        .maximum_table_entries = 131072,
        .maximum_sample_bytes = 1024u * 1024u
    };
}

MediaMp4Demux *media_mp4_open(
    Budget *budget, const MediaRangeReader *reader,
    const MediaMp4Limits *limits, char *error, size_t error_size)
{
    if (error != NULL && error_size != 0) error[0] = '\0';
    if (budget == NULL || reader == NULL || reader->read == NULL
        || reader->length < 8u) {
        mp4_error(error, error_size, "MP4: invalid reader");
        return NULL;
    }
    MediaMp4Limits chosen = limits == NULL
        ? media_mp4_default_limits() : *limits;
    if (chosen.maximum_metadata_bytes < 8u
        || chosen.maximum_track_table_bytes < 1024u
        || chosen.maximum_tracks == 0
        || chosen.maximum_tracks > MEDIA_MP4_INTERNAL_MAX_TRACKS
        || chosen.maximum_samples_per_track == 0
        || chosen.maximum_chunks_per_track == 0
        || chosen.maximum_table_entries == 0
        || chosen.maximum_sample_bytes == 0
        || (uint64_t) chosen.maximum_table_entries * 4u > SIZE_MAX
        || (uint64_t) chosen.maximum_samples_per_track
             * (sizeof(uint32_t) + sizeof(uint64_t)) > SIZE_MAX) {
        mp4_error(error, error_size, "MP4: invalid limits");
        return NULL;
    }
    uint64_t cursor = 0;
    uint64_t moov_offset = 0;
    uint64_t moov_size = 0;
    size_t top_level_boxes = 0;
    while (cursor <= reader->length
           && reader->length - cursor >= 8u
           && top_level_boxes++ < MEDIA_MP4_MAX_TOP_LEVEL_BOXES) {
        unsigned char header[16];
        if (!reader->read(reader->opaque, cursor, header, 8u)) {
            mp4_reader_error(
                reader, error, error_size, "top-level",
                cursor, 8u);
            return NULL;
        }
        uint64_t size = mp4_u32(header);
        size_t header_size = 8;
        if (size == 1) {
            if (reader->length - cursor < 16u
                || !reader->read(reader->opaque, cursor + 8u,
                                 header + 8u, 8u)) {
                if (reader->length - cursor < 16u) {
                    mp4_error(
                        error, error_size,
                        "MP4 extended box truncated");
                } else {
                    mp4_reader_error(
                        reader, error, error_size,
                        "extended-box", cursor + 8u, 8u);
                }
                return NULL;
            }
            size = mp4_u64(header + 8);
            header_size = 16;
        } else if (size == 0) {
            size = reader->length - cursor;
        }
        if (size < header_size || size > reader->length - cursor) {
            mp4_error(error, error_size, "MP4 top box invalid");
            return NULL;
        }
        if (mp4_u32(header + 4) == MEDIA_MP4_FOURCC('m','o','o','v')) {
            moov_offset = cursor;
            moov_size = size;
            break;
        }
        cursor += size;
    }
    if (moov_size == 0 || moov_size > chosen.maximum_metadata_bytes
        || moov_size > SIZE_MAX) {
        mp4_error(error, error_size,
                  "MP4 moov missing/>%zuB",
                  chosen.maximum_metadata_bytes);
        return NULL;
    }
    MediaMp4Demux *demux = budget_calloc_category(
        budget, BUDGET_CATEGORY_NAVIGATION, 1, sizeof(*demux));
    if (demux == NULL) {
        mp4_error(error, error_size, "MP4 state budget");
        return NULL;
    }
    demux->budget = budget;
    demux->reader = *reader;
    demux->limits = chosen;
    demux->sidx_scan_index = SIZE_MAX;
    unsigned char *moov = budget_malloc_category(
        budget, BUDGET_CATEGORY_RESOURCE, (size_t) moov_size);
    demux->tracks = mp4_array(
        demux, chosen.maximum_tracks, sizeof(*demux->tracks), NULL);
    if (moov == NULL || demux->tracks == NULL) {
        budget_free(budget, moov);
        mp4_error(error, error_size, "MP4 metadata allocation failed");
        media_mp4_close(demux);
        return NULL;
    }
    if (!reader->read(
            reader->opaque, moov_offset, moov, (size_t) moov_size)) {
        budget_free(budget, moov);
        mp4_reader_error(
            reader, error, error_size, "metadata",
            moov_offset, (size_t) moov_size);
        media_mp4_close(demux);
        return NULL;
    }
    demux->retained_bytes += (size_t) moov_size;
    Mp4Box root;
    size_t root_cursor = 0;
    if (!mp4_box_next(moov, (size_t) moov_size, &root_cursor, &root)
        || root.type != MEDIA_MP4_FOURCC('m','o','o','v')) {
        budget_free(budget, moov);
        mp4_error(error, error_size, "MP4 moov invalid");
        media_mp4_close(demux);
        return NULL;
    }
    size_t child_cursor = root.payload;
    Mp4Box child;
    bool parse_error = false;
    while (mp4_box_next(moov, root.end, &child_cursor, &child)) {
        if (child.type != MEDIA_MP4_FOURCC('t','r','a','k')) continue;
        if (demux->track_count >= chosen.maximum_tracks) {
            parse_error = true;
            break;
        }
        if (!mp4_parse_track(
                demux, moov, child,
                &demux->tracks[demux->track_count])) {
            mp4_track_free(
                demux, &demux->tracks[demux->track_count]);
            parse_error = true;
            break;
        }
        demux->track_count++;
    }
    if (!parse_error
        && !mp4_parse_fragment_defaults(demux, moov, root)) {
        parse_error = true;
    }
    budget_free(budget, moov);
    demux->retained_bytes -= (size_t) moov_size;
    if (parse_error || demux->track_count == 0) {
        mp4_error(error, error_size,
                  "MP4 sample tables invalid");
        media_mp4_close(demux);
        return NULL;
    }
    bool fragmented = false;
    for (size_t at = 0; at < demux->track_count; at++) {
        fragmented = fragmented || demux->tracks[at].fragmented;
    }
    if (fragmented) {
        int lazy = media_mp4_lazy_sidx_admitted(demux->track_count)
            ? mp4_prepare_lazy_sidx(demux) : 0;
        if (lazy < 0) {
            /*
             * A sidx is an optimization, never the sole authority for valid
             * media. Preserve compatibility with fragmented MP4s whose index
             * is absent or outside Tilefinch's flat bounded subset.
             */
            mp4_abandon_lazy_sidx(demux);
        }
        if (lazy <= 0
            && !mp4_prepare_fragmented_tracks(
                demux, error, error_size)) {
            media_mp4_close(demux);
            return NULL;
        }
    }
    media_mp4_rewind(demux);
    return demux;
}

size_t media_mp4_track_count(const MediaMp4Demux *demux)
{
    return demux == NULL ? 0 : demux->track_count;
}

bool media_mp4_track_info(const MediaMp4Demux *demux, size_t index,
                          MediaMp4TrackInfo *info)
{
    if (demux == NULL || info == NULL || index >= demux->track_count)
        return false;
    *info = demux->tracks[index].info;
    return true;
}

static bool mp4_track_peek(const Mp4Track *track, size_t track_index,
                           MediaMp4Sample *sample)
{
    uint32_t sample_count = track->lazy_fragmented
        ? track->window_sample_count : track->info.sample_count;
    if (track->sample >= sample_count) return false;
    uint32_t duration = track->stts[track->stts_index].delta;
    int64_t composition = track->ctts_count == 0 ? 0
        : track->ctts[track->ctts_index].offset;
    if (track->dts > INT64_MAX
        || (composition > 0
            && (uint64_t) composition
               > (uint64_t) INT64_MAX - track->dts)) {
        return false;
    }
    bool keyframe = track->info.kind != MEDIA_MP4_TRACK_VIDEO
        || (!track->fragmented && track->sync_count == 0)
        || (track->sync_index < track->sync_count
            && track->sync_samples[track->sync_index]
               == track->sample + 1u);
    *sample = (MediaMp4Sample) {
        .track_index = track_index,
        .kind = track->info.kind,
        .offset = track->byte_offset,
        .size = track->sizes[track->sample],
        .dts = track->dts,
        .pts = composition < 0
            && (uint64_t) -composition > track->dts
              ? 0 : (int64_t) track->dts + composition,
        .duration = duration,
        .timescale = track->info.timescale,
        .keyframe = keyframe
    };
    return true;
}

static void mp4_track_advance(Mp4Track *track)
{
    uint32_t size = track->sizes[track->sample];
    uint32_t duration = track->stts[track->stts_index].delta;
    track->byte_offset += size;
    track->sample++;
    track->sample_in_chunk++;
    track->dts += duration;
    if (--track->stts_remaining == 0
        && track->stts_index + 1u < track->stts_count) {
        track->stts_index++;
        track->stts_remaining = track->stts[track->stts_index].count;
    }
    if (track->ctts_count != 0 && --track->ctts_remaining == 0
        && track->ctts_index + 1u < track->ctts_count) {
        track->ctts_index++;
        track->ctts_remaining = track->ctts[track->ctts_index].count;
    }
    if (track->sync_index < track->sync_count
        && track->sync_samples[track->sync_index] == track->sample)
        track->sync_index++;
    uint32_t per_chunk = track->stsc[track->stsc_index].samples_per_chunk;
    if (track->sample_in_chunk == per_chunk
        && track->chunk + 1u < track->chunk_count) {
        track->chunk++;
        track->sample_in_chunk = 0;
        while (track->stsc_index + 1u < track->stsc_count
               && track->chunk + 1u
                  >= track->stsc[track->stsc_index + 1u].first_chunk)
            track->stsc_index++;
        track->byte_offset = track->chunks[track->chunk];
    }
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline, cold))
#endif
static void mp4_timestamp_parts_wide(
    uint64_t value, uint32_t timescale,
    uint64_t *whole, uint32_t *remainder)
{
    *whole = value / timescale;
    *remainder = (uint32_t) (value % timescale);
}

static void mp4_timestamp_parts(
    uint64_t value, uint32_t timescale,
    uint64_t *whole, uint32_t *remainder)
{
    /* Ordinary MP4 decode timestamps stay within 32 bits for many hours at
       web-media timescales. Allegrex has hardware 32-bit division but lowers
       every 64-bit divide/modulo to software helpers, so keep that rare
       long-duration fallback out of the per-sample ordering path. */
    if (value <= UINT32_MAX) {
        uint32_t small = (uint32_t) value;
        *whole = small / timescale;
        *remainder = small % timescale;
        return;
    }
    mp4_timestamp_parts_wide(value, timescale, whole, remainder);
}

bool media_mp4_next_sample(MediaMp4Demux *demux, MediaMp4Sample *sample)
{
    if (demux == NULL || sample == NULL) return false;
    demux->runtime_would_block = false;
    if (demux->lazy_fragmented
        && demux->fragment_index_at == SIZE_MAX) {
        demux->runtime_failed = true;
        snprintf(
            demux->runtime_error, sizeof(demux->runtime_error),
            "MP4 fragment window is invalid after an interrupted load");
        return false;
    }
    for (;;) {
        bool found = false;
        MediaMp4Sample earliest;
        for (size_t i = 0; i < demux->track_count; i++) {
            MediaMp4Sample candidate;
            if (!mp4_track_peek(&demux->tracks[i], i, &candidate)) continue;
            uint64_t candidate_whole = 0, earliest_whole = 0;
            uint32_t candidate_remainder = 0, earliest_remainder = 0;
            if (found) {
                mp4_timestamp_parts(
                    candidate.dts, candidate.timescale,
                    &candidate_whole, &candidate_remainder);
                mp4_timestamp_parts(
                    earliest.dts, earliest.timescale,
                    &earliest_whole, &earliest_remainder);
            }
            bool candidate_before = found
                && (candidate_whole < earliest_whole
                    || (candidate_whole == earliest_whole
                        && (uint64_t) candidate_remainder * earliest.timescale
                           < (uint64_t) earliest_remainder
                               * candidate.timescale));
            bool candidate_equal = found
                && candidate_whole == earliest_whole
                && (uint64_t) candidate_remainder * earliest.timescale
                   == (uint64_t) earliest_remainder * candidate.timescale;
            if (!found || candidate_before
                || (candidate_equal
                    && candidate.track_index < earliest.track_index)) {
                earliest = candidate;
                found = true;
            }
        }
        if (found) {
            *sample = earliest;
            mp4_track_advance(&demux->tracks[earliest.track_index]);
            return true;
        }
        if (!demux->lazy_fragmented
            || demux->fragment_index_at + 1u
               >= demux->fragment_index_count) {
            return false;
        }
        if (!mp4_load_sidx_window(
                demux, demux->fragment_index_at + 1u, false)) {
            /* The window's own bytes have not arrived. The fragment cursor is
               untouched, so the next pump asks for exactly this window again;
               reporting it as a failure would end the stream instead. */
            if (demux->runtime_would_block) return false;
            demux->runtime_failed = true;
            snprintf(
                demux->runtime_error, sizeof(demux->runtime_error),
                "%s", demux->fragment_error[0] == '\0'
                    ? "MP4 lazy fragment load failed"
                    : demux->fragment_error);
            return false;
        }
        demux->runtime_failed = false;
        demux->runtime_error[0] = '\0';
    }
}

static bool media_mp4_read_sample_bounded(MediaMp4Demux *demux,
                           const MediaMp4Sample *sample,
                           void *destination, size_t capacity,
                           bool may_wait)
{
    if (demux == NULL) return false;
    demux->runtime_failed = false;
    demux->runtime_would_block = false;
    demux->runtime_error[0] = '\0';
    if (sample == NULL || destination == NULL
        || sample->track_index >= demux->track_count
        || sample->size > capacity
        || sample->offset > demux->reader.length
        || sample->size > demux->reader.length - sample->offset) {
        demux->runtime_failed = true;
        snprintf(
            demux->runtime_error, sizeof(demux->runtime_error),
            "MP4 sample request is outside its bounded source");
        return false;
    }
    MediaRangeReadStatus status = mp4_reader_read(
        demux, sample->offset, destination, sample->size, may_wait);
    if (status == MEDIA_RANGE_READ_COMPLETE) return true;
    /* Unbuffered is not failed. The sample stays selected in the caller's
       pending slot, so the next pump reads exactly this payload again. */
    if (status == MEDIA_RANGE_READ_WOULD_BLOCK) return false;
    char detail[160] = {0};
    if (demux->reader.describe_failure != NULL) {
        (void) demux->reader.describe_failure(
            demux->reader.opaque, detail, sizeof(detail));
    }
    demux->runtime_failed = true;
    snprintf(
        demux->runtime_error, sizeof(demux->runtime_error),
        "MP4 sample range read failed at offset %llu (%uB)%s%s",
        (unsigned long long) sample->offset, (unsigned) sample->size,
        detail[0] == '\0' ? "" : ": ",
        detail[0] == '\0' ? "" : detail);
    return false;
}

bool media_mp4_read_sample(MediaMp4Demux *demux,
                           const MediaMp4Sample *sample,
                           void *destination, size_t capacity)
{
    return media_mp4_read_sample_bounded(
        demux, sample, destination, capacity, false);
}

bool media_mp4_read_sample_waiting(MediaMp4Demux *demux,
                           const MediaMp4Sample *sample,
                           void *destination, size_t capacity)
{
    return media_mp4_read_sample_bounded(
        demux, sample, destination, capacity, true);
}

static uint64_t mp4_sample_time_us(const MediaMp4Sample *sample)
{
    if (sample->timescale == 0) return UINT64_MAX;
    uint64_t whole = sample->dts / sample->timescale;
    uint64_t remainder = sample->dts % sample->timescale;
    if (whole > UINT64_MAX / UINT64_C(1000000)) return UINT64_MAX;
    uint64_t base = whole * UINT64_C(1000000);
    uint64_t fraction =
        remainder * UINT64_C(1000000) / sample->timescale;
    return fraction > UINT64_MAX - base
        ? UINT64_MAX : base + fraction;
}

static uint64_t mp4_time_units_from_us(uint64_t microseconds,
                                       uint32_t timescale)
{
    if (timescale == 0) return UINT64_MAX;
    uint64_t whole = microseconds / UINT64_C(1000000);
    uint64_t remainder = microseconds % UINT64_C(1000000);
    if (whole > UINT64_MAX / timescale) return UINT64_MAX;
    uint64_t base = whole * timescale;
    uint64_t fraction =
        remainder * timescale / UINT64_C(1000000);
    return fraction > UINT64_MAX - base
        ? UINT64_MAX : base + fraction;
}

static bool media_mp4_seek_internal(MediaMp4Demux *demux,
                                    uint64_t target_us,
                                    bool strictly_after,
                                    uint64_t *actual_us)
{
    if (demux == NULL) return false;
    demux->runtime_failed = false;
    demux->runtime_error[0] = '\0';
    if (demux->lazy_fragmented) {
        uint64_t target_index_units = mp4_time_units_from_us(
            target_us, demux->fragment_index_timescale);
        uint64_t first_time = demux->fragment_index[0].start_time;
        if (target_index_units > UINT64_MAX - first_time)
            target_index_units = UINT64_MAX;
        else
            target_index_units += first_time;
        size_t selected = 0;
        for (size_t i = 1; i < demux->fragment_index_count; i++) {
            if (demux->fragment_index[i].start_time
                > target_index_units) break;
            selected = i;
        }
        if (!mp4_load_sidx_window(demux, selected, true)) {
            demux->runtime_failed = true;
            snprintf(
                demux->runtime_error, sizeof(demux->runtime_error),
                "%s", demux->fragment_error[0] == '\0'
                    ? "MP4 lazy seek fragment load failed"
                    : demux->fragment_error);
            return false;
        }
    } else {
        media_mp4_rewind(demux);
    }
    size_t video_index = SIZE_MAX;
    for (size_t i = 0; i < demux->track_count; i++) {
        if (demux->tracks[i].info.kind == MEDIA_MP4_TRACK_VIDEO) {
            video_index = i;
            break;
        }
    }
    uint64_t selected_us = 0;
    if (video_index != SIZE_MAX && strictly_after) {
        uint64_t target_dts = mp4_time_units_from_us(
            target_us, demux->tracks[video_index].info.timescale);
        bool found_keyframe = false;
        uint64_t selected_dts = 0;
        for (;;) {
            Mp4Track *video = &demux->tracks[video_index];
            MediaMp4Sample sample;
            while (mp4_track_peek(video, video_index, &sample)) {
                if (sample.keyframe && sample.dts > target_dts) {
                    selected_dts = sample.dts;
                    found_keyframe = true;
                    break;
                }
                mp4_track_advance(video);
            }
            if (found_keyframe) break;
            if (!demux->lazy_fragmented
                || demux->fragment_index_at + 1u
                       >= demux->fragment_index_count) break;
            if (!mp4_load_sidx_window(
                    demux, demux->fragment_index_at + 1u, true)) {
                demux->runtime_failed = true;
                snprintf(
                    demux->runtime_error, sizeof(demux->runtime_error),
                    "%s", demux->fragment_error[0] == '\0'
                        ? "MP4 lazy forward keyframe load failed"
                        : demux->fragment_error);
                return false;
            }
        }
        if (!found_keyframe) return false;
        MediaMp4Sample selected_sample = {
            .dts = selected_dts,
            .timescale = demux->tracks[video_index].info.timescale
        };
        selected_us = mp4_sample_time_us(&selected_sample);
    } else if (video_index != SIZE_MAX) {
        uint64_t selected_dts = 0;
        for (;;) {
            Mp4Track *video = &demux->tracks[video_index];
            Mp4Track beginning = *video;
            Mp4Track selected = beginning;
            MediaMp4Sample sample;
            uint64_t target_dts = mp4_time_units_from_us(
                target_us, video->info.timescale);
            selected_dts = beginning.dts;
            bool found_keyframe = false;
            while (mp4_track_peek(video, video_index, &sample)) {
                if (sample.dts > target_dts) break;
                if (sample.keyframe) {
                    selected = *video;
                    selected_dts = sample.dts;
                    found_keyframe = true;
                }
                mp4_track_advance(video);
            }
            if (found_keyframe || !demux->lazy_fragmented
                || demux->fragment_index_at == 0) {
                *video = selected;
                break;
            }
            /*
             * starts_with_SAP is advisory metadata. Verify the actual sample
             * flags and walk backward when an index is optimistic or when a
             * valid segment begins with inter-predicted frames.
             */
            if (!mp4_load_sidx_window(
                    demux, demux->fragment_index_at - 1u, true)) {
                demux->runtime_failed = true;
                snprintf(
                    demux->runtime_error, sizeof(demux->runtime_error),
                    "%s", demux->fragment_error[0] == '\0'
                        ? "MP4 lazy keyframe backtrack failed"
                        : demux->fragment_error);
                return false;
            }
        }
        MediaMp4Sample selected_sample = {
            .dts = selected_dts,
            .timescale = demux->tracks[video_index].info.timescale
        };
        selected_us = mp4_sample_time_us(&selected_sample);
    } else {
        selected_us = target_us;
    }
    for (size_t i = 0; i < demux->track_count; i++) {
        if (i == video_index) continue;
        Mp4Track *track = &demux->tracks[i];
        mp4_track_rewind(track);
        Mp4Track selected = *track;
        MediaMp4Sample sample;
        uint64_t selected_dts = mp4_time_units_from_us(
            selected_us, track->info.timescale);
        while (mp4_track_peek(track, i, &sample)) {
            if (sample.dts > selected_dts) break;
            selected = *track;
            mp4_track_advance(track);
        }
        *track = selected;
    }
    if (actual_us != NULL) *actual_us = selected_us;
    return true;
}

bool media_mp4_seek_us(MediaMp4Demux *demux, uint64_t target_us,
                       uint64_t *actual_us)
{
    return media_mp4_seek_internal(demux, target_us, false, actual_us);
}

bool media_mp4_seek_after_us(MediaMp4Demux *demux, uint64_t target_us,
                             uint64_t *actual_us)
{
    return media_mp4_seek_internal(demux, target_us, true, actual_us);
}

void media_mp4_rewind(MediaMp4Demux *demux)
{
    if (demux == NULL) return;
    demux->runtime_failed = false;
    demux->runtime_error[0] = '\0';
    if (demux->lazy_fragmented && demux->fragment_index_at != 0) {
        if (!mp4_load_sidx_window(demux, 0, true)) {
            demux->runtime_failed = true;
            snprintf(
                demux->runtime_error, sizeof(demux->runtime_error),
                "%s", demux->fragment_error[0] == '\0'
                    ? "MP4 lazy rewind fragment load failed"
                    : demux->fragment_error);
            return;
        }
    }
    for (size_t i = 0; i < demux->track_count; i++)
        mp4_track_rewind(&demux->tracks[i]);
}

bool media_mp4_would_block(const MediaMp4Demux *demux)
{
    return demux != NULL && demux->runtime_would_block
        && !demux->runtime_failed;
}

/*
 * Deliberately does not touch runtime_would_block. That flag is the record of
 * a read this pump actually attempted, and the caller's control flow turns on
 * it; a question about a sample nobody read must not be able to answer it.
 */
bool media_mp4_sample_resident(const MediaMp4Demux *demux,
                               const MediaMp4Sample *sample)
{
    if (demux == NULL || sample == NULL) return false;
    if (sample->offset > demux->reader.length
        || sample->size > demux->reader.length - sample->offset) return false;
    if (demux->reader.resident == NULL) return true;
    return demux->reader.resident(
        demux->reader.opaque, sample->offset, sample->size);
}

bool media_mp4_last_error(const MediaMp4Demux *demux,
                          char *error, size_t error_size)
{
    if (demux == NULL || !demux->runtime_failed) return false;
    if (error != NULL && error_size != 0) {
        snprintf(
            error, error_size, "%s",
            demux->runtime_error[0] == '\0'
                ? "MP4 lazy fragment operation failed"
                : demux->runtime_error);
    }
    return true;
}

size_t media_mp4_retained_bytes(const MediaMp4Demux *demux)
{
    return demux == NULL ? 0 : demux->retained_bytes;
}

void media_mp4_close(MediaMp4Demux *demux)
{
    if (demux == NULL) return;
    for (size_t i = 0; i < demux->track_count; i++)
        mp4_track_free(demux, &demux->tracks[i]);
    budget_free(demux->budget, demux->fragment_index);
    budget_free(demux->budget, demux->tracks);
    Budget *budget = demux->budget;
    memset(demux, 0, sizeof(*demux));
    budget_free(budget, demux);
}
#define MEDIA_H264_MAXIMUM_SPS_BYTES 512u

MediaH264DecoderRoute media_h264_avcc_decoder_route(
    const unsigned char *config, size_t length, uint8_t *profile_idc)
{
    if (profile_idc != NULL) *profile_idc = 0;
    if (config == NULL || length < 10u || config[0] != 1u
        || (config[5] & 31u) == 0u) {
        return MEDIA_H264_DECODER_ROUTE_UNSUPPORTED;
    }
    size_t sps_length = ((size_t) config[6] << 8u) | config[7];
    if (sps_length < 2u || sps_length > length - 8u
        || (config[8] & 0x1fu) != 7u || config[1] != config[9]) {
        return MEDIA_H264_DECODER_ROUTE_UNSUPPORTED;
    }
    uint8_t profile = config[9];
    if (profile_idc != NULL) *profile_idc = profile;
    if (profile == 66u || profile == 77u) {
        return MEDIA_H264_DECODER_ROUTE_PSP_FIRMWARE;
    }
    if (profile == 100u || profile == 110u || profile == 122u
        || profile == 244u || profile == 44u || profile == 83u
        || profile == 86u || profile == 118u || profile == 128u
        || profile == 138u || profile == 139u || profile == 134u
        || profile == 135u) {
        return MEDIA_H264_DECODER_ROUTE_HIGH_EXTENSION;
    }
    return MEDIA_H264_DECODER_ROUTE_UNSUPPORTED;
}

static int media_h264_hex(unsigned char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    return -1;
}

MediaH264DecoderRoute media_h264_codec_string_decoder_route(
    const char *mime, uint8_t *profile_idc)
{
    if (profile_idc != NULL) *profile_idc = 0;
    if (mime == NULL) return MEDIA_H264_DECODER_ROUTE_UNSUPPORTED;
    for (const char *at = mime; (at = strstr(at, "avc1.")) != NULL; at++) {
        unsigned char before = at == mime ? '\0' : (unsigned char) at[-1];
        if (!(at == mime || before == '"' || before == ',' || before == ';'
              || before == ' ' || before == '\t')) continue;
        unsigned value = 0;
        bool valid = true;
        for (size_t i = 0; i < 6u; i++) {
            int digit = media_h264_hex((unsigned char) at[5u + i]);
            if (digit < 0) { valid = false; break; }
            value = (value << 4u) | (unsigned) digit;
        }
        if (!valid) continue;
        unsigned char after = (unsigned char) at[11];
        if (!(after == '\0' || after == '"' || after == ',' || after == ';'
              || after == ' ' || after == '\t')) continue;
        uint8_t profile = (uint8_t) (value >> 16u);
        if (profile_idc != NULL) *profile_idc = profile;
        if (profile == 66u || profile == 77u)
            return MEDIA_H264_DECODER_ROUTE_PSP_FIRMWARE;
        if (profile == 100u || profile == 110u || profile == 122u
            || profile == 244u || profile == 44u || profile == 83u
            || profile == 86u || profile == 118u || profile == 128u
            || profile == 138u || profile == 139u || profile == 134u
            || profile == 135u)
            return MEDIA_H264_DECODER_ROUTE_HIGH_EXTENSION;
        return MEDIA_H264_DECODER_ROUTE_UNSUPPORTED;
    }
    return MEDIA_H264_DECODER_ROUTE_UNSUPPORTED;
}

typedef struct {
    unsigned char data[MEDIA_H264_MAXIMUM_SPS_BYTES];
    size_t length;
    size_t bit;
} MediaH264Bits;

static bool media_h264_bit(MediaH264Bits *bits, uint32_t *value)
{
    if (bits == NULL || value == NULL || bits->bit >= bits->length * 8u)
        return false;
    *value = (bits->data[bits->bit / 8u]
              >> (7u - (bits->bit % 8u))) & 1u;
    bits->bit++;
    return true;
}

static bool media_h264_bits(
    MediaH264Bits *bits, unsigned count, uint32_t *value)
{
    if (value == NULL || count > 32u) return false;
    *value = 0;
    for (unsigned i = 0; i < count; i++) {
        uint32_t bit = 0;
        if (!media_h264_bit(bits, &bit)) return false;
        *value = (*value << 1u) | bit;
    }
    return true;
}

static bool media_h264_ue(MediaH264Bits *bits, uint32_t *value)
{
    unsigned zeros = 0;
    uint32_t bit = 0, suffix = 0;
    while (zeros < 31u) {
        if (!media_h264_bit(bits, &bit)) return false;
        if (bit != 0) break;
        zeros++;
    }
    if (bit == 0 || !media_h264_bits(bits, zeros, &suffix)) return false;
    *value = ((UINT32_C(1) << zeros) - 1u) + suffix;
    return true;
}

static bool media_h264_se(MediaH264Bits *bits, int32_t *value)
{
    uint32_t code = 0;
    if (!media_h264_ue(bits, &code) || code == UINT32_MAX) return false;
    *value = (code & 1u)
        ? (int32_t) ((code + 1u) / 2u)
        : -(int32_t) (code / 2u);
    return true;
}

static bool media_h264_skip_scaling(MediaH264Bits *bits, unsigned count)
{
    int last = 8, next = 8;
    for (unsigned i = 0; i < count; i++) {
        if (next != 0) {
            int32_t delta = 0;
            if (!media_h264_se(bits, &delta)) return false;
            next = (last + delta + 256) & 255;
        }
        if (next != 0) last = next;
    }
    return true;
}

bool media_h264_sps_dimensions(
    const unsigned char *sps, size_t length,
    uint16_t *width, uint16_t *height)
{
    if (width != NULL) *width = 0;
    if (height != NULL) *height = 0;
    if (sps == NULL || width == NULL || height == NULL
        || length < 4u || length > MEDIA_H264_MAXIMUM_SPS_BYTES
        || (sps[0] & 0x80u) != 0
        || (sps[0] & 0x1fu) != 7u) return false;
    MediaH264Bits bits = {0};
    unsigned zeros = 0;
    for (size_t i = 1; i < length; i++) {
        if (zeros >= 2u && sps[i] == 3u
            && i + 1u < length && sps[i + 1u] <= 3u) {
            zeros = 0;
            continue;
        }
        bits.data[bits.length++] = sps[i];
        zeros = sps[i] == 0 ? zeros + 1u : 0u;
    }
    uint32_t profile = 0, ignored = 0, chroma = 1;
    if (!media_h264_bits(&bits, 8, &profile)
        || !media_h264_bits(&bits, 8, &ignored)
        || !media_h264_bits(&bits, 8, &ignored)
        || !media_h264_ue(&bits, &ignored)) return false;
    bool separate_colour_plane = false;
    if (profile == 100u || profile == 110u || profile == 122u
        || profile == 244u || profile == 44u || profile == 83u
        || profile == 86u || profile == 118u || profile == 128u
        || profile == 138u || profile == 139u || profile == 134u
        || profile == 135u) {
        if (!media_h264_ue(&bits, &chroma) || chroma > 3u) return false;
        if (chroma == 3u) {
            if (!media_h264_bit(&bits, &ignored)) return false;
            separate_colour_plane = ignored != 0;
        }
        if (!media_h264_ue(&bits, &ignored)
            || !media_h264_ue(&bits, &ignored)
            || !media_h264_bit(&bits, &ignored)
            || !media_h264_bit(&bits, &ignored)) return false;
        if (ignored != 0) {
            unsigned lists = chroma == 3u ? 12u : 8u;
            for (unsigned i = 0; i < lists; i++) {
                uint32_t present = 0;
                if (!media_h264_bit(&bits, &present)) return false;
                if (present && !media_h264_skip_scaling(
                        &bits, i < 6u ? 16u : 64u)) return false;
            }
        }
    }
    uint32_t order_type = 0;
    if (!media_h264_ue(&bits, &ignored)
        || !media_h264_ue(&bits, &order_type)) return false;
    if (order_type == 0u) {
        if (!media_h264_ue(&bits, &ignored)) return false;
    } else if (order_type == 1u) {
        int32_t signed_value = 0;
        uint32_t cycle = 0;
        if (!media_h264_bit(&bits, &ignored)
            || !media_h264_se(&bits, &signed_value)
            || !media_h264_se(&bits, &signed_value)
            || !media_h264_ue(&bits, &cycle) || cycle > 256u) return false;
        for (uint32_t i = 0; i < cycle; i++) {
            if (!media_h264_se(&bits, &signed_value)) return false;
        }
    } else if (order_type > 2u) {
        return false;
    }
    uint32_t width_mbs = 0, height_map = 0, frame_only = 0;
    if (!media_h264_ue(&bits, &ignored)
        || !media_h264_bit(&bits, &ignored)
        || !media_h264_ue(&bits, &width_mbs)
        || !media_h264_ue(&bits, &height_map)
        || !media_h264_bit(&bits, &frame_only)) return false;
    if (!frame_only && !media_h264_bit(&bits, &ignored)) return false;
    if (!media_h264_bit(&bits, &ignored)) return false;
    uint32_t crop = 0, left = 0, right = 0, top = 0, bottom = 0;
    if (!media_h264_bit(&bits, &crop)) return false;
    if (crop && (!media_h264_ue(&bits, &left)
                 || !media_h264_ue(&bits, &right)
                 || !media_h264_ue(&bits, &top)
                 || !media_h264_ue(&bits, &bottom))) return false;
    uint64_t coded_width = ((uint64_t) width_mbs + 1u) * 16u;
    uint64_t coded_height =
        ((uint64_t) height_map + 1u) * 16u * (2u - frame_only);
    uint32_t chroma_array = separate_colour_plane ? 0u : chroma;
    uint32_t crop_x = chroma_array == 0u || chroma_array == 3u ? 1u : 2u;
    uint32_t crop_y = (chroma_array == 1u ? 2u : 1u) * (2u - frame_only);
    uint64_t crop_width =
        ((uint64_t) left + (uint64_t) right) * crop_x;
    uint64_t crop_height =
        ((uint64_t) top + (uint64_t) bottom) * crop_y;
    if (crop_width >= coded_width || crop_height >= coded_height
        || coded_width - crop_width > UINT16_MAX
        || coded_height - crop_height > UINT16_MAX) return false;
    *width = (uint16_t) (coded_width - crop_width);
    *height = (uint16_t) (coded_height - crop_height);
    return *width != 0 && *height != 0;
}

bool media_h264_avcc_dimensions(
    const unsigned char *config, size_t length,
    uint16_t *width, uint16_t *height, uint8_t *nal_length_size)
{
    if (width != NULL) *width = 0;
    if (height != NULL) *height = 0;
    if (nal_length_size != NULL) *nal_length_size = 0;
    if (config == NULL || width == NULL || height == NULL
        || nal_length_size == NULL || length < 7u || config[0] != 1u)
        return false;
    *nal_length_size = (uint8_t) ((config[4] & 3u) + 1u);
    if (*nal_length_size == 3u) return false;
    unsigned count = config[5] & 31u;
    size_t cursor = 6u;
    if (count == 0) return false;
    for (unsigned i = 0; i < count; i++) {
        if (cursor + 2u > length) return false;
        size_t item = ((size_t) config[cursor] << 8u) | config[cursor + 1u];
        cursor += 2u;
        uint16_t item_width = 0, item_height = 0;
        if (item < 4u || item > length - cursor
            || config[cursor + 1u] != config[1]
            || config[cursor + 2u] != config[2]
            || config[cursor + 3u] != config[3]
            || !media_h264_sps_dimensions(
                config + cursor, item, &item_width, &item_height)
            || (i != 0 && (item_width != *width
                           || item_height != *height))) return false;
        *width = item_width;
        *height = item_height;
        cursor += item;
    }
    if (cursor >= length) return false;
    unsigned pps_count = config[cursor++];
    if (pps_count == 0) return false;
    for (unsigned i = 0; i < pps_count; i++) {
        if (cursor > length || length - cursor < 2u) return false;
        size_t item =
            ((size_t) config[cursor] << 8u) | config[cursor + 1u];
        cursor += 2u;
        if (item == 0 || item > length - cursor) return false;
        cursor += item;
    }
    return true;
}

static bool media_h264_avcc_has_parameter_set(
    const unsigned char *config, size_t length, unsigned wanted_type,
    const unsigned char *nal, size_t nal_length)
{
    if (config == NULL || nal == NULL || length < 7u
        || config[0] != 1u
        || (wanted_type != 7u && wanted_type != 8u)) {
        return false;
    }
    bool found = false;
    size_t cursor = 6u;
    unsigned sps_count = config[5] & 31u;
    if (sps_count == 0) return false;
    for (unsigned i = 0; i < sps_count; i++) {
        if (length - cursor < 2u) return false;
        size_t item =
            ((size_t) config[cursor] << 8u) | config[cursor + 1u];
        cursor += 2u;
        if (item == 0 || item > length - cursor) return false;
        if (wanted_type == 7u && item == nal_length
            && memcmp(config + cursor, nal, item) == 0) {
            found = true;
        }
        cursor += item;
    }
    if (cursor >= length) return false;
    unsigned pps_count = config[cursor++];
    if (pps_count == 0) return false;
    for (unsigned i = 0; i < pps_count; i++) {
        if (length - cursor < 2u) return false;
        size_t item =
            ((size_t) config[cursor] << 8u) | config[cursor + 1u];
        cursor += 2u;
        if (item == 0 || item > length - cursor) return false;
        if (wanted_type == 8u && item == nal_length
            && memcmp(config + cursor, nal, item) == 0) {
            found = true;
        }
        cursor += item;
    }
    return found;
}

bool media_h264_avcc_sample_is_admitted(
    const unsigned char *payload, size_t length,
    uint8_t nal_length_size, uint16_t width, uint16_t height,
    const unsigned char *config, size_t config_length)
{
    if (payload == NULL || length == 0 || width == 0 || height == 0
        || nal_length_size < 1u || nal_length_size > 4u
        || nal_length_size == 3u || config == NULL
        || config_length < 7u || config[0] != 1u
        || (uint8_t) ((config[4] & 3u) + 1u) != nal_length_size) {
        return false;
    }
    size_t cursor = 0;
    while (cursor < length) {
        if (nal_length_size > length - cursor) return false;
        size_t nal_length = 0;
        for (uint8_t i = 0; i < nal_length_size; i++) {
            nal_length = (nal_length << 8u) | payload[cursor++];
        }
        if (nal_length == 0 || nal_length > length - cursor)
            return false;
        unsigned nal_type = payload[cursor] & 0x1fu;
        if ((payload[cursor] & 0x80u) != 0
            || nal_type == 0u || nal_type > 12u) return false;
        if (nal_type == 7u) {
            uint16_t sample_width = 0, sample_height = 0;
            if (!media_h264_sps_dimensions(
                    payload + cursor, nal_length,
                    &sample_width, &sample_height)
                || sample_width != width || sample_height != height
                || !media_h264_avcc_has_parameter_set(
                    config, config_length, nal_type,
                    payload + cursor, nal_length)) {
                return false;
            }
        } else if (nal_type == 8u
                   && !media_h264_avcc_has_parameter_set(
                       config, config_length, nal_type,
                       payload + cursor, nal_length)) {
            return false;
        }
        cursor += nal_length;
    }
    return cursor == length;
}

static size_t media_h264_annexb_start(
    const unsigned char *payload, size_t length, size_t cursor,
    size_t *prefix_bytes)
{
    if (prefix_bytes != NULL) *prefix_bytes = 0;
    if (payload == NULL || prefix_bytes == NULL || cursor > length)
        return SIZE_MAX;
    if (length - cursor < 3u) return SIZE_MAX;
    for (size_t i = cursor; i <= length - 3u; i++) {
        if (length - i >= 4u && payload[i] == 0 && payload[i + 1u] == 0
            && payload[i + 2u] == 0 && payload[i + 3u] == 1u) {
            *prefix_bytes = 4u;
            return i;
        }
        if (payload[i] == 0 && payload[i + 1u] == 0
            && payload[i + 2u] == 1u) {
            *prefix_bytes = 3u;
            return i;
        }
    }
    return SIZE_MAX;
}

bool media_h264_annexb_sample_is_admitted(
    const unsigned char *payload, size_t length,
    uint16_t width, uint16_t height)
{
    if (payload == NULL || length == 0 || width == 0 || height == 0)
        return false;
    size_t prefix = 0;
    size_t start = media_h264_annexb_start(
        payload, length, 0, &prefix);
    if (start == SIZE_MAX) return false;
    bool saw_nal = false;
    while (start != SIZE_MAX) {
        size_t nal = start + prefix;
        if (nal >= length) return false;
        size_t next_prefix = 0;
        size_t next = media_h264_annexb_start(
            payload, length, nal + 1u, &next_prefix);
        size_t nal_end = next == SIZE_MAX ? length : next;
        if (nal_end <= nal || (payload[nal] & 0x80u) != 0
            || (payload[nal] & 0x1fu) == 0u) return false;
        if ((payload[nal] & 0x1fu) == 7u) {
            uint16_t sample_width = 0, sample_height = 0;
            if (!media_h264_sps_dimensions(
                    payload + nal, nal_end - nal,
                    &sample_width, &sample_height)
                || sample_width != width || sample_height != height)
                return false;
        }
        saw_nal = true;
        start = next;
        prefix = next_prefix;
    }
    return saw_nal;
}
