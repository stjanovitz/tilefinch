/* Minimal MPEG-TS demuxer: TS packets in, Annex-B H.264 access units and
   ADTS AAC frames out (with 90 kHz PES timestamps). See swdec_ts.c. */
#ifndef SWDEC_TS_H
#define SWDEC_TS_H

#include <stdint.h>
#include <stddef.h>

#define SWDEC_TS_NOPTS ((uint64_t) -1)
#define SWDEC_TS_MAX_AU (512 * 1024)   /* largest expected 240p AU (I-frames ~50 KB; wide margin) */
#define SWDEC_TS_MAX_ADTS 8192
#define SWDEC_TS_MAX_TAIL 256          /* boundary NAL bytes held across an AU cut */

typedef void (*swdec_ts_video_cb)(void *user, const uint8_t *au, size_t len, uint64_t pts90k);
typedef void (*swdec_ts_audio_cb)(void *user, const uint8_t *adts, size_t len, uint64_t pts90k);

typedef struct {
    int pmt_pid, video_pid, audio_pid;
    swdec_ts_video_cb video_cb;
    swdec_ts_audio_cb audio_cb;
    void *user;

    uint8_t au_buf[SWDEC_TS_MAX_AU];
    size_t au_len;
    uint64_t au_pts, pes_pts;
    int au_has_slice;

    uint8_t ad_buf[SWDEC_TS_MAX_ADTS];
    size_t ad_len;
    uint64_t apes_pts;
    uint32_t audio_pts_remainder;
    uint32_t audio_sample_rate;

    struct {
        unsigned sync_losses, malformed_packets, malformed_psi;
        unsigned au_overflows, adts_overflows, adts_resyncs;
    } stats;
} SwdecTs;

void swdec_ts_init(SwdecTs *ts, swdec_ts_video_cb video_cb, swdec_ts_audio_cb audio_cb, void *user);
int swdec_ts_feed(SwdecTs *ts, const uint8_t *data, size_t len);   /* returns undigested tail bytes */
void swdec_ts_flush(SwdecTs *ts);                                   /* end of stream: emit the last AU */

#endif
