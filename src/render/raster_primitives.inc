static uint64_t render_now_us(void)
{
    return tilefinch_platform_monotonic_time_us();
}

static unsigned draw_font_weight(const DrawCommand *command)
{
    uint8_t code = draw_command_font_weight_code(command);
    return code != 0 ? (unsigned) code * 10u : 400u;
}

static bool draw_uses_bold_face(const DrawCommand *command)
{
    return draw_font_weight(command) >= 650u;
}

static const uint8_t font5x7[91][7] = {
    [' ']={0,0,0,0,0,0,0}, ['!']={4,4,4,4,4,0,4},
    ['#']={10,31,10,10,31,10,0}, ['-']={0,0,0,31,0,0,0},
    ['.']={0,0,0,0,0,6,6}, [',']={0,0,0,0,6,6,4},
    [':']={0,6,6,0,6,6,0}, ['/']={1,2,4,8,16,0,0},
    ['(']={2,4,8,8,8,4,2}, [')']={8,4,2,2,2,4,8},
    ['0']={14,17,19,21,25,17,14}, ['1']={4,12,4,4,4,4,14},
    ['2']={14,17,1,2,4,8,31}, ['3']={30,1,1,14,1,1,30},
    ['4']={2,6,10,18,31,2,2}, ['5']={31,16,16,30,1,1,30},
    ['6']={14,16,16,30,17,17,14}, ['7']={31,1,2,4,8,8,8},
    ['8']={14,17,17,14,17,17,14}, ['9']={14,17,17,15,1,1,14},
    ['?']={14,17,1,2,4,0,4}, ['=']={0,31,0,31,0,0,0},
    ['A']={14,17,17,31,17,17,17}, ['B']={30,17,17,30,17,17,30},
    ['C']={14,17,16,16,16,17,14}, ['D']={30,17,17,17,17,17,30},
    ['E']={31,16,16,30,16,16,31}, ['F']={31,16,16,30,16,16,16},
    ['G']={14,17,16,23,17,17,15}, ['H']={17,17,17,31,17,17,17},
    ['I']={14,4,4,4,4,4,14}, ['J']={7,2,2,2,2,18,12},
    ['K']={17,18,20,24,20,18,17}, ['L']={16,16,16,16,16,16,31},
    ['M']={17,27,21,21,17,17,17}, ['N']={17,25,21,19,17,17,17},
    ['O']={14,17,17,17,17,17,14}, ['P']={30,17,17,30,16,16,16},
    ['Q']={14,17,17,17,21,18,13}, ['R']={30,17,17,30,20,18,17},
    ['S']={15,16,16,14,1,1,30}, ['T']={31,4,4,4,4,4,4},
    ['U']={17,17,17,17,17,17,14}, ['V']={17,17,17,17,17,10,4},
    ['W']={17,17,17,21,21,21,10}, ['X']={17,17,10,4,10,17,17},
    ['Y']={17,17,10,4,4,4,4}, ['Z']={31,1,2,4,8,16,31}
};

static uint16_t rgb565(uint32_t color)
{
    return tilefinch_rgb565_pack_u8(
        (color >> 16) & 0xffu, (color >> 8) & 0xffu, color & 0xffu);
}

static uint16_t rgb565_surface(uint32_t color)
{
    unsigned red = (color >> 16) & 0xffu;
    unsigned green = (color >> 8) & 0xffu;
    unsigned blue = color & 0xffu;
    unsigned red_code = red >> 3;
    unsigned green_code = green >> 2;
    unsigned blue_code = blue >> 3;
    uint16_t packed = tilefinch_rgb565_pack_codes(
        red_code, green_code, blue_code);

    /* Independent 5/6/5 truncation can give a large, almost gray CSS surface
       a visible magenta or green cast.  Apply coordination only to flat page
       and rectangle fills: decoded images, glyph coverage, gradients, and
       alpha blending keep the minimal per-pixel conversion above.  For an
       authored near-neutral whose decoded channel spread would exceed two
       levels, select the closest 5-bit neutral and coordinate green to the
       same decoded intensity.  This is still a pure color mapping with no
       extra storage or order dependence. */
    unsigned source_min = red < green ? red : green;
    if (blue < source_min) source_min = blue;
    unsigned source_max = red > green ? red : green;
    if (blue > source_max) source_max = blue;
    if (source_max - source_min > 2u) return packed;

    unsigned decoded_red = (red_code << 3) | (red_code >> 2);
    unsigned decoded_green = (green_code << 2) | (green_code >> 4);
    unsigned decoded_blue = (blue_code << 3) | (blue_code >> 2);
    unsigned decoded_min = decoded_red < decoded_green
        ? decoded_red : decoded_green;
    if (decoded_blue < decoded_min) decoded_min = decoded_blue;
    unsigned decoded_max = decoded_red > decoded_green
        ? decoded_red : decoded_green;
    if (decoded_blue > decoded_max) decoded_max = decoded_blue;
    if (decoded_max - decoded_min <= 2u) return packed;

    unsigned target = (red + green + blue + 1u) / 3u;
    unsigned neutral_5 = (target * 31u + 127u) / 255u;
    unsigned neutral = (neutral_5 << 3) | (neutral_5 >> 2);
    unsigned neutral_6 = (neutral * 63u + 127u) / 255u;
    return tilefinch_rgb565_pack_codes(neutral_5, neutral_6, neutral_5);
}

/* Ordered (Bayer) dithering for the 24-bit -> RGB565 quantisation step.
   Truncating to 5/6/5 bits banks up to a full quantisation step of error
   into every pixel; across a smooth ramp that error is spatially correlated,
   which is what makes banding visible.  Nudging each pixel by a threshold
   that varies over a 4x4 cell decorrelates it, trading per-pixel accuracy
   for a smoother surface.

   Ordered, NOT error diffusion.  Floyd-Steinberg pushes residual error into
   not-yet-written neighbours, so a pixel's value depends on the order pixels
   are visited.  That would break the span blits below (a span could no
   longer be a straight run), the tile cache (a tile's contents would depend
   on which tiles rasterised first), and the M3 host/device frame-hash
   determinism discipline that all PSP validation rests on.  A Bayer
   threshold is a pure function of (x, y, colour): same pixel, same answer,
   in any order, on any host.

   APPLIED ONLY TO THE RAMPS THE ENGINE SYNTHESISES: gradient fills and
   box-shadow falloffs.  Two wider scopes were tried first and both were
   clearly worse on the frames, not merely on the metrics.

   Dithering every fill: banding is a property of a ramp, and a web page is
   overwhelmingly flat fills, where there is no band to break up and the
   threshold only converts a uniform colour into visible two-tone stipple.
   On a compact news fixture it turned a flat body and masthead into a
   checkerboard, took RGB565 pixel mismatch from 25.6% to 85.7% and
   broke two floors.

   Dithering decoded images as well: a photograph arrives already carrying
   its own sensor and JPEG noise, which dithers it for free.  Adding an
   ordered threshold on top laid a regular crosshatch over an acceptance
   photo -- the row this feature was aimed at -- and moved every
   one of its metrics the wrong way (mae 7.77 -> 8.15, mismatch 39.8% ->
   40.7%, ssim 0.8570 -> 0.8518).

   What is left is exactly the case with no noise of its own: a ramp this
   renderer computed analytically, where consecutive pixels differ by less
   than one quantisation step and truncation therefore produces visible
   bands.  fixtures/banding-probe.html is the shape that motivates it.

   The 4x4 matrix is an exact fit for RGB565 rather than an arbitrary pick:
   its sixteen levels shift down to the eight thresholds a 5-bit channel
   needs (step 8) and the four a 6-bit channel needs (step 4), so every
   threshold a channel can use appears, evenly distributed.

   Coordinates are WORLD (page) coordinates: a tile always covers the same
   world pixels, so the pattern is stable in the tile cache and does not
   crawl across a surface when the page scrolls. */
static const uint8_t bayer_4x4[16] = {
     0,  8,  2, 10,
    12,  4, 14,  6,
     3, 11,  1,  9,
    15,  7, 13,  5
};

/* Applied to the SOURCE colour rather than at the quantisation point, which
   is equivalent for an opaque pixel -- rgb565(color + threshold) is exactly
   the thresholded quantisation -- and scales the dither down with alpha for
   a translucent one, so a faint overlay is not made noisy.  Keeping it here
   also leaves the blend and span machinery below completely untouched. */
static inline uint32_t rgb565_dither_source(uint32_t color, int x, int y)
{
    unsigned threshold = bayer_4x4[((unsigned) y & 3u) * 4u
                                   + ((unsigned) x & 3u)];
    unsigned r = ((color >> 16) & 0xffu) + (threshold >> 1);
    unsigned g = ((color >> 8) & 0xffu) + (threshold >> 2);
    unsigned b = (color & 0xffu) + (threshold >> 1);
    /* Clamping keeps the top of each channel exact: 0xff is already the
       maximum code and must never wrap. */
    if (r > 255u) r = 255u;
    if (g > 255u) g = 255u;
    if (b > 255u) b = 255u;
    return (color & 0xff000000u) | (r << 16) | (g << 8) | b;
}

static size_t utf8_advance(const char *text, size_t remaining,
                           unsigned char *glyph)
{
    unsigned char first = (unsigned char) text[0];
    if (first < 0x80u) {
        *glyph = first;
        return 1;
    }
    size_t width = 1;
    if ((first & 0xe0u) == 0xc0u) width = 2;
    else if ((first & 0xf0u) == 0xe0u) width = 3;
    else if ((first & 0xf8u) == 0xf0u) width = 4;
    if (width > remaining) width = 1;
    for (size_t i = 1; i < width; i++) {
        if (((unsigned char) text[i] & 0xc0u) != 0x80u) { width = 1; break; }
    }
    *glyph = '?';
    return width;
}

static bool intersects(const DrawCommand *command, int left, int top,
                       int right, int bottom)
{
    int shadow_blur = draw_command_text_shadow_blur(command);
    int64_t command_left = (int64_t) command->x - shadow_blur;
    int64_t command_right = (int64_t) command->x + command->width
                            + shadow_blur;
    int64_t command_top = (int64_t) command->y - shadow_blur;
    int64_t command_bottom = (int64_t) command->y + command->height
                             + shadow_blur;
    if (command->type == DRAW_TEXT
        && (command->image_fit & LAYOUT_TEXT_DECORATION_UNDERLINE) != 0
        && draw_command_text_underline_offset(command, NULL)) {
        /* Explicit underlines are ink overflow.  Exact indexing uses the
           selected font metrics in layout.c; this font-independent cull is
           intentionally conservative for fixed/overflow copies. */
        int extent = command->font_size;
        if (extent < 0) extent = 0;
        if (extent > TILEFINCH_FONT_RASTER_PIXEL_LIMIT) {
            extent = TILEFINCH_FONT_RASTER_PIXEL_LIMIT;
        }
        extent += 17;
        command_top -= extent;
        command_bottom += extent;
    }
    return command_left < right
           && command_right > left
           && command_top < bottom
           && command_bottom > top;
}

static bool command_in_overflow(const LayoutDocument *layout, size_t index)
{
    for (size_t i = 0; i < layout->node_box_count; i++) {
        const LayoutNodeBox *box = &layout->node_boxes[i];
        if ((box->clips_x || box->clips_y)
            && index >= box->scroll_command_start
            && index < box->scroll_command_end) return true;
    }
    return false;
}

static bool node_is_descendant(const lxb_dom_node_t *node,
                               const lxb_dom_node_t *ancestor)
{
    for (const lxb_dom_node_t *at = node; at != NULL; at = at->parent) {
        if (at == ancestor) return true;
    }
    return false;
}

static void box_visual_origin(const LayoutDocument *layout,
                              const LayoutNodeBox *box, int *x, int *y)
{
    *x = box->x;
    *y = box->y;
    for (size_t i = 0; i < layout->node_box_count; i++) {
        const LayoutNodeBox *ancestor = &layout->node_boxes[i];
        if (ancestor == box || ancestor->node == NULL || box->node == NULL
            || !node_is_descendant(box->node->parent, ancestor->node)) {
            continue;
        }
        *x -= ancestor->scroll_x;
        *y -= ancestor->scroll_y;
    }
}

static void overflow_clip_rect(const LayoutNodeBox *box, int visual_x,
                               int visual_y, int *left, int *top,
                               int *right, int *bottom)
{
    StyleOverflowClipBox clip_box = layout_node_box_clip_box(box);
    int margin = (int) layout_node_box_clip_margin(box);
    int width = clip_box == STYLE_OVERFLOW_CLIP_BORDER_BOX
        ? box->width
        : (clip_box == STYLE_OVERFLOW_CLIP_CONTENT_BOX
           ? box->width - 2 * box->clip_inset_left : box->client_width);
    int height = clip_box == STYLE_OVERFLOW_CLIP_BORDER_BOX
        ? box->height
        : (clip_box == STYLE_OVERFLOW_CLIP_CONTENT_BOX
           ? box->height - 2 * box->clip_inset_top : box->client_height);
    if (width < 0) width = 0;
    if (height < 0) height = 0;
    *left = visual_x + box->clip_inset_left - margin;
    *top = visual_y + box->clip_inset_top - margin;
    *right = *left + width + 2 * margin;
    *bottom = *top + height + 2 * margin;
}

static int overflow_clip_radius(const LayoutDocument *layout,
                                const LayoutNodeBox *box)
{
    return layout_node_box_effective_clip_radius_code(layout, box);
}

static bool overflow_command_geometry(const LayoutDocument *layout,
                                      size_t index, int *dx, int *dy,
                                      int *clip_left, int *clip_top,
                                      int *clip_right, int *clip_bottom)
{
    bool found = false;
    *dx = 0;
    *dy = 0;
    *clip_left = INT_MIN / 2;
    *clip_top = INT_MIN / 2;
    *clip_right = INT_MAX / 2;
    *clip_bottom = INT_MAX / 2;
    for (size_t i = 0; i < layout->node_box_count; i++) {
        const LayoutNodeBox *box = &layout->node_boxes[i];
        if ((!box->clips_x && !box->clips_y)
            || index < box->scroll_command_start
            || index >= box->scroll_command_end
            || layout_positioned_command_escapes_clip(
                   layout, index, box)) continue;
        found = true;
        *dx -= box->scroll_x;
        *dy -= box->scroll_y;
        int x = 0, y = 0;
        box_visual_origin(layout, box, &x, &y);
        int left, top, right, bottom;
        overflow_clip_rect(box, x, y, &left, &top, &right, &bottom);
        if (box->clips_x) {
            if (left > *clip_left) *clip_left = left;
            if (right < *clip_right) *clip_right = right;
        }
        if (box->clips_y) {
            if (top > *clip_top) *clip_top = top;
            if (bottom < *clip_bottom) *clip_bottom = bottom;
        }
    }
    return found && *clip_right > *clip_left && *clip_bottom > *clip_top;
}

typedef struct {
    int x;
    int y;
    int width;
    int height;
    int radius;
} RoundedClip;

#define MAXIMUM_ROUNDED_CLIPS 16

static bool rounded_distance_outside(int64_t dx, int64_t dy, int radius)
{
    if (radius <= 0) return false;
    uint64_t limit = (uint64_t) radius;
    uint64_t absolute_x = dx < 0 ? (uint64_t) (-dx) : (uint64_t) dx;
    uint64_t absolute_y = dy < 0 ? (uint64_t) (-dy) : (uint64_t) dy;
    if (absolute_x >= limit || absolute_y >= limit) return true;
    uint64_t x_squared = absolute_x * absolute_x;
    uint64_t radius_squared = limit * limit;
    /* Subtraction avoids overflowing the sum even for hostile retained
       coordinates near the signed-int limit. */
    return absolute_y * absolute_y >= radius_squared - x_squared;
}

static bool point_inside_rounded_rect_radii(
    int x, int y, int rect_x, int rect_y,
    int width, int height, const int radii[4])
{
    int64_t left = rect_x;
    int64_t top = rect_y;
    int64_t right = left + width;
    int64_t bottom = top + height;
    int64_t px = x;
    int64_t py = y;
    unsigned corner;
    int radius;
    int64_t cx, cy;
    if (px < left + radii[0] && py < top + radii[0]) {
        corner = 0;
        radius = radii[corner];
        cx = left + radius - 1;
        cy = top + radius - 1;
    } else if (px >= right - radii[1] && py < top + radii[1]) {
        corner = 1;
        radius = radii[corner];
        cx = right - radius;
        cy = top + radius - 1;
    } else if (px >= right - radii[2] && py >= bottom - radii[2]) {
        corner = 2;
        radius = radii[corner];
        cx = right - radius;
        cy = bottom - radius;
    } else if (px < left + radii[3] && py >= bottom - radii[3]) {
        corner = 3;
        radius = radii[corner];
        cx = left + radius - 1;
        cy = bottom - radius;
    } else {
        return true;
    }
    return radius <= 0
        || !rounded_distance_outside(px - cx, py - cy, radius);
}

/* Four fixed subpixel samples give rounded fills a stable 0/25/50/75/100%
   edge without floats, square roots, or a full supersampled tile. The caller
   already bounds x/y to the authored rectangle, so only corner pixels enter
   the circle test. Coordinates are in quarter-pixel units. */
static unsigned rounded_rect_coverage_quarters(
    int x, int y, int rect_x, int rect_y,
    int width, int height, const int radii[4])
{
    int64_t left = rect_x;
    int64_t top = rect_y;
    int64_t right = left + width;
    int64_t bottom = top + height;
    unsigned corner;
    int radius;
    int64_t center_x, center_y;
    if ((int64_t) x < left + radii[0]
        && (int64_t) y < top + radii[0]) {
        corner = 0;
        radius = radii[corner];
        center_x = (left + radius) * 4;
        center_y = (top + radius) * 4;
    } else if ((int64_t) x >= right - radii[1]
               && (int64_t) y < top + radii[1]) {
        corner = 1;
        radius = radii[corner];
        center_x = (right - radius) * 4;
        center_y = (top + radius) * 4;
    } else if ((int64_t) x >= right - radii[2]
               && (int64_t) y >= bottom - radii[2]) {
        corner = 2;
        radius = radii[corner];
        center_x = (right - radius) * 4;
        center_y = (bottom - radius) * 4;
    } else if ((int64_t) x < left + radii[3]
               && (int64_t) y >= bottom - radii[3]) {
        corner = 3;
        radius = radii[corner];
        center_x = (left + radius) * 4;
        center_y = (bottom - radius) * 4;
    } else {
        return 4u;
    }
    if (radius <= 0) return 4u;
    /*
     * Squaring is the only unbounded step here, and both operands come from
     * retained coordinates a hostile page can push near INT_MAX.
     * style_border_radius_resolve_corners caps a non-packed radius at
     * min(width, height)/2, which for an INT_MAX-wide box is about 2^30; at
     * quarter-pixel scale that is 2^32, and its square does not fit in the
     * 64-bit product. The sample offsets are worse: |dx| is bounded only by
     * the rectangle, so dx * dx can overflow the *signed* multiply, which is
     * undefined rather than merely wrong.
     *
     * Both are handled the way rounded_distance_outside() has always handled
     * its own: refuse the multiply instead of widening it.
     *
     * A radius past the bound falls back to that exact binary predicate,
     * which is overflow-free and answers the same question at whole-pixel
     * resolution -- subpixel coverage on a 67-million-pixel corner arc
     * cannot differ from it anyway. Below the bound, a sample farther from
     * the centre than the radius on either axis is outside the circle, so
     * skipping it before squaring cannot change the count: the strict `>`
     * keeps the |dx| == radius, dy == 0 sample that the original inequality
     * counts. Every authored box is orders of magnitude inside this, so no
     * real frame moves a pixel and the fidelity floors do not shift.
     */
    static const int64_t COVERAGE_SCALED_RADIUS_LIMIT = INT64_C(1) << 30;
    int64_t radius_scaled = (int64_t) radius * 4;
    if (radius_scaled > COVERAGE_SCALED_RADIUS_LIMIT) {
        return point_inside_rounded_rect_radii(
                   x, y, rect_x, rect_y, width, height, radii) ? 4u : 0u;
    }
    static const int offsets[2] = {1, 3};
    uint64_t radius_limit = (uint64_t) radius_scaled;
    uint64_t radius_squared = radius_limit * radius_limit;
    unsigned covered = 0;
    for (unsigned sy = 0; sy < 2u; sy++) {
        int64_t sample_y = (int64_t) y * 4 + offsets[sy];
        int64_t dy = sample_y - center_y;
        uint64_t absolute_y = dy < 0 ? (uint64_t) -dy : (uint64_t) dy;
        if (absolute_y > radius_limit) continue;
        uint64_t dy_squared = absolute_y * absolute_y;
        for (unsigned sx = 0; sx < 2u; sx++) {
            int64_t sample_x = (int64_t) x * 4 + offsets[sx];
            int64_t dx = sample_x - center_x;
            uint64_t absolute_x = dx < 0 ? (uint64_t) -dx : (uint64_t) dx;
            if (absolute_x > radius_limit) continue;
            uint64_t dx_squared = absolute_x * absolute_x;
            if (dy_squared <= radius_squared - dx_squared) covered++;
        }
    }
    return covered;
}

/* A rounded stroke is the annulus between two rounded rectangles, so its
   coverage is the outer rectangle's minus the inner one's, measured at the
   same four fixed subpixel samples the fill path uses. Straight-edge pixels
   are unaffected: the inner box edges are whole pixels, so a pixel is either
   entirely inside or entirely outside that box, and both rectangles report
   4/4 away from their corners. Only corner pixels can land between.

   For any authored border the inner rectangle is contained in the outer one
   and the difference cannot go negative. A radius the corner resolver had to
   clamp differently on each rectangle is the exception, and there the inner
   count is taken as covering the whole pixel rather than allowed to wrap. */
typedef struct {
    int x;
    int y;
    int width;
    int height;
    const int *radii;
    bool has_inner;
    int inner_x;
    int inner_y;
    int inner_right;
    int inner_bottom;
    int inner_width;
    int inner_height;
    bool has_inner_radius;
    const int *inner_radii;
} RoundedStrokeGeometry;

static unsigned rounded_stroke_coverage_quarters(
    int x, int y, const RoundedStrokeGeometry *stroke)
{
    unsigned outer = rounded_rect_coverage_quarters(
        x, y, stroke->x, stroke->y, stroke->width, stroke->height,
        stroke->radii);
    if (outer == 0 || !stroke->has_inner
        || x < stroke->inner_x || x >= stroke->inner_right
        || y < stroke->inner_y || y >= stroke->inner_bottom) {
        return outer;
    }
    unsigned inner = stroke->has_inner_radius
        ? rounded_rect_coverage_quarters(
              x, y, stroke->inner_x, stroke->inner_y,
              stroke->inner_width, stroke->inner_height, stroke->inner_radii)
        : 4u;
    return inner >= outer ? 0u : outer - inner;
}

static bool point_inside_rounded_rect(int x, int y, int rect_x, int rect_y,
                                      int width, int height, int radius_code)
{
    if (style_border_radius_maximum(radius_code) <= 0) return true;
    int radii[4];
    style_border_radius_resolve_corners(
        radius_code, width, height, radii);
    return point_inside_rounded_rect_radii(
        x, y, rect_x, rect_y, width, height, radii);
}

/* Intersect one horizontal half-open span with the exact pixel coverage of a
   rounded rectangle. A rounded rectangle has one contiguous interval on each
   row, so two bounded binary searches replace a corner test for every pixel.
   The predicate deliberately remains point_inside_rounded_rect(), keeping the
   established integer edge convention byte-for-byte. */
static bool rounded_rect_clip_row_span(int rect_x, int rect_y,
                                       int width, int height, int radius_code,
                                       int y, int *x0, int *x1)
{
    if (x0 == NULL || x1 == NULL || *x0 >= *x1
        || width <= 0 || height <= 0) return false;
    int radius = style_border_radius_maximum(radius_code);
    int radii[4];
    style_border_radius_resolve_corners(
        radius_code, width, height, radii);
    int64_t left = rect_x;
    int64_t top = rect_y;
    int64_t right = left + width;
    int64_t bottom = top + height;
    if ((int64_t) y < top || (int64_t) y >= bottom
        || (int64_t) *x1 <= left || (int64_t) *x0 >= right) {
        return false;
    }
    int64_t clipped_left = *x0 > left ? *x0 : left;
    int64_t clipped_right = *x1 < right ? *x1 : right;
    if (clipped_left >= clipped_right) return false;
    int span_left = (int) clipped_left;
    int span_right = (int) clipped_right;
    if (radius > 0
        && ((int64_t) y < top + radius
            || (int64_t) y >= bottom - radius)) {
        int64_t left_corner_end = left + radius;
        if ((int64_t) span_left < left_corner_end) {
            int low = span_left;
            int high = span_right < left_corner_end
                ? span_right : (int) left_corner_end;
            while (low < high) {
                int middle = low + (high - low) / 2;
                if (point_inside_rounded_rect_radii(
                        middle, y, rect_x, rect_y,
                        width, height, radii)) {
                    high = middle;
                } else {
                    low = middle + 1;
                }
            }
            span_left = low;
        }
        int64_t right_corner_start = right - radius;
        if ((int64_t) span_right > right_corner_start) {
            int low = span_left > right_corner_start
                ? span_left : (int) right_corner_start;
            int high = span_right;
            while (low < high) {
                int middle = low + (high - low) / 2;
                if (point_inside_rounded_rect_radii(
                        middle, y, rect_x, rect_y,
                        width, height, radii)) {
                    low = middle + 1;
                } else {
                    high = middle;
                }
            }
            span_right = low;
        }
    }
    if (span_left >= span_right) return false;
    *x0 = span_left;
    *x1 = span_right;
    return true;
}

static size_t overflow_rounded_clips(const LayoutDocument *layout,
                                     size_t index, RoundedClip *clips,
                                     size_t capacity)
{
    size_t count = 0;
    for (size_t i = 0; i < layout->node_box_count; i++) {
        const LayoutNodeBox *box = &layout->node_boxes[i];
        if (!box->clips_x || !box->clips_y
            || layout_node_box_clip_radius(box) == 0
            || index < box->scroll_command_start
            || index >= box->scroll_command_end
            || layout_positioned_command_escapes_clip(
                   layout, index, box)) continue;
        if (count >= capacity) continue;
        int x = 0, y = 0;
        box_visual_origin(layout, box, &x, &y);
        int left, top, right, bottom;
        overflow_clip_rect(box, x, y, &left, &top, &right, &bottom);
        int radius = overflow_clip_radius(layout, box);
        if (style_border_radius_maximum(radius) <= 0) continue;
        clips[count++] = (RoundedClip) {
            left, top, right - left, bottom - top, radius
        };
    }
    return count;
}

static bool point_in_rounded_clips(const RoundedClip *clips, size_t count,
                                   int x, int y)
{
    for (size_t i = 0; i < count; i++) {
        const RoundedClip *clip = &clips[i];
        if (!point_inside_rounded_rect(
                x, y, clip->x, clip->y, clip->width, clip->height,
                clip->radius)) return false;
    }
    return true;
}

typedef struct {
    uint16_t *pixels;
    uint8_t *alpha;
    size_t stride;
    int origin_x;
    int origin_y;
    int width;
    int height;
    int left;
    int top;
    int right;
    int bottom;
    const RoundedClip *rounded_clips;
    size_t rounded_clip_count;
    uint8_t filter_code;
    uint8_t forced_dark_role;
    bool forced_dark;
    bool resolved_color_valid;
    uint32_t resolved_color_source;
    uint32_t resolved_color_value;
} RasterTarget;

typedef enum {
    FORCED_DARK_NONE = 0,
    FORCED_DARK_SURFACE,
    FORCED_DARK_INK,
    FORCED_DARK_BORDER
} ForcedDarkRole;

#define OVERFLOW_PARENT_NONE UINT32_MAX

typedef struct {
    uint32_t box_index;
    int visual_x;
    int visual_y;
    int rounded_ordinal;
} OverflowClipBox;

typedef struct {
    uint32_t command_index;
    int dx;
    int dy;
    int clip_left;
    int clip_top;
    int clip_right;
    int clip_bottom;
    uint64_t rounded_mask;
    bool found;
} OverflowGeometry;

struct RenderOverflowCache {
    uint32_t *parent_indices;
    int *scroll_values;
    OverflowClipBox *clip_boxes;
    size_t clip_count;
    OverflowGeometry *geometry;
    size_t geometry_count;
    uint32_t rounded_clip_indices[64];
    size_t rounded_count;
    bool prepared;
};

static void overflow_cache_destroy(TileCache *cache)
{
    if (cache == NULL || cache->overflow_cache == NULL) return;
    struct RenderOverflowCache *overflow = cache->overflow_cache;
    budget_free(cache->budget, overflow->geometry);
    budget_free(cache->budget, overflow->clip_boxes);
    budget_free(cache->budget, overflow->scroll_values);
    budget_free(cache->budget, overflow->parent_indices);
    budget_free(cache->budget, overflow);
    cache->overflow_cache = NULL;
    cache->overflow_cache_bytes = 0;
}

static bool overflow_cache_size_add(size_t *total, size_t count,
                                    size_t width)
{
    if (count != 0 && width > SIZE_MAX / count) return false;
    size_t bytes = count * width;
    if (*total > SIZE_MAX - bytes) return false;
    *total += bytes;
    return true;
}

static bool overflow_cache_build(TileCache *cache)
{
    const LayoutDocument *layout = cache->layout;
    if (layout == NULL || layout->command_flags == NULL
        || layout->node_box_count == 0
        || layout->node_box_count > UINT32_MAX
        || layout->count > UINT32_MAX) return false;
    size_t clip_count = 0;
    for (size_t i = 0; i < layout->node_box_count; i++) {
        if (layout->node_boxes[i].clips_x || layout->node_boxes[i].clips_y) {
            clip_count++;
        }
    }
    size_t geometry_count = 0;
    for (size_t i = 0; i < layout->count; i++) {
        if (layout->command_flags[i] & LAYOUT_COMMAND_OVERFLOW) {
            geometry_count++;
        }
    }
    if (clip_count == 0 || geometry_count == 0) return false;
    size_t persistent_bytes = sizeof(struct RenderOverflowCache);
    if (!overflow_cache_size_add(&persistent_bytes, layout->node_box_count,
                                 sizeof(uint32_t))
        || !overflow_cache_size_add(&persistent_bytes,
                                    layout->node_box_count, 2u * sizeof(int))
        || !overflow_cache_size_add(&persistent_bytes, clip_count,
                                    sizeof(OverflowClipBox))
        || !overflow_cache_size_add(&persistent_bytes, geometry_count,
                                    sizeof(OverflowGeometry))
        || persistent_bytes > TILEFINCH_OVERFLOW_CACHE_BYTES) return false;

    struct RenderOverflowCache *overflow = budget_calloc(
        cache->budget, 1, sizeof(*overflow));
    if (overflow == NULL) return false;
    overflow->parent_indices = budget_malloc(
        cache->budget, layout->node_box_count * sizeof(uint32_t));
    overflow->scroll_values = budget_malloc(
        cache->budget, layout->node_box_count * 2u * sizeof(int));
    overflow->clip_boxes = budget_calloc(
        cache->budget, clip_count, sizeof(*overflow->clip_boxes));
    overflow->geometry = budget_calloc(
        cache->budget, geometry_count, sizeof(*overflow->geometry));
    if (overflow->parent_indices == NULL || overflow->scroll_values == NULL
        || overflow->clip_boxes == NULL
        || overflow->geometry == NULL) {
        cache->overflow_cache = overflow;
        overflow_cache_destroy(cache);
        return false;
    }
    overflow->clip_count = clip_count;
    overflow->geometry_count = geometry_count;

    for (size_t i = 0; i < layout->node_box_count; i++) {
        uint32_t parent = OVERFLOW_PARENT_NONE;
        const lxb_dom_node_t *node = layout->node_boxes[i].node;
        for (const lxb_dom_node_t *at = node != NULL ? node->parent : NULL;
             at != NULL; at = at->parent) {
            const LayoutNodeBox *parent_box = layout_box_for_node(layout, at);
            if (parent_box == NULL) continue;
            ptrdiff_t index = parent_box - layout->node_boxes;
            if (index >= 0 && (size_t) index < layout->node_box_count) {
                parent = (uint32_t) index;
                break;
            }
        }
        overflow->parent_indices[i] = parent;
    }

    size_t clip_at = 0;
    size_t rounded = 0;
    for (size_t i = 0; i < layout->node_box_count; i++) {
        const LayoutNodeBox *box = &layout->node_boxes[i];
        if (!box->clips_x && !box->clips_y) continue;
        int rounded_ordinal = -1;
        if (box->clips_x && box->clips_y
            && layout_node_box_clip_radius(box) != 0) {
            if (rounded < 64u) {
                rounded_ordinal = (int) rounded;
                overflow->rounded_clip_indices[rounded] = (uint32_t) clip_at;
            }
            rounded++;
        }
        overflow->clip_boxes[clip_at++] = (OverflowClipBox) {
            .box_index = (uint32_t) i,
            .rounded_ordinal = rounded_ordinal
        };
    }
    overflow->rounded_count = rounded;
    size_t geometry_at = 0;
    for (size_t i = 0; i < layout->count; i++) {
        if (!(layout->command_flags[i] & LAYOUT_COMMAND_OVERFLOW)) continue;
        overflow->geometry[geometry_at++].command_index = (uint32_t) i;
    }
    cache->overflow_cache = overflow;
    cache->overflow_cache_bytes = persistent_bytes;
    cache->overflow_cache_builds++;
    return true;
}

static size_t overflow_geometry_lower_bound(
    const struct RenderOverflowCache *overflow, size_t command_index)
{
    size_t low = 0, high = overflow->geometry_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2u;
        if (overflow->geometry[middle].command_index < command_index) {
            low = middle + 1u;
        } else {
            high = middle;
        }
    }
    return low;
}

static bool overflow_cache_prepare(TileCache *cache)
{
    if (cache->overflow_cache_disabled) return false;
    if (cache->overflow_cache == NULL && !overflow_cache_build(cache)) {
        cache->overflow_cache_disabled = true;
        cache->overflow_cache_fallbacks++;
        return false;
    }
    struct RenderOverflowCache *overflow = cache->overflow_cache;
    const LayoutDocument *layout = cache->layout;
    bool changed = !overflow->prepared;
    if (!changed) {
        for (size_t i = 0; i < layout->node_box_count; i++) {
            if (overflow->scroll_values[i * 2u]
                    != layout->node_boxes[i].scroll_x
                || overflow->scroll_values[i * 2u + 1u]
                    != layout->node_boxes[i].scroll_y) {
                changed = true;
                break;
            }
        }
    }
    if (!changed) return true;
    /* Fixed descendants participate in the same scroll/clip geometry.  A
       cached fixed layer therefore becomes stale whenever an internal
       overflow offset changes, even though the viewport itself did not. */
    cache->fixed_ready = false;
    for (size_t i = 0; i < layout->node_box_count; i++) {
        overflow->scroll_values[i * 2u] = layout->node_boxes[i].scroll_x;
        overflow->scroll_values[i * 2u + 1u] = layout->node_boxes[i].scroll_y;
    }
    for (size_t i = 0; i < overflow->clip_count; i++) {
        OverflowClipBox *clip = &overflow->clip_boxes[i];
        const LayoutNodeBox *box = &layout->node_boxes[clip->box_index];
        clip->visual_x = box->x;
        clip->visual_y = box->y;
        uint32_t parent = overflow->parent_indices[clip->box_index];
        size_t guard = 0;
        while (parent != OVERFLOW_PARENT_NONE
               && parent < layout->node_box_count
               && guard++ < layout->node_box_count) {
            clip->visual_x -= layout->node_boxes[parent].scroll_x;
            clip->visual_y -= layout->node_boxes[parent].scroll_y;
            parent = overflow->parent_indices[parent];
        }
    }
    for (size_t i = 0; i < overflow->geometry_count; i++) {
        OverflowGeometry *geometry = &overflow->geometry[i];
        geometry->dx = 0;
        geometry->dy = 0;
        geometry->clip_left = INT_MIN / 2;
        geometry->clip_top = INT_MIN / 2;
        geometry->clip_right = INT_MAX / 2;
        geometry->clip_bottom = INT_MAX / 2;
        geometry->rounded_mask = 0;
        geometry->found = false;
    }
    for (size_t i = 0; i < overflow->clip_count; i++) {
        const OverflowClipBox *clip = &overflow->clip_boxes[i];
        const LayoutNodeBox *box = &layout->node_boxes[clip->box_index];
        size_t at = overflow_geometry_lower_bound(
            overflow, box->scroll_command_start);
        while (at < overflow->geometry_count
               && overflow->geometry[at].command_index
                  < box->scroll_command_end) {
            OverflowGeometry *geometry = &overflow->geometry[at++];
            if (layout_positioned_command_escapes_clip(
                    layout, geometry->command_index, box)) {
                continue;
            }
            geometry->found = true;
            geometry->dx -= box->scroll_x;
            geometry->dy -= box->scroll_y;
            int left, top, right, bottom;
            overflow_clip_rect(box, clip->visual_x, clip->visual_y,
                               &left, &top, &right, &bottom);
            if (box->clips_x) {
                if (left > geometry->clip_left) {
                    geometry->clip_left = left;
                }
                if (right < geometry->clip_right) {
                    geometry->clip_right = right;
                }
            }
            if (box->clips_y) {
                if (top > geometry->clip_top) {
                    geometry->clip_top = top;
                }
                if (bottom < geometry->clip_bottom) {
                    geometry->clip_bottom = bottom;
                }
            }
            if (clip->rounded_ordinal >= 0) {
                geometry->rounded_mask |= UINT64_C(1)
                    << (unsigned) clip->rounded_ordinal;
            }
        }
    }
    overflow->prepared = true;
    return true;
}

static const OverflowGeometry *overflow_cached_geometry(
    const TileCache *cache, size_t command_index)
{
    const struct RenderOverflowCache *overflow = cache->overflow_cache;
    if (overflow == NULL || !overflow->prepared) return NULL;
    size_t at = overflow_geometry_lower_bound(overflow, command_index);
    if (at >= overflow->geometry_count
        || overflow->geometry[at].command_index != command_index) return NULL;
    return &overflow->geometry[at];
}

static bool append_cached_rounded_clip(const LayoutDocument *layout,
                                       const OverflowClipBox *clip,
                                       RoundedClip *clips, size_t capacity,
                                       size_t *count)
{
    const LayoutNodeBox *box = &layout->node_boxes[clip->box_index];
    if (!box->clips_x || !box->clips_y
        || layout_node_box_clip_radius(box) == 0) return true;
    if (*count >= capacity) return false;
    int left, top, right, bottom;
    overflow_clip_rect(box, clip->visual_x, clip->visual_y,
                       &left, &top, &right, &bottom);
    int radius = overflow_clip_radius(layout, box);
    if (style_border_radius_maximum(radius) <= 0) return true;
    clips[(*count)++] = (RoundedClip) {
        left, top, right - left, bottom - top, radius
    };
    return true;
}

static size_t overflow_cached_rounded_clips(const TileCache *cache,
                                            size_t command_index,
                                            RoundedClip *clips,
                                            size_t capacity)
{
    const struct RenderOverflowCache *overflow = cache->overflow_cache;
    const OverflowGeometry *geometry = overflow_cached_geometry(
        cache, command_index);
    if (overflow == NULL || geometry == NULL) return 0;
    size_t count = 0;
    if (overflow->rounded_count <= 64u) {
        uint64_t mask = geometry->rounded_mask;
        for (size_t ordinal = 0; ordinal < overflow->rounded_count; ordinal++) {
            if (!(mask & (UINT64_C(1) << ordinal))) continue;
            uint32_t clip_index = overflow->rounded_clip_indices[ordinal];
            if (clip_index >= overflow->clip_count
                || !append_cached_rounded_clip(
                    cache->layout, &overflow->clip_boxes[clip_index], clips,
                    capacity, &count)) break;
        }
        return count;
    }
    for (size_t i = 0; i < overflow->clip_count; i++) {
        const OverflowClipBox *clip = &overflow->clip_boxes[i];
        const LayoutNodeBox *box = &cache->layout->node_boxes[clip->box_index];
        if (command_index < box->scroll_command_start
            || command_index >= box->scroll_command_end) continue;
        if (!append_cached_rounded_clip(cache->layout, clip, clips, capacity,
                                        &count)) break;
    }
    return count;
}

static bool raster_target_index(const RasterTarget *target, int x, int y,
                                size_t *index)
{
    if (target == NULL || target->pixels == NULL
        || x < target->left || y < target->top
        || x >= target->right || y >= target->bottom
        || (target->rounded_clip_count != 0
            && !point_in_rounded_clips(target->rounded_clips,
                                       target->rounded_clip_count, x, y))) {
        return false;
    }
    int local_x = x - target->origin_x;
    int local_y = y - target->origin_y;
    if (local_x < 0 || local_y < 0
        || local_x >= target->width || local_y >= target->height) return false;
    *index = (size_t) local_y * target->stride + local_x;
    return true;
}

static RasterTarget tile_raster_target(RenderTile *tile, int left, int top,
                                       int right, int bottom)
{
    return (RasterTarget) {
        .pixels = tile->pixels,
        .stride = TILEFINCH_TILE_SIZE,
        .origin_x = left,
        .origin_y = top,
        .width = TILEFINCH_TILE_SIZE,
        .height = TILEFINCH_TILE_SIZE,
        .left = left,
        .top = top,
        .right = right,
        .bottom = bottom
    };
}

static uint16_t blend_rgb565(uint16_t destination, uint32_t foreground,
                             unsigned alpha)
{
    unsigned dr = tilefinch_rgb5_to_u8(
        tilefinch_rgb565_red_code(destination));
    unsigned dg = tilefinch_rgb6_to_u8(
        tilefinch_rgb565_green_code(destination));
    unsigned db = tilefinch_rgb5_to_u8(
        tilefinch_rgb565_blue_code(destination));
    unsigned sr = (foreground >> 16) & 0xffu;
    unsigned sg = (foreground >> 8) & 0xffu;
    unsigned sb = foreground & 0xffu;
    unsigned inverse = 255u - alpha;
    uint32_t result =
        (tilefinch_div255_u16(sr * alpha + dr * inverse + 127u) << 16)
        | (tilefinch_div255_u16(sg * alpha + dg * inverse + 127u) << 8)
        | tilefinch_div255_u16(sb * alpha + db * inverse + 127u);
    return rgb565(result);
}

static inline unsigned rgb_luminance(uint32_t color)
{
    unsigned red = (color >> 16) & 255u;
    unsigned green = (color >> 8) & 255u;
    unsigned blue = color & 255u;
    return (red * 77u + green * 150u + blue * 29u) >> 8;
}

static inline uint32_t forced_dark_color(
    uint32_t color, ForcedDarkRole role)
{
    if (role == FORCED_DARK_NONE) return color;
    unsigned red = (color >> 16) & 255u;
    unsigned green = (color >> 8) & 255u;
    unsigned blue = color & 255u;
    unsigned luminance = rgb_luminance(color);
    unsigned target = luminance;

    if (role == FORCED_DARK_SURFACE) {
        /*
         * Preserve surfaces which are already dark. Light surfaces are
         * compressed into a #121212-ish through #404040 range, retaining
         * hue through proportional channel scaling instead of inversion.
         */
        if (luminance <= 72u) return color;
        target = 18u + (255u - luminance) * 46u / 183u;
        red = red * target / luminance;
        green = green * target / luminance;
        blue = blue * target / luminance;
    } else {
        /*
         * Dark ink must become readable on the remapped surface. Existing
         * light ink remains authored-light; dark borders receive a quieter
         * lift than text so they do not dominate the page.
         */
        unsigned cutoff = role == FORCED_DARK_INK ? 176u : 144u;
        if (luminance >= cutoff) return color;
        target = role == FORCED_DARK_INK
            ? 232u - luminance * 32u / cutoff
            : 112u + luminance * 48u / cutoff;
        unsigned lift = target - luminance;
        red = red + lift > 255u ? 255u : red + lift;
        green = green + lift > 255u ? 255u : green + lift;
        blue = blue + lift > 255u ? 255u : blue + lift;
    }
    return (red << 16) | (green << 8) | blue;
}

static inline uint32_t raster_filter_color(
    uint32_t color, unsigned filter, bool forced_dark,
    ForcedDarkRole forced_dark_role)
{
    unsigned red = (color >> 16) & 255u;
    unsigned green = (color >> 8) & 255u;
    unsigned blue = color & 255u;
    if (filter == STYLE_FILTER_GRAYSCALE) {
        unsigned gray = (red * 77u + green * 150u + blue * 29u) >> 8;
        red = green = blue = gray;
    } else if (filter == STYLE_FILTER_INVERT) {
        red = 255u - red;
        green = 255u - green;
        blue = 255u - blue;
    } else if (filter == STYLE_FILTER_SEPIA) {
        unsigned source_red = red, source_green = green, source_blue = blue;
        red = (source_red * 101u + source_green * 197u
               + source_blue * 48u) >> 8;
        green = (source_red * 89u + source_green * 176u
                 + source_blue * 43u) >> 8;
        blue = (source_red * 70u + source_green * 137u
                + source_blue * 34u) >> 8;
        if (red > 255u) red = 255u;
        if (green > 255u) green = 255u;
        if (blue > 255u) blue = 255u;
    } else if (filter == STYLE_FILTER_BRIGHTEN) {
        red += red >> 2; green += green >> 2; blue += blue >> 2;
        if (red > 255u) red = 255u;
        if (green > 255u) green = 255u;
        if (blue > 255u) blue = 255u;
    } else if (filter == STYLE_FILTER_DARKEN) {
        red -= red >> 2; green -= green >> 2; blue -= blue >> 2;
    }
    color = (red << 16) | (green << 8) | blue;
    return forced_dark
        ? forced_dark_color(color, forced_dark_role) : color;
}

static inline uint32_t raster_target_color(
    const RasterTarget *target, uint32_t color)
{
    if (target->resolved_color_valid
        && color == target->resolved_color_source) {
        return target->resolved_color_value;
    }
    return raster_filter_color(
        color, target->filter_code, target->forced_dark,
        (ForcedDarkRole) target->forced_dark_role);
}

/* Blend one already-resolved pixel.  Shared by the per-pixel path and the
   span loops below so the blend math has a single definition. */
static inline void blend_at_index(RasterTarget *target, size_t index,
                                  uint32_t foreground, unsigned alpha)
{
    foreground = raster_target_color(target, foreground);
    if (target->alpha != NULL) {
        if (alpha == 255u) {
            target->pixels[index] = rgb565(foreground);
            target->alpha[index] = 255u;
            return;
        }
        unsigned destination_alpha = target->alpha[index];
        unsigned output_alpha = alpha
            + (destination_alpha * (255u - alpha) + 127u) / 255u;
        uint16_t destination = target->pixels[index];
        if (destination_alpha == 0) {
            target->pixels[index] = rgb565(foreground);
        } else {
            unsigned effective = output_alpha == 0 ? 0
                : alpha * 255u / output_alpha;
            target->pixels[index] = blend_rgb565(
                destination, foreground, effective);
        }
        target->alpha[index] = (uint8_t) output_alpha;
        return;
    }
    uint16_t destination = target->pixels[index];
    target->pixels[index] = alpha == 255u
        ? rgb565(foreground)
        : blend_rgb565(destination, foreground, alpha);
}

/* Flat CSS surfaces use the coordinated neutral quantizer regardless of
   whether clipping or rounded corners select the span or per-pixel path.
   Partially transparent surfaces still use the ordinary blend conversion. */
static inline void blend_surface_at_index(
    RasterTarget *target, size_t index, uint32_t foreground, unsigned alpha)
{
    if (alpha != 255u) {
        blend_at_index(target, index, foreground, alpha);
        return;
    }
    foreground = raster_target_color(target, foreground);
    target->pixels[index] = rgb565_surface(foreground);
    if (target->alpha != NULL) target->alpha[index] = 255u;
}

static void blend_pixel(RasterTarget *target, int world_x, int world_y,
                        uint32_t foreground, unsigned alpha)
{
    if (alpha == 0) return;
    size_t destination_index = 0;
    if (!raster_target_index(target, world_x, world_y,
                             &destination_index)) return;
    blend_at_index(target, destination_index, foreground, alpha);
}

/* Clip one horizontal span [x0, x1) on row y against the target's rectangular
   and rounded clips, resolving the exact row interval and base index once
   instead of re-deriving geometry and address per pixel. */
static bool raster_target_span(const RasterTarget *target, int y, int x0,
                               int x1, int *span_x0, int *span_x1,
                               size_t *row_index, bool *rounded)
{
    if (target == NULL || target->pixels == NULL
        || y < target->top || y >= target->bottom) return false;
    int local_y = y - target->origin_y;
    if (local_y < 0 || local_y >= target->height) return false;
    if (x0 < target->left) x0 = target->left;
    if (x1 > target->right) x1 = target->right;
    if (x0 < target->origin_x) x0 = target->origin_x;
    if (x1 > target->origin_x + target->width) {
        x1 = target->origin_x + target->width;
    }
    for (size_t i = 0; i < target->rounded_clip_count; i++) {
        const RoundedClip *clip = &target->rounded_clips[i];
        if (!rounded_rect_clip_row_span(
                clip->x, clip->y, clip->width, clip->height, clip->radius,
                y, &x0, &x1)) return false;
    }
    if (x0 >= x1) return false;
    *span_x0 = x0;
    *span_x1 = x1;
    *row_index = (size_t) local_y * target->stride
                 + (size_t) (x0 - target->origin_x);
    *rounded = false;
    return true;
}

typedef struct {
    int value;
    int quotient;
    int remainder;
    int denominator;
    int error;
} ScaleStepper;

static ScaleStepper scale_stepper(int position, int numerator,
                                  int denominator)
{
    int product = 0;
#if defined(__GNUC__) || defined(__clang__)
    if (!__builtin_mul_overflow(position, numerator, &product)) {
        return (ScaleStepper) {
            .value = product / denominator,
            .quotient = numerator / denominator,
            .remainder = numerator % denominator,
            .denominator = denominator,
            .error = product % denominator
        };
    }
#endif
    int64_t wide_product = (int64_t) position * numerator;
    ScaleStepper stepper = {
        .value = (int) (wide_product / denominator),
        .quotient = numerator / denominator,
        .remainder = numerator % denominator,
        .denominator = denominator,
        .error = (int) (wide_product % denominator)
    };
    return stepper;
}

static void scale_stepper_advance(ScaleStepper *stepper)
{
    stepper->value += stepper->quotient;
    stepper->error += stepper->remainder;
    if (stepper->error >= stepper->denominator) {
        stepper->error -= stepper->denominator;
        stepper->value++;
    }
}

/* Advances a tile coordinate already reduced into [0, period) by a
   non-negative delta, without a division.  Sizes are compared by
   subtraction so neither branch can form a sum outside the int range; the
   reduction of delta is reached only when one output pixel steps across a
   whole tile, which needs an extreme viewport scale. */
static int sprite_tile_wrap(int wrapped, int delta, int period)
{
    if (period <= 0 || delta < 0) return wrapped;
    if (delta >= period) delta %= period;
    int room = period - delta;
    return wrapped >= room ? wrapped - room : wrapped + delta;
}
