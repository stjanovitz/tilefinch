#include "tilefinch/psp_display.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

static uint32_t psp_display_content_epoch = 1u;

uint32_t psp_display_edram_content_epoch(void)
{
    return psp_display_content_epoch;
}

bool psp_display_begin(PspDisplay *display, const PspDisplayBackend *backend)
{
    if (display == NULL) return false;
    display->backend = NULL;
    display->base = NULL;
    display->back_buffer = 0;
    display->presents = 0;
    display->rejections = 0;
    display->rearms = 0;
    display->rearm_failures = 0;
    display->first_error = 0;
    display->mode_error = 0;
    display->last_rearm_error = 0;
    display->surface = PSP_DISPLAY_SURFACE_RGB565;
    display->surface_entries = 0;
    display->surface_exits = 0;
    display->surface_failures = 0;
    display->last_surface_error = 0;
    if (backend == NULL || backend->compose_base == NULL
        || backend->set_mode == NULL
        || backend->set_frame_buffer == NULL) {
        return false;
    }
    display->backend = backend;

    /*
     * State the mode rather than inheriting whatever the launcher left.  A
     * refusal is recorded but not fatal: the caller may still want to run and
     * report, and the present accounting will show the consequence.
     */
    int mode = backend->set_mode(
        0, PSP_DISPLAY_SCREEN_WIDTH, PSP_DISPLAY_SCREEN_HEIGHT);
    if (mode < 0) display->mode_error = mode;

    display->base = backend->compose_base();
    return display->base != NULL && mode >= 0;
}

static uint16_t *buffer_at(const PspDisplay *display, unsigned index)
{
    if (display == NULL || display->base == NULL) return NULL;
    return display->base
        + (size_t) (index % PSP_DISPLAY_PAGE_BUFFER_COUNT)
            * PSP_DISPLAY_BUFFER_PIXELS;
}

static unsigned page_previous_buffer(unsigned index)
{
    return (index + PSP_DISPLAY_PAGE_BUFFER_COUNT - 1u)
        % PSP_DISPLAY_PAGE_BUFFER_COUNT;
}

/* The video layout starts at the base, over the page buffers it replaces; see
   the EDRAM plan in the header. Both aliases answer the same physical memory,
   so the arithmetic is a byte offset from whatever alias compose_base
   returned. */
static void *edram_at(const PspDisplay *display, size_t offset_bytes)
{
    if (display == NULL || display->base == NULL) return NULL;
    return (unsigned char *) display->base + offset_bytes;
}

static uint32_t *video_buffer_at(const PspDisplay *display, unsigned index)
{
    return (uint32_t *) edram_at(
        display,
        PSP_DISPLAY_VIDEO_BASE_BYTES
            + (size_t) (index % PSP_DISPLAY_VIDEO_BUFFER_COUNT)
                * PSP_DISPLAY_VIDEO_BUFFER_BYTES);
}

bool psp_display_video_active(const PspDisplay *display)
{
    return display != NULL
        && display->surface == PSP_DISPLAY_SURFACE_RGBA8888;
}

uint16_t *psp_display_back_buffer(const PspDisplay *display)
{
    if (display == NULL || psp_display_video_active(display)) return NULL;
    return buffer_at(display, display->back_buffer);
}

uint16_t *psp_display_front_buffer(const PspDisplay *display)
{
    if (display == NULL || psp_display_video_active(display)) return NULL;
    return buffer_at(display, page_previous_buffer(display->back_buffer));
}

uint32_t *psp_display_video_back_buffer(const PspDisplay *display)
{
    if (!psp_display_video_active(display)) return NULL;
    return video_buffer_at(display, display->back_buffer);
}

uint32_t *psp_display_video_front_buffer(const PspDisplay *display)
{
    if (!psp_display_video_active(display)) return NULL;
    return video_buffer_at(display, display->back_buffer ^ 1u);
}

bool psp_display_in_edram(const PspDisplay *display, const void *address)
{
    if (display == NULL || display->base == NULL || address == NULL)
        return false;
    /* Both aliases answer the same physical memory, so compare with the cache
       bit removed: a cached staging pointer and an uncached base still have
       to land in the same 2 MiB. */
    uintptr_t base = (uintptr_t) display->base & 0x1fffffffu;
    uintptr_t at = (uintptr_t) address & 0x1fffffffu;
    return at >= base && at - base < PSP_DISPLAY_EDRAM_BYTES;
}

uint32_t *psp_display_video_texture(const PspDisplay *display)
{
    if (!psp_display_video_active(display)) return NULL;
    return (uint32_t *) edram_at(
        display, PSP_DISPLAY_VIDEO_TEXTURE_BASE_BYTES);
}

uint16_t *psp_display_video_overlay_scratch(const PspDisplay *display)
{
    if (!psp_display_video_active(display)) return NULL;
    return (uint16_t *) edram_at(display, PSP_DISPLAY_OVERLAY_BASE_BYTES);
}

uint16_t *psp_display_video_aux(const PspDisplay *display)
{
    if (!psp_display_video_active(display)) return NULL;
    return (uint16_t *) edram_at(display, PSP_DISPLAY_VIDEO_AUX_BASE_BYTES);
}

size_t psp_display_video_aux_pixels(const PspDisplay *display)
{
    return psp_display_video_active(display)
        ? PSP_DISPLAY_VIDEO_AUX_BYTES / sizeof(uint16_t) : 0u;
}

/* Bytes one buffer of the active surface occupies, which is what has to be
   written back before scanout is pointed at it. */
static size_t surface_buffer_bytes(const PspDisplay *display)
{
    return psp_display_video_active(display)
        ? PSP_DISPLAY_VIDEO_BUFFER_PIXELS * sizeof(uint32_t)
        : PSP_DISPLAY_BUFFER_PIXELS * sizeof(uint16_t);
}

static int surface_format(const PspDisplay *display)
{
    return psp_display_video_active(display)
        ? PSP_DISPLAY_FORMAT_RGBA8888 : PSP_DISPLAY_FORMAT_RGB565;
}

static void *compose_buffer(const PspDisplay *display, unsigned index)
{
    return psp_display_video_active(display)
        ? (void *) video_buffer_at(display, index)
        : (void *) buffer_at(display, index);
}

#if defined(TILEFINCH_PSP_LATCH_PROBE) && defined(__PSP__)
#include <pspthreadman.h>

static PspDisplayValidationLogger latch_probe_logger;
static bool latch_probe_enabled;

typedef struct {
    uint32_t video;
    uint32_t caption;
    uint32_t bar;
} LatchProbeRegions;

void psp_display_set_validation_logger(PspDisplayValidationLogger logger)
{
    latch_probe_logger = logger;
}

void psp_display_set_latch_probe_enabled(bool enabled)
{
    latch_probe_enabled = enabled;
}

/*
 * Flicker probe: one line per latch, hashing the rows the reported flicker
 * lives in, exactly as scanout will read them. Regions are in screen rows;
 * pixel width comes from the active format. Post-fix diagnosis only; the
 * ordinary validation scenarios do not parse these lines.
 */
static uint32_t latch_probe_hash(const void *buffer, bool wide,
                                 int first_row, int end_row)
{
    const unsigned char *bytes = buffer;
    size_t row_bytes = (size_t) PSP_DISPLAY_STRIDE * (wide ? 4u : 2u);
    uint32_t hash = 2166136261u;
    for (int row = first_row; row < end_row; row++) {
        const unsigned char *line = bytes + (size_t) row * row_bytes;
        for (size_t at = 0; at < 480u * (wide ? 4u : 2u); at++) {
            hash ^= line[at];
            hash *= 16777619u;
        }
    }
    return hash;
}

static LatchProbeRegions latch_probe_regions(const void *buffer, bool wide)
{
    return (LatchProbeRegions) {
        .video = latch_probe_hash(buffer, wide, 40, 80),
        .caption = latch_probe_hash(buffer, wide, 140, 203),
        .bar = latch_probe_hash(buffer, wide, 203, 272)
    };
}

static void latch_probe(const PspDisplay *display, const void *buffer,
                        unsigned index)
{
    static unsigned sequence;
    static LatchProbeRegions recorded[PSP_DISPLAY_PAGE_BUFFER_COUNT];
    static bool recorded_valid[PSP_DISPLAY_PAGE_BUFFER_COUNT];
    static bool recorded_wide;
    static bool recorded_format_valid;
    if (latch_probe_logger == NULL || !latch_probe_enabled) return;
    bool wide = psp_display_video_active(display);
    if (!recorded_format_valid || recorded_wide != wide) {
        memset(recorded_valid, 0, sizeof(recorded_valid));
        recorded_wide = wide;
        recorded_format_valid = true;
    }
    unsigned count = wide ? PSP_DISPLAY_VIDEO_BUFFER_COUNT
                          : PSP_DISPLAY_PAGE_BUFFER_COUNT;
    unsigned front_index = (index + count - 1u) % count;
    if (front_index < PSP_DISPLAY_PAGE_BUFFER_COUNT
        && recorded_valid[front_index]) {
        void *front = compose_buffer(display, front_index);
        LatchProbeRegions current = latch_probe_regions(front, wide);
        const LatchProbeRegions *old = &recorded[front_index];
        if (old->video != current.video || old->caption != current.caption
            || old->bar != current.bar) {
            char changed[32];
            size_t used = 0;
#define APPEND_CHANGED(name) do { \
    int wrote = snprintf(changed + used, sizeof(changed) - used, \
                         "%s%s", used == 0 ? "" : ",", (name)); \
    if (wrote > 0 && (size_t) wrote < sizeof(changed) - used) \
        used += (size_t) wrote; \
} while (0)
            if (old->video != current.video) APPEND_CHANGED("video");
            if (old->caption != current.caption) APPEND_CHANGED("caption");
            if (old->bar != current.bar) APPEND_CHANGED("bar");
#undef APPEND_CHANGED
            latch_probe_logger(
                "tilefinch-latch-recheck: n=%u buffer=%u changed=%s "
                "old=%08x/%08x/%08x new=%08x/%08x/%08x\n",
                sequence, front_index, changed,
                old->video, old->caption, old->bar,
                current.video, current.caption, current.bar);
        }
    }
    LatchProbeRegions next = latch_probe_regions(buffer, wide);
    if (index < PSP_DISPLAY_PAGE_BUFFER_COUNT) {
        recorded[index] = next;
        recorded_valid[index] = true;
    }
    latch_probe_logger(
        "tilefinch-latch: n=%u us=%llu thread=0x%08x buffer=%u format=%s "
        "video=%08x caption=%08x bar=%08x\n",
        sequence++,
        (unsigned long long) sceKernelGetSystemTimeWide(),
        (unsigned) sceKernelGetThreadId(), index, wide ? "8888" : "565",
        next.video, next.caption, next.bar);
}
#else
#if defined(TILEFINCH_PSP_VALIDATION_LOG) \
    || defined(TILEFINCH_PSP_LATCH_PROBE)
void psp_display_set_validation_logger(PspDisplayValidationLogger logger)
{
    (void) logger;
}

void psp_display_set_latch_probe_enabled(bool enabled)
{
    (void) enabled;
}
#endif
static void latch_probe(const PspDisplay *display, const void *buffer,
                        unsigned index)
{
    (void) display;
    (void) buffer;
    (void) index;
}
#endif

/* Flush and queue one complete buffer for the next vertical boundary, then
   wait until that boundary has occurred before allowing its old front to be
   reused.  Waiting first and requesting NEXT_FRAME afterward leaves software
   one boundary ahead of scanout: the following compose can then overwrite a
   buffer while the panel is still reading it, which appears as a horizontal
   band through otherwise opaque chrome.  Shared by publish, rearm, and the
   return from the video surface so the three cannot disagree about ordering,
   format, or the sync flag. */
static int latch_buffer(PspDisplay *display, unsigned index)
{
    void *buffer = compose_buffer(display, index);
    if (buffer == NULL) return -1;
    if (display->backend->flush_range != NULL) {
        display->backend->flush_range(
            buffer, surface_buffer_bytes(display));
    }
    latch_probe(display, buffer, index);
    int result = display->backend->set_frame_buffer(
        buffer, PSP_DISPLAY_STRIDE, surface_format(display),
        PSP_DISPLAY_SYNC_NEXT_FRAME);
    if (result >= 0 && display->backend->wait_vblank != NULL) {
        display->backend->wait_vblank();
    }
    return result;
}

bool psp_display_video_begin(PspDisplay *display)
{
    if (display == NULL || display->backend == NULL
        || display->base == NULL) return false;
    if (psp_display_video_active(display)) return true;
    /* The video pair overlays the page triple: video slot 0 covers page slots
       0 and 1, while video slot 1 starts at page slot 2. The first video
       compose must not overwrite the page buffer scanout is still reading.
       Choose the other region from the current page front.

       Merely changing the format of the next decoded publish is not atomic on
       the physical panel: hardware showed one scan where the upper rows had
       adopted 8888 while the lower rows still interpreted the old 565
       surface. Preserve the loading UI across that boundary by expanding its
       complete visible page into the disjoint video buffer and latching that
       visually equivalent bridge frame first. Only after the latch may the
       caller draw a decoded picture into the other video buffer. */
    unsigned page_back = display->back_buffer;
    unsigned page_front = page_previous_buffer(display->back_buffer);
    unsigned video_bridge = page_front == 2u ? 0u : 1u;
    const uint16_t *source = buffer_at(display, page_front);
    uint32_t *bridge = video_buffer_at(display, video_bridge);
    if (source == NULL || bridge == NULL) return false;
    for (size_t at = 0; at < PSP_DISPLAY_BUFFER_PIXELS; at++) {
        uint16_t pixel = source[at];
        unsigned red = (unsigned) (pixel >> 11) & 31u;
        unsigned green = (unsigned) (pixel >> 5) & 63u;
        unsigned blue = (unsigned) pixel & 31u;
        red = (red << 3) | (red >> 2);
        green = (green << 2) | (green >> 4);
        blue = (blue << 3) | (blue >> 2);
        bridge[at] = (uint32_t) red | ((uint32_t) green << 8)
            | ((uint32_t) blue << 16) | UINT32_C(0xff000000);
    }
    display->back_buffer = video_bridge;
    psp_display_content_epoch++;
    if (psp_display_content_epoch == 0u) psp_display_content_epoch = 1u;
    display->surface = PSP_DISPLAY_SURFACE_RGBA8888;
    int result = latch_buffer(display, video_bridge);
    display->presents++;
    if (result < 0) {
        display->rejections++;
        if (display->first_error == 0) display->first_error = result;
        display->surface_failures++;
        display->last_surface_error = result;
        display->surface = PSP_DISPLAY_SURFACE_RGB565;
        display->back_buffer = page_back;
        return false;
    }
    display->back_buffer = video_bridge ^ 1u;
    display->surface_entries++;
    return true;
}

bool psp_display_video_end(PspDisplay *display)
{
    if (display == NULL || display->backend == NULL
        || display->base == NULL) return false;
    if (!psp_display_video_active(display)) return true;
    display->surface = PSP_DISPLAY_SURFACE_RGB565;
    display->surface_exits++;
    /*
     * Unlike entering, leaving cannot wait for the next publish. The
     * composers that follow write the 16-bit buffers, at a different address,
     * and a panel still latched on the video surface while they do is the
     * whole screen turned to noise.
     *
     * The video layout is written over the page buffers, so the one about to
     * be reasserted holds video bytes rather than the page frame it held
     * before. Clear it: one frame of black while the next present composes
     * the page, instead of one frame of a 32-bit picture read as 565. The
     * cost is one memset of a quarter megabyte, once, on the way out of a
     * video session.
     */
    unsigned front_index = page_previous_buffer(display->back_buffer);
    uint16_t *front = buffer_at(display, front_index);
    if (front != NULL)
        memset(front, 0, PSP_DISPLAY_BUFFER_PIXELS * sizeof(*front));
    int result = latch_buffer(display, front_index);
    if (result < 0) {
        display->surface_failures++;
        display->last_surface_error = result;
        return false;
    }
    return true;
}

bool psp_display_publish(PspDisplay *display)
{
    if (display == NULL || display->backend == NULL
        || display->base == NULL) return false;

    /* Scanout reads physical memory and does not snoop the CPU cache, so the
       composed buffer has to be written back before the address is handed
       over. Skipping this is what made composing through the uncached alias
       look necessary. Publish only a completely composed buffer: waiting for
       the address for the next vblank and waiting until that latch completes
       is what keeps a half-drawn title bar off the panel. */
    int result = latch_buffer(display, display->back_buffer);

    display->presents++;
    if (result < 0) {
        display->rejections++;
        if (display->first_error == 0) display->first_error = result;
        /* Do not rotate: the buffer the panel is showing did not change, so
           the next compose must not treat it as the free one. */
        return false;
    }
    if (psp_display_video_active(display))
        display->back_buffer ^= 1u;
    else
        display->back_buffer =
            (display->back_buffer + 1u) % PSP_DISPLAY_PAGE_BUFFER_COUNT;
    return true;
}

bool psp_display_rearm(PspDisplay *display)
{
    if (display == NULL || display->backend == NULL
        || display->base == NULL) return false;
    display->rearms++;
    display->last_rearm_error = 0;

    int result = display->backend->set_mode(
        0, PSP_DISPLAY_SCREEN_WIDTH, PSP_DISPLAY_SCREEN_HEIGHT);
    unsigned front_index = psp_display_video_active(display)
        ? display->back_buffer ^ 1u
        : page_previous_buffer(display->back_buffer);
    if (result >= 0) result = latch_buffer(display, front_index);
    if (result < 0) {
        display->rearm_failures++;
        display->last_rearm_error = result;
        return false;
    }
    return true;
}

bool psp_display_healthy(const PspDisplay *display)
{
    if (display == NULL) return false;
    return display->backend != NULL && display->base != NULL
        && display->mode_error == 0 && display->rejections == 0
        && display->rearm_failures == 0 && display->surface_failures == 0;
}

#if defined(__PSP__)

/* PSPSDK's display enumerators exceed int, which -Wpedantic reports before
   C23. Keep the strict warnings for this file's own code. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include <pspdisplay.h>
#include <pspge.h>
#include <pspkernel.h>
#pragma GCC diagnostic pop

_Static_assert(PSP_DISPLAY_FORMAT_RGB565 == PSP_DISPLAY_PIXEL_FORMAT_565,
               "the shared RGB565 format must match the PSPSDK enumeration");
_Static_assert(PSP_DISPLAY_FORMAT_RGBA8888 == PSP_DISPLAY_PIXEL_FORMAT_8888,
               "the shared 8888 format must match the PSPSDK enumeration");
_Static_assert(PSP_DISPLAY_SYNC_NEXT_FRAME == PSP_DISPLAY_SETBUF_NEXTFRAME,
               "the shared sync mode must match the PSPSDK enumeration");
/* Every surface this file hands out has to be inside the 2 MiB the hardware
   has. Checked here rather than trusted: the arithmetic is in the header and
   nothing else would notice it growing. */
_Static_assert(
    PSP_DISPLAY_VIDEO_AUX_BASE_BYTES
        <= PSP_DISPLAY_EDRAM_BYTES,
    "the video layout must fit the PSP's 2 MiB of EDRAM");
_Static_assert(
    PSP_DISPLAY_BUFFER_PIXELS * sizeof(uint16_t)
        * PSP_DISPLAY_PAGE_BUFFER_COUNT
        <= PSP_DISPLAY_EDRAM_BYTES,
    "the page layout must fit the PSP's 2 MiB of EDRAM");
/* The texture is only worth moving here if it holds the whole shipping
   geometry: 512 columns of the tallest admitted surface. */
_Static_assert(
    PSP_DISPLAY_VIDEO_TEXTURE_BYTES >= (size_t) 512 * 272 * 4,
    "the EDRAM texture staging must hold one 512x272 32-bit surface");

static void *system_compose_base(void)
{
    /* The ordinary cached alias: the CPU gets full-speed burst writes, and
       flush_range below makes the result visible to scanout. */
    return sceGeEdramGetAddr();
}

static void system_flush_range(void *address, size_t bytes)
{
    sceKernelDcacheWritebackRange(address, bytes);
}

static int system_set_mode(int mode, int width, int height)
{
    return sceDisplaySetMode(mode, width, height);
}

static int system_set_frame_buffer(void *address, int stride, int format,
                                   int sync)
{
    return sceDisplaySetFrameBuf(address, stride, format, sync);
}

static void system_wait_vblank(void)
{
    sceDisplayWaitVblankStart();
}

static const PspDisplayBackend system_backend = {
    .compose_base = system_compose_base,
    .flush_range = system_flush_range,
    .set_mode = system_set_mode,
    .set_frame_buffer = system_set_frame_buffer,
    .wait_vblank = system_wait_vblank
};

const PspDisplayBackend *psp_display_system_backend(void)
{
    return &system_backend;
}

#else

const PspDisplayBackend *psp_display_system_backend(void)
{
    return NULL;
}

#endif
