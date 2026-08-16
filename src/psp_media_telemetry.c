#include "psp_media_session_internal.h"

#include <limits.h>
#include <stdio.h>

#include "media_backend_psp_policy.h"
#include "tilefinch/psp_log.h"

#define printf psp_log_printf

/*
 * Say what limited the unit rate.
 *
 * A device log that reports calls, yielded, packets and would-block can show
 * a starved decoder but not name the reason: the same shape appears whether
 * the pipeline was paused, held by a busy Media Engine, or refused by its own
 * decode horizon. This line carries one counter per exit from the bounded
 * loop, plus what the pump got through per browser frame and whether the
 * bounded collect wait was long enough to be worth taking.
 *
 * Its own line rather than more fields on tilefinch-media-job, so the format
 * every existing reader parses is untouched. Emitted while playing as well as
 * at teardown, because a device session that is killed mid-playback still has
 * to answer the question.
 */
/*
 * Name a unit that blocked.
 *
 * A pump unit is supposed to be bounded: every read the playing path performs
 * passes may_wait false and answers WOULD_BLOCK, and both of those paths were
 * checked. A device cycle still recorded one unit at 478,347us with a frame
 * period of 500,488us around it, so something below the demuxer waits. The
 * transport now measures its own pump and its window install, and this prints
 * them beside the unit that was slow -- which is what turns "a unit blocked"
 * into the name of the call that blocked.
 *
 * Bounded to a handful of lines per session: the first few are the evidence
 * and the rest would be the log.
 */
#define PSP_MEDIA_SLOW_UNIT_US 20000u
#define PSP_MEDIA_SLOW_UNIT_REPORTS 6u

void psp_media_telemetry_report_slow_unit(
    PspMediaSession *media, const char *stage, uint64_t unit_us)
{
    if (media == NULL || unit_us < PSP_MEDIA_SLOW_UNIT_US
        || media->slow_unit_reports >= PSP_MEDIA_SLOW_UNIT_REPORTS) return;
    media->slow_unit_reports++;
    MediaHttpRangeStats video = {0};
    MediaHttpRangeStats audio = {0};
    (void) media_http_range_stats(media->range, &video);
    (void) media_http_range_stats(media->audio_range, &audio);
    printf(
        "tilefinch-media-slow-unit: stage=%s unit=%lluus "
        "video-pump-max=%lluus video-install-max=%lluus video-pumps=%zu "
        "audio-pump-max=%lluus audio-install-max=%lluus audio-pumps=%zu "
        "video-new-conn=%zu video-handshakes=%zu video-handshake-max=%lluus "
        "audio-new-conn=%zu audio-handshakes=%zu audio-handshake-max=%lluus "
        "window-pending=%d/%d inflight=%zu/%zu\n",
        stage == NULL ? "unknown" : stage,
        (unsigned long long) unit_us,
        (unsigned long long) video.pump_max_us,
        (unsigned long long) video.install_max_us, video.pump_calls,
        (unsigned long long) audio.pump_max_us,
        (unsigned long long) audio.install_max_us, audio.pump_calls,
        video.window_new_connections, video.window_handshakes,
        (unsigned long long) video.handshake_max_us,
        audio.window_new_connections, audio.window_handshakes,
        (unsigned long long) audio.handshake_max_us,
        video.window_pending ? 1 : 0, audio.window_pending ? 1 : 0,
        video.bytes_in_flight, audio.bytes_in_flight);
}

static unsigned long long psp_media_cadence_mean(
    uint64_t total_us, size_t samples)
{
    return samples == 0 ? 0ull
        : (unsigned long long) (total_us / (uint64_t) samples);
}

/*
 * Where one picture's 51 milliseconds go.
 *
 * The stream is 24 fps and the screen gets 19 to 20, and every knob turned this
 * session moved that by nothing -- so the question stopped being which knob and
 * became which segment of the journey is long. A picture is submitted, firmware
 * decodes it, the colour conversion puts it in the one shared surface, the loop
 * takes it, the presenter borrows and releases it, and only then may the next
 * access unit go in. Those five intervals are measured end to end and printed
 * as count/total/max triples with their means beside them, so seg-mean-sum and
 * period-mean are the same number and whichever term dominates is the answer.
 *
 * Beside them, the two accounts that say why an interval is long: which of the
 * four holds spent each video-submit opportunity -- the aggregate on the feed
 * line could never tell them apart -- and how many pictures a decode returned,
 * since the surface holds one and every surplus picture costs a further visit.
 *
 * Its own line rather than more fields on tilefinch-media-feed, which is
 * already at the edge of readable and which existing readers parse by position.
 * Same call site, same five-second cadence, same phase word.
 */
static void psp_media_report_video_cadence(
    const char *phase, const MediaBackendStats *stats)
{
    unsigned long long decode_mean = psp_media_cadence_mean(
        stats->video_decode_total_us, stats->video_decode_jobs);
    unsigned long long emit_mean = psp_media_cadence_mean(
        stats->video_emit_pending_total_us, stats->video_emits);
    unsigned long long take_mean = psp_media_cadence_mean(
        stats->video_take_age_total_us, stats->video_take_satisfied);
    unsigned long long present_mean = psp_media_cadence_mean(
        stats->video_present_total_us, stats->video_presents);
    unsigned long long reoffer_mean = psp_media_cadence_mean(
        stats->video_reoffer_total_us, stats->video_reoffers);
    unsigned long long period_mean = psp_media_cadence_mean(
        stats->video_submit_period_total_us, stats->video_submit_periods);
    printf(
        "tilefinch-media-video-cadence: phase=%s "
        "take-calls=%zu take-ok=%zu take-empty=%zu take-stale=%zu "
        "take-busy=%zu take-early=%zu "
        "emit=%zu emit-worker=%zu emit-submit=%zu emit-drain=%zu "
        "emit-release=%zu emit-defer-borrow=%zu emit-starved=%zu "
        "claims=%zu staged=%zu displayed=%zu dropped=%zu quiesced=%zu "
        "recoveries=%zu rec-reset=%lluus rec-first-au=%lluus "
        "rec-first-csc=%lluus rec-outage=%lluus rec-outage-max=%uus "
        "rec-media-skip=%lluus "
        "batches=%zu batch-pics=%zu batch-multi=%zu batch-max=%u "
        "seg-decode=%zu/%lluus/%uus seg-emit=%zu/%lluus/%uus "
        "seg-take=%zu/%lluus/%uus seg-present=%zu/%lluus/%uus "
        "seg-reoffer=%zu/%lluus/%uus period=%zu/%lluus/%uus "
        "seg-mean=%llu/%llu/%llu/%llu/%llu seg-mean-sum=%lluus "
        "period-mean=%lluus "
        "hold-video=job:%zu/stage:%zu/refusal:%zu/frame:%zu/batch:%zu/ts:%zu "
        "hold-drain=job:%zu/stage:%zu/refusal:%zu/surface:%zu "
        /* The ownership machine. slot-csc/slot-take say whether the two
           surfaces are actually alternating -- a pair that stopped is a
           pipeline that has quietly gone back to one -- and the three
           integrity counters are the ones that must read zero: a stale job
           credited, a read of a slot that had moved on, a staged copy whose
           source was not the claimed picture, and pixels that did not match
           what the conversion left behind. */
        "slot-csc=%zu/%zu slot-take=%zu/%zu "
        "stale-jobs=%zu borrow-stale=%zu stage-mismatch=%zu "
        "sig-mismatch=%zu\n",
        phase == NULL ? "unknown" : phase,
        stats->video_take_calls, stats->video_take_satisfied,
        stats->video_take_empty, stats->video_take_stale,
        stats->video_take_busy, stats->video_take_early,
        stats->video_emits, stats->video_emits_worker,
        stats->video_emits_submit, stats->video_emits_drain,
        stats->video_emits_release,
        stats->surface_emit_deferrals, stats->video_emit_visit_starved,
        stats->video_claims, stats->video_claims_staged,
        stats->video_claims_displayed, stats->video_claims_dropped,
        stats->video_claims_quiesced,
        stats->video_recoveries,
        (unsigned long long) stats->recovery_reset_total_us,
        (unsigned long long) stats->recovery_first_au_total_us,
        (unsigned long long) stats->recovery_first_csc_total_us,
        (unsigned long long) stats->recovery_first_present_total_us,
        (unsigned) stats->recovery_first_present_max_us,
        (unsigned long long) stats->recovery_media_skip_total_us,
        stats->video_batches, stats->video_batch_pictures,
        stats->video_batch_multi, (unsigned) stats->video_batch_max,
        stats->video_decode_jobs,
        (unsigned long long) stats->video_decode_total_us,
        (unsigned) stats->video_decode_max_us,
        stats->video_emits,
        (unsigned long long) stats->video_emit_pending_total_us,
        (unsigned) stats->video_emit_pending_max_us,
        stats->video_take_satisfied,
        (unsigned long long) stats->video_take_age_total_us,
        (unsigned) stats->video_take_age_max_us,
        stats->video_presents,
        (unsigned long long) stats->video_present_total_us,
        (unsigned) stats->video_present_max_us,
        stats->video_reoffers,
        (unsigned long long) stats->video_reoffer_total_us,
        (unsigned) stats->video_reoffer_max_us,
        stats->video_submit_periods,
        (unsigned long long) stats->video_submit_period_total_us,
        (unsigned) stats->video_submit_period_max_us,
        decode_mean, emit_mean, take_mean, present_mean, reoffer_mean,
        decode_mean + emit_mean + take_mean + present_mean + reoffer_mean,
        period_mean,
        stats->video_hold_job_slot, stats->video_hold_stage_copy,
        stats->video_hold_refusal, stats->video_hold_frame_ready,
        stats->video_hold_batch_pending, stats->video_hold_timestamps,
        stats->drain_hold_job_slot, stats->drain_hold_stage_copy,
        stats->drain_hold_refusal, stats->drain_hold_surface,
        stats->video_slot_conversions[0], stats->video_slot_conversions[1],
        stats->video_slot_claims[0], stats->video_slot_claims[1],
        stats->stale_codec_jobs, stats->surface_borrow_stale,
        stats->stage_mismatches, stats->signature_mismatches);
}

/*
 * Where the pipeline's time goes, as opposed to what it did.
 *
 * tilefinch-media-video-cadence answers "how many" for every event this
 * pipeline has, and its answers now balance: conversions barely exceed
 * claims, the two slots alternate, every integrity counter reads zero, and
 * the stream still arrives at 24 pictures a second and reaches the panel at
 * 20. A pipeline that is doing the right thing at the wrong rate is not a
 * counting problem, and no count can be added that would settle it -- the
 * question is which microseconds were spent and which were not.
 *
 * So this line is entirely durations. Per-slot dwell in the four ownership
 * states, whose four totals sum to the window; the two conditions that
 * describe the pipeline rather than a slot -- nothing writable, and something
 * writable with nothing being written -- and the batch and conversion
 * intervals inside them. The last field says where in the browser's frame
 * each conversion finished, which is what decides whether the take that
 * wanted it is the next one or the one after.
 *
 * Its own line for the reason the cadence line has its own: readers parse
 * tilefinch-media-feed by position. Same call site, same five-second cadence,
 * same phase word, so one window's three lines read together.
 */
static void psp_media_report_slots(
    const char *phase, const MediaBackendStats *stats)
{
    printf(
        "tilefinch-media-slots: phase=%s "
        "slot0=free:%lluus/write:%lluus/ready:%lluus/read:%lluus "
        "slot1=free:%lluus/write:%lluus/ready:%lluus/read:%lluus "
        "no-free=%lluus free-idle=%lluus "
        "ready-depth=%zu/%zu/%u free-to-au=%zu/%lluus/%uus "
        "batch-jobs=%zu batch-in-job=%zu batch-complete=%zu "
        "batch-defer-free=%zu batch-gap=%zu/%lluus/%uus "
        "csc=%zu/%lluus/%uus "
        "csc-phase=other:%zu/pump:%zu/present:%zu/vblank:%zu\n",
        phase == NULL ? "unknown" : phase,
        (unsigned long long) stats->video_slot_dwell_us[0][0],
        (unsigned long long) stats->video_slot_dwell_us[0][1],
        (unsigned long long) stats->video_slot_dwell_us[0][2],
        (unsigned long long) stats->video_slot_dwell_us[0][3],
        (unsigned long long) stats->video_slot_dwell_us[1][0],
        (unsigned long long) stats->video_slot_dwell_us[1][1],
        (unsigned long long) stats->video_slot_dwell_us[1][2],
        (unsigned long long) stats->video_slot_dwell_us[1][3],
        (unsigned long long) stats->slot_no_free_us,
        (unsigned long long) stats->slot_free_idle_us,
        stats->slot_ready_samples, stats->slot_ready_total,
        (unsigned) stats->slot_ready_max,
        stats->slot_free_to_au,
        (unsigned long long) stats->slot_free_to_au_total_us,
        (unsigned) stats->slot_free_to_au_max_us,
        stats->batch_fill_jobs, stats->batch_fill_converted,
        stats->batch_fill_complete, stats->batch_defer_free,
        stats->batch_gap_samples,
        (unsigned long long) stats->batch_gap_total_us,
        (unsigned) stats->batch_gap_max_us,
        stats->video_csc_calls,
        (unsigned long long) stats->video_csc_total_us,
        (unsigned) stats->video_csc_max_us,
        stats->video_csc_phase[PSP_MEDIA_LOOP_PHASE_OTHER],
        stats->video_csc_phase[PSP_MEDIA_LOOP_PHASE_PUMP],
        stats->video_csc_phase[PSP_MEDIA_LOOP_PHASE_PRESENT],
        stats->video_csc_phase[PSP_MEDIA_LOOP_PHASE_VBLANK]);
}

/*
 * The other half: the worker, the one job slot the three codecs share, and
 * the supply behind them.
 *
 * A picture's journey crosses two thread boundaries, and until now neither
 * was measured. The decode timing began when the job was QUEUED, so it
 * carried the scheduler latency of a worker that had not been given the CPU
 * yet; the completion was observed whenever the browser thread next happened
 * to pump. Both are charged to "decode" in every number read so far, and both
 * have fixes that have nothing to do with the decoder.
 *
 * Beneath them, the shared job slot: one AAC job, one AVC job and one drain
 * job all queue through a single handoff, so an audio decode is a video
 * decode that did not happen. The split totals say how much of the window
 * each kind owned; the histogram percentiles say whether the audio jobs are
 * short and frequent or long and occasional, which is the difference between
 * a scheduling problem and a slot-contention one.
 *
 * The supply fields close the loop. Every hold above is a refusal to accept
 * an access unit; these are the access units that were never offered.
 */
static void psp_media_report_worker(
    const char *phase, const MediaBackendStats *stats,
    const MediaPlaybackJobStats *jobs,
    const MediaHttpRangeStats *video_range)
{
    printf(
        "tilefinch-media-worker: phase=%s "
        "dispatch=%zu/%lluus/%uus prologue=%zu/%lluus/%uus "
        "firmware=%zu/%lluus/%uus collect=%zu/%lluus/%uus "
        "job-video=%zu/%lluus job-drain=%zu/%lluus "
        "job-audio=%zu/%lluus job-other=%zu/%lluus "
        "audio-p50=%uus audio-p90=%uus audio-max=%uus "
        "hold-job=video:%zu/audio:%zu/drain:%zu/other:%zu "
        "hold-audio-age=%lluus/%uus "
        "supply=src-block-video:%zu/horizon-video:%zu "
        "http-video=%zu/%lluus/%lluus starved=%lluus/%lluus "
        "audio-batch=jobs:%zu/aus:%zu/defer:%zu/flush:%zu/budget:%zu "
        "batch-blocked=video:%zu/queue:%zu paired-submits=%zu "
        "pending=in:%zu/promote:%zu/full:%zu/peak:%zu/hold:%zu/to:%zu\n",
        phase == NULL ? "unknown" : phase,
        stats->worker_dispatches,
        (unsigned long long) stats->worker_dispatch_total_us,
        (unsigned) stats->worker_dispatch_max_us,
        stats->worker_prologues,
        (unsigned long long) stats->worker_prologue_total_us,
        (unsigned) stats->worker_prologue_max_us,
        stats->worker_firmware_calls,
        (unsigned long long) stats->worker_firmware_total_us,
        (unsigned) stats->worker_firmware_max_us,
        stats->worker_collects,
        (unsigned long long) stats->worker_collect_total_us,
        (unsigned) stats->worker_collect_max_us,
        stats->job_video_count,
        (unsigned long long) stats->job_video_total_us,
        stats->job_drain_count,
        (unsigned long long) stats->job_drain_total_us,
        stats->job_audio_count,
        (unsigned long long) stats->job_audio_total_us,
        stats->job_other_count,
        (unsigned long long) stats->job_other_total_us,
        /* Bucket ceilings, not samples: an upper bound on the percentile.
           See psp_media_job_percentile_us. */
        (unsigned) psp_media_job_percentile_us(
            stats->job_audio_buckets,
            PSP_MEDIA_JOB_HISTOGRAM_BUCKETS, 500u),
        (unsigned) psp_media_job_percentile_us(
            stats->job_audio_buckets,
            PSP_MEDIA_JOB_HISTOGRAM_BUCKETS, 900u),
        (unsigned) stats->job_audio_max_us,
        stats->hold_job_by_video, stats->hold_job_by_audio,
        stats->hold_job_by_drain, stats->hold_job_by_other,
        (unsigned long long) stats->hold_job_audio_age_total_us,
        (unsigned) stats->hold_job_audio_age_max_us,
        jobs == NULL ? 0 : jobs->source_block_video,
        jobs == NULL ? 0 : jobs->horizon_break_video,
        video_range == NULL ? 0 : video_range->window_pending_samples,
        (unsigned long long) (video_range == NULL
            ? 0 : video_range->window_pending_total_us),
        (unsigned long long) (video_range == NULL
            ? 0 : video_range->window_pending_max_us),
        (unsigned long long) (video_range == NULL
            ? 0 : video_range->window_starved_total_us),
        (unsigned long long) (video_range == NULL
            ? 0 : video_range->window_starved_max_us),
        /* aus minus job-audio is the number of worker round trips the
           batching actually removed; flush against defer is what it cost. */
        stats->audio_batch_jobs, stats->audio_batch_aus,
        stats->audio_batch_deferrals, stats->audio_batch_flushes,
        stats->audio_batch_budget_stops,
        stats->audio_batch_blocked_video,
        stats->audio_batch_blocked_queue,
        /* Visits that bought a packet to offer the partner in. Against
           audio-batch jobs it says how often the supply side, rather than the
           worker, is what let a pair form. */
        jobs == NULL ? 0 : jobs->paired_submits,
        stats->audio_pending_enqueued,
        stats->audio_pending_promoted,
        stats->audio_pending_full,
        stats->audio_pending_peak,
        stats->audio_pending_holds,
        stats->audio_pending_timeouts);
    printf(
        "tilefinch-media-prepared: phase=%s published=%zu "
        "audio=%zu chained=%zu cancelled=%zu "
        "ready-to-run=%lluus/%uus\n",
        phase == NULL ? "unknown" : phase,
        stats->worker_prepared_jobs,
        stats->worker_prepared_audio,
        stats->worker_chained_jobs,
        stats->worker_prepared_cancelled,
        (unsigned long long) stats->worker_prepared_wait_total_us,
        (unsigned) stats->worker_prepared_wait_max_us);
}

void psp_media_telemetry_report_feed(
    PspMediaSession *media, const char *phase)
{
    if (media == NULL || media->playback == NULL) return;
    MediaPlaybackJobStats stats = {0};
    MediaBackendStats backend_stats = {0};
    MediaHttpRangeStats video_range = {0};
    MediaHttpRangeStats audio_range = {0};
    media_playback_job_stats(media->playback, &stats);
    bool backend_ready =
        psp_media_backend_stats_snapshot(media, &backend_stats);
    (void) media_http_range_stats(media->range, &video_range);
    (void) media_http_range_stats(media->audio_range, &audio_range);
    PspMediaPresentDmaStats dma = {0};
    psp_media_present_ge_stage_dma_stats(&dma);
    printf(
        "tilefinch-media-feed: phase=%s calls=%zu packets=%zu idle=%zu "
        "submit-block=%zu drain-block=%zu horizon=%zu packet-limit=%zu "
        "queued=%zu source-ended=%zu playing-frames=%zu paused-frames=%zu "
        "pump-frames=%zu pump-units=%zu "
        "pump-submitted=%zu slice-exhausted=%zu unit-cap=%zu "
        "pump-total=%lluus pump-max-unit=%lluus frame-periods=%zu "
        "frame-period=%lluus frame-period-max=%lluus "
        "playing-periods=%zu playing-period-max=%lluus "
        "playing-wall=%lluus playing-steady-periods=%zu "
        "playing-steady-wall=%lluus playing-steady-period=%lluus "
        "draw-pump-frames=%zu "
        "draw-pump-units=%zu draw-pump-submitted=%zu "
        "draw-pump-total=%lluus wait-calls=%zu "
        "wait-collected=%zu wait-total=%lluus wait-ready=%d\n",
        phase == NULL ? "unknown" : phase,
        stats.calls, stats.packets_submitted, stats.idle_calls,
        stats.submit_block_calls, stats.drain_block_calls,
        stats.horizon_breaks, stats.packet_limit_breaks,
        stats.queued_breaks, stats.source_ended_breaks,
        /*
         * How much of the session was actually playing. Every counter above
         * is a sample of that window and of nothing else, and a cycle read a
         * 0.75-second window's 136 units as a session-long feed collapse
         * because the line did not say so: playback had died 0.75s after the
         * play press and the remaining 116 seconds sat on a failed panel,
         * ticking frame-periods.
         */
        media->pump_frames,
        media->advance_periods > media->pump_frames
            ? media->advance_periods - media->pump_frames : 0,
        media->pump_frames, media->pump_units,
        media->pump_units_submitted, media->pump_slice_exhausted,
        media->pump_unit_cap,
        (unsigned long long) media->pump_total_us,
        (unsigned long long) media->pump_max_unit_us,
        media->advance_periods,
        (unsigned long long) (media->advance_periods == 0 ? 0
            : media->advance_period_total_us / media->advance_periods),
        (unsigned long long) media->advance_period_max_us,
        media->advance_playing_periods,
        (unsigned long long) media->advance_playing_period_max_us,
        (unsigned long long) media->advance_playing_period_total_us,
        media->advance_playing_settled_periods,
        (unsigned long long) media->advance_playing_settled_total_us,
        (unsigned long long) (media->advance_playing_settled_periods == 0 ? 0
            : media->advance_playing_settled_total_us
                / media->advance_playing_settled_periods),
        media->pump_draw_frames, media->pump_draw_units,
        media->pump_draw_submitted,
        (unsigned long long) media->pump_draw_us,
        backend_stats.codec_wait_calls,
        backend_stats.codec_wait_collected,
        (unsigned long long) backend_stats.codec_wait_us,
        backend_ready ? 1 : 0);
    /* Transport and presentation have their own bounded records. Keeping the
       periodic aggregate below PSP_LOG_LINE_BYTES is functional: a truncated
       line concealed the right-hand counters during the rate campaign. */
    printf(
        "tilefinch-media-transport: phase=%s "
        "source-block=%zu refill-video=%zu/%zu refill-audio=%zu/%zu "
        "refill-block=%zu inflight=%zu/%zu "
        "new-conn=%zu/%zu handshakes=%zu/%zu handshake-max=%lluus/%lluus "
        "reconnects=%zu/%zu starved-reconnects=%zu/%zu "
        "floor=%zu/%zuBps\n",
        phase == NULL ? "unknown" : phase,
        /* refill-<track> is readahead-requested/windows-installed: a stream
           that never runs its window dry keeps those two within one of each
           other. refill-block counts the reads that answered would-block --
           the interval a unit used to spend waiting on the network. */
        stats.source_block_calls,
        video_range.readahead_requests, video_range.window_installs,
        audio_range.readahead_requests, audio_range.window_installs,
        video_range.would_block_reads + audio_range.would_block_reads,
        video_range.bytes_in_flight, audio_range.bytes_in_flight,
        video_range.window_new_connections,
        audio_range.window_new_connections,
        video_range.window_handshakes, audio_range.window_handshakes,
        (unsigned long long) video_range.handshake_max_us,
        (unsigned long long) audio_range.handshake_max_us,
        video_range.reconnects, audio_range.reconnects,
        video_range.starved_reconnects,
        audio_range.starved_reconnects,
        video_range.minimum_sustained_bytes_per_second,
        audio_range.minimum_sustained_bytes_per_second);
    printf(
        "tilefinch-media-window: phase=%s "
        "superseded=%zu/%zu complete=%zu/%zu waste=%zu/%zu "
        "bridged=%zu/%zu lookahead=%zu/%zu/%zuB\n",
        phase == NULL ? "unknown" : phase,
        video_range.readahead_superseded,
        audio_range.readahead_superseded,
        video_range.completed_readahead_superseded,
        audio_range.completed_readahead_superseded,
        video_range.superseded_bytes,
        audio_range.superseded_bytes,
        video_range.split_window_bridges,
        audio_range.split_window_bridges,
        video_range.lookahead_installs,
        video_range.lookahead_promotions,
        video_range.lookahead_retained_bytes);
    printf(
        "tilefinch-media-stream: phase=%s partial=%zu/%zuB "
        "video-cache=%llu+%zu/%zu fill=%llu+%zu pending=%u "
        "prefetch=%zu/%zu/%zu slots=%u aggressive=%u\n",
        phase == NULL ? "unknown" : phase,
        video_range.streaming_partial_reads,
        video_range.streaming_partial_bytes,
        (unsigned long long) (video_range.cache_length == 0 ? 0
            : video_range.cache_offset),
        video_range.cache_length,
        video_range.cache_consumed,
        (unsigned long long) (video_range.fill_length == 0 ? 0
            : video_range.fill_offset),
        video_range.bytes_in_flight,
        video_range.window_pending ? 1u : 0u,
        video_range.readahead_checks,
        video_range.readahead_waiting_for_consumption,
        video_range.readahead_issue_refusals,
        video_range.lookahead_slots,
        video_range.aggressive_readahead ? 1u : 0u);
    printf(
        "tilefinch-media-present: phase=%s "
        "dma-async=%zu/%zu/%zu dma-copy-max=%uus dma-timeouts=%zu "
        "dma-quarantine=%zu/%zu/%uus surface-quarantines=%zu "
        "borrow-waits=%zu borrow-wait=%lluus borrow-timeouts=%zu "
        "emit-deferred=%zu prevblank-frames=%zu prevblank-submitted=%zu "
        "\n",
        phase == NULL ? "unknown" : phase,
        dma.submitted, dma.completed, dma.failures, dma.max_copy_us,
        dma.timeouts,
        /* Transfers abandoned by a join, transfers later seen to finish, and
           the longest a slot was held unusable by one. All three read zero on
           a healthy device; a quarantine without a matching late completion
           is a transfer that never came back. */
        dma.quarantines, dma.late_completions, dma.quarantine_max_us,
        backend_stats.surface_quarantines,
        backend_stats.surface_borrow_waits,
        (unsigned long long) backend_stats.surface_borrow_wait_us,
        backend_stats.surface_borrow_timeouts,
        backend_stats.surface_emit_deferrals,
        media->prevblank_frames, media->prevblank_submitted);
    /* Keep this proof funnel off the already-long feed line. PSP validation
       logging has a bounded line buffer, and truncating the right-hand fields
       turned a complete counter into an ambiguous one in run-final. */
    printf(
        "tilefinch-media-head: phase=%s block=%zu video=%zu audio=%zu "
        "alt-pending=%zu alt-horizon=%zu alt-resident=%zu lead=%lluus "
        "lead-n=%zu behind=%zu "
        "bypass=%zu submitted=%zu blocked=%zu\n",
        phase == NULL ? "unknown" : phase,
        stats.head_blocks, stats.head_block_video, stats.head_block_audio,
        stats.head_alt_pending, stats.head_alt_in_horizon,
        stats.head_alt_resident,
        (unsigned long long) (stats.head_alt_lead_samples == 0 ? 0
            : stats.head_alt_lead_total_us / stats.head_alt_lead_samples),
        stats.head_alt_lead_samples, stats.head_alt_behind,
        stats.head_alt_bypasses, stats.head_alt_submitted,
        stats.head_alt_blocked);
    /* Same window, same phase, one grep apart. Skipped rather than printed
       empty when the backend refused its stats, because a line of zeros from a
       busy job slot reads exactly like a pipeline that stopped. */
    if (backend_ready) {
        psp_media_report_video_cadence(phase, &backend_stats);
        /* The same window's occupancy and the worker behind it. Three lines
           of one sample: counts, durations, and the handoffs between them. */
        psp_media_report_slots(phase, &backend_stats);
        psp_media_report_worker(phase, &backend_stats, &stats, &video_range);
        /* The per-picture records behind the aggregates, so a reader can check
           any claim the means make against the pictures they were made from. */
        media_psp_backend_dump_picture_trace(phase);
    }
    uint64_t audio_cursor_us = 0;
    bool have_audio_cursor = media_playback_audio_cursor_us(
        media->playback, &audio_cursor_us);
    uint64_t video_cursor_us = backend_ready
        ? backend_stats.presented_video_us : 0;
    int64_t av_skew_us = !backend_ready || !have_audio_cursor
        ? 0
        : audio_cursor_us >= video_cursor_us
            ? (audio_cursor_us - video_cursor_us > INT64_MAX
                ? INT64_MAX
                : (int64_t) (audio_cursor_us - video_cursor_us))
            : (video_cursor_us - audio_cursor_us > INT64_MAX
                ? INT64_MIN
                : -(int64_t) (video_cursor_us - audio_cursor_us));
    printf(
        "tilefinch-media-clock: phase=%s clock=%lluus audio=%lluus "
        "video=%lluus av-skew=%lldus audio-valid=%d video-valid=%d\n",
        phase == NULL ? "unknown" : phase,
        (unsigned long long) media->clock_us,
        (unsigned long long) audio_cursor_us,
        (unsigned long long) video_cursor_us,
        (long long) av_skew_us,
        have_audio_cursor ? 1 : 0,
        backend_ready && video_cursor_us != 0 ? 1 : 0);
}
