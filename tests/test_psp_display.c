/*
 * Regression coverage for the scanout front end.
 *
 * The defect these tests exist for: the browser published frames with an
 * immediate-mode flag the display service rejects, ignored the error, and
 * counted the present as successful.  The panel stayed blank for every run
 * while validation reported healthy presentation counters.  The tests below
 * pin the flag, the mode call, the null-alias guard, and — above all — that a
 * refused present is visible as a failure rather than absorbed.
 */

#include "tilefinch/psp_display.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return false; \
    } \
} while (0)

typedef struct {
    uint16_t *memory;
    int set_mode_calls;
    int mode_result;
    int mode_width;
    int mode_height;
    int present_calls;
    int present_result;
    int vblank_waits;
    int flush_calls;
    void *last_flush_address;
    size_t last_flush_bytes;
    bool flush_after_present;
    void *last_address;
    int last_stride;
    int last_format;
    int last_sync;
    bool vblank_before_present;
} FakeDisplay;

static FakeDisplay fake;

static void *fake_compose_base(void)
{
    return fake.memory;
}

static void fake_flush_range(void *address, size_t bytes)
{
    fake.flush_calls++;
    fake.last_flush_address = address;
    fake.last_flush_bytes = bytes;
    /* Scanout must never be handed a buffer that was not written back. */
    if (fake.flush_calls <= fake.present_calls) fake.flush_after_present = true;
}

static int fake_set_mode(int mode, int width, int height)
{
    (void) mode;
    fake.set_mode_calls++;
    fake.mode_width = width;
    fake.mode_height = height;
    return fake.mode_result;
}

static int fake_set_frame_buffer(void *address, int stride, int format,
                                 int sync)
{
    fake.present_calls++;
    fake.last_address = address;
    fake.last_stride = stride;
    fake.last_format = format;
    fake.last_sync = sync;
    return fake.present_result;
}

static void fake_wait_vblank(void)
{
    fake.vblank_waits++;
    if (fake.vblank_waits > fake.present_calls) {
        fake.vblank_before_present = true;
    }
}

static const PspDisplayBackend fake_backend = {
    .compose_base = fake_compose_base,
    .flush_range = fake_flush_range,
    .set_mode = fake_set_mode,
    .set_frame_buffer = fake_set_frame_buffer,
    .wait_vblank = fake_wait_vblank
};

static bool fake_reset(void)
{
    free(fake.memory);
    memset(&fake, 0, sizeof(fake));
    /* The whole EDRAM plan, not just the two page buffers: the video surface
       and the overlay scratch live above them at fixed offsets. */
    fake.memory = calloc(
        PSP_DISPLAY_EDRAM_BYTES / sizeof(*fake.memory),
        sizeof(*fake.memory));
    fake.vblank_before_present = true;
    return fake.memory != NULL;
}

/* The browser was blank because it published with the immediate-mode flag.
   Pin the sync mode, the format, the stride, and the mode call. */
static bool test_publish_uses_next_frame_and_claims_the_mode(void)
{
    CHECK(fake_reset());
    PspDisplay display;
    CHECK(psp_display_begin(&display, &fake_backend));

    CHECK(fake.set_mode_calls == 1);
    CHECK(fake.mode_width == PSP_DISPLAY_SCREEN_WIDTH);
    CHECK(fake.mode_height == PSP_DISPLAY_SCREEN_HEIGHT);

    CHECK(psp_display_publish(&display));
    CHECK(fake.last_sync == PSP_DISPLAY_SYNC_NEXT_FRAME);
    CHECK(fake.last_format == PSP_DISPLAY_FORMAT_RGB565);
    CHECK(fake.last_stride == PSP_DISPLAY_STRIDE);
    /* A frame must be complete before it is latched. */
    CHECK(fake.vblank_before_present);
    CHECK(psp_display_healthy(&display));
    return true;
}

/* The defect that hid everything else: a refused present reported success. */
static bool test_rejected_present_is_a_failure_not_a_frame(void)
{
    CHECK(fake_reset());
    PspDisplay display;
    CHECK(psp_display_begin(&display, &fake_backend));

    fake.present_result = -1;
    CHECK(!psp_display_publish(&display));
    CHECK(!psp_display_healthy(&display));
    CHECK(display.rejections == 1);
    CHECK(display.presents == 1);
    CHECK(display.first_error == -1);

    /* The panel never changed, so the buffer just composed is still the one
       being scanned out; rotating would hand the caller a live surface. */
    uint16_t *back = psp_display_back_buffer(&display);
    fake.present_result = -2;
    CHECK(!psp_display_publish(&display));
    CHECK(psp_display_back_buffer(&display) == back);
    CHECK(display.rejections == 2);
    /* The first error is the diagnostic one and must not be overwritten. */
    CHECK(display.first_error == -1);
    return true;
}

static bool test_buffers_rotate_and_stay_distinct(void)
{
    CHECK(fake_reset());
    PspDisplay display;
    CHECK(psp_display_begin(&display, &fake_backend));

    uint16_t *first_back = psp_display_back_buffer(&display);
    uint16_t *first_front = psp_display_front_buffer(&display);
    CHECK(first_back != NULL && first_front != NULL);
    CHECK(first_back != first_front);
    /* Page mode owns three stride-pages inside the EDRAM allocation. */
    CHECK(first_front == first_back
              + 2u * PSP_DISPLAY_BUFFER_PIXELS);

    CHECK(psp_display_publish(&display));
    CHECK(fake.last_address == first_back);
    uint16_t *second_back = psp_display_back_buffer(&display);
    CHECK(second_back == first_back + PSP_DISPLAY_BUFFER_PIXELS);
    CHECK(psp_display_front_buffer(&display) == first_back);

    CHECK(psp_display_publish(&display));
    CHECK(fake.last_address == second_back);
    CHECK(psp_display_back_buffer(&display) == first_front);
    CHECK(psp_display_front_buffer(&display) == second_back);
    CHECK(psp_display_publish(&display));
    CHECK(fake.last_address == first_front);
    CHECK(psp_display_back_buffer(&display) == first_back);
    CHECK(psp_display_front_buffer(&display) == first_front);
    CHECK(display.presents == 3 && display.rejections == 0);
    return true;
}

/* A refused mode must be observable rather than assumed to have worked. */
static bool test_refused_mode_is_reported(void)
{
    CHECK(fake_reset());
    fake.mode_result = -3;
    PspDisplay display;
    CHECK(!psp_display_begin(&display, &fake_backend));
    CHECK(display.mode_error == -3);
    CHECK(!psp_display_healthy(&display));
    return true;
}

static bool test_unusable_backend_is_refused(void)
{
    PspDisplay display;
    CHECK(!psp_display_begin(&display, NULL));
    CHECK(!psp_display_publish(&display));
    CHECK(psp_display_back_buffer(&display) == NULL);

    /* A partially populated backend must not be trusted either. */
    PspDisplayBackend partial = fake_backend;
    partial.set_frame_buffer = NULL;
    CHECK(!psp_display_begin(&display, &partial));
    CHECK(!psp_display_healthy(&display));
    return true;
}

/*
 * The CPU composes through the cached alias, so the buffer must be written
 * back before scanout is pointed at it. Composing through the uncached alias
 * instead skips the writeback but costs the cache for a quarter megabyte of
 * stores every frame, which is visible as input lag on the device.
 */
static bool test_publish_writes_back_before_scanout(void)
{
    CHECK(fake_reset());
    PspDisplay display;
    CHECK(psp_display_begin(&display, &fake_backend));

    uint16_t *back = psp_display_back_buffer(&display);
    CHECK(psp_display_publish(&display));
    CHECK(fake.flush_calls == 1);
    CHECK(fake.last_flush_address == back);
    /* The whole stride-padded buffer, not just the visible width. */
    CHECK(fake.last_flush_bytes
          == PSP_DISPLAY_BUFFER_PIXELS * sizeof(uint16_t));
    CHECK(!fake.flush_after_present);

    /* Each buffer is written back as it is published, not only the first. */
    CHECK(psp_display_publish(&display));
    CHECK(fake.flush_calls == 2);
    CHECK(fake.last_flush_address == psp_display_front_buffer(&display));
    CHECK(!fake.flush_after_present);
    return true;
}

static bool test_rearm_reasserts_last_front_without_rotation(void)
{
    CHECK(fake_reset());
    PspDisplay display;
    CHECK(psp_display_begin(&display, &fake_backend));
    uint16_t *first_back = psp_display_back_buffer(&display);
    CHECK(psp_display_publish(&display));
    uint16_t *front = psp_display_front_buffer(&display);
    unsigned back_after_publish = display.back_buffer;
    CHECK(front == first_back);

    CHECK(psp_display_rearm(&display));
    CHECK(display.rearms == 1);
    CHECK(display.rearm_failures == 0);
    CHECK(display.last_rearm_error == 0);
    CHECK(display.back_buffer == back_after_publish);
    CHECK(fake.set_mode_calls == 2);
    CHECK(fake.present_calls == 2);
    CHECK(fake.last_address == front);
    CHECK(fake.last_stride == PSP_DISPLAY_STRIDE);
    CHECK(fake.last_format == PSP_DISPLAY_FORMAT_RGB565);
    CHECK(fake.last_sync == PSP_DISPLAY_SYNC_NEXT_FRAME);
    CHECK(fake.flush_calls == 2);
    CHECK(fake.last_flush_address == front);
    CHECK(fake.vblank_waits == 2);
    return true;
}

static bool test_rearm_failure_is_observable(void)
{
    CHECK(fake_reset());
    PspDisplay display;
    CHECK(psp_display_begin(&display, &fake_backend));
    CHECK(psp_display_publish(&display));
    unsigned back_after_publish = display.back_buffer;
    fake.present_result = -77;

    CHECK(!psp_display_rearm(&display));
    CHECK(display.rearms == 1);
    CHECK(display.rearm_failures == 1);
    CHECK(display.last_rearm_error == -77);
    CHECK(display.back_buffer == back_after_publish);
    CHECK(display.presents == 1);
    CHECK(display.rejections == 0);
    CHECK(!psp_display_healthy(&display));
    return true;
}

/*
 * The fullscreen-video scanout format.
 *
 * Video is the one surface whose bytes must reach the panel unconverted, so
 * scanout runs in 8888 while a decoded frame owns the screen. Two things can
 * go badly wrong and neither is visible from inside a single present: a
 * 16-bit composer writing while the panel is latched on the 32-bit surface,
 * which is the whole screen turned to noise, and a surface that overlaps
 * another and silently corrupts it. Pin the format, the byte counts, the
 * layout, and the mutual exclusion of the two sets of accessors.
 */
static bool test_video_surface_switches_format_and_buffers(void)
{
    CHECK(fake_reset());
    PspDisplay display;
    CHECK(psp_display_begin(&display, &fake_backend));
    CHECK(!psp_display_video_active(&display));
    CHECK(psp_display_video_back_buffer(&display) == NULL);
    CHECK(psp_display_video_overlay_scratch(&display) == NULL);
    CHECK(psp_display_publish(&display));
    CHECK(psp_display_front_buffer(&display) != NULL);
    int presents_before = fake.present_calls;

    /* Entering performs no syscall: the panel keeps showing the last complete
       16-bit frame until a complete 32-bit one replaces it. */
    CHECK(psp_display_video_begin(&display));
    CHECK(psp_display_video_active(&display));
    CHECK(fake.present_calls == presents_before);
    CHECK(display.surface_entries == 1);
    /* And the 16-bit accessors refuse, so a composer that forgot to leave
       fails loudly instead of writing 565 rows into a 32-bit buffer. */
    CHECK(psp_display_back_buffer(&display) == NULL);
    CHECK(psp_display_front_buffer(&display) == NULL);

    uint32_t *video_back = psp_display_video_back_buffer(&display);
    uint32_t *video_front = psp_display_video_front_buffer(&display);
    uint32_t *texture = psp_display_video_texture(&display);
    uint16_t *scratch = psp_display_video_overlay_scratch(&display);
    CHECK(video_back != NULL && video_front != NULL);
    CHECK(texture != NULL && scratch != NULL);
    CHECK(video_back != video_front);
    /*
     * The video layout: two 32-bit buffers at the base, the staged texture
     * above them, the scratch above that, all inside 2 MiB. The texture is in
     * EDRAM because the device measured the engine's read of a main-memory
     * one at 10,312us against 456us to write the frame.
     */
    unsigned char *base = (unsigned char *) fake.memory;
    unsigned char *slot0 = base + PSP_DISPLAY_VIDEO_BASE_BYTES;
    unsigned char *slot1 = slot0 + PSP_DISPLAY_VIDEO_BUFFER_BYTES;
    /* Which of the pair is back and which is front follows the rotation; both
       slots are the two the layout names. */
    CHECK(((unsigned char *) video_back == slot0
           && (unsigned char *) video_front == slot1)
          || ((unsigned char *) video_back == slot1
              && (unsigned char *) video_front == slot0));
    CHECK((unsigned char *) texture
              == base + PSP_DISPLAY_VIDEO_TEXTURE_BASE_BYTES);
    CHECK((unsigned char *) scratch
              == base + PSP_DISPLAY_OVERLAY_BASE_BYTES);
    CHECK(PSP_DISPLAY_OVERLAY_BASE_BYTES
              + PSP_DISPLAY_BUFFER_PIXELS * sizeof(uint16_t)
          <= PSP_DISPLAY_EDRAM_BYTES);
    /* The four video-mode regions must not overlap each other, whatever the
       arithmetic above is edited to. */
    CHECK((unsigned char *) texture
          >= slot1 + PSP_DISPLAY_VIDEO_BUFFER_BYTES);
    CHECK((unsigned char *) scratch
          >= (unsigned char *) texture + PSP_DISPLAY_VIDEO_TEXTURE_BYTES);
    /* It holds the shipping texture: 512 columns of a 272-row surface. */
    CHECK(PSP_DISPLAY_VIDEO_TEXTURE_BYTES >= (size_t) 512 * 272 * 4);
    /* And it deliberately lies over the page buffers, which is what pays for
       it -- so entering video mode must be understood to destroy them. */
    CHECK(PSP_DISPLAY_VIDEO_BASE_BYTES == 0);

    /* Write the video buffers so the page buffers underneath them are
       genuinely destroyed; a clear that is only trivially true proves
       nothing. */
    for (size_t at = 0; at < PSP_DISPLAY_VIDEO_BUFFER_PIXELS; at++) {
        video_back[at] = UINT32_C(0xdeadbeef);
        video_front[at] = UINT32_C(0xdeadbeef);
    }
    CHECK(psp_display_publish(&display));
    CHECK(fake.last_format == PSP_DISPLAY_FORMAT_RGBA8888);
    CHECK(fake.last_stride == PSP_DISPLAY_STRIDE);
    CHECK(fake.last_sync == PSP_DISPLAY_SYNC_NEXT_FRAME);
    CHECK(fake.last_address == video_back);
    /* Four bytes a pixel, or scanout reads half the frame stale. */
    CHECK(fake.last_flush_address == video_back);
    CHECK(fake.last_flush_bytes
          == PSP_DISPLAY_VIDEO_BUFFER_PIXELS * sizeof(uint32_t));
    CHECK(!fake.flush_after_present);
    CHECK(psp_display_video_back_buffer(&display) == video_front);

    /* Leaving cannot wait for the next publish: it reasserts the last 16-bit
       front buffer immediately, because what runs next writes that address. */
    int presents_before_exit = fake.present_calls;
    CHECK(psp_display_video_end(&display));
    CHECK(!psp_display_video_active(&display));
    CHECK(fake.present_calls == presents_before_exit + 1);
    CHECK(fake.last_format == PSP_DISPLAY_FORMAT_RGB565);
    CHECK(fake.last_flush_bytes
          == PSP_DISPLAY_BUFFER_PIXELS * sizeof(uint16_t));
    CHECK(display.surface_exits == 1);
    /* Both surfaces share one rotation, so which 16-bit buffer is reasserted
       follows the video presents left behind. It must be one of the three
       page slots, and the address handed to scanout must be the one the
       accessors now describe. */
    uint16_t *restored = psp_display_front_buffer(&display);
    CHECK(restored != NULL && fake.last_address == restored);
    CHECK(restored >= fake.memory
          && restored < fake.memory
              + PSP_DISPLAY_PAGE_BUFFER_COUNT
                    * PSP_DISPLAY_BUFFER_PIXELS);
    CHECK(((size_t) (restored - fake.memory)
              % PSP_DISPLAY_BUFFER_PIXELS) == 0u);
    CHECK(restored != psp_display_back_buffer(&display));
    /*
     * The video layout was written over the page buffers, so what is being
     * reasserted holds video bytes. It must be cleared first: one frame of
     * black while the next present composes the page, not one frame of a
     * 32-bit picture read as 565.
     */
    for (size_t at = 0; at < PSP_DISPLAY_BUFFER_PIXELS; at++)
        CHECK(restored[at] == 0);

    /* Idempotent, which is what lets every exit path assert it blindly. */
    presents_before_exit = fake.present_calls;
    CHECK(psp_display_video_end(&display));
    CHECK(fake.present_calls == presents_before_exit);
    CHECK(display.surface_exits == 1);
    CHECK(psp_display_healthy(&display));
    return true;
}

/* A display service that refuses the 16-bit reassert must not leave the
   process believing it is back on the page's surface without saying so. */
static bool test_video_surface_exit_failure_is_observable(void)
{
    CHECK(fake_reset());
    PspDisplay display;
    CHECK(psp_display_begin(&display, &fake_backend));
    CHECK(psp_display_publish(&display));
    CHECK(psp_display_video_begin(&display));
    fake.present_result = -9;
    CHECK(!psp_display_video_end(&display));
    CHECK(display.surface_failures == 1);
    CHECK(display.last_surface_error == -9);
    CHECK(!psp_display_healthy(&display));
    /* The accessors still follow the surface field, so the caller composes
       into the buffer scanout will be pointed at on the next publish. */
    CHECK(!psp_display_video_active(&display));
    CHECK(psp_display_back_buffer(&display) != NULL);
    return true;
}

/* Resume reasserts whatever surface is current, in its own format. */
static bool test_rearm_follows_the_active_surface(void)
{
    CHECK(fake_reset());
    PspDisplay display;
    CHECK(psp_display_begin(&display, &fake_backend));
    CHECK(psp_display_video_begin(&display));
    CHECK(psp_display_publish(&display));
    uint32_t *front = psp_display_video_front_buffer(&display);
    CHECK(psp_display_rearm(&display));
    CHECK(fake.last_format == PSP_DISPLAY_FORMAT_RGBA8888);
    CHECK(fake.last_address == front);
    CHECK(fake.last_flush_bytes
          == PSP_DISPLAY_VIDEO_BUFFER_PIXELS * sizeof(uint32_t));
    return true;
}

int main(void)
{
    bool ok = test_publish_uses_next_frame_and_claims_the_mode()
        && test_rejected_present_is_a_failure_not_a_frame()
        && test_buffers_rotate_and_stay_distinct()
        && test_refused_mode_is_reported()
        && test_unusable_backend_is_refused()
        && test_publish_writes_back_before_scanout()
        && test_rearm_reasserts_last_front_without_rotation()
        && test_rearm_failure_is_observable()
        && test_video_surface_switches_format_and_buffers()
        && test_video_surface_exit_failure_is_observable()
        && test_rearm_follows_the_active_surface();
    free(fake.memory);
    if (!ok) return 1;
    puts("psp-display-tests: ok");
    return 0;
}
