#include "psp_media_session_internal.h"

#include <stdio.h>

#include "media_backend_psp_policy.h"
#include "tilefinch/psp_log.h"

#define printf psp_log_printf

void psp_media_buffering_end(PspMediaSession *media, uint64_t now_us)
{
    if (media == NULL || !media->buffering_service_active) return;
    if (now_us >= media->network_buffer_started_us)
        media->network_buffer_total_us +=
            now_us - media->network_buffer_started_us;
    media->buffering_service_active = false;
    media->buffering_started_during_startup = false;
    media->network_buffer_slow_logged = false;
    media->network_buffer_ready_since_us = 0;
    media->network_starved_since_us = 0;
    media_playback_set_buffering(media->playback, false);
    printf("tilefinch-media-buffer: event=resume count=%u held=%lluus\n",
           media->network_buffer_events,
           (unsigned long long) media->network_buffer_total_us);
}

void psp_media_buffering_begin(
    PspMediaSession *media, bool startup, uint64_t now_us)
{
    if (media == NULL || media->playback == NULL
        || media->buffering_service_active) return;
    media->buffering_service_active = true;
    media->buffering_started_during_startup = startup;
    media->network_buffer_slow_logged = false;
    media->network_buffer_started_us = now_us;
    media->network_buffer_ready_since_us = 0;
    media->network_starved_since_us = 0;
    media->network_buffer_events++;
    /* The scheduler response buffer already exists for every range. Starting
       its sequential refill at 1/16 rather than 1/4 consumption gives slow
       PSP Wi-Fi more cover without retaining another byte. Keep the policy
       after the first intentional buffer for the rest of this session. */
    media_http_range_set_aggressive_readahead(media->range, true);
    media_http_range_set_aggressive_readahead(media->audio_range, true);
    media_playback_set_buffering(media->playback, true);
    psp_ui_media_set_buffering(
        &media->ui, true,
        media_playback_buffered_until_us(media->playback));
    printf("tilefinch-media-buffer: event=begin reason=%s count=%u\n",
           startup ? "startup" : "source-starved",
           media->network_buffer_events);
    psp_media_session_dispatch_event(media, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_SOURCE_STARVED
    }, startup ? "startup-buffer" : "network-buffer");
}

static uint64_t psp_media_network_ahead_us(const PspMediaSession *media)
{
    if (media == NULL || media->offline_source) return UINT64_MAX;
    uint64_t duration_us = psp_media_duration_us(media);
    if (media->audio_only)
        return media_http_range_buffered_ahead_us(
            media->audio_range, duration_us);
    uint64_t video_us = media_http_range_buffered_ahead_us(
        media->range, duration_us);
    if (media->audio_range == NULL) return video_us;
    uint64_t audio_us = media_http_range_buffered_ahead_us(
        media->audio_range, duration_us);
    return video_us < audio_us ? video_us : audio_us;
}

static bool psp_media_network_fill_pending(const PspMediaSession *media)
{
    MediaHttpRangeStats video = {0};
    MediaHttpRangeStats audio = {0};
    bool video_pending = media != NULL && media->range != NULL
        && media_http_range_stats(media->range, &video)
        && video.window_pending;
    bool audio_pending = media != NULL && media->audio_range != NULL
        && media_http_range_stats(media->audio_range, &audio)
        && audio.window_pending;
    MediaHlsStats hls = {0};
    bool hls_pending = media != NULL
        && psp_media_hls_stats(media->hls, &hls)
        && hls.active_requests != 0;
    return video_pending || audio_pending || hls_pending;
}

void psp_media_buffering_update(
    PspMediaSession *media, const MediaPlaybackJobStats *stats,
    uint64_t now_us)
{
    if (media == NULL || stats == NULL || media->playback == NULL) return;
    bool source_blocked = stats->source_block_calls
        != media->network_buffer_source_blocks_seen;
    media->network_buffer_source_blocks_seen = stats->source_block_calls;
    bool fill_pending = psp_media_network_fill_pending(media);
    PspMediaBufferPolicyInput policy_input = {
        .playing = psp_media_machine_wants_playing(media),
        .pause_after_next_frame =
            media->machine.state == PSP_MEDIA_SESSION_PRIMING,
        .job_active = media->job_phase != PSP_MEDIA_JOB_NONE,
        .buffering = media->machine.state == PSP_MEDIA_SESSION_BUFFERING,
        .startup = media->buffering_started_during_startup,
        .source_blocked = source_blocked,
        .fill_pending = fill_pending,
        .buffer_events = media->network_buffer_events,
        .now_us = now_us,
        .starved_since_us = media->network_starved_since_us,
        .ready_since_us = media->network_buffer_ready_since_us
    };
    /* Preserve the hot playing path: buffered-ahead estimation performs
       64-bit divisions and was previously paid only while the buffering
       surface was already visible. The pure policy still owns every state
       decision, but its cheap debounce half runs first. */
    if (!psp_media_machine_wants_playing(media)
        || media->machine.state == PSP_MEDIA_SESSION_PRIMING
        || media->job_phase != PSP_MEDIA_JOB_NONE) {
        PspMediaBufferPolicyDecision decision =
            psp_media_buffer_policy(policy_input);
        media->network_starved_since_us = decision.starved_since_us;
        media->network_buffer_ready_since_us = decision.ready_since_us;
        if (decision.action == PSP_MEDIA_BUFFER_END)
            psp_media_buffering_end(media, now_us);
        return;
    }
    if (!media->buffering_service_active) {
        PspMediaBufferPolicyDecision decision =
            psp_media_buffer_policy(policy_input);
        media->network_starved_since_us = decision.starved_since_us;
        media->network_buffer_ready_since_us = decision.ready_since_us;
        if (decision.action == PSP_MEDIA_BUFFER_BEGIN)
            psp_media_buffering_begin(media, false, now_us);
        return;
    }
    /* The transport owns request failure and retry. Never resume merely
       because the wait is long: that produced one visible frame followed by
       an immediate second stall on a slow real Wi-Fi refill. Keep the honest
       UI up and emit one aggregate diagnostic; Circle remains responsive. */
    if (media->buffering_service_active && !media->network_buffer_slow_logged
        && now_us - media->network_buffer_started_us
               >= PSP_MEDIA_BUFFER_SLOW_NOTICE_US) {
        media->network_buffer_slow_logged = true;
        printf("tilefinch-media-buffer: event=slow held=%lluus pending=%u\n",
               (unsigned long long) (now_us - media->network_buffer_started_us),
               fill_pending ? 1u : 0u);
        /* One report for the route makes a player which remains on the
           buffering surface diagnosable even if the transport never reaches
           a terminal callback. The session-level writer cap keeps this far
           below the Memory Stick write budget. */
        psp_media_report_failure_snapshot(
            media, "media-buffering", "BUFFERING EXCEEDED 20 SECONDS",
            fill_pending
                ? "range request still pending"
                : "no range request pending",
            false);
    }

    uint64_t remaining_us = psp_media_duration_us(media) > media->clock_us
        ? psp_media_duration_us(media) - media->clock_us : 0;
    uint64_t decoded_until_us =
        media_playback_buffered_until_us(media->playback);
    uint64_t decoded_ahead_us = decoded_until_us > media->clock_us
        ? decoded_until_us - media->clock_us : 0;
    uint64_t network_ahead_us = psp_media_network_ahead_us(media);
    policy_input.remaining_us = remaining_us;
    policy_input.decoded_ahead_us = decoded_ahead_us;
    policy_input.network_ahead_us = network_ahead_us;
    PspMediaBufferPolicyDecision decision =
        psp_media_buffer_policy(policy_input);
    media->network_starved_since_us = decision.starved_since_us;
    media->network_buffer_ready_since_us = decision.ready_since_us;
    if (decision.action == PSP_MEDIA_BUFFER_BEGIN) {
        psp_media_buffering_begin(media, false, now_us);
    } else if (decision.action == PSP_MEDIA_BUFFER_END) {
        PspMediaPresentationReadiness readiness =
            media->presentation_preroll_audio_held
                ? PSP_MEDIA_PRESENTATION_NEEDS_PRIME
                : PSP_MEDIA_PRESENTATION_READY;
        psp_media_buffering_end(media, now_us);
        /* Observe completion after the observed flag changes. Sampling
           before buffering_end produced one-frame false mismatches on every
           refill even though both machines chose the same destination. */
        psp_media_session_dispatch_event(media, (PspMediaEvent) {
            .type = PSP_MEDIA_EVENT_BUFFER_STABLE,
            .readiness = readiness
        }, "network-buffer-stable");
    }
}
