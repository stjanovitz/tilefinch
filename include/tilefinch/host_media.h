#ifndef TILEFINCH_HOST_MEDIA_H
#define TILEFINCH_HOST_MEDIA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/budget.h"
#include "tilefinch/navigation.h"

typedef struct HostMediaPlayer HostMediaPlayer;

typedef struct {
    int source_width;
    int source_height;
    int output_width;
    int output_height;
    unsigned frame_rate_numerator;
    unsigned frame_rate_denominator;
    uint64_t duration_us;
    uint64_t current_time_us;
    uint64_t audio_time_us;
    int64_t audio_video_skew_us;
    size_t decoded_frames;
    size_t dropped_frames;
    size_t video_packets;
    size_t audio_packets;
    size_t audio_streams;
    uint64_t decoded_audio_samples;
    uint64_t presented_audio_samples;
    uint64_t dropped_audio_samples;
    unsigned audio_sample_rate;
    unsigned audio_channels;
    size_t working_set_bytes;
    bool audio_output_active;
    bool playing;
    bool ended;
    int resolved_itag;
    size_t resolver_bytes;
    char source_kind[24];
    char video_codec[32];
    char audio_codec[32];
    char title[256];
} HostMediaStats;

/*
 * Host-only reference backend. It deliberately consumes an ordinary local
 * MP4 rather than knowing anything about a provider. A later PSP backend can
 * implement the same fixed-surface/timed-frame contract with sceMpeg.
 */
HostMediaPlayer *host_media_create(Budget *budget, BrowserSession *session,
                                   const char *source,
                                   int maximum_width, int maximum_height,
                                   bool enable_audio_output,
                                   char *error, size_t error_size);
bool host_media_attach_first_video(HostMediaPlayer *player,
                                   NavigationSession *navigation,
                                   char *error, size_t error_size);
bool host_media_attach_native_player(HostMediaPlayer *player,
                                     NavigationSession *navigation,
                                     char *error, size_t error_size);
bool host_media_reattach_native_player(HostMediaPlayer *player,
                                       NavigationSession *navigation,
                                       char *error, size_t error_size);
bool host_media_set_native_player_visible(HostMediaPlayer *player,
                                          NavigationSession *navigation,
                                          bool visible,
                                          char *error, size_t error_size);
bool host_media_advance(HostMediaPlayer *player,
                        NavigationSession *navigation,
                        unsigned elapsed_ms, bool *frame_changed,
                        char *error, size_t error_size);
bool host_media_seek(HostMediaPlayer *player,
                     NavigationSession *navigation,
                     uint64_t target_time_us, bool *frame_changed,
                     char *error, size_t error_size);
void host_media_set_playing(HostMediaPlayer *player, bool playing);
bool host_media_is_attached(const HostMediaPlayer *player,
                            const NavigationSession *navigation);
const void *host_media_surface_identity(
    const HostMediaPlayer *player, const NavigationSession *navigation);
bool host_media_copy_current_frame_rgb565(
    const HostMediaPlayer *player, uint16_t *pixels,
    int width, int height, int stride);
bool host_media_get_stats(const HostMediaPlayer *player,
                          HostMediaStats *stats);
void host_media_destroy(HostMediaPlayer *player);

#endif
