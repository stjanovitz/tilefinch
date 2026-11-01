#ifndef TILEFINCH_MEDIA_HLS_H
#define TILEFINCH_MEDIA_HLS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/budget.h"
#include "tilefinch/media_source.h"

#define MEDIA_HLS_MAXIMUM_PLAYLIST_BYTES (64u * 1024u)
#define MEDIA_HLS_MAXIMUM_PLAYLIST_DOWNLOAD_BYTES (2u * 1024u * 1024u)
#define MEDIA_HLS_RETAINED_LIVE_SEGMENTS 12u
#define MEDIA_HLS_MAXIMUM_SEGMENTS 256u
#define MEDIA_HLS_MAXIMUM_SEGMENT_BYTES (2u * 1024u * 1024u)
#define MEDIA_HLS_TRANSPORT_CHUNK_BYTES (16u * 1024u)

typedef enum {
    MEDIA_HLS_PLAYLIST_MEDIA = 0,
    MEDIA_HLS_PLAYLIST_MASTER
} MediaHlsPlaylistKind;

typedef struct MediaHlsPlaylist MediaHlsPlaylist;
typedef struct MediaHlsPlaylistStream MediaHlsPlaylistStream;

MediaHlsPlaylist *media_hls_playlist_parse(
    Budget *budget, const char *playlist_url,
    const unsigned char *bytes, size_t length,
    char *error, size_t error_size);
MediaHlsPlaylistKind media_hls_playlist_kind(const MediaHlsPlaylist *playlist);
#if !defined(__PSP__)
/* Host tests only; the PSP selects through
   media_hls_playlist_select_decodable_streams. */
/* Select the least costly compatible variant, preferring the smallest one at
   or above target_height. Unknown geometry is only a last-resort candidate. */
bool media_hls_playlist_select_variant(
    const MediaHlsPlaylist *playlist, unsigned maximum_width,
    unsigned maximum_height, unsigned target_height,
    char *url, size_t url_size);
/* Select the video rendition and, when the chosen variant names an AUDIO
   group, its default (or first) audio rendition. `audio_url` is left empty
   for an ordinary muxed variant. */
bool media_hls_playlist_select_streams(
    const MediaHlsPlaylist *playlist, unsigned maximum_width,
    unsigned maximum_height, unsigned target_height,
    char *video_url, size_t video_url_size,
    char *audio_url, size_t audio_url_size);
#endif
/* Decoder-aware master selection. Every variant fits within
   software_width x software_height (the software decoder's bound, which is
   what High profile needs). A variant whose CODECS name avc1 Baseline or
   Main is also admitted up to the firmware decoder's geometry, as is one
   that names no AVC profile when admit_undeclared_firmware is set; the
   caller must then check the first segment's SPS (`unverified`). Among
   admitted variants the smallest at or above target_height wins. */
typedef struct {
    unsigned software_width;
    unsigned software_height;
    unsigned target_height;
    bool admit_undeclared_firmware;
    /* The firmware decoder refused a selected variant's H.264 coding (a
       B-frame pyramid, say): every variant must now fit the software bound,
       whatever profile it declares. */
    bool firmware_refused;
} MediaHlsDecoderLimits;

typedef struct {
    /* A compatible video variant was chosen (its audio may still be
       missing when the call fails). */
    bool video_selected;
    /* The chosen variant exceeds the software bound without declaring a
       Baseline/Main profile. */
    bool unverified;
    unsigned width;
    unsigned height;
} MediaHlsSelection;

bool media_hls_playlist_select_decodable_streams(
    const MediaHlsPlaylist *playlist, const MediaHlsDecoderLimits *limits,
    char *video_url, size_t video_url_size,
    char *audio_url, size_t audio_url_size,
    MediaHlsSelection *selection);
size_t media_hls_playlist_segment_count(const MediaHlsPlaylist *playlist);
#if !defined(__PSP__)
/* Host tools only (the stream recorder): the absolute URL and duration of a
   MEDIA playlist's segment INDEX. */
bool media_hls_playlist_segment(const MediaHlsPlaylist *playlist,
                                size_t index, char *url, size_t url_size,
                                uint32_t *duration_ms);
#endif
uint64_t media_hls_playlist_duration_us(const MediaHlsPlaylist *playlist);
bool media_hls_playlist_is_live(const MediaHlsPlaylist *playlist);
void media_hls_playlist_destroy(MediaHlsPlaylist *playlist);

/* Incremental playlist capture. Small playlists retain their exact bytes.
   Oversized media playlists retain only the newest bounded live window;
   oversized masters fail rather than silently losing rendition metadata. */
MediaHlsPlaylistStream *media_hls_playlist_stream_create(
    Budget *budget, const char *playlist_url,
    char *error, size_t error_size);
/* A playlist delivered through a redirect resolves its relative URIs
   against the URL that answered. Only before the first byte is fed. */
bool media_hls_playlist_stream_rebase(
    MediaHlsPlaylistStream *stream, const char *playlist_url);
bool media_hls_playlist_stream_feed(
    MediaHlsPlaylistStream *stream,
    const unsigned char *bytes, size_t length,
    char *error, size_t error_size);
MediaHlsPlaylist *media_hls_playlist_stream_finish(
    MediaHlsPlaylistStream *stream, char *error, size_t error_size);
size_t media_hls_playlist_stream_bytes_seen(
    const MediaHlsPlaylistStream *stream);
bool media_hls_playlist_stream_was_compacted(
    const MediaHlsPlaylistStream *stream);
void media_hls_playlist_stream_destroy(MediaHlsPlaylistStream *stream);

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
    MEDIA_HLS_TRACK_MIXED = 0,
    MEDIA_HLS_TRACK_VIDEO,
    MEDIA_HLS_TRACK_AUDIO
} MediaHlsTrackSelection;

typedef enum {
    MEDIA_HLS_PRIME_PENDING = 0,
    MEDIA_HLS_PRIME_READY,
    MEDIA_HLS_PRIME_FAILED
} MediaHlsPrimeStatus;

typedef struct {
    size_t segments_started;
    size_t segments_completed;
    size_t playlist_bytes_received;
    size_t bytes_received;
    size_t queued_samples;
    size_t queued_bytes;
    size_t queue_overflows;
    size_t malformed_segments;
    size_t playlist_refreshes;
    size_t playlist_refresh_failures;
    size_t skipped_live_segments;
    unsigned ts_sync_losses;
    unsigned ts_malformed_packets;
    unsigned ts_malformed_psi;
    unsigned active_requests;
    uint64_t buffered_until_us;
    bool live;
    bool ended;
    /* The sample queue is full enough that delivery waits for the consumer:
       more waiting cannot buffer further ahead until samples are taken. */
    bool holding;
} MediaHlsStats;

/* Takes ownership of a MEDIA playlist on success only. */
MediaHlsSource *media_hls_source_create(
    Budget *budget, MediaHlsPlaylist *playlist,
    const MediaHlsTransport *transport, char *error, size_t error_size);
MediaHlsSource *media_hls_source_create_track(
    Budget *budget, MediaHlsPlaylist *playlist,
    const MediaHlsTransport *transport, MediaHlsTrackSelection selection,
    char *error, size_t error_size);
MediaHlsPrimeStatus media_hls_source_prime(
    MediaHlsSource *source, char *error, size_t error_size);
/* One bounded delivery visit (up to a few KiB of segment bytes, stopping
   early when the transport waits or the sample queue is half full). The PSP
   frontend calls this once per frame so segment and live-playlist traffic can
   progress while decoded samples remain queued. */
void media_hls_source_pump(MediaHlsSource *source, uint64_t now_us);
bool media_hls_source_wants_playlist_refresh(
    const MediaHlsSource *source, uint64_t now_us);
/* Always consumes replacement, including stale or rejected live snapshots. */
bool media_hls_source_update_playlist(
    MediaHlsSource *source, MediaHlsPlaylist *replacement,
    uint64_t now_us, char *error, size_t error_size);
void media_hls_source_note_playlist_refresh_failure(
    MediaHlsSource *source, uint64_t now_us);
bool media_hls_source_sample_source(
    MediaHlsSource *source, MediaSampleSource *sample_source);
bool media_hls_source_stream_info(
    const MediaHlsSource *source, MediaMp4TrackInfo *video,
    MediaMp4TrackInfo *audio);
void media_hls_source_stats(const MediaHlsSource *source,
                            MediaHlsStats *stats);
bool media_hls_source_failed(const MediaHlsSource *source);
void media_hls_source_destroy(MediaHlsSource *source);

#endif
