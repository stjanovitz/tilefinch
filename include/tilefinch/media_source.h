#ifndef TILEFINCH_MEDIA_SOURCE_H
#define TILEFINCH_MEDIA_SOURCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/media_mp4.h"

/*
 * A bounded packet source consumed by MediaPlayback.
 *
 * MP4 is the ordinary implementation.  Keeping the packet scheduler against
 * this small interface lets a streaming container (notably MPEG-TS/HLS) use
 * the same clock, cancellation, backend, and presentation machinery without
 * growing a second player.  The source remains owned by its caller.
 */
typedef struct {
    size_t (*track_count)(const void *opaque);
    bool (*track_info)(const void *opaque, size_t index,
                       MediaMp4TrackInfo *info);
    bool (*next_sample)(void *opaque, MediaMp4Sample *sample);
    bool (*last_error)(const void *opaque, char *error, size_t error_size);
    bool (*would_block)(const void *opaque);
    bool (*sample_resident)(const void *opaque,
                            const MediaMp4Sample *sample);
    bool (*read_sample_waiting)(void *opaque, const MediaMp4Sample *sample,
                                void *destination, size_t capacity);
    bool (*read_sample)(void *opaque, const MediaMp4Sample *sample,
                        void *destination, size_t capacity);
    bool (*seek_us)(void *opaque, uint64_t target_us, uint64_t *actual_us);
    bool (*seek_after_us)(void *opaque, uint64_t target_us,
                          uint64_t *actual_us);
    /* Optional cooperative metadata preparation. */
    bool (*prepare_seek_us)(void *opaque, uint64_t target_us);
    void (*rewind)(void *opaque);
    size_t (*retained_bytes)(const void *opaque);
    /*
     * Optional, for one source carrying both tracks. MediaPlayback uses the
     * pair only while the source's own head is held by the backend, to keep
     * the independent audio track flowing: a video access unit waiting for a
     * free picture surface must not strand the AAC that advances the clock
     * which frees that surface.
     *
     * peek_track_sample describes the earliest sample of `kind` that
     * next_sample has not yet returned and whose payload is already buffered.
     * It consumes, fetches and allocates nothing. take_track_sample copies
     * exactly that peeked sample and removes it from the source's order, so
     * next_sample never returns it; false leaves the source unchanged.
     */
    bool (*peek_track_sample)(void *opaque, MediaMp4TrackKind kind,
                              MediaMp4Sample *sample);
    bool (*take_track_sample)(void *opaque, const MediaMp4Sample *sample,
                              void *destination, size_t capacity);
    /* Optional. Drop a sample next_sample returned, without reading it (an
       AAC unit before a seek target). Required of a source whose
       next_sample only peeks: without it the same unit would be returned
       again. A source whose next_sample already advanced leaves it NULL. */
    void (*discard_sample)(void *opaque, const MediaMp4Sample *sample);
} MediaSampleSourceOps;

typedef struct {
    void *opaque;
    const MediaSampleSourceOps *ops;
} MediaSampleSource;

bool media_sample_source_from_mp4(MediaMp4Demux *demux,
                                  MediaSampleSource *source);

#endif
