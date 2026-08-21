/*
 * swdec -- software H.264 (High profile) decoder for Tilefinch on PSP.
 *
 * C ABI over the ffmpeg h264 decoder core (single decoder, no threads,
 * arena allocation), shaped for the project's rules:
 *   - all memory comes from one caller-provided arena at open time; no heap
 *     allocation after swdec_open() returns;
 *   - one access unit in, at most one picture out, per call; no threads;
 *   - output is planar 4:2:0 borrowed until the next decode call.
 *
 * Integration seam for the media pipeline: feed Annex-B (or length-prefixed
 * converted to Annex-B) access units in decode order and take pictures in
 * output order.
 */
#ifndef SWDEC_H
#define SWDEC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct swdec swdec;

typedef struct {
    const uint8_t *plane[3];  /* Y, U, V */
    int stride[3];
    int width;                /* cropped luma width  */
    int height;               /* cropped luma height */
    uint64_t pts;             /* echoed from the AU that produced it */
} swdec_picture;

typedef struct {
    size_t arena_bytes;      /* total arena the caller must supply */
    size_t peak_bytes;       /* high-water mark inside the arena */
    uint32_t frames_out;
    uint32_t aus_in;
    uint32_t errors;
} swdec_stats;

/* Bytes the caller must reserve for a 4:2:0 stream of at most
   max_width x max_height with max_refs reference frames. */
size_t swdec_arena_bytes(int max_width, int max_height, int max_refs);

/* Open a decoder using [arena, arena+arena_bytes). Returns NULL on
   failure (arena too small, or the decoder core refused). */
swdec *swdec_open(void *arena, size_t arena_bytes,
                  int max_width, int max_height, int max_refs);

/* Decode one access unit (Annex-B). Returns 1 if *out now holds a picture
   (valid until the next swdec_* call), 0 if none was output, <0 on error.
   Passing NULL/0 flushes: repeated calls drain reordered pictures. */
int swdec_decode(swdec *d, const uint8_t *au, size_t au_bytes,
                 uint64_t pts, swdec_picture *out);

void swdec_stats_get(const swdec *d, swdec_stats *out);

/* ---- AAC-LC audio (ffmpeg aac_fixed: integer-only, runs on the Media Engine) ---- */
typedef struct swdec_audio swdec_audio;

typedef struct {
    const int32_t *plane[2];  /* per-channel 32-bit samples (top 16 bits = S16) */
    int nb_samples;           /* per channel */
    int channels;
    int sample_rate;
} swdec_audio_frame;

/* arena: either the same arena already given to swdec_open (pass bytes = 0),
   or a fresh buffer to initialize (audio-only use) */
swdec_audio *swdec_audio_open(void *arena, size_t arena_bytes);
/* feed one complete ADTS frame; returns 1 when out is filled, 0 for no output
   yet, negative on error */
int swdec_audio_decode(swdec_audio *a, const uint8_t *adts, size_t adts_bytes,
                       swdec_audio_frame *out);
void swdec_audio_close(swdec_audio *a);
void swdec_close(swdec *d);

/* Runtime speed/quality trade-offs the player can flip when the device
   falls behind. None of them cause drift: they only touch pictures that
   no other picture references.
     SWDEC_SPEED_FULL       decode everything as the stream intends
     SWDEC_SPEED_NOREF_LF   skip the deblocking filter on non-reference
                            pictures (typically the B-frames; ~15-20% less
                            work on them, mild blockiness on those frames)
     SWDEC_SPEED_DROP_NOREF drop non-reference pictures entirely (they are
                            never decoded; output rate falls to the
                            reference-picture rate, e.g. 1/4 for bframes=3)
   Takes effect at the next access unit. */
enum {
    SWDEC_SPEED_FULL = 0,
    SWDEC_SPEED_NOREF_LF = 1,
    SWDEC_SPEED_DROP_NOREF = 2,
    /* decode every second non-reference picture (and skip deblocking on
       the ones decoded): ~63% of the work of FULL on a bframes=3 stream,
       12.5 fps effective output at 25 fps input; drift-free */
    SWDEC_SPEED_HALF_NOREF = 3,
    /* drop every 4th non-reference picture (keeps 3/4 of the B-frames;
       ~19 fps effective at a 25 fps source, no drift) */
    SWDEC_SPEED_3Q_NOREF = 4
};
void swdec_set_speed(swdec *d, int speed);

#ifdef __cplusplus
}
#endif
#endif
