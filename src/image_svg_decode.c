#include "image_svg_decode_internal.h"
#include "tilefinch/platform.h"
#include "tilefinch_test_faults.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static Budget *svg_budget;
static unsigned svg_allocations;
static bool svg_allocation_failed;
/* The parse stopped at this engine's nesting limits (image_svg_viewport.inc):
   a limit, like a refused allocation, not markup no decoder takes. */
static bool svg_limit_refused;
static uint64_t svg_slice_started_us;
/* The allocation owner the caller had active: a cooperation hook runs
   under it, not under the decoder's scratch owner, so the hook's own
   allocations survive the scratch rollback. */
static BudgetAllocationOwner svg_caller_owner;
static void svg_raster_shape(void *shape, int width, int height, float scale);
static bool svg_raster_coverage(void *rasterizer, unsigned char *coverage,
                                 int first, int last, int row);

bool image_svg_decode_busy(void)
{
    return svg_budget != NULL;
}

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
    BudgetAllocationOwner scratch = budget_allocation_owner_enter(
        svg_budget, svg_caller_owner);
    (void) tilefinch_platform_cooperate("svg-decode", svg_allocations);
    budget_allocation_owner_leave(svg_budget, scratch);
    svg_slice_started_us = tilefinch_platform_monotonic_time_us();
}

static void *svg_malloc(size_t size)
{
    svg_cooperate();
    void *result = budget_malloc_category(
        svg_budget, BUDGET_CATEGORY_RESOURCE, size);
    if (result == NULL && size != 0) svg_allocation_failed = true;
    return result;
}

static void *svg_realloc(void *pointer, size_t size)
{
    svg_cooperate();
    void *result = budget_realloc_category(
        svg_budget, BUDGET_CATEGORY_RESOURCE, pointer, size);
    if (result == NULL && size != 0) svg_allocation_failed = true;
    return result;
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
#define NANOSVG_RASTER_SHAPE(shape, w, h, scale) svg_raster_shape(shape, w, h, scale)
#define NANOSVG_RASTER_COVERAGE(r, coverage, first, last, row) \
    svg_raster_coverage(r, coverage, first, last, row)
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

#include "image_svg_viewport.inc"

unsigned char *image_svg_decode_bounded(
    const void *data, size_t length, Budget *budget,
    size_t maximum_decoded_bytes, int *width, int *height,
    ImageSvgOversizeTarget target, void *opaque,
    int *source_width, int *source_height, bool *limited)
{
    return image_svg_decode_deferrable(
        data, length, budget, maximum_decoded_bytes, width, height, target,
        opaque, NULL, NULL, source_width, source_height, limited, NULL, 0,
        NULL);
}

/* A parsed document held between calls (image_svg_decode_deferrable's
   `kept`): everything it owns, this record included, is scratch under
   `owner`, so one rollback releases it. */
struct ImageSvgParse {
    Budget *budget;
    BudgetAllocationOwner owner;
    NSVGimage *svg;
    SvgViewportParser *context;
    /* The markup it was parsed from (the parser's copy, restored after the
       parse edits it in place), so a later raster checks it is the same. */
    char *source;
    size_t length;
    size_t bytes;
};

static size_t svg_parses;

size_t image_svg_parse_count(void)
{
    return svg_parses;
}

/* Rasterizes `svg` (parsed with `context`) into output_width x
   output_height pixels allocated outside `scratch_owner`; the rasterizer's
   own state is scratch. svg_budget must be set. NULL on failure. */
static unsigned char *svg_rasterize(Budget *budget,
                                    BudgetAllocationOwner scratch_owner,
                                    NSVGimage *svg,
                                    SvgViewportParser *context,
                                    int output_width, int output_height)
{
    unsigned char *pixels = budget_calloc_category(
        budget, BUDGET_CATEGORY_RESOURCE, 1,
        (size_t) output_width * (size_t) output_height * 4u);
    if (pixels == NULL) return NULL;
    svg_clips = context;
    BudgetAllocationOwner previous_owner =
        budget_allocation_owner_enter(budget, scratch_owner);
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
    if (rasterizer == NULL || svg_allocation_failed) {
        budget_free(budget, pixels);
        pixels = NULL;
    }
    return pixels;
}

/* Deletes the document and reclaims every scratch allocation. */
static void svg_finish(Budget *budget, BudgetAllocationOwner scratch_owner,
                       NSVGimage *svg)
{
    BudgetAllocationOwner previous_owner =
        budget_allocation_owner_enter(budget, scratch_owner);
    if (svg != NULL) nsvgDelete(svg);
    budget_allocation_owner_leave(budget, previous_owner);
    svg_clips = NULL;
    svg_budget = NULL;
    budget_rollback_owner_category(budget, scratch_owner,
                                   BUDGET_CATEGORY_RESOURCE);
}

static void svg_begin(Budget *budget)
{
    svg_allocations = 0;
    svg_allocation_failed = false;
    svg_limit_refused = false;
    svg_slice_started_us = tilefinch_platform_monotonic_time_us();
    svg_caller_owner = budget->active_allocation_owner;
    svg_budget = budget;
}

/* Keeps a deferred document's parse (with its scratch) for a later
   raster, when it fits `limit` bytes. */
static ImageSvgParse *svg_keep(Budget *budget,
                               BudgetAllocationOwner scratch_owner,
                               NSVGimage *svg, char *source,
                               const void *data, size_t length,
                               uint64_t checkpoint, size_t limit)
{
    if (svg_clips == NULL || source == NULL || svg_allocation_failed)
        return NULL;
    BudgetAllocationOwner previous_owner =
        budget_allocation_owner_enter(budget, scratch_owner);
    ImageSvgParse *parse = budget_malloc_category(
        budget, BUDGET_CATEGORY_RESOURCE, sizeof(*parse));
    budget_allocation_owner_leave(budget, previous_owner);
    if (parse == NULL) return NULL;
    /* Reclaim and cooperation may free or allocate unrelated memory while
       parsing. Count this owner's live charges, not net page growth; the
       checkpoint bounds the walk to this parse's allocation window. */
    size_t bytes = budget_owner_category_bytes_since(
        budget, scratch_owner, BUDGET_CATEGORY_RESOURCE, checkpoint);
    if (bytes > limit) {
        budget_free(budget, parse);
        return NULL;
    }
    memcpy(source, data, length);
    *parse = (ImageSvgParse) {
        .budget = budget,
        .owner = scratch_owner,
        .svg = svg,
        .context = svg_clips,
        .source = source,
        .length = length,
        .bytes = bytes
    };
    svg_clips = NULL;
    svg_budget = NULL;
    return parse;
}

unsigned char *image_svg_decode_deferrable(
    const void *data, size_t length, Budget *budget,
    size_t maximum_decoded_bytes, int *width, int *height,
    ImageSvgOversizeTarget target, void *opaque,
    ImageSvgDefer defer, void *defer_opaque,
    int *source_width, int *source_height, bool *limited, bool *deferred,
    size_t keep_limit, ImageSvgParse **kept)
{
    if (limited != NULL) *limited = true;
    if (deferred != NULL) *deferred = false;
    if (kept != NULL) *kept = NULL;
    if (data == NULL || budget == NULL || width == NULL || height == NULL
        || maximum_decoded_bytes < 4 || svg_budget != NULL) return NULL;
    int target_width = *width, target_height = *height;
    /* Parser and rasterizer state is scratch under a private allocation
       owner, so one rollback reclaims it (including what the third-party
       parser leaks on malformed input) while the pixels, allocated outside
       that owner, stay owned. The document is parsed once: on the PSP the
       parse, with its software double-precision number conversion, is most
       of the cost. */
    BudgetAllocationOwner scratch_owner = BUDGET_ALLOCATION_OWNER_NONE;
    if (!budget_allocation_owner_create(budget, &scratch_owner)) return NULL;
    uint64_t checkpoint = budget_checkpoint(budget);
    svg_begin(budget);
    BudgetAllocationOwner previous_owner =
        budget_allocation_owner_enter(budget, scratch_owner);
    char *source = budget_malloc_category(
        budget, BUDGET_CATEGORY_RESOURCE, length + 1);
    if (source != NULL) {
        memcpy(source, data, length);
        source[length] = '\0';
    } else {
        svg_allocation_failed = true;
    }
    NSVGimage *svg = NULL;
    if (source != NULL) {
        svg_parses++;
        svg = svg_parse_viewports(source, budget);
    }
    budget_allocation_owner_leave(budget, previous_owner);
    unsigned char *pixels = NULL;
    int output_width = 0, output_height = 0;
    int intrinsic_width = 0, intrinsic_height = 0;
    /* Markup that parses to nothing, or to no positive size, fails the same
       way every time; every other refusal below is a limit. */
    bool markup_rejected = !svg_allocation_failed && !svg_limit_refused
        && (svg == NULL || !(svg->width > 0.0f && svg->height > 0.0f));
    if (svg != NULL && !svg_allocation_failed
        && svg->width > 0.0f && svg->height > 0.0f
        && svg->width <= 32767.0f && svg->height <= 32767.0f) {
        output_width = (int) ceilf(svg->width);
        output_height = (int) ceilf(svg->height);
        intrinsic_width = output_width;
        intrinsic_height = output_height;
        if (defer != NULL && deferred != NULL && target_width <= 0
            && target_height <= 0
            && defer(defer_opaque, intrinsic_width, intrinsic_height)) {
            *deferred = true;
            output_width = output_height = 0;
        } else if (target_width > 0 && target_height > 0) {
            output_width = target_width;
            output_height = target_height;
        }
        else if (target != NULL
            && (size_t) output_width * (size_t) output_height
                   > maximum_decoded_bytes / 4u) {
            /* Both dimensions are <=32767: the product fits on PSP too.
               This fallback is admission, not a second allocation attempt. */
            if (!target(opaque, intrinsic_width, intrinsic_height,
                        &output_width, &output_height)) {
                output_width = output_height = 0;
            }
        }
        if (output_width > 0 && output_height > 0
            && (size_t) output_width
                   <= SIZE_MAX / (size_t) output_height
            && (size_t) output_width * (size_t) output_height
                   <= SIZE_MAX / 4u
            && (size_t) output_width * (size_t) output_height * 4u
                   <= maximum_decoded_bytes) {
            pixels = svg_rasterize(budget, scratch_owner, svg, svg_clips,
                                   output_width, output_height);
        }
    }
    if (deferred != NULL && *deferred) {
        if (kept != NULL)
            *kept = svg_keep(budget, scratch_owner, svg, source, data,
                             length, checkpoint, keep_limit);
        if (kept == NULL || *kept == NULL)
            svg_finish(budget, scratch_owner, svg);
        if (limited != NULL) *limited = false;
        *width = intrinsic_width;
        *height = intrinsic_height;
        if (source_width != NULL) *source_width = intrinsic_width;
        if (source_height != NULL) *source_height = intrinsic_height;
        return NULL;
    }
    svg_finish(budget, scratch_owner, svg);
    if (pixels == NULL) {
        if (limited != NULL) *limited = !markup_rejected;
        return NULL;
    }
    if (limited != NULL) *limited = false;
    *width = output_width;
    *height = output_height;
    if (source_width != NULL) *source_width = intrinsic_width;
    if (source_height != NULL) *source_height = intrinsic_height;
    return pixels;
}

bool image_svg_parse_matches(const ImageSvgParse *parse, const void *data,
                             size_t length)
{
    return parse != NULL && data != NULL && parse->length == length
        && memcmp(parse->source, data, length) == 0;
}

size_t image_svg_parse_bytes(const ImageSvgParse *parse)
{
    return parse == NULL ? 0 : parse->bytes;
}

void image_svg_parse_release(ImageSvgParse *parse)
{
    /* Everything it holds, this record included, is its owner's scratch. */
    if (parse != NULL)
        budget_rollback_owner_category(parse->budget, parse->owner,
                                       BUDGET_CATEGORY_RESOURCE);
}

unsigned char *image_svg_parse_rasterize(ImageSvgParse *parse,
                                         size_t maximum_decoded_bytes,
                                         int *width, int *height)
{
    if (parse == NULL) return NULL;
    Budget *budget = parse->budget;
    int output_width = width == NULL ? 0 : *width;
    int output_height = height == NULL ? 0 : *height;
    if (svg_budget != NULL || output_width <= 0 || output_height <= 0
        || (size_t) output_width > SIZE_MAX / (size_t) output_height
        || (size_t) output_width * (size_t) output_height > SIZE_MAX / 4u
        || (size_t) output_width * (size_t) output_height * 4u
               > maximum_decoded_bytes) {
        image_svg_parse_release(parse);
        return NULL;
    }
    BudgetAllocationOwner owner = parse->owner;
    NSVGimage *svg = parse->svg;
    SvgViewportParser *context = parse->context;
    svg_begin(budget);
    unsigned char *pixels = svg_rasterize(budget, owner, svg, context,
                                          output_width, output_height);
    /* The record itself is scratch: the rollback below releases it. */
    svg_finish(budget, owner, svg);
    return pixels;
}

unsigned char *image_svg_decode(const void *data, size_t length,
                                Budget *budget,
                                size_t maximum_decoded_bytes,
                                int *width, int *height)
{
    return image_svg_decode_bounded(data, length, budget,
        maximum_decoded_bytes, width, height, NULL, NULL, NULL, NULL, NULL);
}
