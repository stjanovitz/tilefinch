#include "image_svg_decode_internal.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static Budget *svg_budget;

static void *svg_malloc(size_t size)
{
    return budget_malloc_category(
        svg_budget, BUDGET_CATEGORY_RESOURCE, size);
}

static void *svg_realloc(void *pointer, size_t size)
{
    return budget_realloc_category(
        svg_budget, BUDGET_CATEGORY_RESOURCE, pointer, size);
}

static void svg_free(void *pointer)
{
    budget_free(svg_budget, pointer);
}

#define malloc(size) svg_malloc(size)
#define realloc(pointer, size) svg_realloc((pointer), (size))
#define free(pointer) svg_free(pointer)
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-compare"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#define NANOSVG_IMPLEMENTATION
#include <nanosvg.h>
#define NANOSVGRAST_IMPLEMENTATION
#include <nanosvgrast.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#undef malloc
#undef realloc
#undef free

unsigned char *image_svg_decode(const void *data, size_t length,
                                Budget *budget,
                                size_t maximum_decoded_bytes,
                                int *width, int *height)
{
    if (data == NULL || budget == NULL || width == NULL || height == NULL
        || maximum_decoded_bytes < 4 || svg_budget != NULL) return NULL;
    uint64_t metadata_checkpoint = budget_checkpoint(budget);
    svg_budget = budget;
    char *source = budget_malloc_category(
        budget, BUDGET_CATEGORY_RESOURCE, length + 1);
    if (source != NULL) {
        memcpy(source, data, length);
        source[length] = '\0';
    }
    NSVGimage *svg = source == NULL
        ? NULL : nsvgParse(source, "px", 96.0f);
    if (svg == NULL || svg->width <= 0.0f || svg->height <= 0.0f
        || svg->width > 32767.0f || svg->height > 32767.0f) {
        if (svg != NULL) nsvgDelete(svg);
        svg_budget = NULL;
        budget_rollback(budget, metadata_checkpoint);
        return NULL;
    }
    int output_width = (int) ceilf(svg->width);
    int output_height = (int) ceilf(svg->height);
    if (output_width <= 0 || output_height <= 0
        || (size_t) output_width > SIZE_MAX / (size_t) output_height
        || (size_t) output_width * (size_t) output_height > SIZE_MAX / 4u
        || (size_t) output_width * (size_t) output_height * 4u
           > maximum_decoded_bytes) {
        nsvgDelete(svg);
        svg_budget = NULL;
        budget_rollback(budget, metadata_checkpoint);
        return NULL;
    }
    nsvgDelete(svg);
    svg_budget = NULL;
    budget_rollback(budget, metadata_checkpoint);

    size_t bytes = (size_t) output_width * (size_t) output_height * 4u;
    unsigned char *pixels = budget_calloc_category(
        budget, BUDGET_CATEGORY_RESOURCE, 1, bytes);
    if (pixels == NULL) return NULL;
    uint64_t raster_checkpoint = budget_checkpoint(budget);
    svg_budget = budget;
    source = budget_malloc_category(
        budget, BUDGET_CATEGORY_RESOURCE, length + 1);
    if (source != NULL) {
        memcpy(source, data, length);
        source[length] = '\0';
    }
    svg = source == NULL ? NULL : nsvgParse(source, "px", 96.0f);
    NSVGrasterizer *rasterizer = svg != NULL
        ? nsvgCreateRasterizer() : NULL;
    if (svg == NULL || rasterizer == NULL) {
        if (rasterizer != NULL) nsvgDeleteRasterizer(rasterizer);
        if (svg != NULL) nsvgDelete(svg);
        svg_budget = NULL;
        budget_rollback(budget, raster_checkpoint);
        budget_free(budget, pixels);
        return NULL;
    }
    float scale_x = (float) output_width / svg->width;
    float scale_y = (float) output_height / svg->height;
    float scale = scale_x < scale_y ? scale_x : scale_y;
    nsvgRasterize(rasterizer, svg, 0.0f, 0.0f, scale, pixels,
                  output_width, output_height, output_width * 4);
    nsvgDeleteRasterizer(rasterizer);
    nsvgDelete(svg);
    svg_budget = NULL;
    /* Parser/rasterizer state is scratch-only. A generation rollback also
       contains malformed-input leaks in the third-party SVG parser while the
       pixel allocation, made before the checkpoint, remains owned. */
    budget_rollback(budget, raster_checkpoint);
    *width = output_width;
    *height = output_height;
    return pixels;
}
