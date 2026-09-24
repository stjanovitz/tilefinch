#include "image_svg_decode_internal.h"
#include "tilefinch/platform.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static Budget *svg_budget;
static unsigned svg_allocations;
static uint64_t svg_slice_started_us;

/* NanoSVG parses a document in one call (a 10 KB wordmark takes 0.2 s on
   the PSP, mostly software double math) but allocates throughout, as path
   point arrays grow. Its allocator is therefore where the decode can let
   the frontend present and sample input. The parse cannot be abandoned, so
   a cancel request simply persists to the next image checkpoint. */
static void svg_cooperate(void)
{
    if ((++svg_allocations & 15u) != 0) return;
    uint64_t now = tilefinch_platform_monotonic_time_us();
    if (now >= svg_slice_started_us
        && now - svg_slice_started_us < UINT64_C(8000)) return;
    (void) tilefinch_platform_cooperate("svg-decode", svg_allocations);
    svg_slice_started_us = tilefinch_platform_monotonic_time_us();
}

static void *svg_malloc(size_t size)
{
    svg_cooperate();
    return budget_malloc_category(
        svg_budget, BUDGET_CATEGORY_RESOURCE, size);
}

static void *svg_realloc(void *pointer, size_t size)
{
    svg_cooperate();
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
    /* Parser and rasterizer state is scratch under a private allocation
       owner, so one rollback reclaims it (including what the third-party
       parser leaks on malformed input) while the pixels, allocated outside
       that owner, stay owned. The document is parsed once: on the PSP the
       parse, with its software double-precision number conversion, is most
       of the cost. */
    BudgetAllocationOwner scratch_owner = BUDGET_ALLOCATION_OWNER_NONE;
    if (!budget_allocation_owner_create(budget, &scratch_owner)) return NULL;
    svg_allocations = 0;
    svg_slice_started_us = tilefinch_platform_monotonic_time_us();
    BudgetAllocationOwner previous_owner =
        budget_allocation_owner_enter(budget, scratch_owner);
    svg_budget = budget;
    char *source = budget_malloc_category(
        budget, BUDGET_CATEGORY_RESOURCE, length + 1);
    if (source != NULL) {
        memcpy(source, data, length);
        source[length] = '\0';
    }
    NSVGimage *svg = source == NULL
        ? NULL : nsvgParse(source, "px", 96.0f);
    budget_allocation_owner_leave(budget, previous_owner);
    unsigned char *pixels = NULL;
    int output_width = 0, output_height = 0;
    if (svg != NULL && svg->width > 0.0f && svg->height > 0.0f
        && svg->width <= 32767.0f && svg->height <= 32767.0f) {
        output_width = (int) ceilf(svg->width);
        output_height = (int) ceilf(svg->height);
        if (output_width > 0 && output_height > 0
            && (size_t) output_width
                   <= SIZE_MAX / (size_t) output_height
            && (size_t) output_width * (size_t) output_height
                   <= SIZE_MAX / 4u
            && (size_t) output_width * (size_t) output_height * 4u
                   <= maximum_decoded_bytes) {
            pixels = budget_calloc_category(
                budget, BUDGET_CATEGORY_RESOURCE, 1,
                (size_t) output_width * (size_t) output_height * 4u);
        }
    }
    if (pixels != NULL) {
        previous_owner = budget_allocation_owner_enter(budget, scratch_owner);
        NSVGrasterizer *rasterizer = nsvgCreateRasterizer();
        if (rasterizer != NULL) {
            float scale_x = (float) output_width / svg->width;
            float scale_y = (float) output_height / svg->height;
            float scale = scale_x < scale_y ? scale_x : scale_y;
            nsvgRasterize(rasterizer, svg, 0.0f, 0.0f, scale, pixels,
                          output_width, output_height, output_width * 4);
            nsvgDeleteRasterizer(rasterizer);
        }
        budget_allocation_owner_leave(budget, previous_owner);
        if (rasterizer == NULL) {
            budget_free(budget, pixels);
            pixels = NULL;
        }
    }
    previous_owner = budget_allocation_owner_enter(budget, scratch_owner);
    if (svg != NULL) nsvgDelete(svg);
    budget_allocation_owner_leave(budget, previous_owner);
    svg_budget = NULL;
    budget_rollback_owner_category(budget, scratch_owner,
                                   BUDGET_CATEGORY_RESOURCE);
    if (pixels == NULL) return NULL;
    *width = output_width;
    *height = output_height;
    return pixels;
}
