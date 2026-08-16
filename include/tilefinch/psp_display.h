#ifndef TILEFINCH_PSP_DISPLAY_H
#define TILEFINCH_PSP_DISPLAY_H

/*
 * Shared scanout front end for every PSP executable.
 *
 * The fixture and the full browser each grew their own copy of "compose into
 * VRAM and hand the address to the display service".  The copies drifted: the
 * browser began publishing with an immediate-mode flag the display service
 * rejects, so every present silently became a no-op and the panel stayed
 * blank while the browser's own counters reported healthy presentation.
 * Neither binary ever set its display mode.
 *
 * One implementation now owns the mode, the buffer rotation, the flag, and —
 * critically — the result check.  The syscalls sit behind a backend seam so
 * the accounting that catches a rejected present is exercised by host tests
 * rather than only on hardware.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PSP_DISPLAY_SCREEN_WIDTH 480
#define PSP_DISPLAY_SCREEN_HEIGHT 272
/* Scanout rows are padded to 512 pixels regardless of the visible width. */
#define PSP_DISPLAY_STRIDE 512
#define PSP_DISPLAY_BUFFER_COUNT 2u

/* Values match the PSPSDK enumerations; the device backend static-asserts
   the equality so this header cannot drift from the SDK. */
#define PSP_DISPLAY_FORMAT_RGB565 0
#define PSP_DISPLAY_FORMAT_RGBA8888 3
#define PSP_DISPLAY_SYNC_NEXT_FRAME 1

/*
 * Two scanout formats, and why the second one exists.
 *
 * Everything this project draws is composed by the CPU in 16-bit colour, and
 * that is the panel-proven pipeline. A decoded video frame is the exception:
 * the firmware colour converter writes a 32-bit surface whose byte order this
 * project cannot choose, and turning those bytes into 16 bits is a conversion
 * whose channel order the graphics engine and the software scaler disagree
 * about (see src/psp_media_present_ge.c). PMPlayer's answer, which thousands
 * of hardware users have seen produce correct colour, is to not convert at
 * all: run scanout in 8888 for the duration of playback so the decoder's bytes
 * reach the panel uninterpreted.
 *
 * So the browser owns two surface layouts over the same EDRAM, and switches
 * only while a video frame owns the whole screen. Both live at once and at
 * fixed offsets, which is what lets the page's own 16-bit buffers survive a
 * video session untouched.
 */
typedef enum {
    PSP_DISPLAY_SURFACE_RGB565 = 0,
    PSP_DISPLAY_SURFACE_RGBA8888
} PspDisplaySurface;

/*
 * Seam over the display syscalls.
 *
 * The CPU composes through the *cached* EDRAM alias and `flush_range` pushes
 * the finished buffer out before scanout reads it. Composing through the
 * uncached alias instead is also correct and was tried first, but every store
 * then bypasses the cache: a quarter megabyte per frame goes out a halfword
 * at a time with no burst, which is slow enough to feel on the d-pad. One
 * writeback per frame costs far less than losing the cache for all of it.
 */
typedef struct {
    void *(*compose_base)(void);
    void (*flush_range)(void *address, size_t bytes);
    int (*set_mode)(int mode, int width, int height);
    int (*set_frame_buffer)(void *address, int stride, int format, int sync);
    void (*wait_vblank)(void);
} PspDisplayBackend;

typedef struct {
    const PspDisplayBackend *backend;
    uint16_t *base;
    unsigned back_buffer;
    unsigned presents;
    unsigned rejections;
    unsigned rearms;
    unsigned rearm_failures;
    int first_error;
    int mode_error;
    int last_rearm_error;
    /* Which layout the buffer accessors and the next publish describe. */
    PspDisplaySurface surface;
    unsigned surface_entries;
    unsigned surface_exits;
    unsigned surface_failures;
    int last_surface_error;
} PspDisplay;

/* Pixels between the start of one buffer and the next. */
#define PSP_DISPLAY_BUFFER_PIXELS \
    ((size_t) PSP_DISPLAY_STRIDE * (size_t) PSP_DISPLAY_SCREEN_HEIGHT)

/*
 * The EDRAM plan, in bytes from sceGeEdramGetAddr(). Nothing else in the
 * browser allocates from VRAM: the graphics engine's display list and its
 * sceGuGetMemory arena are .bss in main memory, and no depth buffer is ever
 * created.
 *
 * Two layouts share the same memory, because the two modes are never live at
 * once. The page's 16-bit buffers hold nothing that has to survive a video
 * session -- every page present memcpys the engine's own raster into the back
 * buffer from main memory and composites chrome over it, so the surface is
 * rebuilt from scratch on return -- and that is what pays for the texture.
 *
 *   Page mode
 *     0x000000  page buffer 0      278,528 B
 *     0x044000  page buffer 1      278,528 B
 *
 *   Video mode
 *     0x000000  video buffer 0     557,056 B   (over both page buffers)
 *     0x088000  video buffer 1     557,056 B
 *     0x110000  staged texture   557,056 B
 *     0x198000  overlay scratch    278,528 B
 *     0x1dc000  unused             147,456 B
 *
 * The texture is here rather than in main memory because that is the whole
 * cost of a video frame. The device measured the graphics engine's wait split
 * into its two halves: writing 480x270 of 8888 output took 456us, and reading
 * the texture out of main memory took 10,312us -- 96% of the frame, an order
 * of magnitude above what the bus should charge for half a megabyte. EDRAM is
 * the memory the engine is fast against, and it is why PMPlayer renders video
 * through an EDRAM buffer.
 *
 * Video stays double buffered. Single buffering would free the same 557,056
 * bytes, but the chrome compositor writes the buffer scanout is reading, and
 * the page buffers are dead weight during playback whereas the second video
 * buffer is not.
 */
#define PSP_DISPLAY_VIDEO_BUFFER_PIXELS PSP_DISPLAY_BUFFER_PIXELS
#define PSP_DISPLAY_EDRAM_BYTES ((size_t) 2 * 1024 * 1024)
#define PSP_DISPLAY_VIDEO_BUFFER_BYTES \
    (PSP_DISPLAY_VIDEO_BUFFER_PIXELS * sizeof(uint32_t))
/* Video mode starts at the base, over the page buffers it replaces. */
#define PSP_DISPLAY_VIDEO_BASE_BYTES ((size_t) 0)
#define PSP_DISPLAY_VIDEO_TEXTURE_BASE_BYTES \
    (PSP_DISPLAY_VIDEO_BASE_BYTES \
     + PSP_DISPLAY_VIDEO_BUFFER_BYTES * PSP_DISPLAY_BUFFER_COUNT)
/* One 512-pixel-wide surface, which is the shipping texture geometry. */
#define PSP_DISPLAY_VIDEO_TEXTURE_BYTES PSP_DISPLAY_VIDEO_BUFFER_BYTES
#define PSP_DISPLAY_OVERLAY_BASE_BYTES \
    (PSP_DISPLAY_VIDEO_TEXTURE_BASE_BYTES + PSP_DISPLAY_VIDEO_TEXTURE_BYTES)

/* Real syscall backend, or NULL when built for a host. */
const PspDisplayBackend *psp_display_system_backend(void);

/*
 * Claim the display mode and the EDRAM base.  Returns false when the backend
 * is unusable or the mode was refused; the browser can still run, but the
 * caller then knows nothing will be seen.
 */
bool psp_display_begin(PspDisplay *display, const PspDisplayBackend *backend);

/*
 * Buffer the CPU should compose into next, and the one currently scanned
 * out.  Both are compose-side (uncached on device) addresses.
 *
 * NULL while the video surface is active. Every 16-bit composer in the
 * process reaches the panel through these two, so answering NULL is what
 * stops one of them writing 565 rows into a 32-bit buffer if a caller is ever
 * added that forgets to leave the video surface first.
 */
uint16_t *psp_display_back_buffer(const PspDisplay *display);
uint16_t *psp_display_front_buffer(const PspDisplay *display);

/*
 * The fullscreen-video scanout format.
 *
 * `begin` changes what the *next* publish latches and performs no syscall, so
 * the panel keeps showing the last 16-bit frame until a complete 32-bit one
 * replaces it. `end` reasserts the last 16-bit front buffer immediately,
 * because the composers that follow it write a different address and the panel
 * must not be left scanning out video while they do -- an 8888 surface still
 * latched under a 565 compositor is the whole screen turned to noise.
 *
 * `end` is idempotent and safe to call when the surface was never entered,
 * which is what lets every exit path assert it unconditionally.
 */
bool psp_display_video_begin(PspDisplay *display);
bool psp_display_video_end(PspDisplay *display);
bool psp_display_video_active(const PspDisplay *display);

/*
 * True when `address` lies inside the 2 MiB of EDRAM this front end owns.
 * Where the graphics engine's texture lives is the largest term in a video
 * frame, so the present report says which memory it read rather than assuming
 * the staging it was handed was the one it used.
 */
bool psp_display_in_edram(const PspDisplay *display, const void *address);

/* The 32-bit pair, the EDRAM staging the graphics engine samples its texture
   from, and the 16-bit scratch the media chrome is composed into before it is
   expanded over them. NULL unless the video surface is active. */
uint32_t *psp_display_video_back_buffer(const PspDisplay *display);
uint32_t *psp_display_video_front_buffer(const PspDisplay *display);
uint32_t *psp_display_video_texture(const PspDisplay *display);
uint16_t *psp_display_video_overlay_scratch(const PspDisplay *display);

/*
 * Latch the composed back buffer for the next vblank and rotate.  Returns
 * false when the display service refused the address, which is the failure
 * that must never again be mistaken for a successful frame.
 */
bool psp_display_publish(PspDisplay *display);

/*
 * Reassert the mode and last accepted front buffer after system resume.
 * This preserves the buffer rotation and does not count as a newly composed
 * frame.
 */
bool psp_display_rearm(PspDisplay *display);

/* True when every present so far was accepted. */
bool psp_display_healthy(const PspDisplay *display);

#endif
