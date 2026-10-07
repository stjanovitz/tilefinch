/* libFuzzer target: the MPEG-TS demuxer used for HLS segments
   (src/swdec/swdec_ts.c). Input is fed in chunks whose sizes come from the
   first byte, carrying the undigested tail forward like media_hls.c does. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "swdec/swdec_ts.h"

static void on_video(void *user, const uint8_t *au, size_t len,
                     uint64_t pts90k)
{
    (void) pts90k;
    volatile uint8_t sink = 0;
    if (len > SWDEC_TS_MAX_AU) abort();
    for (size_t i = 0; i < len; i += 64) sink ^= au[i];
    if (len != 0) sink ^= au[len - 1];
    (*(size_t *) user)++;
}

static void on_audio(void *user, const uint8_t *adts, size_t len,
                     uint64_t pts90k)
{
    (void) pts90k;
    volatile uint8_t sink = 0;
    if (len > SWDEC_TS_MAX_ADTS) abort();
    if (len != 0) sink ^= adts[0] ^ adts[len - 1];
    (*(size_t *) user)++;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 1 || size > 1024 * 1024) return 0;
    size_t chunk = 188u * (1u + data[0] % 8u) + data[0] / 8u;
    data++;
    size--;
    static SwdecTs ts; /* ~520 KiB: keep it off the stack. */
    size_t emitted = 0;
    swdec_ts_init(&ts, on_video, on_audio, &emitted);
    uint8_t *pending = malloc(chunk + 188u * 2u);
    if (pending == NULL) return 0;
    size_t pending_length = 0;
    size_t offset = 0;
    while (offset < size) {
        size_t take = size - offset;
        if (take > chunk) take = chunk;
        memcpy(pending + pending_length, data + offset, take);
        pending_length += take;
        offset += take;
        int tail = swdec_ts_feed(&ts, pending, pending_length);
        if (tail < 0 || (size_t) tail > pending_length) abort();
        memmove(pending, pending + pending_length - (size_t) tail,
                (size_t) tail);
        pending_length = (size_t) tail;
        if (pending_length > 188u * 2u) abort();
    }
    swdec_ts_flush(&ts);
    free(pending);
    return 0;
}
