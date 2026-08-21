#ifndef TILEFINCH_MEDIA_HLS_H
#define TILEFINCH_MEDIA_HLS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/budget.h"
#include "tilefinch/media_source.h"

#define MEDIA_HLS_MAXIMUM_PLAYLIST_BYTES (64u * 1024u)
#define MEDIA_HLS_MAXIMUM_SEGMENTS 256u
#define MEDIA_HLS_MAXIMUM_SEGMENT_BYTES (2u * 1024u * 1024u)
#define MEDIA_HLS_TRANSPORT_CHUNK_BYTES (16u * 1024u)

typedef enum {
    MEDIA_HLS_PLAYLIST_MEDIA = 0,
    MEDIA_HLS_PLAYLIST_MASTER
} MediaHlsPlaylistKind;

typedef struct MediaHlsPlaylist MediaHlsPlaylist;

MediaHlsPlaylist *media_hls_playlist_parse(
    Budget *budget, const char *playlist_url,
    const unsigned char *bytes, size_t length,
    char *error, size_t error_size);
MediaHlsPlaylistKind media_hls_playlist_kind(const MediaHlsPlaylist *playlist);
/* Select the least costly compatible variant, preferring the smallest one at
   or above target_height. Unknown geometry is only a last-resort candidate. */
bool media_hls_playlist_select_variant(
    const MediaHlsPlaylist *playlist, unsigned maximum_width,
    unsigned maximum_height, unsigned target_height,
    char *url, size_t url_size);
size_t media_hls_playlist_segment_count(const MediaHlsPlaylist *playlist);
uint64_t media_hls_playlist_duration_us(const MediaHlsPlaylist *playlist);
void media_hls_playlist_destroy(MediaHlsPlaylist *playlist);

typedef enum {
    MEDIA_HLS_TRANSPORT_WAIT = 0,
    MEDIA_HLS_TRANSPORT_CHUNK,
    MEDIA_HLS_TRANSPORT_COMPLETE,
    MEDIA_HLS_TRANSPORT_ERROR
} MediaHlsTransportPollResult;

typedef struct {
    void *opaque;
    /* A zero handle with an empty error is transient admission pressure. */
    uint64_t (*start)(void *opaque, const char *url, size_t maximum_bytes,
                      char *error, size_t error_size);
    MediaHlsTransportPollResult (*poll)(
        void *opaque, uint64_t handle, unsigned char *destination,
        size_t capacity, size_t *length, char *error, size_t error_size);
    void (*cancel)(void *opaque, uint64_t handle);
} MediaHlsTransport;

typedef struct MediaHlsSource MediaHlsSource;

typedef enum {
    MEDIA_HLS_PRIME_PENDING = 0,
    MEDIA_HLS_PRIME_READY,
    MEDIA_HLS_PRIME_FAILED
} MediaHlsPrimeStatus;

typedef struct {
    size_t segments_started;
    size_t segments_completed;
    size_t bytes_received;
    size_t queued_samples;
    size_t queued_bytes;
    size_t queue_overflows;
    size_t malformed_segments;
    unsigned ts_sync_losses;
    unsigned ts_malformed_packets;
    unsigned ts_malformed_psi;
    unsigned active_requests;
    bool ended;
} MediaHlsStats;

/* Takes ownership of a MEDIA playlist on success only. */
MediaHlsSource *media_hls_source_create(
    Budget *budget, MediaHlsPlaylist *playlist,
    const MediaHlsTransport *transport, char *error, size_t error_size);
MediaHlsPrimeStatus media_hls_source_prime(
    MediaHlsSource *source, char *error, size_t error_size);
bool media_hls_source_sample_source(
    MediaHlsSource *source, MediaSampleSource *sample_source);
bool media_hls_source_stream_info(
    const MediaHlsSource *source, MediaMp4TrackInfo *video,
    MediaMp4TrackInfo *audio);
void media_hls_source_stats(const MediaHlsSource *source,
                            MediaHlsStats *stats);
void media_hls_source_destroy(MediaHlsSource *source);

#endif
