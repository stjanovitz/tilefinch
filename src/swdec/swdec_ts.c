/* swdec_ts.c - minimal MPEG-TS demuxer for the PSP media pipeline.
 *
 * Input: 188-byte TS packets (one segment or a whole stream, fed in chunks).
 * Output: complete H.264 access units (Annex-B, exactly what swdec_decode
 * eats) and complete ADTS AAC frames (what swdec_audio_decode eats), each
 * with the PES PTS (90 kHz) that covered them.
 *
 * Scope (matches the target CDNs): single program, one H.264 PID, one
 * AAC/ADTS PID, no scrambling, no PCR use (PTS is enough for the player).
 * Video AUs are split on access-unit boundaries: a new AU starts at the
 * first slice NAL whose first_mb_in_slice is 0 (or AUD/SPS/PPS run before
 * it); simpler and robust for the 1-slice-per-frame streams we target, and
 * correct in general because we cut on AUD when present.
 */
#include "swdec_ts.h"

#include <string.h>

#define TS_PKT 188

static void au_reset(SwdecTs *ts) { ts->au_len = 0; ts->au_pts = SWDEC_TS_NOPTS; }

void swdec_ts_init(SwdecTs *ts,
                   swdec_ts_video_cb video_cb, swdec_ts_audio_cb audio_cb, void *user)
{
    memset(ts, 0, sizeof *ts);
    ts->pmt_pid = -1; ts->video_pid = -1; ts->audio_pid = -1;
    ts->video_cb = video_cb; ts->audio_cb = audio_cb; ts->user = user;
    au_reset(ts);
    ts->pes_pts = SWDEC_TS_NOPTS;
    ts->apes_pts = SWDEC_TS_NOPTS;
}

static uint64_t parse_pts(const uint8_t *p)
{
    if ((p[0] & 1u) == 0 || (p[2] & 1u) == 0 || (p[4] & 1u) == 0)
        return SWDEC_TS_NOPTS;
    return ((uint64_t) (p[0] & 0x0E) << 29) | ((uint64_t) p[1] << 22)
         | ((uint64_t) (p[2] & 0xFE) << 14) | ((uint64_t) p[3] << 7)
         | ((uint64_t) p[4] >> 1);
}

/* deliver one complete video AU */
static void flush_au(SwdecTs *ts)
{
    if (ts->au_len > 0 && ts->video_cb)
        ts->video_cb(ts->user, ts->au_buf, ts->au_len, ts->au_pts);
    au_reset(ts);
}

/* Scan freshly appended Annex-B bytes for an access-unit boundary and emit
   completed AUs. `base` is the offset where the new bytes begin (minus 4 so a
   start code split across appends is still seen). */
static void video_scan(SwdecTs *ts, size_t from)
{
    const uint8_t *b = ts->au_buf;
    size_t n = ts->au_len;
    size_t i = from > 4 ? from - 4 : 0;
    for (; i + 4 < n; i++) {
        if (b[i] != 0 || b[i + 1] != 0 || b[i + 2] != 1) continue;
        unsigned nal = b[i + 3] & 0x1F;
        int boundary = 0;
        if (nal == 9) {                          /* access unit delimiter */
            boundary = 1;
        } else if ((nal == 1 || nal == 5) && i + 4 < n) {
            /* first_mb_in_slice == 0 <=> first exp-Golomb bit is 1 */
            if (b[i + 4] & 0x80) boundary = 1;
        } else if (nal == 7 || nal == 8) {       /* SPS/PPS start a new AU when
                                                    slices were already seen */
            boundary = ts->au_has_slice;
        }
        if (boundary && ts->au_has_slice && i > 0) {
            size_t cut = i;
            /* keep a preceding zero_byte with the start code */
            if (cut > 0 && b[cut - 1] == 0) { /* 4-byte start codes: emit up to cut anyway */ }
            size_t tail = n - cut;
            uint8_t hold[SWDEC_TS_MAX_TAIL];
            if (tail > sizeof hold) tail = sizeof hold;   /* bounded; boundary NALs are tiny here */
            memcpy(hold, b + cut, tail);
            uint64_t next_pts = ts->pes_pts;
            ts->au_len = cut;
            flush_au(ts);
            memcpy(ts->au_buf, hold, tail);
            ts->au_len = tail;
            ts->au_pts = next_pts;
            ts->au_has_slice = 0;
            b = ts->au_buf; n = ts->au_len; i = 0;
        }
        if (nal == 1 || nal == 5) ts->au_has_slice = 1;
    }
}

static void video_payload(SwdecTs *ts, const uint8_t *p, size_t n, int pes_start)
{
    if (pes_start) {
        /* PES header: 00 00 01 E0.. */
        if (n < 9 || p[0] != 0 || p[1] != 0 || p[2] != 1) return;
        unsigned hdrlen = p[8];
        uint64_t pts = SWDEC_TS_NOPTS;
        if ((p[7] & 0x80) && hdrlen >= 5u && n >= 14) pts = parse_pts(p + 9);
        ts->pes_pts = pts;
        if (ts->au_len == 0) ts->au_pts = pts;
        if (9u + hdrlen > n) return;
        p += 9 + hdrlen; n -= 9 + hdrlen;
    }
    if (ts->au_len + n > sizeof ts->au_buf) {    /* overflow: drop the AU */
        ts->stats.au_overflows++;
        au_reset(ts);
        ts->au_has_slice = 0;
        return;
    }
    size_t from = ts->au_len;
    memcpy(ts->au_buf + ts->au_len, p, n);
    ts->au_len += n;
    if (ts->au_pts == SWDEC_TS_NOPTS) ts->au_pts = ts->pes_pts;
    video_scan(ts, from);
}

/* audio: ADTS frames are self-delimiting; buffer PES payloads and cut on the
   ADTS frame_length field */
static void audio_payload(SwdecTs *ts, const uint8_t *p, size_t n, int pes_start)
{
    if (pes_start) {
        if (n < 9 || p[0] != 0 || p[1] != 0 || p[2] != 1) return;
        unsigned hdrlen = p[8];
        if ((p[7] & 0x80) && hdrlen >= 5u && n >= 14) {
            ts->apes_pts = parse_pts(p + 9);
            ts->audio_pts_remainder = 0;
        }
        if (9u + hdrlen > n) return;
        p += 9 + hdrlen; n -= 9 + hdrlen;
    }
    if (n > sizeof ts->ad_buf) {
        ts->stats.adts_overflows++;
        ts->ad_len = 0;
        return;
    }
    if (ts->ad_len > sizeof ts->ad_buf - n) {
        ts->stats.adts_overflows++;
        ts->ad_len = 0;
    }
    memcpy(ts->ad_buf + ts->ad_len, p, n);
    ts->ad_len += n;
    size_t i = 0;
    while (i + 7 <= ts->ad_len) {
        if (ts->ad_buf[i] != 0xFF || (ts->ad_buf[i + 1] & 0xF0) != 0xF0) { i++; ts->stats.adts_resyncs++; continue; }
        size_t fl = ((size_t) (ts->ad_buf[i + 3] & 3) << 11)
                  | ((size_t) ts->ad_buf[i + 4] << 3)
                  | ((size_t) ts->ad_buf[i + 5] >> 5);
        if (fl < 7) { i++; ts->stats.adts_resyncs++; continue; }
        if (i + fl > ts->ad_len) break;          /* incomplete */
        uint64_t frame_pts = ts->apes_pts;
        if (ts->audio_cb)
            ts->audio_cb(ts->user, ts->ad_buf + i, fl, ts->apes_pts);
        /* PES timestamps cover the first frame. Derive the following ADTS
           frame positions with an exact integer remainder so a long segment
           neither collapses them to NOPTS nor accumulates rounding drift. */
        unsigned rate_index = (ts->ad_buf[i + 2] >> 2) & 0x0fu;
        static const uint32_t rates[] = {
            96000, 88200, 64000, 48000, 44100, 32000, 24000,
            22050, 16000, 12000, 11025, 8000, 7350
        };
        if (rate_index < sizeof(rates) / sizeof(rates[0])
            && frame_pts != SWDEC_TS_NOPTS) {
            uint32_t rate = rates[rate_index];
            uint32_t blocks = (ts->ad_buf[i + 6] & 3u) + 1u;
            uint32_t numerator = ts->audio_pts_remainder
                + blocks * UINT32_C(1024) * UINT32_C(90000);
            ts->apes_pts = frame_pts + numerator / rate;
            ts->audio_pts_remainder = numerator % rate;
            ts->audio_sample_rate = rate;
        } else {
            ts->apes_pts = SWDEC_TS_NOPTS;
            ts->audio_pts_remainder = 0;
            ts->audio_sample_rate = 0;
        }
        i += fl;
    }
    memmove(ts->ad_buf, ts->ad_buf + i, ts->ad_len - i);
    ts->ad_len -= i;
}

int swdec_ts_feed(SwdecTs *ts, const uint8_t *data, size_t len)
{
    if (ts == NULL || (data == NULL && len != 0)) return (int) len;
    size_t off = 0;
    while (off + TS_PKT <= len) {
        const uint8_t *p = data + off;
        if (p[0] != 0x47) {                      /* resync */
            off++;
            ts->stats.sync_losses++;
            continue;
        }
        off += TS_PKT;
        unsigned pid = ((p[1] & 0x1F) << 8) | p[2];
        int pusi = (p[1] & 0x40) != 0;
        unsigned afc = (p[3] >> 4) & 3;
        const uint8_t *pl = p + 4;
        size_t pn = TS_PKT - 4;
        if (afc == 0) {
            ts->stats.malformed_packets++;
            continue;
        }
        if (afc == 2) continue;
        if (afc == 3) {
            if (pn == 0) {
                ts->stats.malformed_packets++;
                continue;
            }
            unsigned alen = pl[0] + 1u;
            if (alen > pn) {
                ts->stats.malformed_packets++;
                continue;
            }
            if (alen == pn) continue;
            pl += alen; pn -= alen;
        }
        if (pid == 0) {                          /* PAT */
            if (!pusi || pn < 1) continue;
            size_t pointer = pl[0];
            if (pointer > pn - 1u || pn - 1u - pointer < 12u) {
                ts->stats.malformed_psi++;
                continue;
            }
            const uint8_t *sec = pl + 1u + pointer;
            size_t available = pn - 1u - pointer;
            size_t section = 3u
                + ((((size_t) sec[1] & 0x0fu) << 8) | sec[2]);
            if (sec[0] != 0u || section < 12u || section > available) {
                ts->stats.malformed_psi++;
                continue;
            }
            /* first program's PMT PID (single-program streams) */
            ts->pmt_pid = (int) (((sec[10] & 0x1F) << 8) | sec[11]);
        } else if ((int) pid == ts->pmt_pid) {   /* PMT */
            if (!pusi || pn < 1) continue;
            size_t pointer = pl[0];
            if (pointer > pn - 1u || pn - 1u - pointer < 16u) {
                ts->stats.malformed_psi++;
                continue;
            }
            const uint8_t *sec = pl + 1u + pointer;
            size_t available = pn - 1u - pointer;
            size_t slen = (((sec[1] & 0x0F) << 8) | sec[2]) + 3u;
            if (sec[0] != 2u || slen < 16u || slen > available) {
                ts->stats.malformed_psi++;
                continue;
            }
            size_t pil = ((sec[10] & 0x0F) << 8) | sec[11];
            if (pil > slen - 16u) {
                ts->stats.malformed_psi++;
                continue;
            }
            size_t i = 12 + pil;
            size_t payload_end = slen - 4u;
            while (i <= payload_end && payload_end - i >= 5u) {
                unsigned stype = sec[i];
                unsigned epid = ((sec[i + 1] & 0x1F) << 8) | sec[i + 2];
                size_t eil = ((sec[i + 3] & 0x0F) << 8) | sec[i + 4];
                if (eil > payload_end - i - 5u) {
                    ts->stats.malformed_psi++;
                    break;
                }
                if (stype == 0x1B) ts->video_pid = (int) epid;          /* H.264 */
                else if (stype == 0x0F) ts->audio_pid = (int) epid;     /* AAC/ADTS */
                i += 5 + eil;
            }
        } else if ((int) pid == ts->video_pid) {
            video_payload(ts, pl, pn, pusi);
        } else if ((int) pid == ts->audio_pid) {
            audio_payload(ts, pl, pn, pusi);
        }
    }
    return (int) (len - off);                    /* undigested tail bytes (caller re-feeds) */
}

void swdec_ts_flush(SwdecTs *ts)
{
    if (ts->au_has_slice) flush_au(ts);
    au_reset(ts);
    ts->au_has_slice = 0;
    ts->ad_len = 0;
    ts->apes_pts = SWDEC_TS_NOPTS;
    ts->audio_pts_remainder = 0;
    ts->audio_sample_rate = 0;
}
