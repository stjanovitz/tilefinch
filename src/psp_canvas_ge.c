#include "psp_canvas_ge.h"

#include "tilefinch/psp_display.h"
#include "tilefinch/psp_media_present.h"
#include "tilefinch/resources.h"

#include <string.h>

#if defined(__PSP__)

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include <pspge.h>
#include <pspgu.h>
#include <pspkernel.h>
#pragma GCC diagnostic pop

#define CANVAS_GE_STRIP_TEXELS 32u
#define CANVAS_GE_MAX_SOURCE_WIDTH 320u
#define CANVAS_GE_MAX_HEIGHT 272u
#define CANVAS_GE_VERTICES \
    ((CANVAS_GE_MAX_SOURCE_WIDTH / CANVAS_GE_STRIP_TEXELS) \
     * CANVAS_GE_MAX_HEIGHT * 2u)
#define CANVAS_GE_UNCACHED UINT32_C(0x40000000)
#define CANVAS_GE_PHYSICAL_MASK UINT32_C(0x1fffffff)

typedef struct {
    float u;
    float v;
    int16_t x;
    int16_t y;
    int16_t z;
    int16_t pad;
} CanvasGeVertex;

static CanvasGeVertex __attribute__((aligned(64)))
    canvas_ge_vertices[CANVAS_GE_VERTICES];
static unsigned int __attribute__((aligned(64))) canvas_ge_list[256];
static unsigned canvas_ge_vertex_count;
static int canvas_ge_key[5] = {-1, -1, -1, -1, -1};

static unsigned texture_extent(int value)
{
    unsigned extent = 1u;
    while (extent < (unsigned) value && extent < 512u) extent <<= 1;
    return extent;
}

/* Strip-major sprites; each source row y spans the destination rows the
   CPU kernel floor-maps to it: [ceil(y*H/h), ceil((y+1)*H/h)). */
static bool canvas_ge_prepare_vertices(const RenderCanvasDeferral *d)
{
    int key[5] = {d->source_width, d->source_height, d->height, d->x, d->y};
    if (memcmp(key, canvas_ge_key, sizeof(key)) == 0
        && canvas_ge_vertex_count != 0) return true;
    unsigned source_width = (unsigned) d->source_width;
    unsigned source_height = (unsigned) d->source_height;
    unsigned height = (unsigned) d->height;
    unsigned count = 0;
    for (unsigned sx = 0; sx < source_width; sx += CANVAS_GE_STRIP_TEXELS) {
        unsigned ex = sx + CANVAS_GE_STRIP_TEXELS;
        if (ex > source_width) ex = source_width;
        for (unsigned y = 0; y < source_height; y++) {
            unsigned top = (y * height + source_height - 1u) / source_height;
            unsigned bottom = ((y + 1u) * height + source_height - 1u)
                / source_height;
            if (count + 2u > CANVAS_GE_VERTICES) return false;
            canvas_ge_vertices[count++] = (CanvasGeVertex) {
                (float) sx - .25f, (float) y,
                (int16_t) (d->x + (int) (sx * 3u / 2u)),
                (int16_t) (d->y + (int) top), 0, 0};
            canvas_ge_vertices[count++] = (CanvasGeVertex) {
                (float) ex - .25f, (float) (y + 1u),
                (int16_t) (d->x + (int) (ex * 3u / 2u)),
                (int16_t) (d->y + (int) bottom), 0, 0};
        }
    }
    sceKernelDcacheWritebackRange(
        canvas_ge_vertices, count * sizeof(canvas_ge_vertices[0]));
    canvas_ge_vertex_count = count;
    memcpy(canvas_ge_key, key, sizeof(key));
    return true;
}

/* A list normally completes in about 0.7 ms. Spin briefly (the owner
   thread would wait anyway), then poll with short sleeps so lower-priority
   threads run, and give up after a bound far beyond any healthy list. */
#define CANVAS_GE_SPIN_US 2000u
#define CANVAS_GE_POLL_SLEEP_US 200u
#define CANVAS_GE_TIMEOUT_US 50000u

static bool canvas_ge_outstanding;

static bool canvas_ge_list_running(void)
{
    return sceGuSync(GU_SYNC_FINISH, GU_SYNC_NOWAIT) != 0;
}

bool psp_canvas_ge_busy(void *context)
{
    (void) context;
    if (canvas_ge_outstanding && !canvas_ge_list_running())
        canvas_ge_outstanding = false;
    return canvas_ge_outstanding;
}

void psp_canvas_ge_fail(void *context, const char *reason)
{
    (void) context;
    psp_media_present_ge_latch_failure(reason);
}

int psp_canvas_ge_scale(void *context, uint16_t *back,
                        const RenderCanvasDeferral *d,
                        unsigned row_top, unsigned row_end,
                        uint64_t *elapsed_us)
{
    (void) context;
    if (elapsed_us != NULL) *elapsed_us = 0;
    if (back == NULL || d == NULL || d->source == NULL
        || row_top >= row_end || row_end > PSP_DISPLAY_SCREEN_HEIGHT
        || d->source_width <= 0
        || (unsigned) d->source_width > CANVAS_GE_MAX_SOURCE_WIDTH
        || d->source_width % 4 != 0 || d->source_height <= 0
        || d->height > (int) CANVAS_GE_MAX_HEIGHT
        || d->width != d->source_width / 2 * 3
        || d->x != 0 || d->y != 0
        || d->width != PSP_DISPLAY_SCREEN_WIDTH
        || d->height != PSP_DISPLAY_SCREEN_HEIGHT
        || d->source_stride % 64u != 0 || d->source_stride / 4u > 1024u
        || d->serial != image_canvas_native_serial()
        || d->epoch != psp_display_edram_content_epoch()
        || !psp_media_present_ge_context_acquire()
        || !psp_media_present_ge_context_idle()
        || !canvas_ge_prepare_vertices(d)) return 0;
    void *edram = sceGeEdramGetAddr();
    if (edram == NULL) return 0;
    uintptr_t base = (uintptr_t) edram & CANVAS_GE_PHYSICAL_MASK;
    uintptr_t target = (uintptr_t) back & CANVAS_GE_PHYSICAL_MASK;
    uintptr_t source = (uintptr_t) d->source & CANVAS_GE_PHYSICAL_MASK;
    if (target < base || target - base >= PSP_DISPLAY_EDRAM_BYTES
        || source < base || source - base >= PSP_DISPLAY_EDRAM_BYTES)
        return 0;
    uint64_t started = (uint64_t) sceKernelGetSystemTimeWide();
    /* No dirty CPU line may later be written back over GE output, and no
       clean stale line may be read by chrome composition afterwards. The
       whole-cache form is a constant 16 KiB walk (6 us measured). */
    sceKernelDcacheWritebackInvalidateAll();
    int dither = sceGuGetStatus(GU_DITHER);
    if (sceGuStart(GU_DIRECT, (void *) ((uintptr_t) canvas_ge_list
                                        | CANVAS_GE_UNCACHED)) < 0)
        return 0;
    sceGuDrawBufferList(GU_PSM_5650, (void *) (target - base),
                        PSP_DISPLAY_STRIDE);
    sceGuOffset(2048 - PSP_DISPLAY_SCREEN_WIDTH / 2,
                2048 - PSP_DISPLAY_SCREEN_HEIGHT / 2);
    sceGuViewport(2048, 2048, PSP_DISPLAY_SCREEN_WIDTH,
                  PSP_DISPLAY_SCREEN_HEIGHT);
    /* This SDK's sceGuScissor takes a width and height. */
    sceGuScissor(0, (int) row_top, PSP_DISPLAY_SCREEN_WIDTH,
                 (int) (row_end - row_top));
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDisable(GU_DEPTH_TEST); sceGuDepthMask(GU_TRUE);
    sceGuDisable(GU_ALPHA_TEST); sceGuDisable(GU_BLEND);
    sceGuDisable(GU_DITHER); sceGuEnable(GU_TEXTURE_2D);
    sceGuPixelMask(0u);
    sceGuTexMode(GU_PSM_8888, 0, 0, GU_FALSE);
    sceGuTexImage(0, (int) texture_extent(d->source_width),
                  (int) texture_extent(d->source_height),
                  (int) (d->source_stride / 4u), d->source);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
    sceGuTexFilter(GU_NEAREST, GU_NEAREST);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuTexScale(1.0f, 1.0f); sceGuTexOffset(0.0f, 0.0f);
    sceGuTexFlush();
    sceGuDrawArray(GU_SPRITES,
                   GU_TEXTURE_32BITF | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                   (int) canvas_ge_vertex_count, NULL, canvas_ge_vertices);
    /* WebGL never sets dither; leave the shared context as found. */
    if (dither) sceGuEnable(GU_DITHER);
    int list_bytes = sceGuFinish();
    bool complete = false;
    for (;;) {
        if (!canvas_ge_list_running()) {
            complete = true;
            break;
        }
        uint64_t waited = (uint64_t) sceKernelGetSystemTimeWide() - started;
        if (waited >= CANVAS_GE_TIMEOUT_US) break;
        if (waited >= CANVAS_GE_SPIN_US)
            sceKernelDelayThread(CANVAS_GE_POLL_SLEEP_US);
    }
    if (elapsed_us != NULL)
        *elapsed_us = (uint64_t) sceKernelGetSystemTimeWide() - started;
    if (!complete) canvas_ge_outstanding = true;
    return list_bytes > 0 && complete ? 1 : -1;
}

#else

int psp_canvas_ge_scale(void *context, uint16_t *back,
                        const RenderCanvasDeferral *deferral,
                        unsigned row_top, unsigned row_end,
                        uint64_t *elapsed_us)
{
    (void) context; (void) back; (void) deferral;
    (void) row_top; (void) row_end;
    if (elapsed_us != NULL) *elapsed_us = 0;
    return 0;
}

bool psp_canvas_ge_busy(void *context)
{
    (void) context;
    return false;
}

void psp_canvas_ge_fail(void *context, const char *reason)
{
    (void) context; (void) reason;
}

#endif
