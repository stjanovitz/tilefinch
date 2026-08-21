#include "psp_media_session_internal.h"

#include <stdio.h>
#include <string.h>

#include "media_backend_psp_policy.h"
#include "tilefinch/platform.h"
#include "tilefinch/psp_log.h"
#include "tilefinch/user_agent.h"

#define printf psp_log_printf
#define KIB 1024u
#define PSP_MEDIA_CONNECT_TIMEOUT_MS 3000
#define PSP_MEDIA_REUSED_URL_MINIMUM_LIFETIME_SECONDS 60u

typedef enum {
    PSP_PAGE_MEDIA_PROBE_PENDING = 0,
    PSP_PAGE_MEDIA_PROBE_READY,
    PSP_PAGE_MEDIA_PROBE_FAILED
} PspPageMediaProbeStatus;

static PspPageMediaProbeStatus psp_media_page_probe_finish(
    PspMediaSession *media, FetchResult *result,
    bool fetched, char *error, size_t error_size)
{
    if (media == NULL || result == NULL) return PSP_PAGE_MEDIA_PROBE_FAILED;
    char content_range[128] = {0};
    uint64_t complete_length = 0;
    TilefinchRequestContext context = {
        .target_url = media->source,
        .initiator_url = media->page_document_url,
        .top_level_url = media->page_document_url,
        .method = "GET",
        .mode = media->page_media_mode,
        .credentials = media->page_media_credentials,
        .destination = TILEFINCH_DESTINATION_MEDIA
    };
    TilefinchResourceGrant grant;
    TilefinchResourceDeniedReason denied;
    bool admitted = fetched && result->status_code == 206
        && result->length == 1u
        && fetch_response_header_value(
               result, "content-range", content_range,
               sizeof(content_range))
        && media_http_parse_content_range(
               content_range, 0, 0, result->length, &complete_length)
        && fetch_resource_grant_create(
               result, &context,
               media->page_media_mode == TILEFINCH_REQUEST_MODE_CORS,
               true, false, &grant, &denied);
    if (!admitted) {
        snprintf(error, error_size,
                 "page %s range probe failed (HTTP %ld): %.150s",
                 media->page_audio ? "audio" : "video",
                 result->status_code,
                 result->error[0] == '\0'
                     ? "server did not authorize byte ranges"
                     : result->error);
        return PSP_PAGE_MEDIA_PROBE_FAILED;
    }
    for (size_t at = 0; at < result->set_cookie_count; at++) {
        context.target_url = fetch_set_cookie_url(
            result, at, result->effective_url);
        (void) browser_session_cookie_set_http_context(
            media->session, &context, result->set_cookies[at]);
    }
    memset(&media->stream, 0, sizeof(media->stream));
    snprintf(media->stream.title, sizeof(media->stream.title), "%s",
             media->page_audio ? "Page audio" : "Page video");
    const char *effective_url = result->effective_url[0] == '\0'
        ? media->source : result->effective_url;
    if (media->page_audio) {
        snprintf(media->stream.audio_url,
                 sizeof(media->stream.audio_url), "%s", effective_url);
        snprintf(media->stream.audio_mime_type,
                 sizeof(media->stream.audio_mime_type), "%s", "audio/mp4");
        media->stream.audio_content_length = complete_length;
    } else {
        snprintf(media->stream.media_url,
                 sizeof(media->stream.media_url), "%s", effective_url);
        snprintf(media->stream.mime_type,
                 sizeof(media->stream.mime_type), "%s", "video/mp4");
        media->stream.content_length = complete_length;
    }
    media->stream.expires_unix = UINT64_MAX;
    return PSP_PAGE_MEDIA_PROBE_READY;
}

static PspPageMediaProbeStatus psp_media_page_probe(
    PspMediaSession *media, char *error, size_t error_size)
{
    if (media == NULL) return PSP_PAGE_MEDIA_PROBE_FAILED;
    if (media->page_media_probe_request != 0) {
        FetchBackgroundProgress progress = {0};
        if (!fetch_background_transport_progress(
                media->page_media_probe_request, &progress)) {
            media->page_media_probe_request = 0;
            snprintf(error, error_size, "page %s probe disappeared",
                     media->page_audio ? "audio" : "video");
            return PSP_PAGE_MEDIA_PROBE_FAILED;
        }
        if (!progress.complete) return PSP_PAGE_MEDIA_PROBE_PENDING;
        FetchResult *result = fetch_result_create(media->budget);
        if (result == NULL) {
            snprintf(error, error_size, "page %s probe budget",
                     media->page_audio ? "audio" : "video");
            return PSP_PAGE_MEDIA_PROBE_FAILED;
        }
        uint64_t request = media->page_media_probe_request;
        media->page_media_probe_request = 0;
        bool fetched = fetch_background_transport_take_fetch_result(
            request, media->budget, result);
        PspPageMediaProbeStatus status = psp_media_page_probe_finish(
            media, result, fetched, error, error_size);
        fetch_result_free(result);
        return status;
    }

    FetchPreparedPageRequest *prepared = budget_malloc_category(
        media->budget, BUDGET_CATEGORY_NAVIGATION, sizeof(*prepared));
    if (prepared == NULL) {
        snprintf(error, error_size, "page %s request budget",
                 media->page_audio ? "audio" : "video");
        return PSP_PAGE_MEDIA_PROBE_FAILED;
    }
    char range_header[64];
    TilefinchRequestContext context = {
        .target_url = media->source,
        .initiator_url = media->page_document_url,
        .top_level_url = media->page_document_url,
        .method = "GET",
        .mode = media->page_media_mode,
        .credentials = media->page_media_credentials,
        .destination = TILEFINCH_DESTINATION_MEDIA
    };
    FetchRequest transport = {
        .allow_http_errors = true,
        .accept = media->page_audio
            ? "audio/mp4,audio/*;q=0.9,*/*;q=0.5"
            : "video/mp4,video/*;q=0.9,*/*;q=0.5",
        .user_agent = TILEFINCH_BROWSER_USER_AGENT,
        .connect_timeout_ms = PSP_MEDIA_CONNECT_TIMEOUT_MS,
        .redirect_same_origin_only = true
    };
    FetchRequestValidationError validation_error;
    bool ready = media_http_build_range_header(
            0, 0, range_header, sizeof(range_header));
    transport.extra_headers = range_header;
    ready = ready && fetch_prepare_page_request_context(
        &context, media->page_document_url, NULL, media->session, NULL,
        media->session == NULL ? NULL : media->session->content_blocker,
        &transport, prepared, &validation_error);
    const FetchRequest *request = ready
        ? fetch_prepared_page_request(prepared) : NULL;
    if (!ready || request == NULL) {
        budget_free(media->budget, prepared);
        snprintf(error, error_size, "page %s request refused",
                 media->page_audio ? "audio" : "video");
        return PSP_PAGE_MEDIA_PROBE_FAILED;
    }
    if (fetch_background_transport_available()) {
        FetchBackgroundEnqueueStatus enqueue_status;
        media->page_media_probe_request =
            fetch_background_transport_enqueue_media_diagnosed(
                media->source, request, 1u, 15000, &enqueue_status);
        budget_free(media->budget, prepared);
        if (media->page_media_probe_request == 0) {
            if (enqueue_status == FETCH_BACKGROUND_ENQUEUE_SATURATED
                || enqueue_status == FETCH_BACKGROUND_ENQUEUE_ADMISSION_CLOSED)
                return PSP_PAGE_MEDIA_PROBE_PENDING;
            snprintf(error, error_size, "page %s transport unavailable",
                     media->page_audio ? "audio" : "video");
            return PSP_PAGE_MEDIA_PROBE_FAILED;
        }
        return PSP_PAGE_MEDIA_PROBE_PENDING;
    }
    FetchResult *result = fetch_result_create(media->budget);
    bool fetched = result != NULL && fetch_request_cancelable(
        media->budget, media->source, request, 1u, 15000,
        psp_media_cancel_callback, media, result);
    budget_free(media->budget, prepared);
    if (result == NULL) {
        snprintf(error, error_size, "page %s probe budget",
                 media->page_audio ? "audio" : "video");
        return PSP_PAGE_MEDIA_PROBE_FAILED;
    }
    PspPageMediaProbeStatus status = psp_media_page_probe_finish(
        media, result, fetched, error, error_size);
    fetch_result_free(result);
    return status;
}

static void psp_media_page_track_metadata(
    PspMediaSession *media, MediaMp4Demux *demux)
{
    if (media == NULL || !media->page_source || demux == NULL) return;
    uint64_t duration_ms = 0;
    for (size_t at = 0; at < media_mp4_track_count(demux); at++) {
        MediaMp4TrackInfo info;
        if (!media_mp4_track_info(demux, at, &info)
            || info.timescale == 0) continue;
        uint64_t track_ms = info.duration <= UINT64_MAX / UINT64_C(1000)
            ? info.duration * UINT64_C(1000) / info.timescale
            : info.duration / info.timescale * UINT64_C(1000);
        if (track_ms > duration_ms) duration_ms = track_ms;
        if (info.kind == MEDIA_MP4_TRACK_VIDEO) {
            media->stream.width = info.width;
            media->stream.height = info.height;
        }
    }
    media->stream.duration_ms = duration_ms;
}

static bool psp_media_page_validate_audio_track(
    PspMediaSession *media, char *error, size_t error_size)
{
    if (media == NULL || media->audio_demux == NULL || !media->page_audio)
        return true;
    bool found = false;
    for (size_t at = 0;
         at < media_mp4_track_count(media->audio_demux); at++) {
        MediaMp4TrackInfo info;
        if (media_mp4_track_info(media->audio_demux, at, &info)
            && info.kind == MEDIA_MP4_TRACK_AUDIO
            && info.codec == MEDIA_MP4_FOURCC('m','p','4','a')) {
            found = true;
            break;
        }
    }
    if (!found) {
        snprintf(error, error_size, "%s",
                 "page audio has no supported AAC track");
        return false;
    }
    return true;
}

static void psp_media_apply_decoder_hint(PspMediaSession *media)
{
    if (media == NULL) return;
    media->use_swdec = psp_swdec_component_owns_me(&media->swdec);
    media->decoder_profile_idc = 0;
    uint8_t profile = 0;
    MediaH264DecoderRoute route = media_h264_codec_string_decoder_route(
        media->stream.mime_type, &profile);
    if (route == MEDIA_H264_DECODER_ROUTE_HIGH_EXTENSION)
        media->use_swdec = true;
    if (route != MEDIA_H264_DECODER_ROUTE_UNSUPPORTED)
        media->decoder_profile_idc = profile;
}

static bool psp_media_apply_demux_decoder_route(
    PspMediaSession *media, char *error, size_t error_size)
{
    if (media == NULL || media->demux == NULL || media->audio_only)
        return true;
    MediaMp4TrackInfo video = {0};
    bool found = false;
    for (size_t i = 0; i < media_mp4_track_count(media->demux); i++) {
        if (media_mp4_track_info(media->demux, i, &video)
            && video.kind == MEDIA_MP4_TRACK_VIDEO
            && video.codec == MEDIA_MP4_FOURCC('a','v','c','1')) {
            found = true;
            break;
        }
    }
    uint8_t profile = 0;
    MediaH264DecoderRoute route = found
        ? media_h264_avcc_decoder_route(
              video.codec_config, video.codec_config_length, &profile)
        : MEDIA_H264_DECODER_ROUTE_UNSUPPORTED;
    if (route == MEDIA_H264_DECODER_ROUTE_UNSUPPORTED) {
        snprintf(error, error_size, "%s", "unsupported AVC profile");
        return false;
    }
    media->decoder_profile_idc = profile;
    bool swdec_required = route == MEDIA_H264_DECODER_ROUTE_HIGH_EXTENSION
        || psp_swdec_component_owns_me(&media->swdec);
    if (swdec_required) {
        uint16_t width = 0, height = 0;
        uint8_t nal_length = 0;
        if (!media_h264_avcc_dimensions(
                video.codec_config, video.codec_config_length,
                &width, &height, &nal_length)
            || width == 0 || height == 0 || width > 432u || height > 240u) {
            snprintf(error, error_size,
                     "software H.264 is limited to 432x240");
            return false;
        }
        (void) nal_length;
        media->use_swdec = true;
    } else {
        media->use_swdec = false;
    }
    if (media->use_swdec
        && !psp_swdec_component_prepare(
            &media->swdec, error, error_size)) return false;
    return true;
}

bool psp_media_resolved_stream_reusable(const PspMediaSession *media)
{
    if (media == NULL) return false;
    if (media->page_audio) {
        if (media->stream.audio_content_length == 0
            || media->stream.audio_url[0] == '\0') return false;
    } else if (media->audio_only) {
        if (media->stream.audio_content_length == 0
            || media->stream.audio_url[0] == '\0') return false;
    } else if (media->stream.content_length == 0
               || media->stream.media_url[0] == '\0') return false;
    if (media->offline_source) {
        return media->offline_video_path[0] != '\0'
            && (!media->stream.split_streams
                || (media->stream.audio_content_length != 0
                    && media->offline_audio_path[0] != '\0'));
    }
    if (media->stream.split_streams
        && (media->stream.audio_content_length == 0
            || media->stream.audio_url[0] == '\0')) return false;
    uint64_t now = tilefinch_platform_wall_time_ns()
        / UINT64_C(1000000000);
    return media->stream.expires_unix > now
        && media->stream.expires_unix - now
             >= PSP_MEDIA_REUSED_URL_MINIMUM_LIFETIME_SECONDS;
}

static bool psp_media_create_playback(PspMediaSession *media,
                                      uint64_t audio_start_us,
                                      char *error, size_t error_size)
{
    MediaBackend backend = {0};
    bool backend_ready = media->hls != NULL
        ? media_psp_swdec_backend_create_sources(
            media->budget, &media->hls_source, NULL,
            psp_swdec_component_api(&media->swdec),
            &backend, error, error_size)
        : media->use_swdec && !media->audio_only
        ? media_psp_swdec_backend_create_split(
            media->budget, media->demux, media->audio_demux,
            psp_swdec_component_api(&media->swdec),
            &backend, error, error_size)
        : media->use_swdec && media->audio_only
        ? media_psp_swdec_backend_create_audio(
            media->budget, media->audio_demux,
            psp_swdec_component_api(&media->swdec),
            &backend, error, error_size)
        : media->audio_only
        ? media_psp_backend_create_audio(
            media->budget, media->audio_demux,
            &backend, error, error_size)
        : media_psp_backend_create_split(
            media->budget, media->demux, media->audio_demux,
            &backend, error, error_size);
    if (!backend_ready) {
        return false;
    }
    /*
     * Decode and retain the first timestamp-aligned PCM/video prefix before
     * allowing the audio worker to present it. MediaPlayback remains active
     * so the bounded first-frame job can submit packets.
     */
    backend.set_playing(backend.opaque, false);
    MediaPlaybackOptions options = {
        .decode_lead_us = backend.preferred_decode_lead_us != 0
            ? backend.preferred_decode_lead_us : PSP_MEDIA_DECODE_LEAD_US,
        .audio_start_us = audio_start_us,
        .maximum_packet_bytes = PSP_MEDIA_MAXIMUM_PACKET_BYTES,
        .preallocate_maximum_packet_bytes = true
    };
    media->playback = media->hls != NULL
        ? media_playback_create_sources(
            media->budget, &media->hls_source, NULL,
            &backend, &options, error, error_size)
        : media->audio_only
        ? media_playback_create(
            media->budget, media->audio_demux, &backend, &options,
            error, error_size)
        : media->audio_demux == NULL
        ? media_playback_create(
            media->budget, media->demux, &backend, &options,
            error, error_size)
        : media_playback_create_split(
            media->budget, media->demux, media->audio_demux,
            &backend, &options, error, error_size);
    if (media->playback == NULL) {
        backend.destroy(backend.opaque);
        return false;
    }
    return true;
}

static bool psp_media_create_audio_http_range(
    PspMediaSession *media, char *error, size_t error_size)
{
    if (media == NULL) return false;
    if (media->audio_range != NULL) return true;
    MediaHttpRangeOptions range_options = {
        .cache_bytes = 256u * KIB,
        .minimum_sustained_bytes_per_second =
            psp_media_transport_rate_floor(
                media->stream.audio_content_length,
                media->stream.duration_ms),
        .timeout_ms = 15000,
        .connect_timeout_ms = PSP_MEDIA_CONNECT_TIMEOUT_MS,
        .referer = media->page_audio
            ? media->page_document_url : media->source,
        .standard_range_header = media->page_audio,
        .audio_only = true,
        .page_request_context = media->page_audio
            ? &(TilefinchRequestContext) {
                .target_url = media->stream.audio_url,
                .initiator_url = media->page_document_url,
                .top_level_url = media->page_document_url,
                .method = "GET",
                .mode = media->page_media_mode,
                .credentials = media->page_media_credentials,
                .destination = TILEFINCH_DESTINATION_MEDIA
              } : NULL,
        .url_validator = media->page_audio
            ? NULL : youtube_media_url_supported,
        .cancel = psp_media_cancel_callback,
        .cancel_opaque = media
    };
    media->audio_range = media_http_range_create(
        media->budget, media->session, media->stream.audio_url,
        media->stream.audio_content_length, &range_options,
        error, error_size);
    if (media->audio_range == NULL) return false;
    if (browser_profile_video_startup_buffering(media->profile))
        media_http_range_set_aggressive_readahead(media->audio_range, true);
    return true;
}

uint64_t psp_media_recovery_position_us(
    const PspMediaSession *media)
{
    if (media == NULL) return 0;
    if (media->job_phase == PSP_MEDIA_JOB_SEEK_PREPARE
        || media->job_phase == PSP_MEDIA_JOB_SEEK_PRIME
        || media->job_phase == PSP_MEDIA_JOB_SEEK_DECODE) {
        /* Preview movement is tentative until the user confirms it. A retry,
           suspend, or close during its decode must restore the position from
           which preview began, not silently commit the highlighted target. */
        return media->job_preview && media->seek_preview_started
            ? media->job_restore_us : media->job_target_us;
    }
    if (media->job_phase == PSP_MEDIA_JOB_PREVIEW_RESTORE_PREPARE
        || media->job_phase == PSP_MEDIA_JOB_PREVIEW_RESTORE_DECODE)
        return media->job_restore_us;
    return media->clock_us;
}

void psp_media_remember_retry_state(
    PspMediaSession *media, bool resume_playing)
{
    if (media == NULL || media->playback == NULL) return;
    media->reopen_resume_us = psp_media_recovery_position_us(media);
    media->reopen_resume_playing = resume_playing;
    media->reopen_resume_pending = true;
}

/* True while the transaction that is failing is one of the two seek legs or
   the preview restore, which are the phases that have already repositioned the
   demuxer. */
static bool psp_media_job_moved_the_source(const PspMediaSession *media)
{
    return media != NULL
        && (media->job_phase == PSP_MEDIA_JOB_SEEK_PREPARE
            || media->job_phase == PSP_MEDIA_JOB_SEEK_PRIME
            || media->job_phase == PSP_MEDIA_JOB_SEEK_DECODE
            || media->job_phase == PSP_MEDIA_JOB_PREVIEW_RESTORE_PREPARE
            || media->job_phase == PSP_MEDIA_JOB_PREVIEW_RESTORE_DECODE);
}

void psp_media_job_failed(PspMediaSession *media,
                                 const char *operation,
                                 const char *error)
{
    if (media == NULL) return;
    media->reopen_reuse_resolved_stream = false;
    bool opening_failure = psp_media_open_phase(media->job_phase);
    bool seeking_failure = psp_media_seek_phase(media->job_phase);
    psp_media_release_presentation_preroll(media, true);
    /*
     * Sampled before the phase is cleared, because both answers are phase
     * derived: the retry ladder's resume position (which keeps a tentative
     * preview scrub uncommitted) and the position the source is actually
     * sitting at (which does not). They are different questions and after a
     * failed preview seek they have different answers -- see
     * psp_media_seek_failure_clock_us.
     */
    bool moved_source = psp_media_job_moved_the_source(media);
    bool restoring_preview =
        media->job_phase == PSP_MEDIA_JOB_PREVIEW_RESTORE_PREPARE
        || media->job_phase == PSP_MEDIA_JOB_PREVIEW_RESTORE_DECODE;
    bool tentative_preview = seeking_failure && !restoring_preview
        && media->job_preview && media->seek_preview_started
        && media->ui.seek_preview_active;
    uint64_t source_us = psp_media_seek_failure_clock_us(
        restoring_preview || tentative_preview,
        media->job_target_us, media->job_restore_us);
    psp_media_remember_retry_state(
        media, media->ui.playing || media->job_resume_playing);
    if (moved_source && media->playback != NULL
        && media->clock_us != source_us) {
        printf("tilefinch-media-clock: event=seek-failed-reconcile "
               "operation=%s clock=%lluus source=%lluus resume=%lluus\n",
               operation == NULL ? "unknown" : operation,
               (unsigned long long) media->clock_us,
               (unsigned long long) source_us,
               (unsigned long long) media->reopen_resume_us);
        media->clock_us = source_us;
    }
    /*
     * A scrub thumbnail is optional. In particular, a cold ranged source can
     * reach the committed position again yet miss the exact preview-frame
     * deadline. Treating that as SEEK_FAILED destroyed the highlighted time,
     * left the player paused, and changed the next Cross into a dead Play
     * press. Once the restore leg has put the source back, retain the target
     * as UI/intent state and let Cross commit it or Circle cancel it. The
     * ordinary playback pipeline remains valid and no failure panel is owed.
     */
    if (tentative_preview && !media->preview_commit_pending
        && media->playback != NULL
        && !media_psp_backend_quarantined()) {
        /* The exact thumbnail missed its deadline, but the user has not
           committed it. Restore the demuxer to the original position and
           retain the highlighted time; failure of that restore takes the
           retain-target branch below rather than killing healthy playback. */
        media->job_phase = PSP_MEDIA_JOB_PREVIEW_RESTORE_PREPARE;
        media->job_started_us = psp_media_internal_now_us(media);
        media->job_phase_started_us = 0;
        media->job_units = 0;
        media->job_maximum_unit_us = 0;
        media->job_preview = true;
        media->job_resume_playing = false;
        media->job_resume_open = false;
        media->reopen_resume_pending = false;
        media_playback_set_playing(media->playback, false);
        printf("tilefinch-media: preview unavailable operation=%s "
               "action=restore target=%lluus restore=%lluus "
               "reason=\"%.160s\"\n",
               operation == NULL ? "unknown" : operation,
               (unsigned long long) media->ui.seek_preview_time_us,
               (unsigned long long) media->job_restore_us,
               error == NULL ? "preview unavailable" : error);
        return;
    }
    if (seeking_failure
        && (restoring_preview
            || (tentative_preview && media->preview_commit_pending))
        && media->seek_preview_started
        && media->ui.seek_preview_active
        && media->playback != NULL
        && !media_psp_backend_quarantined()) {
        uint64_t preview_target_us = media->ui.seek_preview_time_us;
        bool preview_was_playing = media->seek_preview_was_playing;
        media->job_phase = PSP_MEDIA_JOB_NONE;
        media->job_started_us = 0;
        media->job_phase_started_us = 0;
        media->job_units = 0;
        media->job_maximum_unit_us = 0;
        media->job_preview = false;
        media->job_resume_playing = false;
        media->job_resume_open = false;
        media->seek_preview_cancel_pending = false;
        media->reopen_resume_pending = false;
        /* Suppress advance's exact-preview coalescer. The target is still
           highlighted, but no second thumbnail transaction is started. */
        media->job_target_us = preview_target_us;
        media->seek_preview_started = true;
        media->seek_preview_was_playing = preview_was_playing;
        media_playback_set_playing(media->playback, false);
        psp_ui_media_set(
            &media->ui, true, false, false, media->clock_us,
            psp_media_duration_us(media), media->stream.title);
        psp_ui_media_set_seek_preview(&media->ui, preview_target_us);
        printf("tilefinch-media: preview unavailable operation=%s "
               "action=retain-target target=%lluus restore=%lluus "
               "reason=\"%.160s\"\n",
               operation == NULL ? "unknown" : operation,
               (unsigned long long) preview_target_us,
               (unsigned long long) media->clock_us,
               error == NULL ? "preview unavailable" : error);
        return;
    }
    media->job_phase = PSP_MEDIA_JOB_NONE;
    media->job_started_us = 0;
    media->job_phase_started_us = 0;
    media->job_units = 0;
    media->job_maximum_unit_us = 0;
    media->job_preview = false;
    media->job_resume_playing = false;
    media->job_resume_open = false;
    media->seek_preview_started = false;
    media->seek_preview_was_playing = false;
    media->seek_preview_cancel_pending = false;
    media->reopen_preview_target_us = 0;
    media->reopen_preview_pending = false;
    media->preview_commit_target_us = 0;
    media->preview_commit_pending = false;
    media->preview_commit_resume_playing = false;
    psp_ui_media_cancel_seek_preview(&media->ui);
    psp_media_raise_error(media, error, NULL);
    /* Every backend entry point refuses a quarantined firmware decoder for
       the rest of the process, so the panel must not present a Retry the
       session would only decline. Sampled after the panel is raised, because
       raising it clears the flag. */
    media->ui.retry_unavailable = media_psp_backend_quarantined();
    printf("tilefinch-media: %s failed: %s\n", operation, error);
    if (media->ui.retry_unavailable) {
        printf("tilefinch-media: retry=unavailable "
               "reason=codec-quarantine operation=%s\n", operation);
    }
    PspMediaEvent failure_event = psp_media_service_completion(
        media, opening_failure
            ? PSP_MEDIA_EVENT_OPEN_FAILED
            : seeking_failure
                ? PSP_MEDIA_EVENT_SEEK_FAILED
                : PSP_MEDIA_EVENT_PLAYBACK_FAILED);
    psp_media_session_dispatch_event(media, failure_event,
        opening_failure ? "open-failed"
        : seeking_failure ? "seek-failed" : "playback-failed");
    if (psp_media_owned_pipeline(media)
            == PSP_MEDIA_PIPELINE_NONE)
        psp_media_finish_synchronous_quiesce(
            media, "failed-released");
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    psp_media_session_checkpoint(media, "job-failed");
#endif
}

static void psp_youtube_log_text(
    const char *source, char *output, size_t output_size)
{
    if (output == NULL || output_size == 0) return;
    size_t used = 0;
    if (source != NULL) {
        while (*source != '\0' && used + 1u < output_size) {
            unsigned char byte = (unsigned char) *source++;
            output[used++] =
                byte < 0x20u || byte == 0x7fu || byte == '"'
                    ? ' ' : (char) byte;
        }
    }
    output[used] = '\0';
}

static bool psp_media_transport_refresh_needed(
    const PspMediaSession *media, long *status)
{
    if (status != NULL) *status = 0;
    if (media == NULL) return false;
    MediaHttpRangeStats video = {0};
    MediaHttpRangeStats audio = {0};
    (void) media_http_range_stats(media->range, &video);
    (void) media_http_range_stats(media->audio_range, &audio);
    long failed_status = video.failures != 0
        ? video.last_http_status
        : audio.failures != 0
            ? audio.last_http_status
            : video.last_http_status != 0
                ? video.last_http_status : audio.last_http_status;
    if (status != NULL) *status = failed_status;
    uint64_t now = tilefinch_platform_wall_time_ns()
                 / UINT64_C(1000000000);
    return video.failures != 0 || audio.failures != 0
        || psp_media_transport_refresh_policy(
        false, false, video.last_http_status, audio.last_http_status,
        media->stream.expires_unix, now);
}

bool psp_media_retry_transport(
    PspMediaSession *media, const char *operation, const char *error,
    bool delivery_candidate_rejected)
{
    long status = 0;
    bool refresh_needed = psp_media_transport_refresh_needed(media, &status);
    if (media == NULL || media->offline_source
        || media->transport_reresolve_attempts
             >= PSP_MEDIA_TRANSPORT_REFRESH_MAXIMUM_ATTEMPTS
        || psp_media_cancel_requested(media)
        || (!delivery_candidate_rejected && !refresh_needed)) {
        return false;
    }
    uint64_t resume_us = psp_media_recovery_position_us(media);
    uint64_t duration_us = psp_media_duration_us(media);
    bool commit_pending = media->preview_commit_pending;
    bool preview_pending = !commit_pending
        && media->seek_preview_started
        && (media->ui.seek_preview_active
            || media->machine.preview_active
            || media->job_preview);
    uint64_t preview_target_us = commit_pending
        ? media->preview_commit_target_us
        : media->ui.seek_preview_active
            ? media->ui.seek_preview_time_us
            : media->job_target_us;
    bool resume_playing = psp_media_retry_preview_should_resume(
        media->ui.playing, media->job_resume_playing,
        media->seek_preview_started, media->seek_preview_was_playing)
        || psp_media_machine_wants_playing(media)
        || (commit_pending && media->preview_commit_resume_playing);
    psp_media_session_dispatch_event(media, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_CLOSE,
        .retain_pipeline = false
    }, "transport-refresh-close");
    psp_media_pipeline_destroy(media);
    psp_media_finish_synchronous_quiesce(
        media, "transport-refresh-released");
    if (media_psp_backend_quarantined()) {
        psp_media_job_failed(
            media, operation,
            "VIDEO DECODER NEEDS APP RESTART");
        return true;
    }
    media->transport_reresolve_attempts++;
    media->transport_refresh_rearm_us =
        resume_us > UINT64_MAX - UINT64_C(5000000)
            ? UINT64_MAX : resume_us + UINT64_C(5000000);
    media->reopen_resume_us = resume_us;
    media->reopen_resume_playing = resume_playing;
    media->reopen_resume_pending = true;
    media->reopen_preview_target_us = preview_target_us;
    media->reopen_preview_pending = preview_pending;
    media->reopen_reuse_resolved_stream = false;
    media->open_service_pending = true;
    psp_media_session_dispatch_event(media, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_OPEN,
        .autoplay = resume_playing,
        .has_separate_audio = false,
        .audio_only = media->audio_only
    }, "transport-refresh-open");
    psp_ui_media_set_resolving(&media->ui, "Refreshing video link");
    if (preview_pending || commit_pending) {
        media->ui.duration_us = duration_us;
        psp_ui_media_set_seek_preview(&media->ui, preview_target_us);
    }
    char log_error[161];
    psp_youtube_log_text(error, log_error, sizeof(log_error));
    const char *quality_policy =
        media->requested_quality == BROWSER_YOUTUBE_QUALITY_240P
        && !media->quality_fallback_attempted
            ? "pinned"
            : media->requested_quality == BROWSER_YOUTUBE_QUALITY_360P
                ? "fallback-eligible" : "fallback-active";
    printf(
        "tilefinch-youtube: stage=transport-mid status=%ld "
        "action=reresolve operation=%s attempt=%u/%u candidate-rejected=%d "
        "quality=%up quality-policy=%s resume=%lluus "
        "reason=\"%.160s\"\n",
        status, operation == NULL ? "unknown" : operation,
        media->transport_reresolve_attempts,
        PSP_MEDIA_TRANSPORT_REFRESH_MAXIMUM_ATTEMPTS,
        delivery_candidate_rejected ? 1 : 0,
        (unsigned) media->requested_quality, quality_policy,
        (unsigned long long) resume_us,
        log_error);
    return true;
}

bool psp_media_retry_transport_expiry(PspMediaSession *media)
{
    if (media == NULL) return false;
    uint64_t now_us = psp_media_internal_now_us(media);
    if (media->transport_next_expiry_check_us != 0
        && now_us < media->transport_next_expiry_check_us) return false;
    media->transport_next_expiry_check_us =
        now_us > UINT64_MAX - UINT64_C(1000000)
            ? UINT64_MAX : now_us + UINT64_C(1000000);
    return psp_media_retry_transport(
        media, "expiry", "media URL is near expiry", false);
}

bool psp_media_retry_240p(
    PspMediaSession *media, const char *operation, const char *error,
    bool delivery_candidate_rejected)
{
    MediaBackendStats failure_stats = {0};
    if (media == NULL) return false;
    if (!delivery_candidate_rejected
        && psp_media_transport_refresh_needed(media, NULL)) return false;
    if (delivery_candidate_rejected
        && media->transport_reresolve_attempts
             >= PSP_MEDIA_TRANSPORT_REFRESH_MAXIMUM_ATTEMPTS) return false;
    if (media_psp_backend_quarantined()) return false;
    if (media != NULL && media->playback != NULL
        && media_playback_backend_stats(
            media->playback, &failure_stats)
        && failure_stats.last_native_error
             == MEDIA_BACKEND_RAW_NAL_BRIDGE_UNAVAILABLE) {
        return false;
    }
    if (media->audio_only || media->offline_source || media->page_source
        || media->requested_quality != BROWSER_YOUTUBE_QUALITY_360P
        || media->stream.height <= 240
        || media->quality_fallback_attempted
        || psp_media_cancel_requested(media)) return false;
    uint64_t resume_us = psp_media_recovery_position_us(media);
    uint64_t duration_us = psp_media_duration_us(media);
    bool commit_pending = media->preview_commit_pending;
    bool preview_pending = !commit_pending
        && media->seek_preview_started
        && (media->ui.seek_preview_active
            || media->machine.preview_active
            || media->job_preview);
    uint64_t preview_target_us = commit_pending
        ? media->preview_commit_target_us
        : media->ui.seek_preview_active
            ? media->ui.seek_preview_time_us
            : media->job_target_us;
    bool resume_playing = psp_media_retry_preview_should_resume(
        media->ui.playing, media->job_resume_playing,
        media->seek_preview_started, media->seek_preview_was_playing)
        || psp_media_machine_wants_playing(media)
        || (commit_pending && media->preview_commit_resume_playing);
    psp_media_session_dispatch_event(media, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_CLOSE,
        .retain_pipeline = false
    }, "quality-fallback-close");
    psp_media_pipeline_destroy(media);
    psp_media_finish_synchronous_quiesce(
        media, "quality-fallback-released");
    if (media_psp_backend_quarantined()) {
        /* A timed-out native job retains its complete backend so the worker
           cannot access freed memory.  Do not promise a lower-quality retry
           after that process-wide recovery boundary has closed. */
        psp_media_job_failed(
            media, operation,
            "VIDEO DECODER NEEDS APP RESTART");
        return true;
    }
    media->requested_quality = BROWSER_YOUTUBE_QUALITY_240P;
    media->quality_fallback_attempted = true;
    if (delivery_candidate_rejected) {
        /* This is the third delivery candidate, not an independent codec
           fallback. Count it against the same per-incident replacement cap
           so a CDN which rejects both qualities cannot form a resolve loop. */
        media->transport_reresolve_attempts++;
        media->transport_refresh_rearm_us =
            resume_us > UINT64_MAX - UINT64_C(5000000)
                ? UINT64_MAX : resume_us + UINT64_C(5000000);
    }
    media->reopen_resume_us = resume_us;
    media->reopen_resume_playing = resume_playing;
    media->reopen_resume_pending = true;
    media->reopen_preview_target_us = preview_target_us;
    media->reopen_preview_pending = preview_pending;
    media->reopen_reuse_resolved_stream = false;
    media->quality_fallbacks++;
    media->open_service_pending = true;
    psp_media_session_dispatch_event(media, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_OPEN,
        .autoplay = resume_playing,
        .has_separate_audio = false,
        .audio_only = media->audio_only
    }, "quality-fallback-open");
    psp_ui_media_set_resolving(
        &media->ui,
        media->stream.title[0] == '\0'
            ? "YouTube video" : media->stream.title);
    psp_ui_media_set_resolving_progress(
        &media->ui, "Loading...", 20u);
    if (preview_pending || commit_pending) {
        media->ui.duration_us = duration_us;
        psp_ui_media_set_seek_preview(&media->ui, preview_target_us);
    }
    printf(
        "tilefinch-media-quality: fallback=360p-to-240p operation=%s "
        "resume=%lluus reason=\"%.160s\" system-free=%zu "
        "delivery-candidate=%d attempt=%u/%u system-largest=%zu\n",
        operation == NULL ? "unknown" : operation,
        (unsigned long long) resume_us,
        error == NULL ? "" : error,
        psp_media_free_memory(media),
        delivery_candidate_rejected ? 1 : 0,
        media->transport_reresolve_attempts,
        PSP_MEDIA_TRANSPORT_REFRESH_MAXIMUM_ATTEMPTS,
        psp_media_maximum_free_block(media));
    return true;
}

bool psp_media_retry_delivery_failure(
    PspMediaSession *media, const char *operation, const char *error,
    bool delivery_candidate_rejected)
{
    if (media == NULL) return false;
    bool failed = psp_media_transport_refresh_needed(media, NULL);
    if (!delivery_candidate_rejected && !failed) return false;
    /* Candidate one was the requested rendition. After one fresh URL at the
       same quality fails, spend the final candidate on a separately resolved
       240p rendition instead of asking the resolver for a third equivalent
       360p URL. The quality retry increments the shared incident counter. */
    if (media->requested_quality == BROWSER_YOUTUBE_QUALITY_360P
        && media->transport_reresolve_attempts != 0
        && psp_media_retry_240p(
               media, operation, error, true)) return true;
    return psp_media_retry_transport(
        media, operation, error, delivery_candidate_rejected || failed);
}

static bool psp_media_open_pump_step(PspMediaSession *media);

/*
 * Publish the caller's cancellation token for exactly the duration of one
 * pump. Every cancellation check the step performs -- the resolver and range
 * fetch callbacks, the pump-end check, the transport and 240p retry gates --
 * reads it through psp_media_cancel_requested, so open-side cancellation now
 * depends on the token this caller passed rather than only on the
 * process-global "the active cooperate scope belongs to this open job"
 * invariant. Clearing it on the way out keeps a scope that has since ended
 * from being consulted by seek, expiry, or close paths.
 */
bool psp_media_open_pump(
    PspMediaSession *media, const TilefinchCancellation *cancellation)
{
    if (media == NULL) return false;
    bool starting = media->open_service_pending
        && media->job_phase == PSP_MEDIA_JOB_NONE;
    PspMediaJobPhase phase_before = media->job_phase;
    if (starting) {
        if (media->machine.state != PSP_MEDIA_SESSION_OPENING) {
            psp_media_session_dispatch_event(media, (PspMediaEvent) {
                .type = PSP_MEDIA_EVENT_OPEN,
                .autoplay = media->reopen_resume_playing,
                .has_separate_audio = false,
                .audio_only = media->audio_only
            }, "open-begin");
        }
        /* open_pump_step installs OPEN_RESOLVE and may also complete it in
           this same call. That happens for offline media and for a rewind
           reusing an authorized direct stream. Treat the unit as the resolve
           phase even though the physical phase was NONE at entry, otherwise
           the controller misses exactly one completion and remains Opening
           after the full pipeline has been created. Async resolution used to
           hide this because a later pump naturally entered with RESOLVE. */
        phase_before = PSP_MEDIA_JOB_OPEN_RESOLVE;
    }
    media->open_cancellation = cancellation;
    bool pumped = psp_media_open_pump_step(media);
    media->open_cancellation = NULL;
    if (phase_before >= PSP_MEDIA_JOB_OPEN_RESOLVE
        && phase_before <= PSP_MEDIA_JOB_OPEN_PLAYBACK
        && media->job_phase != phase_before) {
        if (!media->ui.failed) {
            PspMediaEvent completed = psp_media_service_completion(
                media, PSP_MEDIA_EVENT_OPEN_PHASE_COMPLETE);
            completed.has_separate_audio = media->stream.split_streams;
            completed.audio_only = media->audio_only;
            psp_media_session_dispatch_event(media, completed, "open-phase");
            if (phase_before == PSP_MEDIA_JOB_OPEN_PLAYBACK
                && psp_media_seek_phase(media->job_phase)) {
                psp_media_session_dispatch_event(media, (PspMediaEvent) {
                    .type = PSP_MEDIA_EVENT_SEEK
                }, "open-resume-seek");
            } else if (phase_before == PSP_MEDIA_JOB_OPEN_PLAYBACK) {
                (void) psp_media_begin_startup_preroll(media);
                /* A passive open still owes the page a stable poster frame.
                   An autoplay open already committed PLAYING as its resume
                   target; arming the legacy one-frame pause here changed it
                   back to PAUSED as soon as its first picture arrived. */
                bool wants_playing =
                    psp_media_machine_wants_playing(media);
                if (!wants_playing) {
                    psp_media_session_dispatch_event(media, (PspMediaEvent) {
                        .type = PSP_MEDIA_EVENT_PAUSE_AFTER_FRAME
                    }, "open-first-frame-boundary");
                    if (media->audio_only)
                        psp_media_session_dispatch_event(
                            media, (PspMediaEvent) {
                                .type = PSP_MEDIA_EVENT_FRAME_DISPLAYED
                            }, "open-audio-boundary");
                }
                media_playback_set_playing(media->playback, wants_playing);
            }
        }
    }
    if (media->ui.failed
        && psp_media_owned_pipeline(media)
               == PSP_MEDIA_PIPELINE_NONE)
        psp_media_finish_synchronous_quiesce(
            media, "open-failed-released");
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    psp_media_session_checkpoint(media, "open-pump");
#endif
    return pumped;
}

/* The stage names the open reports, so a log line and a failure both say which
   phase a transaction was in rather than only that it was opening. */
static const char *psp_media_open_stage_name(PspMediaJobPhase phase)
{
    switch (phase) {
        case PSP_MEDIA_JOB_OPEN_RESOLVE: return "resolve";
        case PSP_MEDIA_JOB_OPEN_VIDEO_RANGE: return "video-range";
        case PSP_MEDIA_JOB_OPEN_VIDEO_DEMUX: return "video-demux";
        case PSP_MEDIA_JOB_OPEN_VIDEO_PRIME: return "video-prime";
        case PSP_MEDIA_JOB_OPEN_AUDIO_RANGE: return "audio-range";
        case PSP_MEDIA_JOB_OPEN_AUDIO_DEMUX: return "audio-demux";
        case PSP_MEDIA_JOB_OPEN_DECODER_PREPARE: return "decoder-prepare";
        case PSP_MEDIA_JOB_OPEN_PLAYBACK: return "playback";
        default: return "none";
    }
}

/*
 * Hand both range sources what is left of the open's clocks, so the blocking
 * reads a demux performs share the phase budget instead of each re-arming the
 * per-window timeout. Cleared when the transaction ends: playback's reads are
 * governed by the horizon and the per-window bound, not by an open that is
 * over.
 */
void psp_media_open_arm_wait_budget(PspMediaSession *media)
{
    uint64_t budget_us = psp_media_open_wait_budget_us(
        media->job_started_us, media->job_phase_started_us,
        psp_media_internal_now_us(media));
    media_http_range_set_wait_budget_us(media->range, budget_us);
    media_http_range_set_wait_budget_us(media->audio_range, budget_us);
}

/* The same handoff for a seek, on the seek's own much smaller clock. */
void psp_media_seek_arm_wait_budget(PspMediaSession *media)
{
    uint64_t budget_us = psp_media_seek_wait_budget_us(
        media->job_started_us, psp_media_internal_now_us(media));
    media_http_range_set_wait_budget_us(media->range, budget_us);
    media_http_range_set_wait_budget_us(media->audio_range, budget_us);
}

void psp_media_open_clear_wait_budget(PspMediaSession *media)
{
    media_http_range_clear_wait_budget(media->range);
    media_http_range_clear_wait_budget(media->audio_range);
}

/*
 * Say where an open is while it is still there.
 *
 * The device run this exists for printed `tilefinch-media-modules: stage=ready`
 * and then nothing at all for more than ten minutes: the phase could not be
 * named, the elapsed time could not be read, and whether any byte had moved
 * could not be told from a log at all. One line every five seconds costs a
 * stuck open nothing and turns silence into a diagnosis.
 */
static void psp_media_open_report(PspMediaSession *media, const char *event)
{
    MediaHttpRangeStats video = {0};
    MediaHttpRangeStats audio = {0};
    (void) media_http_range_stats(media->range, &video);
    (void) media_http_range_stats(media->audio_range, &audio);
    uint64_t now_us = psp_media_internal_now_us(media);
    printf("tilefinch-media-open: event=%s stage=%s elapsed=%lluus "
           "phase-elapsed=%lluus bytes=%zu window-pending=%d/%d "
           "inflight=%zu/%zu\n",
           event, psp_media_open_stage_name(media->job_phase),
           (unsigned long long) (media->job_started_us == 0
               || now_us < media->job_started_us
                   ? 0 : now_us - media->job_started_us),
           (unsigned long long) (media->job_phase_started_us == 0
               || now_us < media->job_phase_started_us
                   ? 0 : now_us - media->job_phase_started_us),
           video.bytes_received + audio.bytes_received,
           video.window_pending ? 1 : 0, audio.window_pending ? 1 : 0,
           video.bytes_in_flight, audio.bytes_in_flight);
}

/*
 * The open's own deadline and cancellation check, asked in one place.
 *
 * Returns true when the transaction was retired here. Every open phase runs
 * behind this, not only module preparation: the phases that call into a
 * network source used to inherit whatever bound that source applied to one
 * read, and those bounds do not compose.
 */
static bool psp_media_open_retire_if_over(PspMediaSession *media)
{
    PspMediaOpenWatchVerdict verdict = psp_media_open_watch(
        psp_media_cancel_requested(media), media->job_started_us,
        media->job_phase_started_us, psp_media_internal_now_us(media));
    if (verdict == PSP_MEDIA_OPEN_WATCH_CONTINUE) return false;
    psp_media_open_report(
        media,
        verdict == PSP_MEDIA_OPEN_WATCH_CANCELLED ? "cancelled"
            : verdict == PSP_MEDIA_OPEN_WATCH_PHASE_TIMEOUT
                ? "phase-timeout" : "total-timeout");
    char failure[96];
    if (verdict == PSP_MEDIA_OPEN_WATCH_CANCELLED) {
        snprintf(failure, sizeof(failure), "%s", "VIDEO OPEN STOPPED");
    } else {
        snprintf(failure, sizeof(failure), "VIDEO OPEN TIMED OUT IN %s",
                 psp_media_open_stage_name(media->job_phase));
    }
    media->open_service_pending = false;
    psp_media_open_clear_wait_budget(media);
    psp_media_job_failed(media, "open", failure);
    return true;
}

bool psp_media_open_watchdog(PspMediaSession *media)
{
    /*
     * The frames on which nothing pumps the open at all.
     *
     * The interactive loop suppresses psp_media_advance while a page
     * navigation owns the cooperate scope, so an open can sit with its phase
     * clock running and nothing looking at it -- no deadline, and no path for
     * a CIRCLE press to reach the transaction. The loop calls this every frame
     * an open is pending, whether or not it also pumps, which is what makes
     * the bound and the cancel law independent of who is running.
     */
    if (media == NULL || !psp_media_open_work_pending(media)) return false;
    if (media->open_service_pending && media->job_phase == PSP_MEDIA_JOB_NONE)
        return false;
    return psp_media_open_retire_if_over(media);
}

static bool psp_media_open_pump_step(PspMediaSession *media)
{
    if (media == NULL) return false;
    if (media->open_service_pending) {
        media->open_service_pending = false;
        psp_media_set_transport_priority(
            media, media->page_source
                || (youtube_watch_url_supported(media->source)
                    && !psp_media_offline_route(media, media->source)));
        /* A quarantined firmware worker or audio channel still owns the
           intentionally leaked backend. Retrying network and MP4 preparation
           cannot repair that process-wide ownership boundary, so fail before
           spending seconds and scarce contiguous memory on known-dead work. */
        if (media_psp_backend_quarantined()) {
            psp_media_job_failed(
                media, "open", "VIDEO DECODER NEEDS APP RESTART");
            return true;
        }
        bool reuse_resolved_stream =
            media->reopen_reuse_resolved_stream
            && psp_media_resolved_stream_reusable(media);
        media->reopen_reuse_resolved_stream = reuse_resolved_stream;
        psp_media_pipeline_destroy(media);
        if (!reuse_resolved_stream)
            memset(&media->stream, 0, sizeof(media->stream));
        media->job_phase = PSP_MEDIA_JOB_OPEN_RESOLVE;
        media->job_started_us =
            psp_media_internal_now_us(media);
        media->job_phase_started_us = media->job_started_us;
        media->job_maximum_unit_us = 0;
        media->job_units = 0;
        media->open_report_next_us = media->job_started_us;
    }
    if (media->job_phase < PSP_MEDIA_JOB_OPEN_RESOLVE
        || media->job_phase > PSP_MEDIA_JOB_OPEN_PLAYBACK) return false;
    /* Before the unit, not after it: a phase that is already over must not be
       given one more blocking read to spend. */
    if (psp_media_open_retire_if_over(media)) return true;
    uint64_t unit_started =
        psp_media_internal_now_us(media);
    if (unit_started >= media->open_report_next_us) {
        psp_media_open_report(media, "progress");
        media->open_report_next_us =
            unit_started + PSP_MEDIA_OPEN_REPORT_INTERVAL_US;
    }
    /* Whatever this unit calls into inherits the transaction's remaining
       time, so the reads inside one demux share one budget. */
    psp_media_open_arm_wait_budget(media);
    int phase_before = (int) media->job_phase;
    /* The internal stage remains in telemetry; the player surface presents
       one stable operation instead of flashing implementation terms such as
       "video demux" and "audio range" at the user. These values follow the
       actual phase order, so the visible bar never moves backwards. */
    static const unsigned progress[] = {
        [PSP_MEDIA_JOB_OPEN_RESOLVE] = 80u,
        [PSP_MEDIA_JOB_OPEN_VIDEO_RANGE] = 360u,
        [PSP_MEDIA_JOB_OPEN_VIDEO_DEMUX] = 520u,
        [PSP_MEDIA_JOB_OPEN_VIDEO_PRIME] = 820u,
        [PSP_MEDIA_JOB_OPEN_AUDIO_RANGE] = 600u,
        [PSP_MEDIA_JOB_OPEN_AUDIO_DEMUX] = 720u,
        [PSP_MEDIA_JOB_OPEN_DECODER_PREPARE] = 220u,
        [PSP_MEDIA_JOB_OPEN_PLAYBACK] = 900u
    };
    if ((size_t) media->job_phase
            < sizeof(progress) / sizeof(progress[0])
        && progress[media->job_phase] != 0u) {
        psp_ui_media_set_resolving_progress(
            &media->ui, "Loading...", progress[media->job_phase]);
    }
    char error[256] = {0};
    bool ok = true;
    switch (media->job_phase) {
    case PSP_MEDIA_JOB_OPEN_RESOLVE: {
        bool offline_route = psp_media_offline_route(media, media->source);
        bool reused = media->reopen_reuse_resolved_stream
            && psp_media_resolved_stream_reusable(media);
        media->reopen_reuse_resolved_stream = false;
        if (reused) {
            uint64_t now = tilefinch_platform_wall_time_ns()
                / UINT64_C(1000000000);
            printf(
                "tilefinch-media-open: event=reuse-resolved-stream "
                "itag=%d audio-itag=%d expire-remaining=%llus\n",
                media->stream.itag, media->stream.audio_itag,
                (unsigned long long) (media->offline_source
                    || media->stream.expires_unix <= now
                        ? 0 : media->stream.expires_unix - now));
        } else if (media->page_hls) {
            if (media->hls == NULL) {
                media->hls = psp_media_hls_create(
                    media->budget, media->session,
                    media->source, media->page_document_url,
                    media->page_media_mode, media->page_media_credentials,
                    error, sizeof(error));
            }
            if (media->hls == NULL) {
                ok = false;
            } else {
                PspMediaHlsOpenStatus status = psp_media_hls_pump(
                    media->hls, error, sizeof(error));
                if (status == PSP_MEDIA_HLS_OPEN_PENDING) {
                    ok = true;
                    break;
                }
                ok = status == PSP_MEDIA_HLS_OPEN_READY
                    && psp_media_hls_sample_source(
                        media->hls, &media->hls_source);
                if (ok) {
                    MediaMp4TrackInfo video = {0}, audio = {0};
                    ok = psp_media_hls_stream_info(
                        media->hls, &video, &audio);
                    if (ok) {
                        memset(&media->stream, 0, sizeof(media->stream));
                        snprintf(media->stream.title,
                                 sizeof(media->stream.title), "%s",
                                 "Page video");
                        snprintf(media->stream.mime_type,
                                 sizeof(media->stream.mime_type), "%s",
                                 "video/mp2t; codecs=avc1.64001e,mp4a.40.2");
                        media->stream.width = video.width;
                        media->stream.height = video.height;
                        media->stream.duration_ms =
                            video.timescale == 0 ? 0
                            : video.duration * UINT64_C(1000)
                                / video.timescale;
                        media->stream.expires_unix = UINT64_MAX;
                        media->use_swdec = true;
                        media->decoder_profile_idc = 100u;
                    }
                }
            }
        } else if (media->page_source) {
            PspPageMediaProbeStatus status = psp_media_page_probe(
                media, error, sizeof(error));
            if (status == PSP_PAGE_MEDIA_PROBE_PENDING) {
                ok = true;
                break;
            }
            ok = status == PSP_PAGE_MEDIA_PROBE_READY;
        } else if (offline_route) {
            memset(&media->stream, 0, sizeof(media->stream));
            PspMediaOfflineSource source = {0};
            ok = media->platform.resolve_offline(
                media->platform.context, media->source, &source);
            if (ok) {
                media->stream = source.stream;
                snprintf(
                    media->offline_video_path,
                    sizeof(media->offline_video_path), "%s",
                    source.video_path);
                snprintf(
                    media->offline_audio_path,
                    sizeof(media->offline_audio_path), "%s",
                    source.audio_path);
                media->offline_source = true;
            } else {
                snprintf(error, sizeof(error),
                         "offline video is unavailable");
            }
        } else if (fetch_background_transport_available()) {
            memset(&media->stream, 0, sizeof(media->stream));
            if (media->resolver_job == NULL) {
                media->resolver_job = youtube_resolve_job_begin(
                    media->budget, media->session, media->source,
                    (int) media->requested_quality, 30000,
                    psp_media_cancel_callback, media);
            }
            if (media->resolver_job == NULL) {
                ok = false;
                snprintf(error, sizeof(error), "%s",
                         "player: resolver worker was unavailable");
            } else {
                YoutubeResolveJobStatus status =
                    youtube_resolve_job_pump(media->resolver_job);
                if (status == YOUTUBE_RESOLVE_JOB_PENDING) {
                    ok = true;
                    break;
                }
                ok = status == YOUTUBE_RESOLVE_JOB_COMPLETE
                    && youtube_resolve_job_take(
                           media->resolver_job, &media->stream);
                if (!ok) {
                    snprintf(error, sizeof(error), "%s",
                             youtube_resolve_job_error(
                                 media->resolver_job));
                }
                youtube_resolve_job_destroy(media->resolver_job);
                media->resolver_job = NULL;
            }
        } else {
            memset(&media->stream, 0, sizeof(media->stream));
            ok = youtube_resolve_progressive_mp4_cancelable(
                media->budget, media->session, media->source,
                (int) media->requested_quality, 30000,
                psp_media_cancel_callback, media,
                &media->stream, error, sizeof(error));
        }
        if (ok && media->audio_only && !media->page_audio
            && (!media->stream.split_streams
                || media->stream.audio_url[0] == '\0'
                || media->stream.audio_content_length == 0)) {
            ok = false;
            snprintf(error, sizeof(error), "%s",
                     "format: adaptive AAC unavailable for audio-only");
        }
        if (ok) {
            psp_media_apply_decoder_hint(media);
            uint64_t now = tilefinch_platform_wall_time_ns()
                         / UINT64_C(1000000000);
            uint64_t expiry_remaining =
                media->stream.expires_unix > now
                    ? media->stream.expires_unix - now : 0;
            printf(
                "tilefinch-media: resolved itag=%d audio-itag=%d split=%d "
                "source=%dx%d video=%lluB audio=%lluB "
                "mime=%.80s audio-mime=%.80s\n",
                media->stream.itag, media->stream.audio_itag,
                media->stream.split_streams ? 1 : 0,
                media->stream.width, media->stream.height,
                (unsigned long long) media->stream.content_length,
                (unsigned long long) media->stream.audio_content_length,
                media->stream.mime_type, media->stream.audio_mime_type);
            printf(
                "tilefinch-youtube: stage=%s status=%ld client=%s "
                "attempts=%u itag=%d audio-itag=%d "
                "expire-remaining=%llus watch-bytes=%zu "
                "player-bytes=%zu\n",
                media->offline_source ? "offline" : "player",
                media->stream.player_status,
                media->stream.client_name[0] == '\0'
                    ? "unknown" : media->stream.client_name,
                media->stream.client_attempts,
                media->stream.itag, media->stream.audio_itag,
                (unsigned long long) expiry_remaining,
                media->stream.watch_bytes, media->stream.player_bytes);
            /* Load the process-wide decoder modules before allocating the
               two range windows and fragment tables. On PSP the module
               worker and firmware PRXs need contiguous memory; preparing at
               the old post-demux checkpoint made otherwise valid playback
               depend on allocation order and defeated the lower-memory 240p
               retry. */
            /* Provider metadata names its codec before any range allocation,
               so it can preserve the early low-memory module admission.
               Generic page media says only video/mp4 until avcC is parsed;
               defer its decoder choice until VIDEO_DEMUX rather than loading
               firmware first and the larger software component afterward. */
            media->job_phase = media->page_hls
                ? PSP_MEDIA_JOB_OPEN_DECODER_PREPARE
                : media->page_audio
                ? PSP_MEDIA_JOB_OPEN_DECODER_PREPARE
                : media->page_source
                ? PSP_MEDIA_JOB_OPEN_VIDEO_RANGE
                : PSP_MEDIA_JOB_OPEN_DECODER_PREPARE;
        } else {
            const char *stage = "format";
            if (strncmp(error, "watch:", 6) == 0) stage = "watch";
            else if (strncmp(error, "player:", 7) == 0)
                stage = "player";
            else if (strncmp(error, "playability:", 12) == 0)
                stage = "playability";
            char log_error[201];
            psp_youtube_log_text(error, log_error, sizeof(log_error));
            printf(
                "tilefinch-youtube: stage=%s status=0 "
                "reason=\"%s\"\n", stage, log_error);
        }
        break;
    }
    case PSP_MEDIA_JOB_OPEN_VIDEO_RANGE: {
        if (media->offline_source) {
            media->file_range = media_file_range_open(
                media->budget, media->offline_video_path,
                media->stream.content_length,
                error, sizeof(error));
            ok = media->file_range != NULL;
            if (ok) media->job_phase = PSP_MEDIA_JOB_OPEN_VIDEO_DEMUX;
            break;
        }
        MediaHttpRangeOptions range_options = {
            /* A captured 23.976 fps replay under a fixed burst/gap schedule
               measured 128 KiB at one 3.07 s hold. With the header-admitted
               streaming successor below, 256 KiB reduced the longest hold to
               0.56 s and total held time by 21%: most boundary samples could
               consume arrived prefixes before the response tail reached EOF.
               The extra 256 KiB is bounded and the allocation still degrades
               safely to active+in-flight if pressure refuses the successor. */
            .cache_bytes = 256u * KIB,
            /* Active + one successor is the guaranteed 512 KiB path. A
               second successor is attempted through the same Budget and is
               optional; request admission remains at one until audio has its
               three-second reserve. */
            .lookahead_windows = 2,
            .initial_lookahead_windows = 1,
            /* Deterministic 48 KiB publication replay tripled the longest
               captured-CDN hold (0.56 -> 1.62 s). A media-only 16 KiB quantum
               preserves navigation's coarser worker throughput policy. */
            .stream_publication_bytes = 16u * KIB,
            .minimum_sustained_bytes_per_second =
                psp_media_transport_rate_floor(
                    media->stream.content_length,
                    media->stream.duration_ms),
            .timeout_ms = 15000,
            .connect_timeout_ms = PSP_MEDIA_CONNECT_TIMEOUT_MS,
            .referer = media->page_source
                ? media->page_document_url : media->source,
            .standard_range_header = media->page_source,
            .page_request_context = media->page_source
                ? &(TilefinchRequestContext) {
                    .target_url = media->stream.media_url,
                    .initiator_url = media->page_document_url,
                    .top_level_url = media->page_document_url,
                    .method = "GET",
                    .mode = media->page_media_mode,
                    .credentials = media->page_media_credentials,
                    .destination = TILEFINCH_DESTINATION_MEDIA
                  } : NULL,
            .url_validator = media->page_source
                ? NULL : youtube_media_url_supported,
            .cancel = psp_media_cancel_callback,
            .cancel_opaque = media
        };
        media->range = media_http_range_create(
            media->budget, media->session, media->stream.media_url,
            media->stream.content_length, &range_options,
            error, sizeof(error));
        ok = media->range != NULL;
        if (ok) {
            media->video_lookahead_limit = 1u;
            media->video_lookahead_next_sample_us = 0;
            /* Buffered startup is useful only if it overlaps work before the
               play press. Arm the first bounded successor now; the optional
               second remains gated by audio reserve. */
            if (browser_profile_video_startup_buffering(media->profile))
                media_http_range_set_aggressive_readahead(
                    media->range, true);
            /* The module worker has already finished, so both required range
               caches are now safe to admit. Put the video metadata window on
               the single transport worker first, then its audio companion;
               this removes frame-boundary idle gaps without concurrent curl
               work or a larger peak allocation. A deferred hint remains an
               ordinary metadata read, not an open failure. */
            (void) media_http_range_prefetch_metadata(media->range);
            if (media->stream.split_streams && !media->page_source) {
                ok = psp_media_create_audio_http_range(
                    media, error, sizeof(error));
                if (ok)
                    (void) media_http_range_prefetch_metadata(
                        media->audio_range);
            }
        }
        if (ok) {
            media->job_phase = PSP_MEDIA_JOB_OPEN_VIDEO_DEMUX;
        }
        break;
    }
    case PSP_MEDIA_JOB_OPEN_VIDEO_DEMUX: {
        MediaRangeReader reader = media->offline_source
            ? media_file_range_reader(media->file_range)
            : media_http_range_reader(media->range);
        media->demux = media_mp4_open(
            media->budget, &reader, NULL, error, sizeof(error));
        ok = media->demux != NULL;
        if (ok) {
            psp_media_page_track_metadata(media, media->demux);
            ok = psp_media_apply_demux_decoder_route(
                media, error, sizeof(error));
        }
        if (ok) {
            /* Give the small audio metadata reads first access to the link.
               A full video successor is useful read-ahead, but issuing it
               before audio open can monopolize a slow PSP connection and
               merely move the visible startup stall to the next phase. */
            media->job_phase = media->page_source
                ? PSP_MEDIA_JOB_OPEN_DECODER_PREPARE
                : media->stream.split_streams
                ? PSP_MEDIA_JOB_OPEN_AUDIO_RANGE
                : PSP_MEDIA_JOB_OPEN_VIDEO_PRIME;
        }
        break;
    }
    case PSP_MEDIA_JOB_OPEN_VIDEO_PRIME: {
        MediaHttpRangePrimeStatus primed = media->offline_source
                || !browser_profile_video_startup_buffering(media->profile)
            ? MEDIA_HTTP_RANGE_PRIME_READY
            : media_http_range_prime_successor(media->range);
        /* Prime starts the ordinary successor-window read-ahead, but a full
           256 KiB response is not a prerequisite for opening audio or the
           decoder. On PSP Wi-Fi that mandatory gate could spend the entire
           phase deadline at a truthful 60%. The worker retains the request
           and playback pumps it from the same bounded range source. */
        ok = primed != MEDIA_HTTP_RANGE_PRIME_FAILED;
        if (ok) {
            media->job_phase = PSP_MEDIA_JOB_OPEN_PLAYBACK;
        } else {
            MediaRangeReader reader = media_http_range_reader(media->range);
            if (reader.describe_failure == NULL
                || !reader.describe_failure(
                       reader.opaque, error, sizeof(error))) {
                snprintf(error, sizeof(error), "%s",
                         "video delivery rejected a later range");
            }
        }
        break;
    }
    case PSP_MEDIA_JOB_OPEN_AUDIO_RANGE: {
        if (media->offline_source) {
            media->audio_file_range = media_file_range_open(
                media->budget, media->offline_audio_path,
                media->stream.audio_content_length,
                error, sizeof(error));
            ok = media->audio_file_range != NULL;
            if (ok) media->job_phase = PSP_MEDIA_JOB_OPEN_AUDIO_DEMUX;
            break;
        }
        ok = psp_media_create_audio_http_range(
            media, error, sizeof(error));
        if (ok) {
            (void) media_http_range_prefetch_metadata(media->audio_range);
            media->job_phase = PSP_MEDIA_JOB_OPEN_AUDIO_DEMUX;
        }
        break;
    }
    case PSP_MEDIA_JOB_OPEN_AUDIO_DEMUX: {
        MediaRangeReader reader = media->offline_source
            ? media_file_range_reader(media->audio_file_range)
            : media_http_range_reader(media->audio_range);
        media->audio_demux = media_mp4_open(
            media->budget, &reader, NULL, error, sizeof(error));
        ok = media->audio_demux != NULL;
        if (ok && media->page_audio) {
            psp_media_page_track_metadata(media, media->audio_demux);
            ok = psp_media_page_validate_audio_track(
                media, error, sizeof(error));
        }
        if (ok) media->job_phase = media->audio_only
            ? PSP_MEDIA_JOB_OPEN_PLAYBACK
            : PSP_MEDIA_JOB_OPEN_VIDEO_PRIME;
        break;
    }
    case PSP_MEDIA_JOB_OPEN_DECODER_PREPARE: {
        uint64_t now_us = psp_media_internal_now_us(media);
        if (psp_media_deadline_reached(
                media->job_phase_started_us, now_us,
                PSP_MEDIA_MODULE_PREPARE_TIMEOUT_US)) {
            snprintf(
                error, sizeof(error), "%s",
                "PSP AV module worker timed out");
            ok = false;
            break;
        }
        if (media->use_swdec) {
            ok = psp_swdec_component_prepare(
                &media->swdec, error, sizeof(error));
        } else {
            MediaPspPrepareResult prepared =
                media_psp_backend_prepare_pump(error, sizeof(error));
            if (prepared == MEDIA_PSP_PREPARE_PENDING) break;
            ok = prepared == MEDIA_PSP_PREPARE_READY;
        }
        if (ok) media->job_phase = media->hls != NULL
            ? PSP_MEDIA_JOB_OPEN_PLAYBACK
            : media->page_audio && media->audio_demux != NULL
            ? PSP_MEDIA_JOB_OPEN_PLAYBACK
            : media->demux != NULL
            ? media->stream.split_streams
                ? PSP_MEDIA_JOB_OPEN_AUDIO_RANGE
                : PSP_MEDIA_JOB_OPEN_VIDEO_PRIME
            : media->audio_only
            ? PSP_MEDIA_JOB_OPEN_AUDIO_RANGE
            : PSP_MEDIA_JOB_OPEN_VIDEO_RANGE;
        break;
    }
    case PSP_MEDIA_JOB_OPEN_PLAYBACK: {
        ok = psp_media_create_playback(media, 0, error, sizeof(error));
        if (!ok) break;
        media->clock_us = 0;
        media->have_frame = false;
        media->pause_boundary_pending =
            !psp_media_machine_wants_playing(media);
        media->first_frame_started_us = psp_media_internal_now_us(media);
        media->first_frame_opened_us = media->first_frame_started_us;
        media->first_frame_pump_us = 0;
        media->first_frame_bytes = psp_media_range_bytes(media);
        media->first_frame_codec_logged = false;
        printf("tilefinch-media-first-frame: stage=open-complete at=%lluus "
               "source=%dx%d itag=%d\n",
               (unsigned long long) media->first_frame_opened_us,
               media->stream.width, media->stream.height,
               media->stream.itag);
        psp_ui_media_set_resolving(&media->ui, media->stream.title);
        media->ui.duration_us = psp_media_duration_us(media);
        psp_ui_media_set_resolving_progress(
            &media->ui, "Loading...", 920u);
        /* set_resolving necessarily clears an old pipeline's controls. A
           retry-open is different: its highlighted scrub target is live
           input state and must survive every open phase, including the final
           playback-create refresh where Cross may arrive on the supervisor
           thread. */
        if (media->preview_commit_pending)
            psp_ui_media_set_seek_preview(
                &media->ui, media->preview_commit_target_us);
        else if (media->reopen_preview_pending)
            psp_ui_media_set_seek_preview(
                &media->ui, media->reopen_preview_target_us);
        uint64_t duration_us = psp_media_duration_us(media);
        /* Only an in-process retry, quality fallback, or system resume may
           continue an existing playback transaction. A fresh watch-page open
           always starts at zero: durable profile positions made a later app
           launch pay an immediate random-access seek over a new CDN route,
           which is both surprising and less reliable than a clean start. A
           zero/paused recovery already has the desired initial state and
           avoids an unnecessary decoder reset. */
        bool reopen_resume_available = media->reopen_resume_pending
            && (media->reopen_resume_us != 0
                || media->reopen_resume_playing)
            && duration_us != 0;
        if (reopen_resume_available) {
            media->job_target_us = media->reopen_resume_us > duration_us
                ? duration_us : media->reopen_resume_us;
            media->job_resume_playing = media->reopen_resume_playing;
            media->job_preview = false;
            media->job_resume_open = true;
            media->job_phase = PSP_MEDIA_JOB_SEEK_PREPARE;
            media->job_started_us = psp_media_internal_now_us(media);
            media->job_phase_started_us = 0;
            media->job_units = 0;
            media->job_maximum_unit_us = 0;
            media->pause_boundary_pending = false;
            media->first_frame_started_us = 0;
            media->first_frame_opened_us = 0;
            media->first_frame_pump_us = 0;
            media->last_resume_saved_us = media->job_target_us;
        } else {
            media->job_phase = PSP_MEDIA_JOB_NONE;
            media->job_started_us = 0;
            media->job_phase_started_us = 0;
            media->job_units = 0;
            media->job_maximum_unit_us = 0;
        }
        if (media->reopen_seek_completion_pending
            && !reopen_resume_available) {
            /* A zero-position paused reopen needs no follow-up seek. The
               replacement backend is already at its target, so this is the
               completion boundary for that special case. */
            media->seek_completions++;
            media->reopen_seek_completion_pending = false;
        }
        media->reopen_resume_us = 0;
        media->reopen_resume_playing = false;
        media->reopen_resume_pending = false;
        break;
    }
    default:
        return false;
    }
    /* The module worker is intentionally not killed mid-firmware call, but
       cancellation must still retire this open transaction. Previously a
       pending prepare phase survived after the UI said it had stopped and
       resumed under a fresh token on the next frame. */
    if (psp_media_cancel_requested(media)) {
        ok = false;
        snprintf(error, sizeof(error), "%s", "VIDEO OPEN STOPPED");
    }
    uint64_t unit_us =
        psp_media_internal_now_us(media) - unit_started;
    media->job_units++;
    if (unit_us > media->job_maximum_unit_us)
        media->job_maximum_unit_us = unit_us;
    if (ok && (int) media->job_phase != phase_before
        && media->job_phase >= PSP_MEDIA_JOB_OPEN_RESOLVE
        && media->job_phase <= PSP_MEDIA_JOB_OPEN_PLAYBACK) {
        media->job_phase_started_us = psp_media_internal_now_us(media);
    }
    /* The transaction's budget belongs to the transaction. Playback's reads
       are governed by the horizon and the ordinary per-window bound. */
    if (!ok || media->job_phase < PSP_MEDIA_JOB_OPEN_RESOLVE
        || media->job_phase > PSP_MEDIA_JOB_OPEN_PLAYBACK) {
        psp_media_open_clear_wait_budget(media);
    }
    if (ok && phase_before == PSP_MEDIA_JOB_OPEN_PLAYBACK) {
        MediaHttpRangeStats video_http = {0};
        MediaHttpRangeStats audio_http = {0};
        (void) media_http_range_stats(media->range, &video_http);
        (void) media_http_range_stats(media->audio_range, &audio_http);
        printf("tilefinch-media: pipeline-ready itag=%d audio-itag=%d "
               "split=%d source=%dx%d length=%llu cache=256KiB "
               "units=%zu max-unit=%lluus\n",
               media->stream.itag, media->stream.audio_itag,
               media->stream.split_streams ? 1 : 0,
               media->stream.width, media->stream.height,
               (unsigned long long) media->stream.content_length,
               media->job_units,
               (unsigned long long) media->job_maximum_unit_us);
        printf("tilefinch-media-open-transport: "
               "video=req:%zu/pump:%zu/%lluus/install:%lluus/bytes:%zu "
               "audio=req:%zu/pump:%zu/%lluus/install:%lluus/bytes:%zu\n",
               video_http.requests, video_http.pump_calls,
               (unsigned long long) video_http.pump_max_us,
               (unsigned long long) video_http.install_max_us,
               video_http.bytes_received,
               audio_http.requests, audio_http.pump_calls,
               (unsigned long long) audio_http.pump_max_us,
               (unsigned long long) audio_http.install_max_us,
               audio_http.bytes_received);
    }
    if (!ok) {
        MediaHttpRangeStats video_stats = {0};
        MediaHttpRangeStats audio_stats = {0};
        (void) media_http_range_stats(media->range, &video_stats);
        (void) media_http_range_stats(media->audio_range, &audio_stats);
        printf("tilefinch-media: open-phase=%d "
               "video-ranges=%zu/retry=%zu/%zuB/fail=%zu/http=%ld "
               "audio-ranges=%zu/retry=%zu/%zuB/fail=%zu/http=%ld "
               "system-free=%zu "
               "system-largest=%zu\n",
               phase_before,
               video_stats.requests, video_stats.retry_attempts,
               video_stats.bytes_received,
               video_stats.failures, video_stats.last_http_status,
               audio_stats.requests, audio_stats.retry_attempts,
               audio_stats.bytes_received,
               audio_stats.failures, audio_stats.last_http_status,
               psp_media_free_memory(media),
               psp_media_maximum_free_block(media));
        if (psp_media_cancel_requested(media)) {
            snprintf(error, sizeof(error), "%s", "VIDEO OPEN STOPPED");
        }
        const char *failure =
            error[0] == '\0' ? "media open failed" : error;
        bool delivery_candidate_rejected =
            phase_before == PSP_MEDIA_JOB_OPEN_VIDEO_PRIME
            || video_stats.failures != 0 || audio_stats.failures != 0;
        bool fallback = false;
        /* Candidate one is the requested 360p stream, candidate two is a
           fresh URL at the same quality, and candidate three is newly
           resolved at 240p. Resolution is deliberately changed only after a
           second candidate proves the problem is delivery, not one bad URL. */
        fallback = psp_media_retry_delivery_failure(
            media, "open", failure, delivery_candidate_rejected);
        /* Decoder-module preparation is independent of the selected video
           resolution.  Advertising a 240p retry here only repeats the same
           module failure and leaves the user waiting for a fallback which
           cannot help.  Backend creation and decode failures still retain
           the lower-memory/lower-profile 240p recovery. */
        if (!fallback
            && phase_before != PSP_MEDIA_JOB_OPEN_DECODER_PREPARE) {
            fallback = psp_media_retry_240p(
                media, "open", failure, false);
        }
        if (!fallback) {
            psp_media_report_failure_snapshot(
                media, "media-open", "VIDEO COULD NOT OPEN", failure,
                true);
            psp_media_pipeline_destroy(media);
            psp_media_job_failed(media, "open", failure);
        }
    }
    return true;
}

bool psp_media_open_work_pending(const PspMediaSession *media)
{
    return media != NULL
        && media->machine.state == PSP_MEDIA_SESSION_OPENING;
}

bool psp_media_decode_work_pending(const PspMediaSession *media)
{
    if (media == NULL || !media->ui.visible
        || psp_media_open_work_pending(media)) return false;
    if (media->job_phase == PSP_MEDIA_JOB_SEEK_PREPARE
        || media->job_phase == PSP_MEDIA_JOB_SEEK_PRIME
        || media->job_phase == PSP_MEDIA_JOB_SEEK_DECODE
        || media->job_phase == PSP_MEDIA_JOB_PREVIEW_RESTORE_PREPARE
        || media->job_phase == PSP_MEDIA_JOB_PREVIEW_RESTORE_DECODE) {
        return true;
    }
    return media->playback != NULL
        && (media->ui.playing || media->decode_job_pending
            || media->pause_boundary_pending);
}
