#ifndef TILEFINCH_SWDEC_AUDIO_DIAG_H
#define TILEFINCH_SWDEC_AUDIO_DIAG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Validation diagnostic: the first compressed audio packet the decoder
 * failed on, copied while the browser still owns its slot. A packet slot is
 * reused as soon as the Media Engine has decoded it, and decoded audio runs
 * far ahead of playback, so the packet must be captured from the decoder's
 * results (scanned before any slot is written again), never at playback. */
#define SWDEC_AUDIO_BAD_HEAD_BYTES 16u
typedef struct {
    bool captured;
    unsigned index;           /* packet number within its stream position */
    unsigned bytes;
    uint64_t pts_us;
    unsigned char head[SWDEC_AUDIO_BAD_HEAD_BYTES];
} SwdecAudioBadPacket;

/* What was submitted in each packet slot. */
typedef struct {
    unsigned bytes;
    uint64_t pts_us;
} SwdecAudioPacketMeta;

/* The component's result for packet `index`: 0 when not decoded yet, else
 * `samples` holds a sample count or (negative) the decoder's error. */
typedef int (*SwdecAudioPoll)(unsigned index, unsigned *crc,
                              unsigned *samples);

/* Browser thread: look at every packet decoded since `*checked` up to
 * `done` and copy the first failed one. Call it before writing any packet
 * slot, so every slot about to be reused has been looked at. */
static inline void swdec_audio_bad_scan(
    SwdecAudioBadPacket *bad, unsigned *checked, unsigned done,
    SwdecAudioPoll poll, const unsigned char *packets, size_t stride,
    unsigned slots, const SwdecAudioPacketMeta *meta)
{
    if (bad->captured || poll == NULL || packets == NULL || slots == 0) {
        *checked = done;
        return;
    }
    while (*checked != done) {
        unsigned index = *checked;
        unsigned crc = 0, samples = 0;
        if (poll(index, &crc, &samples) == 0) return;
        *checked = index + 1u;
        if ((int) samples >= 0) continue;
        const SwdecAudioPacketMeta *slot_meta = &meta[index % slots];
        bad->captured = true;
        bad->index = index;
        bad->bytes = slot_meta->bytes;
        bad->pts_us = slot_meta->pts_us;
        size_t copy = slot_meta->bytes < SWDEC_AUDIO_BAD_HEAD_BYTES
            ? slot_meta->bytes : SWDEC_AUDIO_BAD_HEAD_BYTES;
        memset(bad->head, 0, sizeof(bad->head));
        memcpy(bad->head, packets + (size_t) (index % slots) * stride, copy);
        *checked = done;
        return;
    }
}

#endif
