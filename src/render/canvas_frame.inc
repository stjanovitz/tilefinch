/* Canvas 2D fast frames: the opaque-canvas overlay plan, the direct
   canvas raster and blit, deferral, and the PSP validation hooks. Included
   once by render.c, in the same translation unit as the tile cache. */
typedef struct {
    DrawCommand command;
    const ImageResource *image;
    size_t command_index;
    size_t paint_order;
    int clip_left;
    int clip_top;
    int clip_right;
    int clip_bottom;
} CanvasFastFrameCandidate;

static bool canvas_surface_is_opaque(const ImageResource *image)
{
    if (image == NULL || !image->is_canvas || image->pixels == NULL
        || image->width <= 0 || image->height <= 0
        || (size_t) image->width > SIZE_MAX / (size_t) image->height
        || (size_t) image->width * (size_t) image->height
               > SIZE_MAX / 4u) {
        return false;
    }
    size_t pixels = (size_t) image->width * (size_t) image->height;
    for (size_t i = 0; i < pixels; i++) {
        if (image->pixels[i * 4u + 3u] != 255u) return false;
    }
    return true;
}

static inline uint32_t canvas_fast_rgb565(uint32_t pixel)
{
#if defined(_MIPS_ARCH_ALLEGREX)
    uint32_t red, green, blue;
    __asm__("ext %0,%1,3,5" : "=r" (red) : "r" (pixel));
    __asm__("ext %0,%1,10,6" : "=r" (green) : "r" (pixel));
    __asm__("ext %0,%1,19,5" : "=r" (blue) : "r" (pixel));
    __asm__("ins %0,%1,5,6" : "+r" (red) : "r" (green));
    __asm__("ins %0,%1,11,5" : "+r" (red) : "r" (blue));
    return red;
#else
    return ((pixel & UINT32_C(0x000000f8)) << 8)
        | ((pixel & UINT32_C(0x0000fc00)) >> 5)
        | ((pixel & UINT32_C(0x00f80000)) >> 19);
#endif
}

static inline uint32_t canvas_fast_rgb565_pair(uint32_t low, uint32_t high)
{
#if defined(_MIPS_ARCH_ALLEGREX)
    __asm__("ins %0,%1,16,16" : "+r" (low) : "r" (high));
    return low;
#else
    return low | (high << 16);
#endif
}

/* Exact nearest-neighbour 2:3 expansion used by a 320-pixel WebGL drawing
   buffer presented across the 480-pixel PSP viewport. Four source pixels
   become p0,p0,p1,p2,p2,p3, written as three aligned 32-bit pairs. */
static void canvas_fast_scale_row_3_to_2(
    uint16_t *output, const unsigned char *source, int source_width)
{
    const uint32_t *input = (const uint32_t *) (const void *) source;
    uint32_t *pairs = (uint32_t *) (void *) output;
    for (int x = 0; x < source_width; x += 4) {
        uint32_t p0 = canvas_fast_rgb565(input[x]);
        uint32_t p1 = canvas_fast_rgb565(input[x + 1]);
        uint32_t p2 = canvas_fast_rgb565(input[x + 2]);
        uint32_t p3 = canvas_fast_rgb565(input[x + 3]);
        *pairs++ = canvas_fast_rgb565_pair(p0, p0);
        *pairs++ = canvas_fast_rgb565_pair(p1, p2);
        *pairs++ = canvas_fast_rgb565_pair(p2, p3);
    }
}

#if defined(_MIPS_ARCH_ALLEGREX)
/* vt5650.q packs exactly the truncations used by canvas_fast_rgb565.
   Borrow one quad, preserving its bits and all three pending prefixes:
   no GUM context, shared scratch, GE ownership change, or allocation.
   https://github.com/pspdev/vfpu-docs (vt5650, source-prefix semantics).
   Call only with 16-byte-aligned input and a positive multiple-of-four
   width; the existing admission and additional alignment guard do this.
   Keep the ordinary kernel out of line just like the timed validation
   variant, rather than promoting an unmeasured inlined/register variant. */
static __attribute__((noinline)) void canvas_fast_scale_row_vfpu_3_to_2(
    uint16_t *output, const unsigned char *source, int source_width)
{
    uint32_t saved[4] __attribute__((aligned(16)));
    uint32_t prefix_s, prefix_t, prefix_d;
    __asm__ volatile (
        "sv.q C000,0(%3)\n"
        "mfvc %0,$128\n" "mfvc %1,$129\n" "mfvc %2,$130\n"
        "mtvc %4,$128\n" "mtvc %4,$129\n" "mtvc $0,$130\n"
        : "=&r" (prefix_s), "=&r" (prefix_t), "=&r" (prefix_d)
        : "r" (saved), "r" (UINT32_C(0xe4)) : "memory");
    const uint32_t *input = (const uint32_t *) (const void *) source;
    uint32_t *pairs = (uint32_t *) (void *) output;
    for (int x = 0; x < source_width; x += 4) {
        uint32_t low, high;
        __asm__ volatile (
            "lv.q C000,0(%2)\n" "vt5650.q C000,C000\n"
            "mfv %0,S000\n" "mfv %1,S001\n"
            : "=r" (low), "=r" (high) : "r" (input + x) : "memory");
        uint32_t p0 = low & UINT32_C(0xffff);
        *pairs++ = canvas_fast_rgb565_pair(p0, p0);
        *pairs++ = canvas_fast_rgb565_pair(low >> 16, high);
        *pairs++ = high;
    }
    __asm__ volatile (
        "lv.q C000,0(%3)\n"
        "mtvc %0,$128\n" "mtvc %1,$129\n" "mtvc %2,$130\n"
        : : "r" (prefix_s), "r" (prefix_t), "r" (prefix_d), "r" (saved)
        : "memory");
}
#endif

#ifdef TILEFINCH_PSP_VALIDATION_LOG
/* The ordinary kernel is on, as in shipping builds. canvas-vfpu-off selects
   the scalar reference for an A/B run; a failed check refuses the kernel
   until a later check passes. */
static bool canvas_validation_vfpu_conversion = true;
static bool canvas_validation_vfpu_usable = true;
static bool canvas_validation_setup_once = true;

void tile_cache_validation_canvas_setup(bool enabled)
{
    canvas_validation_setup_once = enabled;
}

void tile_cache_validation_vfpu_conversion(bool enabled)
{
    canvas_validation_vfpu_conversion = enabled && canvas_validation_vfpu_usable;
}

RenderCanvasVfpuProbe tile_cache_validation_probe_vfpu_conversion(void)
{
    RenderCanvasVfpuProbe result = {0};
    bool was_enabled = canvas_validation_vfpu_conversion;
    canvas_validation_vfpu_usable = false;
    canvas_validation_vfpu_conversion = false;
#if defined(_MIPS_ARCH_ALLEGREX)
    uint32_t input[324] __attribute__((aligned(16)));
    uint32_t expected[244] __attribute__((aligned(16)));
    uint32_t output[246] __attribute__((aligned(16)));
    uint32_t saved[4] __attribute__((aligned(16)));
    uint32_t after[4] __attribute__((aligned(16)));
    const uint32_t sentinel[4] __attribute__((aligned(16))) = {
        UINT32_C(0x01234567), UINT32_C(0x89abcdef),
        UINT32_C(0x76543210), UINT32_C(0xfedcba98)};
    uint32_t original_s, original_t, original_d;
    __asm__ volatile (
        "sv.q C000,0(%3)\n"
        "mfvc %0,$128\n" "mfvc %1,$129\n" "mfvc %2,$130\n"
        : "=r" (original_s), "=r" (original_t), "=r" (original_d)
        : "r" (saved) : "memory");
    /* Every RGB565 code, low-bit truncation variants, alpha variations,
       small/tail widths and output canaries. Source rows stay aligned. */
    for (unsigned row = 0; row < 512u; row++) {
        int width = row < 508u ? 320 : 4 + (int) (row - 508u) * 4;
        for (int x = 0; x < width; x++) {
            uint32_t code = (row * 320u + (unsigned) x) & 0xffffu;
            input[x] = ((code & 31u) << 3)
                | ((code & 0x7e0u) << 5) | ((code & 0xf800u) << 8)
                | (row & 7u) | ((row & 3u) << 8) | ((row & 7u) << 16)
                | ((row & 255u) << 24);
        }
        canvas_fast_scale_row_3_to_2(
            (uint16_t *) (void *) expected, (const unsigned char *) input, width);
        uint32_t ps = UINT32_C(0x000f4321), pt = UINT32_C(0x000e9876);
        uint32_t pd = UINT32_C(0x00000a5a);
        __asm__ volatile (
            "lv.q C000,0(%3)\n"
            "mtvc %0,$128\n" "mtvc %1,$129\n" "mtvc %2,$130\n"
            : : "r" (ps), "r" (pt), "r" (pd), "r" (sentinel) : "memory");
        output[0] = UINT32_C(0x13579bdf);
        output[1u + (unsigned) width / 4u * 3u] = UINT32_C(0x2468ace0);
        canvas_fast_scale_row_vfpu_3_to_2(
            (uint16_t *) (void *) (output + 1),
            (const unsigned char *) input, width);
        uint32_t after_s, after_t, after_d;
        __asm__ volatile (
            "sv.q C000,0(%3)\n"
            "mfvc %0,$128\n" "mfvc %1,$129\n" "mfvc %2,$130\n"
            : "=r" (after_s), "=r" (after_t), "=r" (after_d)
            : "r" (after) : "memory");
        if (after_s != ps || after_t != pt || after_d != pd
            || memcmp(after, sentinel, sizeof(after)) != 0)
            result.state_mismatches++;
        if (output[0] != UINT32_C(0x13579bdf)
            || output[1u + (unsigned) width / 4u * 3u] != UINT32_C(0x2468ace0))
            result.mismatches++;
        const uint16_t *actual = (const uint16_t *) (const void *) (output + 1);
        const uint16_t *reference = (const uint16_t *) (const void *) expected;
        for (int x = 0; x < width / 2 * 3; x++) {
            result.pixels++;
            if (actual[x] != reference[x]) result.mismatches++;
        }
        result.rows++;
    }
    __asm__ volatile (
        "lv.q C000,0(%3)\n"
        "mtvc %0,$128\n" "mtvc %1,$129\n" "mtvc %2,$130\n"
        : : "r" (original_s), "r" (original_t), "r" (original_d), "r" (saved)
        : "memory");
    result.available = true;
#endif
    canvas_validation_vfpu_usable = result.available
        && result.mismatches == 0 && result.state_mismatches == 0;
    canvas_validation_vfpu_conversion =
        was_enabled && canvas_validation_vfpu_usable;
    return result;
}
#endif

#ifndef __PSP__
size_t render_test_alpha_blend_mismatches(uint16_t destination,
                                          uint32_t foreground)
{
    size_t mismatches = 0;
    for (unsigned destination_alpha = 0; destination_alpha < 256u;
         destination_alpha++) {
        for (unsigned alpha = 0; alpha < 256u; alpha++) {
            uint16_t pixel = destination;
            uint8_t coverage = (uint8_t) destination_alpha;
            RasterTarget target = {
                .pixels = &pixel, .alpha = &coverage, .stride = 1,
                .width = 1, .height = 1, .right = 1, .bottom = 1,
                .blend_mode = LAYOUT_MIX_BLEND_NORMAL
            };
            blend_at_index(&target, 0, foreground, alpha);
            /* The arithmetic before the transparent/opaque shortcuts. */
            uint16_t expected_pixel;
            unsigned expected_alpha;
            if (alpha == 255u) {
                expected_pixel = rgb565(foreground);
                expected_alpha = 255u;
            } else {
                expected_alpha = alpha
                    + (destination_alpha * (255u - alpha) + 127u) / 255u;
                expected_pixel = destination_alpha == 0
                    ? rgb565(foreground)
                    : blend_rgb565(destination, foreground,
                                   expected_alpha == 0 ? 0
                                       : alpha * 255u / expected_alpha);
            }
            if (pixel != expected_pixel || coverage != expected_alpha)
                mismatches++;
        }
    }
    return mismatches;
}
#endif

/* The retained overlay still shows this layout at this scroll position:
   a canvas fast frame recomposes the screen exactly without new canvas
   pixels. */
static bool canvas_overlay_current(const TileCache *cache, int scroll_y,
                                   int viewport_width, int viewport_height)
{
    return cache->canvas_overlay_ready
        && cache->canvas_overlay_scroll_y == scroll_y
        && cache->canvas_overlay_viewport_width == viewport_width
        && cache->canvas_overlay_viewport_height == viewport_height
        && cache->source_layout != NULL
        && cache->canvas_overlay_scroll_generation
               == cache->source_layout->scroll_generation;
}

static bool canvas_fast_frame_candidate_impl(
    const TileCache *cache, int scroll_y, int viewport_width,
    int viewport_height, bool require_pending,
    CanvasFastFrameCandidate *candidate)
{
    if (candidate != NULL) memset(candidate, 0, sizeof(*candidate));
    /* The frame still holds this page at this scroll position, so a canvas
       frame need only repaint the canvas and what is drawn over it. */
    bool frame_valid = cache != NULL && cache->last_frame_scroll_valid
        && cache->last_frame_scroll_y == scroll_y;
    if (cache == NULL || candidate == NULL
        /* No new canvas pixels, but a decoration painted after
           composition (a focus outline, a find highlight) moved: while
           the overlay is current, a canvas frame recomposes the screen.
           The tile path would rasterize the whole page, which a canvas
           page never keeps in tiles (about 0.5 s of slices on a PSP,
           with the canvas frozen behind the pending job). */
        || (require_pending && frame_valid && !cache->canvas_paint_pending
            && !canvas_overlay_current(
                   cache, scroll_y, viewport_width, viewport_height))
        || cache->frame == NULL || cache->layout == NULL
        || cache->frame_work.pending || cache->frame_work.ready
#ifndef __PSP__
        || (!frame_valid && tilefinch_test_faults()->canvas_frame_reference)
#endif
        || viewport_width <= 0 || viewport_height <= 0
        || cache->frame_pixels
               < (size_t) viewport_width * (size_t) viewport_height
        || cache->layout->paint_order_count != cache->layout->count
        || layout_has_css_backdrop_filter(cache->layout)) return false;

    size_t canvas_count = 0;
    for (size_t order = 0; order < cache->layout->count; order++) {
        size_t index = cache->layout->paint_order[order];
        if (index >= cache->layout->count) return false;
        const DrawCommand *command = &cache->layout->commands[index];
        if (command->type != DRAW_IMAGE || command->image == NULL
            || !command->image->is_canvas) continue;
        canvas_count++;
        bool positioned_range = false;
        for (size_t range = 0; range < cache->layout->fixed_count; range++) {
            const FixedRange *fixed = &cache->layout->fixed_ranges[range];
            if (index >= fixed->command_start && index < fixed->command_end) {
                positioned_range = true;
                break;
            }
        }
        for (size_t range = 0;
             !positioned_range && range < cache->layout->sticky_count;
             range++) {
            const StickyRange *sticky = &cache->layout->sticky_ranges[range];
            if (index >= sticky->command_start
                && index < sticky->command_end) positioned_range = true;
        }
        uint8_t flags = cache->layout->command_flags == NULL
            ? 0 : cache->layout->command_flags[index];
        if (canvas_count != 1u
            || positioned_range
            || (flags & (LAYOUT_COMMAND_FIXED
                         | LAYOUT_COMMAND_LATE_POSITIONED)) != 0
            || (flags != 0 && (flags & LAYOUT_COMMAND_OVERFLOW) == 0)
            || command->image_fit != LAYOUT_IMAGE_FIT_STRETCH
            || draw_command_rotation_quadrants(command) != 0u
            || draw_command_image_clip_radius(command) != 0
            || draw_command_filter_code(command) != 0u
            || draw_command_blend_mode(command) != LAYOUT_MIX_BLEND_NORMAL
            || command->opacity_scale != 256u) {
            return false;
        }
        candidate->command = *command;
        candidate->image = command->image;
        candidate->command_index = index;
        candidate->paint_order = order;
        candidate->clip_left = 0;
        candidate->clip_top = scroll_y;
        candidate->clip_right = viewport_width;
        candidate->clip_bottom = scroll_y + viewport_height;
        if ((flags & LAYOUT_COMMAND_OVERFLOW) != 0
            || (cache->layout->command_flags == NULL
                && command_in_overflow(cache->layout, index))) {
            int dx = 0, dy = 0;
            const OverflowGeometry *geometry =
                overflow_cached_geometry(cache, index);
            if (geometry != NULL) {
                if (!geometry->found) return false;
                dx = geometry->dx;
                dy = geometry->dy;
                candidate->clip_left = geometry->clip_left;
                candidate->clip_top = geometry->clip_top;
                candidate->clip_right = geometry->clip_right;
                candidate->clip_bottom = geometry->clip_bottom;
                RoundedClip rounded[MAXIMUM_ROUNDED_CLIPS];
                if (overflow_cached_rounded_clips(
                        cache, index, rounded,
                        MAXIMUM_ROUNDED_CLIPS) != 0) return false;
            } else {
                if (!overflow_command_geometry(
                        cache->layout, index, &dx, &dy,
                        &candidate->clip_left, &candidate->clip_top,
                        &candidate->clip_right, &candidate->clip_bottom, NULL)) {
                    return false;
                }
                RoundedClip rounded[MAXIMUM_ROUNDED_CLIPS];
                if (overflow_rounded_clips(
                        cache->layout, index, rounded,
                        MAXIMUM_ROUNDED_CLIPS, NULL) != 0) return false;
            }
            candidate->command.x = render_saturating_add_int(
                candidate->command.x, dx);
            candidate->command.y = render_saturating_add_int(
                candidate->command.y, dy);
        }
        if (candidate->clip_left < 0) candidate->clip_left = 0;
        if (candidate->clip_top < scroll_y) candidate->clip_top = scroll_y;
        if (candidate->clip_right > viewport_width)
            candidate->clip_right = viewport_width;
        if (candidate->clip_bottom > scroll_y + viewport_height)
            candidate->clip_bottom = scroll_y + viewport_height;
        if (candidate->clip_right <= candidate->clip_left
            || candidate->clip_bottom <= candidate->clip_top
            || !intersects(&candidate->command,
                           candidate->clip_left, candidate->clip_top,
                           candidate->clip_right, candidate->clip_bottom)) {
            return false;
        }
    }
    /* Without a valid previous frame (a relayout replaced the layout, or
       the view moved) only an opaque canvas covering the whole viewport
       can compose the frame by itself: it hides everything painted before
       it, and the overlay and fixed layer are rebuilt over it. Rasterizing
       that hidden page in tile slices is what made a full-screen game's
       first frame wait about a second on a PSP. */
    return canvas_count == 1u
        && (frame_valid
            || (candidate->clip_left == 0
                && candidate->clip_top == scroll_y
                && candidate->clip_right == viewport_width
                && candidate->clip_bottom == scroll_y + viewport_height
                && candidate->command.x <= 0
                && candidate->command.y <= scroll_y
                && (int64_t) candidate->command.x + candidate->command.width
                       >= viewport_width
                && (int64_t) candidate->command.y + candidate->command.height
                       >= (int64_t) scroll_y + viewport_height))
        && (candidate->image->canvas_opaque
            || canvas_surface_is_opaque(candidate->image));
}

static bool canvas_fast_frame_candidate(
    const TileCache *cache, int scroll_y, int viewport_width,
    int viewport_height, CanvasFastFrameCandidate *candidate)
{
    return canvas_fast_frame_candidate_impl(
        cache, scroll_y, viewport_width, viewport_height, true, candidate);
}

/* Animated canvases are often followed by a small HTML HUD. Retain only
   disjoint post-canvas regions: a top score and bottom power label should not
   turn into a second viewport-sized surface. On a DOM mutation the exact
   damage region is cleared and rerasterized in place, so the common fixed-
   width score update never rebuilds the other HUD regions. */
/* One full PSP viewport (480x272 = 130,560 pixels; 3 bytes each with
   alpha). A game's menu panel over its live canvas is viewport-sized; past
   the former 32,768-pixel cap it fell back to rasterizing every overlay
   command (text, rounded borders, a blurred shadow) on every canvas frame:
   about 260 ms per frame for Treadline's title menu on a PSP-3000. The
   retained copy is Budget-admitted, so a refusal keeps that fallback. */
enum { CANVAS_OVERLAY_PIXEL_LIMIT = 131072 };

typedef struct {
    RenderCanvasOverlayRegion regions[TILEFINCH_CANVAS_OVERLAY_REGION_LIMIT];
    size_t count;
    size_t pixels;
} CanvasOverlayPlan;

typedef struct {
    DrawCommand command;
    int clip_left;
    int clip_top;
    int clip_right;
    int clip_bottom;
    RoundedClip rounded_clips[MAXIMUM_ROUNDED_CLIPS];
    size_t rounded_clip_count;
    bool visible;
} CanvasOverlayCommand;

static void canvas_overlay_cache_clear(TileCache *cache)
{
    if (cache == NULL) return;
    if (cache->budget != NULL) {
        budget_free(cache->budget, cache->canvas_overlay_alpha);
        budget_free(cache->budget, cache->canvas_overlay_pixels);
        budget_free(cache->budget, cache->canvas_overlay_blend_cache);
    }
    cache->canvas_overlay_blend_cache = NULL;
    cache->canvas_overlay_pixels = NULL;
    cache->canvas_overlay_alpha = NULL;
    cache->canvas_overlay_region_count = 0;
    cache->canvas_overlay_pixel_count = 0;
    cache->canvas_overlay_ready = false;
}

static bool canvas_overlay_plan_add(CanvasOverlayPlan *plan,
                                    int left, int top, int right, int bottom)
{
    RenderCanvasOverlayRegion next = {
        .left = (int16_t) left, .top = (int16_t) top,
        .width = (uint16_t) (right - left),
        .height = (uint16_t) (bottom - top)
    };
    for (size_t i = 0; i < plan->count;) {
        RenderCanvasOverlayRegion *region = &plan->regions[i];
        bool disjoint = next.left >= region->left + (int) region->width
            || next.left + (int) next.width <= region->left
            || next.top >= region->top + (int) region->height
            || next.top + (int) next.height <= region->top;
        if (disjoint && i + 1u == plan->count
            && plan->count >= TILEFINCH_CANVAS_OVERLAY_REGION_LIMIT) {
            /* Out of regions (a menu panel plus its scattered labels):
               fold the new box into the existing region whose bounding
               union grows least, then re-merge. Larger regions only carry
               more transparent pixels; the pixel cap still bounds memory. */
            size_t best = 0;
            uint64_t best_growth = UINT64_MAX;
            for (size_t j = 0; j < plan->count; j++) {
                const RenderCanvasOverlayRegion *candidate = &plan->regions[j];
                int l = next.left < candidate->left ? next.left : candidate->left;
                int t = next.top < candidate->top ? next.top : candidate->top;
                int r = next.left + (int) next.width;
                int cr = candidate->left + (int) candidate->width;
                if (cr > r) r = cr;
                int b = next.top + (int) next.height;
                int cb = candidate->top + (int) candidate->height;
                if (cb > b) b = cb;
                uint64_t growth = (uint64_t) (r - l) * (uint64_t) (b - t)
                    - (uint64_t) candidate->width * candidate->height;
                if (growth < best_growth) {
                    best_growth = growth;
                    best = j;
                }
            }
            i = best;
            region = &plan->regions[i];
            disjoint = false;
        }
        if (disjoint) {
            i++;
            continue;
        }
        int merged_left = next.left < region->left
            ? next.left : region->left;
        int merged_top = next.top < region->top ? next.top : region->top;
        int merged_right = next.left + (int) next.width;
        int region_right = region->left + (int) region->width;
        if (region_right > merged_right) merged_right = region_right;
        int merged_bottom = next.top + (int) next.height;
        int region_bottom = region->top + (int) region->height;
        if (region_bottom > merged_bottom) merged_bottom = region_bottom;
        next.left = (int16_t) merged_left;
        next.top = (int16_t) merged_top;
        next.width = (uint16_t) (merged_right - merged_left);
        next.height = (uint16_t) (merged_bottom - merged_top);
        plan->count--;
        memmove(&plan->regions[i], &plan->regions[i + 1u],
                (plan->count - i) * sizeof(*plan->regions));
        i = 0;
    }
    if (plan->count >= TILEFINCH_CANVAS_OVERLAY_REGION_LIMIT) return false;
    plan->regions[plan->count++] = next;
    return true;
}

static void canvas_overlay_command(
    TileCache *cache, size_t index, int scroll_y,
    int viewport_width, int viewport_height, CanvasOverlayCommand *resolved)
{
    memset(resolved, 0, sizeof(*resolved));
    resolved->command = cache->layout->commands[index];
    resolved->clip_left = 0;
    resolved->clip_top = scroll_y;
    resolved->clip_right = viewport_width;
    resolved->clip_bottom = scroll_y + viewport_height;
    uint8_t flags = cache->layout->command_flags[index];
    bool overflow = (flags & LAYOUT_COMMAND_OVERFLOW) != 0;
    if (overflow) {
        int dx = 0, dy = 0;
        if (!overflow_command_geometry(
                cache->layout, index, &dx, &dy,
                &resolved->clip_left, &resolved->clip_top,
                &resolved->clip_right, &resolved->clip_bottom, NULL)) return;
        resolved->command.x = render_saturating_add_int(
            resolved->command.x, dx);
        resolved->command.y = render_saturating_add_int(
            resolved->command.y, dy);
        resolved->rounded_clip_count = overflow_rounded_clips(
            cache->layout, index, resolved->rounded_clips,
            MAXIMUM_ROUNDED_CLIPS, NULL);
    }
    if (resolved->clip_left < 0) resolved->clip_left = 0;
    if (resolved->clip_top < scroll_y) resolved->clip_top = scroll_y;
    if (resolved->clip_right > viewport_width) {
        resolved->clip_right = viewport_width;
    }
    if (resolved->clip_bottom > scroll_y + viewport_height) {
        resolved->clip_bottom = scroll_y + viewport_height;
    }
    resolved->visible = resolved->clip_right > resolved->clip_left
        && resolved->clip_bottom > resolved->clip_top
        && intersects(&resolved->command, resolved->clip_left,
                      resolved->clip_top, resolved->clip_right,
                      resolved->clip_bottom);
}

static bool canvas_overlay_plan(
    TileCache *cache, const CanvasFastFrameCandidate *candidate,
    int scroll_y, int viewport_width, int viewport_height,
    CanvasOverlayPlan *plan)
{
    memset(plan, 0, sizeof(*plan));
    if (cache->layout->sticky_count != 0
        || cache->layout->command_flags == NULL) return false;
    for (size_t order = candidate->paint_order + 1u;
         order < cache->layout->count; order++) {
        size_t index = cache->layout->paint_order[order];
        uint8_t flags = cache->layout->command_flags[index];
        if ((flags & LAYOUT_COMMAND_UNTILED) != 0) continue;
        bool overflow = (flags & LAYOUT_COMMAND_OVERFLOW) != 0;
        if (!overflow && flags != 0
            && (flags & LAYOUT_COMMAND_LATE_POSITIONED) == 0) {
            continue;
        }
        const DrawCommand *command = &cache->layout->commands[index];
        if (draw_command_blend_mode(command) != LAYOUT_MIX_BLEND_NORMAL
            || draw_command_backdrop_blur(command) != 0u
            || (command->type == DRAW_IMAGE && command->image != NULL
                && command->image->is_canvas)) return false;
        CanvasOverlayCommand resolved;
        canvas_overlay_command(
            cache, index, scroll_y, viewport_width, viewport_height,
            &resolved);
        if (!resolved.visible) continue;
        int left = resolved.command.x > resolved.clip_left
            ? resolved.command.x : resolved.clip_left;
        int top = resolved.command.y > resolved.clip_top
            ? resolved.command.y : resolved.clip_top;
        int right = resolved.command.x + resolved.command.width;
        int bottom = resolved.command.y + resolved.command.height;
        if (right > resolved.clip_right) right = resolved.clip_right;
        if (bottom > resolved.clip_bottom) bottom = resolved.clip_bottom;
        if (right <= left || bottom <= top) continue;
        if (!canvas_overlay_plan_add(
                plan, left, top - scroll_y, right, bottom - scroll_y)) {
            return false;
        }
    }
    for (size_t i = 0; i < plan->count; i++) {
        RenderCanvasOverlayRegion *region = &plan->regions[i];
        size_t area = (size_t) region->width * region->height;
        if (area > CANVAS_OVERLAY_PIXEL_LIMIT - plan->pixels) return false;
        region->pixel_offset = (uint32_t) plan->pixels;
        plan->pixels += area;
    }
    return true;
}

/* clip_* bound the rasterized pixels in world coordinates: a patch
   repaints only its damage rectangle of each region. Every raster arm is
   pixel-local (the shadow falloff is analytic, dithering is positional),
   so commands clipped to the rectangle reproduce a full build there. */
static void canvas_overlay_raster(
    TileCache *cache, const CanvasFastFrameCandidate *candidate,
    uint8_t region_mask, int clip_left, int clip_top, int clip_right,
    int clip_bottom)
{
    for (size_t order = candidate->paint_order + 1u;
         order < cache->layout->count; order++) {
        size_t index = cache->layout->paint_order[order];
        uint8_t flags = cache->layout->command_flags[index];
        bool overflow = (flags & LAYOUT_COMMAND_OVERFLOW) != 0;
        if ((flags & LAYOUT_COMMAND_UNTILED) != 0
            || (!overflow && flags != 0
                && (flags & LAYOUT_COMMAND_LATE_POSITIONED) == 0)) continue;
        CanvasOverlayCommand resolved;
        canvas_overlay_command(
            cache, index, cache->canvas_overlay_scroll_y,
            cache->canvas_overlay_viewport_width,
            cache->canvas_overlay_viewport_height, &resolved);
        if (!resolved.visible) continue;
        const DrawCommand *command = &resolved.command;
        for (size_t i = 0; i < cache->canvas_overlay_region_count; i++) {
            if ((region_mask & (UINT8_C(1) << i)) == 0) continue;
            const RenderCanvasOverlayRegion *region =
                &cache->canvas_overlay_regions[i];
            int world_top = cache->canvas_overlay_scroll_y + region->top;
            int area_left = region->left > clip_left
                ? region->left : clip_left;
            int area_top = world_top > clip_top ? world_top : clip_top;
            int area_right = region->left + (int) region->width;
            if (area_right > clip_right) area_right = clip_right;
            int area_bottom = world_top + (int) region->height;
            if (area_bottom > clip_bottom) area_bottom = clip_bottom;
            if (area_right <= area_left || area_bottom <= area_top
                || !intersects(command, area_left, area_top, area_right,
                               area_bottom)) continue;
            RasterTarget target = {
                .pixels = cache->canvas_overlay_pixels
                    + region->pixel_offset,
                .alpha = cache->canvas_overlay_alpha
                    + region->pixel_offset,
                .stride = region->width,
                .origin_x = region->left,
                .origin_y = world_top,
                .width = region->width,
                .height = region->height,
                .left = area_left,
                .top = area_top,
                .right = area_right,
                .bottom = area_bottom,
                .rounded_clips = resolved.rounded_clips,
                .rounded_clip_count = resolved.rounded_clip_count
            };
            if (resolved.clip_left > target.left) {
                target.left = resolved.clip_left;
            }
            if (resolved.clip_top > target.top) target.top = resolved.clip_top;
            if (resolved.clip_right < target.right) {
                target.right = resolved.clip_right;
            }
            if (resolved.clip_bottom < target.bottom) {
                target.bottom = resolved.clip_bottom;
            }
            rasterize_command(cache, &target, command);
        }
    }
}

static bool canvas_overlay_build(
    TileCache *cache, const CanvasFastFrameCandidate *candidate,
    int scroll_y, int viewport_width, int viewport_height)
{
    CanvasOverlayPlan plan;
    if (!canvas_overlay_plan(cache, candidate, scroll_y, viewport_width,
                             viewport_height, &plan)) return false;
    canvas_overlay_cache_clear(cache);
    if (plan.pixels != 0) {
        cache->canvas_overlay_pixels = budget_calloc(
            cache->budget, plan.pixels,
            sizeof(*cache->canvas_overlay_pixels));
        cache->canvas_overlay_alpha = budget_calloc(
            cache->budget, plan.pixels,
            sizeof(*cache->canvas_overlay_alpha));
        if (cache->canvas_overlay_pixels == NULL
            || cache->canvas_overlay_alpha == NULL) {
            canvas_overlay_cache_clear(cache);
            return false;
        }
    }
    memcpy(cache->canvas_overlay_regions, plan.regions,
           plan.count * sizeof(*plan.regions));
    cache->canvas_overlay_region_count = plan.count;
    cache->canvas_overlay_pixel_count = plan.pixels;
    cache->canvas_overlay_scroll_y = scroll_y;
    cache->canvas_overlay_viewport_width = viewport_width;
    cache->canvas_overlay_viewport_height = viewport_height;
    cache->canvas_overlay_scroll_generation =
        cache->source_layout->scroll_generation;
    if (plan.count != 0) {
        uint8_t all = (uint8_t) ((UINT16_C(1) << plan.count) - 1u);
        canvas_overlay_raster(cache, candidate, all, INT_MIN, INT_MIN,
                              INT_MAX, INT_MAX);
    }
    cache->canvas_overlay_ready = true;
    cache->canvas_overlay_builds++;
    return true;
}

static bool canvas_overlay_patch(
    TileCache *cache, const CanvasFastFrameCandidate *candidate,
    int damage_left, int damage_top, int damage_right, int damage_bottom)
{
    CanvasOverlayPlan plan;
    if (!canvas_overlay_plan(
            cache, candidate, cache->canvas_overlay_scroll_y,
            cache->canvas_overlay_viewport_width,
            cache->canvas_overlay_viewport_height, &plan)
        || !cache->canvas_overlay_ready
        || cache->canvas_overlay_region_count != plan.count
        || cache->canvas_overlay_pixel_count != plan.pixels) return false;
    for (size_t i = 0; i < plan.count; i++) {
        const RenderCanvasOverlayRegion *left =
            &cache->canvas_overlay_regions[i];
        const RenderCanvasOverlayRegion *right = &plan.regions[i];
        if (left->left != right->left || left->top != right->top
            || left->width != right->width
            || left->height != right->height) return false;
    }
    /* Repaint only the damage rectangle within each region it touches
       (a focus ring or one text line, not the whole panel and its glow).
       The damage is the union of changed command boxes; the margin covers
       glyph ink that overhangs its advance box. */
    enum { CANVAS_OVERLAY_DAMAGE_MARGIN = 2 };
    cache->canvas_overlay_scroll_generation =
        cache->source_layout->scroll_generation;
    if (damage_right <= damage_left || damage_bottom <= damage_top)
        return true;
    damage_left = render_saturating_subtract_int(
        damage_left, CANVAS_OVERLAY_DAMAGE_MARGIN);
    damage_top = render_saturating_subtract_int(
        damage_top, CANVAS_OVERLAY_DAMAGE_MARGIN);
    damage_right = render_saturating_add_int(
        damage_right, CANVAS_OVERLAY_DAMAGE_MARGIN);
    damage_bottom = render_saturating_add_int(
        damage_bottom, CANVAS_OVERLAY_DAMAGE_MARGIN);
    uint8_t affected = 0;
    int screen_top = render_saturating_subtract_int(
        damage_top, cache->canvas_overlay_scroll_y);
    int screen_bottom = render_saturating_subtract_int(
        damage_bottom, cache->canvas_overlay_scroll_y);
    for (size_t i = 0; i < cache->canvas_overlay_region_count; i++) {
        const RenderCanvasOverlayRegion *region =
            &cache->canvas_overlay_regions[i];
        int left = region->left > damage_left ? region->left : damage_left;
        int top = region->top > screen_top ? region->top : screen_top;
        int right = region->left + (int) region->width;
        if (right > damage_right) right = damage_right;
        int bottom = region->top + (int) region->height;
        if (bottom > screen_bottom) bottom = screen_bottom;
        if (right <= left || bottom <= top) continue;
        affected |= (uint8_t) (UINT8_C(1) << i);
        size_t width = (size_t) (right - left);
        for (int y = top; y < bottom; y++) {
            size_t at = region->pixel_offset
                + (size_t) (y - region->top) * region->width
                + (size_t) (left - region->left);
            memset(cache->canvas_overlay_pixels + at, 0,
                   width * sizeof(*cache->canvas_overlay_pixels));
            memset(cache->canvas_overlay_alpha + at, 0,
                   width * sizeof(*cache->canvas_overlay_alpha));
        }
        cache->canvas_overlay_patch_regions++;
        cache->canvas_overlay_patch_pixels += width * (size_t) (bottom - top);
    }
    if (affected == 0) return true;
    canvas_overlay_raster(cache, candidate, affected, damage_left,
                          damage_top, damage_right, damage_bottom);
    canvas_overlay_blend_unplan(cache);
    cache->canvas_overlay_patches++;
    return true;
}

/* Exact run blending. Each retained overlay row is walked as runs of one
   (colour, alpha): transparent runs are skipped, opaque runs are stored,
   and a long translucent run (a panel background) uses per-channel tables
   for its (colour, alpha) instead of blend_rgb565's expansions, multiplies
   and divisions per pixel. blend_rgb565 is per channel and rgb565 packs
   channels independently, so the OR of the three entries is bit-identical.
   Short runs (antialiased glyph edges) keep the reference blend. */
typedef struct {
    uint32_t key;
    uint16_t red[32];
    uint16_t green[64];
    uint16_t blue[32];
} CanvasOverlayBlend;

static uint32_t canvas_overlay_rgb(uint16_t color)
{
    return (tilefinch_rgb5_to_u8(tilefinch_rgb565_red_code(color)) << 16)
        | (tilefinch_rgb6_to_u8(tilefinch_rgb565_green_code(color)) << 8)
        | tilefinch_rgb5_to_u8(tilefinch_rgb565_blue_code(color));
}

static void canvas_overlay_blend_build(CanvasOverlayBlend *blend,
                                       uint16_t color, unsigned alpha)
{
    uint32_t rgb = canvas_overlay_rgb(color);
    unsigned inverse = 255u - alpha;
    unsigned sr = ((rgb >> 16) & 0xffu) * alpha + 127u;
    unsigned sg = ((rgb >> 8) & 0xffu) * alpha + 127u;
    unsigned sb = (rgb & 0xffu) * alpha + 127u;
    for (unsigned code = 0; code < 32u; code++) {
        unsigned d = tilefinch_rgb5_to_u8(code);
        blend->red[code] = tilefinch_rgb565_pack_u8(
            tilefinch_div255_u16(sr + d * inverse), 0u, 0u);
        blend->blue[code] = tilefinch_rgb565_pack_u8(
            0u, 0u, tilefinch_div255_u16(sb + d * inverse));
    }
    for (unsigned code = 0; code < 64u; code++)
        blend->green[code] = tilefinch_rgb565_pack_u8(
            0u, tilefinch_div255_u16(
                sg + tilefinch_rgb6_to_u8(code) * inverse), 0u);
    blend->key = (uint32_t) color << 8 | alpha;
}

enum { CANVAS_OVERLAY_TABLE_RUN = 16 };

/* The retained overlay is unchanged from frame to frame, so its translucent
   (colour, alpha) pairs are too: a panel background over its own dithered
   glow is about sixteen pairs repeating across tens of thousands of pixels.
   The first blit after the overlay changes counts pixels per pair (two
   heavy-hitter counters per hashed set) and builds tables for the pairs
   that cover enough pixels to repay one; every later frame blends those
   pixels with three table lookups. Other pairs keep the reference blend. */
enum {
    CANVAS_OVERLAY_BLEND_SETS = 32,
    /* A table costs about as much as this many reference blends. */
    CANVAS_OVERLAY_BLEND_MINIMUM_PIXELS = 48
};

typedef struct CanvasOverlayBlendCache {
    CanvasOverlayBlend blends[CANVAS_OVERLAY_BLEND_SETS][2];
    uint32_t counts[CANVAS_OVERLAY_BLEND_SETS][2];
    bool planned;
} CanvasOverlayBlendCache;

static void canvas_overlay_blend_unplan(TileCache *cache)
{
    if (cache->canvas_overlay_blend_cache != NULL)
        cache->canvas_overlay_blend_cache->planned = false;
}

static unsigned canvas_overlay_blend_set(uint32_t key)
{
    return (unsigned) ((key * UINT32_C(2654435761)) >> 27);
}

static void canvas_overlay_blend_count(CanvasOverlayBlendCache *tables,
                                       uint32_t key, uint32_t pixels)
{
    unsigned set = canvas_overlay_blend_set(key);
    CanvasOverlayBlend *ways = tables->blends[set];
    uint32_t *counts = tables->counts[set];
    for (unsigned way = 0; way < 2u; way++) {
        if (ways[way].key == key) {
            counts[way] += pixels;
            return;
        }
    }
    for (unsigned way = 0; way < 2u; way++) {
        if (counts[way] == 0) {
            ways[way].key = key;
            counts[way] = pixels;
            return;
        }
    }
    uint32_t drop = counts[0] < counts[1] ? counts[0] : counts[1];
    if (pixels < drop) drop = pixels;
    for (unsigned way = 0; way < 2u; way++) {
        counts[way] -= drop;
        if (counts[way] == 0) ways[way].key = UINT32_MAX;
    }
}

static void canvas_overlay_blend_plan(TileCache *cache,
                                      CanvasOverlayBlendCache *tables)
{
    for (unsigned set = 0; set < CANVAS_OVERLAY_BLEND_SETS; set++) {
        for (unsigned way = 0; way < 2u; way++) {
            tables->blends[set][way].key = UINT32_MAX;
            tables->counts[set][way] = 0;
        }
    }
    for (size_t i = 0; i < cache->canvas_overlay_region_count; i++) {
        const RenderCanvasOverlayRegion *region =
            &cache->canvas_overlay_regions[i];
        size_t pixels = (size_t) region->width * region->height;
        const uint8_t *alpha = cache->canvas_overlay_alpha
            + region->pixel_offset;
        const uint16_t *color = cache->canvas_overlay_pixels
            + region->pixel_offset;
        for (size_t at = 0; at < pixels;) {
            size_t end = at + 1u;
            while (end < pixels && alpha[end] == alpha[at]
                   && color[end] == color[at]) end++;
            if (alpha[at] != 0 && alpha[at] != 255)
                canvas_overlay_blend_count(
                    tables, (uint32_t) color[at] << 8 | alpha[at],
                    (uint32_t) (end - at));
            at = end;
        }
    }
    /* Counted keys are a lower bound on their pixels: build tables only
       where that bound repays the build. */
    for (unsigned set = 0; set < CANVAS_OVERLAY_BLEND_SETS; set++) {
        for (unsigned way = 0; way < 2u; way++) {
            CanvasOverlayBlend *blend = &tables->blends[set][way];
            if (blend->key == UINT32_MAX
                || tables->counts[set][way]
                       < CANVAS_OVERLAY_BLEND_MINIMUM_PIXELS) {
                blend->key = UINT32_MAX;
                continue;
            }
            canvas_overlay_blend_build(blend, (uint16_t) (blend->key >> 8),
                                       blend->key & 0xffu);
        }
    }
    tables->planned = true;
}

/* One overlay row over the frame with planned tables. A dithered panel
   background changes (colour, alpha) every pixel and repeats every four, so
   the per-pair lookup is remembered per pixel lane (x mod 4), including a
   pair that has no table; runs are not worth finding there. */
__attribute__((noinline))
static void canvas_overlay_blit_row(const CanvasOverlayBlendCache *tables,
                                    uint16_t *out, const uint16_t *color_row,
                                    const uint8_t *alpha_row, int first_x,
                                    int end_x)
{
    uint32_t lane_keys[4] = {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
    const CanvasOverlayBlend *lane_tables[4] = {NULL, NULL, NULL, NULL};
    for (int x = first_x; x < end_x; x++) {
        unsigned alpha = alpha_row[x];
        if (alpha == 0) continue;
        uint16_t color = color_row[x];
        if (alpha == 255) {
            out[x] = color;
            continue;
        }
        uint32_t key = (uint32_t) color << 8 | alpha;
        unsigned lane = (unsigned) x & 3u;
        if (lane_keys[lane] != key) {
            const CanvasOverlayBlend *ways =
                tables->blends[canvas_overlay_blend_set(key)];
            lane_keys[lane] = key;
            lane_tables[lane] = ways[0].key == key ? &ways[0]
                : ways[1].key == key ? &ways[1] : NULL;
        }
        const CanvasOverlayBlend *blend = lane_tables[lane];
        uint16_t destination = out[x];
        out[x] = blend == NULL
            ? blend_rgb565(destination, canvas_overlay_rgb(color), alpha)
            : (uint16_t) (blend->red[tilefinch_rgb565_red_code(destination)]
                | blend->green[tilefinch_rgb565_green_code(destination)]
                | blend->blue[tilefinch_rgb565_blue_code(destination)]);
    }
}

static void canvas_overlay_blit(TileCache *cache, uint16_t *frame,
                                int viewport_width, int viewport_height)
{
    /* Two tables: a panel background and one other long translucent run
       per row do not evict each other. */
    CanvasOverlayBlend blends[2];
    blends[0].key = UINT32_MAX;
    blends[1].key = UINT32_MAX;
    unsigned victim = 0;
    CanvasOverlayBlendCache *tables = NULL;
    bool planned_tables = true;
#ifndef __PSP__
    planned_tables = !tilefinch_test_faults()->raster_span_reference;
#endif
    if (planned_tables && cache->canvas_overlay_blend_cache == NULL
        && cache->budget != NULL)
        cache->canvas_overlay_blend_cache = budget_malloc(
            cache->budget, sizeof(*cache->canvas_overlay_blend_cache));
    if (planned_tables && cache->canvas_overlay_blend_cache != NULL) {
        tables = cache->canvas_overlay_blend_cache;
        if (!tables->planned) canvas_overlay_blend_plan(cache, tables);
    }
    for (size_t i = 0; i < cache->canvas_overlay_region_count; i++) {
        const RenderCanvasOverlayRegion *region =
            &cache->canvas_overlay_regions[i];
        int first_x = region->left < 0 ? -region->left : 0;
        int end_x = region->width;
        if (region->left + end_x > viewport_width)
            end_x = viewport_width - region->left;
        for (size_t y = 0; y < region->height; y++) {
            int screen_y = region->top + (int) y;
            if (screen_y < 0 || screen_y >= viewport_height) continue;
            const uint8_t *alpha_row = cache->canvas_overlay_alpha
                + region->pixel_offset + y * region->width;
            const uint16_t *color_row = cache->canvas_overlay_pixels
                + region->pixel_offset + y * region->width;
            uint16_t *row = frame + (size_t) screen_y * viewport_width
                + (size_t) (region->left + first_x);
            if (tables != NULL) {
                canvas_overlay_blit_row(tables, row - first_x, color_row,
                                        alpha_row, first_x, end_x);
                continue;
            }
            int x = first_x;
            while (x < end_x) {
                unsigned alpha = alpha_row[x];
                uint16_t color = color_row[x];
                int run_end = x + 1;
                while (run_end < end_x && alpha_row[run_end] == alpha
                       && color_row[run_end] == color) run_end++;
                uint16_t *out = row + (x - first_x);
                int count = run_end - x;
                x = run_end;
                if (alpha == 0) continue;
                if (alpha == 255) {
                    for (int at = 0; at < count; at++) out[at] = color;
                    continue;
                }
                uint32_t key = (uint32_t) color << 8 | alpha;
                const CanvasOverlayBlend *blend = NULL;
                if (tables != NULL) {
                    const CanvasOverlayBlend *ways =
                        tables->blends[canvas_overlay_blend_set(key)];
                    blend = ways[0].key == key ? &ways[0]
                        : ways[1].key == key ? &ways[1] : NULL;
                }
                if (blend == NULL && count < CANVAS_OVERLAY_TABLE_RUN) {
                    uint32_t rgb = canvas_overlay_rgb(color);
                    for (int at = 0; at < count; at++)
                        out[at] = blend_rgb565(out[at], rgb, alpha);
                    continue;
                }
                if (blend == NULL) {
                    blend = blends[0].key == key ? &blends[0]
                        : blends[1].key == key ? &blends[1] : NULL;
                }
                if (blend == NULL) {
                    canvas_overlay_blend_build(&blends[victim], color, alpha);
                    blend = &blends[victim];
                    victim ^= 1u;
                }
                for (int at = 0; at < count; at++) {
                    uint16_t destination = out[at];
                    out[at] = (uint16_t) (
                        blend->red[tilefinch_rgb565_red_code(destination)]
                        | blend->green[tilefinch_rgb565_green_code(destination)]
                        | blend->blue[tilefinch_rgb565_blue_code(destination)]);
                }
            }
        }
    }
}

/* Admit a complete, aligned 2:3 canvas once instead of repeating its clip and
   row-kernel checks for every output row. Unsupported targets keep the general
   path. Source epochs, VFPU packing and framebuffer ownership are unchanged. */
static __attribute__((noinline)) bool canvas_setup_render(RasterTarget *target,
    const DrawCommand *command, const ImageResource *image,
    const unsigned char *source, size_t source_stride, TileCache *metrics)
{
    if (target->pixels == NULL || target->alpha != NULL
        || target->rounded_clip_count != 0 || image->width <= 0
        || image->width > 320 || image->width % 4 != 0
        || image->height <= 0 || image->height > 272
        || command->width != image->width / 2 * 3
        || command->height < image->height || command->height > 272
        || command->height - image->height > image->height
        || target->width <= 0 || target->height <= 0
        || target->stride < (size_t) target->width
        || target->stride > SIZE_MAX / sizeof(uint16_t)
        || (size_t) target->height > SIZE_MAX / target->stride
        || (size_t) target->height * target->stride > SIZE_MAX / sizeof(uint16_t))
        return false;
    int64_t local_x = (int64_t) command->x - target->origin_x;
    int64_t local_y = (int64_t) command->y - target->origin_y;
    if (command->x < target->left || command->y < target->top
        || (int64_t) command->x + command->width > target->right
        || (int64_t) command->y + command->height > target->bottom
        || local_x < 0 || local_y < 0
        || local_x + command->width > target->width
        || local_y + command->height > target->height
        || source == NULL || source_stride < (size_t) image->width * 4u
        || source_stride > SIZE_MAX / (size_t) image->height
        || (uintptr_t) source % 4u != 0 || source_stride % 4u != 0
        || target->stride % 2u != 0) return false;
    uint16_t *output = target->pixels
        + (size_t) local_y * target->stride + (size_t) local_x;
    if ((uintptr_t) output % 4u != 0) return false;
    uintptr_t a = (uintptr_t) source, b = (uintptr_t) target->pixels;
    size_t source_bytes = source_stride * (size_t) image->height;
    size_t target_bytes = target->stride * (size_t) target->height * 2u;
    if (a <= b ? b - a < source_bytes : a - b < target_bytes) return false;
#if defined(_MIPS_ARCH_ALLEGREX)
    /* A partially aligned padded stride can use VFPU on some rows. Retain
       the original per-row dispatch rather than demoting the whole canvas. */
    if (source_stride % 16u != 0) return false;
#endif
    metrics->canvas_setup_frames++;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    metrics->canvas_fast_conversion.setup_once = true;
#endif
#if defined(_MIPS_ARCH_ALLEGREX)
    bool vfpu = (uintptr_t) source % 16u == 0 && source_stride % 16u == 0
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        && canvas_validation_vfpu_conversion
#endif
        ;
#endif
    ScaleStepper source_y = scale_stepper(0, image->height, command->height);
    for (int y = 0; y < command->height;) {
        int row = source_y.value;
        scale_stepper_advance(&source_y);
        bool duplicate = y + 1 < command->height && source_y.value == row;
        uint16_t *destination = output + (size_t) y * target->stride;
        const unsigned char *input = source + (size_t) row * source_stride;
#if defined(_MIPS_ARCH_ALLEGREX)
        if (vfpu) {
            canvas_fast_scale_row_vfpu_3_to_2(destination, input, image->width);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
            metrics->canvas_fast_conversion.vfpu_rows++;
#endif
        } else
#endif
            canvas_fast_scale_row_3_to_2(destination, input, image->width);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        metrics->canvas_fast_conversion.kernel_rows++;
        metrics->canvas_fast_conversion.source_pixels += (uint32_t) image->width;
#endif
        y++;
        if (duplicate) {
            memcpy(destination + target->stride, destination,
                   (size_t) command->width * sizeof(*destination));
#ifdef TILEFINCH_PSP_VALIDATION_LOG
            metrics->canvas_fast_conversion.repeat_rows++;
#endif
            y++;
            scale_stepper_advance(&source_y);
        }
    }
    return true;
}

static void rasterize_opaque_canvas_fast(
    RasterTarget *target, const DrawCommand *command,
    const ImageResource *image, TileCache *metrics)
{
    const unsigned char *source_pixels = image->pixels;
    size_t source_stride = (size_t) image->width * 4u;
    const unsigned char *native_pixels = NULL;
    size_t native_stride = 0u;
    if (image_resource_native_canvas_source(
            image, &native_pixels, &native_stride)) {
        source_pixels = native_pixels;
        source_stride = native_stride;
    }
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    memset(&metrics->canvas_fast_conversion, 0,
           sizeof(metrics->canvas_fast_conversion));
    metrics->canvas_fast_conversion.source_address = (uintptr_t) source_pixels;
    metrics->canvas_fast_conversion.source_stride = source_stride;
    metrics->canvas_fast_conversion.source_width = image->width;
    metrics->canvas_fast_conversion.source_height = image->height;
    metrics->canvas_fast_conversion.output_width = command->width;
    metrics->canvas_fast_conversion.output_height = command->height;
    metrics->canvas_fast_conversion.native_source = native_pixels != NULL;
#endif
    if (
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        canvas_validation_setup_once &&
#endif
        canvas_setup_render(
            target, command, image, source_pixels, source_stride, metrics))
        return;
    int x0, y0, x1, y1;
    raster_command_bounds(target, command, &x0, &y0, &x1, &y1);
    ScaleStepper source_y = scale_stepper(
        y0 - command->y, image->height, command->height);
    int previous_source_y = -1;
    int previous_span_x0 = 0;
    int previous_span_x1 = 0;
    size_t previous_destination = 0;
    for (int y = y0; y < y1; y++) {
        int span_x0 = 0, span_x1 = 0;
        size_t destination = 0;
        bool rounded = false;
        if (!raster_target_span(target, y, x0, x1,
                                &span_x0, &span_x1,
                                &destination, &rounded)) {
#ifdef TILEFINCH_PSP_VALIDATION_LOG
            metrics->canvas_fast_conversion.skipped_rows++;
#endif
            scale_stepper_advance(&source_y);
            continue;
        }
        size_t span_pixels = (size_t) (span_x1 - span_x0);
        if (source_y.value == previous_source_y
            && span_x0 == previous_span_x0
            && span_x1 == previous_span_x1) {
#ifdef TILEFINCH_PSP_VALIDATION_LOG
            metrics->canvas_fast_conversion.repeat_rows++;
#endif
            memcpy(target->pixels + destination,
                   target->pixels + previous_destination,
                   span_pixels * sizeof(*target->pixels));
            previous_destination = destination;
            scale_stepper_advance(&source_y);
            continue;
        }
        ScaleStepper source_x = scale_stepper(
            span_x0 - command->x, image->width, command->width);
        const unsigned char *source_row = source_pixels
            + (size_t) source_y.value * source_stride;
        /* A 320-wide drawing buffer scaled across the PSP's 480 columns is
           the common game path.  Its exact nearest-neighbour mapping repeats
           each even source pixel twice and each odd source pixel once.  Use
           that fixed rational kernel instead of advancing and branching a
           ScaleStepper for every destination pixel.  Other ratios and clips
           retain the general pixel-identical path below. */
        if (span_x0 == command->x
            && span_x1 - span_x0 == command->width
            && image->width > 0
            && image->width % 2 == 0
            && image->width % 4 == 0
            && command->width > 0 && command->width % 3 == 0
            && command->width / 3 * 2 == image->width
            && ((uintptr_t) (const void *) source_row % 4u) == 0u
            && ((uintptr_t) (void *)
                    (target->pixels + destination) % 4u) == 0u) {
#if defined(_MIPS_ARCH_ALLEGREX)
            if (((uintptr_t) (const void *) source_row % 16u) == 0u
#ifdef TILEFINCH_PSP_VALIDATION_LOG
                && canvas_validation_vfpu_conversion
#endif
                ) {
                canvas_fast_scale_row_vfpu_3_to_2(
                    target->pixels + destination, source_row, image->width);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
                metrics->canvas_fast_conversion.vfpu_rows++;
#endif
            } else
#endif
            {
                canvas_fast_scale_row_3_to_2(
                    target->pixels + destination, source_row, image->width);
            }
#ifdef TILEFINCH_PSP_VALIDATION_LOG
            metrics->canvas_fast_conversion.kernel_rows++;
            metrics->canvas_fast_conversion.source_pixels += (uint32_t) image->width;
#endif
            previous_source_y = source_y.value;
            previous_span_x0 = span_x0;
            previous_span_x1 = span_x1;
            previous_destination = destination;
            scale_stepper_advance(&source_y);
            continue;
        }
        int previous_source_x = -1;
        uint16_t converted = 0;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        metrics->canvas_fast_conversion.general_rows++;
#endif
        for (int x = span_x0; x < span_x1; x++, destination++) {
            if (source_x.value != previous_source_x) {
#ifdef TILEFINCH_PSP_VALIDATION_LOG
                metrics->canvas_fast_conversion.source_pixels++;
#endif
                const unsigned char *pixel = source_row
                    + (size_t) source_x.value * 4u;
                converted = (uint16_t) (
                    ((uint16_t) (pixel[0] & 0xf8u) << 8)
                    | ((uint16_t) (pixel[1] & 0xfcu) << 3)
                    | ((uint16_t) pixel[2] >> 3));
                previous_source_x = source_x.value;
            }
            target->pixels[destination] = converted;
            scale_stepper_advance(&source_x);
        }
        previous_source_y = source_y.value;
        previous_span_x0 = span_x0;
        previous_span_x1 = span_x1;
        previous_destination = destination - span_pixels;
        scale_stepper_advance(&source_y);
    }
}

static bool canvas_validation_defer;

void tile_cache_set_canvas_defer(bool enabled)
{
    canvas_validation_defer = enabled;
}

bool tile_cache_canvas_defer_enabled(void)
{
    return canvas_validation_defer;
}

const RenderCanvasDeferral *tile_cache_canvas_deferral(
    const TileCache *cache)
{
    return cache != NULL && cache->canvas_deferred
        ? &cache->canvas_deferral : NULL;
}

static uint32_t canvas_defer_epoch(void)
{
#if defined(__PSP__)
    return psp_display_edram_content_epoch();
#else
    return 0u;
#endif
}

/* Only the exact shape the GE presenter reproduces pixel-for-pixel: one
   opaque native canvas filling the whole viewport at the rational 2:3 width
   (the kernel canvas_setup_render runs), with nothing composited above it in
   the RAM frame (no HTML overlay region, fixed/sticky content or scroll
   indicator, and the engine has reported no find/focus decoration). */
static bool canvas_defer_admit(
    TileCache *cache, const CanvasFastFrameCandidate *candidate,
    int scroll_y, int viewport_width, int viewport_height,
    bool overlay_ready)
{
    const DrawCommand *command = &candidate->command;
    const ImageResource *image = candidate->image;
    const unsigned char *source = NULL;
    size_t stride = 0;
    if (!overlay_ready || cache->canvas_overlay_region_count != 0
        || cache->layout->fixed_count != 0 || cache->layout->sticky_count != 0
        || (scroll_y > 0 && cache->layout->height > viewport_height)
        || image == NULL
        || !image_resource_native_canvas_source(image, &source, &stride)
        || command->x != 0 || command->y != scroll_y
        || command->width != viewport_width
        || command->height != viewport_height
        || candidate->clip_left > 0 || candidate->clip_top > scroll_y
        || candidate->clip_right < viewport_width
        || candidate->clip_bottom < scroll_y + viewport_height
        || image->width <= 0 || image->width > 320 || image->width % 4 != 0
        || command->width != image->width / 2 * 3
        || image->height <= 0 || command->height < image->height
        || command->height - image->height > image->height
        || stride % 16u != 0 || (uintptr_t) source % 16u != 0
        || cache->frame == NULL
        || cache->frame_pixels
               < (size_t) viewport_width * (size_t) viewport_height)
        return false;
    cache->canvas_deferral = (RenderCanvasDeferral) {
        .source = source, .source_stride = stride,
        .source_width = image->width, .source_height = image->height,
        .x = 0, .y = 0, .width = command->width, .height = command->height,
        .serial = image_canvas_native_serial(),
        .epoch = canvas_defer_epoch()
    };
    return true;
}

bool tile_cache_canvas_materialize(TileCache *cache)
{
    if (cache == NULL || !cache->canvas_deferred) return true;
    RenderCanvasDeferral deferral = cache->canvas_deferral;
    cache->canvas_deferred = false;
    ImageResource image;
    memset(&image, 0, sizeof(image));
    image.width = deferral.source_width;
    image.height = deferral.source_height;
    DrawCommand command;
    memset(&command, 0, sizeof(command));
    command.width = deferral.width;
    command.height = deferral.height;
    RasterTarget target = {
        .pixels = cache->frame,
        .stride = (size_t) deferral.width,
        .width = deferral.width,
        .height = deferral.height,
        .right = deferral.width,
        .bottom = deferral.height
    };
    if (deferral.serial != image_canvas_native_serial()
        || deferral.epoch != canvas_defer_epoch() || cache->frame == NULL
        || !canvas_setup_render(&target, &command, &image, deferral.source,
                                deferral.source_stride, cache)) {
        /* The surface moved on: the RAM frame cannot be recovered, so the
           next frame must be painted from scratch rather than reused. */
        cache->canvas_materialize_failures++;
        cache->last_frame_scroll_valid = false;
        cache->canvas_paint_pending = true;
        return false;
    }
    cache->canvas_materializations++;
    return true;
}

bool tile_cache_canvas_frame_fast_eligible(
    const TileCache *cache, int scroll_y,
    int viewport_width, int viewport_height)
{
    if (cache == NULL || cache->source_layout == NULL
        || cache->layout == NULL) return false;
    scroll_y = viewport_css_to_device(&cache->source_layout->viewport,
                                      scroll_y);
    int maximum_scroll = cache->layout->height - viewport_height;
    if (maximum_scroll < 0) maximum_scroll = 0;
    if (scroll_y > maximum_scroll) scroll_y = maximum_scroll;
    CanvasFastFrameCandidate candidate;
    return canvas_fast_frame_candidate(
        cache, scroll_y, viewport_width, viewport_height, &candidate);
}

#ifdef TILEFINCH_PSP_VALIDATION_LOG
#include "render/canvas_setup_validation.inc"
#endif

RenderCanvasFrameResult tile_cache_render_canvas_frame_fast(
    TileCache *cache, int scroll_y, int viewport_width, int viewport_height)
{
    if (cache == NULL || cache->source_layout == NULL) {
        return RENDER_CANVAS_FRAME_FAILED;
    }
    scroll_y = viewport_css_to_device(&cache->source_layout->viewport,
                                      scroll_y);
    int maximum_scroll = cache->layout->height - viewport_height;
    if (maximum_scroll < 0) maximum_scroll = 0;
    if (scroll_y > maximum_scroll) scroll_y = maximum_scroll;
    CanvasFastFrameCandidate candidate;
    bool canvas_pending = cache->canvas_paint_pending;
    if (!canvas_fast_frame_candidate(
            cache, scroll_y, viewport_width, viewport_height, &candidate)) {
        if (canvas_pending) cache->canvas_fast_refusals++;
        return RENDER_CANVAS_FRAME_NOT_APPLICABLE;
    }

    uint64_t started = render_now_us();
    RasterTarget target = {
        .pixels = cache->frame,
        .stride = (size_t) viewport_width,
        .origin_x = 0,
        .origin_y = scroll_y,
        .width = viewport_width,
        .height = viewport_height,
        .left = candidate.clip_left,
        .top = candidate.clip_top,
        .right = candidate.clip_right,
        .bottom = candidate.clip_bottom
    };
    bool deferred = false;
    bool overlay_cached = false;
    if (canvas_validation_defer && cache->canvas_defer_allowed) {
        overlay_cached = canvas_overlay_current(
            cache, scroll_y, viewport_width, viewport_height);
        if (!overlay_cached) {
            overlay_cached = canvas_overlay_build(
                cache, &candidate, scroll_y, viewport_width,
                viewport_height);
        }
        deferred = canvas_defer_admit(
            cache, &candidate, scroll_y, viewport_width, viewport_height,
            overlay_cached);
    }
    cache->canvas_deferred = deferred;
    if (deferred) cache->canvas_deferrals++;
    else
    rasterize_opaque_canvas_fast(
        &target, &candidate.command, candidate.image, cache);
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
    uint64_t rastered = render_now_us();
#endif
    if (!overlay_cached) {
        overlay_cached = canvas_overlay_current(
            cache, scroll_y, viewport_width, viewport_height);
    }
    if (!overlay_cached) {
        overlay_cached = canvas_overlay_build(
            cache, &candidate, scroll_y, viewport_width, viewport_height);
    }
    if (overlay_cached) {
        canvas_overlay_blit(cache, cache->frame,
                            viewport_width, viewport_height);
    } else {
        target.left = 0;
        target.top = scroll_y;
        target.right = viewport_width;
        target.bottom = scroll_y + viewport_height;
        for (size_t order = candidate.paint_order + 1u;
             order < cache->layout->count; order++) {
            size_t index = cache->layout->paint_order[order];
            uint8_t flags = cache->layout->command_flags == NULL
                ? 0 : cache->layout->command_flags[index];
            if ((flags & LAYOUT_COMMAND_UNTILED) != 0) continue;
            if ((flags & LAYOUT_COMMAND_OVERFLOW) != 0
                || (cache->layout->command_flags == NULL
                    && command_in_overflow(cache->layout, index))) {
                paint_overflow_command(cache, cache->frame, index, scroll_y,
                                       viewport_width, viewport_height, NULL);
                continue;
            }
            if ((flags & LAYOUT_COMMAND_LATE_POSITIONED) != 0) {
                paint_overlay_command(
                    cache, cache->frame, &cache->layout->commands[index],
                    scroll_y, viewport_width, viewport_height);
                continue;
            }
            if (flags != 0) continue;
            cache->command_candidates++;
            rasterize_command(
                cache, &target, &cache->layout->commands[index]);
        }
    }
    paint_sticky_overlays(cache, cache->frame, scroll_y,
                          viewport_width, viewport_height,
                          candidate.paint_order + 1u);
    if (cache->layout->fixed_count != 0) {
        if (build_fixed_cache(cache, viewport_width, viewport_height)) {
            bool bounded_first = false;
            (void) fixed_cache_composition_order(
                cache->layout, &bounded_first);
            if (bounded_first) {
                paint_fixed_overlays(cache, cache->frame, scroll_y,
                                     viewport_width, viewport_height, true);
            }
            paint_cached_fixed_overlays(cache, cache->frame,
                                        viewport_width, viewport_height);
            if (!bounded_first) {
                paint_fixed_overlays(cache, cache->frame, scroll_y,
                                     viewport_width, viewport_height, true);
            }
        } else {
            paint_fixed_overlays(cache, cache->frame, scroll_y,
                                 viewport_width, viewport_height, false);
        }
    }
    paint_scroll_indicator(cache, cache->frame, scroll_y,
                           viewport_width, viewport_height);
    cache->last_frame_scroll_y = scroll_y;
    cache->last_frame_viewport_width = viewport_width;
    cache->last_frame_viewport_height = viewport_height;
    cache->last_frame_scroll_valid = true;
    cache->presented_scroll_y = scroll_y;
    cache->presented_scroll_valid = true;
    cache->canvas_paint_pending = false;
    memset(&cache->frame_work, 0, sizeof(cache->frame_work));
    uint64_t elapsed = render_now_us() - started;
    cache->canvas_fast_frames++;
    cache->canvas_fast_us += elapsed;
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
    cache->canvas_fast_raster_us += rastered - started;
    cache->canvas_fast_overlay_us += elapsed - (rastered - started);
#endif
    if (elapsed > cache->canvas_fast_max_us) {
        cache->canvas_fast_max_us = elapsed;
    }
    cache->frame_us += elapsed;
    if (elapsed > cache->max_frame_us) cache->max_frame_us = elapsed;
    cache->frames_rendered++;
    TILEFINCH_WORK_ADD(frames, 1);
    return RENDER_CANVAS_FRAME_COMPLETE;
}
