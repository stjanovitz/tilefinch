/*
 * Validation-only WebGL-to-GE cost probe.
 *
 * This is deliberately not a WebGL implementation. It answers the question
 * that must precede one: whether the proposed runtime workloads fit the PSP's
 * page-mode EDRAM tail and what reaches the graphics engine after translation.
 * Wall time under PPSSPP is advisory; list bytes, draw calls, vertices,
 * uploads and synchronizations are invariant workload facts.
 */

#include "psp_webgl_ge_probe.h"

#include "tilefinch/psp_display.h"
#include "tilefinch/js_runtime.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#if defined(__PSP__)

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include <pspge.h>
#include <pspgu.h>
#include <pspgum.h>
#include <pspkernel.h>
#pragma GCC diagnostic pop

#define PROBE_WARMUP_FRAMES 12u
#define PROBE_MEASURED_FRAMES 120u
#define PROBE_LIST_BYTES (64u * 1024u)
#define PROBE_UNCACHED UINT32_C(0x40000000)
#define PROBE_PHYSICAL_MASK UINT32_C(0x1fffffff)

#define PROBE_PAGE_BYTES \
    (PSP_DISPLAY_BUFFER_PIXELS * sizeof(uint16_t) \
     * PSP_DISPLAY_PAGE_BUFFER_COUNT)
#define PROBE_COLOR_OFFSET PROBE_PAGE_BYTES
#define PROBE_COLOR_BYTES \
    (PSP_DISPLAY_BUFFER_PIXELS * sizeof(uint32_t))
#define PROBE_DEPTH_OFFSET (PROBE_COLOR_OFFSET + PROBE_COLOR_BYTES)
#define PROBE_DEPTH_BYTES \
    (PSP_DISPLAY_BUFFER_PIXELS * sizeof(uint16_t))
#define PROBE_TEXTURE_OFFSET (PROBE_DEPTH_OFFSET + PROBE_DEPTH_BYTES)
#define PROBE_TEXTURE_EDGE 256u
#define PROBE_TEXTURE_BYTES \
    ((size_t) PROBE_TEXTURE_EDGE * PROBE_TEXTURE_EDGE * sizeof(uint32_t))
#define PROBE_TEXTURE_CACHE_BYTES \
    (PSP_DISPLAY_EDRAM_BYTES - PROBE_TEXTURE_OFFSET)

#define PROBE_CPU_VERTICES 2046u
#define PROBE_SPRITES 64u
#define PROBE_PRESSURE_DRAWS 64u
#define PROBE_CONVERSION_SOURCE_WIDTH 320u
#define PROBE_CONVERSION_SOURCE_HEIGHT 180u
#define PROBE_CONVERSION_DEST_WIDTH 480u
#define PROBE_CONVERSION_DEST_HEIGHT 270u

_Static_assert(PROBE_COLOR_OFFSET == (size_t) 0x0cc000,
               "the probe color surface must follow three page buffers");
_Static_assert(PROBE_DEPTH_OFFSET == (size_t) 0x154000,
               "the probe depth surface must follow the color surface");
_Static_assert(PROBE_TEXTURE_OFFSET == (size_t) 0x198000,
               "the probe texture cache must follow the depth surface");
_Static_assert(PROBE_TEXTURE_BYTES <= PROBE_TEXTURE_CACHE_BYTES,
               "one 256x256 RGBA texture must fit the residual EDRAM cache");

typedef struct {
    uint32_t color;
    float x;
    float y;
    float z;
} ProbeColorVertex;

typedef struct {
    float u;
    float v;
    uint32_t color;
    float x;
    float y;
    float z;
} ProbeTextureVertex;

static unsigned int __attribute__((aligned(64)))
    probe_list[PROBE_LIST_BYTES / sizeof(unsigned int)];
static ProbeColorVertex __attribute__((aligned(64)))
    probe_cpu_vertices[PROBE_CPU_VERTICES];
static uint32_t __attribute__((aligned(64)))
    probe_upload_source[PROBE_TEXTURE_EDGE * PROBE_TEXTURE_EDGE];

static const ProbeColorVertex probe_cube_vertices[36] = {
    {0xff4040ffu,-1,-1, 1},{0xff4040ffu, 1,-1, 1},{0xff4040ffu, 1, 1, 1},
    {0xff4040ffu,-1,-1, 1},{0xff4040ffu, 1, 1, 1},{0xff4040ffu,-1, 1, 1},
    {0xff40ff40u, 1,-1,-1},{0xff40ff40u,-1,-1,-1},{0xff40ff40u,-1, 1,-1},
    {0xff40ff40u, 1,-1,-1},{0xff40ff40u,-1, 1,-1},{0xff40ff40u, 1, 1,-1},
    {0xffff4040u,-1,-1,-1},{0xffff4040u,-1,-1, 1},{0xffff4040u,-1, 1, 1},
    {0xffff4040u,-1,-1,-1},{0xffff4040u,-1, 1, 1},{0xffff4040u,-1, 1,-1},
    {0xffffff40u, 1,-1, 1},{0xffffff40u, 1,-1,-1},{0xffffff40u, 1, 1,-1},
    {0xffffff40u, 1,-1, 1},{0xffffff40u, 1, 1,-1},{0xffffff40u, 1, 1, 1},
    {0xff40ffffu,-1, 1, 1},{0xff40ffffu, 1, 1, 1},{0xff40ffffu, 1, 1,-1},
    {0xff40ffffu,-1, 1, 1},{0xff40ffffu, 1, 1,-1},{0xff40ffffu,-1, 1,-1},
    {0xffff40ffu,-1,-1,-1},{0xffff40ffu, 1,-1,-1},{0xffff40ffu, 1,-1, 1},
    {0xffff40ffu,-1,-1,-1},{0xffff40ffu, 1,-1, 1},{0xffff40ffu,-1,-1, 1}
};

typedef enum {
    PROBE_SCENE_CUBE = 0,
    PROBE_SCENE_SPRITES,
    PROBE_SCENE_DRAW_PRESSURE,
    PROBE_SCENE_CPU_VERTEX,
    PROBE_SCENE_TEXTURE_UPLOAD
} ProbeSceneKind;

static void *probe_uncached_list(void)
{
    return (void *) ((uintptr_t) probe_list | PROBE_UNCACHED);
}

static void *probe_edram_at(void *base, size_t offset)
{
    return (unsigned char *) base + offset;
}

static uint64_t probe_now(void)
{
    return (uint64_t) sceKernelGetSystemTimeWide();
}

static void probe_fill_texture(uint32_t salt)
{
    for (unsigned y = 0; y < PROBE_TEXTURE_EDGE; y++) {
        for (unsigned x = 0; x < PROBE_TEXTURE_EDGE; x++) {
            uint32_t checker = ((x >> 4) ^ (y >> 4)) & 1u;
            uint32_t red = checker ? 0xd0u : 0x28u;
            uint32_t green = ((x + salt) & 0xffu);
            uint32_t blue = ((y + salt * 3u) & 0xffu);
            probe_upload_source[(size_t) y * PROBE_TEXTURE_EDGE + x] =
                UINT32_C(0xff000000) | blue << 16 | green << 8 | red;
        }
    }
}

static void probe_target_state(bool depth)
{
    sceGuDrawBufferList(
        GU_PSM_8888, (void *) (uintptr_t) PROBE_COLOR_OFFSET,
        PSP_DISPLAY_STRIDE);
    sceGuDepthBuffer(
        (void *) (uintptr_t) PROBE_DEPTH_OFFSET, PSP_DISPLAY_STRIDE);
    sceGuOffset(
        2048 - PSP_DISPLAY_SCREEN_WIDTH / 2,
        2048 - PSP_DISPLAY_SCREEN_HEIGHT / 2);
    sceGuViewport(
        2048, 2048, PSP_DISPLAY_SCREEN_WIDTH, PSP_DISPLAY_SCREEN_HEIGHT);
    sceGuScissor(
        0, 0, PSP_DISPLAY_SCREEN_WIDTH, PSP_DISPLAY_SCREEN_HEIGHT);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDisable(GU_LIGHTING);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDisable(GU_BLEND);
    sceGuDisable(GU_CULL_FACE);
    sceGuDisable(GU_DITHER);
    sceGuDepthMask(depth ? GU_FALSE : GU_TRUE);
    if (depth) {
        sceGuEnable(GU_DEPTH_TEST);
        sceGuDepthFunc(GU_GEQUAL);
        sceGuDepthRange(65535, 0);
        sceGuClearDepth(0);
    } else {
        sceGuDisable(GU_DEPTH_TEST);
    }
    /* Match production's alpha:false resolve contract: alpha aliases the
       stencil plane on a 8888 GE target, so clear it opaque once and mask it
       from subsequent geometry writes. */
    sceGuPixelMask(0u);
    sceGuClearColor(0xff18120cu);
    sceGuClearStencil(0xffu);
    sceGuClear((depth ? GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT
                      : GU_COLOR_BUFFER_BIT) | GU_STENCIL_BUFFER_BIT);
    sceGuPixelMask(UINT32_C(0xff000000));
}

static void probe_texture_state(const uint32_t *texture)
{
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(GU_PSM_8888, 0, 0, GU_FALSE);
    sceGuTexImage(
        0, PROBE_TEXTURE_EDGE, PROBE_TEXTURE_EDGE,
        PROBE_TEXTURE_EDGE, texture);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuTexFilter(GU_NEAREST, GU_NEAREST);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);
    sceGuTexFlush();
}

static void probe_draw_cube(unsigned frame)
{
    ProbeColorVertex *vertices = sceGuGetMemory(sizeof(probe_cube_vertices));
    if (vertices == NULL) return;
    memcpy(vertices, probe_cube_vertices, sizeof(probe_cube_vertices));
    sceGuDisable(GU_TEXTURE_2D);
    sceGuShadeModel(GU_SMOOTH);
    sceGumMatrixMode(GU_PROJECTION);
    sceGumLoadIdentity();
    sceGumPerspective(
        58.0f, (float) PSP_DISPLAY_SCREEN_WIDTH / PSP_DISPLAY_SCREEN_HEIGHT,
        0.5f, 32.0f);
    sceGumMatrixMode(GU_VIEW);
    sceGumLoadIdentity();
    ScePspFVector3 translate = {0.0f, 0.0f, -5.0f};
    sceGumTranslate(&translate);
    sceGumMatrixMode(GU_MODEL);
    sceGumLoadIdentity();
    ScePspFVector3 rotation = {
        (float) frame * 0.013f, (float) frame * 0.019f, 0.0f};
    sceGumRotateXYZ(&rotation);
    sceGumDrawArray(
        GU_TRIANGLES,
        GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D,
        36, NULL, vertices);
}

static ProbeTextureVertex *probe_make_sprites(unsigned count)
{
    ProbeTextureVertex *vertices =
        sceGuGetMemory((int) (count * 2u * sizeof(*vertices)));
    if (vertices == NULL) return NULL;
    for (unsigned at = 0; at < count; at++) {
        unsigned column = at & 7u;
        unsigned row = at >> 3;
        float x0 = (float) (column * 60u);
        float y0 = (float) (row * 34u);
        float x1 = x0 + 60.0f;
        float y1 = y0 + 34.0f;
        vertices[at * 2u] = (ProbeTextureVertex) {
            0, 0, 0xffffffffu, x0, y0, 0};
        vertices[at * 2u + 1u] = (ProbeTextureVertex) {
            256, 256, 0xffffffffu, x1, y1, 0};
    }
    return vertices;
}

static void probe_draw_sprites(const uint32_t *texture)
{
    probe_texture_state(texture);
    ProbeTextureVertex *vertices = probe_make_sprites(PROBE_SPRITES);
    if (vertices == NULL) return;
    sceGuDrawArray(
        GU_SPRITES,
        GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF
            | GU_TRANSFORM_2D,
        (int) (PROBE_SPRITES * 2u), NULL, vertices);
}

static void probe_draw_pressure(const uint32_t *texture)
{
    probe_texture_state(texture);
    for (unsigned at = 0; at < PROBE_PRESSURE_DRAWS; at++) {
        ProbeTextureVertex *vertices = probe_make_sprites(1);
        if (vertices == NULL) return;
        float shift_x = (float) ((at & 7u) * 60u);
        float shift_y = (float) ((at >> 3) * 34u);
        vertices[0].x += shift_x;
        vertices[0].y += shift_y;
        vertices[1].x += shift_x;
        vertices[1].y += shift_y;
        if ((at & 1u) != 0u) sceGuEnable(GU_BLEND);
        else sceGuDisable(GU_BLEND);
        sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
        sceGuDrawArray(
            GU_SPRITES,
            GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF
                | GU_TRANSFORM_2D,
            2, NULL, vertices);
    }
    sceGuDisable(GU_BLEND);
}

static void probe_prepare_cpu_vertices(unsigned frame)
{
    const float phase = (float) (frame & 63u) * 0.03125f;
    for (unsigned at = 0; at < PROBE_CPU_VERTICES; at++) {
        float source_x = (float) ((at * 37u) % 480u) - 240.0f;
        float source_y = (float) ((at * 53u) % 272u) - 136.0f;
        float x = source_x * 0.997f - source_y * 0.077f + phase;
        float y = source_x * 0.077f + source_y * 0.997f - phase;
        probe_cpu_vertices[at] = (ProbeColorVertex) {
            0xff80c0ffu,
            x + 240.0f,
            y + 136.0f,
            0.0f
        };
    }
    sceKernelDcacheWritebackRange(
        probe_cpu_vertices, sizeof(probe_cpu_vertices));
}

static void probe_draw_cpu_vertices(void)
{
    sceGuDisable(GU_TEXTURE_2D);
    sceGuShadeModel(GU_FLAT);
    sceGuDrawArray(
        GU_TRIANGLES,
        GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D,
        PROBE_CPU_VERTICES, NULL, probe_cpu_vertices);
}

static void probe_draw_full_texture(const uint32_t *texture)
{
    probe_texture_state(texture);
    ProbeTextureVertex *vertices = probe_make_sprites(1);
    if (vertices == NULL) return;
    vertices[0].x = 0.0f;
    vertices[0].y = 0.0f;
    vertices[1].x = (float) PSP_DISPLAY_SCREEN_WIDTH;
    vertices[1].y = (float) PSP_DISPLAY_SCREEN_HEIGHT;
    sceGuDrawArray(
        GU_SPRITES,
        GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF
            | GU_TRANSFORM_2D,
        2, NULL, vertices);
}

static uint32_t probe_checksum(
    const uint32_t *surface, unsigned frame, ProbeSceneKind kind)
{
    uint32_t hash = UINT32_C(2166136261);
    for (unsigned y = 7u; y < PSP_DISPLAY_SCREEN_HEIGHT; y += 31u) {
        for (unsigned x = 5u; x < PSP_DISPLAY_SCREEN_WIDTH; x += 47u) {
            hash ^= surface[(size_t) y * PSP_DISPLAY_STRIDE + x];
            hash *= UINT32_C(16777619);
        }
    }
    hash ^= frame;
    hash ^= (uint32_t) kind << 24;
    return hash;
}

static void probe_scene_defaults(
    PspWebglGeProbeScene *scene, ProbeSceneKind kind)
{
    static const char *const names[PSP_WEBGL_GE_PROBE_SCENE_COUNT] = {
        "native-cube", "sprite-batch", "draw-pressure",
        "cpu-vertex", "texture-upload"
    };
    static const unsigned draws[PSP_WEBGL_GE_PROBE_SCENE_COUNT] = {
        1u, 1u, PROBE_PRESSURE_DRAWS, 1u, 1u
    };
    static const unsigned vertices[PSP_WEBGL_GE_PROBE_SCENE_COUNT] = {
        36u, PROBE_SPRITES * 2u, PROBE_PRESSURE_DRAWS * 2u,
        PROBE_CPU_VERTICES, 2u
    };
    memset(scene, 0, sizeof(*scene));
    scene->name = names[kind];
    scene->frames = PROBE_MEASURED_FRAMES;
    scene->draw_calls_per_frame = draws[kind];
    scene->vertices_per_frame = vertices[kind];
    scene->upload_bytes_per_frame =
        kind == PROBE_SCENE_TEXTURE_UPLOAD ? PROBE_TEXTURE_BYTES : 0u;
}

static bool probe_run_scene(
    ProbeSceneKind kind, uint32_t *color, uint32_t *texture,
    PspWebglGeProbeScene *scene, unsigned *synchronizations)
{
    probe_scene_defaults(scene, kind);
    unsigned total_frames = PROBE_WARMUP_FRAMES + PROBE_MEASURED_FRAMES;
    for (unsigned frame = 0; frame < total_frames; frame++) {
        uint64_t frame_started = probe_now();
        uint64_t prepare_started = frame_started;
        if (kind == PROBE_SCENE_CPU_VERTEX)
            probe_prepare_cpu_vertices(frame);
        if (kind == PROBE_SCENE_TEXTURE_UPLOAD) {
            /* Keep the census focused on upload cost. Generating 65,536
               texels on the CPU each frame is not part of a WebGL upload;
               mutate one cache line so the source cannot be treated as an
               invariant blob while leaving the transfer size realistic. */
            size_t sample = (size_t) frame * 16u
                % (sizeof(probe_upload_source)
                   / sizeof(probe_upload_source[0]));
            probe_upload_source[sample] ^= UINT32_C(0x00010101);
            memcpy(texture, probe_upload_source, PROBE_TEXTURE_BYTES);
            sceKernelDcacheWritebackRange(texture, PROBE_TEXTURE_BYTES);
        }
        uint64_t prepared = probe_now();
        sceKernelDcacheWritebackInvalidateRange(color, PROBE_COLOR_BYTES);
        uint64_t emit_started = probe_now();
        if (sceGuStart(GU_DIRECT, probe_uncached_list()) < 0) return false;
        probe_target_state(kind == PROBE_SCENE_CUBE);
        switch (kind) {
            case PROBE_SCENE_CUBE: probe_draw_cube(frame); break;
            case PROBE_SCENE_SPRITES: probe_draw_sprites(texture); break;
            case PROBE_SCENE_DRAW_PRESSURE:
                probe_draw_pressure(texture); break;
            case PROBE_SCENE_CPU_VERTEX: probe_draw_cpu_vertices(); break;
            case PROBE_SCENE_TEXTURE_UPLOAD:
                probe_draw_full_texture(texture); break;
        }
        int list_bytes = sceGuFinish();
        uint64_t emitted = probe_now();
        if (list_bytes <= 0 || (unsigned) list_bytes > PROBE_LIST_BYTES)
            return false;
        uint64_t wait_started = emitted;
        if (sceGuSync(GU_SYNC_FINISH, GU_SYNC_WAIT) < 0) return false;
        uint64_t finished = probe_now();
        (*synchronizations)++;
        sceKernelDcacheInvalidateRange(color, PROBE_COLOR_BYTES);
        if (frame < PROBE_WARMUP_FRAMES) continue;
        uint64_t prepare_us = prepared - prepare_started;
        uint64_t emit_us = emitted - emit_started;
        uint64_t wait_us = finished - wait_started;
        uint64_t total_us = finished - frame_started;
        scene->prepare_us += prepare_us;
        scene->emit_us += emit_us;
        scene->wait_us += wait_us;
        scene->total_us += total_us;
        scene->list_bytes += (size_t) list_bytes;
        if (total_us > scene->maximum_frame_us)
            scene->maximum_frame_us = total_us;
        if ((size_t) list_bytes > scene->maximum_list_bytes)
            scene->maximum_list_bytes = (size_t) list_bytes;
    }
    scene->checksum = probe_checksum(
        color, total_frames - 1u, kind);
    /* Hardware-rendered PPSSPP keeps the color target in a host FBO, so a
       CPU checksum is diagnostic rather than a correctness oracle there.
       The physical-device follow-up can qualify rendered pixels. */
    scene->passed = scene->maximum_list_bytes <= PROBE_LIST_BYTES;
    return scene->passed;
}

/* Match the WebGL bottom-left scissor (100,50,50,50) against the exact
   top-left pixel rectangle used by the software renderer. This catches the
   easy-to-miss GU contract: the final two arguments are dimensions, not
   right/bottom edges. */
static bool probe_offset_scissor(
    uint32_t *color, unsigned *synchronizations,
    PspWebglGeProbeReport *report)
{
    if (color == NULL || synchronizations == NULL || report == NULL)
        return false;
    const int left = 100, top = PSP_DISPLAY_SCREEN_HEIGHT - 50 - 50;
    const uint32_t background = UINT32_C(0xff18120c);
    const uint32_t foreground = UINT32_C(0xff30d070);
    sceKernelDcacheWritebackInvalidateRange(color, PROBE_COLOR_BYTES);
    int start_result = sceGuStart(GU_DIRECT, probe_uncached_list());
    if (start_result < 0) {
        snprintf(report->detail, sizeof(report->detail),
                 "offset scissor start failed (0x%08x)",
                 (unsigned) start_result);
        return false;
    }
    probe_target_state(false);
    sceGuScissor(left, top, 50, 50);
    /* GU clear commands have their own clear-region behavior and therefore
       cannot qualify the scissor contract used by WebGL draw commands. Draw
       an ordinary full-screen sprite through the exact production state. */
    ProbeColorVertex *vertices = sceGuGetMemory(2 * sizeof(*vertices));
    if (vertices == NULL) {
        snprintf(report->detail, sizeof(report->detail),
                 "offset scissor vertex allocation failed");
        (void) sceGuFinish();
        (void) sceGuSync(GU_SYNC_FINISH, GU_SYNC_WAIT);
        return false;
    }
    vertices[0] = (ProbeColorVertex) {foreground, 0, 0, 0};
    vertices[1] = (ProbeColorVertex) {
        foreground, PSP_DISPLAY_SCREEN_WIDTH, PSP_DISPLAY_SCREEN_HEIGHT, 0
    };
    sceGuDisable(GU_TEXTURE_2D);
    sceGuDrawArray(
        GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D,
        2, NULL, vertices);
    int finish_result = sceGuFinish();
    int sync_result = finish_result <= 0
        ? INT_MIN : sceGuSync(GU_SYNC_FINISH, GU_SYNC_WAIT);
    if (finish_result <= 0 || sync_result < 0) {
        snprintf(report->detail, sizeof(report->detail),
                 "offset scissor submit failed (%08x/%08x)",
                 (unsigned) finish_result, (unsigned) sync_result);
        return false;
    }
    (*synchronizations)++;
    sceKernelDcacheInvalidateRange(color, PROBE_COLOR_BYTES);
    uint32_t inside_a = color[(size_t) top * PSP_DISPLAY_STRIDE + left];
    uint32_t inside_b = color[
        (size_t) (top + 49) * PSP_DISPLAY_STRIDE + left + 49];
    uint32_t above = color[(size_t) (top - 1) * PSP_DISPLAY_STRIDE + left];
    uint32_t before = color[(size_t) top * PSP_DISPLAY_STRIDE + left - 1];
    uint32_t below = color[(size_t) (top + 50) * PSP_DISPLAY_STRIDE + left];
    uint32_t after = color[(size_t) top * PSP_DISPLAY_STRIDE + left + 50];
    /* Scissor is an RGB coverage contract. Alpha aliases the PSP stencil
       plane and is intentionally irrelevant to the page's RGB565 conversion;
       coupling it to this test hid correct offset-scissor results on hardware
       before the conversion probe could run. */
    const uint32_t rgb = UINT32_C(0x00ffffff);
    bool passed = (inside_a & rgb) == (foreground & rgb)
        && (inside_b & rgb) == (foreground & rgb)
        && (above & rgb) == (background & rgb)
        && (before & rgb) == (background & rgb)
        && (below & rgb) == (background & rgb)
        && (after & rgb) == (background & rgb);
    if (!passed) snprintf(
        report->detail, sizeof(report->detail),
        "offset scissor pixels %08x/%08x %08x/%08x/%08x/%08x",
        (unsigned) inside_a, (unsigned) inside_b, (unsigned) above,
        (unsigned) before, (unsigned) below, (unsigned) after);
    return passed;
}

static bool probe_realm_cache_incarnation(
    uint32_t *color, uint32_t *texture, unsigned *synchronizations)
{
    if (color == NULL || texture == NULL || synchronizations == NULL)
        return false;
    ScriptWebglCacheAdmission owner = {0};
    const uint32_t colors[2] = {
        UINT32_C(0xff2030d0), UINT32_C(0xff20d040)
    };
    for (uint32_t realm = 1u; realm <= 2u; realm++) {
        bool reset = false;
        /* The canvas handle and all page-controlled texture metadata are
           intentionally identical. Only the native realm epoch changes. */
        if (!script_runtime_webgl_cache_admit(
                &owner, 0u, realm, INT64_C(0x0000000100000001), &reset)
            || !reset || owner.cached_entries != 0
            || owner.cached_bytes != 0) return false;
        for (size_t pixel = 0;
             pixel < (size_t) PROBE_TEXTURE_EDGE * PROBE_TEXTURE_EDGE;
             pixel++) texture[pixel] = colors[realm - 1u];
        sceKernelDcacheWritebackRange(texture, PROBE_TEXTURE_BYTES);
        owner.cached_entries = 1u;
        owner.cached_bytes = PROBE_TEXTURE_BYTES;

        sceKernelDcacheWritebackInvalidateRange(color, PROBE_COLOR_BYTES);
        if (sceGuStart(GU_DIRECT, probe_uncached_list()) < 0) return false;
        probe_target_state(false);
        probe_draw_full_texture(texture);
        if (sceGuFinish() <= 0
            || sceGuSync(GU_SYNC_FINISH, GU_SYNC_WAIT) < 0) return false;
        (*synchronizations)++;
        sceKernelDcacheInvalidateRange(color, PROBE_COLOR_BYTES);
        uint32_t sampled = color[
            (size_t) (PSP_DISPLAY_SCREEN_HEIGHT / 2) * PSP_DISPLAY_STRIDE
            + PSP_DISPLAY_SCREEN_WIDTH / 2];
        if ((sampled & UINT32_C(0x00ffffff))
            != (colors[realm - 1u] & UINT32_C(0x00ffffff))) return false;
    }
    return true;
}

static uint32_t probe_convert_pixel(uint32_t pixel)
{
    uint32_t red, green, blue;
    __asm__("ext %0,%1,3,5" : "=r" (red) : "r" (pixel));
    __asm__("ext %0,%1,10,6" : "=r" (green) : "r" (pixel));
    __asm__("ext %0,%1,19,5" : "=r" (blue) : "r" (pixel));
    __asm__("ins %0,%1,5,6" : "+r" (red) : "r" (green));
    __asm__("ins %0,%1,11,5" : "+r" (red) : "r" (blue));
    return red;
}

static uint32_t probe_convert_pair(uint32_t low, uint32_t high)
{
    __asm__("ins %0,%1,16,16" : "+r" (low) : "r" (high));
    return low;
}

static void probe_cpu_convert_row(
    uint16_t *output, const uint32_t *input)
{
    uint32_t *pairs = (uint32_t *) (void *) output;
    for (unsigned x = 0; x < PROBE_CONVERSION_SOURCE_WIDTH; x += 4u) {
        uint32_t p0 = probe_convert_pixel(input[x]);
        uint32_t p1 = probe_convert_pixel(input[x + 1u]);
        uint32_t p2 = probe_convert_pixel(input[x + 2u]);
        uint32_t p3 = probe_convert_pixel(input[x + 3u]);
        *pairs++ = probe_convert_pair(p0, p0);
        *pairs++ = probe_convert_pair(p1, p2);
        *pairs++ = probe_convert_pair(p2, p3);
    }
}

static void probe_fill_conversion_source(uint32_t *color)
{
    for (unsigned y = 0; y < PROBE_CONVERSION_SOURCE_HEIGHT; y++) {
        uint32_t *row = color + (size_t) y * PSP_DISPLAY_STRIDE;
        for (unsigned x = 0; x < PROBE_CONVERSION_SOURCE_WIDTH; x++) {
            unsigned red = (x * 13u + y * 3u + 17u) & 0xffu;
            unsigned green = (x * 5u + y * 11u + 29u) & 0xffu;
            unsigned blue = (x * 7u + y * 19u + 43u) & 0xffu;
            row[x] = UINT32_C(0xff000000)
                | blue << 16 | green << 8 | red;
        }
    }
    sceKernelDcacheWritebackRange(color, PROBE_COLOR_BYTES);
}

/* The exact common shipping kernel: a 320x180 drawing buffer is expanded by
   nearest neighbour to 480x270, with every fourth source-pixel group becoming
   p0,p0,p1,p2,p2,p3 and repeated destination rows copied instead of converted
   twice. Keep this probe-local; it measures the current path without making
   the validation object part of shipping render ownership. */
static void probe_cpu_convert_3_to_2(
    uint16_t *destination, const uint32_t *source)
{
    for (unsigned source_y = 0, destination_y = 0;
         source_y < PROBE_CONVERSION_SOURCE_HEIGHT;
         source_y += 4u, destination_y += 6u) {
        uint16_t *output = destination
            + (size_t) destination_y * PROBE_CONVERSION_DEST_WIDTH;
        const uint32_t *input = source
            + (size_t) source_y * PSP_DISPLAY_STRIDE;
        probe_cpu_convert_row(output, input);
        memcpy(output + PROBE_CONVERSION_DEST_WIDTH, output,
               PROBE_CONVERSION_DEST_WIDTH * sizeof(*output));
        probe_cpu_convert_row(
            output + PROBE_CONVERSION_DEST_WIDTH * 2u,
            input + PSP_DISPLAY_STRIDE);
        probe_cpu_convert_row(
            output + PROBE_CONVERSION_DEST_WIDTH * 3u,
            input + PSP_DISPLAY_STRIDE * 2u);
        memcpy(output + PROBE_CONVERSION_DEST_WIDTH * 4u,
               output + PROBE_CONVERSION_DEST_WIDTH * 3u,
               PROBE_CONVERSION_DEST_WIDTH * sizeof(*output));
        probe_cpu_convert_row(
            output + PROBE_CONVERSION_DEST_WIDTH * 5u,
            input + PSP_DISPLAY_STRIDE * 3u);
    }
}

static void probe_copy_conversion_to_page(
    uint16_t *page, const uint16_t *converted)
{
    for (unsigned y = 0; y < PROBE_CONVERSION_DEST_HEIGHT; y++) memcpy(
        page + (size_t) y * PSP_DISPLAY_STRIDE,
        converted + (size_t) y * PROBE_CONVERSION_DEST_WIDTH,
        PROBE_CONVERSION_DEST_WIDTH * sizeof(*page));
}

static uint32_t probe_checksum_565(
    const uint16_t *pixels, size_t stride)
{
    uint32_t hash = UINT32_C(2166136261);
    for (unsigned y = 0; y < PROBE_CONVERSION_DEST_HEIGHT; y++) {
        for (unsigned x = 0; x < PROBE_CONVERSION_DEST_WIDTH; x++) {
            uint16_t pixel = pixels[(size_t) y * stride + x];
            hash ^= pixel & 0xffu; hash *= UINT32_C(16777619);
            hash ^= pixel >> 8; hash *= UINT32_C(16777619);
        }
    }
    return hash;
}

static bool probe_ge_convert_3_to_2(
    uint16_t *page, uint32_t *color,
    uint64_t *submit_us, uint64_t *wait_us, uint64_t *total_us)
{
    uintptr_t base = (uintptr_t) sceGeEdramGetAddr() & PROBE_PHYSICAL_MASK;
    uintptr_t target = (uintptr_t) page & PROBE_PHYSICAL_MASK;
    if (target < base || target - base >= PSP_DISPLAY_EDRAM_BYTES)
        return false;
    uint64_t started = probe_now();
    if (sceGuStart(GU_DIRECT, probe_uncached_list()) < 0) return false;
    sceGuDrawBufferList(
        GU_PSM_5650, (void *) (target - base), PSP_DISPLAY_STRIDE);
    sceGuOffset(
        2048 - PROBE_CONVERSION_DEST_WIDTH / 2,
        2048 - PROBE_CONVERSION_DEST_HEIGHT / 2);
    sceGuViewport(
        2048, 2048,
        PROBE_CONVERSION_DEST_WIDTH, PROBE_CONVERSION_DEST_HEIGHT);
    sceGuScissor(
        0, 0, PROBE_CONVERSION_DEST_WIDTH, PROBE_CONVERSION_DEST_HEIGHT);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDisable(GU_DEPTH_TEST); sceGuDepthMask(GU_TRUE);
    sceGuDisable(GU_ALPHA_TEST); sceGuDisable(GU_BLEND);
    sceGuDisable(GU_DITHER); sceGuEnable(GU_TEXTURE_2D);
    sceGuPixelMask(0u);
    sceGuTexMode(GU_PSM_8888, 0, 0, GU_FALSE);
    sceGuTexImage(0, 512, 256, PSP_DISPLAY_STRIDE, color);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
    sceGuTexFilter(GU_NEAREST, GU_NEAREST);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuTexScale(1.0f, 1.0f); sceGuTexOffset(0.0f, 0.0f);
    sceGuTexFlush();
    ProbeTextureVertex *vertices = sceGuGetMemory(2 * sizeof(*vertices));
    if (vertices == NULL) {
        (void) sceGuFinish();
        (void) sceGuSync(GU_SYNC_FINISH, GU_SYNC_WAIT);
        return false;
    }
    vertices[0] = (ProbeTextureVertex) {
        0, 0, 0xffffffffu, 0, 0, 0};
    vertices[1] = (ProbeTextureVertex) {
        PROBE_CONVERSION_SOURCE_WIDTH, PROBE_CONVERSION_SOURCE_HEIGHT,
        0xffffffffu,
        PROBE_CONVERSION_DEST_WIDTH, PROBE_CONVERSION_DEST_HEIGHT, 0};
    sceGuDrawArray(
        GU_SPRITES,
        GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF
            | GU_TRANSFORM_2D,
        2, NULL, vertices);
    int list_bytes = sceGuFinish();
    uint64_t submitted = probe_now();
    if (list_bytes <= 0
        || sceGuSync(GU_SYNC_FINISH, GU_SYNC_WAIT) < 0) return false;
    uint64_t synchronized = probe_now();
    sceKernelDcacheInvalidateRange(
        page, PSP_DISPLAY_BUFFER_PIXELS * sizeof(*page));
    uint64_t completed = probe_now();
    if (submit_us != NULL) *submit_us = submitted - started;
    if (wait_us != NULL) *wait_us = synchronized - submitted;
    if (total_us != NULL) *total_us = completed - started;
    return true;
}

static bool probe_conversion_cost(
    uint16_t *page, uint16_t *cpu, size_t cpu_pixels, uint32_t *color,
    PspWebglGeConversionProbe *result, unsigned *synchronizations)
{
    if (page == NULL || cpu == NULL || color == NULL || result == NULL
        || synchronizations == NULL
        || cpu_pixels < (size_t) PSP_DISPLAY_SCREEN_WIDTH
                            * PSP_DISPLAY_SCREEN_HEIGHT) return false;
    memset(result, 0, sizeof(*result));
    probe_fill_conversion_source(color);
    unsigned total_frames = PROBE_WARMUP_FRAMES + PROBE_MEASURED_FRAMES;
    for (unsigned frame = 0; frame < total_frames; frame++) {
        uint64_t convert_started = probe_now();
        probe_cpu_convert_3_to_2(cpu, color);
        uint64_t converted = probe_now();
        probe_copy_conversion_to_page(page, cpu);
        uint64_t copied = probe_now();
        if (frame < PROBE_WARMUP_FRAMES) continue;
        uint64_t convert_us = converted - convert_started;
        uint64_t copy_us = copied - converted;
        result->cpu_convert_us += convert_us;
        result->cpu_copy_us += copy_us;
        if (convert_us > result->cpu_convert_max_us)
            result->cpu_convert_max_us = convert_us;
        if (copy_us > result->cpu_copy_max_us)
            result->cpu_copy_max_us = copy_us;
    }
    result->cpu_checksum = probe_checksum_565(
        cpu, PROBE_CONVERSION_DEST_WIDTH);
    /* Discard dirty CPU cache lines for the scanout target before GE owns it;
       otherwise a later writeback could overwrite the conversion we measure. */
    sceKernelDcacheWritebackInvalidateRange(
        page, PSP_DISPLAY_BUFFER_PIXELS * sizeof(*page));
    for (unsigned frame = 0; frame < total_frames; frame++) {
        uint64_t submit_us = 0, wait_us = 0, total_us = 0;
        if (!probe_ge_convert_3_to_2(
                page, color, &submit_us, &wait_us, &total_us)) return false;
        (*synchronizations)++;
        if (frame < PROBE_WARMUP_FRAMES) continue;
        result->ge_submit_us += submit_us;
        result->ge_wait_us += wait_us;
        result->ge_total_us += total_us;
        if (total_us > result->ge_total_max_us)
            result->ge_total_max_us = total_us;
    }
    result->ge_checksum = probe_checksum_565(page, PSP_DISPLAY_STRIDE);
    for (unsigned y = 0; y < PROBE_CONVERSION_DEST_HEIGHT; y++) {
        for (unsigned x = 0; x < PROBE_CONVERSION_DEST_WIDTH; x++) {
            result->compared_pixels++;
            if (page[(size_t) y * PSP_DISPLAY_STRIDE + x]
                != cpu[(size_t) y * PROBE_CONVERSION_DEST_WIDTH + x]) {
                result->mismatched_pixels++;
            }
        }
    }
    result->frames = PROBE_MEASURED_FRAMES;
    result->available = true;
    result->pixel_exact = result->mismatched_pixels == 0;
    return true;
}

static bool probe_composite_page(uint16_t *page, uint32_t *color)
{
    if (page == NULL || color == NULL) return false;
    uintptr_t base = (uintptr_t) sceGeEdramGetAddr() & PROBE_PHYSICAL_MASK;
    uintptr_t target = (uintptr_t) page & PROBE_PHYSICAL_MASK;
    if (target < base || target - base >= PSP_DISPLAY_EDRAM_BYTES)
        return false;
    size_t page_bytes = PSP_DISPLAY_BUFFER_PIXELS * sizeof(*page);
    sceKernelDcacheWritebackInvalidateRange(page, page_bytes);
    if (sceGuStart(GU_DIRECT, probe_uncached_list()) < 0) return false;
    sceGuDrawBufferList(
        GU_PSM_5650, (void *) (target - base), PSP_DISPLAY_STRIDE);
    sceGuOffset(
        2048 - PSP_DISPLAY_SCREEN_WIDTH / 2,
        2048 - PSP_DISPLAY_SCREEN_HEIGHT / 2);
    sceGuViewport(
        2048, 2048, PSP_DISPLAY_SCREEN_WIDTH, PSP_DISPLAY_SCREEN_HEIGHT);
    sceGuScissor(
        0, 0, PSP_DISPLAY_SCREEN_WIDTH, PSP_DISPLAY_SCREEN_HEIGHT);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDepthMask(GU_TRUE);
    sceGuDisable(GU_BLEND);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(GU_PSM_8888, 0, 0, GU_FALSE);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
    sceGuTexFilter(GU_NEAREST, GU_NEAREST);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);
    for (unsigned strip = 0; strip < 2u; strip++) {
        unsigned source_y = strip == 0u ? 0u : 256u;
        unsigned rows = strip == 0u ? 256u : 16u;
        sceGuTexImage(
            0, 512, rows, PSP_DISPLAY_STRIDE,
            color + (size_t) source_y * PSP_DISPLAY_STRIDE);
        sceGuTexFlush();
        ProbeTextureVertex *vertices = sceGuGetMemory(2 * sizeof(*vertices));
        if (vertices == NULL) {
            (void) sceGuFinish();
            (void) sceGuSync(GU_SYNC_FINISH, GU_SYNC_WAIT);
            return false;
        }
        vertices[0] = (ProbeTextureVertex) {
            0, 0, 0xffffffffu, 0, (float) source_y, 0};
        vertices[1] = (ProbeTextureVertex) {
            480, (float) rows, 0xffffffffu,
            480, (float) (source_y + rows), 0};
        sceGuDrawArray(
            GU_SPRITES,
            GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF
                | GU_TRANSFORM_2D,
            2, NULL, vertices);
    }
    if (sceGuFinish() <= 0) return false;
    if (sceGuSync(GU_SYNC_FINISH, GU_SYNC_WAIT) < 0) return false;
    sceKernelDcacheInvalidateRange(page, page_bytes);
    return true;
}

bool psp_webgl_ge_probe_run(
    uint16_t *page_destination,
    uint16_t *cpu_destination, size_t cpu_destination_pixels,
    PspWebglGeProbeReport *report)
{
    if (report == NULL) return false;
    memset(report, 0, sizeof(*report));
    report->color_bytes = PROBE_COLOR_BYTES;
    report->depth_bytes = PROBE_DEPTH_BYTES;
    report->texture_cache_bytes = PROBE_TEXTURE_CACHE_BYTES;
    void *edram = sceGeEdramGetAddr();
    if (edram == NULL || page_destination == NULL) {
        snprintf(report->detail, sizeof(report->detail),
                 "EDRAM or page destination unavailable");
        return false;
    }
    uint32_t *color = probe_edram_at(edram, PROBE_COLOR_OFFSET);
    uint32_t *texture = probe_edram_at(edram, PROBE_TEXTURE_OFFSET);
    probe_fill_texture(0u);
    memcpy(texture, probe_upload_source, PROBE_TEXTURE_BYTES);
    sceKernelDcacheWritebackRange(texture, PROBE_TEXTURE_BYTES);
    if (sceGuInit() < 0) {
        snprintf(report->detail, sizeof(report->detail), "sceGuInit failed");
        return false;
    }
    report->context_initializations = 1u;
    bool passed = true;
    for (unsigned at = 0; at < PSP_WEBGL_GE_PROBE_SCENE_COUNT; at++) {
        if (!probe_run_scene(
                (ProbeSceneKind) at, color, texture, &report->scenes[at],
                &report->synchronizations)) {
            snprintf(report->detail, sizeof(report->detail),
                     "%s scene failed",
                     report->scenes[at].name == NULL
                         ? "unknown" : report->scenes[at].name);
            passed = false;
            break;
        }
    }
    if (passed && !probe_offset_scissor(
            color, &report->synchronizations, report)) {
        if (report->detail[0] == '\0') snprintf(
            report->detail, sizeof(report->detail),
            "offset scissor did not match software bounds");
        passed = false;
    }
    if (passed && !probe_realm_cache_incarnation(
            color, texture, &report->synchronizations)) {
        snprintf(report->detail, sizeof(report->detail),
                 "realm cache incarnation reused stale texture pixels");
        passed = false;
    }
    if (passed && !probe_conversion_cost(
            page_destination, cpu_destination, cpu_destination_pixels,
            color, &report->conversion, &report->synchronizations)) {
        snprintf(report->detail, sizeof(report->detail),
                 "GE conversion cost probe failed");
        passed = false;
    }
    if (passed && !probe_composite_page(page_destination, color)) {
        snprintf(report->detail, sizeof(report->detail),
                 "page composite failed");
        passed = false;
    }
    sceGuTerm();
    report->passed = passed;
    if (passed)
        snprintf(report->detail, sizeof(report->detail), "all scenes passed");
    return passed;
}

#else

bool psp_webgl_ge_probe_run(
    uint16_t *page_destination,
    uint16_t *cpu_destination, size_t cpu_destination_pixels,
    PspWebglGeProbeReport *report)
{
    (void) page_destination;
    (void) cpu_destination;
    (void) cpu_destination_pixels;
    if (report != NULL) {
        memset(report, 0, sizeof(*report));
        snprintf(report->detail, sizeof(report->detail), "PSP-only probe");
    }
    return false;
}

#endif
