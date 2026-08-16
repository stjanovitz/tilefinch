/* Destination geometry shared by both video presenters. See
   include/tilefinch/psp_media_present.h for what it is for. */

#include "tilefinch/psp_media_present.h"

#include <string.h>

static int present_floor_to_int(float value)
{
    int truncated = (int) value;
    return (float) truncated > value ? truncated - 1 : truncated;
}

static int present_pow2_at_least(int value)
{
    int size = 1;
    while (size < value && size < PSP_MEDIA_PRESENT_TEXTURE_MAX) size <<= 1;
    return size;
}

/*
 * The far end of the sampled range along one axis.
 *
 * The natural mapping puts destination pixel k at source coordinate
 * (k + 0.5) * source / output, whose bilinear taps are the texels either side
 * of that minus a half. Downscale far enough and the last tap lands inside
 * the picture on its own. At or above 1:1 it lands on the texel one past the
 * end -- on macroblock padding across the width, and on rows the colour
 * conversion never wrote down the height. The GE cannot be told to stop
 * there: texture dimensions are powers of two, so clamping happens at 256 or
 * 512, not at 240 or 426.
 *
 * So pull the far edge in, but only when it is needed and only by the least
 * that works. The cost is a scale error under two parts in ten thousand; what
 * it buys is that uninitialised memory can never reach the panel.
 */
#define PSP_MEDIA_PRESENT_EDGE_MARGIN (1.0f / 64.0f)

static float present_far_edge(int source_extent, int output_extent)
{
    float source = (float) source_extent;
    if (output_extent <= 0) return source;
    float output = (float) output_extent;
    /* The highest texel the last destination pixel's second tap reaches with
       the plain mapping. */
    float centre = (output - 0.5f) * source / output - 0.5f;
    if (present_floor_to_int(centre) + 1 <= source_extent - 1) return source;
    /* A sixty-fourth of a texel past the exact solution, so the boundary case
       is settled by arithmetic rather than by float rounding. */
    float guarded = (source - 0.5f - PSP_MEDIA_PRESENT_EDGE_MARGIN)
        * output / (output - 0.5f);
    return guarded < source ? guarded : source;
}

static void present_add_band(
    PspMediaPresentPlan *plan, int x, int y, int width, int height)
{
    if (width <= 0 || height <= 0) return;
    if (plan->band_count >= PSP_MEDIA_PRESENT_MAX_BANDS) return;
    plan->bands[plan->band_count++] =
        (PspMediaPresentRect) {x, y, width, height};
}

bool psp_media_present_plan(
    PspMediaPresentPlan *plan,
    int source_width, int source_height, int source_stride_pixels,
    int screen_width, int screen_height)
{
    if (plan == NULL) return false;
    memset(plan, 0, sizeof(*plan));
    if (source_width <= 0 || source_height <= 0) return false;
    if (source_stride_pixels < source_width) return false;
    if (screen_width <= 0 || screen_height <= 0) return false;

    /* The historical fit: widest that keeps the aspect ratio, then clamp to
       the panel height. Presented rectangles must not move because the
       presenter behind them changed. */
    int output_width = screen_width;
    int output_height = source_height * output_width / source_width;
    if (output_height > screen_height) {
        output_height = screen_height;
        output_width = source_width * output_height / source_height;
    }
    if (output_width <= 0 || output_height <= 0) return false;
    if (output_width > screen_width || output_height > screen_height)
        return false;
    int left = (screen_width - output_width) / 2;
    int top = (screen_height - output_height) / 2;
    int right = left + output_width;
    int bottom = top + output_height;
    plan->video = (PspMediaPresentRect) {
        left, top, output_width, output_height};
    present_add_band(plan, 0, 0, screen_width, top);
    present_add_band(plan, 0, bottom, screen_width, screen_height - bottom);
    present_add_band(plan, 0, top, left, output_height);
    present_add_band(
        plan, right, top, screen_width - right, output_height);

    /*
     * From here the answer is about the graphics engine only. Anything it
     * cannot express leaves quad_count at zero, which asks the caller to
     * present this same rectangle with the software scaler -- it is not a
     * refusal to present, because the rectangle and the bands above are
     * exactly what either presenter needs.
     */
    if (source_height > PSP_MEDIA_PRESENT_TEXTURE_MAX) return true;

    float far_u = present_far_edge(source_width, output_width);
    float far_v = present_far_edge(source_height, output_height);
    int texture_height = present_pow2_at_least(source_height);
    float y0 = (float) top;
    float y1 = (float) bottom;

    if (source_width <= PSP_MEDIA_PRESENT_TEXTURE_MAX) {
        plan->quads[0] = (PspMediaPresentQuad) {
            .texture_column = 0,
            .texture_width = present_pow2_at_least(source_width),
            .texture_height = texture_height,
            .u0 = 0.0f, .v0 = 0.0f, .u1 = far_u, .v1 = far_v,
            .x0 = (float) left, .y0 = y0,
            .x1 = (float) right, .y1 = y1
        };
        plan->quad_count = 1;
        return true;
    }

    /*
     * Wider than one texture. Hand the leftmost destination columns a texture
     * anchored at the surface origin and give the rest a second texture that
     * starts part way along the same rows.
     *
     * The split point is chosen by the sampler, not by arithmetic
     * convenience: the last column the first quad draws must still have both
     * of its bilinear taps inside texel 511. Both quads then evaluate the one
     * global mapping, so the seam carries no phase discontinuity -- the pixel
     * either side of it samples exactly what an unsplit draw would have.
     */
    int split = 0;
    for (int column = 0; column < output_width; column++) {
        float centre = ((float) column + 0.5f) * far_u
            / (float) output_width - 0.5f;
        if (present_floor_to_int(centre) + 1
            > PSP_MEDIA_PRESENT_TEXTURE_MAX - 1) break;
        split = column + 1;
    }
    if (split <= 0 || split >= output_width) return true;
    float seam_u = (float) split * far_u / (float) output_width;
    int base_column = present_floor_to_int(seam_u);
    base_column -= base_column % PSP_MEDIA_PRESENT_COLUMN_ALIGN;
    if (base_column <= 0) return true;
    /* The second texture must reach the last source column. */
    if (source_width - base_column > PSP_MEDIA_PRESENT_TEXTURE_MAX)
        return true;

    plan->quads[0] = (PspMediaPresentQuad) {
        .texture_column = 0,
        .texture_width = PSP_MEDIA_PRESENT_TEXTURE_MAX,
        .texture_height = texture_height,
        .u0 = 0.0f, .v0 = 0.0f, .u1 = seam_u, .v1 = far_v,
        .x0 = (float) left, .y0 = y0,
        .x1 = (float) (left + split), .y1 = y1
    };
    plan->quads[1] = (PspMediaPresentQuad) {
        .texture_column = base_column,
        .texture_width =
            present_pow2_at_least(source_width - base_column),
        .texture_height = texture_height,
        .u0 = seam_u - (float) base_column, .v0 = 0.0f,
        .u1 = far_u - (float) base_column, .v1 = far_v,
        .x0 = (float) (left + split), .y0 = y0,
        .x1 = (float) right, .y1 = y1
    };
    plan->quad_count = 2;
    return true;
}

bool psp_media_present_wide_strip_plan(
    PspMediaPresentStripPlan *strips, const PspMediaPresentPlan *full,
    int source_width, int source_height, int source_stride_pixels,
    size_t capacity)
{
    if (strips == NULL) return false;
    memset(strips, 0, sizeof(*strips));
    /* The 360p inventory is not uniformly 16:9: portrait-derived and older
       uploads can be 540 pixels wide while retaining the same 768-pixel
       firmware stride. The old 638..640 admission sent those pictures down
       the generic direct-texture path, whose GE wait is slower than real time
       on hardware. The vertical strip proof depends on the 360 rows and
       padded stride, not on the visible aspect ratio. Keep the admission to
       two-quad pictures so the established 240p path remains untouched. */
    if (full == NULL || source_width <= 512 || source_width > 640
        || source_height != 360
        || source_stride_pixels != 768 || full->quad_count != 2
        || full->video.height <= 0 || (full->video.height & 1) != 0)
        return false;

    enum { CORE_ROWS = 180, COPY_ROWS = 181 };
    int output_rows = full->video.height / 2;
    size_t copy_bytes = (size_t) source_stride_pixels * COPY_ROWS * 4u;
    if (copy_bytes > capacity) return false;

    for (size_t at = 0; at < PSP_MEDIA_PRESENT_WIDE_STRIPS; at++) {
        PspMediaPresentStrip *strip = &strips->strips[at];
        /* The first pass carries the first row of the second half; the second
           starts one row before its own half. Those are the two taps around
           the seam, and both copies remain 181 contiguous padded rows. */
        strip->source_row = at == 0 ? 0 : CORE_ROWS - 1;
        strip->copy_rows = COPY_ROWS;
        strip->draw.video = (PspMediaPresentRect) {
            full->video.x,
            full->video.y + (int) at * output_rows,
            full->video.width,
            output_rows
        };
        strip->draw.quad_count = full->quad_count;
        for (size_t quad_at = 0; quad_at < full->quad_count; quad_at++) {
            PspMediaPresentQuad quad = full->quads[quad_at];
            quad.texture_height = 256;
            /* The global 0..360 mapping becomes 0..180 for the first
               texture and 1..181 for the second texture whose copy begins at
               source row 179. Keeping the original horizontal mapping makes
               each strip preserve the already-proven 512+remainder seam. */
            float local_start = at == 0 ? 0.0f : 1.0f;
            quad.v0 = local_start;
            quad.v1 = local_start + (float) CORE_ROWS;
            quad.y0 = (float) strip->draw.video.y;
            quad.y1 = (float) (strip->draw.video.y + output_rows);
            strip->draw.quads[quad_at] = quad;
        }
    }
    strips->strip_count = PSP_MEDIA_PRESENT_WIDE_STRIPS;
    return true;
}

/*
 * One axis of the containment proof. `limit` is the highest texel index the
 * quad may address; `allow_low_clamp` is true only at the picture's own left
 * or top edge, where a tap of -1 is clamped back onto texel 0 -- a real
 * pixel, and the correct answer for an edge sample.
 */
static bool present_axis_inside(
    float a0, float a1, int count, int limit, bool allow_low_clamp)
{
    if (count <= 0) return false;
    float step = (a1 - a0) / (float) count;
    float first = a0 + 0.5f * step - 0.5f;
    float last = a0 + ((float) count - 0.5f) * step - 0.5f;
    int low = present_floor_to_int(first);
    int high = present_floor_to_int(last) + 1;
    if (low < (allow_low_clamp ? -1 : 0)) return false;
    return high <= limit;
}

bool psp_media_present_quad_samples_inside(
    const PspMediaPresentQuad *quad, int source_width, int source_height,
    int quad_source_column)
{
    if (quad == NULL) return false;
    int columns = (int) (quad->x1 - quad->x0);
    int rows = (int) (quad->y1 - quad->y0);
    if (columns <= 0 || rows <= 0) return false;
    int width_limit = source_width - 1 - quad_source_column;
    if (width_limit > quad->texture_width - 1)
        width_limit = quad->texture_width - 1;
    int height_limit = source_height - 1;
    if (height_limit > quad->texture_height - 1)
        height_limit = quad->texture_height - 1;
    return present_axis_inside(
               quad->u0, quad->u1, columns, width_limit,
               quad_source_column == 0)
        && present_axis_inside(
               quad->v0, quad->v1, rows, height_limit, true);
}

const char *psp_media_present_mode_name(PspMediaPresentMode mode)
{
    switch (mode) {
        case PSP_MEDIA_PRESENT_MODE_GE_SMOOTH: return "ge-smooth";
        case PSP_MEDIA_PRESENT_MODE_SOFTWARE_SHARP: return "software-sharp";
        case PSP_MEDIA_PRESENT_MODE_SOFTWARE_FALLBACK:
            return "software-fallback";
    }
    return "unknown";
}

bool psp_media_present_skip_allowed(
    const PspMediaPresentRecord *record, uint64_t identity,
    uint64_t generation, const PspMediaPresentRect *video,
    bool chrome_paints)
{
    if (record == NULL || video == NULL) return false;
    if (chrome_paints || !record->valid) return false;
    return record->identity == identity
        && record->generation == generation
        && record->video.x == video->x
        && record->video.y == video->y
        && record->video.width == video->width
        && record->video.height == video->height;
}

void psp_media_present_record(
    PspMediaPresentRecord *record, uint64_t identity, uint64_t generation,
    const PspMediaPresentRect *video)
{
    if (record == NULL || video == NULL) return;
    record->identity = identity;
    record->generation = generation;
    record->video = *video;
    record->valid = true;
}

void psp_media_present_records_reset(
    PspMediaPresentRecord *records, size_t count)
{
    if (records == NULL) return;
    for (size_t at = 0; at < count; at++) records[at].valid = false;
}

/*
 * The stager.
 *
 * Deliberately in this translation unit rather than beside the display list:
 * it is arithmetic over memory with no PSP call in it, so a host runs it, a
 * host proves it against the layout's definition, and a host measures what it
 * costs. The device's only contribution is the reason it exists.
 *
 * Shaped as a streaming copy. The destination advances one word at a time and
 * is never revisited; the source is read sixteen bytes at a time down eight
 * rows, which walks eight cache lines and then consumes the rest of each of
 * them across the next three blocks. One pass, no temporaries, no division in
 * the loop.
 */
void psp_media_present_stage(
    void *destination, const void *source, int source_stride_pixels,
    int texture_width, int rows)
{
    if (destination == NULL || source == NULL
        || source_stride_pixels <= 0 || texture_width < 1 || rows < 1
        || texture_width > source_stride_pixels) return;
    uint32_t *out = destination;
    const uint32_t *in = source;
    /* The whole point is that this is a plain sequential copy, not a reorder.
       A device probe proved the graphics engine draws a linear texture from
       EDRAM at exactly the cost of a swizzled one (6.8ms either way), so the
       block interleave the earlier version performed bought nothing once the
       texture lived in EDRAM -- it was pure per-picture overhead. When the
       texture is as wide as the source's stride, which is the shipping 512
       case, the copy is one contiguous run and the compiler lowers it to a
       single memcpy. */
    if (texture_width == source_stride_pixels) {
        memcpy(out, in, (size_t) texture_width * (size_t) rows * 4u);
        return;
    }
    for (int row = 0; row < rows; row++) {
        memcpy(out, in, (size_t) texture_width * 4u);
        out += texture_width;
        in += source_stride_pixels;
    }
}

int psp_media_present_stage_rows(int height)
{
    /* Every row the sampler can reach. A linear copy has no block alignment
       to satisfy, so this is the picture height itself rather than the next
       multiple of eight the block interleave once needed. */
    return height <= 0 ? 0 : height;
}

size_t psp_media_present_stage_bytes(int texture_width, int rows)
{
    if (texture_width <= 0 || rows <= 0) return 0;
    return (size_t) texture_width * (size_t) rows * 4u;
}

bool psp_media_present_stage_fits(
    const PspMediaPresentPlan *plan, int source_height, size_t capacity)
{
    if (plan == NULL || plan->quad_count != 1 || source_height <= 0)
        return false;
    const PspMediaPresentQuad *quad = &plan->quads[0];
    if (quad->texture_column != 0) return false;
    int rows = psp_media_present_stage_rows(source_height);
    /* The sampler never reaches past the picture, so only the blocks that
       cover it are staged -- but the declared texture is what addresses
       them, and a picture taller than its own texture would be a plan bug. */
    if (rows > quad->texture_height) rows = quad->texture_height;
    if (rows < 8 || quad->texture_width < 4) return false;
    return psp_media_present_stage_bytes(quad->texture_width, rows)
        <= capacity;
}
