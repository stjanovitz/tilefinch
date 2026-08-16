#include "tilefinch/psp_display.h"

#include <stddef.h>
#include <string.h>

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
        + (size_t) (index % PSP_DISPLAY_BUFFER_COUNT)
            * PSP_DISPLAY_BUFFER_PIXELS;
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
            + (size_t) (index % PSP_DISPLAY_BUFFER_COUNT)
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
    return buffer_at(display, display->back_buffer ^ 1u);
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

/* Flush, wait for the boundary, and latch one buffer of the active surface.
   Shared by publish, rearm, and the return from the video surface so the three
   cannot disagree about the format or the sync flag. */
static int latch_buffer(PspDisplay *display, unsigned index)
{
    void *buffer = compose_buffer(display, index);
    if (buffer == NULL) return -1;
    if (display->backend->flush_range != NULL) {
        display->backend->flush_range(
            buffer, surface_buffer_bytes(display));
    }
    if (display->backend->wait_vblank != NULL) {
        display->backend->wait_vblank();
    }
    return display->backend->set_frame_buffer(
        buffer, PSP_DISPLAY_STRIDE, surface_format(display),
        PSP_DISPLAY_SYNC_NEXT_FRAME);
}

bool psp_display_video_begin(PspDisplay *display)
{
    if (display == NULL || display->backend == NULL
        || display->base == NULL) return false;
    if (psp_display_video_active(display)) return true;
    /*
     * No syscall here on purpose. The 32-bit buffer this rotation names holds
     * whatever the last video session left, and latching it now would put that
     * on the panel for one frame. Changing only what the next publish
     * describes means the panel keeps showing the last complete 16-bit frame
     * until a complete 32-bit one replaces it.
     */
    display->surface = PSP_DISPLAY_SURFACE_RGBA8888;
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
    uint16_t *front = buffer_at(display, display->back_buffer ^ 1u);
    if (front != NULL)
        memset(front, 0, PSP_DISPLAY_BUFFER_PIXELS * sizeof(*front));
    int result = latch_buffer(display, display->back_buffer ^ 1u);
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
       the vblank boundary and then latching for the *next* frame is what keeps
       a half-drawn title bar off the panel. */
    int result = latch_buffer(display, display->back_buffer);

    display->presents++;
    if (result < 0) {
        display->rejections++;
        if (display->first_error == 0) display->first_error = result;
        /* Do not rotate: the buffer the panel is showing did not change, so
           the next compose must not treat it as the free one. */
        return false;
    }
    display->back_buffer ^= 1u;
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
    if (result >= 0) result = latch_buffer(display, display->back_buffer ^ 1u);
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
    PSP_DISPLAY_OVERLAY_BASE_BYTES
        + PSP_DISPLAY_BUFFER_PIXELS * sizeof(uint16_t)
        <= PSP_DISPLAY_EDRAM_BYTES,
    "the video layout must fit the PSP's 2 MiB of EDRAM");
_Static_assert(
    PSP_DISPLAY_BUFFER_PIXELS * sizeof(uint16_t) * PSP_DISPLAY_BUFFER_COUNT
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
