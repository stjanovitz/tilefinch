#ifndef TILEFINCH_MEDIA_BACKEND_PSP_POOL_H
#define TILEFINCH_MEDIA_BACKEND_PSP_POOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "media_backend_psp_policy.h"
#include "media_h264_psp_compat.h"

/*
 * Where the Media Engine stops being able to see user memory.
 *
 * A PSP-2000/3000 carries 64 MiB of RAM. The stock user partition is the
 * ~24 MiB window that starts at 0x08800000; everything from 0x0A000000 upward
 * is the extra bank that custom firmware unlocks, and that bank is reachable
 * only by the main CPU. The Media Engine -- which is what actually executes
 * sceMpeg's AVC program and sceAudiocodec's AAC program, reading their buffers
 * by DMA rather than through the main CPU -- cannot address it at all.
 *
 * Tilefinch runs with the extra-memory unlock active (the device log reports
 * `heap-capacity=40MB`), so by the time a page has loaded and a video opens,
 * ordinary heap growth can easily have pushed the media backend's large
 * aligned allocations past this line. Firmware then DMAs from an address the
 * Media Engine does not have, both codecs are handed garbage, and they fail
 * on structurally perfect input: the raw-NAL bridge returns 0xFFFFFFFF for a
 * valid IDR and sceAudiocodecDecode returns 0x807F0001 for a valid AAC frame.
 * Nothing in an emulator models this -- PPSSPP's HLE decodes from main memory
 * -- and PMPlayer, the implementation this backend was transcribed from,
 * predates the high-memory mod entirely.
 */
#define PSP_MEDIA_ME_VISIBLE_LIMIT UINT32_C(0x0A000000)

/* Firmware wants a 64-byte-aligned address and length for every buffer it
   invalidates or DMAs; the pool never hands back anything weaker. */
#define PSP_MEDIA_POOL_ALIGNMENT ((size_t) 64u)

/* Audio and access-unit block sizes live here rather than beside their uses
   so the pool can be sized from them without pulling in PSP headers. */
#define PSP_MEDIA_AUDIO_CODEC_WORDS 65u
#define PSP_MEDIA_AUDIO_CODEC_BYTES \
    (PSP_MEDIA_AUDIO_CODEC_WORDS * sizeof(unsigned long))
#define PSP_MEDIA_AUDIO_PCM_BYTES \
    (PSP_MEDIA_AUDIO_SAMPLES * 2u * sizeof(int16_t))
#define PSP_MEDIA_AUDIO_QUEUE_BYTES \
    (PSP_MEDIA_AUDIO_QUEUE_SLOTS * PSP_MEDIA_AUDIO_PCM_BYTES)
#define PSP_MEDIA_VIDEO_AU_BYTES 64u

/*
 * sceMpegQueryMemSize is a runtime call, so the workspace cannot be sized
 * exactly at boot. Every firmware revision this backend has run on answers
 * 0x10000 for the admitted modes; reserve four times that so a larger answer
 * still comes out of the pool, and fall back to the heap with a logged
 * `me-pool-exhausted` line if some firmware asks for more than that.
 */
#define PSP_MEDIA_POOL_MPEG_WORKSPACE_BYTES ((size_t) (256u * 1024u))
/* SPS + PPS for the single admitted parameter-set pair. Real avcC records
   are well under 128 bytes; this is the ceiling, not the expectation. */
#define PSP_MEDIA_POOL_PARAMETER_SET_BYTES ((size_t) (4u * 1024u))
/* The largest admitted CSC target: 640x360 clamped to a 768-pixel stride and
   368 rows of RGBA (psp_media_surface_policy). Per slot. */
#define PSP_MEDIA_POOL_MAX_SURFACE_BYTES \
    ((size_t) PSP_MEDIA_360P_FRAME_STRIDE * PSP_MEDIA_360P_FRAME_ROWS * 4u)
#define PSP_MEDIA_PACKET_STAGING_BYTES \
    ((size_t) PSP_MEDIA_MAXIMUM_PACKET_BYTES \
     + MEDIA_H264_PSP_COMPAT_EXTRA_BYTES)
/*
 * The AAC codec's own work buffer, which PMPlayer allocates from ordinary
 * user memory instead of calling sceAudiocodecGetEDRAM for. Its size is
 * whatever sceAudiocodecCheckNeedMem wrote into control word 4, so like the
 * MPEG workspace it cannot be known at boot; every firmware revision answers
 * well under 16 KiB for AAC. Reserve four times that so a larger answer still
 * comes out of the pool rather than falling back to the heap.
 */
#define PSP_MEDIA_POOL_AUDIO_EDRAM_BYTES ((size_t) (64u * 1024u))
/* Alignment padding between blocks plus room for one more small firmware
   buffer without moving the reservation. */
#define PSP_MEDIA_POOL_SLACK_BYTES ((size_t) (192u * 1024u))

/*
 * Everything the Media Engine ever touches, reserved once. The DDR arena is
 * both the largest block and the one with the strictest alignment, so the
 * reservation itself is made on a PSP_MEDIA_DDR_ALIGNMENT boundary and the
 * arena is handed out from offset 0 -- the 4 MiB alignment then costs nothing
 * inside the pool.
 */
#define PSP_MEDIA_POOL_BYTES                                                 \
    ((size_t) PSP_MEDIA_DDR_BYTES                                            \
     + PSP_MEDIA_POOL_MAX_SURFACE_BYTES * PSP_MEDIA_SURFACE_SLOTS             \
     + PSP_MEDIA_POOL_MPEG_WORKSPACE_BYTES                                   \
     + (size_t) PSP_MEDIA_PACKET_STAGING_BYTES                              \
     + (size_t) PSP_MEDIA_AUDIO_CODEC_BYTES                                  \
     + (size_t) PSP_MEDIA_AUDIO_PCM_BYTES                                    \
     + (size_t) PSP_MEDIA_AUDIO_QUEUE_BYTES                                  \
     /* Two compressed AAC access units, so one worker job can carry a
        batch. Separate from the shared packet staging on purpose: that
        buffer is overwritten by the next VIDEO submission, and a staged
        audio unit now outlives the call that staged it. */               \
     + (size_t) PSP_MEDIA_AUDIO_STAGING_BYTES                                \
     + (size_t) PSP_MEDIA_AUDIO_PENDING_BYTES                                \
     + PSP_MEDIA_POOL_AUDIO_EDRAM_BYTES                                      \
     + (size_t) PSP_MEDIA_VIDEO_AU_BYTES                                     \
     + PSP_MEDIA_POOL_PARAMETER_SET_BYTES                                    \
     + PSP_MEDIA_POOL_SLACK_BYTES)

/*
 * Only reserve when the heap is big enough that holding the pool for the whole
 * process cannot cost a page its content ceiling.
 *
 * This is not only a safety margin, it is the exact condition under which the
 * pool is needed at all. The stock user partition runs from 0x08800000 to
 * 0x09FFFFFF, so on a PSP without the extra-memory unlock every heap address
 * is below PSP_MEDIA_ME_VISIBLE_LIMIT by construction and the ordinary heap
 * path was never at risk. A heap small enough to fail this check is therefore
 * also one where the reservation buys nothing.
 *
 * The quantity compared here is newlib heap capacity, not
 * sceKernelTotalFreeMemSize. The browser links PSP_HEAP_SIZE_KB(-1) with
 * PSP_HEAP_THRESHOLD_SIZE_KB(2048), so PSPSDK hands the whole largest
 * partition block bar 2 MiB to newlib before main() is entered: the partition
 * then reports about 2.2 MB free on every device, unlocked or not (the
 * hardware log's `free-mem=2326528`), which says nothing at all about how much
 * memory the pool's memalign can draw. Sampling the partition instead of the
 * heap is what made the first version of this gate refuse the reservation on
 * the very hardware it was written for.
 */
#define PSP_MEDIA_POOL_CONTENT_HEADROOM_BYTES ((size_t) (24u * 1024u * 1024u))

static inline bool psp_media_pool_reservation_admitted(size_t capacity_bytes)
{
    /* Compare by subtraction: the addition wraps on a 32-bit target. */
    if (capacity_bytes < PSP_MEDIA_POOL_CONTENT_HEADROOM_BYTES) return false;
    return capacity_bytes - PSP_MEDIA_POOL_CONTENT_HEADROOM_BYTES
        >= PSP_MEDIA_POOL_BYTES;
}

/*
 * How the heap capacity above is sampled: allocate 1 MiB blocks until one
 * fails or the answer can no longer change the verdict, then release them all
 * again. There is no firmware call that reports newlib's capacity, and the
 * validation-only `heap-capacity=%dMB` probe cannot serve as the signal
 * because it deliberately runs after the reservation and only in logging
 * builds.
 *
 * The cap is what makes this shipping-safe. The probe stops at the first
 * count that satisfies the predicate, so it never walks the whole 40 MB heap
 * and it can never answer "yes" without having actually held that much memory
 * at once. It runs at the very top of boot, before anything else has taken a
 * byte, so the blocks come off a pristine heap and are freed -- coalescing
 * back into it -- before the reservation's memalign asks for a 4 MiB-aligned
 * base. The device log's `event=me-pool ... high=` flag is the standing check
 * that the base is still low afterwards.
 */
#define PSP_MEDIA_POOL_PROBE_BLOCK_BYTES ((size_t) (1024u * 1024u))
#define PSP_MEDIA_POOL_PROBE_BLOCKS                                          \
    ((PSP_MEDIA_POOL_CONTENT_HEADROOM_BYTES + PSP_MEDIA_POOL_BYTES           \
      + PSP_MEDIA_POOL_PROBE_BLOCK_BYTES - 1u)                               \
     / PSP_MEDIA_POOL_PROBE_BLOCK_BYTES)

/*
 * A bump allocator over that reservation.
 *
 * Media buffers are allocated together when a stream opens and released
 * together when its backend is destroyed, so a cursor and a reset are the
 * whole lifetime; there is deliberately no general free. `poisoned` exists
 * because a quarantined teardown leaks every firmware-visible buffer on
 * purpose -- firmware may still be reading from them -- so the pool must
 * never be reused after one, even though the reservation itself is kept.
 */
typedef struct {
    unsigned char *base;
    size_t bytes;
    size_t cursor;
    size_t high_water;
    unsigned allocations;
    unsigned exhaustions;
    bool poisoned;
} PspMediaPool;

static inline void psp_media_pool_init(
    PspMediaPool *pool, void *base, size_t bytes)
{
    if (pool == NULL) return;
    pool->base = base == NULL ? NULL : (unsigned char *) base;
    pool->bytes = base == NULL ? 0 : bytes;
    pool->cursor = 0;
    pool->high_water = 0;
    pool->allocations = 0;
    pool->exhaustions = 0;
    pool->poisoned = false;
}

static inline bool psp_media_pool_available(const PspMediaPool *pool)
{
    return pool != NULL && pool->base != NULL && pool->bytes != 0
        && !pool->poisoned;
}

static inline size_t psp_media_pool_remaining(const PspMediaPool *pool)
{
    if (!psp_media_pool_available(pool)) return 0;
    return pool->bytes - pool->cursor;
}

static inline bool psp_media_pool_owns(
    const PspMediaPool *pool, const void *pointer)
{
    if (pool == NULL || pool->base == NULL || pointer == NULL) return false;
    uintptr_t address = (uintptr_t) pointer;
    uintptr_t base = (uintptr_t) pool->base;
    return address >= base && address < base + (uintptr_t) pool->bytes;
}

/*
 * Hand out `bytes` on an `alignment` boundary. `needed` reports what the
 * request would have consumed including alignment padding and `remaining`
 * what was left, so an exhausted pool can say both in one log line. A NULL
 * result is a normal outcome: the caller falls back to the ordinary heap,
 * because a possibly-invisible buffer still beats no decode attempt at all.
 */
static inline void *psp_media_pool_alloc(
    PspMediaPool *pool, size_t bytes, size_t alignment,
    size_t *needed, size_t *remaining)
{
    if (needed != NULL) *needed = bytes;
    if (remaining != NULL) *remaining = psp_media_pool_remaining(pool);
    if (bytes == 0 || !psp_media_pool_available(pool)) return NULL;
    if (alignment < PSP_MEDIA_POOL_ALIGNMENT)
        alignment = PSP_MEDIA_POOL_ALIGNMENT;
    if ((alignment & (alignment - 1u)) != 0) return NULL;
    if (bytes > SIZE_MAX - (PSP_MEDIA_POOL_ALIGNMENT - 1u)) return NULL;
    size_t rounded = (bytes + (PSP_MEDIA_POOL_ALIGNMENT - 1u))
        & ~(PSP_MEDIA_POOL_ALIGNMENT - 1u);
    uintptr_t base = (uintptr_t) pool->base;
    uintptr_t address = base + (uintptr_t) pool->cursor;
    /* Align the address, not the offset: a host test may reserve storage the
       C library aligned however it liked, and on device the DDR arena's 4 MiB
       boundary is a property of the address firmware receives. */
    uintptr_t aligned = (address + (uintptr_t) (alignment - 1u))
        & ~(uintptr_t) (alignment - 1u);
    if (aligned < address) return NULL;
    size_t offset = (size_t) (aligned - base);
    /* Compare by subtraction: a 32-bit target wraps the addition. */
    if (offset > pool->bytes || rounded > pool->bytes - offset) {
        if (needed != NULL) *needed = (offset - pool->cursor) + rounded;
        pool->exhaustions++;
        return NULL;
    }
    pool->cursor = offset + rounded;
    if (pool->cursor > pool->high_water) pool->high_water = pool->cursor;
    pool->allocations++;
    if (needed != NULL) *needed = rounded;
    if (remaining != NULL) *remaining = pool->bytes - pool->cursor;
    return pool->base + offset;
}

/* Every media buffer is released at once, so a destroy that completed can
   simply rewind. Refuses while poisoned. */
static inline bool psp_media_pool_reset(PspMediaPool *pool)
{
    if (pool == NULL || pool->base == NULL || pool->poisoned) return false;
    pool->cursor = 0;
    pool->allocations = 0;
    return true;
}

static inline void psp_media_pool_poison(PspMediaPool *pool)
{
    if (pool == NULL) return;
    pool->poisoned = true;
}

/* Nonzero for an address the Media Engine cannot reach. Takes an integer so
   the check is exercisable on a host whose real pointers are all "high". */
static inline bool psp_media_me_invisible(uintptr_t address)
{
    return address != 0
        && address >= (uintptr_t) PSP_MEDIA_ME_VISIBLE_LIMIT;
}

static inline unsigned psp_media_me_invisible_count(
    const void *const *addresses, size_t count)
{
    unsigned high = 0;
    if (addresses == NULL) return 0;
    for (size_t index = 0; index < count; index++)
        if (psp_media_me_invisible((uintptr_t) addresses[index])) high++;
    return high;
}

#endif
