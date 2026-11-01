#ifndef TILEFINCH_MEDIA_REPLAY_CHECK_H
#define TILEFINCH_MEDIA_REPLAY_CHECK_H

/*
 * Host-only (lab and tests): plays a page's media through the PSP's own
 * fetch and demux path -- the page-media probe, psp_media_hls.c over the
 * background transport, or the media_http range source under the MP4
 * demuxer -- and reads every sample payload up to a media time, without
 * decoding. Under a response-keyed HTTP replay this proves a recording holds
 * everything the device will request (src/fetch/trace_media_lane.inc);
 * under a capture it records exactly those requests.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/budget.h"
#include "tilefinch/psp_media_hls.h"
#include "tilefinch/session.h"

typedef enum {
    MEDIA_REPLAY_DECODER_UNKNOWN = 0,
    MEDIA_REPLAY_DECODER_FIRMWARE,
    MEDIA_REPLAY_DECODER_SOFTWARE,
    MEDIA_REPLAY_DECODER_REFUSED
} MediaReplayDecoder;

typedef struct {
    bool hls;
    bool opened;
    bool ended;
    unsigned video_samples;
    unsigned audio_samples;
    uint64_t video_until_us;
    uint64_t audio_until_us;
    uint64_t payload_bytes;
    uint64_t content_length;
    size_t hls_segments;
    unsigned width;
    unsigned height;
    /* The decoder the PSP open path would give the video track
       (media_replay_check_decoder): its verdict, the SPS profile and
       level, and the refusal text when no decoder takes it. */
    MediaReplayDecoder decoder;
    unsigned profile_idc;
    unsigned level_idc;
    char decoder_refusal[128];
    /* The video track's duration as the player reads it (0: live or
       unknown). */
    uint64_t duration_us;
    /* media_replay_check_seek only: where the HLS source landed for the
       seek target (a segment start), the first video sample and the first
       audio sample at or after the landing read there (and the audio
       units before it that playback drops), and whether they agree with
       that landing. */
    uint64_t seek_actual_us;
    uint64_t seek_video_us;
    uint64_t seek_audio_us;
    unsigned seek_audio_dropped;
    bool seek_ran;
    bool seek_ok;
    char error[256];
} MediaReplayCheckReport;

/* Reads samples until the video track reaches target_us or the stream ends.
   True when the stream opened and delivered video up to the target or to
   its clean end.

   record_range_bytes (recording only): a progressive file is fetched as one
   Range response of its first record_range_bytes bytes (all of it when
   smaller) instead of window by window. A replay answers every window the
   device asks for from that one stored body, whatever window sizes and
   order its player uses. HLS is always recorded as the player requests it. */
/* The PSP open path's decoder choice for a page video track, from its
   codec config (avcC, or Annex-B SPS/PPS when annexb): the firmware
   decoder when route, reference structure, level and geometry all admit
   it (wide program enabled, the default), the software decoder when the
   firmware refuses the coding and the picture fits 432x240, otherwise
   refused with the open path's message in `refusal`. */
MediaReplayDecoder media_replay_check_decoder(
    const unsigned char *config, size_t length, bool annexb,
    unsigned *profile_idc, unsigned *level_idc,
    char *refusal, size_t refusal_size);

bool media_replay_check(Budget *budget, BrowserSession *session,
                        const char *media_url, const char *document_url,
                        PspMediaHlsSelectionBound bound, uint64_t target_us,
                        uint64_t record_range_bytes,
                        MediaReplayCheckReport *report);
/* media_replay_check, then (HLS only) the player's seek: the sources are
   repositioned to seek_us and the first samples read there. A failed seek
   sets the report's error and returns false. */
bool media_replay_check_seek(Budget *budget, BrowserSession *session,
                             const char *media_url, const char *document_url,
                             PspMediaHlsSelectionBound bound,
                             uint64_t target_us, uint64_t seek_us,
                             MediaReplayCheckReport *report);

#endif
