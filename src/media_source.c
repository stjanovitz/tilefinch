#include "tilefinch/media_source.h"

static size_t mp4_track_count(const void *opaque)
{
    return media_mp4_track_count(opaque);
}

static bool mp4_track_info(const void *opaque, size_t index,
                           MediaMp4TrackInfo *info)
{
    return media_mp4_track_info(opaque, index, info);
}

static bool mp4_next_sample(void *opaque, MediaMp4Sample *sample)
{
    return media_mp4_next_sample(opaque, sample);
}

static bool mp4_last_error(const void *opaque, char *error, size_t error_size)
{
    return media_mp4_last_error(opaque, error, error_size);
}

static bool mp4_would_block(const void *opaque)
{
    return media_mp4_would_block(opaque);
}

static bool mp4_sample_resident(const void *opaque,
                                const MediaMp4Sample *sample)
{
    return media_mp4_sample_resident(opaque, sample);
}

static bool mp4_read_sample_waiting(void *opaque,
                                    const MediaMp4Sample *sample,
                                    void *destination, size_t capacity)
{
    return media_mp4_read_sample_waiting(
        opaque, sample, destination, capacity);
}

static bool mp4_read_sample(void *opaque, const MediaMp4Sample *sample,
                            void *destination, size_t capacity)
{
    return media_mp4_read_sample(opaque, sample, destination, capacity);
}

static bool mp4_seek_us(void *opaque, uint64_t target_us, uint64_t *actual_us)
{
    return media_mp4_seek_us(opaque, target_us, actual_us);
}

static bool mp4_seek_after_us(void *opaque, uint64_t target_us,
                              uint64_t *actual_us)
{
    return media_mp4_seek_after_us(opaque, target_us, actual_us);
}

static void mp4_rewind(void *opaque)
{
    media_mp4_rewind(opaque);
}

static size_t mp4_retained_bytes(const void *opaque)
{
    return media_mp4_retained_bytes(opaque);
}

static const MediaSampleSourceOps mp4_source_ops = {
    .track_count = mp4_track_count,
    .track_info = mp4_track_info,
    .next_sample = mp4_next_sample,
    .last_error = mp4_last_error,
    .would_block = mp4_would_block,
    .sample_resident = mp4_sample_resident,
    .read_sample_waiting = mp4_read_sample_waiting,
    .read_sample = mp4_read_sample,
    .seek_us = mp4_seek_us,
    .seek_after_us = mp4_seek_after_us,
    .rewind = mp4_rewind,
    .retained_bytes = mp4_retained_bytes
};

bool media_sample_source_from_mp4(MediaMp4Demux *demux,
                                  MediaSampleSource *source)
{
    if (demux == NULL || source == NULL) return false;
    *source = (MediaSampleSource) {
        .opaque = demux,
        .ops = &mp4_source_ops
    };
    return true;
}
