/*
 * The graphics engine draws the video rectangle.
 *
 * This is the only file in the project that uses the GU. Web pages stay on
 * the software rasterizer without exception -- that rasterizer is the
 * fidelity oracle, and every committed floor is a measurement of it. A
 * decoded video frame is live content that appears in no golden and no
 * manifest, so it is the one surface whose pixels the hardware may produce.
 *
 * What the GE buys is the filter. Bilinear costs the main CPU nothing here
 * and cost it everything in software; the owner measured both on real frames
 * and chose bilinear as the default. The software scaler stays as the "Sharp"
 * option and as this file's fallback.
 *
 * Everything here is 32-bit, at both ends, and that is the resolution of a
 * problem this presenter was latched off for.
 *
 * Drawing into the 16-bit page framebuffer also forced a colour conversion.
 * PSP 5650 orders red, green, blue from the least-significant field upward;
 * the old host-style RGB565 packer reversed the outer fields and made a raw
 * framebuffer capture repeat the same mistake while the LCD showed it. The
 * shared target-aware packer now closes that contract for the software path.
 *
 * PMPlayer's robust fast path is still to remove the conversion entirely:
 * it runs its display in 8888 for the duration of playback, textures the
 * converted surface as GU_PSM_8888, draws into an 8888 buffer and scans out
 * 8888. Bytes pass through uninterpreted from the decoder to the panel, so
 * which byte holds which colour is never asked. Thousands of hardware users
 * see correct colour that way. This file adopts the same shape, scoped to
 * fullscreen video: src/psp_display.c owns the scanout format and switches it
 * only while a decoded frame owns the whole screen.
 *
 * Three device facts still shape the code:
 *
 *  - The decoded surface is written by the Media Engine and then
 *    cache-invalidated by src/media_backend_psp.c, which leaves the pixels in
 *    physical RAM with no dirty CPU line anywhere over them. The GE reads
 *    physical RAM. That is exactly the coherence the texture unit needs, and
 *    it is why nothing here writes back the source.
 *
 *  - The destination is the browser's own video back buffer, composed through
 *    the *cached* EDRAM alias. A GE write and a dirty CPU line over the same
 *    address is a race the CPU wins at publish time, silently replacing the
 *    frame with whatever the cache still held. The draw is therefore bracketed
 *    by a writeback-invalidate before and an invalidate after, over the video
 *    rectangle's whole rows.
 *
 *  - libpspgu calls no cache function of any kind (`psp-nm` on libpspgu.a
 *    reports no sceKernelDcache* import). A display list built through the
 *    ordinary cached alias would therefore sit in the CPU's cache while the
 *    GE read stale memory. This file builds its list through the uncached
 *    alias so the question cannot arise.
 */

#include "tilefinch/psp_media_present.h"
#include "tilefinch/psp_threads.h"
#include "tilefinch/psp_media_scale.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#if defined(__PSP__)

/* PSPSDK's enumerators exceed int, which -Wpedantic reports before C23. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include <pspdmac.h>
#include <pspge.h>
#include <pspgu.h>
#include <pspkernel.h>
#pragma GCC diagnostic pop

#include <stdatomic.h>

/*
 * 4 KiB of .bss, claimed once for the process and only when a video frame is
 * actually presented. One frame's list is a few dozen state commands, two
 * texture bindings and eighty bytes of vertices -- under a kilobyte -- so
 * this is roughly four times what the worst frame needs.
 */
#define PSP_MEDIA_PRESENT_GE_LIST_BYTES 4096u
/* Scanout rows are padded to 512 pixels; see include/tilefinch/psp_display.h. */
#define PSP_MEDIA_PRESENT_VRAM_STRIDE 512
#define PSP_MEDIA_PRESENT_EDRAM_BYTES 0x00200000u
#define PSP_MEDIA_PRESENT_UNCACHED 0x40000000u
#define PSP_MEDIA_PRESENT_PHYSICAL_MASK 0x1fffffffu
/* The three colour bytes. The fourth is the primitive's under GU_TCC_RGB and
   the panel ignores it in 8888, so no comparison here may include it. */
#define PSP_MEDIA_PRESENT_RGB_MASK UINT32_C(0x00ffffff)

static unsigned int __attribute__((aligned(64)))
    psp_media_present_ge_list[PSP_MEDIA_PRESENT_GE_LIST_BYTES / 4u];

/* Texture coordinates first, then position: the order sceGuDrawArray expects
   for GU_TEXTURE_32BITF | GU_VERTEX_32BITF. */
typedef struct {
    float u;
    float v;
    float x;
    float y;
    float z;
} PspMediaPresentGeVertex;

/* 0 before the first attempt, 1 once the context exists, -1 once latched off
   for the rest of the process. */
static int psp_media_present_ge_state;
static const char *psp_media_present_ge_failure;
/* Whether the passthrough question has been asked at all, which is different
   from the answer: a presenter that has never been checked must not be used,
   and one that has been checked must not be checked again. */
static bool psp_media_present_ge_checked;
/* What the engine wrote for the passthrough pixel and what it was asked to
   copy, so a mismatch reports both values rather than only that there was
   one. */
static uint32_t psp_media_present_ge_passthrough_drawn;
static uint32_t psp_media_present_ge_passthrough_source;
/* And where each source byte went, per world, so the log carries the
   mechanism and not only the symptom. */
static uint32_t psp_media_present_ge_map_engine[
    PSP_MEDIA_PRESENT_CHANNEL_PROBES];
static uint16_t psp_media_present_ge_map_scaler[
    PSP_MEDIA_PRESENT_CHANNEL_PROBES];

void psp_media_present_ge_passthrough(uint32_t *drawn, uint32_t *source)
{
    if (drawn != NULL) *drawn = psp_media_present_ge_passthrough_drawn;
    if (source != NULL) *source = psp_media_present_ge_passthrough_source;
}

void psp_media_present_ge_channel_map(
    uint32_t engine[PSP_MEDIA_PRESENT_CHANNEL_PROBES],
    uint16_t scaler[PSP_MEDIA_PRESENT_CHANNEL_PROBES])
{
    for (unsigned at = 0; at < PSP_MEDIA_PRESENT_CHANNEL_PROBES; at++) {
        if (engine != NULL) engine[at] = psp_media_present_ge_map_engine[at];
        if (scaler != NULL) scaler[at] = psp_media_present_ge_map_scaler[at];
    }
}

static void psp_media_present_ge_latch(const char *reason)
{
    if (psp_media_present_ge_failure == NULL)
        psp_media_present_ge_failure = reason;
    psp_media_present_ge_state = -1;
}

const char *psp_media_present_ge_reason(void)
{
    return psp_media_present_ge_failure;
}

static void *psp_media_present_ge_uncached_list(void)
{
    return (void *) ((uintptr_t) psp_media_present_ge_list
                     | PSP_MEDIA_PRESENT_UNCACHED);
}

static bool psp_media_present_ge_ready(void)
{
    if (psp_media_present_ge_state != 0)
        return psp_media_present_ge_state > 0;
    /*
     * Lazily, on the first video present, never at boot: a browsing session
     * that opens no video must not pay for a graphics context, and the
     * device-cost baseline is measured on exactly such a session.
     *
     * sceGuInit claims the GE, registers its own completion callback and sets
     * the EDRAM address translation width to the firmware default. It does
     * not touch the display service -- sceGuDispBuffer and sceGuDisplay are
     * the two entry points that would, and neither is called here or
     * anywhere. Scanout stays owned by src/psp_display.c.
     */
    int status = sceGuInit();
    if (status < 0) {
        psp_media_present_ge_latch("sceGuInit");
        return false;
    }
    psp_media_present_ge_state = 1;
    return true;
}

static bool psp_media_present_ge_offset(
    const uint32_t *destination, unsigned *offset)
{
    const void *edram = sceGeEdramGetAddr();
    if (edram == NULL || destination == NULL) return false;
    /* Both EDRAM aliases answer the same physical memory. Compare with the
       cache bit removed so a cached destination and an uncached base still
       subtract to the buffer's position within VRAM, which is what
       sceGuDrawBufferList wants. */
    uintptr_t base = (uintptr_t) edram & PSP_MEDIA_PRESENT_PHYSICAL_MASK;
    uintptr_t target =
        (uintptr_t) destination & PSP_MEDIA_PRESENT_PHYSICAL_MASK;
    if (target < base) return false;
    uintptr_t distance = target - base;
    if (distance >= PSP_MEDIA_PRESENT_EDRAM_BYTES) return false;
    *offset = (unsigned) distance;
    return true;
}

/*
 * Prove, on the machine that will run it, that a source pixel's bytes reach
 * the panel unchanged.
 *
 * This is the question the 16-bit target made unanswerable. There, the two
 * presenters had to agree about a conversion, and they did not; here there is
 * no conversion to disagree about, so the check has an absolute answer rather
 * than a relative one: the bytes that come back are the bytes that went in, or
 * this presenter is not used.
 *
 * The cost is one 16x16 draw and one sync, once per process, into a video
 * buffer that has not been published. It scribbles the top-left corner, which
 * the caller then repaints: a present always covers the whole panel between
 * its video rectangle and its bands.
 */
#define PSP_MEDIA_PRESENT_AGREE_EDGE 16
/* Little-endian R,G,B,A. Saturated and asymmetric, so an exchanged pair
   cannot look like a copy. */
#define PSP_MEDIA_PRESENT_AGREE_PIXEL 0xff0000ffu

static uint32_t __attribute__((aligned(64))) psp_media_present_agree_source[
    PSP_MEDIA_PRESENT_AGREE_EDGE * PSP_MEDIA_PRESENT_AGREE_EDGE];

static bool psp_media_present_ge_draw_plan(
    const PspMediaPresentPlan *plan, const PspMediaPresentTexture *texture,
    uint32_t *destination, PspMediaPresentGeCost *cost);

/*
 * Draw one flat 16x16 source pixel through the presenter and read back what
 * landed. The whole comparison rests on this being the same call the presenter
 * makes, so it goes through psp_media_present_ge_draw_plan() rather than a
 * shortcut.
 */
static bool psp_media_present_ge_draw_probe_pixel(
    uint32_t *destination, uint32_t source, uint32_t *engine)
{
    for (size_t at = 0;
         at < sizeof(psp_media_present_agree_source)
             / sizeof(psp_media_present_agree_source[0]); at++) {
        psp_media_present_agree_source[at] = source;
    }
    sceKernelDcacheWritebackRange(
        psp_media_present_agree_source,
        (unsigned) sizeof(psp_media_present_agree_source));
    PspMediaPresentPlan plan;
    memset(&plan, 0, sizeof(plan));
    plan.video = (PspMediaPresentRect) {
        0, 0, PSP_MEDIA_PRESENT_AGREE_EDGE, PSP_MEDIA_PRESENT_AGREE_EDGE};
    plan.quad_count = 1;
    plan.quads[0] = (PspMediaPresentQuad) {
        .texture_column = 0,
        .texture_width = PSP_MEDIA_PRESENT_AGREE_EDGE,
        .texture_height = PSP_MEDIA_PRESENT_AGREE_EDGE,
        .u0 = 0.0f, .v0 = 0.0f,
        .u1 = (float) PSP_MEDIA_PRESENT_AGREE_EDGE,
        .v1 = (float) PSP_MEDIA_PRESENT_AGREE_EDGE,
        .x0 = 0.0f, .y0 = 0.0f,
        .x1 = (float) PSP_MEDIA_PRESENT_AGREE_EDGE,
        .y1 = (float) PSP_MEDIA_PRESENT_AGREE_EDGE
    };
    PspMediaPresentTexture texture = {
        .pixels = psp_media_present_agree_source,
        .stride_pixels = PSP_MEDIA_PRESENT_AGREE_EDGE,
        .staged = false
    };
    if (!psp_media_present_ge_draw_plan(
            &plan, &texture, destination, NULL)) {
        return false;
    }
    *engine = destination[PSP_MEDIA_PRESENT_VRAM_STRIDE
        + PSP_MEDIA_PRESENT_AGREE_EDGE / 2];
    return true;
}

static bool psp_media_present_ge_copies_bytes(uint32_t *destination)
{
    uint32_t engine = 0;
    if (!psp_media_present_ge_draw_probe_pixel(
            destination, PSP_MEDIA_PRESENT_AGREE_PIXEL, &engine)) {
        return false;
    }
    psp_media_present_ge_passthrough_drawn = engine;
    psp_media_present_ge_passthrough_source = PSP_MEDIA_PRESENT_AGREE_PIXEL;
    return (engine & PSP_MEDIA_PRESENT_RGB_MASK)
        == (PSP_MEDIA_PRESENT_AGREE_PIXEL & PSP_MEDIA_PRESENT_RGB_MASK);
}

/*
 * One saturated byte at a time. Under passthrough each answer must be the
 * probe itself, which is a stronger statement than the 16-bit map's "byte k
 * reached field k" ever was. The scaler column is computed rather than drawn:
 * it is the panel-proven 16-bit mapping the Sharp option and the chrome's own
 * colours are written for, and recording it beside the engine's is what makes
 * expanding a 16-bit overlay into this buffer a documented operation instead
 * of a guess.
 */
static void psp_media_present_ge_measure_channel_map(uint32_t *destination)
{
    static const uint32_t probes[PSP_MEDIA_PRESENT_CHANNEL_PROBES] = {
        UINT32_C(0xff0000f8),  /* byte 0 saturated */
        UINT32_C(0xff00fc00),  /* byte 1 saturated */
        UINT32_C(0xfff80000)   /* byte 2 saturated */
    };
    for (unsigned at = 0; at < PSP_MEDIA_PRESENT_CHANNEL_PROBES; at++) {
        uint32_t engine = 0;
        if (psp_media_present_ge_draw_probe_pixel(
                destination, probes[at], &engine)) {
            psp_media_present_ge_map_engine[at] = engine;
        }
        psp_media_present_ge_map_scaler[at] =
            psp_media_scale_convert_pixel(probes[at]);
    }
}

bool psp_media_present_ge_passthrough_check(uint32_t *destination)
{
    if (destination == NULL) return false;
    if (psp_media_present_ge_checked)
        return psp_media_present_ge_state > 0;
    if (!psp_media_present_ge_ready()) return false;
    psp_media_present_ge_checked = true;
    if (!psp_media_present_ge_copies_bytes(destination)) {
        psp_media_present_ge_latch("passthrough-mismatch");
        return false;
    }
    return true;
}

bool psp_media_present_ge_draw(
    const PspMediaPresentPlan *plan, const PspMediaPresentTexture *texture,
    uint32_t *destination, PspMediaPresentGeCost *cost)
{
    /* The caller proves the passthrough before it commits the panel to the
       video surface, so by the time an ordinary frame is drawn the answer
       already exists. Refuse rather than assume if it does not. */
    if (!psp_media_present_ge_checked) {
        if (!psp_media_present_ge_passthrough_check(destination))
            return false;
    }
    return psp_media_present_ge_draw_plan(plan, texture, destination, cost);
}

/* The rows the submitted draw owns, retained so the completion half can hand
   them back without being told again. */
static void *psp_media_present_ge_pending_rows;
static unsigned psp_media_present_ge_pending_bytes;
static uint64_t psp_media_present_ge_submitted_us;
static bool psp_media_present_ge_pending;

void psp_media_present_ge_stage_flush(const void *pixels, size_t bytes)
{
    if (pixels == NULL || bytes == 0) return;
    sceKernelDcacheWritebackRange((void *) (uintptr_t) pixels,
                                  (unsigned) bytes);
}

bool psp_media_present_ge_stage_dma(
    void *destination, const void *source, size_t bytes)
{
    if (destination == NULL || source == NULL || bytes == 0) return false;
    /* Drop any CPU lines over the destination so a later writeback cannot
       clobber the DMA result. The source is already clean: media_backend_psp
       invalidates the CSC surface after the conversion, so physical RAM holds
       the picture with no dirty line to write back. */
    sceKernelDcacheWritebackInvalidateRange(destination, (unsigned) bytes);
    return sceDmacMemcpy(destination, source, (SceSize) bytes) >= 0;
}

/* ---- The same copy, off the interactive thread --------------------------
 *
 * One worker thread and two event flags realise the async kick the DMAC does
 * not offer in user mode. The interactive thread posts a copy (submit), feeds
 * the decoder while the DMA controller runs it, and collects it (join) just
 * before it starts the list. Only ever one copy in flight, so the payload is
 * plain and the two event-flag syscalls -- set on one side, waited on the
 * other -- order the writes across the threads. The worker runs above the
 * interactive thread (0x20) and below audio, so the kick is picked up at once
 * yet nothing time-critical is starved; it holds the CPU only long enough to
 * enqueue the transfer before sceDmacMemcpy blocks it, leaving the controller
 * to move the bytes in parallel.
 */
#define PSP_MEDIA_DMA_WAKE 1u
#define PSP_MEDIA_DMA_STACK_BYTES (4u * 1024u)
/*
 * How long the interactive thread may wait for a copy it posted.
 *
 * The device measured this copy at 4.2ms at its worst, and the worker runs
 * above the interactive thread, so a healthy join never sees a tenth of this.
 * It exists for the two outcomes that are not a slow copy: a worker that left
 * its loop, and a controller that never answered. Both used to be an
 * unbounded wait on an event flag nothing would ever set -- a browser frozen
 * with no recovery and no line in the log. Ten times the measured worst case
 * is far outside any real copy and far inside a user noticing.
 */
#define PSP_MEDIA_DMA_JOIN_TIMEOUT_US 50000u

/*
 * How long a quarantined transfer is given to be observed finishing before
 * the session is ended instead.
 *
 * Two seconds is forty times the join timeout that already gave up on it, and
 * a transfer of a quarter-megabyte between two memories on the same bus that
 * has not completed in two seconds is not slow; it is a controller that will
 * not complete at all. Waiting longer buys nothing and holds the decoded slot
 * and the staging buffer hostage while the panel shows a frozen picture --
 * whereas ending the session says so, and the retry ladder can restart the
 * whole media stack including the worker.
 */
#define PSP_MEDIA_DMA_QUARANTINE_DEADLINE_US 2000000u

static SceUID psp_media_dma_thread = -1;
static SceUID psp_media_dma_request = -1;
static SceUID psp_media_dma_done = -1;
static bool psp_media_dma_started;
static bool psp_media_dma_in_flight; /* interactive thread only */
static atomic_int psp_media_dma_stop;
/* Latched by whichever side discovers the worker cannot answer: the worker's
   own wait failing, or a join that timed out. Submit consults it, so one
   unanswerable copy costs the session the overlap and nothing else -- every
   caller of submit already performs the copy itself when it refuses. */
static atomic_int psp_media_dma_dead;
static void *psp_media_dma_dst;      /* payload, ordered by the event flags */
static const void *psp_media_dma_src;
static SceSize psp_media_dma_bytes;
/*
 * WHICH picture that source pointer is.
 *
 * The address alone is not enough and this is the site where that stopped
 * being pedantic. The recovery below repeats the copy from psp_media_dma_src
 * on the interactive thread, after a join has already timed out -- so an
 * unknown amount of time has passed, and with two decoded-output slots the
 * surface at that address may by then hold a completely different picture.
 * Repeating the copy would stage it under the claimed picture's identity, and
 * every check downstream would pass. So the submit records the slot and the
 * generation it copied, and the recovery refuses unless the caller still means
 * the same one.
 */
static unsigned psp_media_dma_slot;
static uint32_t psp_media_dma_generation;
/*
 * The transfer nobody could join, and the two addresses it still owns.
 *
 * Interactive-thread state, like psp_media_dma_in_flight: it is written by the
 * join that gave up and read by the frames that follow, all on that one
 * thread. The DMA worker never touches it -- it does not know it has been
 * abandoned, which is exactly the problem this describes.
 */
static bool psp_media_dma_quarantine_live;
static int psp_media_dma_quarantine_slot = -1;
static const void *psp_media_dma_quarantine_dst;
static uint32_t psp_media_dma_quarantine_since_us;
static atomic_uint psp_media_dma_quarantines;
static atomic_uint psp_media_dma_late_completions;
static atomic_uint psp_media_dma_quarantine_max_us;
static atomic_int psp_media_dma_result;
static atomic_uint psp_media_dma_submitted;
static atomic_uint psp_media_dma_completed;
static atomic_uint psp_media_dma_failures;
static atomic_uint psp_media_dma_last_us;
static atomic_uint psp_media_dma_max_us;
static atomic_uint psp_media_dma_timeouts;

static int psp_media_dma_worker_main(SceSize argument_size, void *arguments)
{
    (void) argument_size;
    (void) arguments;
    for (;;) {
        uint32_t bits = 0;
        int wait = sceKernelWaitEventFlag(
            psp_media_dma_request, PSP_MEDIA_DMA_WAKE,
            PSP_EVENT_WAITOR | PSP_EVENT_WAITCLEAR, &bits, NULL);
        if (wait < 0) {
            /*
             * A worker leaving its loop must not look like one that is still
             * about to answer. Latch the death so submit stops posting into a
             * thread that is gone, publish a failed result, and set the done
             * bit so a join already waiting on this copy is released rather
             * than left on an event flag nothing will ever raise again.
             */
            atomic_store_explicit(
                &psp_media_dma_dead, 1, memory_order_release);
            atomic_store_explicit(
                &psp_media_dma_result, -1, memory_order_release);
            atomic_fetch_add_explicit(
                &psp_media_dma_failures, 1u, memory_order_relaxed);
            (void) sceKernelSetEventFlag(
                psp_media_dma_done, PSP_MEDIA_DMA_WAKE);
            break;
        }
        if (atomic_load_explicit(&psp_media_dma_stop, memory_order_acquire))
            break;
        void *dst = psp_media_dma_dst;
        const void *src = psp_media_dma_src;
        SceSize bytes = psp_media_dma_bytes;
        int result = -1;
        uint64_t started_us = (uint64_t) sceKernelGetSystemTimeWide();
        if (dst != NULL && src != NULL && bytes != 0) {
            /* The same discipline as the synchronous copy: drop the CPU lines
               over the destination; the source is already clean. */
            sceKernelDcacheWritebackInvalidateRange(dst, (unsigned) bytes);
            result = sceDmacMemcpy(dst, src, bytes);
        }
        uint32_t elapsed_us =
            (uint32_t) (sceKernelGetSystemTimeWide() - started_us);
        atomic_store_explicit(
            &psp_media_dma_result, result, memory_order_release);
        atomic_store_explicit(
            &psp_media_dma_last_us, elapsed_us, memory_order_relaxed);
        if (result >= 0)
            atomic_fetch_add_explicit(
                &psp_media_dma_completed, 1u, memory_order_relaxed);
        else
            atomic_fetch_add_explicit(
                &psp_media_dma_failures, 1u, memory_order_relaxed);
        uint32_t max = atomic_load_explicit(
            &psp_media_dma_max_us, memory_order_relaxed);
        while (elapsed_us > max
               && !atomic_compare_exchange_weak_explicit(
                   &psp_media_dma_max_us, &max, elapsed_us,
                   memory_order_relaxed, memory_order_relaxed)) {
        }
        (void) sceKernelSetEventFlag(psp_media_dma_done, PSP_MEDIA_DMA_WAKE);
    }
    return 0;
}

static bool psp_media_dma_worker_start(void)
{
    if (psp_media_dma_started) return true;
    atomic_init(&psp_media_dma_stop, 0);
    atomic_init(&psp_media_dma_result, -1);
    atomic_init(&psp_media_dma_submitted, 0);
    atomic_init(&psp_media_dma_completed, 0);
    atomic_init(&psp_media_dma_failures, 0);
    atomic_init(&psp_media_dma_last_us, 0);
    atomic_init(&psp_media_dma_max_us, 0);
    atomic_init(&psp_media_dma_timeouts, 0);
    atomic_init(&psp_media_dma_quarantines, 0);
    atomic_init(&psp_media_dma_late_completions, 0);
    atomic_init(&psp_media_dma_quarantine_max_us, 0);
    psp_media_dma_request = sceKernelCreateEventFlag(
        "tilefinch_dma_req", PSP_EVENT_WAITSINGLE, 0, NULL);
    if (psp_media_dma_request < 0) return false;
    psp_media_dma_done = sceKernelCreateEventFlag(
        "tilefinch_dma_done", PSP_EVENT_WAITSINGLE, 0, NULL);
    if (psp_media_dma_done < 0) {
        (void) sceKernelDeleteEventFlag(psp_media_dma_request);
        psp_media_dma_request = -1;
        return false;
    }
    psp_media_dma_thread = sceKernelCreateThread(
        "tilefinch_dma", psp_media_dma_worker_main,
        TILEFINCH_PSP_THREAD_PRIORITY_DMA,
        PSP_MEDIA_DMA_STACK_BYTES, PSP_THREAD_ATTR_USER, NULL);
    if (psp_media_dma_thread < 0) {
        (void) sceKernelDeleteEventFlag(psp_media_dma_request);
        (void) sceKernelDeleteEventFlag(psp_media_dma_done);
        psp_media_dma_request = -1;
        psp_media_dma_done = -1;
        return false;
    }
    if (sceKernelStartThread(psp_media_dma_thread, 0, NULL) < 0) {
        (void) sceKernelDeleteThread(psp_media_dma_thread);
        (void) sceKernelDeleteEventFlag(psp_media_dma_request);
        (void) sceKernelDeleteEventFlag(psp_media_dma_done);
        psp_media_dma_thread = -1;
        psp_media_dma_request = -1;
        psp_media_dma_done = -1;
        return false;
    }
    psp_media_dma_started = true;
    return true;
}

bool psp_media_present_ge_stage_dma_busy(void)
{
    if (!psp_media_dma_in_flight) return false;
    /* Still running until the worker sets the done bit. Poll, do not wait or
       clear, so the feed loop can stop the instant the copy lands and the join
       that follows still consumes the completion. */
    uint32_t bits = 0;
    int poll = sceKernelPollEventFlag(
        psp_media_dma_done, PSP_MEDIA_DMA_WAKE, PSP_EVENT_WAITOR, &bits);
    return !(poll == 0 && (bits & PSP_MEDIA_DMA_WAKE) != 0);
}

PspMediaPresentDmaJoin psp_media_present_ge_stage_dma_join(void)
{
    if (!psp_media_dma_in_flight) return PSP_MEDIA_DMA_JOIN_SUCCESS;
    uint32_t bits = 0;
    SceUInt timeout_us = PSP_MEDIA_DMA_JOIN_TIMEOUT_US;
    int waited = sceKernelWaitEventFlag(
        psp_media_dma_done, PSP_MEDIA_DMA_WAKE,
        PSP_EVENT_WAITOR | PSP_EVENT_WAITCLEAR, &bits, &timeout_us);
    psp_media_dma_in_flight = false;
    if (waited < 0) {
        /*
         * Nobody stopped this transfer. The wait gave up on it, which is a
         * statement about this thread and not about the controller: it may
         * still be reading the decoded slot and writing the staging texture,
         * and there is no call that can cancel it.
         *
         * This used to be reported as an ordinary failure and answered by
         * repeating the copy into the same destination. That is a second
         * writer of a buffer a live controller is writing, and the previous
         * comment here argued it was safe because "whichever writes last
         * writes the same thing" -- true only for as long as the destination
         * holds THIS picture. It does not: the staging buffer is shared, the
         * next picture is staged into it a frame later, and the abandoned
         * transfer then lands on top of that newer picture after its
         * signature has already been taken. Right identity, previous pixels,
         * and provably invisible to the one check built to catch it.
         *
         * So both addresses are quarantined until the transfer is OBSERVED to
         * have ended. The worker death is still latched, which is what makes
         * the observation sound: no further copy is posted, so the next done
         * event can only be this one.
         */
        atomic_store_explicit(
            &psp_media_dma_dead, 1, memory_order_release);
        atomic_fetch_add_explicit(
            &psp_media_dma_timeouts, 1u, memory_order_relaxed);
        psp_media_dma_quarantine_live = true;
        psp_media_dma_quarantine_slot = (int) psp_media_dma_slot;
        psp_media_dma_quarantine_dst = psp_media_dma_dst;
        psp_media_dma_quarantine_since_us = sceKernelGetSystemTimeLow();
        atomic_fetch_add_explicit(
            &psp_media_dma_quarantines, 1u, memory_order_relaxed);
        return PSP_MEDIA_DMA_JOIN_TIMED_OUT_STILL_LIVE;
    }
    /* The worker answered, so the controller is finished with both addresses
       whatever it reported. A refused copy is the caller's to repeat. */
    return atomic_load_explicit(
               &psp_media_dma_result, memory_order_acquire) >= 0
        ? PSP_MEDIA_DMA_JOIN_SUCCESS
        : PSP_MEDIA_DMA_JOIN_COMPLETED_FAILURE;
}

bool psp_media_present_ge_stage_dma_quarantined(void)
{
    return psp_media_dma_quarantine_live;
}

bool psp_media_present_ge_stage_dma_quarantine_holds(int slot)
{
    return psp_media_dma_quarantine_live && slot >= 0
        && slot == psp_media_dma_quarantine_slot;
}

bool psp_media_present_ge_stage_dma_quarantine_holds_staging(
    const void *destination)
{
    return psp_media_dma_quarantine_live && destination != NULL
        && destination == psp_media_dma_quarantine_dst;
}

bool psp_media_present_ge_stage_dma_quarantine_poll(void)
{
    if (!psp_media_dma_quarantine_live) return false;
    /* Poll, never wait: this runs on frames that have their own work to do,
       and the whole point of the quarantine is that nothing has to block on a
       transfer nobody can stop. */
    uint32_t bits = 0;
    int poll = sceKernelPollEventFlag(
        psp_media_dma_done, PSP_MEDIA_DMA_WAKE, PSP_EVENT_WAITOR, &bits);
    if (poll != 0 || (bits & PSP_MEDIA_DMA_WAKE) == 0) return false;
    (void) sceKernelClearEventFlag(psp_media_dma_done, ~PSP_MEDIA_DMA_WAKE);
    uint32_t held_us =
        sceKernelGetSystemTimeLow() - psp_media_dma_quarantine_since_us;
    if (held_us > atomic_load_explicit(
            &psp_media_dma_quarantine_max_us, memory_order_relaxed)) {
        atomic_store_explicit(
            &psp_media_dma_quarantine_max_us, held_us, memory_order_relaxed);
    }
    psp_media_dma_quarantine_live = false;
    psp_media_dma_quarantine_slot = -1;
    psp_media_dma_quarantine_dst = NULL;
    atomic_fetch_add_explicit(
        &psp_media_dma_late_completions, 1u, memory_order_relaxed);
    return true;
}

bool psp_media_present_ge_stage_dma_quarantine_expired(void)
{
    if (!psp_media_dma_quarantine_live) return false;
    return sceKernelGetSystemTimeLow() - psp_media_dma_quarantine_since_us
        >= PSP_MEDIA_DMA_QUARANTINE_DEADLINE_US;
}

bool psp_media_present_ge_stage_dma_recover(
    int slot, uint32_t generation)
{
    /* The payload of the copy that failed, repeated on this thread. Only a
       source whose rows are already as wide as the texture is ever posted, so
       the transfer is linear and the CPU can finish what the controller
       could not. */
    void *destination = psp_media_dma_dst;
    const void *source = psp_media_dma_src;
    size_t bytes = (size_t) psp_media_dma_bytes;
    if (destination == NULL || source == NULL || bytes == 0) return false;
    /* Not this picture's copy. Refusing is the whole point: the caller answers
       a false by forgetting the staged identity and staging again next frame,
       which costs one frame, where copying anyway would put another picture's
       pixels on the screen under this one's name and cost nothing visible at
       all. */
    if (slot < 0 || (unsigned) slot != psp_media_dma_slot
        || generation != psp_media_dma_generation) return false;
    if (psp_media_present_ge_stage_dma(destination, source, bytes))
        return true;
    memcpy(destination, source, bytes);
    psp_media_present_ge_stage_flush(destination, bytes);
    return true;
}

bool psp_media_present_ge_stage_dma_submit(
    void *destination, const void *source, size_t bytes,
    unsigned slot, uint32_t generation)
{
    if (destination == NULL || source == NULL || bytes == 0) return false;
    if (atomic_load_explicit(&psp_media_dma_dead, memory_order_acquire))
        return false;
    if (!psp_media_dma_worker_start()) return false;
    /* Never two copies into the destination at once. The paired flow has
       already joined by here; this only guards a misuse. */
    if (psp_media_dma_in_flight)
        (void) psp_media_present_ge_stage_dma_join();
    /* A stale completion cannot outlive its join (join clears it, and so does
       the guard above), but clear the done bit before arming so a lost wakeup
       can never be read as this copy's completion. */
    (void) sceKernelClearEventFlag(psp_media_dma_done, ~PSP_MEDIA_DMA_WAKE);
    psp_media_dma_dst = destination;
    psp_media_dma_src = source;
    psp_media_dma_bytes = (SceSize) bytes;
    psp_media_dma_slot = slot;
    psp_media_dma_generation = generation;
    psp_media_dma_in_flight = true;
    atomic_fetch_add_explicit(
        &psp_media_dma_submitted, 1u, memory_order_relaxed);
    if (sceKernelSetEventFlag(psp_media_dma_request, PSP_MEDIA_DMA_WAKE) < 0) {
        psp_media_dma_in_flight = false;
        return false; /* the caller performs the synchronous copy */
    }
    return true;
}

void psp_media_present_ge_stage_dma_stats(PspMediaPresentDmaStats *stats)
{
    if (stats == NULL) return;
    stats->submitted = atomic_load_explicit(
        &psp_media_dma_submitted, memory_order_relaxed);
    stats->completed = atomic_load_explicit(
        &psp_media_dma_completed, memory_order_relaxed);
    stats->failures = atomic_load_explicit(
        &psp_media_dma_failures, memory_order_relaxed);
    stats->last_copy_us = atomic_load_explicit(
        &psp_media_dma_last_us, memory_order_relaxed);
    stats->max_copy_us = atomic_load_explicit(
        &psp_media_dma_max_us, memory_order_relaxed);
    stats->timeouts = atomic_load_explicit(
        &psp_media_dma_timeouts, memory_order_relaxed);
    stats->quarantines = atomic_load_explicit(
        &psp_media_dma_quarantines, memory_order_relaxed);
    stats->late_completions = atomic_load_explicit(
        &psp_media_dma_late_completions, memory_order_relaxed);
    stats->quarantine_max_us = atomic_load_explicit(
        &psp_media_dma_quarantine_max_us, memory_order_relaxed);
}

bool psp_media_present_ge_drawing(void)
{
    if (!psp_media_present_ge_pending) return false;
    /* Non-blocking: a list still running answers non-zero. */
    return sceGuSync(GU_SYNC_FINISH, GU_SYNC_NOWAIT) != 0;
}

bool psp_media_present_ge_complete(PspMediaPresentGeCost *cost)
{
    if (!psp_media_present_ge_pending) return false;
    psp_media_present_ge_pending = false;
    uint64_t entered_us = (uint64_t) sceKernelGetSystemTimeWide();
    /*
     * The wait, and the whole cost model of this presenter. Whatever ran
     * between the submit and here did not touch these rows; from this line
     * the CPU owns them again.
     */
    int synced = sceGuSync(GU_SYNC_FINISH, GU_SYNC_WAIT);
    uint64_t finished_us = (uint64_t) sceKernelGetSystemTimeWide();
    if (cost != NULL) {
        cost->sync_us = finished_us > psp_media_present_ge_submitted_us
            ? finished_us - psp_media_present_ge_submitted_us : 0;
        cost->wait_us = finished_us > entered_us
            ? finished_us - entered_us : 0;
    }
    if (synced < 0) {
        psp_media_present_ge_latch("sceGuSync");
        return false;
    }
    /* The GE wrote physical RAM. Drop the stale clean lines so the compositor
       reads the frame it just drew, and so the publish writeback has nothing
       older to push over it. */
    sceKernelDcacheInvalidateRange(
        psp_media_present_ge_pending_rows,
        psp_media_present_ge_pending_bytes);
    return true;
}

static bool psp_media_present_ge_draw_plan(
    const PspMediaPresentPlan *plan, const PspMediaPresentTexture *texture,
    uint32_t *destination, PspMediaPresentGeCost *cost)
{
    if (!psp_media_present_ge_submit(plan, texture, destination, cost))
        return false;
    return psp_media_present_ge_complete(cost);
}

bool psp_media_present_ge_submit(
    const PspMediaPresentPlan *plan, const PspMediaPresentTexture *texture,
    uint32_t *destination, PspMediaPresentGeCost *cost)
{
    if (cost != NULL) {
        cost->submit_us = 0;
        cost->sync_us = 0;
        cost->wait_us = 0;
    }
    if (plan == NULL || texture == NULL || texture->pixels == NULL
        || destination == NULL || plan->quad_count == 0
        || texture->stride_pixels <= 0) return false;
    if (!psp_media_present_ge_ready()) return false;
    unsigned offset = 0;
    if (!psp_media_present_ge_offset(destination, &offset)) {
        psp_media_present_ge_latch("destination-outside-edram");
        return false;
    }

    /*
     * Retire every dirty line over the rows the GE is about to write. What is
     * pushed out is the previous frame's content, which the draw then
     * replaces; what matters is that nothing is left behind to be pushed out
     * *after* the draw. A row of 32-bit scanout is 2,048 bytes, so a
     * row-aligned range is automatically cache-line aligned and no
     * neighbouring line is disturbed.
     */
    const PspMediaPresentRect *video = &plan->video;
    void *rows = destination
        + (size_t) video->y * (size_t) PSP_MEDIA_PRESENT_VRAM_STRIDE;
    unsigned row_bytes = (unsigned) video->height
        * (unsigned) PSP_MEDIA_PRESENT_VRAM_STRIDE * 4u;
    sceKernelDcacheWritebackInvalidateRange(rows, row_bytes);

    uint64_t started_us = (uint64_t) sceKernelGetSystemTimeWide();
    sceGuStart(GU_DIRECT, psp_media_present_ge_uncached_list());
    /* 8888 in, 8888 out. The one line this whole change exists for: with the
       target's format equal to the texture's, the rasterizer moves bytes and
       names no channel. */
    sceGuDrawBufferList(
        GU_PSM_8888, (void *) (uintptr_t) offset,
        PSP_MEDIA_PRESENT_VRAM_STRIDE);
    sceGuOffset(
        2048u - (unsigned) (PSP_MEDIA_PRESENT_SCREEN_WIDTH / 2),
        2048u - (unsigned) (PSP_MEDIA_PRESENT_SCREEN_HEIGHT / 2));
    sceGuViewport(
        2048, 2048,
        PSP_MEDIA_PRESENT_SCREEN_WIDTH, PSP_MEDIA_PRESENT_SCREEN_HEIGHT);
    /* Whole panel: the letterbox bands are memset by the caller *after* this
       draw completes, so they repair any edge the rasterizer rounds outward
       and the scissor does not have to be the guarantee. */
    sceGuScissor(
        0, 0,
        PSP_MEDIA_PRESENT_SCREEN_WIDTH, PSP_MEDIA_PRESENT_SCREEN_HEIGHT);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDisable(GU_DEPTH_TEST);
    /* No depth buffer is allocated, so depth writes must be off as well as
       depth testing. */
    sceGuDepthMask(GU_TRUE);
    sceGuDisable(GU_BLEND);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDisable(GU_LIGHTING);
    sceGuDisable(GU_CULL_FACE);
    sceGuDisable(GU_CLIP_PLANES);
    sceGuDisable(GU_DITHER);
    sceGuShadeModel(GU_FLAT);
    sceGuEnable(GU_TEXTURE_2D);
    /*
     * Always linear. Both the staged EDRAM texture and the main-RAM source
     * surface are linear 8888; what makes the draw fast is the staged one
     * living in EDRAM, not any block reorder. A device probe measured a linear
     * EDRAM texture at 6.8ms against a swizzled one at 7.0ms -- identical -- so
     * the swizzle the earlier version applied was pure per-picture overhead and
     * is gone.
     */
    sceGuTexMode(GU_PSM_8888, 0, 0, GU_FALSE);
    /* The decoder's alpha byte means nothing on an opaque panel. */
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    /* Unit scale with GU_TRANSFORM_2D means the vertex u/v below are texels,
       which is what the plan computes. */
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);

    bool submitted = true;
    for (size_t at = 0; at < plan->quad_count; at++) {
        const PspMediaPresentQuad *quad = &plan->quads[at];
        /* Every linear layout advances to the quad's aligned source column.
           The ordinary retained stage has one quad at column zero; the wide
           strip stage keeps the decoder's 768-pixel pitch and therefore has
           the same 512+remainder column origins as the source surface. */
        const unsigned char *texels = (const unsigned char *) texture->pixels
            + (size_t) quad->texture_column * 4u;
        sceGuTexImage(
            0, quad->texture_width, quad->texture_height,
            texture->stride_pixels, texels);
        /* A different picture arrives every frame, and with two decoded-output
           slots it does not even arrive at a consistent address: consecutive
           pictures alternate between two surfaces, and the staged texture is a
           third. Flush unconditionally -- there is no address the cache can be
           assumed to still be right about. */
        sceGuTexFlush();
        PspMediaPresentGeVertex *vertices =
            sceGuGetMemory(2 * (int) sizeof(*vertices));
        if (vertices == NULL) {
            submitted = false;
            break;
        }
        vertices[0] = (PspMediaPresentGeVertex) {
            quad->u0, quad->v0, quad->x0, quad->y0, 0.0f};
        vertices[1] = (PspMediaPresentGeVertex) {
            quad->u1, quad->v1, quad->x1, quad->y1, 0.0f};
        sceGuDrawArray(
            GU_SPRITES,
            GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_2D,
            2, NULL, vertices);
    }
    sceGuFinish();
    uint64_t submitted_us = (uint64_t) sceKernelGetSystemTimeWide();
    if (cost != NULL) cost->submit_us = submitted_us - started_us;
    if (!submitted) {
        /*
         * The list was started and finished, so the engine is running whatever
         * quads did fit -- into these very rows. Returning here without
         * waiting hands the caller a destination the engine still owns: it
         * falls back to the software scaler and writes EDRAM underneath a live
         * draw. Wait for the partial list before giving the rows back, then
         * fail. This costs a frame that was already lost.
         */
        (void) sceGuSync(GU_SYNC_FINISH, GU_SYNC_WAIT);
        psp_media_present_ge_latch("display-list-exhausted");
        return false;
    }
    /*
     * From here the rows are the graphics engine's until
     * psp_media_present_ge_complete() returns. The CPU must not touch a byte
     * it still owns -- but it is free to touch anything else, and on this
     * device the wait is tens of milliseconds the decoder badly needs.
     */
    psp_media_present_ge_pending_rows = rows;
    psp_media_present_ge_pending_bytes = row_bytes;
    psp_media_present_ge_submitted_us = submitted_us;
    psp_media_present_ge_pending = true;
    return true;
}

/*
 * Two saturated primaries and a third for the padding. Under passthrough the
 * value each must become is known exactly -- it is the source pixel -- so
 * unlike the 16-bit probe this file now does assert colour as well as
 * geometry.
 */
#define PSP_MEDIA_PRESENT_PROBE_PICTURE 0xff0000ffu
#define PSP_MEDIA_PRESENT_PROBE_PADDING 0xffff0000u
/* Green on purpose. Red and blue are exactly the pair an exchanged channel
   order maps onto each other, so a picture made of those two cannot tell a
   mirrored draw from a recoloured one. Green occupies the middle byte under
   either reading, which makes the orientation question answerable
   independently of the colour one. */
#define PSP_MEDIA_PRESENT_PROBE_SECOND  0xff00ff00u
#define PSP_MEDIA_PRESENT_PROBE_SENTINEL 0x12345678u

static void psp_media_present_probe_fill(
    uint32_t *surface, int stride, int rows,
    int picture_width, int picture_height, bool split_colours)
{
    for (int y = 0; y < rows; y++) {
        uint32_t *row = surface + (size_t) y * (size_t) stride;
        for (int x = 0; x < stride; x++) {
            if (y >= picture_height || x >= picture_width) {
                row[x] = PSP_MEDIA_PRESENT_PROBE_PADDING;
            } else if (split_colours && x >= picture_width / 2) {
                row[x] = PSP_MEDIA_PRESENT_PROBE_SECOND;
            } else {
                row[x] = PSP_MEDIA_PRESENT_PROBE_PICTURE;
            }
        }
    }
}

static void psp_media_present_probe_poison(
    uint32_t *destination, const PspMediaPresentRect *video)
{
    for (int y = video->y; y < video->y + video->height; y++) {
        uint32_t *row =
            destination + (size_t) y * PSP_MEDIA_PRESENT_VRAM_STRIDE;
        for (int x = 0; x < PSP_MEDIA_PRESENT_SCREEN_WIDTH; x++)
            row[x] = PSP_MEDIA_PRESENT_PROBE_SENTINEL;
    }
}

static uint32_t psp_media_present_probe_at(
    const uint32_t *destination, int x, int y)
{
    return destination[(size_t) y * PSP_MEDIA_PRESENT_VRAM_STRIDE + x]
        & PSP_MEDIA_PRESENT_RGB_MASK;
}

/*
 * The same output rectangle, drawn from a texture small enough to stay in the
 * texture cache. Its wait is the bilinear filter over every output pixel plus
 * the 8888 framebuffer write, with the texture-read bandwidth removed -- so
 * the caller's real draw minus this isolates what reading the real texture out
 * of main memory cost. The panel is left scribbled, which the probe's next
 * poison-and-draw repairs.
 */
static uint64_t psp_media_present_probe_output_sync(
    uint32_t *destination, const PspMediaPresentRect *video)
{
    for (size_t at = 0;
         at < sizeof(psp_media_present_agree_source)
             / sizeof(psp_media_present_agree_source[0]); at++) {
        psp_media_present_agree_source[at] = PSP_MEDIA_PRESENT_PROBE_PICTURE;
    }
    sceKernelDcacheWritebackRange(
        psp_media_present_agree_source,
        (unsigned) sizeof(psp_media_present_agree_source));
    PspMediaPresentPlan plan;
    memset(&plan, 0, sizeof(plan));
    plan.video = *video;
    plan.quad_count = 1;
    plan.quads[0] = (PspMediaPresentQuad) {
        .texture_column = 0,
        .texture_width = PSP_MEDIA_PRESENT_AGREE_EDGE,
        .texture_height = PSP_MEDIA_PRESENT_AGREE_EDGE,
        .u0 = 0.0f, .v0 = 0.0f,
        .u1 = (float) PSP_MEDIA_PRESENT_AGREE_EDGE,
        .v1 = (float) PSP_MEDIA_PRESENT_AGREE_EDGE,
        .x0 = (float) video->x, .y0 = (float) video->y,
        .x1 = (float) (video->x + video->width),
        .y1 = (float) (video->y + video->height)
    };
    PspMediaPresentTexture texture = {
        .pixels = psp_media_present_agree_source,
        .stride_pixels = PSP_MEDIA_PRESENT_AGREE_EDGE,
        .staged = false
    };
    PspMediaPresentGeCost cost = {0, 0, 0};
    if (!psp_media_present_ge_draw_plan(
            &plan, &texture, destination, &cost)) {
        return 0;
    }
    return cost.sync_us;
}

/* Stage the probe's synthetic surface into the EDRAM texture the same way
   playback does -- the DMA controller when contiguous, the CPU otherwise -- so
   the probe certifies the shipped staging mechanism's cache discipline, not a
   substitute. The surface has been written back by the caller, so physical RAM
   holds it. */
static void psp_media_present_probe_stage(
    uint32_t *staging, const uint32_t *surface, int stride,
    int texture_width, int rows, unsigned slot, uint32_t generation)
{
    size_t bytes = psp_media_present_stage_bytes(texture_width, rows);
    /* Stage through the shipped async worker so the probe certifies its cache
       discipline and copy, not a stand-in: submit then join at once, no feed
       between, so the picture is whole before the draw samples it. A worker
       that could not start, or a strided case, falls back exactly as the
       present does. */
    if (stride == texture_width
        && psp_media_present_ge_stage_dma_submit(
               staging, surface, bytes, slot, generation)) {
        (void) psp_media_present_ge_stage_dma_join();
        return;
    }
    if (stride == texture_width
        && psp_media_present_ge_stage_dma(staging, surface, bytes)) {
        return;
    }
    psp_media_present_stage(staging, surface, stride, texture_width, rows);
    sceKernelDcacheWritebackRange(staging, (unsigned) bytes);
}

/* The wide shipping path must finish each strip before its draw and therefore
   uses the bounded synchronous DMAC call. Keep that distinction in the probe:
   certifying the async whole-frame worker would not certify the path a 360p
   picture actually takes. */
static void psp_media_present_probe_stage_strip(
    uint32_t *staging, const uint32_t *surface, int stride,
    int texture_width, int rows)
{
    size_t bytes = psp_media_present_stage_bytes(texture_width, rows);
    if (stride == texture_width
        && psp_media_present_ge_stage_dma(staging, surface, bytes)) {
        return;
    }
    psp_media_present_stage(staging, surface, stride, texture_width, rows);
    sceKernelDcacheWritebackRange(staging, (unsigned) bytes);
}

/* Draw exactly the layout playback will use for this case. Narrow pictures
   keep the retained one-copy stage. The exact 360p geometry is copied and
   drawn in the two guarded EDRAM strips produced by the pure planner. */
static bool psp_media_present_probe_draw(
    const PspMediaPresentPlan *plan,
    const PspMediaPresentStripPlan *strips,
    uint32_t *surface, int stride, uint32_t *staging,
    uint32_t *destination, unsigned slot, uint32_t generation,
    bool retained_stage, int retained_rows,
    PspMediaPresentGeCost *cost)
{
    if (cost != NULL) *cost = (PspMediaPresentGeCost) {0, 0, 0};
    if (strips != NULL && strips->strip_count != 0) {
        PspMediaPresentGeCost total = {0, 0, 0};
        for (size_t at = 0; at < strips->strip_count; at++) {
            const PspMediaPresentStrip *strip = &strips->strips[at];
            const uint32_t *source = surface
                + (size_t) strip->source_row * (size_t) stride;
            psp_media_present_probe_stage_strip(
                staging, source, stride, stride, strip->copy_rows);
            PspMediaPresentTexture texture = {
                .pixels = staging, .stride_pixels = stride, .staged = true
            };
            PspMediaPresentGeCost pass = {0, 0, 0};
            if (!psp_media_present_ge_draw_plan(
                    &strip->draw, &texture, destination, &pass))
                return false;
            total.submit_us += pass.submit_us;
            total.sync_us += pass.sync_us;
            total.wait_us += pass.wait_us;
        }
        if (cost != NULL) *cost = total;
        return true;
    }

    PspMediaPresentTexture texture = {
        .pixels = surface, .stride_pixels = stride, .staged = false
    };
    if (retained_stage) {
        psp_media_present_probe_stage(
            staging, surface, stride, plan->quads[0].texture_width,
            retained_rows, slot, generation);
        texture.pixels = staging;
        texture.stride_pixels = plan->quads[0].texture_width;
        texture.staged = true;
    }
    return psp_media_present_ge_draw_plan(
        plan, &texture, destination, cost);
}

static bool psp_media_present_probe_case(
    uint32_t *surface, uint32_t *staging, size_t staging_bytes,
    uint32_t *destination,
    int picture_width, int picture_height, int stride, int rows,
    unsigned slot,
    PspMediaPresentProbeCase *report, char *detail, size_t detail_size)
{
    memset(report, 0, sizeof(*report));
    report->slot = slot;
    report->source_width = picture_width;
    report->source_height = picture_height;
    report->source_stride = stride;
    PspMediaPresentPlan plan;
    if (!psp_media_present_plan(
            &plan, picture_width, picture_height, stride,
            PSP_MEDIA_PRESENT_SCREEN_WIDTH,
            PSP_MEDIA_PRESENT_SCREEN_HEIGHT)
        || plan.quad_count == 0) {
        snprintf(detail, detail_size, "%dx%d slot %u: no plan",
                 picture_width, picture_height, slot);
        return false;
    }
    report->output_width = plan.video.width;
    report->output_height = plan.video.height;
    report->quads = (unsigned) plan.quad_count;
    /*
     * The layout the shipping path would choose for this geometry, chosen the
     * same way: a probe that certified the linear texture while playback drew
     * a staged one would be certifying a pipeline nothing runs.
     */
    bool retained_stage = psp_media_present_stage_fits(
        &plan, picture_height, staging_bytes);
    PspMediaPresentStripPlan strips;
    bool strip_staged = psp_media_present_wide_strip_plan(
        &strips, &plan, picture_width, picture_height, stride,
        staging_bytes);
    bool staged = retained_stage || strip_staged;
    report->staged = staged;
    int staged_rows = psp_media_present_stage_rows(picture_height);
    const PspMediaPresentRect *video = &plan.video;
    int centre_y = video->y + video->height / 2;
    int quarter_x = video->x + video->width / 4;
    int three_quarter_x = video->x + (video->width * 3) / 4;
    uint32_t picture = PSP_MEDIA_PRESENT_PROBE_PICTURE
        & PSP_MEDIA_PRESENT_RGB_MASK;
    uint32_t second = PSP_MEDIA_PRESENT_PROBE_SECOND
        & PSP_MEDIA_PRESENT_RGB_MASK;

    /* --- One flat colour, padding poisoned ------------------------------ */
    psp_media_present_probe_fill(
        surface, stride, rows, picture_width, picture_height, false);
    /* The colour conversion normally leaves this surface in RAM with no dirty
       line over it. Here the CPU wrote it, so it must be pushed out before
       the GE reads physical memory. */
    sceKernelDcacheWritebackRange(
        surface, (unsigned) ((size_t) stride * (size_t) rows * 4u));
    /* Poison the destination so "drew the wrong thing" and "drew nothing" are
       different answers rather than the same one. */
    psp_media_present_probe_poison(destination, video);
    PspMediaPresentGeCost cost = {0, 0, 0};
    if (!psp_media_present_probe_draw(
            &plan, strip_staged ? &strips : NULL,
            surface, stride, staging, destination, slot, 1u,
            retained_stage, staged_rows, &cost)) {
        const char *why = psp_media_present_ge_reason();
        snprintf(detail, detail_size, "%dx%d slot %u: draw refused (%s)",
                 picture_width, picture_height, slot,
                 why == NULL ? "?" : why);
        return false;
    }
    /*
     * Every pixel is the source pixel. This one loop now carries what took
     * three checks and a second presenter before: a tap that reached the
     * poisoned padding, a stride or base-address error, a byte reordering and
     * a draw that wrote nothing all fail it, and there is no panel channel
     * order left to be agnostic about.
     */
    for (int y = video->y; y < video->y + video->height; y++) {
        for (int x = video->x; x < video->x + video->width; x++) {
            uint32_t pixel = psp_media_present_probe_at(destination, x, y);
            if (pixel != picture) {
                snprintf(detail, detail_size,
                         "%dx%d slot %u: pixel %d,%d is 0x%06x not the "
                         "source 0x%06x",
                         picture_width, picture_height, slot, x, y,
                         (unsigned) pixel, (unsigned) picture);
                return false;
            }
        }
    }

    /* --- Two colours, so orientation and the split are visible ---------- */
    psp_media_present_probe_fill(
        surface, stride, rows, picture_width, picture_height, true);
    sceKernelDcacheWritebackRange(
        surface, (unsigned) ((size_t) stride * (size_t) rows * 4u));
    psp_media_present_probe_poison(destination, video);
    if (!psp_media_present_probe_draw(
            &plan, strip_staged ? &strips : NULL,
            surface, stride, staging, destination, slot, 4u,
            retained_stage, staged_rows, &cost)) {
        snprintf(detail, detail_size, "%dx%d slot %u: second draw refused",
                 picture_width, picture_height, slot);
        return false;
    }
    uint32_t left = psp_media_present_probe_at(
        destination, quarter_x, centre_y);
    uint32_t right = psp_media_present_probe_at(
        destination, three_quarter_x, centre_y);
    /* Absolute, not relative: the left quarter is the first colour and the
       right quarter is the second. A mirrored draw swaps them, a sheared one
       moves the split, and either fails. */
    /* The output-only reference, drawn last so it does not sit between the two
       checked draws above. It scribbles the rectangle, which the caller's next
       case re-poisons, and the final case's is simply overwritten by the
       chrome the caller composites. */
    report->output_sync_us =
        psp_media_present_probe_output_sync(destination, video);
    report->flat = picture;
    report->left = left;
    report->right = right;
    report->submit_us = cost.submit_us;
    report->sync_us = cost.sync_us;
    if (left != picture || right != second) {
        snprintf(detail, detail_size,
                 "%dx%d slot %u: halves 0x%06x/0x%06x, expected "
                 "0x%06x/0x%06x",
                 picture_width, picture_height, slot,
                 (unsigned) left, (unsigned) right,
                 (unsigned) picture, (unsigned) second);
        return false;
    }
    report->passed = true;
    return true;
}

bool psp_media_present_ge_probe(
    void *scratch, size_t scratch_bytes, uint32_t *staging,
    size_t staging_bytes, uint32_t *destination,
    PspMediaPresentProbeCase cases[PSP_MEDIA_PRESENT_PROBE_CASES],
    char *detail, size_t detail_size)
{
    if (detail != NULL && detail_size != 0) detail[0] = '\0';
    if (cases != NULL)
        memset(cases, 0, sizeof(*cases) * PSP_MEDIA_PRESENT_PROBE_CASES);
    if (scratch == NULL || destination == NULL || detail == NULL
        || detail_size == 0 || cases == NULL || staging == NULL
        || scratch_bytes < PSP_MEDIA_PRESENT_PROBE_SCRATCH_BYTES) {
        if (detail != NULL && detail_size != 0)
            snprintf(detail, detail_size, "probe arguments unusable");
        return false;
    }
    /* Ask the passthrough question first and record its answer, then clear the
       latch it may have set: a mismatch must not stop this probe from
       reporting whether the geometry is right, which is the more expensive
       thing to get wrong and the harder thing to discover. */
    bool copied = psp_media_present_ge_passthrough_check(destination);
    uint32_t drawn = 0;
    uint32_t source_pixel = 0;
    psp_media_present_ge_passthrough(&drawn, &source_pixel);
    if (psp_media_present_ge_state < 0
        && psp_media_present_ge_failure != NULL
        && strcmp(psp_media_present_ge_failure,
                  "passthrough-mismatch") == 0) {
        psp_media_present_ge_state = 1;
        psp_media_present_ge_failure = NULL;
    }
    /* Measured here, reported by the caller: this translation unit's printf
       goes to the PSP console, which nothing collects, while the caller's is
       the validation log the truth cycle actually reads. */
    if (psp_media_present_ge_state > 0)
        psp_media_present_ge_measure_channel_map(destination);
    /*
     * The two shipping surface geometries -- the 240p stream inside its
     * 512-pixel stride, and the 360p stream whose 640 columns exceed one
     * texture and must be drawn as two quads -- from each decoded-output slot.
     *
     * Both slots, because playback draws from both. A single-surface probe
     * certifies one texture base address and says nothing about the other, and
     * "it worked from slot 0" is exactly the report a mis-slotted draw would
     * also produce. Stops at the first failure so `detail` names it.
     */
    bool passed = true;
    for (unsigned slot = 0;
         passed && slot < PSP_MEDIA_PRESENT_PROBE_SLOTS; slot++) {
        uint32_t *surface = (uint32_t *) ((unsigned char *) scratch
            + (size_t) slot * PSP_MEDIA_PRESENT_PROBE_SURFACE_BYTES);
        passed = psp_media_present_probe_case(
            surface, staging, staging_bytes, destination, 426, 240, 512, 272,
            slot, &cases[slot * PSP_MEDIA_PRESENT_PROBE_GEOMETRIES],
            detail, detail_size);
        if (!passed) break;
        passed = psp_media_present_probe_case(
            surface, staging, staging_bytes, destination, 640, 360, 768, 368,
            slot, &cases[slot * PSP_MEDIA_PRESENT_PROBE_GEOMETRIES + 1u],
            detail, detail_size);
    }
    if (passed && !copied) {
        snprintf(detail, detail_size,
                 "geometry passed; the engine wrote 0x%08x for source "
                 "0x%08x", (unsigned) drawn, (unsigned) source_pixel);
        return false;
    }
    return passed;
}

#else

bool psp_media_present_ge_probe(
    void *scratch, size_t scratch_bytes, uint32_t *staging,
    size_t staging_bytes, uint32_t *destination,
    PspMediaPresentProbeCase cases[PSP_MEDIA_PRESENT_PROBE_CASES],
    char *detail, size_t detail_size)
{
    (void) scratch;
    (void) scratch_bytes;
    (void) staging;
    (void) staging_bytes;
    (void) destination;
    if (cases != NULL)
        memset(cases, 0, sizeof(*cases) * PSP_MEDIA_PRESENT_PROBE_CASES);
    if (detail != NULL && detail_size != 0)
        snprintf(detail, detail_size, "no graphics engine");
    return false;
}

bool psp_media_present_ge_draw(
    const PspMediaPresentPlan *plan, const PspMediaPresentTexture *texture,
    uint32_t *destination, PspMediaPresentGeCost *cost)
{
    (void) plan;
    (void) texture;
    (void) destination;
    if (cost != NULL) {
        cost->submit_us = 0;
        cost->sync_us = 0;
        cost->wait_us = 0;
    }
    return false;
}

bool psp_media_present_ge_submit(
    const PspMediaPresentPlan *plan, const PspMediaPresentTexture *texture,
    uint32_t *destination, PspMediaPresentGeCost *cost)
{
    return psp_media_present_ge_draw(plan, texture, destination, cost);
}

bool psp_media_present_ge_complete(PspMediaPresentGeCost *cost)
{
    (void) cost;
    return false;
}

void psp_media_present_ge_stage_flush(const void *pixels, size_t bytes)
{
    (void) pixels;
    (void) bytes;
}

bool psp_media_present_ge_stage_dma(
    void *destination, const void *source, size_t bytes)
{
    (void) destination;
    (void) source;
    (void) bytes;
    return false;
}

bool psp_media_present_ge_stage_dma_submit(
    void *destination, const void *source, size_t bytes,
    unsigned slot, uint32_t generation)
{
    (void) destination;
    (void) source;
    (void) bytes;
    (void) slot;
    (void) generation;
    return false;
}

PspMediaPresentDmaJoin psp_media_present_ge_stage_dma_join(void)
{
    return PSP_MEDIA_DMA_JOIN_SUCCESS;
}

/* No worker, so nothing is ever posted, so nothing can ever be left live: the
   quarantine is empty by construction here rather than by policy. */
bool psp_media_present_ge_stage_dma_quarantined(void)
{
    return false;
}

bool psp_media_present_ge_stage_dma_quarantine_holds(int slot)
{
    (void) slot;
    return false;
}

bool psp_media_present_ge_stage_dma_quarantine_holds_staging(
    const void *destination)
{
    (void) destination;
    return false;
}

bool psp_media_present_ge_stage_dma_quarantine_poll(void)
{
    return false;
}

bool psp_media_present_ge_stage_dma_quarantine_expired(void)
{
    return false;
}

/* Nothing is ever posted here, so there is never a copy to repeat -- and no
   picture this could be asked to repeat it for. */
bool psp_media_present_ge_stage_dma_recover(
    int slot, uint32_t generation)
{
    (void) slot;
    (void) generation;
    return false;
}

bool psp_media_present_ge_stage_dma_busy(void)
{
    return false;
}

void psp_media_present_ge_stage_dma_stats(PspMediaPresentDmaStats *stats)
{
    if (stats != NULL) memset(stats, 0, sizeof(*stats));
}

bool psp_media_present_ge_drawing(void)
{
    return false;
}

bool psp_media_present_ge_passthrough_check(uint32_t *destination)
{
    (void) destination;
    return false;
}

const char *psp_media_present_ge_reason(void)
{
    return "no-graphics-engine";
}

void psp_media_present_ge_passthrough(uint32_t *drawn, uint32_t *source)
{
    if (drawn != NULL) *drawn = 0;
    if (source != NULL) *source = 0;
}

void psp_media_present_ge_channel_map(
    uint32_t engine[PSP_MEDIA_PRESENT_CHANNEL_PROBES],
    uint16_t scaler[PSP_MEDIA_PRESENT_CHANNEL_PROBES])
{
    for (unsigned at = 0; at < PSP_MEDIA_PRESENT_CHANNEL_PROBES; at++) {
        if (engine != NULL) engine[at] = 0;
        if (scaler != NULL) scaler[at] = 0;
    }
}

#endif
