/*
 * swdec.c -- C ABI over ffmpeg's h264 decoder (libavcodec, single-decoder
 * build, no threads, no logging, --malloc-prefix=swdec_ so every allocation
 * lands in the caller's arena).
 *
 * The decoder is the H.264 conformance reference; openh264 was rejected for
 * this target after it proved non-conformant on B-slices (74% of the target
 * content). See NOTES.md.
 */
#include "swdec.h"
#include "swdec_arena.h"
#include "swdec_bounds.h"

#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>

static int au_nal_ref_idc(const uint8_t *au, size_t n);

struct swdec {
    AVCodecContext *ctx;
    AVPacket *pkt;
    AVFrame *frame;
    swdec_stats acct;
    int flushed;
    int speed;
    int max_width;
    int max_height;
    unsigned noref_seen;
};

size_t swdec_arena_bytes(int max_width, int max_height, int max_refs)
{
    /* Sized from measurement (arena_peak on host and device); ffmpeg's h264
       keeps refs+2 frames in its pool plus per-MB tables and slice context.
       Generous by design; the harness reports true peak so it can tighten. */
    size_t w = (size_t) ((max_width + 31) & ~31), h = (size_t) ((max_height + 31) & ~31);
    size_t pic = (w + 64) * (h + 64) * 3 / 2;
    size_t pics = pic * (size_t) (max_refs + 4);
    size_t mbs = ((size_t) max_width / 16 + 1) * ((size_t) max_height / 16 + 1);
    return pics + mbs * 1024 + 1536 * 1024;
}

swdec *swdec_open(void *arena, size_t arena_bytes,
                  int max_width, int max_height, int max_refs)
{
#ifdef SWDEC_POISON
    memset(arena, 0xAA, arena_bytes);   /* diagnosis: make uninitialized heap reads deterministic and loud */
#endif

    (void) max_refs;
    if (arena == NULL || arena_bytes < 512 * 1024
        || max_width <= 0 || max_height <= 0) return NULL;
    swdec_arena_bind(arena, arena_bytes);
    av_log_set_level(AV_LOG_QUIET);

    swdec *d = (swdec *) swdec_malloc(sizeof(swdec));
    if (d == NULL) { swdec_arena_unbind(); return NULL; }
    memset(d, 0, sizeof(*d));

    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (codec == NULL) { swdec_arena_unbind(); return NULL; }
    d->ctx = avcodec_alloc_context3(codec);
    if (d->ctx == NULL) { swdec_arena_unbind(); return NULL; }
    d->ctx->thread_count = 1;
    d->ctx->thread_type = 0;
    /* Output every decoded picture as soon as the reorder rules allow;
       flags2 FAST would trade conformance for speed -- not here. */
    if (avcodec_open2(d->ctx, codec, NULL) < 0) { swdec_arena_unbind(); return NULL; }
    d->pkt = av_packet_alloc();
    d->frame = av_frame_alloc();
    if (d->pkt == NULL || d->frame == NULL) { swdec_arena_unbind(); return NULL; }
    d->acct.arena_bytes = arena_bytes;
    d->max_width = max_width;
    d->max_height = max_height;
    return d;
}

static void fill_picture(const AVFrame *f, swdec_picture *out);

static int publish_picture(swdec *d, swdec_picture *out)
{
    if (!swdec_dimensions_admitted(
            d->frame->width, d->frame->height,
            d->max_width, d->max_height)) {
        d->acct.errors++;
        return -5;
    }
    d->acct.frames_out++;
    fill_picture(d->frame, out);
    return 1;
}

static void fill_picture(const AVFrame *f, swdec_picture *out)
{
    out->plane[0] = f->data[0];
    out->plane[1] = f->data[1];
    out->plane[2] = f->data[2];
    out->stride[0] = f->linesize[0];
    out->stride[1] = f->linesize[1];
    out->stride[2] = f->linesize[2];
    out->width = f->width;
    out->height = f->height;
    out->pts = (uint64_t) f->pts;
}

int swdec_decode(swdec *d, const uint8_t *au, size_t au_bytes,
                 uint64_t pts, swdec_picture *out)
{
    if (d == NULL || out == NULL) return -1;
    memset(out, 0, sizeof(*out));
    int rc;
    if (au != NULL && au_bytes != 0) {
        d->acct.aus_in++;
        if ((d->speed == SWDEC_SPEED_HALF_NOREF || d->speed == SWDEC_SPEED_3Q_NOREF)
            && au_nal_ref_idc(au, au_bytes) == 0
            && (d->speed == SWDEC_SPEED_HALF_NOREF ? (d->noref_seen++ & 1)
                                                   : ((d->noref_seen++ & 3) == 3))) {
            /* drop every second non-reference picture: legal (nothing references it) */
            rc = avcodec_receive_frame(d->ctx, d->frame);
            d->acct.peak_bytes = swdec_arena_peak();
            if (rc == 0) return publish_picture(d, out);
            return 0;
        }
        /* libavcodec wants AV_INPUT_BUFFER_PADDING_SIZE bytes of zeroed
           slack after the payload; the caller's buffer may not have it, so
           we copy into a padded packet (one memcpy of a few KB per AU). */
        if (av_new_packet(d->pkt, (int) au_bytes) < 0) return -2;
        memcpy(d->pkt->data, au, au_bytes);
        d->pkt->pts = (int64_t) pts;
        rc = avcodec_send_packet(d->ctx, d->pkt);
        av_packet_unref(d->pkt);
        if (rc < 0 && rc != AVERROR(EAGAIN)) d->acct.errors++;
    } else if (!d->flushed) {
        avcodec_send_packet(d->ctx, NULL);
        d->flushed = 1;
    }
    rc = avcodec_receive_frame(d->ctx, d->frame);
    d->acct.peak_bytes = swdec_arena_peak();
    if (rc == 0) {
        return publish_picture(d, out);
    }
    if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return 0;
    d->acct.errors++;
    return -3;
}

void swdec_set_speed(swdec *d, int speed)
{
    if (d == NULL || d->ctx == NULL) return;
    switch (speed) {
    default:
    case SWDEC_SPEED_FULL:
        d->ctx->skip_loop_filter = AVDISCARD_DEFAULT;
        d->ctx->skip_frame = AVDISCARD_DEFAULT;
        break;
    case SWDEC_SPEED_NOREF_LF:
        d->ctx->skip_loop_filter = AVDISCARD_NONREF;
        d->ctx->skip_frame = AVDISCARD_DEFAULT;
        break;
    case SWDEC_SPEED_DROP_NOREF:
        d->ctx->skip_loop_filter = AVDISCARD_NONREF;
        d->ctx->skip_frame = AVDISCARD_NONREF;
        break;
    case SWDEC_SPEED_3Q_NOREF:
    case SWDEC_SPEED_HALF_NOREF:
        d->ctx->skip_loop_filter = AVDISCARD_NONREF;
        d->ctx->skip_frame = AVDISCARD_DEFAULT;   /* the wrapper drops non-ref AUs itself (1-in-2 or 1-in-4) */
        break;
    }
    d->speed = speed;
}

/* nal_ref_idc of the first VCL NAL of an Annex-B access unit (0 = the
   picture is never referenced and may be dropped without drift), -1 if none */
static int au_nal_ref_idc(const uint8_t *au, size_t n)
{
    for (size_t i = 0; i + 3 < n; i++) {
        if (au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 1) {
            int t = au[i + 3] & 0x1F;
            if (t >= 1 && t <= 5) return (au[i + 3] >> 5) & 3;
            i += 2;
        }
    }
    return -1;
}

void swdec_stats_get(const swdec *d, swdec_stats *out)
{
    if (d == NULL || out == NULL) return;
    *out = d->acct;
    out->peak_bytes = swdec_arena_peak();
}

void swdec_close(swdec *d)
{
    if (d == NULL) return;
    if (d->frame) av_frame_free(&d->frame);
    if (d->pkt) av_packet_free(&d->pkt);
    if (d->ctx) avcodec_free_context(&d->ctx);
    swdec_arena_unbind();
}


/* ---------------- AAC-LC audio (aac_fixed, integer-only) ---------------- */
struct swdec_audio {
    AVCodecContext *ctx;
    AVFrame *frame;
    AVPacket *pkt;
};

swdec_audio *swdec_audio_open(void *arena, size_t arena_bytes)
{
    if (arena != NULL && arena_bytes > 0)
        swdec_arena_bind(arena, arena_bytes);   /* audio-only use; video callers pass bytes = 0 (arena already bound) */
    av_log_set_level(AV_LOG_QUIET);
    const AVCodec *dec = avcodec_find_decoder_by_name("aac_fixed");
    if (dec == NULL) return NULL;
    swdec_audio *a = swdec_malloc(sizeof *a);
    if (a == NULL) return NULL;
    memset(a, 0, sizeof *a);
    a->ctx = avcodec_alloc_context3(dec);
    a->frame = av_frame_alloc();
    a->pkt = av_packet_alloc();
    if (a->ctx == NULL || a->frame == NULL || a->pkt == NULL) { swdec_audio_close(a); return NULL; }
    if (avcodec_open2(a->ctx, dec, NULL) < 0) { swdec_audio_close(a); return NULL; }
    return a;
}

int swdec_audio_decode(swdec_audio *a, const uint8_t *adts, size_t adts_bytes, swdec_audio_frame *out)
{
    int rc;
    if (a == NULL || out == NULL) return -1;
    memset(out, 0, sizeof(*out));
    if (adts != NULL && adts_bytes > 0) {
        if (av_new_packet(a->pkt, (int) adts_bytes) < 0) return -2;
        memcpy(a->pkt->data, adts, adts_bytes);
        rc = avcodec_send_packet(a->ctx, a->pkt);
        av_packet_unref(a->pkt);
        if (rc < 0 && rc != AVERROR(EAGAIN)) return -3;
    }
    rc = avcodec_receive_frame(a->ctx, a->frame);
    if (rc == 0) {
        int channels = a->frame->ch_layout.nb_channels;
        if (!swdec_audio_channels_admitted(channels)
            || a->frame->data[0] == NULL
            || (channels == 2 && a->frame->data[1] == NULL)) return -5;
        out->plane[0] = (const int32_t *) a->frame->data[0];
        out->plane[1] = (const int32_t *) (a->frame->data[1] ? a->frame->data[1] : a->frame->data[0]);
        out->nb_samples = a->frame->nb_samples;
        out->channels = channels;
        out->sample_rate = a->frame->sample_rate;
        return 1;
    }
    if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return 0;
    return -4;
}

void swdec_audio_close(swdec_audio *a)
{
    if (a == NULL) return;
    if (a->ctx) avcodec_free_context(&a->ctx);
    if (a->frame) av_frame_free(&a->frame);
    if (a->pkt) av_packet_free(&a->pkt);
    swdec_free(a);
}
