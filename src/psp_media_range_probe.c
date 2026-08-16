#include "tilefinch/psp_media_range_probe.h"

#ifdef __PSP__

#include <pspkernel.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "tilefinch/browser_profile.h"
#include "tilefinch/media_http.h"
#include "tilefinch/media_mp4.h"
#include "tilefinch/psp_log.h"
#include "tilefinch/youtube_resolver.h"

/* The device logging redirect, after every other header exactly as
   src/psp_media_session.c and src/psp_app/psp_app_internal.h establish it.
   Plain printf reaches the console and nothing collects it; the per-source
   counters this probe exists to publish have to reach the validation log. */
#define printf psp_log_printf

/* The shipping window and connect bound, so the probe measures the transport
   the browser actually uses rather than a friendlier one. */
#define PSP_MEDIA_RANGE_PROBE_WINDOW_BYTES (256u * 1024u)
#define PSP_MEDIA_RANGE_PROBE_CONNECT_MS 3000L
#define PSP_MEDIA_RANGE_PROBE_TIMEOUT_MS 15000L
/* A 240p keyframe is a few tens of kilobytes; this is the ceiling the probe
   admits before it refuses to read a sample rather than silently skipping it. */
#define PSP_MEDIA_RANGE_PROBE_SAMPLE_BYTES (256u * 1024u)
/* Two fragment boundaries per source: the first proves a sidx window can be
   advanced at all, the second proves the reader kept going afterwards. */
#define PSP_MEDIA_RANGE_PROBE_BOUNDARIES 2u
/* And two transport windows, which is the other half of the same run: the
   first was fetched to open the stream, so a second one means the sequential
   reader outran its window and the refill delivered -- the path a playing
   pipeline spends its whole life in, and the one this probe exists to reach
   without a decoder. Fragment boundaries alone can all land inside the
   opening window on a low-bitrate rendition. */
#define PSP_MEDIA_RANGE_PROBE_WINDOWS 2u
#define PSP_MEDIA_RANGE_PROBE_DEADLINE_US UINT64_C(60000000)
/* A stream whose fragments are enormous, or which is not fragmented at all,
   must still end the probe rather than run to the deadline. */
#define PSP_MEDIA_RANGE_PROBE_SAMPLE_LIMIT 2000u

typedef struct {
    const char *name;
    MediaHttpRange *range;
    MediaMp4Demux *demux;
    MediaMp4Sample pending;
    bool have_pending;
    bool contiguous_known;
    uint64_t contiguous_offset;
    bool finished;
    PspMediaRangeSourceReport *report;
} PspMediaRangeProbeSource;

static void range_probe_error(
    char *error, size_t error_size, const char *format, ...)
{
    if (error == NULL || error_size == 0) return;
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(error, error_size, format, arguments);
    va_end(arguments);
}

static void range_probe_collect(PspMediaRangeProbeSource *source)
{
    MediaHttpRangeStats stats = {0};
    if (source->range == NULL
        || !media_http_range_stats(source->range, &stats)) return;
    source->report->requests = stats.requests;
    source->report->window_installs = stats.window_installs;
    source->report->bytes_received = stats.bytes_received;
    source->report->failures = stats.failures;
    source->report->source_blocks = stats.would_block_reads;
}

static bool range_probe_open(
    Budget *budget, BrowserSession *session, const char *referer,
    const char *url, uint64_t content_length,
    PspMediaRangeProbeSource *source, char *error, size_t error_size)
{
    MediaHttpRangeOptions options = {
        .cache_bytes = PSP_MEDIA_RANGE_PROBE_WINDOW_BYTES,
        .timeout_ms = PSP_MEDIA_RANGE_PROBE_TIMEOUT_MS,
        .connect_timeout_ms = PSP_MEDIA_RANGE_PROBE_CONNECT_MS,
        .referer = referer,
        .url_validator = youtube_media_url_supported
    };
    source->range = media_http_range_create(
        budget, session, url, content_length, &options, error, error_size);
    if (source->range == NULL) return false;
    MediaRangeReader reader = media_http_range_reader(source->range);
    /* This is the moov, and for an indexed fragmented stream the sidx and its
       first window too: the one part of a stream that is read with the
       blocking form, inside a phase that owns its own deadline. */
    source->demux = media_mp4_open(
        budget, &reader, NULL, error, error_size);
    range_probe_collect(source);
    source->report->opened = source->demux != NULL;
    return source->demux != NULL;
}

/*
 * One bounded step for one source: take the next sample if none is pending,
 * then read its payload. Neither call is allowed to wait -- both answer
 * would-block and the caller comes back after a pump, which is exactly the
 * contract a playing pipeline reads samples under.
 */
static bool range_probe_step(
    PspMediaRangeProbeSource *source, unsigned char *payload,
    size_t capacity, char *error, size_t error_size)
{
    if (source->finished) return true;
    if (!source->have_pending) {
        if (!media_mp4_next_sample(source->demux, &source->pending)) {
            if (media_mp4_would_block(source->demux)) return true;
            char detail[192] = {0};
            if (media_mp4_last_error(source->demux, detail, sizeof(detail))) {
                range_probe_error(
                    error, error_size, "%s sample stream failed: %.160s",
                    source->name, detail);
                return false;
            }
            /* Clean end of stream before the boundary target: report it as
               the outcome rather than pretending the target was met. */
            source->finished = true;
            return true;
        }
        source->have_pending = true;
    }
    if (source->pending.size > capacity) {
        range_probe_error(
            error, error_size, "%s sample %uB exceeds the probe's %zuB bound",
            source->name, (unsigned) source->pending.size, capacity);
        return false;
    }
    if (!media_mp4_read_sample(
            source->demux, &source->pending, payload, capacity)) {
        if (media_mp4_would_block(source->demux)) return true;
        char detail[192] = {0};
        (void) media_mp4_last_error(source->demux, detail, sizeof(detail));
        range_probe_error(
            error, error_size, "%s sample read failed: %.160s",
            source->name, detail);
        return false;
    }
    if (source->contiguous_known
        && source->pending.offset != source->contiguous_offset)
        source->report->fragment_boundaries++;
    source->contiguous_offset = source->pending.offset + source->pending.size;
    source->contiguous_known = true;
    source->report->samples++;
    source->have_pending = false;
    range_probe_collect(source);
    if ((source->report->fragment_boundaries
             >= PSP_MEDIA_RANGE_PROBE_BOUNDARIES
         && source->report->window_installs
             >= PSP_MEDIA_RANGE_PROBE_WINDOWS)
        || source->report->samples >= PSP_MEDIA_RANGE_PROBE_SAMPLE_LIMIT)
        source->finished = true;
    return true;
}

static void range_probe_close(
    Budget *budget, PspMediaRangeProbeSource *source)
{
    range_probe_collect(source);
    if (source->demux != NULL) media_mp4_close(source->demux);
    if (source->range != NULL) media_http_range_destroy(source->range);
    source->demux = NULL;
    source->range = NULL;
    (void) budget;
}

bool psp_media_range_probe_run(
    Budget *budget, BrowserSession *session, const char *watch_url,
    PspMediaRangeProbeReport *report, char *error, size_t error_size)
{
    if (error != NULL && error_size != 0) error[0] = '\0';
    if (budget == NULL || session == NULL || watch_url == NULL
        || report == NULL) {
        range_probe_error(error, error_size, "range probe request invalid");
        return false;
    }
    memset(report, 0, sizeof(*report));
    printf(
        "tilefinch-media-range-probe: event=begin url=%.180s window=%uB "
        "boundary-target=%u window-target=%u free=%u\n",
        watch_url, PSP_MEDIA_RANGE_PROBE_WINDOW_BYTES,
        PSP_MEDIA_RANGE_PROBE_BOUNDARIES, PSP_MEDIA_RANGE_PROBE_WINDOWS,
        sceKernelTotalFreeMemSize());

    YoutubeStream stream = {0};
    if (!youtube_resolve_progressive_mp4_cancelable(
            budget, session, watch_url,
            (int) BROWSER_YOUTUBE_QUALITY_240P, 30000, NULL, NULL,
            &stream, error, error_size)) {
        printf("tilefinch-media-range-probe: outcome=unresolved "
               "error=\"%.180s\"\n", error == NULL ? "" : error);
        return false;
    }
    report->resolved = true;
    report->split = stream.split_streams;
    report->itag = stream.itag;
    report->audio_itag = stream.audio_itag;
    printf(
        "tilefinch-media-range-probe: resolved itag=%d audio-itag=%d "
        "split=%d source=%dx%d video=%lluB audio=%lluB\n",
        stream.itag, stream.audio_itag, stream.split_streams ? 1 : 0,
        stream.width, stream.height,
        (unsigned long long) stream.content_length,
        (unsigned long long) stream.audio_content_length);

    unsigned char *payload = budget_malloc_category(
        budget, BUDGET_CATEGORY_NAVIGATION,
        PSP_MEDIA_RANGE_PROBE_SAMPLE_BYTES);
    if (payload == NULL) {
        range_probe_error(
            error, error_size, "range probe payload buffer unavailable");
        printf("tilefinch-media-range-probe: outcome=incomplete "
               "error=\"%.180s\"\n", error == NULL ? "" : error);
        return false;
    }

    PspMediaRangeProbeSource sources[2] = {
        {.name = "video", .report = &report->video},
        {.name = "audio", .report = &report->audio}
    };
    size_t source_count = stream.split_streams ? 2u : 1u;
    bool ok = range_probe_open(
        budget, session, watch_url, stream.media_url,
        stream.content_length, &sources[0], error, error_size);
    if (ok && stream.split_streams) {
        ok = range_probe_open(
            budget, session, watch_url, stream.audio_url,
            stream.audio_content_length, &sources[1], error, error_size);
    }
    if (!stream.split_streams) sources[1].finished = true;

    uint64_t started = (uint64_t) sceKernelGetSystemTimeWide();
    while (ok) {
        bool all_finished = true;
        for (size_t at = 0; at < source_count; at++) {
            if (!range_probe_step(
                    &sources[at], payload,
                    PSP_MEDIA_RANGE_PROBE_SAMPLE_BYTES,
                    error, error_size)) {
                ok = false;
                break;
            }
            if (!sources[at].finished) all_finished = false;
        }
        if (!ok || all_finished) break;
        /* The same once-per-frame pump a playing session gives the refill, so
           a window that has not landed makes progress between steps instead of
           being waited on. */
        for (size_t at = 0; at < source_count; at++) {
            if (sources[at].range != NULL)
                (void) media_http_range_pump(sources[at].range);
        }
        uint64_t now = (uint64_t) sceKernelGetSystemTimeWide();
        if (now >= started
            && now - started > PSP_MEDIA_RANGE_PROBE_DEADLINE_US) {
            range_probe_error(
                error, error_size,
                "range probe deadline reached video=%u/%zu audio=%u/%zu "
                "(boundaries/windows)",
                report->video.fragment_boundaries,
                report->video.window_installs,
                report->audio.fragment_boundaries,
                report->audio.window_installs);
            ok = false;
            break;
        }
        sceKernelDelayThread(1000);
    }

    for (size_t at = 0; at < source_count; at++)
        range_probe_close(budget, &sources[at]);
    budget_free(budget, payload);

    for (size_t at = 0; at < source_count; at++) {
        const PspMediaRangeSourceReport *source = sources[at].report;
        printf(
            "tilefinch-media-range-probe: source=%s opened=%d requests=%zu "
            "window-installs=%zu bytes-received=%zu failures=%zu "
            "source-block=%zu samples=%u boundaries=%u\n",
            sources[at].name, source->opened ? 1 : 0, source->requests,
            source->window_installs, source->bytes_received,
            source->failures, source->source_blocks, source->samples,
            source->fragment_boundaries);
    }
    /* Both sources must have reached the target for the run to mean what it
       claims: an audio window that never advanced is exactly the asymmetry a
       transport wedge shows first. */
    bool crossed = report->video.fragment_boundaries
            >= PSP_MEDIA_RANGE_PROBE_BOUNDARIES
        && report->video.window_installs >= PSP_MEDIA_RANGE_PROBE_WINDOWS
        && (!stream.split_streams
            || (report->audio.fragment_boundaries
                    >= PSP_MEDIA_RANGE_PROBE_BOUNDARIES
                && report->audio.window_installs
                    >= PSP_MEDIA_RANGE_PROBE_WINDOWS));
    bool passed = ok && crossed
        && report->video.failures == 0 && report->audio.failures == 0;
    printf(
        "tilefinch-media-range-probe: outcome=%s crossed=%d split=%d "
        "error=\"%.180s\" free=%u\n",
        passed ? "complete" : "incomplete", crossed ? 1 : 0,
        stream.split_streams ? 1 : 0, error == NULL ? "" : error,
        sceKernelTotalFreeMemSize());
    return passed;
}

#endif
