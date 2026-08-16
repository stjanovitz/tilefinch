#ifndef TILEFINCH_PSP_MEDIA_PRESENT_H
#define TILEFINCH_PSP_MEDIA_PRESENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Where a decoded video frame lands on the panel, and the textured quads that
 * put it there.
 *
 * Two presenters share this file. The software scaler
 * (include/tilefinch/psp_media_scale.h) is the "Sharp" option and the
 * fallback; the graphics engine draws the same rectangle for the default
 * "Smooth" option. Both need the identical destination rectangle and the
 * identical letterbox bands, so the arithmetic lives once, in a host-testable
 * translation unit, rather than twice inside two presenters that could drift.
 *
 * Everything here is pure: no PSP headers, no globals, no allocation. The GE
 * translation unit (src/psp_media_present_ge.c) is the only file that knows
 * what a display list is.
 */

#define PSP_MEDIA_PRESENT_SCREEN_WIDTH 480
#define PSP_MEDIA_PRESENT_SCREEN_HEIGHT 272

/*
 * The graphics engine encodes texture dimensions as log2, so a texture is at
 * most 512 texels on a side and always a power of two. A 640-wide stream
 * therefore cannot be one texture: it is drawn as two quads whose second
 * texture starts part way along the same surface row.
 */
#define PSP_MEDIA_PRESENT_TEXTURE_MAX 512
/*
 * The second quad's texture base is the surface pointer advanced by whole
 * pixels. Keep that advance a multiple of sixteen pixels so the byte address
 * handed to the GE stays 64-byte aligned, comfortably above the 16-byte
 * alignment linear textures require.
 */
#define PSP_MEDIA_PRESENT_COLUMN_ALIGN 16
#define PSP_MEDIA_PRESENT_MAX_QUADS 2
#define PSP_MEDIA_PRESENT_MAX_BANDS 4

typedef struct {
    int x;
    int y;
    int width;
    int height;
} PspMediaPresentRect;

typedef struct {
    /* Texture origin as a column offset in source pixels. Always a multiple
       of PSP_MEDIA_PRESENT_COLUMN_ALIGN. */
    int texture_column;
    /* Declared texture extent: powers of two, at most 512. Deliberately
       allowed to exceed the sampled region (a 426-wide frame is declared 512
       wide) because the GE has no other encoding; the UV range below is what
       keeps the sampler inside the pixels the decoder actually wrote. */
    int texture_width;
    int texture_height;
    /* Texel coordinates at the quad's two corners, relative to
       texture_column. */
    float u0;
    float v0;
    float u1;
    float v1;
    /* Screen coordinates at the quad's two corners. */
    float x0;
    float y0;
    float x1;
    float y1;
} PspMediaPresentQuad;

typedef struct {
    /* The video rectangle, in screen pixels. */
    PspMediaPresentRect video;
    /* Pixels the video does not cover, in the order top, bottom, left,
       right. Empty bands are omitted, so band_count is 0..4. */
    PspMediaPresentRect bands[PSP_MEDIA_PRESENT_MAX_BANDS];
    size_t band_count;
    PspMediaPresentQuad quads[PSP_MEDIA_PRESENT_MAX_QUADS];
    size_t quad_count;
} PspMediaPresentPlan;

/*
 * One pass of the wide-frame EDRAM presenter.
 *
 * A 640x360 picture arrives in a 768-pixel CSC stride. The whole padded
 * surface is 1.08 MiB and cannot coexist with the browser's two video
 * scanout buffers in EDRAM. Exactly 181 padded rows do fit the existing
 * 557,056-byte texture allocation, however, so the wide path copies and
 * draws two half-height strips. Each pass carries one guard row across the
 * boundary so bilinear filtering observes the same two texels an unsplit
 * draw would have used.
 *
 * `source_row` and `copy_rows` describe the contiguous DMA input. `draw` is
 * the destination slice and its two horizontal texture quads, with v
 * coordinates made relative to that copied strip.
 */
typedef struct {
    int source_row;
    int copy_rows;
    PspMediaPresentPlan draw;
} PspMediaPresentStrip;

#define PSP_MEDIA_PRESENT_WIDE_STRIPS 2u

typedef struct {
    PspMediaPresentStrip strips[PSP_MEDIA_PRESENT_WIDE_STRIPS];
    size_t strip_count;
} PspMediaPresentStripPlan;

/*
 * Fit `source_width` x `source_height` into the panel the way the presenter
 * always has -- widest fit that keeps the aspect ratio, centred -- and derive
 * the letterbox bands and the textured quads from that rectangle.
 *
 * The two failure modes are deliberately different. False means no presenter
 * can honour this geometry -- a non-positive size, a stride narrower than the
 * picture, a rectangle that will not fit the panel -- and nothing may be
 * drawn. True with `quad_count == 0` means only the graphics engine cannot
 * express it (taller than one texture, or too wide to split); the rectangle
 * and bands are still exactly right, and the software scaler should present
 * them.
 */
bool psp_media_present_plan(
    PspMediaPresentPlan *plan,
    int source_width, int source_height, int source_stride_pixels,
    int screen_width, int screen_height);

/*
 * Derive the wide-only two-strip presentation described above.
 *
 * This recognises the admitted two-quad 360-row pictures inside the firmware's
 * 768-stride surface. Their aspect ratios may differ, but every one can be
 * divided into two 180-row source strips and two equal destination strips.
 * False means the caller uses its existing presentation path. In particular,
 * the proven 240p geometry never enters this code. `capacity` is the single
 * EDRAM staging allocation available to each pass.
 */
bool psp_media_present_wide_strip_plan(
    PspMediaPresentStripPlan *strips, const PspMediaPresentPlan *full,
    int source_width, int source_height, int source_stride_pixels,
    size_t capacity);

/*
 * True when `quad` samples only texels inside
 * [0, source_width) x [0, source_height) once bilinear filtering has taken
 * its second tap. This is the property the plan exists to guarantee: the
 * macroblock padding beyond the display rectangle is decoder scratch, and the
 * rows past the coded height were never written at all, so a filter tap that
 * reaches them would put uninitialised memory on the panel. Exported because
 * it is the assertion the host tests make, not because a caller needs it.
 */
bool psp_media_present_quad_samples_inside(
    const PspMediaPresentQuad *quad, int source_width, int source_height,
    int quad_source_column);

/* What one graphics-engine present spent, in microseconds. `submit_us` is
   CPU time building the display list; `sync_us` is CPU time blocked waiting
   for the GE to finish, which it must do before the chrome compositor may
   touch the same buffer. */
typedef struct {
    uint64_t submit_us;
    /* Submit to done, which spans whatever the caller ran in between. */
    uint64_t sync_us;
    /*
     * Time actually blocked in sceGuSync, after the caller's own work
     * returned. sync_us stopped measuring the engine the moment a pump was
     * put inside it -- a device cycle read 11.06ms of "sync" that was 8.3ms
     * of decoder feeding -- so this is the number that says whether the
     * engine is still costing anything. Near zero means it finished while the
     * pump ran and is entirely hidden.
     */
    uint64_t wait_us;
} PspMediaPresentGeCost;

/*
 * What the graphics engine samples, and where it lives.
 *
 * Both layouts are linear 8888. What differs is the memory. The decoder's own
 * surface is in main RAM, and drawing a 480x270 bilinear magnification from a
 * 2 KiB-pitch linear texture there measured 47.4ms of wait -- the engine reads
 * four texels at a time out of main memory and the Media Engine decoding out
 * of the same memory slows with it. Copying that surface into EDRAM first,
 * where the engine is fast, brings the same draw to 6.8ms.
 *
 * `staged` says which: true means `pixels` is a copy of the picture placed at
 * the origin of an EDRAM buffer (the shipping 240p case); false means `pixels`
 * is the source surface itself, read where it lies, with the quad's column
 * offset applied (the 360p split, which does not fit one staging buffer). The
 * copy is gated on the picture changing rather than on the present, because it
 * is per-picture overhead -- see psp_media_present_stage. An earlier version
 * reordered the copy into the texture unit's swizzled block layout; a device
 * probe proved that reorder bought nothing once the texture was in EDRAM, so
 * it is now a plain sequential copy.
 */
typedef struct {
    const void *pixels;
    /* Row pitch in pixels for a source-surface texture; the texture's own
       width for a staged one, whose rows are exactly as wide as the texture. */
    int stride_pixels;
    bool staged;
} PspMediaPresentTexture;

/*
 * Copy `texture_width` columns and `rows` rows of a 32-bit linear surface into
 * a contiguous EDRAM texture buffer -- a plain sequential copy, no reorder.
 *
 * Writes `rows * texture_width` words. When `texture_width` equals the source
 * stride, which is the shipping 512 case, it is one memcpy; otherwise one per
 * row. There is no block-alignment requirement, unlike the swizzled layout it
 * replaced.
 */
void psp_media_present_stage(
    void *destination, const void *source, int source_stride_pixels,
    int texture_width, int rows);

/* Rows to copy for a sampler that reaches `height` -- the picture height. */
int psp_media_present_stage_rows(int height);

/* Bytes psp_media_present_stage writes for that geometry. */
size_t psp_media_present_stage_bytes(int texture_width, int rows);

/*
 * True when `plan` can be drawn from one EDRAM staging buffer of `capacity`
 * bytes: one quad anchored at the surface origin, whose texture fits. A split
 * wide-frame plan does not qualify -- its second texture starts part way along
 * the source rows, an offset a single origin-anchored staging buffer cannot
 * hold -- and is drawn from the source surface in main RAM instead.
 */
bool psp_media_present_stage_fits(
    const PspMediaPresentPlan *plan, int source_height, size_t capacity);

/*
 * One 512-pixel-wide surface of staging, which is the shipping geometry: a
 * 426x240 picture inside a 512-pixel stride declares a 512x256 texture and
 * fills 240 rows of it. Sized for the whole 272-row surface so no admitted
 * height can overrun it.
 */
#define PSP_MEDIA_PRESENT_STAGE_BYTES ((size_t) 512 * 272 * 4)

/*
 * Draw the plan's quads into `destination` -- the browser's 32-bit video back
 * buffer, addressed through the cached EDRAM alias -- sampling `source` as an
 * unstaged 32-bit texture at `source_stride_pixels`.
 *
 * Both ends are 8888, which is the whole point: the decoder's bytes are
 * copied, filtered and written without any channel being named, so the byte
 * order the firmware colour converter chose stops mattering. See
 * src/psp_media_present_ge.c.
 *
 * Returns false when the graphics engine is unavailable or refused the work,
 * in which case the caller must leave the video surface and present with the
 * software scaler; the failure is latched for the rest of the process, so a
 * broken GE costs one attempt per session and never one log line per frame.
 *
 * The letterbox bands must be filled by the caller AFTER this returns. See
 * src/psp_media_present_ge.c for why.
 *
 * Device-only. The host build links a stub that always answers false.
 */
bool psp_media_present_ge_draw(
    const PspMediaPresentPlan *plan, const PspMediaPresentTexture *texture,
    uint32_t *destination, PspMediaPresentGeCost *cost);

/*
 * The same draw, in its two halves, so the CPU can do something else while
 * the graphics engine works.
 *
 * The device measured this draw at 46ms a frame against the software
 * scaler's 6-10ms, and essentially all of it is the wait: building the list
 * is microseconds. Blocking there is 46ms per frame in which the decoder is
 * fed nothing, on a pipeline that is already starving -- so the caller
 * submits, feeds the decoder, and only then completes.
 *
 * Between the two calls the destination rows belong to the graphics engine.
 * Nothing may read or write them: not the letterbox bands, not the chrome
 * compositor, not a publish. Whatever runs in between must touch other
 * memory, which the media pump does.
 *
 * `complete` must be called after every successful `submit`, and returns
 * false if the wait failed -- in which case the rows hold no promised frame.
 * `cost` accumulates across the pair: submit fills submit_us, complete fills
 * sync_us.
 */
bool psp_media_present_ge_submit(
    const PspMediaPresentPlan *plan, const PspMediaPresentTexture *texture,
    uint32_t *destination, PspMediaPresentGeCost *cost);
bool psp_media_present_ge_complete(PspMediaPresentGeCost *cost);

/*
 * Whether the submitted list is still running, asked without blocking.
 *
 * The pump that fills the engine's wait must be bounded by that wait and not
 * by a slice of its own: a device cycle ran it 23 units and 15.1ms deep inside
 * a present whose engine only needed 2.5ms, which is a 40ms budget living
 * inside a 33ms frame. This is the question that ends the pump at the right
 * moment. A false answer is safe in both directions -- the blocking sync in
 * complete() is still what guarantees the rows are the CPU's again.
 */
bool psp_media_present_ge_drawing(void);

/*
 * Push a staged texture out of the CPU's cache. The engine reads physical
 * memory and snoops nothing, so a staged buffer the CPU has just written
 * is invisible to it until this runs. No-op on a host.
 */
void psp_media_present_ge_stage_flush(const void *pixels, size_t bytes);

/*
 * Copy `bytes` from a main-RAM source into the EDRAM texture with the DMA
 * controller instead of the CPU.
 *
 * The stage copy was 4.0ms of pure main-thread time, bandwidth-bound reading
 * half a megabyte of decoded picture out of main RAM -- not a reorder, which
 * is why linearizing it barely helped. The DMA controller moves the same
 * bytes at bus speed without touching the CPU or its cache, so the copy costs
 * the interactive thread almost nothing. Contiguous only: the caller uses it
 * for the shipping stride-equals-width case and falls back to the CPU stage
 * otherwise.
 *
 * The source is the decoder's CSC surface, already cache-invalidated after the
 * colour conversion, so physical RAM holds it with no dirty CPU line over it.
 * The destination is EDRAM the graphics engine will read physically; its CPU
 * lines are dropped before the transfer so nothing stale is written back over
 * the DMA result. Returns false when the DMA is unavailable (the host), so the
 * caller performs the CPU copy instead.
 */
bool psp_media_present_ge_stage_dma(
    void *destination, const void *source, size_t bytes);

/*
 * The same DMA copy, off the interactive thread.
 *
 * sceDmacMemcpy blocks its caller for the whole transfer and the DMAC has no
 * user-mode async kick, so even the 1.67ms device copy still sat on the frame's
 * critical path -- the frame measured 17.8ms, a hair over the 16.67ms vblank,
 * so every present waited for the second one and the loop ran at 30Hz. These
 * hand the copy to a dedicated thread: submit posts it and returns at once, the
 * interactive thread feeds the decoder while the DMA controller moves the
 * picture in parallel, and join collects it just before the list is started so
 * the graphics engine never samples an unfinished texture. The transfer, and
 * its cache discipline, are exactly psp_media_present_ge_stage_dma's; only who
 * waits changes.
 *
 * submit returns false -- copying nothing -- when the worker is not running
 * (the host, or a thread that would not start), so the caller does the
 * synchronous copy and the shipped behaviour degrades to today's. It also joins
 * any still-in-flight copy first, so a submit never runs concurrently with an
 * earlier one into the same destination. join blocks until the posted copy
 * finishes and returns its result >= 0, or true immediately when nothing is in
 * flight. busy answers whether a copy is still running, so the feed loop knows
 * when to stop and collect.
 */
bool psp_media_present_ge_stage_dma_submit(
    void *destination, const void *source, size_t bytes,
    unsigned slot, uint32_t generation);
/*
 * How a posted copy ended, in the three cases that have three different
 * answers -- which a bool conflated, and the conflation was a defect.
 *
 * SUCCESS and COMPLETED_FAILURE are both COMPLETIONS: the worker answered, so
 * the controller is provably finished with the source and the destination and
 * the caller may do as it likes with both. A failure is repeated on the
 * calling thread and the slot handed back, exactly as before.
 *
 * TIMED_OUT_STILL_LIVE is neither. Nobody stopped the transfer -- the join
 * gave up waiting for it -- so a DMA controller may still be READING the
 * decoded-output slot and WRITING the staging texture, with nothing in the
 * system tracking either. Treating that as "the copy failed" is what made it
 * dangerous: the old path cleared the in-flight flag and the borrow lease,
 * repeated the copy into the same destination, and let both addresses be
 * reused. Two silent substitutions follow from that, and neither is visible
 * to any check downstream. A later claim or reset frees the source slot and
 * the Media Engine converts a new picture into memory the old transfer is
 * still reading. Worse, a newer picture staged into the same destination is
 * overwritten by the old transfer AFTER its pixel signature was taken --
 * right identity, previous picture's pixels, and the signature that exists to
 * catch exactly that cannot see it, because it sampled before the write.
 *
 * So this outcome quarantines both addresses instead. See
 * psp_media_present_ge_stage_dma_quarantined below.
 */
typedef enum {
    PSP_MEDIA_DMA_JOIN_SUCCESS = 0,
    PSP_MEDIA_DMA_JOIN_COMPLETED_FAILURE = 1,
    PSP_MEDIA_DMA_JOIN_TIMED_OUT_STILL_LIVE = 2
} PspMediaPresentDmaJoin;

PspMediaPresentDmaJoin psp_media_present_ge_stage_dma_join(void);
/*
 * The quarantine a timed-out join leaves behind, and the only way out of it.
 *
 * While it stands, neither address the abandoned transfer holds may be
 * reused: the caller must not stage into that destination, and the decoded
 * pipeline must not return that source slot to its writer. Reader quiescence
 * -- which asks the borrow leases -- must ask this too, because the lease was
 * dropped by a thread that had already stopped tracking the transfer.
 *
 * It is lifted by OBSERVING the completion, never by assuming it. The
 * transfer is not cancellable, so the only sound end is the worker's own done
 * event, polled on later frames at no cost (poll, never wait). Because a
 * timed-out join also latches the worker dead, no further copy is ever posted
 * and that event can only mean the abandoned one finished -- which is what
 * makes an opportunistic poll a proof rather than a guess.
 *
 * A transfer that is never observed to finish has no safe continuation at
 * all, so `expired` reports the outer deadline and the caller ends the
 * session through its ordinary failure surface. Failing a video is a cost;
 * displaying one picture's identity over another's pixels is a defect.
 */
bool psp_media_present_ge_stage_dma_quarantined(void);
bool psp_media_present_ge_stage_dma_quarantine_holds(int slot);
bool psp_media_present_ge_stage_dma_quarantine_holds_staging(
    const void *destination);
/* True the once, on the frame the abandoned transfer is finally seen to have
   completed. Poll from anywhere on the interactive thread. */
bool psp_media_present_ge_stage_dma_quarantine_poll(void);
bool psp_media_present_ge_stage_dma_quarantine_expired(void);
/*
 * Repeat the last posted copy on the calling thread, for a join that failed.
 * The same bytes from the same source to the same destination, so a stalled
 * transfer that lands afterwards writes the identical picture.
 *
 * The caller must name the picture it still means. A join only fails after it
 * has waited, and the decoded-output slot it copied from can hold a different
 * picture by then -- repeating the copy blind would stage that successor under
 * the claimed identity, which nothing downstream could detect. False when
 * there is no copy to repeat OR when the one on record is not the caller's;
 * either way the caller stages again next frame rather than trusting this one.
 */
bool psp_media_present_ge_stage_dma_recover(
    int slot, uint32_t generation);
bool psp_media_present_ge_stage_dma_busy(void);

/* What the off-thread copy did, for the feed report: how many pictures it was
   handed, how many the controller finished, how many it could not, and the
   transfer time it measured on the copy thread -- the device confirmation that
   the copy really is running in parallel and how long it takes. */
typedef struct {
    size_t submitted;
    size_t completed;
    size_t failures;
    unsigned last_copy_us;
    unsigned max_copy_us;
    /* Joins that gave up on the copy. Appended: the feed report prints these
       in order and a reader compares runs by position. */
    size_t timeouts;
    /*
     * And what became of them. Appended for the same reason.
     *
     * quarantines counts the timed-out joins that left a transfer live;
     * late_completions the ones eventually observed to have finished. Their
     * difference is the transfers still outstanding -- or, at the end of a
     * run, the ones that never came back and ended the session. Both have
     * read zero on every device run so far, which is the point of printing
     * them: this is a path being hardened before it is taken, and the next
     * soak says whether it is ever taken at all.
     */
    size_t quarantines;
    size_t late_completions;
    unsigned quarantine_max_us;
} PspMediaPresentDmaStats;
void psp_media_present_ge_stage_dma_stats(PspMediaPresentDmaStats *stats);

/* Why the graphics engine is unusable, or NULL while it has not failed. */
const char *psp_media_present_ge_reason(void);

/*
 * Prove, once per process and before anything is published, that this
 * presenter copies a source pixel's bytes to the panel unchanged.
 *
 * The old question was whether the graphics engine and the software scaler
 * agreed about the 16-bit value a source pixel becomes. They did not, and no
 * pixel format could reconcile them, because the conversion has a channel
 * order and the two presenters disagreed about it. In 8888 there is no
 * conversion: the texture's bytes are the target's bytes are the panel's
 * bytes, so the question becomes byte equality with the source and has one
 * correct answer.
 *
 * `destination` is a 32-bit video buffer the caller is willing to have
 * scribbled -- it must not have been published yet. A mismatch latches the
 * presenter off for the process and the caller falls back to the software
 * scaler in the 16-bit surface. Device-only; the host stub answers false.
 */
bool psp_media_present_ge_passthrough_check(uint32_t *destination);

/*
 * The two values that check produced: what the graphics engine wrote, and the
 * source pixel it was asked to copy. Equal in the low 24 bits is the only
 * acceptable answer. Zero before the check has run.
 *
 * The alpha byte is deliberately excluded. GU_TCC_RGB takes only three
 * channels from the texture and the fourth comes from the primitive, and the
 * panel ignores it in 8888 anyway; comparing it would fail a presenter that
 * is doing exactly the right thing.
 */
void psp_media_present_ge_passthrough(uint32_t *drawn, uint32_t *source);

/*
 * Where each byte of a decoded surface pixel ends up, measured rather than
 * assumed, in both worlds at once.
 *
 * `engine` receives what the graphics engine wrote into the 32-bit target for
 * a source pixel with exactly one saturated colour byte -- which under
 * passthrough must be that same pixel. `scaler` receives the 16-bit value the
 * software scaler produces from it, which is the panel-proven mapping the
 * Sharp option still uses and the chrome's own colours are written for. Side
 * by side they are the record that byte 0 reaches the 32-bit target's byte 0
 * and the 16-bit panel's bits 15..11, which is what makes expanding a 16-bit
 * overlay into a 32-bit video buffer well defined.
 *
 * Three entries each, indexed by source byte. Zero for a byte not yet asked
 * about.
 */
#define PSP_MEDIA_PRESENT_CHANNEL_PROBES 3u

void psp_media_present_ge_channel_map(
    uint32_t engine[PSP_MEDIA_PRESENT_CHANNEL_PROBES],
    uint16_t scaler[PSP_MEDIA_PRESENT_CHANNEL_PROBES]);

/*
 * Draw synthetic frames through the same call the presenter makes, and check
 * the pixels that came back.
 *
 * This exists because the ordinary path cannot be reached off-device: PPSSPP
 * emulates the graphics engine faithfully but has no raw-NAL decoder, so no
 * decoded picture ever exists there and the presenter is never called. The
 * probe supplies its own picture and therefore reaches every part of the draw
 * that does not involve firmware -- context setup, the framebuffer address,
 * the texture format and stride, the UV mapping, the wide-frame split, the
 * completion wait, and the cache handshake.
 *
 * The check is exact rather than approximate, and does not depend on knowing
 * the hardware's filter weights. The synthetic picture is one saturated
 * colour and the macroblock padding around it is a different one, so every
 * pixel inside the video rectangle must come back as exactly the picture's
 * colour: any tap that strayed into padding, any stride or base-address
 * error, and any byte-order mistake all produce something else.
 *
 * The check is exact rather than approximate, and does not depend on knowing
 * the hardware's filter weights or the panel's channel order. The synthetic
 * picture is one saturated colour and the macroblock padding around it is a
 * different one, and the target is 8888, so every pixel inside the video
 * rectangle must come back as exactly the source pixel's own bytes: any tap
 * that strayed into padding, any stride or base-address error, and any
 * reordering all produce something else.
 *
 * `scratch` must be at least PSP_MEDIA_PRESENT_PROBE_SCRATCH_BYTES and
 * `destination` is a 32-bit video buffer the caller is willing to have
 * overwritten. Returns true when every case passed; `detail` receives a
 * description either way.
 */
/* The widest synthetic surface the probe builds. Its stage staging is the
   caller's EDRAM texture, not scratch, so the read it measures is the read
   playback performs. */
#define PSP_MEDIA_PRESENT_PROBE_SURFACE_BYTES ((size_t) 768 * 368 * 4)
/*
 * One synthetic surface per decoded-output slot, and the probe draws every
 * geometry from each of them.
 *
 * It used to build one and certify one address, which was a complete
 * qualification while the decoder had one surface to write. It is not one now:
 * playback alternates between two surfaces at different addresses, and a
 * texture base, a cache flush or a staging copy that is right for the first
 * and wrong for the second would pass a single-surface probe and fail on the
 * device -- which is precisely the class of defect this gate exists to catch
 * before a soak does. The pair is allocated as one block so the two are as far
 * apart as the pool's own carve makes them.
 */
#define PSP_MEDIA_PRESENT_PROBE_SLOTS 2u
#define PSP_MEDIA_PRESENT_PROBE_SCRATCH_BYTES \
    (PSP_MEDIA_PRESENT_PROBE_SURFACE_BYTES * PSP_MEDIA_PRESENT_PROBE_SLOTS)

/*
 * One case's result, returned rather than printed.
 *
 * The probe used to print its per-case line itself, and that line never
 * reached anybody: this translation unit's stdout is the PSP console, which
 * nothing collects, while the validation log the truth cycle reads is written
 * by the caller. The emulator runner asks for those lines and could not have
 * found them on a passing run. Hand the numbers back and let the caller log
 * them -- including submit_us and sync_us, which are the whole cost model of
 * this presenter.
 */
typedef struct {
    int source_width;
    int source_height;
    int source_stride;
    int output_width;
    int output_height;
    unsigned quads;
    /* The three colour bytes the engine wrote at the rectangle's origin and
       at its two quarter points. Under passthrough all three are the source
       pixels the probe supplied. */
    uint32_t flat;
    uint32_t left;
    uint32_t right;
    uint64_t submit_us;
    uint64_t sync_us;
    /*
     * The same output rectangle drawn from a 16x16 cache-resident texture,
     * so its wait is the bilinear filter plus the 8888 framebuffer write with
     * essentially no texture-read bandwidth. sync_us - output_sync_us is then
     * what reading the real texture out of main memory costs -- the number
     * that says whether the remaining GE cost is the texture (a layout or
     * placement lever remains) or the output write (irreducible at 8888).
     * Zero off-device, where the emulator's graphics engine is a CPU loop and
     * neither number is a time.
     */
    uint64_t output_sync_us;
    /* Which texture layout the case drew from, chosen exactly as playback
       chooses it. */
    bool staged;
    /* And which decoded-output slot's surface it drew from, so a failure names
       the address rather than only the geometry. */
    unsigned slot;
    bool passed;
} PspMediaPresentProbeCase;

/* Two geometries -- the 240p stream in its 512-pixel stride and the 360p one
   that must be split across two quads -- from each slot. */
#define PSP_MEDIA_PRESENT_PROBE_GEOMETRIES 2u
#define PSP_MEDIA_PRESENT_PROBE_CASES \
    (PSP_MEDIA_PRESENT_PROBE_GEOMETRIES * PSP_MEDIA_PRESENT_PROBE_SLOTS)

bool psp_media_present_ge_probe(
    void *scratch, size_t scratch_bytes, uint32_t *staging,
    size_t staging_bytes, uint32_t *destination,
    PspMediaPresentProbeCase cases[PSP_MEDIA_PRESENT_PROBE_CASES],
    char *detail, size_t detail_size);

typedef enum {
    /* Bilinear, drawn by the graphics engine. The default. */
    PSP_MEDIA_PRESENT_MODE_GE_SMOOTH = 0,
    /* Nearest neighbour, drawn by the CPU. The user's "Sharp" choice. */
    PSP_MEDIA_PRESENT_MODE_SOFTWARE_SHARP,
    /* Nearest neighbour, drawn by the CPU because the GE could not be used. */
    PSP_MEDIA_PRESENT_MODE_SOFTWARE_FALLBACK
} PspMediaPresentMode;

const char *psp_media_present_mode_name(PspMediaPresentMode mode);

/*
 * What the last present into one back buffer put there.
 *
 * The panel is double buffered, so "the frame already on screen" is really
 * two independent buffers and a present may only be skipped when *this*
 * buffer already holds *this* picture.
 */
typedef struct {
    uint64_t identity;
    uint64_t generation;
    PspMediaPresentRect video;
    bool valid;
} PspMediaPresentRecord;

/*
 * True when re-presenting would reproduce the pixels the buffer already
 * holds.
 *
 * Three things must all hold. The buffer must carry the same decoded picture
 * of the same stream at the same rectangle -- `identity` counts decoded
 * pictures and the browser presents about half again as often as the decoder
 * produces them. The overlay must not paint: some media chrome blends with
 * the picture, and a control that moves or disappears would leave its previous
 * position behind. And the record must have been made by a present that was
 * itself overlay-free, or the buffer already has chrome baked into the video
 * rectangle.
 *
 * `chrome_paints` is therefore both the current frame's question and the
 * record's own precondition: a record is only ever written for an
 * overlay-free present.
 */
bool psp_media_present_skip_allowed(
    const PspMediaPresentRecord *record, uint64_t identity,
    uint64_t generation, const PspMediaPresentRect *video,
    bool chrome_paints);

/* Note an overlay-free present of `identity` at `video`. */
void psp_media_present_record(
    PspMediaPresentRecord *record, uint64_t identity, uint64_t generation,
    const PspMediaPresentRect *video);

/* Forget what a buffer holds, for every buffer. Any writer that is not this
   presenter -- a page compose, the loading supervisor, a resumed display --
   must call this or a later skip would trust pixels that are gone. */
void psp_media_present_records_reset(
    PspMediaPresentRecord *records, size_t count);

#endif
