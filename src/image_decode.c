/* Keep the single-header raster codecs and the PSP JPEG worker out of the
   DOM/resource loader translation unit. They change rarely and are costly to
   compile; ordinary image scheduling edits should not rebuild them. */
#include <stdatomic.h>
#include "tilefinch/psp_fpu.h"
#include <limits.h>
#include <stdint.h>
#include <string.h>

#include "image_decode_internal.h"
#include "image_svg_decode_internal.h"
#include "tilefinch/integer_math.h"
#include "tilefinch/platform.h"

#if defined(__PSP__)
#include <pspkernel.h>

#include "tilefinch/psp_threads.h"
#include "psp_thread_contract.h"
#endif

#include <webp/decode.h>

#undef budget_malloc
#undef budget_realloc
#define budget_malloc(b, s) budget_malloc_category((b), BUDGET_CATEGORY_RESOURCE, (s))
#define budget_realloc(b, p, s) budget_realloc_category((b), BUDGET_CATEGORY_RESOURCE, (p), (s))

typedef struct {
    unsigned char *base;
    size_t capacity;
    size_t used;
    bool exhausted;
} ImageDecodeArena;

typedef union {
    struct {
        size_t size;
    } value;
    max_align_t alignment;
} ImageDecodeArenaHeader;

static Budget *decode_budget;
static ImageDecodeArena *decode_arena;
/* Bytes the current decode holds through image_malloc, the libwebp hooks
   and its output, and their peak: the decode's real working set, which the
   census reports instead of the source-size product. */
static size_t decode_live_bytes;
static size_t decode_peak_bytes;

static void image_decode_charged(const void *pointer)
{
    if (pointer == NULL) return;
    decode_live_bytes += budget_usable_size(pointer);
    if (decode_live_bytes > decode_peak_bytes)
        decode_peak_bytes = decode_live_bytes;
}

static void image_decode_discharged(const void *pointer)
{
    if (pointer == NULL) return;
    size_t bytes = budget_usable_size(pointer);
    decode_live_bytes -= bytes < decode_live_bytes ? bytes : decode_live_bytes;
}

void image_point_sample_rgba(const unsigned char *source, int source_width,
                             int source_height, unsigned char *target,
                             int target_width, int target_height)
{
    for (int y = 0; y < target_height; y++) {
        const unsigned char *row = source
            + (size_t) image_point_sample_index(y, source_height,
                                                target_height)
              * (size_t) source_width * 4u;
        unsigned char *out = target + (size_t) y * (size_t) target_width * 4u;
        for (int x = 0; x < target_width; x++) {
            memcpy(out + (size_t) x * 4u,
                   row + (size_t) image_point_sample_index(
                             x, source_width, target_width) * 4u,
                   4u);
        }
    }
}

size_t image_decode_last_peak_bytes(void)
{
    return decode_peak_bytes;
}
static atomic_bool decode_gate = ATOMIC_VAR_INIT(false);
static atomic_bool image_worker_decode_pending = ATOMIC_VAR_INIT(false);

static bool image_decode_begin_budget(Budget *budget)
{
    bool expected = false;
    if (budget == NULL || atomic_load_explicit(
            &image_worker_decode_pending, memory_order_acquire)
        || !atomic_compare_exchange_strong_explicit(
            &decode_gate, &expected, true,
            memory_order_acquire, memory_order_relaxed)) return false;
    decode_budget = budget;
    decode_arena = NULL;
    return true;
}

static bool image_decode_begin_arena(ImageDecodeArena *arena)
{
    bool expected = false;
    if (arena == NULL || !atomic_compare_exchange_strong_explicit(
            &decode_gate, &expected, true,
            memory_order_acquire, memory_order_relaxed)) return false;
    decode_budget = NULL;
    decode_arena = arena;
    return true;
}

static void image_decode_end(void)
{
    decode_budget = NULL;
    decode_arena = NULL;
    atomic_store_explicit(&decode_gate, false, memory_order_release);
}

bool image_decode_busy(void)
{
    return atomic_load_explicit(
               &image_worker_decode_pending, memory_order_acquire)
        || atomic_load_explicit(&decode_gate, memory_order_acquire);
}

static void *image_arena_malloc(size_t size)
{
    ImageDecodeArena *arena = decode_arena;
    if (arena == NULL) return NULL;
    if (size == 0) size = 1;
    const size_t alignment = _Alignof(max_align_t);
    size_t header_at = arena->used;
    size_t remainder = header_at % alignment;
    if (remainder != 0) header_at += alignment - remainder;
    if (header_at > arena->capacity
        || sizeof(ImageDecodeArenaHeader) > arena->capacity - header_at
        || size > arena->capacity - header_at
                         - sizeof(ImageDecodeArenaHeader)) {
        arena->exhausted = true;
        return NULL;
    }
    ImageDecodeArenaHeader *header =
        (ImageDecodeArenaHeader *) (void *) (arena->base + header_at);
    header->value.size = size;
    arena->used = header_at + sizeof(*header) + size;
    return header + 1;
}

static void *image_arena_realloc(void *pointer, size_t size)
{
    if (pointer == NULL) return image_arena_malloc(size);
    if (size == 0) return NULL;
    ImageDecodeArenaHeader *header =
        ((ImageDecodeArenaHeader *) pointer) - 1;
    void *replacement = image_arena_malloc(size);
    if (replacement != NULL) {
        size_t copied = header->value.size < size
            ? header->value.size : size;
        memcpy(replacement, pointer, copied);
    }
    return replacement;
}

static bool image_decode_is_webp(const unsigned char *encoded, size_t length)
{
    return encoded != NULL && length >= 12
        && memcmp(encoded, "RIFF", 4) == 0
        && memcmp(encoded + 8, "WEBP", 4) == 0;
}

/* libwebp allocates only through WebPSafeMalloc/WebPSafeCalloc/WebPSafeFree,
   and cmake/TilefinchDependencies.cmake compiles its utils.c with malloc,
   calloc and free renamed to these. They charge the decode's Budget while a
   WebP decode is in progress and refuse otherwise, so libwebp's real scratch
   (row caches, alpha and lossless planes) is accounted exactly. */
static Budget *webp_budget;

void *tilefinch_webp_malloc(size_t size);
void *tilefinch_webp_calloc(size_t count, size_t size);
void tilefinch_webp_free(void *pointer);

void *tilefinch_webp_malloc(size_t size)
{
    void *pointer = webp_budget == NULL ? NULL
        : budget_malloc(webp_budget, size);
    image_decode_charged(pointer);
    return pointer;
}

void *tilefinch_webp_calloc(size_t count, size_t size)
{
    void *pointer = webp_budget == NULL ? NULL
        : budget_calloc_category(
              webp_budget, BUDGET_CATEGORY_RESOURCE, count, size);
    image_decode_charged(pointer);
    return pointer;
}

void tilefinch_webp_free(void *pointer)
{
    if (pointer == NULL) return;
    image_decode_discharged(pointer);
    budget_free(webp_budget, pointer);
}

static unsigned char *image_decode_webp_scaled(
    const unsigned char *encoded, size_t encoded_length,
    int target_width, int target_height, int *source_width,
    int *source_height, int *components, bool *interrupted)
{
    if (interrupted != NULL) *interrupted = false;
    if (decode_budget == NULL
        || !image_decode_is_webp(encoded, encoded_length)
        || target_width <= 0 || target_height <= 0
        || encoded_length > INT32_MAX) return NULL;
    WebPDecoderConfig config;
    if (!WebPInitDecoderConfig(&config)
        || WebPGetFeatures(encoded, encoded_length, &config.input)
               != VP8_STATUS_OK
        || config.input.width <= 0 || config.input.height <= 0
        || config.input.has_animation
        || target_width > config.input.width
        || target_height > config.input.height) return NULL;
    const int decoded_source_width = config.input.width;
    const int decoded_source_height = config.input.height;
    if ((size_t) target_width > SIZE_MAX / (size_t) target_height
        || (size_t) target_width * (size_t) target_height
               > SIZE_MAX / 4u) return NULL;
    size_t target_pixels = (size_t) target_width * (size_t) target_height;

    size_t target_bytes = target_pixels * 4u;
    unsigned char *output = budget_malloc(decode_budget, target_bytes);
    if (output == NULL) return NULL;
    image_decode_charged(output);
    config.options.use_scaling = target_width != config.input.width
                                 || target_height != config.input.height;
    config.options.scaled_width = target_width;
    config.options.scaled_height = target_height;
    config.options.use_threads = 0;
    config.output.colorspace = MODE_RGBA;
    config.output.is_external_memory = 1;
    config.output.u.RGBA.rgba = output;
    config.output.u.RGBA.stride = target_width * 4;
    config.output.u.RGBA.size = target_bytes;
    /* The one-shot decoder can spend hundreds of milliseconds inside one
       upstream call on Allegrex, including when invoked by a raster cache
       miss. Feed the same decoder incrementally so input/cancel/watchdog
       service gets a checkpoint between small compressed-data windows. The
       external destination and the Budget-routed allocator keep ownership
       and memory accounting identical to the one-shot path. */
    webp_budget = decode_budget;
    WebPIDecoder *decoder = WebPIDecode(NULL, 0, &config);
    VP8StatusCode status = VP8_STATUS_SUSPENDED;
    size_t offset = 0;
    size_t work_units = 0;
    while (decoder != NULL && offset < encoded_length
           && status == VP8_STATUS_SUSPENDED) {
        size_t chunk = encoded_length - offset;
        if (chunk > 2048u) chunk = 2048u;
        status = WebPIAppend(decoder, encoded + offset, chunk);
        offset += chunk;
        work_units++;
        if (status == VP8_STATUS_SUSPENDED && offset < encoded_length
            && !tilefinch_platform_cooperate(
                   "image-webp-decode", work_units)) {
            if (interrupted != NULL) *interrupted = true;
            break;
        }
    }
    if (decoder != NULL) WebPIDelete(decoder);
    WebPFreeDecBuffer(&config.output);
    webp_budget = NULL;
    if (status != VP8_STATUS_OK) {
        image_decode_discharged(output);
        budget_free(decode_budget, output);
        return NULL;
    }
    if (source_width != NULL) *source_width = decoded_source_width;
    if (source_height != NULL) *source_height = decoded_source_height;
    if (components != NULL) *components = config.input.has_alpha ? 4 : 3;
    return output;
}

static void *image_malloc(size_t size)
{
    if (decode_arena != NULL) return image_arena_malloc(size);
    void *pointer = budget_malloc(decode_budget, size);
    image_decode_charged(pointer);
    return pointer;
}

static void *image_realloc(void *pointer, size_t size)
{
    if (decode_arena != NULL) return image_arena_realloc(pointer, size);
    size_t before = pointer == NULL ? 0 : budget_usable_size(pointer);
    void *replacement = budget_realloc(decode_budget, pointer, size);
    if (replacement != NULL) {
        decode_live_bytes -= before < decode_live_bytes
            ? before : decode_live_bytes;
        image_decode_charged(replacement);
    }
    return replacement;
}

static void image_free(void *pointer)
{
    if (decode_arena != NULL) return;
    image_decode_discharged(pointer);
    budget_free(decode_budget, pointer);
}

#define STBI_MALLOC(size) image_malloc(size)
#define STBI_REALLOC(pointer, size) image_realloc((pointer), (size))
#define STBI_FREE(pointer) image_free(pointer)
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
/* Nothing reads stbi_failure_reason() (every caller only checks for a NULL
   or zero result, which stbi__err still returns), and the flip,
   unpremultiply and iPhone-PNG flags are never set, so their per-thread
   copies always fall back to the zero globals. Without the failure strings
   and thread-local copies stb stops storing a reason string at each of its
   error sites and needs no emulated TLS on the PSP. */
#define STBI_NO_FAILURE_STRINGS
#define STBI_NO_THREAD_LOCALS
#if defined(TILEFINCH_DISABLE_GIF)
#define STBI_NO_GIF
#endif
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

ImageDecodeProbeResult image_decode_probe_info(
    Budget *budget, const unsigned char *encoded, size_t encoded_length,
    int *width, int *height, int *components, bool *is_webp)
{
    bool webp = image_decode_is_webp(encoded, encoded_length);
    if (is_webp != NULL) *is_webp = webp;
    if (webp) {
        bool supported = WebPGetInfo(
            encoded, encoded_length, width, height) != 0;
        if (supported && components != NULL) *components = 4;
        return supported ? IMAGE_DECODE_PROBE_SUPPORTED
                         : IMAGE_DECODE_PROBE_UNSUPPORTED;
    }
    if (!image_decode_begin_budget(budget)) return IMAGE_DECODE_PROBE_BUSY;
    bool supported = encoded_length <= INT32_MAX
        && stbi_info_from_memory(encoded, (int) encoded_length,
                                 width, height, components) != 0;
    image_decode_end();
    return supported ? IMAGE_DECODE_PROBE_SUPPORTED
                     : IMAGE_DECODE_PROBE_UNSUPPORTED;
}

#if !defined(TILEFINCH_DISABLE_GIF)
static bool image_is_gif(const unsigned char *encoded, size_t length)
{
    return encoded != NULL && length >= 6u
        && (memcmp(encoded, "GIF87a", 6u) == 0
            || memcmp(encoded, "GIF89a", 6u) == 0);
}

/* stb's public GIF entry point places stbi__gif (whose LZW table is roughly
   35 KiB) on the caller's stack. That exceeds the PSP thread-frame policy.
   The same internal decoder is safe when its state is charged to the bounded
   image Budget instead; Tilefinch needs only the first composited frame. */
static unsigned char *image_decode_gif_first_frame(
    const unsigned char *encoded, int encoded_length,
    int *width, int *height, int *components)
{
    if (encoded == NULL || encoded_length <= 0 || width == NULL
        || height == NULL || components == NULL) return NULL;
    stbi__context stream;
    stbi__start_mem(&stream, encoded, encoded_length);
    if (!stbi__gif_test(&stream)) return NULL;
    stbi__rewind(&stream);
    stbi__gif *gif = (stbi__gif *) stbi__malloc(sizeof(*gif));
    if (gif == NULL) return NULL;
    memset(gif, 0, sizeof(*gif));
    stbi_uc *pixels = stbi__gif_load_next(
        &stream, gif, components, 4, NULL);
    if (pixels == (stbi_uc *) &stream) pixels = NULL;
    if (pixels != NULL) {
        *width = gif->w;
        *height = gif->h;
        *components = 4;
    } else if (gif->out != NULL) {
        STBI_FREE(gif->out);
    }
    STBI_FREE(gif->history);
    STBI_FREE(gif->background);
    STBI_FREE(gif);
    return pixels;
}
#endif

#include "image_decode/row_sampler.inc"
#include "image_decode/png_stream.inc"

/* The largest IDCT reduction (1/2, 1/4 or 1/8 per dimension) that still
   leaves at least the target in both dimensions. */
static int image_jpeg_scale_shift(int source_width, int source_height,
                                  int target_width, int target_height)
{
    int shift = 0;
    while (shift < 3
           && ((source_width + (2 << shift) - 1) >> (shift + 1))
                  >= target_width
           && ((source_height + (2 << shift) - 1) >> (shift + 1))
                  >= target_height) {
        shift++;
    }
    return shift;
}

/* A JPEG shown smaller than its source is decoded near the target and never
   at full resolution. stb's entropy decoder (patched, see
   patches/stb-31c1ad37-jpeg-scaled-idct.patch) stores every 8x8 IDCT block
   box-averaged by 2^shift, so its component planes are allocated at the
   reduced size. Rows of the reduced image are then chroma-upsampled with
   stb's own kernels one at a time, and those the row sampler takes for the
   remaining factor (below two) are colour-converted into the target.
   Progressive streams still hold their full coefficient planes until the
   last scan. */
static unsigned char *image_decode_jpeg_scaled(
    const unsigned char *encoded, int encoded_length,
    int expected_width, int expected_height,
    int target_width, int target_height, int *source_width,
    int *source_height, int *components, unsigned char *external_output)
{
    if (encoded == NULL || encoded_length <= 0
        || target_width <= 0 || target_height <= 0
        || expected_width < target_width
        || expected_height < target_height) return NULL;
    /* stb does not initialize img_n in start_mem. A pre-SOF failure must
       clean up zero components rather than interpreting stack bytes. */
    stbi__context stream = {0};
    stbi__start_mem(&stream, encoded, encoded_length);
    if (!stbi__jpeg_test(&stream)) return NULL;
    stbi__rewind(&stream);
    stbi__jpeg *jpeg = (stbi__jpeg *) stbi__malloc(sizeof(*jpeg));
    if (jpeg == NULL) return NULL;
    memset(jpeg, 0, sizeof(*jpeg));
    jpeg->s = &stream;
    stbi__setup_jpeg(jpeg);
    int shift = image_jpeg_scale_shift(
        expected_width, expected_height, target_width, target_height);
    jpeg->tilefinch_scale_shift = shift;
    if (!stbi__decode_jpeg_image(jpeg) || jpeg->scan_n <= 0) {
        stbi__cleanup_jpeg(jpeg);
        STBI_FREE(jpeg);
        return NULL;
    }
    int width = stream.img_x;
    int height = stream.img_y;
    int decode_components = stream.img_n;
    int step = 1 << shift;
    int reduced_width = (width + step - 1) >> shift;
    int reduced_height = (height + step - 1) >> shift;
    if (width != expected_width || height != expected_height
        || decode_components <= 0 || decode_components > 4
        || target_width > reduced_width || target_height > reduced_height
        || (size_t) target_width > SIZE_MAX / (size_t) target_height
        || (size_t) target_width * (size_t) target_height > SIZE_MAX / 4u) {
        stbi__cleanup_jpeg(jpeg);
        STBI_FREE(jpeg);
        return NULL;
    }

    stbi__resample resample[4];
    int plane_rows[4] = {0, 0, 0, 0};
    int plane_stride[4] = {0, 0, 0, 0};
    stbi_uc *component_rows[4] = {NULL, NULL, NULL, NULL};
    bool setup_ok = true;
    for (int component = 0; setup_ok && component < decode_components;
         component++) {
        stbi__resample *row = &resample[component];
        jpeg->img_comp[component].linebuf =
            (stbi_uc *) stbi__malloc((size_t) reduced_width + 3u);
        if (jpeg->img_comp[component].linebuf == NULL) {
            setup_ok = false;
            break;
        }
        plane_rows[component] =
            (jpeg->img_comp[component].y + step - 1) >> shift;
        plane_stride[component] = jpeg->img_comp[component].w2 >> shift;
        row->hs = jpeg->img_h_max / jpeg->img_comp[component].h;
        row->vs = jpeg->img_v_max / jpeg->img_comp[component].v;
        row->ystep = row->vs >> 1;
        row->w_lores = (reduced_width + row->hs - 1) / row->hs;
        row->ypos = 0;
        row->line0 = row->line1 = jpeg->img_comp[component].data;
        if (row->hs == 1 && row->vs == 1) {
            row->resample = resample_row_1;
        } else if (row->hs == 1 && row->vs == 2) {
            row->resample = stbi__resample_row_v_2;
        } else if (row->hs == 2 && row->vs == 1) {
            row->resample = stbi__resample_row_h_2;
        } else if (row->hs == 2 && row->vs == 2) {
            row->resample = jpeg->resample_row_hv_2_kernel;
        } else {
            row->resample = stbi__resample_row_generic;
        }
    }
    bool scaled = reduced_width != target_width
        || reduced_height != target_height;
    size_t target_bytes = (size_t) target_width * (size_t) target_height * 4u;
    stbi_uc *output = setup_ok && external_output != NULL
        ? external_output
        : setup_ok ? (stbi_uc *) stbi__malloc(target_bytes) : NULL;
    stbi_uc *rgba_row = output == NULL || !scaled
        ? NULL : (stbi_uc *) stbi__malloc((size_t) reduced_width * 4u);
    ImageRowSampler sampler;
    memset(&sampler, 0, sizeof(sampler));
    if (output == NULL || (scaled && (rgba_row == NULL
            || !image_row_sampler_init(
                   &sampler, reduced_width, reduced_height, target_width,
                   target_height, output)))) setup_ok = false;

    bool source_is_rgb = stream.img_n == 3
        && (jpeg->rgb == 3
            || (jpeg->app14_color_transform == 0 && !jpeg->jfif));
    int source_y = 0;
    for (; setup_ok && source_y < reduced_height; source_y++) {
        for (int component = 0; component < decode_components; component++) {
            stbi__resample *row = &resample[component];
            int lower = row->ystep >= (row->vs >> 1);
            component_rows[component] = row->resample(
                jpeg->img_comp[component].linebuf,
                lower ? row->line1 : row->line0,
                lower ? row->line0 : row->line1,
                row->w_lores, row->hs);
            if (++row->ystep >= row->vs) {
                row->ystep = 0;
                row->line0 = row->line1;
                if (++row->ypos < plane_rows[component]) {
                    row->line1 += plane_stride[component];
                }
            }
        }
        if (scaled && !image_row_sampler_wants(&sampler)) {
            (void) image_row_sampler_push(&sampler, NULL);
            continue;
        }
        stbi_uc *out = scaled ? rgba_row
            : output + (size_t) source_y * (size_t) target_width * 4u;
        if (stream.img_n == 3 && source_is_rgb) {
            for (int x = 0; x < reduced_width; x++) {
                out[(size_t) x * 4u] = component_rows[0][x];
                out[(size_t) x * 4u + 1u] = component_rows[1][x];
                out[(size_t) x * 4u + 2u] = component_rows[2][x];
                out[(size_t) x * 4u + 3u] = 255;
            }
        } else if (stream.img_n == 3) {
            jpeg->YCbCr_to_RGB_kernel(
                out, component_rows[0], component_rows[1],
                component_rows[2], reduced_width, 4);
        } else if (stream.img_n == 4
                   && jpeg->app14_color_transform == 0) {
            for (int x = 0; x < reduced_width; x++) {
                stbi_uc multiplier = component_rows[3][x];
                out[(size_t) x * 4u] = stbi__blinn_8x8(
                    component_rows[0][x], multiplier);
                out[(size_t) x * 4u + 1u] = stbi__blinn_8x8(
                    component_rows[1][x], multiplier);
                out[(size_t) x * 4u + 2u] = stbi__blinn_8x8(
                    component_rows[2][x], multiplier);
                out[(size_t) x * 4u + 3u] = 255;
            }
        } else if (stream.img_n == 4
                   && jpeg->app14_color_transform == 2) {
            jpeg->YCbCr_to_RGB_kernel(
                out, component_rows[0], component_rows[1],
                component_rows[2], reduced_width, 4);
            for (int x = 0; x < reduced_width; x++) {
                stbi_uc multiplier = component_rows[3][x];
                out[(size_t) x * 4u] = stbi__blinn_8x8(
                    255 - out[(size_t) x * 4u], multiplier);
                out[(size_t) x * 4u + 1u] = stbi__blinn_8x8(
                    255 - out[(size_t) x * 4u + 1u], multiplier);
                out[(size_t) x * 4u + 2u] = stbi__blinn_8x8(
                    255 - out[(size_t) x * 4u + 2u], multiplier);
            }
        } else if (stream.img_n >= 3) {
            jpeg->YCbCr_to_RGB_kernel(
                out, component_rows[0], component_rows[1],
                component_rows[2], reduced_width, 4);
        } else {
            for (int x = 0; x < reduced_width; x++) {
                stbi_uc value = component_rows[0][x];
                out[(size_t) x * 4u] = value;
                out[(size_t) x * 4u + 1u] = value;
                out[(size_t) x * 4u + 2u] = value;
                out[(size_t) x * 4u + 3u] = 255;
            }
        }
        if (scaled && !image_row_sampler_push(&sampler, rgba_row)) {
            setup_ok = false;
        }
    }
    bool complete = setup_ok && source_y == reduced_height
        && (!scaled || image_row_sampler_finished(&sampler));
    image_row_sampler_release(&sampler);
    STBI_FREE(rgba_row);
    stbi__cleanup_jpeg(jpeg);
    STBI_FREE(jpeg);
    if (!complete) {
        if (external_output == NULL) STBI_FREE(output);
        return NULL;
    }
    if (source_width != NULL) *source_width = width;
    if (source_height != NULL) *source_height = height;
    if (components != NULL) *components = stream.img_n >= 3 ? 3 : 1;
    return output;
}

/* JPEG entropy decoding is the one raster operation that cannot be sliced
   inside stb without maintaining a private fork. On PSP it therefore runs
   in one lower-priority slot: input preempts the worker, while the browser
   thread remains the sole publisher of pixels and layout. The worker never
   enters Budget. Its complete allocation arena is admitted by the browser
   thread before dispatch and shrunk to the final RGBA prefix on collection. */
#define IMAGE_DECODE_WORKER_STACK_BYTES (32u * 1024u)
#define IMAGE_DECODE_WORKER_WAKE 1u
#define IMAGE_DECODE_WORKER_SCRATCH_FLOOR (128u * 1024u)

typedef enum {
    IMAGE_DECODE_WORKER_FREE = 0,
    IMAGE_DECODE_WORKER_QUEUED,
    IMAGE_DECODE_WORKER_RUNNING,
    IMAGE_DECODE_WORKER_READY
} ImageDecodeWorkerState;

typedef struct {
    atomic_uint state;
    atomic_bool stop_requested;
    atomic_bool failed;
    Budget *budget;
    BudgetReservation stack_reservation;
    BrowserSharedBody *encoded_body;
    unsigned char *arena_storage;
    size_t arena_bytes;
    size_t output_bytes;
    ImageDecodeArena arena;
    uint32_t generation;
    const unsigned char *encoded;
    size_t encoded_length;
    int target_width;
    int target_height;
    int expected_source_width;
    int expected_source_height;
    int source_width;
    int source_height;
    ImageDecodeStatus result_status;
    uint32_t abandoned_generation;
    bool initialized;
#if defined(__PSP__)
    SceUID event;
    SceUID thread;
#endif
} ImageDecodeWorker;

static ImageDecodeWorker image_decode_worker;

static void image_decode_worker_execute(ImageDecodeWorker *worker)
{
    if (worker == NULL) return;
    atomic_store_explicit(
        &worker->state, IMAGE_DECODE_WORKER_RUNNING,
        memory_order_release);
    int source_width = 0, source_height = 0, components = 0;
    unsigned char *pixels = NULL;
    bool entered = image_decode_begin_arena(&worker->arena);
    if (entered) {
        pixels = image_decode_jpeg_scaled(
            worker->encoded, (int) worker->encoded_length,
            worker->expected_source_width, worker->expected_source_height,
            worker->target_width, worker->target_height,
            &source_width, &source_height, &components,
            worker->arena_storage);
        image_decode_end();
    }
    worker->source_width = source_width;
    worker->source_height = source_height;
    worker->result_status = pixels != NULL
        && source_width > 0 && source_height > 0
        ? IMAGE_DECODE_SUCCEEDED
        : worker->arena.exhausted || !entered
          ? IMAGE_DECODE_TRANSIENT_FAILURE
          : IMAGE_DECODE_DETERMINISTIC_FAILURE;
    atomic_store_explicit(
        &worker->state, IMAGE_DECODE_WORKER_READY,
        memory_order_release);
}

#if defined(__PSP__)
static int image_decode_worker_main(SceSize argument_size, void *arguments)
{
    psp_fpu_mask_exceptions();
    ImageDecodeWorker *worker = NULL;
    if (arguments != NULL && argument_size == sizeof(worker))
        memcpy(&worker, arguments, sizeof(worker));
    if (worker == NULL) return -1;
    while (!atomic_load_explicit(
               &worker->stop_requested, memory_order_acquire)) {
        uint32_t bits = 0;
        int waited = sceKernelWaitEventFlag(
            worker->event, IMAGE_DECODE_WORKER_WAKE,
            PSP_EVENT_WAITOR | PSP_EVENT_WAITCLEAR, &bits, NULL);
        if (waited < 0) {
            atomic_store_explicit(
                &worker->failed, true, memory_order_release);
            if (atomic_load_explicit(
                    &worker->state, memory_order_acquire)
                == IMAGE_DECODE_WORKER_QUEUED) {
                worker->result_status = IMAGE_DECODE_TRANSIENT_FAILURE;
                atomic_store_explicit(
                    &worker->state, IMAGE_DECODE_WORKER_READY,
                    memory_order_release);
            }
            break;
        }
        if (atomic_load_explicit(
                &worker->stop_requested, memory_order_acquire)) break;
        if (atomic_load_explicit(
                &worker->state, memory_order_acquire)
            == IMAGE_DECODE_WORKER_QUEUED) {
            image_decode_worker_execute(worker);
        }
    }
    return 0;
}
#endif

static bool image_decode_worker_start(Budget *budget)
{
    ImageDecodeWorker *worker = &image_decode_worker;
    /* The PSP frontend owns one BrowserEngine and one Budget at a time. Keep
       that physical ownership explicit: a second host engine must use the
       synchronous fallback rather than sharing a worker whose stack and job
       arena are charged to the first engine. */
    if (worker->initialized) return worker->budget == budget;
    memset(worker, 0, sizeof(*worker));
    atomic_init(&worker->state, IMAGE_DECODE_WORKER_FREE);
    atomic_init(&worker->stop_requested, false);
    atomic_init(&worker->failed, false);
    worker->budget = budget;
#if defined(__PSP__)
    worker->event = -1;
    worker->thread = -1;
    if (!budget_reservation_acquire(
            &worker->stack_reservation, budget,
            BUDGET_CATEGORY_RESOURCE,
            IMAGE_DECODE_WORKER_STACK_BYTES)) return false;
    worker->event = sceKernelCreateEventFlag(
        "tilefinch_jpeg", PSP_EVENT_WAITSINGLE, 0, NULL);
    worker->thread = worker->event < 0 ? -1 : sceKernelCreateThread(
        "tilefinch_jpeg", image_decode_worker_main,
        TILEFINCH_PSP_THREAD_PRIORITY_IMAGE_DECODE,
        IMAGE_DECODE_WORKER_STACK_BYTES, PSP_THREAD_ATTR_USER, NULL);
    ImageDecodeWorker *pointer = worker;
    if (worker->event < 0 || worker->thread < 0
        || sceKernelStartThread(
               worker->thread, sizeof(pointer), &pointer) < 0) {
        if (worker->thread >= 0)
            (void) sceKernelDeleteThread(worker->thread);
        if (worker->event >= 0)
            (void) sceKernelDeleteEventFlag(worker->event);
        budget_reservation_release(&worker->stack_reservation);
        memset(worker, 0, sizeof(*worker));
        return false;
    }
#endif
    worker->initialized = true;
    return true;
}

static void image_decode_worker_release_job(ImageDecodeWorker *worker)
{
    if (worker == NULL) return;
    browser_shared_body_release(worker->encoded_body);
    worker->encoded_body = NULL;
    budget_free(worker->budget, worker->arena_storage);
    worker->arena_storage = NULL;
    worker->arena_bytes = 0;
    worker->output_bytes = 0;
    worker->encoded = NULL;
    worker->encoded_length = 0;
    worker->abandoned_generation = 0;
    memset(&worker->arena, 0, sizeof(worker->arena));
    atomic_store_explicit(
        &image_worker_decode_pending, false, memory_order_release);
    atomic_store_explicit(
        &worker->state, IMAGE_DECODE_WORKER_FREE,
        memory_order_release);
}

/* Worker scratch for one scaled JPEG decode, from the frame header: the
   reduced component planes (stb pads each to whole MCUs), progressive
   coefficient planes (full size, two bytes per coefficient), one reduced
   RGBA row, line buffers and the sampler's column table. The floor covers the
   decoder state, Huffman tables and allocation headers. */
static bool image_jpeg_worker_scratch(
    const unsigned char *encoded, size_t length, int target_width,
    int target_height, size_t *scratch_bytes)
{
    if (encoded == NULL || length < 4u || target_width <= 0
        || target_height <= 0
        || encoded[0] != 0xffu || encoded[1] != 0xd8u) return false;
    size_t at = 2u;
    while (at + 1u < length) {
        while (at < length && encoded[at] != 0xffu) at++;
        while (at < length && encoded[at] == 0xffu) at++;
        if (at >= length) break;
        unsigned marker = encoded[at++];
        if (marker == 0xd9u || marker == 0xdau) return false;
        if (marker == 0x01u || (marker >= 0xd0u && marker <= 0xd8u))
            continue;
        if (at + 1u >= length) break;
        size_t segment = ((size_t) encoded[at] << 8u) | encoded[at + 1u];
        if (segment < 2u || segment > length - at) break;
        if (marker != 0xc0u && marker != 0xc1u && marker != 0xc2u) {
            at += segment;
            continue;
        }
        const unsigned char *sof = encoded + at + 2u;
        if (segment < 8u) return false;
        int height = (sof[1] << 8) | sof[2];
        int width = (sof[3] << 8) | sof[4];
        int count = sof[5];
        if (width <= 0 || height <= 0 || count <= 0 || count > 4
            || segment != 8u + 3u * (size_t) count) return false;
        int h[4], v[4], h_max = 1, v_max = 1;
        for (int i = 0; i < count; i++) {
            h[i] = sof[7 + i * 3] >> 4;
            v[i] = sof[7 + i * 3] & 15;
            if (h[i] < 1 || h[i] > 4 || v[i] < 1 || v[i] > 4) return false;
            if (h[i] > h_max) h_max = h[i];
            if (v[i] > v_max) v_max = v[i];
        }
        int shift = image_jpeg_scale_shift(
            width, height, target_width, target_height);
        size_t mcu_x = ((size_t) width + (size_t) h_max * 8u - 1u)
                       / ((size_t) h_max * 8u);
        size_t mcu_y = ((size_t) height + (size_t) v_max * 8u - 1u)
                       / ((size_t) v_max * 8u);
        size_t total = IMAGE_DECODE_WORKER_SCRATCH_FLOOR;
        for (int i = 0; i < count; i++) {
            size_t plane_w = mcu_x * (size_t) h[i] * 8u;
            size_t plane_h = mcu_y * (size_t) v[i] * 8u;
            total += (plane_w >> shift) * (plane_h >> shift) + 16u;
            if (marker == 0xc2u) total += plane_w * plane_h * 2u + 16u;
        }
        size_t reduced_width = ((size_t) width + (1u << shift) - 1u) >> shift;
        total += reduced_width * (4u + 4u + (size_t) count)
            + (size_t) target_width * 64u;
        *scratch_bytes = total;
        return true;
    }
    return false;
}

ImageDecodeSubmitResult image_decode_worker_submit(
    const ImageResource *resource, Budget *budget, uint32_t *token)
{
    if (resource == NULL || budget == NULL || token == NULL
        || resource->encoded_body == NULL
        || resource->encoded == NULL || resource->encoded_length < 2u
        || resource->encoded_length > INT32_MAX
        || resource->encoded[0] != 0xffu || resource->encoded[1] != 0xd8u
        || resource->source_width <= resource->width
        || resource->source_height <= resource->height
        || resource->width <= 0 || resource->height <= 0
        || resource->source_width <= 0 || resource->source_height <= 0)
        return IMAGE_DECODE_SUBMIT_REJECTED;
    ImageDecodeWorker *worker = &image_decode_worker;
    if (!image_decode_worker_start(budget))
        return IMAGE_DECODE_SUBMIT_REJECTED;
    if (atomic_load_explicit(&worker->failed, memory_order_acquire))
        return IMAGE_DECODE_SUBMIT_REJECTED;
    unsigned state = atomic_load_explicit(
        &worker->state, memory_order_acquire);
    if (state == IMAGE_DECODE_WORKER_READY) {
        /* READY belongs to the token holder until it collects or explicitly
           abandons the generation. Never make room for a second producer by
           silently discarding another continuation's completed pixels. */
        if (worker->abandoned_generation != worker->generation)
            return IMAGE_DECODE_SUBMIT_BUSY;
        image_decode_worker_release_job(worker);
        state = IMAGE_DECODE_WORKER_FREE;
    }
    if (state != IMAGE_DECODE_WORKER_FREE)
        return IMAGE_DECODE_SUBMIT_BUSY;
    size_t source_width = (size_t) resource->source_width;
    size_t source_height = (size_t) resource->source_height;
    size_t target_width = (size_t) resource->width;
    size_t target_height = (size_t) resource->height;
    if (source_width > SIZE_MAX / source_height
        || target_width > SIZE_MAX / target_height) {
        return IMAGE_DECODE_SUBMIT_REJECTED;
    }
    size_t target_pixels = target_width * target_height;
    if (target_pixels > SIZE_MAX / 4u)
        return IMAGE_DECODE_SUBMIT_REJECTED;
    size_t output_bytes = target_pixels * 4u;
    size_t output_aligned = output_bytes;
    const size_t alignment = _Alignof(max_align_t);
    size_t remainder = output_aligned % alignment;
    if (remainder != 0) {
        size_t padding = alignment - remainder;
        if (output_aligned > SIZE_MAX - padding)
            return IMAGE_DECODE_SUBMIT_REJECTED;
        output_aligned += padding;
    }
    size_t scratch_bytes = 0;
    if (!image_jpeg_worker_scratch(
            resource->encoded, resource->encoded_length, resource->width,
            resource->height, &scratch_bytes)
        || scratch_bytes > SIZE_MAX - resource->encoded_length)
        return IMAGE_DECODE_SUBMIT_REJECTED;
    scratch_bytes += resource->encoded_length;
    if (output_aligned > SIZE_MAX - scratch_bytes)
        return IMAGE_DECODE_SUBMIT_REJECTED;
    size_t arena_bytes = output_aligned + scratch_bytes;
    unsigned char *storage = budget_malloc(budget, arena_bytes);
    BrowserSharedBody *body = storage == NULL ? NULL
        : browser_shared_body_retain(resource->encoded_body);
    if (storage == NULL || body == NULL) {
        budget_free(budget, storage);
        return IMAGE_DECODE_SUBMIT_REJECTED;
    }
    worker->encoded_body = body;
    worker->arena_storage = storage;
    worker->arena_bytes = arena_bytes;
    worker->output_bytes = output_bytes;
    worker->arena = (ImageDecodeArena) {
        .base = storage + output_aligned,
        .capacity = scratch_bytes
    };
    worker->encoded = body->data;
    worker->encoded_length = resource->encoded_length;
    worker->target_width = resource->width;
    worker->target_height = resource->height;
    worker->expected_source_width = resource->source_width;
    worker->expected_source_height = resource->source_height;
    worker->source_width = 0;
    worker->source_height = 0;
    worker->result_status = IMAGE_DECODE_TRANSIENT_FAILURE;
    worker->generation++;
    if (worker->generation == 0) worker->generation++;
    *token = worker->generation;
    atomic_store_explicit(
        &image_worker_decode_pending, true, memory_order_release);
    atomic_store_explicit(
        &worker->state, IMAGE_DECODE_WORKER_QUEUED,
        memory_order_release);
#if defined(__PSP__)
    if (sceKernelSetEventFlag(
            worker->event, IMAGE_DECODE_WORKER_WAKE) < 0) {
        atomic_store_explicit(
            &worker->failed, true, memory_order_release);
        image_decode_worker_release_job(worker);
        return IMAGE_DECODE_SUBMIT_REJECTED;
    }
#else
    image_decode_worker_execute(worker);
#endif
    return IMAGE_DECODE_SUBMIT_ACCEPTED;
}

bool image_decode_worker_collect(
    uint32_t token, ImageDecodeWorkerResult *result)
{
    ImageDecodeWorker *worker = &image_decode_worker;
    if (!worker->initialized || result == NULL
        || atomic_load_explicit(&worker->state, memory_order_acquire)
               != IMAGE_DECODE_WORKER_READY
        || worker->generation != token) return false;
    *result = (ImageDecodeWorkerResult) {
        .status = worker->result_status,
        .pixel_bytes = worker->output_bytes,
        .working_bytes = worker->output_bytes + worker->arena.used,
        .source_width = worker->source_width,
        .source_height = worker->source_height
    };
    if (result->status == IMAGE_DECODE_SUCCEEDED) {
        unsigned char *shrunk = budget_realloc(
            worker->budget, worker->arena_storage,
            worker->output_bytes);
        result->pixels = shrunk != NULL
            ? shrunk : worker->arena_storage;
        worker->arena_storage = NULL;
    }
    image_decode_worker_release_job(worker);
    return true;
}

void image_decode_worker_abandon(uint32_t token)
{
    ImageDecodeWorker *worker = &image_decode_worker;
    if (!worker->initialized || token == 0
        || worker->generation != token) return;
    worker->abandoned_generation = token;
    if (atomic_load_explicit(&worker->state, memory_order_acquire)
            == IMAGE_DECODE_WORKER_READY) {
        image_decode_worker_release_job(worker);
        worker->abandoned_generation = 0;
    }
}

bool images_decode_worker_reap_cancelled(Budget *budget)
{
    ImageDecodeWorker *worker = &image_decode_worker;
    if (!worker->initialized) return true;
    if (worker->budget != budget) return false;
    if (worker->abandoned_generation == 0) return true;
    if (atomic_load_explicit(&worker->state, memory_order_acquire)
            != IMAGE_DECODE_WORKER_READY) return false;
    if (worker->generation == worker->abandoned_generation)
        image_decode_worker_release_job(worker);
    worker->abandoned_generation = 0;
    return true;
}

bool images_decode_worker_shutdown(Budget *budget)
{
    ImageDecodeWorker *worker = &image_decode_worker;
    if (!worker->initialized) return true;
    if (worker->budget != budget) return false;
#if defined(__PSP__)
    atomic_store_explicit(
        &worker->stop_requested, true, memory_order_release);
    (void) sceKernelSetEventFlag(worker->event, IMAGE_DECODE_WORKER_WAKE);
    if (psp_thread_wait_end_bounded(worker->thread, 2000000u) < 0)
        return false;
    (void) sceKernelDeleteThread(worker->thread);
    (void) sceKernelDeleteEventFlag(worker->event);
#endif
    unsigned state = atomic_load_explicit(
        &worker->state, memory_order_acquire);
    if (state != IMAGE_DECODE_WORKER_FREE)
        image_decode_worker_release_job(worker);
#if defined(__PSP__)
    budget_reservation_release(&worker->stack_reservation);
#endif
    memset(worker, 0, sizeof(*worker));
    atomic_store_explicit(&decode_gate, false, memory_order_release);
    return true;
}
ImageDecodeStatus image_resource_decode_checked(
    const ImageResource *image, Budget *budget, unsigned char **decoded)
{
    if (decoded == NULL) return IMAGE_DECODE_DETERMINISTIC_FAILURE;
    *decoded = NULL;
    if (image == NULL || budget == NULL || image->encoded == NULL
        || image->encoded_length == 0
        || image->encoded_length > INT32_MAX) {
        return IMAGE_DECODE_DETERMINISTIC_FAILURE;
    }
    if ((image->retarget_flags & IMAGE_RETARGET_VECTOR) != 0) {
        /* Vector markup is rasterized straight at the size wanted. */
        int width = image->width, height = image->height;
        if (width <= 0 || height <= 0
            || (size_t) width > SIZE_MAX / 4u / (size_t) height)
            return IMAGE_DECODE_DETERMINISTIC_FAILURE;
        if (image_svg_decode_busy()) return IMAGE_DECODE_TRANSIENT_FAILURE;
        size_t failures_before = budget->failure_count;
        *decoded = image_svg_decode(
            image->encoded, image->encoded_length, budget,
            (size_t) width * (size_t) height * 4u, &width, &height);
        if (*decoded != NULL) return IMAGE_DECODE_SUCCEEDED;
        return budget->failure_count != failures_before
            ? IMAGE_DECODE_TRANSIENT_FAILURE
            : IMAGE_DECODE_DETERMINISTIC_FAILURE;
    }
    if (!image_decode_begin_budget(budget))
        return IMAGE_DECODE_TRANSIENT_FAILURE;
    decode_live_bytes = 0;
    decode_peak_bytes = 0;
    int width = 0, height = 0, components = 0;
    size_t failures_before = budget->failure_count;
    int expected_width = image->source_width > 0
                         ? image->source_width : image->width;
    int expected_height = image->source_height > 0
                          ? image->source_height : image->height;
    bool reduces = image->width > 0 && image->height > 0
        && image->width <= expected_width && image->height <= expected_height
        && (image->width < expected_width
            || image->height < expected_height);
    bool scaled_jpeg = reduces && image->encoded_length >= 2u
        && image->encoded[0] == 0xffu && image->encoded[1] == 0xd8u;
    bool webp = image_decode_is_webp(
        image->encoded, image->encoded_length);
    bool interrupted = false;
    bool at_target = false;
    unsigned char *pixels = NULL;
    if (webp) {
        pixels = image_decode_webp_scaled(
            image->encoded, image->encoded_length,
            image->width, image->height,
            &width, &height, &components, &interrupted);
        at_target = true;
    } else if (scaled_jpeg) {
        pixels = image_decode_jpeg_scaled(
            image->encoded, (int) image->encoded_length,
            expected_width, expected_height, image->width, image->height,
            &width, &height, &components, NULL);
        at_target = true;
    } else {
        ImagePngStreamResult png = image->width > 0 && image->height > 0
            ? image_decode_png_stream(
                  image->encoded, image->encoded_length,
                  image->width, image->height, &width, &height, &pixels,
                  &interrupted)
            : IMAGE_PNG_STREAM_FAILED;
        bool png_signature = image->encoded_length >= 8u
            && memcmp(image->encoded, "\x89PNG\r\n\x1a\n", 8u) == 0;
        /* A PNG the streaming decoder refused for a reason other than memory
           or an interruption still gets stb's verdict, so stb's tolerance
           remains the floor. */
        bool png_retry = png == IMAGE_PNG_STREAM_FAILED && png_signature
            && !interrupted && budget->failure_count == failures_before;
        if (png == IMAGE_PNG_STREAM_DECODED) {
            components = 4;
            at_target = true;
        } else if (!png_signature || png == IMAGE_PNG_STREAM_FALLBACK
                   || png_retry) {
#if !defined(TILEFINCH_DISABLE_GIF)
            pixels = image_is_gif(image->encoded, image->encoded_length)
                ? image_decode_gif_first_frame(
                      image->encoded, (int) image->encoded_length,
                      &width, &height, &components)
                : stbi_load_from_memory(
                      image->encoded, (int) image->encoded_length,
                      &width, &height, &components, 4);
#else
            pixels = stbi_load_from_memory(
                image->encoded, (int) image->encoded_length,
                &width, &height, &components, 4);
#endif
        }
    }
    image_decode_end();
    int source_width = expected_width;
    int source_height = expected_height;
    if (pixels == NULL) {
        return interrupted || budget->failure_count != failures_before
            ? IMAGE_DECODE_TRANSIENT_FAILURE
            : IMAGE_DECODE_DETERMINISTIC_FAILURE;
    }
    if (width != source_width || height != source_height) {
        image_resource_free_decoded(budget, pixels);
        return IMAGE_DECODE_DETERMINISTIC_FAILURE;
    }
    if (at_target
        || (width == image->width && height == image->height)) {
        *decoded = pixels;
        return IMAGE_DECODE_SUCCEEDED;
    }
    /* GIF, BMP and the other stb formats (and interlaced PNG) still decode
       at source size before this reduction. */
    if (image->width <= 0 || image->height <= 0
        || (size_t) image->width > SIZE_MAX / (size_t) image->height
        || (size_t) image->width * (size_t) image->height > SIZE_MAX / 4u) {
        image_resource_free_decoded(budget, pixels);
        return IMAGE_DECODE_DETERMINISTIC_FAILURE;
    }
    size_t target_bytes = (size_t) image->width * (size_t) image->height * 4u;
    unsigned char *target = budget_malloc(budget, target_bytes);
    if (target == NULL) {
        image_resource_free_decoded(budget, pixels);
        return IMAGE_DECODE_TRANSIENT_FAILURE;
    }
    image_decode_charged(target);
    image_point_sample_rgba(pixels, width, height, target, image->width,
                            image->height);
    image_resource_free_decoded(budget, pixels);
    *decoded = target;
    return IMAGE_DECODE_SUCCEEDED;
}

unsigned char *image_resource_decode(const ImageResource *image,
                                     Budget *budget)
{
    unsigned char *pixels = NULL;
    return image_resource_decode_checked(image, budget, &pixels)
               == IMAGE_DECODE_SUCCEEDED
        ? pixels : NULL;
}

void image_resource_free_decoded(Budget *budget, unsigned char *pixels)
{
    if (budget == NULL || pixels == NULL) return;
    budget_free(budget, pixels);
}
