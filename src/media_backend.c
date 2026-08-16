#include "tilefinch/media_backend.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

struct MediaPlayback {
    Budget *budget;
    MediaMp4Demux *demux[2];
    size_t demux_count;
    MediaBackend backend;
    unsigned char *packet;
    size_t packet_capacity;
    size_t maximum_packet_bytes;
    uint64_t decode_lead_us;
    uint64_t audio_start_us;
    uint64_t buffered_until_us;
    bool has_audio;
    bool audio_submission_blocked;
    MediaMp4Sample pending[2];
    uint64_t pending_time_us[2];
    bool have_pending[2];
    bool source_ended[2];
    bool playing;
    bool ended;
    MediaPlaybackJobStats job_stats;
};

static void playback_error(char *error, size_t error_size,
                           const char *format, ...)
{
    if (error == NULL || error_size == 0) return;
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(error, error_size, format, arguments);
    va_end(arguments);
}

static uint64_t sample_time_us(const MediaMp4Sample *sample)
{
    if (sample->timescale == 0) return UINT64_MAX;
    /* Web video/audio clocks and ordinary clip timestamps fit this path.
       Decompose the ratio so Allegrex uses its native 32-bit divider instead
       of __udivdi3 once per pending sample, without changing floor rounding. */
    uint32_t scale_remainder =
        UINT32_C(1000000) % sample->timescale;
    if (sample->dts <= UINT32_MAX
        && (uint64_t) (sample->timescale - 1u) * scale_remainder
               <= UINT32_MAX) {
        uint32_t value = (uint32_t) sample->dts;
        uint32_t whole = value / sample->timescale;
        uint32_t remainder = value % sample->timescale;
        uint32_t scale_whole = UINT32_C(1000000) / sample->timescale;
        return (uint64_t) whole * UINT32_C(1000000)
             + (uint64_t) remainder * scale_whole
             + (remainder * scale_remainder) / sample->timescale;
    }
    uint64_t whole = sample->dts / sample->timescale;
    uint64_t remainder = sample->dts % sample->timescale;
    if (whole > UINT64_MAX / UINT64_C(1000000)) return UINT64_MAX;
    uint64_t base = whole * UINT64_C(1000000);
    uint64_t fraction =
        remainder * UINT64_C(1000000) / sample->timescale;
    return fraction > UINT64_MAX - base
        ? UINT64_MAX : base + fraction;
}

/*
 * What the other source was holding at the instant this visit gave up on the
 * selected one.
 *
 * Called from the two exits that have a chosen sample in hand and could not
 * place it: its payload was not buffered, or the backend refused it. The
 * "nothing selected at all" exit is not one of these -- there is no head to be
 * blocked behind.
 *
 * Observation only. It reads the pending sample the pump already peeked and
 * asks the demuxer a residency question that copies nothing and fetches
 * nothing, so the schedule it measures is the schedule that would have
 * happened without it.
 */
static bool playback_note_head_block(
    MediaPlayback *playback, size_t selected, uint64_t horizon)
{
    playback->job_stats.head_blocks++;
    if (playback->pending[selected].kind == MEDIA_MP4_TRACK_VIDEO)
        playback->job_stats.head_block_video++;
    else playback->job_stats.head_block_audio++;
    /* One source carrying both tracks has no alternate to have offered. */
    if (playback->demux_count < 2) return false;
    size_t other = selected == 0 ? 1u : 0u;
    if (!playback->have_pending[other]) return false;
    playback->job_stats.head_alt_pending++;
    uint64_t alternate = playback->pending_time_us[other];
    if (alternate > horizon) return false;
    playback->job_stats.head_alt_in_horizon++;
    /* Normal merge order selects the earliest sample. A split seek may hold
       one source while advancing the other, so that invariant is temporarily
       false. Keep the unsigned lead useful and make that state explicit. */
    uint64_t selected_time = playback->pending_time_us[selected];
    if (alternate >= selected_time) {
        playback->job_stats.head_alt_lead_total_us +=
            alternate - selected_time;
        playback->job_stats.head_alt_lead_samples++;
    } else {
        playback->job_stats.head_alt_behind++;
    }
    if (!media_mp4_sample_resident(
            playback->demux[other], &playback->pending[other])) return false;
    playback->job_stats.head_alt_resident++;
    return true;
}

static void media_playback_fail_seek(MediaPlayback *playback)
{
    if (playback == NULL) return;
    memset(playback->have_pending, 0, sizeof(playback->have_pending));
    memset(playback->source_ended, 1, sizeof(playback->source_ended));
    playback->buffered_until_us = 0;
    playback->ended = true;
    playback->playing = false;
    playback->audio_submission_blocked = false;
    if (playback->backend.set_playing != NULL) {
        playback->backend.set_playing(
            playback->backend.opaque, false);
    }
}

static MediaPlayback *media_playback_create_sources(
    Budget *budget, MediaMp4Demux *video_demux,
    MediaMp4Demux *audio_demux, const MediaBackend *backend,
    const MediaPlaybackOptions *options, char *error, size_t error_size)
{
    if (error != NULL && error_size != 0) error[0] = '\0';
    if (budget == NULL || video_demux == NULL || backend == NULL
        || backend->submit == NULL || backend->advance == NULL
        || backend->destroy == NULL) {
        playback_error(error, error_size, "media: invalid backend");
        return NULL;
    }
    size_t largest = 0;
    bool has_audio = audio_demux != NULL;
    MediaMp4Demux *sources[2] = {video_demux, audio_demux};
    size_t source_count = audio_demux == NULL ? 1u : 2u;
    for (size_t source = 0; source < source_count; source++) {
        for (size_t i = 0;
             i < media_mp4_track_count(sources[source]); i++) {
            MediaMp4TrackInfo info;
            if (!media_mp4_track_info(sources[source], i, &info)) {
                playback_error(
                    error, error_size,
                    "MP4 track metadata unavailable");
                return NULL;
            }
            if (info.largest_sample > largest) largest = info.largest_sample;
            if (info.kind == MEDIA_MP4_TRACK_AUDIO) has_audio = true;
        }
    }
    size_t maximum = options != NULL && options->maximum_packet_bytes != 0
        ? options->maximum_packet_bytes : 1024u * 1024u;
    if (largest == 0 || largest > maximum) {
        playback_error(error, error_size,
                       "MP4 sample %zu > %zu-byte limit",
                       largest, maximum);
        return NULL;
    }
    MediaPlayback *playback = budget_calloc_category(
        budget, BUDGET_CATEGORY_NAVIGATION, 1, sizeof(*playback));
    if (playback == NULL) {
        playback_error(error, error_size,
                       "media state exceeds budget");
        return NULL;
    }
    size_t initial_capacity = options != NULL
        && options->preallocate_maximum_packet_bytes ? maximum : largest;
    playback->packet = budget_malloc_category(
        budget, BUDGET_CATEGORY_RESOURCE, initial_capacity);
    if (playback->packet == NULL) {
        budget_free(budget, playback);
        playback_error(error, error_size,
                       "media packet exceeds budget");
        return NULL;
    }
    playback->budget = budget;
    playback->demux[0] = video_demux;
    playback->demux[1] = audio_demux;
    playback->demux_count = source_count;
    playback->backend = *backend;
    playback->packet_capacity = initial_capacity;
    playback->maximum_packet_bytes = maximum;
    playback->decode_lead_us = options == NULL
        || options->decode_lead_us == 0 ? 100000u : options->decode_lead_us;
    playback->audio_start_us =
        options == NULL ? 0 : options->audio_start_us;
    playback->has_audio = has_audio;
    playback->playing = true;
    return playback;
}

MediaPlayback *media_playback_create(
    Budget *budget, MediaMp4Demux *demux, const MediaBackend *backend,
    const MediaPlaybackOptions *options, char *error, size_t error_size)
{
    return media_playback_create_sources(
        budget, demux, NULL, backend, options, error, error_size);
}

MediaPlayback *media_playback_create_split(
    Budget *budget, MediaMp4Demux *video_demux,
    MediaMp4Demux *audio_demux, const MediaBackend *backend,
    const MediaPlaybackOptions *options, char *error, size_t error_size)
{
    if (audio_demux == NULL) {
        playback_error(error, error_size,
                       "adaptive audio unavailable");
        return NULL;
    }
    return media_playback_create_sources(
        budget, video_demux, audio_demux, backend,
        options, error, error_size);
}

bool media_playback_advance(MediaPlayback *playback, uint64_t clock_us,
                            char *error, size_t error_size)
{
    return media_playback_advance_bounded(
        playback, clock_us, SIZE_MAX, error, error_size)
        != MEDIA_PLAYBACK_ADVANCE_ERROR;
}

MediaPlaybackAdvanceResult media_playback_advance_bounded(
    MediaPlayback *playback, uint64_t clock_us, size_t maximum_packets,
    char *error, size_t error_size)
{
    return media_playback_advance_bounded_cancelable(
        playback, clock_us, maximum_packets, NULL, error, error_size);
}

MediaPlaybackAdvanceResult media_playback_advance_bounded_cancelable(
    MediaPlayback *playback, uint64_t clock_us, size_t maximum_packets,
    const TilefinchCancellation *cancellation,
    char *error, size_t error_size)
{
    if (playback == NULL) {
        playback_error(error, error_size, "media unavailable");
        return MEDIA_PLAYBACK_ADVANCE_ERROR;
    }
    playback->job_stats.calls++;
    if (tilefinch_cancellation_requested(cancellation)) {
        return MEDIA_PLAYBACK_ADVANCE_CANCELLED;
    }
    if (!playback->playing || playback->ended) {
        playback->job_stats.idle_calls++;
        bool advanced = playback->backend.advance(
            playback->backend.opaque, clock_us, error, error_size);
        if (tilefinch_cancellation_requested(cancellation))
            return MEDIA_PLAYBACK_ADVANCE_CANCELLED;
        return advanced ? MEDIA_PLAYBACK_ADVANCE_COMPLETE
                        : MEDIA_PLAYBACK_ADVANCE_ERROR;
    }
    uint64_t horizon = clock_us > UINT64_MAX - playback->decode_lead_us
        ? UINT64_MAX : clock_us + playback->decode_lead_us;
    size_t processed = 0;
    /* Extra packets this visit may spend so a backend holding a staged access
       unit can be offered its partner. Granted one at a time, only when the
       backend asks, and capped whatever it answers. */
    size_t paired_credit = 0;
    /*
     * A blocked split-track head must not strand an already-buffered unit from
     * the independent other track. Once that counterfactual is proven, keep
     * the blocked source pending and exclude it for the rest of this bounded
     * call. This is not reordering within a codec stream: adaptive playback
     * owns one demux per track, and both units have already passed the common
     * clock horizon. The next public call starts from the earliest source
     * again, so neither track can be skipped indefinitely.
     */
    size_t deferred_source = playback->demux_count;
    MediaPlaybackAdvanceResult status = MEDIA_PLAYBACK_ADVANCE_COMPLETE;
    bool source_blocked = false;
    while (true) {
        if (tilefinch_cancellation_requested(cancellation)) {
            return MEDIA_PLAYBACK_ADVANCE_CANCELLED;
        }
        if (processed >= maximum_packets + paired_credit) {
            playback->job_stats.packet_limit_breaks++;
            status = MEDIA_PLAYBACK_ADVANCE_PENDING;
            break;
        }
        for (size_t source = 0; source < playback->demux_count; source++) {
            if (!playback->have_pending[source]
                && !playback->source_ended[source]) {
                if (!media_mp4_next_sample(
                        playback->demux[source],
                        &playback->pending[source])) {
                    if (tilefinch_cancellation_requested(cancellation)) {
                        return MEDIA_PLAYBACK_ADVANCE_CANCELLED;
                    }
                    /*
                     * Unbuffered bytes are neither the end of the source nor a
                     * transport failure. Leave the source open with nothing
                     * pending and let the pump retire this unit: the fetch the
                     * demuxer just asked for proceeds while the decoder works.
                     */
                    if (media_mp4_would_block(playback->demux[source])) {
                        source_blocked = true;
                        /* Source zero is the video demuxer in both shapes:
                           the split form is contractually video-then-audio,
                           and a progressive MP4 carries both tracks there. */
                        if (source == 0)
                            playback->job_stats.source_block_video++;
                        continue;
                    }
                    char demux_error[256] = {0};
                    if (media_mp4_last_error(
                            playback->demux[source],
                            demux_error, sizeof(demux_error))) {
                        playback_error(
                            error, error_size, "%s", demux_error);
                        return MEDIA_PLAYBACK_ADVANCE_ERROR;
                    }
                    playback->source_ended[source] = true;
                } else {
                    playback->have_pending[source] = true;
                    playback->pending_time_us[source] =
                        sample_time_us(&playback->pending[source]);
                }
            }
        }
        size_t selected = playback->demux_count;
        uint64_t selected_time = UINT64_MAX;
        bool held_audio_pending = false;
        for (size_t source = 0; source < playback->demux_count; source++) {
            if (source == deferred_source) continue;
            if (!playback->have_pending[source]) continue;
            /* In the split form source one is contractually audio. During a
               seek preroll its first target AU remains pending, while source
               zero decodes forward from the preceding random-access point. */
            if (playback->audio_submission_blocked && source == 1u) {
                held_audio_pending = true;
                continue;
            }
            uint64_t candidate = playback->pending_time_us[source];
            if (selected == playback->demux_count
                || candidate < selected_time) {
                selected = source;
                selected_time = candidate;
            }
        }
        if (selected == playback->demux_count
            && deferred_source != playback->demux_count) {
            /* The other source advanced as far as this bounded call allowed;
               the original blocked head is intentionally still pending for
               the next call. It is not end-of-stream and must never drain. */
            status = MEDIA_PLAYBACK_ADVANCE_PENDING;
            break;
        }
        if (selected == playback->demux_count && held_audio_pending) {
            /* The video source is caught up for this horizon. Audio is
               intentionally retained, not EOF and not a drain condition. */
            status = MEDIA_PLAYBACK_ADVANCE_PENDING;
            break;
        }
        if (selected == playback->demux_count && source_blocked) {
            /* Nothing to submit and at least one source is still fetching:
               this is a pending pipeline, not a drained one. Draining here
               would publish end-of-stream in the middle of a refill. */
            playback->job_stats.would_block_calls++;
            playback->job_stats.source_block_calls++;
            status = MEDIA_PLAYBACK_ADVANCE_PENDING;
            break;
        }
        if (selected == playback->demux_count) {
            playback->job_stats.source_ended_breaks++;
            if (playback->backend.drain == NULL) {
                playback->ended = true;
                break;
            }
            /*
             * A native decoder drain is an irreducible unit just like one
             * packet submission. Invoke at most one per public bounded pump;
             * cancellation is observed immediately before and after it.
             */
            MediaBackendResult result = playback->backend.drain(
                playback->backend.opaque, error, error_size);
            playback->job_stats.drain_calls++;
            if (result == MEDIA_BACKEND_ERROR) {
                return MEDIA_PLAYBACK_ADVANCE_ERROR;
            }
            if (result == MEDIA_BACKEND_END)
                playback->ended = true;
            if (tilefinch_cancellation_requested(cancellation)) {
                return MEDIA_PLAYBACK_ADVANCE_CANCELLED;
            }
            if (result == MEDIA_BACKEND_WOULD_BLOCK) {
                playback->job_stats.would_block_calls++;
                playback->job_stats.drain_block_calls++;
                status = MEDIA_PLAYBACK_ADVANCE_PENDING;
            } else if (result == MEDIA_BACKEND_ACCEPTED
                       || result == MEDIA_BACKEND_QUEUED) {
                status = MEDIA_PLAYBACK_ADVANCE_PENDING;
            }
            break;
        }
        if (selected_time > horizon) {
            playback->job_stats.horizon_breaks++;
            /* Which track was far enough ahead to stop the pump.
               Both tracks are measured against one decode_lead_us -- the
               per-kind leads went out with the slot-split revert -- so the
               split is still worth having, but for a different reason than
               the one written here before: against a shared horizon, the
               track that reaches it first is the track that is running
               ahead, and an aggregate cannot say which. */
            if (playback->pending[selected].kind == MEDIA_MP4_TRACK_VIDEO)
                playback->job_stats.horizon_break_video++;
            break;
        }
        /*
         * Video must decode forward from its keyframe after a seek. AAC
         * access units are independent, so submitting a pre-target unit
         * would instead queue audible content from before the requested
         * position. The secondary split source is contractually audio; the
         * kind check covers progressive MP4s where both tracks share source
         * zero.
         */
        if (selected_time < playback->audio_start_us
            && (selected != 0
                || playback->pending[selected].kind
                   == MEDIA_MP4_TRACK_AUDIO)) {
            playback->have_pending[selected] = false;
            processed++;
            continue;
        }
        if (playback->pending[selected].size
                > playback->packet_capacity) {
            size_t needed = playback->pending[selected].size;
            if (needed > playback->maximum_packet_bytes) {
                playback_error(
                    error, error_size,
                    "MP4 sample %zu > %zu-byte limit",
                    needed, playback->maximum_packet_bytes);
                return MEDIA_PLAYBACK_ADVANCE_ERROR;
            }
            unsigned char *grown = budget_realloc_category(
                playback->budget, BUDGET_CATEGORY_RESOURCE,
                playback->packet, needed);
            if (grown == NULL) {
                playback_error(
                    error, error_size,
                    "media packet growth exceeds budget");
                return MEDIA_PLAYBACK_ADVANCE_ERROR;
            }
            playback->packet = grown;
            playback->packet_capacity = needed;
        }
        if (!media_mp4_read_sample(
                playback->demux[selected], &playback->pending[selected],
                playback->packet, playback->packet_capacity)) {
            if (tilefinch_cancellation_requested(cancellation)) {
                return MEDIA_PLAYBACK_ADVANCE_CANCELLED;
            }
            /* The payload is not buffered. The sample stays pending, so the
               next pump re-reads exactly this one once the window lands. */
            if (media_mp4_would_block(playback->demux[selected])) {
                playback->job_stats.would_block_calls++;
                playback->job_stats.source_block_calls++;
                /* The sample is chosen and eligible and its payload is not
                   there: video eligible-but-not-buffered, exactly. */
                if (playback->pending[selected].kind
                    == MEDIA_MP4_TRACK_VIDEO)
                    playback->job_stats.source_block_video++;
                bool can_bypass = playback_note_head_block(
                    playback, selected, horizon);
                status = MEDIA_PLAYBACK_ADVANCE_PENDING;
                if (can_bypass
                    && deferred_source == playback->demux_count) {
                    deferred_source = selected;
                    playback->job_stats.head_alt_bypasses++;
                    continue;
                }
                break;
            }
            char demux_error[256] = {0};
            if (media_mp4_last_error(
                    playback->demux[selected],
                    demux_error, sizeof(demux_error))) {
                playback_error(error, error_size, "%s", demux_error);
            } else {
                playback_error(error, error_size, "MP4 sample read failed");
            }
            return MEDIA_PLAYBACK_ADVANCE_ERROR;
        }
        if (tilefinch_cancellation_requested(cancellation)) {
            return MEDIA_PLAYBACK_ADVANCE_CANCELLED;
        }
        MediaBackendResult result = playback->backend.submit(
            playback->backend.opaque, &playback->pending[selected],
            playback->packet, playback->pending[selected].size,
            error, error_size);
        if (result == MEDIA_BACKEND_WOULD_BLOCK) {
            playback->job_stats.would_block_calls++;
            playback->job_stats.submit_block_calls++;
            bool can_bypass = playback_note_head_block(
                playback, selected, horizon);
            status = MEDIA_PLAYBACK_ADVANCE_PENDING;
            if (deferred_source != playback->demux_count) {
                playback->job_stats.head_alt_blocked++;
            } else if (can_bypass) {
                deferred_source = selected;
                playback->job_stats.head_alt_bypasses++;
                continue;
            }
            break;
        }
        if (result == MEDIA_BACKEND_ERROR) {
            return MEDIA_PLAYBACK_ADVANCE_ERROR;
        }
        uint64_t submitted_time = playback->pending_time_us[selected];
        if (submitted_time > playback->buffered_until_us)
            playback->buffered_until_us = submitted_time;
        playback->have_pending[selected] = false;
        processed++;
        playback->job_stats.packets_submitted++;
        if (deferred_source != playback->demux_count
            && selected != deferred_source)
            playback->job_stats.head_alt_submitted++;
        /*
         * The backend took this unit without starting native work and says it
         * is waiting for a partner. Buy it one more packet of this visit.
         *
         * Asked after the submit rather than predicted before it, because the
         * answer is a property of what the submit just did -- and asked
         * again for every unit, so a backend that decides video now wants the
         * decoder stops being granted the extension immediately. It buys
         * budget and nothing else: the selection above is unchanged, so a
         * video access unit that is due first is still submitted first, and a
         * partner that is not buffered ends the visit on the same would-block
         * path as any other unbuffered sample rather than fetching for it.
         */
        if (result == MEDIA_BACKEND_ACCEPTED
            && paired_credit < MEDIA_PLAYBACK_MAXIMUM_PAIRED_SUBMITS
            && playback->backend.wants_paired_submit != NULL
            && playback->backend.wants_paired_submit(
                   playback->backend.opaque,
                   (int) playback->pending[selected].kind)) {
            paired_credit++;
            playback->job_stats.paired_submits++;
        }
        if (result == MEDIA_BACKEND_QUEUED) {
            /* The backend owns the copied packet. Yield before selecting a
               second source so its asynchronous native unit retains sole
               ownership of any shared staging or firmware state. */
            playback->job_stats.queued_breaks++;
            status = MEDIA_PLAYBACK_ADVANCE_PENDING;
            break;
        }
        /*
         * The backend owns an accepted packet even if cancellation arrived
         * while a native decoder call was in flight. Keep accounting
         * monotonic and stop before submitting another packet.
         */
        if (tilefinch_cancellation_requested(cancellation)) {
            return MEDIA_PLAYBACK_ADVANCE_CANCELLED;
        }
        if (result == MEDIA_BACKEND_END) {
            playback->ended = true;
            break;
        }
    }
    if (tilefinch_cancellation_requested(cancellation)) {
        return MEDIA_PLAYBACK_ADVANCE_CANCELLED;
    }
    if (!playback->backend.advance(
            playback->backend.opaque, clock_us, error, error_size)) {
        return MEDIA_PLAYBACK_ADVANCE_ERROR;
    }
    if (tilefinch_cancellation_requested(cancellation)) {
        return MEDIA_PLAYBACK_ADVANCE_CANCELLED;
    }
    if (status == MEDIA_PLAYBACK_ADVANCE_PENDING) {
        playback->job_stats.yielded_calls++;
    }
    return status;
}

void media_playback_job_stats(const MediaPlayback *playback,
                              MediaPlaybackJobStats *stats)
{
    if (stats == NULL) return;
    *stats = playback == NULL
        ? (MediaPlaybackJobStats) {0} : playback->job_stats;
}

bool media_playback_backend_stats(const MediaPlayback *playback,
                                  MediaBackendStats *stats)
{
    if (stats == NULL) return false;
    *stats = (MediaBackendStats) {0};
    return playback != NULL && playback->backend.stats != NULL
        && playback->backend.stats(playback->backend.opaque, stats);
}

bool media_playback_audio_cursor_us(const MediaPlayback *playback,
                                    uint64_t *cursor_us)
{
    return playback != NULL && cursor_us != NULL
        && playback->backend.audio_cursor_us != NULL
        && playback->backend.audio_cursor_us(
               playback->backend.opaque, cursor_us);
}

unsigned media_playback_ready_video_frames(const MediaPlayback *playback)
{
    return playback != NULL
        && playback->backend.ready_video_frames != NULL
        ? playback->backend.ready_video_frames(playback->backend.opaque)
        : 0;
}

bool media_playback_ready_video_start_us(
    const MediaPlayback *playback, uint64_t *start_us)
{
    return playback != NULL && start_us != NULL
        && playback->backend.ready_video_start_us != NULL
        && playback->backend.ready_video_start_us(
            playback->backend.opaque, start_us);
}

size_t media_playback_displayed_video_frames(const MediaPlayback *playback)
{
    return playback != NULL
        && playback->backend.displayed_video_frames != NULL
        ? playback->backend.displayed_video_frames(
              playback->backend.opaque)
        : 0;
}

void media_playback_set_presentation_clock_us(
    MediaPlayback *playback, uint64_t clock_us)
{
    if (playback != NULL
        && playback->backend.set_presentation_clock_us != NULL) {
        playback->backend.set_presentation_clock_us(
            playback->backend.opaque, clock_us);
    }
}

bool media_playback_has_audio(const MediaPlayback *playback)
{
    return playback != NULL && playback->has_audio;
}

bool media_playback_set_audio_submission_blocked(
    MediaPlayback *playback, bool blocked)
{
    if (playback == NULL) return false;
    if (blocked && playback->demux_count < 2u) return false;
    playback->audio_submission_blocked = blocked;
    return true;
}

bool media_playback_take_video_frame(MediaPlayback *playback,
                                     MediaVideoFrame *frame)
{
    return playback != NULL && frame != NULL
        && playback->backend.take_video_frame != NULL
        && playback->backend.take_video_frame(
            playback->backend.opaque, frame);
}

size_t media_playback_discard_video_before(
    MediaPlayback *playback, uint64_t floor_us)
{
    return playback != NULL && floor_us != 0
        && playback->backend.discard_video_before != NULL
        ? playback->backend.discard_video_before(
              playback->backend.opaque, floor_us)
        : 0;
}

bool media_playback_release_video_slot(
    MediaPlayback *playback, unsigned slot, uint32_t generation)
{
    return playback != NULL
        && playback->backend.release_video_slot != NULL
        && playback->backend.release_video_slot(
            playback->backend.opaque, slot, generation);
}

bool media_playback_emit_pending_video(MediaPlayback *playback)
{
    /* The error text is deliberately dropped. This is an opportunity, not a
       request: a backend that cannot convert right now leaves the picture
       captured and the ordinary submit or drain visit reports anything real
       that went wrong, on the path that already knows how to fail. */
    char error[128];
    error[0] = '\0';
    return playback != NULL
        && playback->backend.emit_pending_video != NULL
        && playback->backend.emit_pending_video(
            playback->backend.opaque, error, sizeof(error));
}

bool media_playback_ended(const MediaPlayback *playback)
{
    return playback == NULL || playback->ended;
}

uint64_t media_playback_buffered_until_us(const MediaPlayback *playback)
{
    if (playback == NULL) return 0;
    uint64_t buffered = playback->buffered_until_us;
    for (size_t source = 0; source < playback->demux_count; source++) {
        if (playback->have_pending[source]) {
            uint64_t pending = playback->pending_time_us[source];
            if (pending > buffered) buffered = pending;
        }
    }
    return buffered;
}

static bool playback_warm_source(
    MediaPlayback *playback, size_t index, uint64_t target_us,
    char *error, size_t error_size)
{
    if (playback == NULL || index >= playback->demux_count
        || playback->demux[index] == NULL || playback->packet == NULL) {
        return false;
    }
    MediaMp4Sample sample;
    if (!media_mp4_next_sample(playback->demux[index], &sample)) {
        /* The window has not arrived, or the source ended. Neither is a
           reason to fail the open; the playing path will make the connection
           if this could not. Restore the cursor either way. */
        (void) media_mp4_seek_us(playback->demux[index], target_us, NULL);
        return false;
    }
    bool warmed = false;
    if (sample.size <= playback->packet_capacity) {
        warmed = media_mp4_read_sample_waiting(
            playback->demux[index], &sample, playback->packet,
            playback->packet_capacity);
    }
    /* The read advanced past the keyframe; put the cursor back so the decode
       that follows begins at the target. The sidx window is cached, so this
       re-seek is local. */
    uint64_t restored = 0;
    if (media_mp4_seek_us(playback->demux[index], target_us, &restored)) {
        memset(playback->have_pending, 0, sizeof(playback->have_pending));
        memset(playback->source_ended, 0, sizeof(playback->source_ended));
    } else if (error != NULL && error_size != 0) {
        /* A failed restore is the caller's to notice; leave a message but do
           not manufacture a return code the warm has no use for. */
        (void) media_mp4_last_error(
            playback->demux[index], error, error_size);
    }
    return warmed;
}

bool media_playback_warm_video(MediaPlayback *playback, uint64_t target_us,
                               char *error, size_t error_size)
{
    return playback_warm_source(playback, 0, target_us, error, error_size);
}

bool media_playback_warm_audio(MediaPlayback *playback, uint64_t target_us,
                               char *error, size_t error_size)
{
    /* Only an adaptive stream has a second source to warm; a progressive MP4
       carries audio in source zero, which warming video already reached. */
    if (playback == NULL || playback->demux_count < 2) return false;
    return playback_warm_source(playback, 1, target_us, error, error_size);
}

static bool media_playback_seek_internal(MediaPlayback *playback,
                                         uint64_t target_us,
                                         bool strictly_after,
                                         uint64_t *actual_us, char *error,
                                         size_t error_size)
{
    if (playback == NULL || playback->backend.reset == NULL) {
        playback_error(error, error_size,
                       "media backend cannot reset for seeking");
        return false;
    }
    uint64_t selected_us = target_us;
    if (!playback->backend.reset(
            playback->backend.opaque, error, error_size)
        || !(strictly_after
             ? media_mp4_seek_after_us(
                   playback->demux[0], target_us, &selected_us)
             : media_mp4_seek_us(
                   playback->demux[0], target_us, &selected_us))) {
        char demux_error[256] = {0};
        if (media_mp4_last_error(
                playback->demux[0],
                demux_error, sizeof(demux_error))) {
            playback_error(error, error_size, "%s", demux_error);
        } else if (error != NULL && error_size != 0 && error[0] == '\0') {
            playback_error(error, error_size, "MP4 seek failed");
        }
        media_playback_fail_seek(playback);
        return false;
    }
    for (size_t source = 1; source < playback->demux_count; source++) {
        if (!media_mp4_seek_us(
                playback->demux[source], selected_us, NULL)) {
            char demux_error[256] = {0};
            if (media_mp4_last_error(
                    playback->demux[source],
                    demux_error, sizeof(demux_error))) {
                playback_error(error, error_size, "%s", demux_error);
            } else {
                playback_error(
                    error, error_size, "adaptive MP4 seek failed");
            }
            media_playback_fail_seek(playback);
            return false;
        }
    }
    memset(playback->have_pending, 0, sizeof(playback->have_pending));
    memset(playback->source_ended, 0, sizeof(playback->source_ended));
    playback->buffered_until_us =
        selected_us;
    /* An ordinary scrub may begin audio at the user's exact target even when
       video rounded backward to a keyframe. Recovery is the opposite: the
       audio demux rounds to its unit immediately before the selected video
       point, and that pre-landing unit must not play ahead of the recovered
       picture. */
    playback->audio_start_us = strictly_after ? selected_us : target_us;
    playback->ended = false;
    if (actual_us != NULL) *actual_us = selected_us;
    return true;
}

bool media_playback_seek(MediaPlayback *playback, uint64_t target_us,
                         uint64_t *actual_us, char *error,
                         size_t error_size)
{
    return media_playback_seek_internal(
        playback, target_us, false, actual_us, error, error_size);
}

bool media_playback_seek_after(MediaPlayback *playback, uint64_t target_us,
                               uint64_t *actual_us, char *error,
                               size_t error_size)
{
    return media_playback_seek_internal(
        playback, target_us, true, actual_us, error, error_size);
}

size_t media_playback_packet_bytes(const MediaPlayback *playback)
{
    return playback == NULL ? 0 : playback->packet_capacity;
}

void media_playback_set_playing(MediaPlayback *playback, bool playing)
{
    if (playback == NULL) return;
    playback->playing = playing;
    if (playback->backend.set_playing != NULL)
        playback->backend.set_playing(playback->backend.opaque, playing);
}

void media_playback_set_buffering(MediaPlayback *playback, bool buffering)
{
    if (playback != NULL && playback->backend.set_buffering != NULL)
        playback->backend.set_buffering(
            playback->backend.opaque, buffering);
}

void media_playback_destroy(MediaPlayback *playback)
{
    if (playback == NULL) return;
    playback->backend.destroy(playback->backend.opaque);
    budget_free(playback->budget, playback->packet);
    Budget *budget = playback->budget;
    memset(playback, 0, sizeof(*playback));
    budget_free(budget, playback);
}
