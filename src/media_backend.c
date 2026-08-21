#include "tilefinch/media_backend.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

struct MediaPlayback {
    Budget *budget;
    MediaSampleSource source[2];
    size_t source_count;
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

static bool source_valid(const MediaSampleSource *source)
{
    return source != NULL && source->opaque != NULL && source->ops != NULL
        && source->ops->track_count != NULL
        && source->ops->track_info != NULL
        && source->ops->next_sample != NULL
        && source->ops->last_error != NULL
        && source->ops->would_block != NULL
        && source->ops->sample_resident != NULL
        && source->ops->read_sample_waiting != NULL
        && source->ops->read_sample != NULL
        && source->ops->seek_us != NULL
        && source->ops->seek_after_us != NULL;
}

static void playback_error(char *error, size_t error_size,
                           const char *format, ...)
{
    if (error == NULL || error_size == 0) return;
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(error, error_size, format, arguments);
    va_end(arguments);
}

static bool playback_ensure_packet_capacity(
    MediaPlayback *playback, size_t needed, char *error, size_t error_size)
{
    if (needed <= playback->packet_capacity) return true;
    if (needed > playback->maximum_packet_bytes) {
        playback_error(error, error_size,
                       "MP4 sample %zu > %zu-byte limit",
                       needed, playback->maximum_packet_bytes);
        return false;
    }
    unsigned char *grown = budget_realloc_category(
        playback->budget, BUDGET_CATEGORY_RESOURCE,
        playback->packet, needed);
    if (grown == NULL) {
        playback_error(error, error_size,
                       "media packet growth exceeds budget");
        return false;
    }
    playback->packet = grown;
    playback->packet_capacity = needed;
    return true;
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
    if (playback->source_count < 2) return false;
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
    const MediaSampleSource *other_source = &playback->source[other];
    if (!other_source->ops->sample_resident(
            other_source->opaque, &playback->pending[other])) return false;
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

MediaPlayback *media_playback_create_sources(
    Budget *budget, const MediaSampleSource *video_source,
    const MediaSampleSource *audio_source, const MediaBackend *backend,
    const MediaPlaybackOptions *options, char *error, size_t error_size)
{
    if (error != NULL && error_size != 0) error[0] = '\0';
    if (budget == NULL || !source_valid(video_source) || backend == NULL
        || backend->submit == NULL || backend->advance == NULL
        || backend->destroy == NULL) {
        playback_error(error, error_size, "media: invalid backend");
        return NULL;
    }
    size_t largest = 0;
    bool has_audio = audio_source != NULL;
    const MediaSampleSource *sources[2] = {video_source, audio_source};
    size_t source_count = audio_source == NULL ? 1u : 2u;
    if (audio_source != NULL && !source_valid(audio_source)) {
        playback_error(error, error_size, "media: invalid audio source");
        return NULL;
    }
    for (size_t source = 0; source < source_count; source++) {
        for (size_t i = 0;
             i < sources[source]->ops->track_count(
                     sources[source]->opaque); i++) {
            MediaMp4TrackInfo info;
            if (!sources[source]->ops->track_info(
                    sources[source]->opaque, i, &info)) {
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
    playback->source[0] = *video_source;
    if (audio_source != NULL) playback->source[1] = *audio_source;
    playback->source_count = source_count;
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
    MediaSampleSource source;
    if (!media_sample_source_from_mp4(demux, &source)) {
        playback_error(error, error_size, "media: invalid MP4 source");
        return NULL;
    }
    return media_playback_create_sources(
        budget, &source, NULL, backend, options, error, error_size);
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
    MediaSampleSource video_source, audio_source;
    if (!media_sample_source_from_mp4(video_demux, &video_source)
        || !media_sample_source_from_mp4(audio_demux, &audio_source)) {
        playback_error(error, error_size, "media: invalid MP4 source");
        return NULL;
    }
    return media_playback_create_sources(
        budget, &video_source, &audio_source, backend,
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
    size_t deferred_source = playback->source_count;
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
        for (size_t source = 0; source < playback->source_count; source++) {
            if (!playback->have_pending[source]
                && !playback->source_ended[source]) {
                MediaSampleSource *input = &playback->source[source];
                if (!input->ops->next_sample(
                        input->opaque, &playback->pending[source])) {
                    if (tilefinch_cancellation_requested(cancellation)) {
                        return MEDIA_PLAYBACK_ADVANCE_CANCELLED;
                    }
                    /*
                     * Unbuffered bytes are neither the end of the source nor a
                     * transport failure. Leave the source open with nothing
                     * pending and let the pump retire this unit: the fetch the
                     * demuxer just asked for proceeds while the decoder works.
                     */
                    if (input->ops->would_block(input->opaque)) {
                        source_blocked = true;
                        /* Source zero is the video demuxer in both shapes:
                           the split form is contractually video-then-audio,
                           and a progressive MP4 carries both tracks there. */
                        if (source == 0)
                            playback->job_stats.source_block_video++;
                        continue;
                    }
                    char demux_error[256] = {0};
                    if (input->ops->last_error(
                            input->opaque,
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
        size_t selected = playback->source_count;
        uint64_t selected_time = UINT64_MAX;
        bool held_audio_pending = false;
        for (size_t source = 0; source < playback->source_count; source++) {
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
            if (selected == playback->source_count
                || candidate < selected_time) {
                selected = source;
                selected_time = candidate;
            }
        }
        if (selected == playback->source_count
            && deferred_source != playback->source_count) {
            /* The other source advanced as far as this bounded call allowed;
               the original blocked head is intentionally still pending for
               the next call. It is not end-of-stream and must never drain. */
            status = MEDIA_PLAYBACK_ADVANCE_PENDING;
            break;
        }
        if (selected == playback->source_count && held_audio_pending) {
            /* The video source is caught up for this horizon. Audio is
               intentionally retained, not EOF and not a drain condition. */
            status = MEDIA_PLAYBACK_ADVANCE_PENDING;
            break;
        }
        if (selected == playback->source_count && source_blocked) {
            /* Nothing to submit and at least one source is still fetching:
               this is a pending pipeline, not a drained one. Draining here
               would publish end-of-stream in the middle of a refill. */
            playback->job_stats.would_block_calls++;
            playback->job_stats.source_block_calls++;
            status = MEDIA_PLAYBACK_ADVANCE_PENDING;
            break;
        }
        if (selected == playback->source_count) {
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
        if (!playback_ensure_packet_capacity(
                playback, playback->pending[selected].size,
                error, error_size)) return MEDIA_PLAYBACK_ADVANCE_ERROR;
        MediaSampleSource *selected_source = &playback->source[selected];
        if (!selected_source->ops->read_sample(
                selected_source->opaque, &playback->pending[selected],
                playback->packet, playback->packet_capacity)) {
            if (tilefinch_cancellation_requested(cancellation)) {
                return MEDIA_PLAYBACK_ADVANCE_CANCELLED;
            }
            /* The payload is not buffered. The sample stays pending, so the
               next pump re-reads exactly this one once the window lands. */
            if (selected_source->ops->would_block(selected_source->opaque)) {
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
                    && deferred_source == playback->source_count) {
                    deferred_source = selected;
                    playback->job_stats.head_alt_bypasses++;
                    continue;
                }
                break;
            }
            char demux_error[256] = {0};
            if (selected_source->ops->last_error(
                    selected_source->opaque,
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
            if (deferred_source != playback->source_count) {
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
        if (deferred_source != playback->source_count
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

unsigned media_playback_startup_ready_frames(const MediaPlayback *playback)
{
    return playback == NULL ? 0 : playback->backend.startup_ready_frames;
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
    if (blocked && playback->source_count < 2u) return false;
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

bool media_playback_borrow_video_slot(
    MediaPlayback *playback, unsigned slot, uint32_t generation)
{
    const MediaBackendPresentationOps *ops = playback != NULL
        ? playback->backend.presentation : NULL;
    return ops == NULL || ops->borrow == NULL
        || ops->borrow(playback->backend.opaque, slot, generation);
}

void media_playback_release_video_read(
    MediaPlayback *playback, unsigned slot)
{
    const MediaBackendPresentationOps *ops = playback != NULL
        ? playback->backend.presentation : NULL;
    if (ops != NULL && ops->release != NULL)
        ops->release(playback->backend.opaque, slot);
}

void media_playback_end_auxiliary_video_read(
    MediaPlayback *playback, unsigned slot)
{
    const MediaBackendPresentationOps *ops = playback != NULL
        ? playback->backend.presentation : NULL;
    if (ops != NULL && ops->end_auxiliary_read != NULL)
        ops->end_auxiliary_read(playback->backend.opaque, slot);
}

void media_playback_quarantine_video_slot(
    MediaPlayback *playback, unsigned slot)
{
    const MediaBackendPresentationOps *ops = playback != NULL
        ? playback->backend.presentation : NULL;
    if (ops != NULL && ops->quarantine != NULL)
        ops->quarantine(playback->backend.opaque, slot);
}

void media_playback_release_video_slot_quarantine(
    MediaPlayback *playback, unsigned slot)
{
    const MediaBackendPresentationOps *ops = playback != NULL
        ? playback->backend.presentation : NULL;
    if (ops != NULL && ops->release_quarantine != NULL)
        ops->release_quarantine(playback->backend.opaque, slot);
}

bool media_playback_video_slot_quarantined(
    const MediaPlayback *playback, unsigned slot)
{
    const MediaBackendPresentationOps *ops = playback != NULL
        ? playback->backend.presentation : NULL;
    return ops != NULL && ops->is_quarantined != NULL
        && ops->is_quarantined(playback->backend.opaque, slot);
}

void media_playback_note_frame_staged(
    MediaPlayback *playback, const MediaVideoFrame *frame)
{
    const MediaBackendPresentationOps *ops = playback != NULL
        ? playback->backend.presentation : NULL;
    if (ops != NULL && ops->note_staged != NULL)
        ops->note_staged(playback->backend.opaque, frame);
}

void media_playback_note_frame_displayed(
    MediaPlayback *playback, const MediaVideoFrame *frame, int present_path)
{
    const MediaBackendPresentationOps *ops = playback != NULL
        ? playback->backend.presentation : NULL;
    if (ops != NULL && ops->note_displayed != NULL)
        ops->note_displayed(playback->backend.opaque, frame, present_path);
}

void media_playback_note_frame_quiesced(
    MediaPlayback *playback, const MediaVideoFrame *frame)
{
    const MediaBackendPresentationOps *ops = playback != NULL
        ? playback->backend.presentation : NULL;
    if (ops != NULL && ops->note_quiesced != NULL)
        ops->note_quiesced(playback->backend.opaque, frame);
}

void media_playback_note_stage_signature(
    MediaPlayback *playback, const MediaVideoFrame *frame,
    uint32_t signature)
{
    const MediaBackendPresentationOps *ops = playback != NULL
        ? playback->backend.presentation : NULL;
    if (ops != NULL && ops->note_stage_signature != NULL)
        ops->note_stage_signature(playback->backend.opaque, frame, signature);
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
    for (size_t source = 0; source < playback->source_count; source++) {
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
    if (playback == NULL || index >= playback->source_count
        || !source_valid(&playback->source[index])
        || playback->packet == NULL) {
        return false;
    }
    MediaMp4Sample sample;
    MediaSampleSource *source = &playback->source[index];
    if (!source->ops->next_sample(source->opaque, &sample)) {
        /* The window has not arrived, or the source ended. Neither is a
           reason to fail the open; the playing path will make the connection
           if this could not. Restore the cursor either way. */
        (void) source->ops->seek_us(source->opaque, target_us, NULL);
        return false;
    }
    bool warmed = false;
    if (sample.size <= playback->packet_capacity) {
        warmed = source->ops->read_sample_waiting(
            source->opaque, &sample, playback->packet,
            playback->packet_capacity);
    }
    /* The read advanced past the keyframe; put the cursor back so the decode
       that follows begins at the target. The sidx window is cached, so this
       re-seek is local. */
    uint64_t restored = 0;
    if (source->ops->seek_us(source->opaque, target_us, &restored)) {
        memset(playback->have_pending, 0, sizeof(playback->have_pending));
        memset(playback->source_ended, 0, sizeof(playback->source_ended));
    } else if (error != NULL && error_size != 0) {
        /* A failed restore is the caller's to notice; leave a message but do
           not manufacture a return code the warm has no use for. */
        (void) source->ops->last_error(
            source->opaque, error, error_size);
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
    if (playback == NULL || !playback->has_audio) return false;
    /* Split A/V keeps AAC in source one. An audio-only pipeline deliberately
       installs that same adaptive AAC demux as its sole source. Progressive
       A/V remains source zero and callers warm it through warm_video. */
    if (playback->source_count < 2) {
        return playback_warm_source(
            playback, 0, target_us, error, error_size);
    }
    return playback_warm_source(
        playback, 1, target_us, error, error_size);
}

#define MEDIA_PLAYBACK_PRIME_MAXIMUM_SKIPPED_SAMPLES 64u

static MediaPlaybackSourcePrimeStatus playback_prime_source(
    MediaPlayback *playback, size_t index, uint64_t target_us,
    bool at_or_after_target, bool absent_is_ready, uint64_t *sample_us,
    char *error, size_t error_size)
{
    if (sample_us != NULL) *sample_us = target_us;
    if (playback == NULL || playback->packet == NULL) {
        playback_error(error, error_size, "media source is unavailable");
        return MEDIA_PLAYBACK_SOURCE_PRIME_FAILED;
    }
    if (index >= playback->source_count
        || !source_valid(&playback->source[index])) {
        if (absent_is_ready) return MEDIA_PLAYBACK_SOURCE_PRIME_READY;
        playback_error(error, error_size, "media video source is unavailable");
        return MEDIA_PLAYBACK_SOURCE_PRIME_FAILED;
    }
    MediaSampleSource *source = &playback->source[index];
    if (!source->ops->seek_us(source->opaque, target_us, NULL)) {
        if (source->ops->would_block(source->opaque))
            return MEDIA_PLAYBACK_SOURCE_PRIME_PENDING;
        if (!source->ops->last_error(source->opaque, error, error_size))
            playback_error(error, error_size,
                           "media source could not seek for priming");
        return MEDIA_PLAYBACK_SOURCE_PRIME_FAILED;
    }

    MediaMp4Sample sample = {0};
    bool found = false;
    bool scan_limit_reached = false;
    for (unsigned skipped = 0;
         skipped <= MEDIA_PLAYBACK_PRIME_MAXIMUM_SKIPPED_SAMPLES;
         skipped++) {
        if (!source->ops->next_sample(source->opaque, &sample)) break;
        uint64_t time_us = sample_time_us(&sample);
        if (!at_or_after_target || time_us >= target_us) {
            found = true;
            if (sample_us != NULL) *sample_us = time_us;
            break;
        }
        if (skipped == MEDIA_PLAYBACK_PRIME_MAXIMUM_SKIPPED_SAMPLES)
            scan_limit_reached = true;
    }
    if (!found) {
        bool pending = source->ops->would_block(source->opaque);
        bool failed = source->ops->last_error(
            source->opaque, error, error_size);
        (void) source->ops->seek_us(source->opaque, target_us, NULL);
        if (pending) return MEDIA_PLAYBACK_SOURCE_PRIME_PENDING;
        if (scan_limit_reached) {
            playback_error(error, error_size,
                           "media source prime scan exceeded %u samples",
                           MEDIA_PLAYBACK_PRIME_MAXIMUM_SKIPPED_SAMPLES);
            return MEDIA_PLAYBACK_SOURCE_PRIME_FAILED;
        }
        /* End-of-track is a terminally known source head, not a delivery
           failure. This is reachable near the end of a split stream when
           audio legitimately finishes before the last video frame. The
           ordinary pump will rediscover EOF after the cursor restore. */
        return failed ? MEDIA_PLAYBACK_SOURCE_PRIME_FAILED
                      : MEDIA_PLAYBACK_SOURCE_PRIME_READY;
    }
    if (!playback_ensure_packet_capacity(
            playback, sample.size, error, error_size)) {
        (void) source->ops->seek_us(source->opaque, target_us, NULL);
        return MEDIA_PLAYBACK_SOURCE_PRIME_FAILED;
    }

    bool ready = source->ops->sample_resident(source->opaque, &sample);
    bool pending = false;
    bool failed = false;
    if (!ready) {
        ready = source->ops->read_sample(
            source->opaque, &sample, playback->packet,
            playback->packet_capacity);
        pending = !ready && source->ops->would_block(source->opaque);
        if (!ready && !pending)
            failed = source->ops->last_error(
                source->opaque, error, error_size);
    }
    if (!source->ops->seek_us(source->opaque, target_us, NULL)) {
        bool restore_pending = source->ops->would_block(source->opaque);
        if (restore_pending) {
            ready = false;
            pending = true;
        } else {
            if (!source->ops->last_error(
                    source->opaque, error, error_size))
                playback_error(error, error_size,
                               "media source could not restore seek target");
            return MEDIA_PLAYBACK_SOURCE_PRIME_FAILED;
        }
    }
    memset(playback->have_pending, 0, sizeof(playback->have_pending));
    memset(playback->source_ended, 0, sizeof(playback->source_ended));
    if (ready) return MEDIA_PLAYBACK_SOURCE_PRIME_READY;
    if (pending) return MEDIA_PLAYBACK_SOURCE_PRIME_PENDING;
    if (!failed)
        playback_error(error, error_size, "media source priming failed");
    return MEDIA_PLAYBACK_SOURCE_PRIME_FAILED;
}

MediaPlaybackSourcePrimeStatus media_playback_prime_video_source(
    MediaPlayback *playback, uint64_t target_us, uint64_t *sample_us,
    char *error, size_t error_size)
{
    return playback_prime_source(
        playback, 0u, target_us, false, false, sample_us,
        error, error_size);
}

MediaPlaybackSourcePrimeStatus media_playback_prime_audio_source(
    MediaPlayback *playback, uint64_t target_us, uint64_t *sample_us,
    char *error, size_t error_size)
{
    size_t source = playback != NULL && playback->has_audio
            && playback->source_count == 1u
        ? 0u : 1u;
    return playback_prime_source(
        playback, source, target_us, true, true, sample_us,
        error, error_size);
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
             ? playback->source[0].ops->seek_after_us(
                   playback->source[0].opaque, target_us, &selected_us)
             : playback->source[0].ops->seek_us(
                   playback->source[0].opaque, target_us, &selected_us))) {
        char demux_error[256] = {0};
        if (playback->source[0].ops->last_error(
                playback->source[0].opaque,
                demux_error, sizeof(demux_error))) {
            playback_error(error, error_size, "%s", demux_error);
        } else if (error != NULL && error_size != 0 && error[0] == '\0') {
            playback_error(error, error_size, "MP4 seek failed");
        }
        media_playback_fail_seek(playback);
        return false;
    }
    for (size_t source = 1; source < playback->source_count; source++) {
        if (!playback->source[source].ops->seek_us(
                playback->source[source].opaque, selected_us, NULL)) {
            char demux_error[256] = {0};
            if (playback->source[source].ops->last_error(
                    playback->source[source].opaque,
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
