#include "tilefinch/psp_media_fixture.h"

#ifdef __PSP__

#include <pspkernel.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "tilefinch/media_backend.h"
#include "tilefinch/media_mp4.h"
#include "tilefinch/psp_log.h"
#include "tilefinch/psp_media_scale.h"

/* The device logging redirect, after every other header, exactly as
   src/psp_media_session.c and src/psp_app/psp_app_internal.h establish it.
   Without it this file's per-clip and per-candidate lines reach the console
   and nothing collects them: they never appear in the validation log the
   runner and every device report actually read. */
#define printf psp_log_printf

extern const unsigned char tilefinch_media_fixture_240_start[];
extern const unsigned char tilefinch_media_fixture_240_end[];
extern const unsigned char tilefinch_media_fixture_360_start[];
extern const unsigned char tilefinch_media_fixture_360_end[];

typedef struct {
    const unsigned char *bytes;
    size_t length;
} FixtureReader;

static void fixture_error(
    char *error, size_t error_size, const char *format, ...)
{
    if (error == NULL || error_size == 0) return;
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(error, error_size, format, arguments);
    va_end(arguments);
}

static bool fixture_read(
    void *opaque, uint64_t offset, void *destination, size_t length)
{
    const FixtureReader *reader = opaque;
    if (reader == NULL || destination == NULL
        || offset > reader->length
        || length > reader->length - (size_t) offset) return false;
    memcpy(destination, reader->bytes + (size_t) offset, length);
    return true;
}

static uint64_t fixture_frame_signature(
    const MediaVideoFrame *frame, unsigned *variation)
{
    if (variation != NULL) *variation = 0;
    if (frame == NULL || frame->pixels == NULL
        || frame->format != MEDIA_PIXEL_RGBA8888
        || frame->width <= 0 || frame->height <= 0
        || frame->stride_pixels < frame->width) return 0;
    const uint32_t *pixels = frame->pixels;
    uint64_t hash = UINT64_C(1469598103934665603);
    uint32_t first = 0;
    bool have_first = false;
    unsigned different = 0;
    /* A 16x12 grid is enough to reject an untouched/canary surface while
       keeping this diagnostic negligible beside firmware decode. Hash the
       actual little-endian ABGR words; the expected property is stable,
       nonuniform, changing output rather than a host-decoder pixel identity. */
    for (unsigned gy = 0; gy < 12u; gy++) {
        int y = (int) (((uint64_t) gy * (unsigned) (frame->height - 1)) / 11u);
        const uint32_t *row = pixels + (size_t) y * frame->stride_pixels;
        for (unsigned gx = 0; gx < 16u; gx++) {
            int x = (int) (((uint64_t) gx * (unsigned) (frame->width - 1)) / 15u);
            uint32_t pixel = row[x];
            if (!have_first) {
                first = pixel;
                have_first = true;
            } else if (pixel != first) {
                different++;
            }
            hash ^= pixel;
            hash *= UINT64_C(1099511628211);
        }
    }
    if (variation != NULL) *variation = different;
    return hash;
}

static bool fixture_prepare_modules(char *error, size_t error_size)
{
    uint64_t started = (uint64_t) sceKernelGetSystemTimeWide();
    for (unsigned pumps = 0; pumps < 10000u; pumps++) {
        MediaPspPrepareResult result = media_psp_backend_prepare_pump(
            error, error_size);
        if (result == MEDIA_PSP_PREPARE_READY) return true;
        if (result == MEDIA_PSP_PREPARE_ERROR) return false;
        uint64_t now = (uint64_t) sceKernelGetSystemTimeWide();
        if (now - started >= UINT64_C(10000000)) break;
        sceKernelDelayThread(1000);
    }
    fixture_error(error, error_size, "firmware module preparation timed out");
    return false;
}

static bool fixture_run_clip(
    Budget *budget, const char *name, const unsigned char *bytes,
    size_t length, unsigned expected_width, unsigned expected_height,
    bool exercise_seek, PspMediaFixtureReport *report,
    char *error, size_t error_size)
{
    FixtureReader source = {.bytes = bytes, .length = length};
    MediaRangeReader reader = {
        .opaque = &source,
        .length = length,
        .read = fixture_read,
        .describe_failure = NULL
    };
    MediaMp4Demux *demux = media_mp4_open(
        budget, &reader, NULL, error, error_size);
    if (demux == NULL) return false;

    bool have_video = false, have_audio = false;
    for (size_t track = 0; track < media_mp4_track_count(demux); track++) {
        MediaMp4TrackInfo info;
        if (!media_mp4_track_info(demux, track, &info)) continue;
        if (info.kind == MEDIA_MP4_TRACK_VIDEO
            && info.codec == MEDIA_MP4_FOURCC('a','v','c','1')
            && info.width == expected_width
            && info.height == expected_height) have_video = true;
        if (info.kind == MEDIA_MP4_TRACK_AUDIO
            && info.codec == MEDIA_MP4_FOURCC('m','p','4','a')
            && info.sample_rate == 48000u
            && media_aac_esds_is_low_complexity(
                   info.codec_config, info.codec_config_length))
            have_audio = true;
    }
    if (!have_video || !have_audio) {
        fixture_error(
            error, error_size, "%s fixture inventory invalid video=%d audio=%d",
            name, have_video ? 1 : 0, have_audio ? 1 : 0);
        media_mp4_close(demux);
        return false;
    }

    MediaBackend backend = {0};
    if (!media_psp_backend_create(
            budget, demux, &backend, error, error_size)) {
        media_mp4_close(demux);
        return false;
    }
    MediaPlaybackOptions options = {
        .decode_lead_us = 100000u,
        .maximum_packet_bytes = 1u * 1024u * 1024u
    };
    MediaPlayback *playback = media_playback_create(
        budget, demux, &backend, &options, error, error_size);
    if (playback == NULL) {
        backend.destroy(backend.opaque);
        media_mp4_close(demux);
        return false;
    }

    uint64_t started = (uint64_t) sceKernelGetSystemTimeWide();
    uint64_t previous_signature = 0;
    unsigned observed = 0, distinct = 0;
    bool sought = !exercise_seek;
    bool failed = false;
    while (!media_playback_ended(playback)) {
        uint64_t now = (uint64_t) sceKernelGetSystemTimeWide();
        uint64_t elapsed = now >= started ? now - started : 0;
        if (elapsed > UINT64_C(8000000)) {
            fixture_error(error, error_size, "%s decode timed out", name);
            failed = true;
            break;
        }
        /* This loop is the Media Engine's only driver here, so it owns the
           refusal hold too: nothing else would ever lift it and the probe
           would stall out on its own timeout instead of reporting. */
        media_psp_backend_release_refusal_hold();
        MediaPlaybackAdvanceResult advanced =
            media_playback_advance_bounded(
                playback, elapsed, 4u, error, error_size);
        if (advanced == MEDIA_PLAYBACK_ADVANCE_ERROR) {
            failed = true;
            break;
        }
        MediaVideoFrame frame = {0};
        if (media_playback_take_video_frame(playback, &frame)) {
            unsigned variation = 0;
            uint64_t signature = fixture_frame_signature(&frame, &variation);
            if (frame.width != (int) expected_width
                || frame.height != (int) expected_height
                || signature == 0 || variation < 24u) {
                fixture_error(
                    error, error_size,
                    "%s decoded frame invalid geometry=%dx%d variation=%u",
                    name, frame.width, frame.height, variation);
                failed = true;
                break;
            }
            observed++;
            if (previous_signature == 0 || signature != previous_signature)
                distinct++;
            previous_signature = signature;
        }
        if (!sought && observed >= 5u) {
            uint64_t actual_us = 0;
            if (!media_playback_seek(
                    playback, UINT64_C(1000000), &actual_us,
                    error, error_size)) {
                failed = true;
                break;
            }
            sought = true;
            report->seek_completions++;
            started = (uint64_t) sceKernelGetSystemTimeWide() - actual_us;
        }
        sceKernelDelayThread(1000);
    }

    MediaBackendStats stats = {0};
    bool have_stats = media_playback_backend_stats(playback, &stats);
    report->last_native_error = have_stats ? stats.last_native_error : 0;
    report->frames_decoded +=
        have_stats ? (unsigned) stats.decoded_video_frames : 0u;
    report->frames_observed += observed;
    report->distinct_frame_signatures += distinct;
    printf(
        "tilefinch-media-fixture: clip=%s bytes=%zu observed=%u "
        "distinct=%u decoded=%zu audio-samples=%llu native=0x%08X "
        "result=%s\n",
        name, length, observed, distinct,
        have_stats ? stats.decoded_video_frames : 0u,
        (unsigned long long) (
            have_stats ? stats.decoded_audio_samples : 0u),
        (unsigned) report->last_native_error,
        failed ? "failed" : "complete");
    if (!failed && (observed < 8u || distinct < 4u
                    || !have_stats || stats.decoded_video_frames < 8u
                    || stats.decoded_audio_samples == 0u)) {
        fixture_error(
            error, error_size,
            "%s insufficient output frames=%u distinct=%u audio=%llu",
            name, observed, distinct,
            (unsigned long long) stats.decoded_audio_samples);
        failed = true;
    }
    media_playback_destroy(playback);
    media_mp4_close(demux);
    if (!failed) report->clips_completed++;
    return !failed;
}

#define PSP_MEDIA_CSC_PROBE_HEAD_PIXELS 8u

/*
 * The 16-bit value the graphics engine's texture unit produces for one surface
 * word. This is a measurement, not a convention: the present probe drew three
 * source pixels with one saturated byte each and read back byte0=0x001f,
 * byte1=0x07e0, byte2=0xf800. Surface byte k reaches
 * destination field k counting from the least significant bit, as every PSP
 * pixel format does.
 */
static uint16_t fixture_engine_565(uint32_t word)
{
    unsigned byte0 = word & 0xffu;
    unsigned byte1 = (word >> 8u) & 0xffu;
    unsigned byte2 = (word >> 16u) & 0xffu;
    return (uint16_t) (((byte2 >> 3u) << 11u)
                       | ((byte1 >> 2u) << 5u)
                       | (byte0 >> 3u));
}

static void fixture_format_words(
    const uint32_t *words, unsigned count, char *output, size_t output_size)
{
    size_t at = 0;
    if (output == NULL || output_size == 0) return;
    output[0] = '\0';
    for (unsigned i = 0; i < count && at < output_size; i++) {
        int written = snprintf(
            output + at, output_size - at, i == 0 ? "%08x" : ",%08x",
            (unsigned) words[i]);
        if (written <= 0 || (size_t) written >= output_size - at) return;
        at += (size_t) written;
    }
}

static void fixture_format_halfwords(
    const uint16_t *words, unsigned count, char *output, size_t output_size)
{
    size_t at = 0;
    if (output == NULL || output_size == 0) return;
    output[0] = '\0';
    for (unsigned i = 0; i < count && at < output_size; i++) {
        int written = snprintf(
            output + at, output_size - at, i == 0 ? "%04x" : ",%04x",
            (unsigned) words[i]);
        if (written <= 0 || (size_t) written >= output_size - at) return;
        at += (size_t) written;
    }
}

/*
 * Sweep the two undocumented mode words of sceMpegBaseCscAvc over one already
 * decoded picture.
 *
 * The candidates are probes of an enumeration nothing here can read. PMPlayer,
 * the mature raw-MP4 player this backend follows, passes 0/0 and never
 * anything else, so no candidate above the baseline can come from it. They are
 * therefore kept to bounded single digits -- the range in which an
 * enumeration's other members live if it has any -- one word at a time before
 * both together, and every candidate's result code is checked before its
 * output is read. A refused candidate writes nothing; the surface is
 * pool-owned and rewritten in place, so the sweep allocates nothing and can
 * never outlive the picture it repeats.
 */
static const struct {
    int mode0;
    int mode1;
} psp_media_csc_candidates[] = {
    {0, 0}, {1, 0}, {0, 1}, {1, 1}, {2, 0}, {0, 2}, {3, 0}, {0, 3}
};

bool psp_media_fixture_csc_order_probe(
    Budget *budget, PspMediaCscOrderReport *report,
    char *error, size_t error_size)
{
    if (error != NULL && error_size != 0) error[0] = '\0';
    if (budget == NULL || report == NULL) {
        fixture_error(error, error_size, "CSC order probe request invalid");
        return false;
    }
    memset(report, 0, sizeof(*report));
    size_t bytes_240 = (size_t) (
        tilefinch_media_fixture_240_end - tilefinch_media_fixture_240_start);
    printf(
        "tilefinch-media-csc-probe: event=begin fixture-240=%zu "
        "candidates=%zu free=%u largest=%u\n",
        bytes_240,
        sizeof(psp_media_csc_candidates)
            / sizeof(psp_media_csc_candidates[0]),
        sceKernelTotalFreeMemSize(), sceKernelMaxFreeMemSize());
    if (!fixture_prepare_modules(error, error_size)) return false;

    FixtureReader source = {
        .bytes = tilefinch_media_fixture_240_start, .length = bytes_240
    };
    MediaRangeReader reader = {
        .opaque = &source,
        .length = bytes_240,
        .read = fixture_read,
        .describe_failure = NULL
    };
    MediaMp4Demux *demux = media_mp4_open(
        budget, &reader, NULL, error, error_size);
    if (demux == NULL) return false;
    MediaBackend backend = {0};
    if (!media_psp_backend_create(
            budget, demux, &backend, error, error_size)) {
        media_mp4_close(demux);
        return false;
    }
    MediaPlaybackOptions options = {
        .decode_lead_us = 100000u,
        .maximum_packet_bytes = 1u * 1024u * 1024u
    };
    MediaPlayback *playback = media_playback_create(
        budget, demux, &backend, &options, error, error_size);
    if (playback == NULL) {
        backend.destroy(backend.opaque);
        media_mp4_close(demux);
        return false;
    }

    /* One picture, then stop: the sweep repeats the conversion of exactly the
       picture the decoder last produced, and another decode would replace the
       YUV planes underneath it. */
    uint32_t baseline[PSP_MEDIA_CSC_PROBE_HEAD_PIXELS] = {0};
    uint64_t started = (uint64_t) sceKernelGetSystemTimeWide();
    bool failed = false;
    while (!report->decoded_picture && !media_playback_ended(playback)) {
        uint64_t now = (uint64_t) sceKernelGetSystemTimeWide();
        uint64_t elapsed = now >= started ? now - started : 0;
        if (elapsed > UINT64_C(8000000)) {
            fixture_error(error, error_size, "CSC probe decode timed out");
            failed = true;
            break;
        }
        /* This loop is the Media Engine's only driver here, so it owns the
           refusal hold too: nothing else would ever lift it and the probe
           would stall out on its own timeout instead of reporting. */
        media_psp_backend_release_refusal_hold();
        if (media_playback_advance_bounded(
                playback, elapsed, 4u, error, error_size)
            == MEDIA_PLAYBACK_ADVANCE_ERROR) {
            failed = true;
            break;
        }
        MediaVideoFrame frame = {0};
        if (media_playback_take_video_frame(playback, &frame)
            && frame.pixels != NULL
            && frame.width >= (int) PSP_MEDIA_CSC_PROBE_HEAD_PIXELS) {
            const uint32_t *pixels = frame.pixels;
            for (unsigned at = 0;
                 at < PSP_MEDIA_CSC_PROBE_HEAD_PIXELS; at++)
                baseline[at] = pixels[at];
            report->decoded_picture = true;
            break;
        }
        sceKernelDelayThread(1000);
    }
    MediaBackendStats stats = {0};
    if (media_playback_backend_stats(playback, &stats))
        report->last_native_error = stats.last_native_error;
    if (!report->decoded_picture && !failed) {
        fixture_error(
            error, error_size, "CSC probe produced no decoded picture");
        failed = true;
    }

    if (!failed) {
        /* What the panel is proven to want for this picture, from the
           composition the software scaler ships. Printed once, because it is
           the reference every candidate below is measured against. */
        uint16_t scaler[PSP_MEDIA_CSC_PROBE_HEAD_PIXELS] = {0};
        char text[PSP_MEDIA_CSC_PROBE_HEAD_PIXELS * 10u + 1u];
        for (unsigned at = 0; at < PSP_MEDIA_CSC_PROBE_HEAD_PIXELS; at++)
            scaler[at] = psp_media_scale_convert_pixel(baseline[at]);
        fixture_format_halfwords(
            scaler, PSP_MEDIA_CSC_PROBE_HEAD_PIXELS, text, sizeof(text));
        printf("tilefinch-media-csc-probe: scaler-head=%s\n", text);
        for (size_t at = 0;
             at < sizeof(psp_media_csc_candidates)
                  / sizeof(psp_media_csc_candidates[0]); at++) {
            int mode0 = psp_media_csc_candidates[at].mode0;
            int mode1 = psp_media_csc_candidates[at].mode1;
            uint32_t head[PSP_MEDIA_CSC_PROBE_HEAD_PIXELS] = {0};
            report->candidates_tried++;
            int status = media_psp_backend_csc_order_probe(
                backend.opaque, mode0, mode1,
                head, PSP_MEDIA_CSC_PROBE_HEAD_PIXELS);
            if (status < 0) {
                report->candidates_refused++;
                report->last_native_error = status;
                printf(
                    "tilefinch-media-csc-probe: mode0=%d mode1=%d "
                    "status=0x%08X\n",
                    mode0, mode1, (unsigned) status);
                /* A refused candidate is the expected answer for most of this
                   list and says nothing about the ones after it. A quarantine
                   is different: the firmware worker or audio channel is gone
                   for the rest of the process, so stop asking. */
                if (media_psp_backend_quarantined()) {
                    report->quarantined = true;
                    break;
                }
                continue;
            }
            bool match = true;
            for (unsigned i = 0; i < PSP_MEDIA_CSC_PROBE_HEAD_PIXELS; i++) {
                if (fixture_engine_565(head[i]) != scaler[i]) match = false;
            }
            if (match) report->candidates_usable++;
            fixture_format_words(
                head, PSP_MEDIA_CSC_PROBE_HEAD_PIXELS, text, sizeof(text));
            printf(
                "tilefinch-media-csc-probe: mode0=%d mode1=%d status=0x0 "
                "head=%s match=%s\n",
                mode0, mode1, text, match ? "yes" : "no");
        }
    }

    media_playback_destroy(playback);
    media_mp4_close(demux);
    printf(
        "tilefinch-media-csc-probe: outcome=%s candidates=%u usable=%u "
        "refused=%u quarantine=%d native=0x%08X free=%u largest=%u\n",
        failed ? "incomplete" : "complete",
        report->candidates_tried, report->candidates_usable,
        report->candidates_refused, report->quarantined ? 1 : 0,
        (unsigned) report->last_native_error,
        sceKernelTotalFreeMemSize(), sceKernelMaxFreeMemSize());
    return !failed;
}

bool psp_media_fixture_run(
    Budget *budget, PspMediaFixtureReport *report,
    char *error, size_t error_size)
{
    if (error != NULL && error_size != 0) error[0] = '\0';
    if (budget == NULL || report == NULL) {
        fixture_error(error, error_size, "media fixture request invalid");
        return false;
    }
    memset(report, 0, sizeof(*report));
    size_t bytes_240 = (size_t) (
        tilefinch_media_fixture_240_end - tilefinch_media_fixture_240_start);
    size_t bytes_360 = (size_t) (
        tilefinch_media_fixture_360_end - tilefinch_media_fixture_360_start);
    printf(
        "tilefinch-media-fixture: event=begin fixture-240=%zu "
        "fixture-360=%zu free=%u largest=%u\n",
        bytes_240, bytes_360,
        sceKernelTotalFreeMemSize(), sceKernelMaxFreeMemSize());
    if (!fixture_prepare_modules(error, error_size)) return false;

    if (!fixture_run_clip(
            budget, "baseline-240", tilefinch_media_fixture_240_start,
            bytes_240, 320u, 240u, true, report, error, error_size))
        return false;
    if (!fixture_run_clip(
            budget, "main-360", tilefinch_media_fixture_360_start,
            bytes_360, 640u, 360u, true, report, error, error_size))
        return false;
    /* Model the browser's power-suspend boundary after all per-stream state
       has gone away. The replay must rebuild process-global MPEG state and
       select its profile again instead of trusting stale process statics. */
    media_psp_backend_system_suspend();
    printf("tilefinch-media-fixture: event=runtime-suspend-boundary\n");
    /* A second ordinary-profile creation proves that teardown/suspend did not
       leave the process-wide module/runtime state unusable after changing ME
       type. */
    if (!fixture_run_clip(
            budget, "baseline-240-replay", tilefinch_media_fixture_240_start,
            bytes_240, 320u, 240u, false, report, error, error_size))
        return false;
    report->replay_completions = 1u;
    printf(
        "tilefinch-media-fixture: event=hardware-pass clips=%u "
        "decoded=%u observed=%u distinct=%u seeks=%u replay=%u "
        "free=%u largest=%u\n",
        report->clips_completed, report->frames_decoded,
        report->frames_observed, report->distinct_frame_signatures,
        report->seek_completions, report->replay_completions,
        sceKernelTotalFreeMemSize(), sceKernelMaxFreeMemSize());
    return report->clips_completed == 3u
        && report->seek_completions == 2u
        && report->replay_completions == 1u;
}

#endif
