/* libFuzzer target: MP4 demuxer (box tree, sample tables, avcC/esds codec
   configuration) plus the standalone H.264/AAC parameter-set helpers.
   The whole input is the file served through a MediaRangeReader. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tilefinch/budget.h"
#include "tilefinch/media_mp4.h"

#define MIB (1024u * 1024u)

typedef struct {
    const uint8_t *bytes;
    size_t length;
} FuzzReader;

static bool fuzz_read(void *opaque, uint64_t offset, void *destination,
                      size_t length)
{
    const FuzzReader *reader = opaque;
    if (offset > reader->length || length > reader->length - offset)
        return false;
    if (length != 0) memcpy(destination, reader->bytes + offset, length);
    return true;
}

static void exercise_codec_config(const MediaMp4TrackInfo *info)
{
    if (info->codec_config == NULL || info->codec_config_length == 0) return;
    /* Copy to an exact-size buffer so over-reads are visible to ASan. */
    unsigned char *config = malloc(info->codec_config_length);
    if (config == NULL) return;
    memcpy(config, info->codec_config, info->codec_config_length);
    size_t length = info->codec_config_length;
    uint16_t width = 0, height = 0;
    uint8_t nal = 0, profile = 0;
    (void) media_h264_avcc_dimensions(config, length, &width, &height, &nal);
    (void) media_h264_avcc_decoder_route(config, length, &profile);
    (void) media_h264_annexb_decoder_route(config, length, &profile);
    MediaAacStreamInfo aac;
    (void) media_aac_esds_stream_info(config, length, &aac);
    (void) media_aac_esds_is_low_complexity(config, length);
    free(config);
}

static void exercise_annexb(const uint8_t *data, size_t size)
{
    if (size > 64 * 1024) size = 64 * 1024;
    unsigned char *payload = malloc(size + size / 2 + 16);
    if (payload == NULL) return;
    memcpy(payload, data, size);
    uint16_t width = 0, height = 0;
    (void) media_h264_sps_dimensions(payload, size, &width, &height);
    (void) media_h264_annexb_sample_is_admitted(payload, size, 480, 272);
    (void) media_h264_annexb_sample_matches_config(payload, size, 480, 272,
                                                   payload, size / 2);
    (void) media_h264_avcc_sample_is_admitted(payload, size, 4, 480, 272,
                                              payload, size / 2);
    const unsigned char *sps = NULL, *pps = NULL;
    size_t sps_length = 0, pps_length = 0;
    (void) media_h264_annexb_parameter_sets(payload, size, &sps, &sps_length,
                                            &pps, &pps_length);
    size_t output_length = 0;
    unsigned nal_count = 0;
    (void) media_h264_annexb_to_avcc_in_place(
        payload, size, size + size / 2 + 16, &output_length, &nal_count);
    free(payload);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > 1024 * 1024) return 0;
    /* Exact-size copy: libFuzzer's buffer is already exact, but keep the
       reader independent of it. */
    uint8_t *file = malloc(size == 0 ? 1 : size);
    if (file == NULL) return 0;
    if (size != 0) memcpy(file, data, size);

    Budget budget;
    budget_init(&budget, 24u * MIB);
    FuzzReader fuzz_reader = {file, size};
    MediaRangeReader reader = {
        .opaque = &fuzz_reader, .length = size, .read = fuzz_read
    };
    char error[256];
    MediaMp4Demux *demux = media_mp4_open(&budget, &reader, NULL, error,
                                          sizeof(error));
    if (demux == NULL && getenv("TILEFINCH_FUZZ_DEBUG") != NULL)
        fprintf(stderr, "media_mp4_open: %s\n", error);
    if (demux != NULL) {
        size_t tracks = media_mp4_track_count(demux);
        uint32_t largest = 0;
        for (size_t i = 0; i < tracks; i++) {
            MediaMp4TrackInfo info;
            if (media_mp4_track_info(demux, i, &info)) {
                exercise_codec_config(&info);
                if (info.largest_sample > largest)
                    largest = info.largest_sample;
            }
        }
        size_t capacity = largest > 4u * MIB ? 4u * MIB : largest;
        unsigned char *sample_buffer = malloc(capacity == 0 ? 1 : capacity);
        for (int pass = 0; pass < 2 && sample_buffer != NULL; pass++) {
            MediaMp4Sample sample;
            for (size_t n = 0; n < 4096 && media_mp4_next_sample(demux, &sample);
                 n++) {
                if (sample.size <= capacity)
                    (void) media_mp4_read_sample(demux, &sample,
                                                 sample_buffer, capacity);
            }
            (void) media_mp4_last_error(demux, error, sizeof(error));
            uint64_t actual = 0;
            (void) media_mp4_seek_us(demux, pass == 0 ? 1500000u : 0u,
                                     &actual);
            (void) media_mp4_seek_after_us(demux, 250000u, &actual);
            if (pass == 0) media_mp4_rewind(demux);
        }
        free(sample_buffer);
        (void) media_mp4_retained_bytes(demux);
        media_mp4_close(demux);
    }
    exercise_annexb(file, size);
    if (budget.current != 0) {
        fprintf(stderr, "fuzz_mp4: budget leak of %zu bytes\n",
                budget.current);
        abort();
    }
    free(file);
    return 0;
}
