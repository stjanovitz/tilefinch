#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tilefinch/budget.h"
#include "tilefinch/media_backend.h"
#include "tilefinch/media_mp4.h"

typedef struct {
    FILE *file;
    uint64_t length;
    size_t reads;
    size_t bytes;
} ProbeReader;

typedef struct {
    size_t video_packets;
    size_t audio_packets;
    uint64_t bytes;
    uint16_t video_width;
    uint16_t video_height;
    uint8_t nal_length_size;
    const unsigned char *avcc;
    size_t avcc_length;
    bool validate_avc;
} ProbeBackend;

static MediaBackendResult probe_submit(
    void *opaque, const MediaMp4Sample *sample,
    const unsigned char *payload, size_t length,
    char *error, size_t error_size)
{
    ProbeBackend *backend = opaque;
    if (backend == NULL || sample == NULL || payload == NULL
        || length != sample->size) return MEDIA_BACKEND_ERROR;
    if (sample->kind == MEDIA_MP4_TRACK_VIDEO) {
        if (backend->validate_avc
            && !media_h264_avcc_sample_is_admitted(
                payload, length, backend->nal_length_size,
                backend->video_width, backend->video_height,
                backend->avcc, backend->avcc_length)) {
            if (error != NULL && error_size != 0) {
                snprintf(
                    error, error_size,
                    "AVC access unit %zu failed PSP admission",
                    backend->video_packets);
            }
            return MEDIA_BACKEND_ERROR;
        }
        backend->video_packets++;
    }
    if (sample->kind == MEDIA_MP4_TRACK_AUDIO) backend->audio_packets++;
    backend->bytes += length;
    return MEDIA_BACKEND_ACCEPTED;
}

static bool probe_advance(void *opaque, uint64_t clock_us,
                          char *error, size_t error_size)
{
    (void) clock_us;
    (void) error;
    (void) error_size;
    return opaque != NULL;
}

static void probe_destroy(void *opaque)
{
    (void) opaque;
}

static bool probe_read(void *opaque, uint64_t offset,
                       void *destination, size_t length)
{
    ProbeReader *reader = opaque;
    if (offset > reader->length || length > reader->length - offset
        || offset > LONG_MAX || fseek(
            reader->file, (long) offset, SEEK_SET) != 0) return false;
    if (fread(destination, 1, length, reader->file) != length) return false;
    reader->reads++;
    reader->bytes += length;
    return true;
}

static const char *track_kind(MediaMp4TrackKind kind)
{
    if (kind == MEDIA_MP4_TRACK_VIDEO) return "video";
    if (kind == MEDIA_MP4_TRACK_AUDIO) return "audio";
    return "other";
}

static void fourcc(uint32_t value, char output[5])
{
    output[0] = (char) (value >> 24);
    output[1] = (char) (value >> 16);
    output[2] = (char) (value >> 8);
    output[3] = (char) value;
    output[4] = '\0';
}

static uint64_t probe_time_us(uint64_t value, uint32_t timescale)
{
    if (timescale == 0) return UINT64_MAX;
    uint64_t whole = value / timescale;
    uint64_t remainder = value % timescale;
    if (whole > UINT64_MAX / UINT64_C(1000000))
        return UINT64_MAX;
    uint64_t base = whole * UINT64_C(1000000);
    uint64_t fraction =
        remainder * UINT64_C(1000000) / timescale;
    return fraction > UINT64_MAX - base
        ? UINT64_MAX : base + fraction;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s INPUT.mp4\n", argv[0]);
        return 2;
    }
    FILE *file = fopen(argv[1], "rb");
    if (file == NULL || fseek(file, 0, SEEK_END) != 0) {
        fprintf(stderr, "could not open MP4\n");
        if (file != NULL) fclose(file);
        return 1;
    }
    long length = ftell(file);
    if (length <= 0) {
        fprintf(stderr, "invalid MP4 length\n");
        fclose(file);
        return 1;
    }
    ProbeReader probe = {.file = file, .length = (uint64_t) length};
    MediaRangeReader reader = {
        .opaque = &probe, .length = probe.length, .read = probe_read
    };
    Budget budget;
    budget_init(&budget, 8u * 1024u * 1024u);
    char error[256];
    MediaMp4Demux *demux = media_mp4_open(
        &budget, &reader, NULL, error, sizeof(error));
    if (demux == NULL) {
        fprintf(stderr, "media probe failed: %s\n", error);
        fclose(file);
        return 1;
    }
    printf("media-probe tracks=%zu file-bytes=%" PRIu64
           " metadata-retained=%zu\n",
           media_mp4_track_count(demux), probe.length,
           media_mp4_retained_bytes(demux));
    uint64_t duration_us = 0;
    bool have_video = false;
    ProbeBackend backend_state = {0};
    for (size_t i = 0; i < media_mp4_track_count(demux); i++) {
        MediaMp4TrackInfo info;
        char codec[5];
        if (!media_mp4_track_info(demux, i, &info)) return 1;
        fourcc(info.codec, codec);
        unsigned avc_profile = info.codec_config_length > 1u
            ? info.codec_config[1] : 0u;
        unsigned avc_level = info.codec_config_length > 3u
            ? info.codec_config[3] : 0u;
        printf("track=%zu kind=%s codec=%s samples=%u max-sample=%u "
               "timescale=%u duration=%" PRIu64 " geometry=%ux%u "
               "audio=%u/%u config=%zu nal-length=%u avc-profile=%u "
               "avc-level=%u\n",
               i, track_kind(info.kind), codec, info.sample_count,
               info.largest_sample, info.timescale, info.duration,
               info.width, info.height, info.sample_rate, info.channels,
               info.codec_config_length, info.nal_length_size,
               avc_profile, avc_level);
        if (info.kind == MEDIA_MP4_TRACK_VIDEO) {
            have_video = true;
            if (info.codec == MEDIA_MP4_FOURCC('a','v','c','1')) {
                uint16_t width = 0, height = 0;
                uint8_t nal_length_size = 0;
                if (!media_h264_avcc_dimensions(
                        info.codec_config, info.codec_config_length,
                        &width, &height, &nal_length_size)) {
                    fprintf(stderr, "AVC config failed PSP admission\n");
                    media_mp4_close(demux);
                    fclose(file);
                    return 1;
                }
                backend_state.video_width = width;
                backend_state.video_height = height;
                backend_state.nal_length_size = nal_length_size;
                backend_state.avcc = info.codec_config;
                backend_state.avcc_length = info.codec_config_length;
                backend_state.validate_avc = true;
            }
        }
        if (info.timescale != 0) {
            uint64_t track_us =
                probe_time_us(info.duration, info.timescale);
            if (track_us > duration_us) duration_us = track_us;
        }
    }
    uint64_t payload = 0;
    size_t samples = 0;
    MediaMp4Sample sample;
    while (media_mp4_next_sample(demux, &sample)) {
        payload += sample.size;
        samples++;
    }
    char schedule_error[256] = {0};
    if (media_mp4_last_error(
            demux, schedule_error, sizeof(schedule_error))) {
        fprintf(stderr, "media schedule failed: %s\n", schedule_error);
        media_mp4_close(demux);
        fclose(file);
        return 1;
    }
    printf("schedule samples=%zu payload=%" PRIu64
           " range-reads=%zu range-bytes=%zu budget-peak=%zu\n",
           samples, payload, probe.reads, probe.bytes, budget.peak);
    if (duration_us != 0 && duration_us != UINT64_MAX) {
        uint64_t target_us =
            (duration_us / 3u) * 2u
            + (duration_us % 3u) * 2u / 3u;
        uint64_t actual_us = UINT64_MAX;
        MediaMp4Sample sought = {0};
        if (!media_mp4_seek_us(
                demux, target_us, &actual_us)
            || !media_mp4_next_sample(demux, &sought)
            || (have_video && !sought.keyframe)
            || probe_time_us(sought.dts, sought.timescale) > target_us) {
            char seek_error[256] = {0};
            (void) media_mp4_last_error(
                demux, seek_error, sizeof(seek_error));
            fprintf(
                stderr,
                "media seek probe failed: target=%" PRIu64
                " actual=%" PRIu64 " %s\n",
                target_us, actual_us,
                seek_error[0] == '\0' ? "invalid sample" : seek_error);
            media_mp4_close(demux);
            fclose(file);
            return 1;
        }
        printf(
            "seek target=%" PRIu64 " actual=%" PRIu64
            " first-dts=%" PRIu64 " keyframe=%s\n",
            target_us, actual_us,
            probe_time_us(sought.dts, sought.timescale),
            sought.keyframe ? "yes" : "no");
    }
    media_mp4_rewind(demux);
    char rewind_error[256] = {0};
    if (media_mp4_last_error(
            demux, rewind_error, sizeof(rewind_error))) {
        fprintf(stderr, "media rewind failed: %s\n", rewind_error);
        media_mp4_close(demux);
        fclose(file);
        return 1;
    }
    MediaBackend backend = {
        .opaque = &backend_state,
        .submit = probe_submit,
        .advance = probe_advance,
        .destroy = probe_destroy
    };
    char playback_error[256];
    MediaPlayback *playback = media_playback_create(
        &budget, demux, &backend, NULL,
        playback_error, sizeof(playback_error));
    if (playback == NULL) {
        fprintf(stderr, "media playback probe failed: %s\n", playback_error);
        media_mp4_close(demux);
        fclose(file);
        return 1;
    }
    /* Exercise the complete schedule, not an arbitrary first minute. Keep a
       hard six-hour ceiling so a hostile timescale cannot turn this lab tool
       into an unbounded run. */
    const uint64_t maximum_probe_us = UINT64_C(6) * 60u * 60u * 1000000u;
    uint64_t clock_limit = duration_us < maximum_probe_us - UINT64_C(10000000)
        ? duration_us + UINT64_C(10000000) : maximum_probe_us;
    uint64_t clock_us = 0;
    while (!media_playback_ended(playback) && clock_us < clock_limit) {
        if (!media_playback_advance(
                playback, clock_us,
                playback_error, sizeof(playback_error))) {
            fprintf(stderr, "media playback advance failed: %s\n",
                    playback_error);
            media_playback_destroy(playback);
            media_mp4_close(demux);
            fclose(file);
            return 1;
        }
        clock_us += 100000u;
    }
    printf("backend video-packets=%zu audio-packets=%zu bytes=%" PRIu64
           " packet-buffer=%zu ended=%s\n",
           backend_state.video_packets, backend_state.audio_packets,
           backend_state.bytes, media_playback_packet_bytes(playback),
           media_playback_ended(playback) ? "yes" : "no");
    if (!media_playback_ended(playback)
        || backend_state.video_packets + backend_state.audio_packets
           != samples
        || backend_state.bytes != payload) {
        fprintf(stderr, "media backend schedule diverged\n");
        media_playback_destroy(playback);
        media_mp4_close(demux);
        fclose(file);
        return 1;
    }
    media_playback_destroy(playback);
    media_mp4_close(demux);
    fclose(file);
    if (budget.current != 0) {
        fprintf(stderr, "media probe leaked %zu budget bytes\n",
                budget.current);
        return 1;
    }
    puts("media-probe status=PASS");
    return 0;
}
