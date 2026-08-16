#ifndef TILEFINCH_HOST_MEDIA_TIMING_H
#define TILEFINCH_HOST_MEDIA_TIMING_H

#include <stdbool.h>
#include <stdint.h>
#include <limits.h>

typedef struct {
    uint64_t presentation_limit_us;
    uint64_t current_time_us;
    int64_t audio_video_skew_us;
} HostMediaTiming;

/*
 * Native audio is the presentation clock, so its decode horizon must advance
 * from samples that have actually reached that clock.  A caller's logical
 * clock can be ahead while the device is starting or doing expensive frame
 * work; using it here overfills a bounded output queue and turns whole decoded
 * frames into audible gaps.
 */
static inline uint64_t host_media_audio_prefetch_target_samples(
    uint64_t presented_samples, unsigned sample_rate,
    unsigned read_ahead_ms)
{
    uint64_t lead = (uint64_t) sample_rate * read_ahead_ms / 1000u;
    return lead > UINT64_MAX - presented_samples
        ? UINT64_MAX : presented_samples + lead;
}

static inline bool host_media_audio_prefetch_needed(
    uint64_t successfully_queued_samples, uint64_t target_samples)
{
    return successfully_queued_samples < target_samples;
}

static inline bool host_media_audio_queue_admits(
    uint64_t queued_bytes, uint64_t incoming_bytes,
    uint64_t maximum_bytes)
{
    return queued_bytes <= maximum_bytes
        && incoming_bytes <= maximum_bytes - queued_bytes;
}

/*
 * Native audio remains the presentation master while decoded PCM is still
 * audible. Once the track is known to be exhausted and the device queue has
 * drained, the caller clock must take over or a longer video track can never
 * present its tail.
 */
static inline bool host_media_audio_clock_is_master(
    bool native_audio, bool decoder_drained,
    bool declared_end_reached, bool output_pending)
{
    return native_audio
        && (output_pending
            || (!decoder_drained && !declared_end_reached));
}

/*
 * A frame whose complete display interval is already behind the presentation
 * clock can be discarded before color conversion. The retained candidate is
 * therefore the first frame which might actually reach the screen.
 */
static inline bool host_media_timing_frame_is_stale(
    uint64_t frame_time_us, uint64_t presentation_clock_us,
    uint64_t frame_duration_us)
{
    if (frame_duration_us == 0
        || frame_time_us > presentation_clock_us) return false;
    return frame_duration_us <= presentation_clock_us - frame_time_us;
}

/*
 * A display can only choose whole video frames. Selecting only frames whose
 * PTS is at or behind the audio cursor leaves a systematic lag of almost one
 * frame. Permit the immediately following frame when it is closer to the
 * audio cursor than the currently displayed one. This bounds ideal-cadence
 * absolute skew to half a frame without accumulating clock corrections or
 * making timing specific to a particular frame rate.
 */
static inline bool host_media_timing_should_present(
    uint64_t presented_video_us, uint64_t audio_cursor_us,
    uint64_t candidate_video_us)
{
    if (candidate_video_us <= audio_cursor_us) return true;
    if (presented_video_us >= audio_cursor_us) return false;
    return candidate_video_us - audio_cursor_us
         < audio_cursor_us - presented_video_us;
}

static inline bool host_media_timing_should_present_seek(
    bool have_seek_frame, uint64_t presented_video_us,
    uint64_t target_us, uint64_t candidate_video_us)
{
    if (candidate_video_us <= target_us || !have_seek_frame) return true;
    return host_media_timing_should_present(
        presented_video_us, target_us, candidate_video_us);
}

static inline int64_t host_media_timing_delta(uint64_t left,
                                              uint64_t right)
{
    if (left >= right) {
        uint64_t magnitude = left - right;
        return magnitude > (uint64_t) INT64_MAX
            ? INT64_MAX : (int64_t) magnitude;
    }
    uint64_t magnitude = right - left;
    return magnitude > (uint64_t) INT64_MAX
        ? INT64_MIN : -(int64_t) magnitude;
}

/*
 * The demux/decode head may be substantially ahead of what the speakers and
 * screen have presented. Keep it out of both synchronization and UI time.
 */
static inline HostMediaTiming host_media_timing_snapshot(
    uint64_t decode_head_us, uint64_t presented_video_us,
    uint64_t presented_audio_us, bool native_audio,
    bool audio_clock_initialized)
{
    HostMediaTiming timing = {
        .presentation_limit_us = decode_head_us,
        .current_time_us = decode_head_us,
        .audio_video_skew_us = host_media_timing_delta(
            presented_audio_us, presented_video_us)
    };
    if (native_audio) {
        timing.presentation_limit_us = audio_clock_initialized
            ? presented_audio_us : presented_video_us;
        timing.current_time_us = timing.presentation_limit_us;
    }
    return timing;
}

#endif
