#include "tilefinch/media_replay_check.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "tilefinch/fetch.h"
#include "tilefinch/media_http.h"
#include "tilefinch/media_mp4.h"
#include "tilefinch/media_source.h"
#include "tilefinch/platform.h"
#include "tilefinch/user_agent.h"
#include "media_backend_psp_policy.h"
#include "media_video_geometry.h"

/* The page-media probe's sniff and the playing path's bounds, as
   src/psp_media_open.c sets them. */
#define CHECK_PROBE_BYTES MEDIA_HTTP_PROBE_SNIFF_BYTES
#define CHECK_SAMPLE_BYTES (1024u * 1024u)
#define CHECK_WALL_LIMIT_US UINT64_C(180000000)
#define CHECK_IDLE_LIMIT 200000u

static void check_error(MediaReplayCheckReport *report, const char *format,
                        ...)
{
    if (report->error[0] != '\0') return;
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(report->error, sizeof(report->error), format, arguments);
    va_end(arguments);
}

static bool check_url_is_hls(const char *url)
{
    size_t path = strcspn(url, "?#");
    for (size_t at = 0; at + 5u <= path; at++)
        if (strncasecmp(url + at, ".m3u8", 5) == 0) return true;
    return false;
}

static uint64_t check_sample_end_us(const MediaMp4Sample *sample)
{
    if (sample->timescale == 0) return 0;
    uint64_t end = (sample->pts < 0 ? 0u : (uint64_t) sample->pts)
        + sample->duration;
    return end * UINT64_C(1000000) / sample->timescale;
}

typedef struct {
    MediaSampleSource source;
    bool present;
    bool ended;
} CheckTrack;

typedef void (*CheckPump)(void *opaque);

/* Takes samples from every track until video reaches the target or every
   track has ended. Payloads are read in full; nothing is decoded. */
static bool check_drain(CheckTrack *tracks, size_t track_count,
                        CheckPump pump, void *pump_opaque,
                        unsigned char *payload, uint64_t target_us,
                        MediaReplayCheckReport *report)
{
    uint64_t started_us = tilefinch_platform_monotonic_time_us();
    unsigned idle = 0;
    for (;;) {
        bool progressed = false, open = false;
        for (size_t i = 0; i < track_count; i++) {
            CheckTrack *track = &tracks[i];
            if (!track->present || track->ended) continue;
            const MediaSampleSourceOps *ops = track->source.ops;
            void *opaque = track->source.opaque;
            MediaMp4Sample sample;
            if (!ops->next_sample(opaque, &sample)) {
                if (ops->would_block != NULL && ops->would_block(opaque)) {
                    open = true;
                    continue;
                }
                char detail[192] = {0};
                if (ops->last_error != NULL
                    && ops->last_error(opaque, detail, sizeof(detail))) {
                    check_error(report, "sample stream failed: %s", detail);
                    return false;
                }
                track->ended = true;
                continue;
            }
            open = true;
            if (sample.size > CHECK_SAMPLE_BYTES) {
                check_error(report, "sample of %u bytes exceeds the check "
                            "buffer", (unsigned) sample.size);
                return false;
            }
            bool read = false;
            for (unsigned attempt = 0; attempt < CHECK_IDLE_LIMIT && !read;
                 attempt++) {
                read = ops->read_sample_waiting != NULL
                    ? ops->read_sample_waiting(opaque, &sample, payload,
                                               CHECK_SAMPLE_BYTES)
                    : ops->read_sample(opaque, &sample, payload,
                                       CHECK_SAMPLE_BYTES);
                if (read) break;
                if (ops->would_block == NULL || !ops->would_block(opaque))
                    break;
                pump(pump_opaque);
            }
            if (!read) {
                char detail[192] = {0};
                if (ops->last_error != NULL)
                    (void) ops->last_error(opaque, detail, sizeof(detail));
                check_error(report, "sample payload read failed: %s",
                            detail[0] == '\0' ? "unavailable" : detail);
                return false;
            }
            progressed = true;
            report->payload_bytes += sample.size;
            uint64_t end_us = check_sample_end_us(&sample);
            if (sample.kind == MEDIA_MP4_TRACK_VIDEO) {
                report->video_samples++;
                if (end_us > report->video_until_us)
                    report->video_until_us = end_us;
            } else if (sample.kind == MEDIA_MP4_TRACK_AUDIO) {
                report->audio_samples++;
                if (end_us > report->audio_until_us)
                    report->audio_until_us = end_us;
            }
        }
        if (report->video_until_us >= target_us) return true;
        if (!open) {
            report->ended = true;
            return report->video_samples != 0;
        }
        idle = progressed ? 0 : idle + 1u;
        if (idle > CHECK_IDLE_LIMIT
            || tilefinch_platform_monotonic_time_us() - started_us
                   > CHECK_WALL_LIMIT_US) {
            check_error(report, "stream stalled at %.2f s of video",
                        (double) report->video_until_us / 1e6);
            return false;
        }
        pump(pump_opaque);
    }
}

MediaReplayDecoder media_replay_check_decoder(
    const unsigned char *config, size_t length, bool annexb,
    unsigned *profile_idc, unsigned *level_idc,
    char *refusal, size_t refusal_size)
{
    char scratch[8];
    if (refusal == NULL || refusal_size == 0) {
        refusal = scratch;
        refusal_size = sizeof(scratch);
    }
    refusal[0] = '\0';
    if (profile_idc != NULL) *profile_idc = 0;
    if (level_idc != NULL) *level_idc = 0;
    const unsigned char *sps = NULL, *pps = NULL;
    size_t sps_length = 0, pps_length = 0;
    if (annexb) {
        (void) media_h264_annexb_parameter_sets(
            config, length, &sps, &sps_length, &pps, &pps_length);
    } else if (config != NULL && length >= 8u) {
        sps = config + 8;
        sps_length = ((size_t) config[6] << 8u) | config[7];
        if (sps_length > length - 8u) sps = NULL;
    }
    uint16_t width = 0, height = 0;
    if (sps == NULL || sps_length < 4u
        || !media_h264_sps_dimensions(sps, sps_length, &width, &height)) {
        snprintf(refusal, refusal_size, "%s", "unsupported AVC profile");
        return MEDIA_REPLAY_DECODER_REFUSED;
    }
    unsigned profile = sps[1], level = sps[3];
    if (profile_idc != NULL) *profile_idc = profile;
    if (level_idc != NULL) *level_idc = level;
    uint8_t routed = 0;
    MediaH264DecoderRoute route = annexb
        ? media_h264_annexb_decoder_route(config, length, &routed)
        : media_h264_avcc_decoder_route(config, length, &routed);
    if (route == MEDIA_H264_DECODER_ROUTE_UNSUPPORTED) {
        snprintf(refusal, refusal_size, "%s", "unsupported AVC profile");
        return MEDIA_REPLAY_DECODER_REFUSED;
    }
    /* psp_media_open.c: what sends a page stream to the software decoder
       (High, a conclusive reference refusal, a level above the firmware
       range), then the backend's own admission (media_backend_psp.c). */
    const char *firmware_refusal = NULL;
    if (route == MEDIA_H264_DECODER_ROUTE_PSP_FIRMWARE) {
        MediaH264FirmwareReferences verdict = annexb
            ? media_h264_annexb_firmware_references(config, length)
            : media_h264_avcc_firmware_references(config, length);
        if (media_h264_firmware_references_conclusive(verdict, true))
            firmware_refusal =
                media_h264_firmware_references_refusal(verdict);
        else if (!psp_video_avc_level_supported(level))
            firmware_refusal = MEDIA_H264_FIRMWARE_LEVEL_REFUSAL;
    }
    if (route == MEDIA_H264_DECODER_ROUTE_HIGH_EXTENSION
        || firmware_refusal != NULL) {
        if (psp_swdec_dimensions_supported(width, height))
            return MEDIA_REPLAY_DECODER_SOFTWARE;
        snprintf(refusal, refusal_size, "%s",
                 firmware_refusal != NULL
                     ? firmware_refusal
                     : "software H.264 is limited to 432x240");
        return MEDIA_REPLAY_DECODER_REFUSED;
    }
    PspMediaDecoderPolicy policy;
    if (!media_h264_psp_dimensions_supported(width, height)) {
        snprintf(refusal, refusal_size, "%s",
                 "PSP AVC SPS geometry unsupported");
        return MEDIA_REPLAY_DECODER_REFUSED;
    }
    if (!psp_media_decoder_policy(profile, level, width, height, true,
                                  &policy)) {
        snprintf(refusal, refusal_size,
                 MEDIA_H264_FIRMWARE_LEVEL_REFUSAL ": %u/%u at %ux%u",
                 profile, level, (unsigned) width, (unsigned) height);
        return MEDIA_REPLAY_DECODER_REFUSED;
    }
    return MEDIA_REPLAY_DECODER_FIRMWARE;
}

static void check_report_decoder(MediaReplayCheckReport *report,
                                 const MediaMp4TrackInfo *video,
                                 bool annexb)
{
    report->decoder = media_replay_check_decoder(
        video->codec_config, video->codec_config_length, annexb,
        &report->profile_idc, &report->level_idc,
        report->decoder_refusal, sizeof(report->decoder_refusal));
}

static void check_hls_pump(void *opaque)
{
    psp_media_hls_pump_delivery(opaque,
                                tilefinch_platform_monotonic_time_us());
    fetch_background_transport_wait(1u);
}

/* What playback takes first after a seek: the first video sample, and the
   first audio sample at or after audio_start_us (earlier units are dropped,
   as media_backend.c drops them). Either pointer NULL: not wanted. */
static bool check_seek_first(CheckTrack *track, CheckPump pump,
                             void *pump_opaque, unsigned char *payload,
                             uint64_t audio_start_us, uint64_t *video_us,
                             uint64_t *audio_us, unsigned *audio_dropped)
{
    const MediaSampleSourceOps *ops = track->source.ops;
    void *opaque = track->source.opaque;
    uint64_t started_us = tilefinch_platform_monotonic_time_us();
    for (unsigned read = 0;
         read < 4096u && (video_us != NULL || audio_us != NULL);) {
        MediaMp4Sample sample;
        if (!ops->next_sample(opaque, &sample)) {
            if (!ops->would_block(opaque)
                || tilefinch_platform_monotonic_time_us() - started_us
                       > CHECK_WALL_LIMIT_US) return false;
            pump(pump_opaque);
            continue;
        }
        uint64_t time_us = sample.timescale == 0 ? 0
            : (uint64_t) (sample.pts < 0 ? 0 : sample.pts)
                * UINT64_C(1000000) / sample.timescale;
        if (sample.kind == MEDIA_MP4_TRACK_VIDEO && video_us != NULL) {
            *video_us = time_us;
            video_us = NULL;
        } else if (sample.kind == MEDIA_MP4_TRACK_AUDIO && audio_us != NULL) {
            if (time_us >= audio_start_us) {
                *audio_us = time_us;
                audio_us = NULL;
            } else {
                (*audio_dropped)++;
            }
        }
        if (sample.size > CHECK_SAMPLE_BYTES
            || !ops->read_sample(opaque, &sample, payload,
                                 CHECK_SAMPLE_BYTES)) return false;
        read++;
    }
    return video_us == NULL && audio_us == NULL;
}

/* As media_playback_seek_keyframe (the software decoder's seek): the video
   (or muxed) source picks the segment holding the target and lands on its
   start, a separate audio rendition follows to that landing (its own
   segment holding it, which can start up to a segment earlier: renditions'
   EXTINF differ), and sound resumes with the picture. Video must start at
   the landing and audio within one AAC frame or two after it. */
static void check_hls_seek(CheckTrack *tracks, PspMediaHlsContext *hls,
                           bool has_audio, uint64_t seek_us,
                           unsigned char *payload,
                           MediaReplayCheckReport *report)
{
    report->seek_ran = true;
    uint64_t actual_us = 0;
    MediaSampleSource *video = &tracks[0].source;
    if (!video->ops->seek_us(video->opaque, seek_us, &actual_us)
        || (tracks[1].present
            && !tracks[1].source.ops->seek_us(
                   tracks[1].source.opaque, actual_us, NULL))) {
        check_error(report, "HLS seek to %.2f s refused",
                    (double) seek_us / 1e6);
        return;
    }
    report->seek_actual_us = actual_us;
    bool video_found, audio_found;
    if (tracks[1].present) {
        video_found = check_seek_first(
            &tracks[0], check_hls_pump, hls, payload, actual_us,
            &report->seek_video_us, NULL, &report->seek_audio_dropped);
        audio_found = check_seek_first(
            &tracks[1], check_hls_pump, hls, payload, actual_us, NULL,
            &report->seek_audio_us, &report->seek_audio_dropped);
    } else {
        video_found = audio_found = check_seek_first(
            &tracks[0], check_hls_pump, hls, payload, actual_us,
            &report->seek_video_us,
            has_audio ? &report->seek_audio_us : NULL,
            &report->seek_audio_dropped);
    }
    if (!has_audio) report->seek_audio_us = report->seek_video_us;
    report->seek_ok = video_found && audio_found
        && actual_us <= seek_us
        && report->seek_video_us >= actual_us
        && report->seek_video_us - actual_us < UINT64_C(100000)
        && report->seek_audio_us - actual_us < UINT64_C(100000);
    if (!report->seek_ok)
        check_error(report, "HLS seek to %.2f s landed at %.2f s with "
                    "video %s%.2f s, audio %s%.2f s",
                    (double) seek_us / 1e6, (double) actual_us / 1e6,
                    video_found ? "" : "missing ",
                    (double) report->seek_video_us / 1e6,
                    audio_found ? "" : "missing ",
                    (double) report->seek_audio_us / 1e6);
}

static bool check_hls(Budget *budget, BrowserSession *session,
                      const char *url, const char *document_url,
                      PspMediaHlsSelectionBound bound, uint64_t target_us,
                      uint64_t seek_us, unsigned char *payload,
                      MediaReplayCheckReport *report)
{
    report->hls = true;
    char error[192] = {0};
    PspMediaHlsContext *hls = psp_media_hls_create(
        budget, session, url, document_url, TILEFINCH_REQUEST_MODE_NO_CORS,
        TILEFINCH_CREDENTIALS_INCLUDE, bound, NULL, true,
        error, sizeof(error));
    if (hls == NULL) {
        check_error(report, "HLS create failed: %s", error);
        return false;
    }
    PspMediaHlsOpenStatus status = PSP_MEDIA_HLS_OPEN_PENDING;
    uint64_t started_us = tilefinch_platform_monotonic_time_us();
    while (status == PSP_MEDIA_HLS_OPEN_PENDING
           && tilefinch_platform_monotonic_time_us() - started_us
                  < CHECK_WALL_LIMIT_US) {
        status = psp_media_hls_pump(hls, error, sizeof(error));
        if (status == PSP_MEDIA_HLS_OPEN_PENDING)
            fetch_background_transport_wait(1u);
    }
    bool ok = status == PSP_MEDIA_HLS_OPEN_READY;
    if (!ok) check_error(report, "HLS open failed: %s", error);
    CheckTrack tracks[2] = {{0}};
    bool separate = false, has_audio = false;
    if (ok) {
        report->opened = true;
        MediaMp4TrackInfo video = {0}, audio = {0};
        if (psp_media_hls_stream_info(hls, &video, &audio)) {
            report->width = video.width;
            report->height = video.height;
            has_audio = audio.codec != 0;
            report->duration_us = video.timescale == 0 ? 0
                : video.duration * UINT64_C(1000000) / video.timescale;
            check_report_decoder(report, &video, true);
        }
        ok = psp_media_hls_sample_sources(
            hls, &tracks[0].source, &tracks[1].source, &separate);
        tracks[0].present = ok;
        tracks[1].present = ok && separate;
        if (!ok) check_error(report, "HLS sample sources unavailable");
    }
    if (ok) {
        ok = check_drain(tracks, 2, check_hls_pump, hls, payload, target_us,
                         report);
    }
    if (ok && seek_us != UINT64_MAX) {
        check_hls_seek(tracks, hls, has_audio, seek_us, payload, report);
        ok = report->seek_ok;
    }
    MediaHlsStats stats = {0};
    if (psp_media_hls_stats(hls, &stats))
        report->hls_segments = stats.segments_completed;
    if (psp_media_hls_failed(hls))
        check_error(report, "HLS source failed");
    psp_media_hls_destroy(hls);
    return ok;
}

static void check_range_pump(void *opaque)
{
    (void) media_http_range_pump(opaque);
    fetch_background_transport_wait(1u);
}

static bool check_range(Budget *budget, BrowserSession *session,
                        const char *url, const char *document_url,
                        uint64_t content_length, uint64_t target_us,
                        unsigned char *payload,
                        MediaReplayCheckReport *report)
{
    TilefinchRequestContext context = {
        .target_url = url,
        .initiator_url = document_url,
        .top_level_url = document_url,
        .method = "GET",
        .mode = TILEFINCH_REQUEST_MODE_NO_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_MEDIA
    };
    MediaHttpRangeOptions options = {
        .cache_bytes = 256u * 1024u,
        .lookahead_windows = 2,
        .initial_lookahead_windows = 1,
        .stream_publication_bytes = 16u * 1024u,
        .timeout_ms = 15000,
        .connect_timeout_ms = 3000,
        .referer = document_url,
        .standard_range_header = true,
        .page_request_context = &context
    };
    char error[192] = {0};
    MediaHttpRange *range = media_http_range_create(
        budget, session, url, content_length, &options,
        error, sizeof(error));
    if (range == NULL) {
        check_error(report, "range source failed: %s", error);
        return false;
    }
    MediaRangeReader reader = media_http_range_reader(range);
    MediaMp4Demux *demux = media_mp4_open(budget, &reader, NULL,
                                          error, sizeof(error));
    bool ok = demux != NULL;
    CheckTrack track = {0};
    if (!ok) {
        check_error(report, "MP4 open failed: %s", error);
    } else {
        report->opened = true;
        for (size_t i = 0; i < media_mp4_track_count(demux); i++) {
            MediaMp4TrackInfo info = {0};
            if (media_mp4_track_info(demux, i, &info)
                && info.kind == MEDIA_MP4_TRACK_VIDEO) {
                report->width = info.width;
                report->height = info.height;
                if (info.codec == MEDIA_MP4_FOURCC('a','v','c','1'))
                    check_report_decoder(report, &info, false);
            }
        }
        ok = media_sample_source_from_mp4(demux, &track.source);
        track.present = ok;
        if (!ok) check_error(report, "MP4 sample source unavailable");
    }
    if (ok) {
        ok = check_drain(&track, 1, check_range_pump, range, payload,
                         target_us, report);
    }
    media_mp4_close(demux);
    media_http_range_destroy(range);
    return ok;
}

/* A progressive file is judged before it is recorded in bulk, the way the
   PSP's open judges it: its first CHECK_RECORD_HEAD_BYTES, and when the moov
   is not among them (a large file indexed at its end), the top-level box
   headers and then the moov itself by targeted Range reads. Every read is a
   captured response, so a replay holds what the device reads to reach the
   same verdict. Only a file the PSP would play (or one whose verdict could
   not be taken) is then recorded up to the byte cap; a refused file costs
   its metadata alone. */
#define CHECK_RECORD_HEAD_BYTES (2u * 1024u * 1024u)
/* The moov read starts this far before the box, so a device window aligned
   below it is also answered from the record. */
#define CHECK_RECORD_MOOV_SLACK_BYTES (1024u * 1024u)
#define CHECK_RECORD_MOOV_LIMIT_BYTES (8u * 1024u * 1024u)
#define CHECK_RECORD_BOX_LIMIT 64u

typedef struct {
    uint64_t start;
    size_t length;
    unsigned char *bytes;
} CheckRecordWindow;

typedef struct {
    uint64_t length;
    /* The head of the file, then the moov read. */
    CheckRecordWindow window[2];
} CheckRecordEdges;

static bool check_record_edges_read(void *opaque, uint64_t offset,
                                    void *destination, size_t length)
{
    const CheckRecordEdges *edges = opaque;
    if (length > edges->length || offset > edges->length - length)
        return false;
    unsigned char *out = destination;
    /* The windows may meet or overlap; a read may span them. */
    while (length != 0) {
        const CheckRecordWindow *found = NULL;
        for (size_t i = 0; i < 2 && found == NULL; i++) {
            const CheckRecordWindow *window = &edges->window[i];
            if (window->bytes != NULL && offset >= window->start
                && offset - window->start < window->length)
                found = window;
        }
        if (found == NULL) return false;
        size_t take = (size_t) (found->start + found->length - offset);
        if (take > length) take = length;
        memcpy(out, found->bytes + (offset - found->start), take);
        out += take;
        offset += take;
        length -= take;
    }
    return true;
}

/* A captured Range read of [first, first + length) into DESTINATION. */
static bool check_record_fetch(Budget *budget, const char *url,
                               const char *document_url, uint64_t first,
                               size_t length, unsigned char *destination)
{
    char range_header[96];
    snprintf(range_header, sizeof(range_header), "Range: bytes=%llu-%llu",
             (unsigned long long) first,
             (unsigned long long) (first + length - 1u));
    FetchRequest request = {
        .allow_http_errors = true,
        .accept = "*/*",
        .user_agent = TILEFINCH_BROWSER_USER_AGENT,
        .referer = document_url,
        .sec_fetch_dest = "video",
        .extra_headers = range_header,
        .connect_timeout_ms = 10000
    };
    FetchResult *result = fetch_result_create(budget);
    bool ok = result != NULL && fetch_request_cancelable(
        budget, url, &request, length + 1u, 120000, NULL, NULL, result)
        && result->status_code == 206 && result->length == length;
    if (ok) memcpy(destination, result->data, length);
    fetch_result_free(result);
    return ok;
}

/* Finds the moov by its top-level box headers and reads it into the third
   window. False when the file has none in reach. */
static bool check_record_find_moov(Budget *budget, const char *url,
                                   const char *document_url,
                                   CheckRecordEdges *edges,
                                   MediaReplayCheckReport *report)
{
    uint64_t offset = 0;
    for (unsigned box = 0; box < CHECK_RECORD_BOX_LIMIT
                           && edges->length - offset >= 8u; box++) {
        unsigned char header[16];
        size_t wanted = edges->length - offset >= 16u ? 16u : 8u;
        if (!check_record_edges_read(edges, offset, header, wanted)
            && !check_record_fetch(budget, url, document_url, offset,
                                   wanted, header)) return false;
        report->payload_bytes += wanted;
        uint64_t size = ((uint64_t) header[0] << 24)
            | ((uint64_t) header[1] << 16) | ((uint64_t) header[2] << 8)
            | header[3];
        if (size == 1u && wanted == 16u) {
            size = 0;
            for (unsigned i = 8; i < 16; i++) size = (size << 8) | header[i];
        } else if (size == 0) {
            size = edges->length - offset;
        }
        if (size < 8u || size > edges->length - offset) return false;
        if (memcmp(header + 4, "moov", 4) == 0) {
            uint64_t start = offset > CHECK_RECORD_MOOV_SLACK_BYTES
                ? offset - CHECK_RECORD_MOOV_SLACK_BYTES : 0;
            uint64_t span = offset + size - start;
            if (span > CHECK_RECORD_MOOV_LIMIT_BYTES) return false;
            CheckRecordWindow *window = &edges->window[1];
            window->bytes = budget_malloc(budget, (size_t) span);
            if (window->bytes == NULL
                || !check_record_fetch(budget, url, document_url, start,
                                       (size_t) span, window->bytes)) {
                budget_free(budget, window->bytes);
                window->bytes = NULL;
                return false;
            }
            window->start = start;
            window->length = (size_t) span;
            report->payload_bytes += span;
            return true;
        }
        offset += size;
    }
    return false;
}

static bool check_record_open_verdict(Budget *budget,
                                      CheckRecordEdges *edges,
                                      MediaReplayCheckReport *report)
{
    MediaRangeReader reader = {
        .opaque = edges,
        .length = edges->length,
        .read = check_record_edges_read
    };
    char error[192] = {0};
    MediaMp4Demux *demux = media_mp4_open(budget, &reader, NULL,
                                          error, sizeof(error));
    for (size_t i = 0; demux != NULL && i < media_mp4_track_count(demux);
         i++) {
        MediaMp4TrackInfo info = {0};
        if (media_mp4_track_info(demux, i, &info)
            && info.kind == MEDIA_MP4_TRACK_VIDEO) {
            report->width = info.width;
            report->height = info.height;
            if (info.codec == MEDIA_MP4_FOURCC('a','v','c','1'))
                check_report_decoder(report, &info, false);
            break;
        }
    }
    bool opened = demux != NULL;
    media_mp4_close(demux);
    return opened;
}

static bool check_discard_body(void *opaque, const unsigned char *data,
                               size_t length)
{
    (void) data;
    *(uint64_t *) opaque += length;
    return true;
}

/* Judges a progressive file from its metadata, then records the first
   `bytes` of it as one Range response (streamed past into the capture,
   never retained) unless the PSP would refuse it. */
static bool check_record_range(Budget *budget, const char *url,
                               const char *document_url,
                               uint64_t content_length, uint64_t bytes,
                               MediaReplayCheckReport *report)
{
    uint64_t length = content_length < bytes ? content_length : bytes;
    if (length == 0 || length > SIZE_MAX - 1u) {
        check_error(report, "nothing to record");
        return false;
    }
    CheckRecordEdges edges = {.length = content_length};
    CheckRecordWindow *head = &edges.window[0];
    head->length = content_length < CHECK_RECORD_HEAD_BYTES
        ? (size_t) content_length : CHECK_RECORD_HEAD_BYTES;
    if (head->length > length) head->length = (size_t) length;
    head->bytes = budget_malloc(budget, head->length);
    bool ok = head->bytes != NULL
        && check_record_fetch(budget, url, document_url, 0, head->length,
                              head->bytes);
    if (!ok) {
        check_error(report, "range record failed: the file's first %zu "
                    "bytes", head->length);
        budget_free(budget, head->bytes);
        return false;
    }
    report->opened = true;
    report->payload_bytes = head->length;
    if (!check_record_open_verdict(budget, &edges, report)
        && check_record_find_moov(budget, url, document_url, &edges,
                                  report))
        (void) check_record_open_verdict(budget, &edges, report);
    budget_free(budget, edges.window[1].bytes);
    budget_free(budget, head->bytes);
    if (report->decoder == MEDIA_REPLAY_DECODER_REFUSED
        || length <= CHECK_RECORD_HEAD_BYTES) return true;
    char range_header[96];
    snprintf(range_header, sizeof(range_header), "Range: bytes=0-%llu",
             (unsigned long long) (length - 1u));
    FetchRequest request = {
        .allow_http_errors = true,
        .accept = "*/*",
        .user_agent = TILEFINCH_BROWSER_USER_AGENT,
        .referer = document_url,
        .sec_fetch_dest = "video",
        .extra_headers = range_header,
        .connect_timeout_ms = 10000
    };
    uint64_t received = 0;
    FetchStreamOptions stream = {
        .on_body = check_discard_body,
        .opaque = &received,
        .chunk_bytes = 64u * 1024u
    };
    FetchStreamMetrics metrics;
    FetchResult *result = fetch_result_create(budget);
    bool fetched = result != NULL && fetch_request_stream_cancelable(
        budget, url, &request, (size_t) length + 1u, 600000, NULL, NULL,
        &stream, &metrics, result);
    report->payload_bytes += received;
    if (!fetched || received != length
        || (result->status_code != 206 && result->status_code != 200)) {
        check_error(report, "range record failed (HTTP %ld, %llu bytes): %s",
                    result == NULL ? 0L : result->status_code,
                    (unsigned long long) received,
                    result == NULL ? "budget" : result->error);
        fetched = false;
    }
    fetch_result_free(result);
    return fetched;
}

static bool check_media(Budget *budget, BrowserSession *session,
                        const char *media_url, const char *document_url,
                        PspMediaHlsSelectionBound bound, uint64_t target_us,
                        uint64_t record_range_bytes, uint64_t seek_us,
                        MediaReplayCheckReport *report)
{
    if (report == NULL) return false;
    memset(report, 0, sizeof(*report));
    if (budget == NULL || session == NULL || media_url == NULL
        || document_url == NULL) {
        check_error(report, "invalid media check");
        return false;
    }
    if (!fetch_background_transport_initialize(budget)) {
        check_error(report, "media transport unavailable");
        return false;
    }
    unsigned char *payload = budget_malloc(budget, CHECK_SAMPLE_BYTES);
    if (payload == NULL) {
        check_error(report, "sample buffer exceeds the budget");
        return false;
    }
    bool ok = false;
    if (check_url_is_hls(media_url)) {
        ok = check_hls(budget, session, media_url, document_url, bound,
                       target_us, seek_us, payload, report);
    } else {
        /* The page-media probe: a short Range read that routes the address
           to the range source or to HLS by what it answers. */
        char range_header[64];
        FetchRequest request = {
            .allow_http_errors = true,
            .accept = "*/*",
            .user_agent = TILEFINCH_BROWSER_USER_AGENT,
            .referer = document_url,
            .sec_fetch_dest = "video",
            .connect_timeout_ms = 3000
        };
        TilefinchRequestContext context = {
            .target_url = media_url,
            .initiator_url = document_url,
            .top_level_url = document_url,
            .method = "GET",
            .mode = TILEFINCH_REQUEST_MODE_NO_CORS,
            .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
            .destination = TILEFINCH_DESTINATION_MEDIA
        };
        FetchResult *result = fetch_result_create(budget);
        bool fetched = result != NULL
            && media_http_build_probe_range_header(range_header,
                                                   sizeof(range_header));
        request.extra_headers = range_header;
        /* As the PSP probes when its worker transport is available (the
           replay lane provides it): a private scheduler, pumped here. */
        if (fetched && fetch_background_transport_available()) {
            FetchScheduler *probe = NULL;
            uint64_t id = media_http_probe_enqueue_background(
                budget, media_url, &request, 15000, &probe);
            /* Wait on the request itself, bounded by its timeout: a count of
               idle pumps expires in milliseconds while a recorded (live)
               probe is still in flight. */
            fetched = id != 0 && fetch_scheduler_wait(probe, id, result);
            if (id == 0) check_error(report, "probe transport unavailable");
            else if (!fetched && result->error[0] == '\0')
                check_error(report, "probe failed: %s",
                            fetch_scheduler_last_error(probe));
            fetch_scheduler_destroy(probe);
        } else {
            fetched = fetched && fetch_request_cancelable(
                budget, media_url, &request, CHECK_PROBE_BYTES, 15000,
                NULL, NULL, result);
        }
        uint64_t length = 0;
        MediaHttpProbeRoute route = result == NULL
            ? MEDIA_HTTP_PROBE_REFUSED
            : media_http_probe_route(session, &context, true, result,
                                     fetched, &length);
        if (route == MEDIA_HTTP_PROBE_HLS) {
            ok = check_hls(budget, session, media_url, document_url, bound,
                           target_us, seek_us, payload, report);
        } else if (route == MEDIA_HTTP_PROBE_RANGE) {
            report->content_length = length;
            ok = record_range_bytes != 0
                ? check_record_range(budget, media_url, document_url,
                                     length, record_range_bytes, report)
                : check_range(budget, session, media_url, document_url,
                              length, target_us, payload, report);
        } else {
            check_error(report, "probe refused (HTTP %ld): %s",
                        result == NULL ? 0L : result->status_code,
                        result == NULL ? "budget" : result->error);
        }
        fetch_result_free(result);
    }
    budget_free(budget, payload);
    return ok;
}

bool media_replay_check(Budget *budget, BrowserSession *session,
                        const char *media_url, const char *document_url,
                        PspMediaHlsSelectionBound bound, uint64_t target_us,
                        uint64_t record_range_bytes,
                        MediaReplayCheckReport *report)
{
    return check_media(budget, session, media_url, document_url, bound,
                       target_us, record_range_bytes, UINT64_MAX, report);
}

bool media_replay_check_seek(Budget *budget, BrowserSession *session,
                             const char *media_url, const char *document_url,
                             PspMediaHlsSelectionBound bound,
                             uint64_t target_us, uint64_t seek_us,
                             MediaReplayCheckReport *report)
{
    bool ok = check_media(budget, session, media_url, document_url, bound,
                          target_us, 0, seek_us, report);
    if (ok && report != NULL && !report->seek_ran) {
        check_error(report, "seek is checked for HLS only");
        ok = false;
    }
    return ok;
}
