/*
 * Coverage for the shared video-presentation geometry.
 *
 * None of this can be observed off-device: the graphics engine exists only on
 * a PSP, and a decoded frame exists only when firmware produced one. What CAN
 * be proved on a host is every number the GE is handed -- the destination
 * rectangle, the letterbox bands, the split point for a frame wider than one
 * texture, and above all where each bilinear tap lands.
 *
 * That last one is the reason this file exists. The decoded surface is
 * macroblock-padded: columns 426..431 of a 426-wide frame hold decoder
 * padding, and the rows past the coded height were never written at all. A
 * filter tap half a texel past the picture would put uninitialised memory on
 * the panel, on hardware, in a path no golden covers. So every quad the
 * planner emits is checked tap by tap against the real picture bounds.
 */

#include "tilefinch/psp_media_present.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return false; \
    } \
} while (0)

#define SCREEN_WIDTH PSP_MEDIA_PRESENT_SCREEN_WIDTH
#define SCREEN_HEIGHT PSP_MEDIA_PRESENT_SCREEN_HEIGHT

static bool rect_equals(
    PspMediaPresentRect rect, int x, int y, int width, int height)
{
    return rect.x == x && rect.y == y
        && rect.width == width && rect.height == height;
}

/* Every band the plan reports, plus the video rectangle, must tile the panel
   exactly once: no gap that keeps a stale pixel, no overlap that would let a
   band erase the picture. */
static bool bands_tile_the_panel(const PspMediaPresentPlan *plan)
{
    static unsigned char covered[SCREEN_HEIGHT][SCREEN_WIDTH];
    memset(covered, 0, sizeof(covered));
    PspMediaPresentRect rects[PSP_MEDIA_PRESENT_MAX_BANDS + 1];
    size_t count = 0;
    rects[count++] = plan->video;
    for (size_t at = 0; at < plan->band_count; at++)
        rects[count++] = plan->bands[at];
    for (size_t at = 0; at < count; at++) {
        PspMediaPresentRect rect = rects[at];
        if (rect.width <= 0 || rect.height <= 0) return false;
        if (rect.x < 0 || rect.y < 0) return false;
        if (rect.x + rect.width > SCREEN_WIDTH) return false;
        if (rect.y + rect.height > SCREEN_HEIGHT) return false;
        for (int y = rect.y; y < rect.y + rect.height; y++) {
            for (int x = rect.x; x < rect.x + rect.width; x++) {
                if (covered[y][x] != 0) return false;
                covered[y][x] = 1;
            }
        }
    }
    for (int y = 0; y < SCREEN_HEIGHT; y++) {
        for (int x = 0; x < SCREEN_WIDTH; x++) {
            if (covered[y][x] == 0) return false;
        }
    }
    return true;
}

/* The quads must together cover the video rectangle's columns exactly once,
   with no destination column drawn twice and none left undrawn. */
static bool quads_tile_the_video_rect(const PspMediaPresentPlan *plan)
{
    float left = (float) plan->video.x;
    float right = (float) (plan->video.x + plan->video.width);
    float top = (float) plan->video.y;
    float bottom = (float) (plan->video.y + plan->video.height);
    if (plan->quad_count == 0) return false;
    if (plan->quads[0].x0 != left) return false;
    if (plan->quads[plan->quad_count - 1].x1 != right) return false;
    for (size_t at = 0; at < plan->quad_count; at++) {
        if (plan->quads[at].y0 != top || plan->quads[at].y1 != bottom)
            return false;
        if (plan->quads[at].x1 <= plan->quads[at].x0) return false;
        if (at > 0 && plan->quads[at].x0 != plan->quads[at - 1].x1)
            return false;
    }
    return true;
}

static bool quads_sample_inside(
    const PspMediaPresentPlan *plan, int source_width, int source_height)
{
    for (size_t at = 0; at < plan->quad_count; at++) {
        if (!psp_media_present_quad_samples_inside(
                &plan->quads[at], source_width, source_height,
                plan->quads[at].texture_column)) {
            fprintf(stderr, "  quad %zu samples outside the picture\n", at);
            return false;
        }
        if (plan->quads[at].texture_width > PSP_MEDIA_PRESENT_TEXTURE_MAX
            || plan->quads[at].texture_height
                > PSP_MEDIA_PRESENT_TEXTURE_MAX) {
            fprintf(stderr, "  quad %zu exceeds the texture limit\n", at);
            return false;
        }
        int width = plan->quads[at].texture_width;
        int height = plan->quads[at].texture_height;
        if ((width & (width - 1)) != 0 || (height & (height - 1)) != 0) {
            fprintf(stderr, "  quad %zu texture is not a power of two\n", at);
            return false;
        }
        if (plan->quads[at].texture_column % PSP_MEDIA_PRESENT_COLUMN_ALIGN
            != 0) {
            fprintf(stderr, "  quad %zu texture base is misaligned\n", at);
            return false;
        }
    }
    return true;
}

/* The device's own stream: a 426x240 display rectangle inside a 432-wide
   macroblock-padded surface at the 512-pixel stride. */
static bool test_device_240p_geometry(void)
{
    PspMediaPresentPlan plan;
    CHECK(psp_media_present_plan(
        &plan, 426, 240, 512, SCREEN_WIDTH, SCREEN_HEIGHT));
    CHECK(rect_equals(plan.video, 0, 1, 480, 270));
    CHECK(plan.quad_count == 1);
    CHECK(plan.quads[0].texture_column == 0);
    CHECK(plan.quads[0].texture_width == 512);
    CHECK(plan.quads[0].texture_height == 256);
    CHECK(plan.quads[0].u0 == 0.0f && plan.quads[0].v0 == 0.0f);
    /* Upscaled on both axes, so both far edges are pulled in. */
    CHECK(plan.quads[0].u1 < 426.0f && plan.quads[0].u1 > 425.0f);
    CHECK(plan.quads[0].v1 < 240.0f && plan.quads[0].v1 > 239.0f);
    CHECK(plan.quads[0].x0 == 0.0f && plan.quads[0].x1 == 480.0f);
    CHECK(plan.quads[0].y0 == 1.0f && plan.quads[0].y1 == 271.0f);
    /* One row above and one below: 272 minus 270. */
    CHECK(plan.band_count == 2);
    CHECK(rect_equals(plan.bands[0], 0, 0, 480, 1));
    CHECK(rect_equals(plan.bands[1], 0, 271, 480, 1));
    CHECK(bands_tile_the_panel(&plan));
    CHECK(quads_tile_the_video_rect(&plan));
    CHECK(quads_sample_inside(&plan, 426, 240));
    return true;
}

/* 640x360 at the 768-pixel stride: wider than one texture, so it splits. */
static bool test_360p_splits_into_two_quads(void)
{
    PspMediaPresentPlan plan;
    CHECK(psp_media_present_plan(
        &plan, 640, 360, 768, SCREEN_WIDTH, SCREEN_HEIGHT));
    CHECK(rect_equals(plan.video, 0, 1, 480, 270));
    CHECK(plan.quad_count == 2);
    CHECK(plan.quads[0].texture_column == 0);
    CHECK(plan.quads[0].texture_width == 512);
    CHECK(plan.quads[1].texture_column == 512);
    CHECK(plan.quads[1].texture_width == 128);
    /* 640 into 480 is an exact three-quarters, so the seam lands on a whole
       destination column and the second texture starts on texel 512. */
    CHECK(plan.quads[0].x1 == 384.0f);
    CHECK(plan.quads[1].x0 == 384.0f);
    CHECK(plan.quads[0].u1 == 512.0f);
    CHECK(plan.quads[1].u0 == 0.0f);
    CHECK(plan.quads[1].u1 == 128.0f);
    /* Downscaled horizontally: no far-edge guard is needed, and the two
       quads' mappings are one continuous line. */
    CHECK(plan.quads[0].u0 == 0.0f);
    CHECK(bands_tile_the_panel(&plan));
    CHECK(quads_tile_the_video_rect(&plan));
    CHECK(quads_sample_inside(&plan, 640, 360));
    return true;
}

/* The split must stay correct when the seam does NOT fall on a whole column,
   which is every geometry that is not exactly four-to-three. */
static bool test_split_handles_a_fractional_seam(void)
{
    PspMediaPresentPlan plan;
    /* 640x480 fits to the panel height, so the destination is 362 wide. */
    CHECK(psp_media_present_plan(
        &plan, 640, 480, 768, SCREEN_WIDTH, SCREEN_HEIGHT));
    CHECK(plan.video.width == 362 && plan.video.height == 272);
    CHECK(plan.quad_count == 2);
    CHECK(plan.quads[1].texture_column % PSP_MEDIA_PRESENT_COLUMN_ALIGN == 0);
    CHECK(plan.quads[1].texture_column >= 640 - 512);
    CHECK(plan.quads[0].u1 > 0.0f);
    /* The seam carries no phase break: the second quad's mapping is the first
       quad's line continued, rebased onto its own texture. */
    float scale_a = (plan.quads[0].u1 - plan.quads[0].u0)
        / (plan.quads[0].x1 - plan.quads[0].x0);
    float scale_b = (plan.quads[1].u1 - plan.quads[1].u0)
        / (plan.quads[1].x1 - plan.quads[1].x0);
    float difference = scale_a > scale_b
        ? scale_a - scale_b : scale_b - scale_a;
    CHECK(difference < 0.0005f);
    float seam_a = plan.quads[0].u1;
    float seam_b = plan.quads[1].u0 + (float) plan.quads[1].texture_column;
    float seam_difference = seam_a > seam_b
        ? seam_a - seam_b : seam_b - seam_a;
    CHECK(seam_difference < 0.0005f);
    CHECK(bands_tile_the_panel(&plan));
    CHECK(quads_tile_the_video_rect(&plan));
    CHECK(quads_sample_inside(&plan, 640, 480));
    return true;
}

/* Pillarboxing: a tall picture leaves bands on the left and right. */
static bool test_pillarbox_bands(void)
{
    PspMediaPresentPlan plan;
    CHECK(psp_media_present_plan(
        &plan, 240, 240, 256, SCREEN_WIDTH, SCREEN_HEIGHT));
    CHECK(rect_equals(plan.video, 104, 0, 272, 272));
    CHECK(plan.band_count == 2);
    CHECK(rect_equals(plan.bands[0], 0, 0, 104, 272));
    CHECK(rect_equals(plan.bands[1], 376, 0, 104, 272));
    CHECK(bands_tile_the_panel(&plan));
    CHECK(quads_tile_the_video_rect(&plan));
    CHECK(quads_sample_inside(&plan, 240, 240));
    return true;
}

/*
 * A 1:1 picture needs no bands at all. It still needs the far-edge guard:
 * mapped plainly, the last destination pixel's second bilinear tap lands
 * exactly on the texel past the picture. The weight on that tap is zero, but
 * the address is still formed, and the texel it names is padding the decoder
 * wrote nothing into.
 */
static bool test_exact_fit_has_no_bands(void)
{
    PspMediaPresentPlan plan;
    CHECK(psp_media_present_plan(
        &plan, 480, 272, 512, SCREEN_WIDTH, SCREEN_HEIGHT));
    CHECK(rect_equals(plan.video, 0, 0, 480, 272));
    CHECK(plan.band_count == 0);
    CHECK(plan.quad_count == 1);
    CHECK(plan.quads[0].u1 < 480.0f && plan.quads[0].u1 > 479.9f);
    CHECK(plan.quads[0].v1 < 272.0f && plan.quads[0].v1 > 271.9f);
    CHECK(plan.quads[0].texture_width == 512);
    CHECK(plan.quads[0].texture_height == 512);
    CHECK(bands_tile_the_panel(&plan));
    CHECK(quads_sample_inside(&plan, 480, 272));
    return true;
}

/*
 * The property the whole file is for. Sweep every stream geometry the
 * decoder policy admits and prove no bilinear tap ever reaches padding.
 */
static bool test_no_geometry_samples_padding(void)
{
    int checked = 0;
    for (int width = 16; width <= 640; width += 2) {
        for (int height = 16; height <= 480; height += 2) {
            int stride = width <= 426 ? 512 : 768;
            if (width > stride) continue;
            PspMediaPresentPlan plan;
            if (!psp_media_present_plan(
                    &plan, width, height, stride,
                    SCREEN_WIDTH, SCREEN_HEIGHT)) {
                fprintf(stderr, "  refused %dx%d\n", width, height);
                return false;
            }
            if (!bands_tile_the_panel(&plan)) {
                fprintf(stderr, "  bands do not tile at %dx%d\n",
                        width, height);
                return false;
            }
            /* No quads means the graphics engine sits this geometry out and
               the software scaler presents the same rectangle. */
            if (plan.quad_count == 0) continue;
            if (!quads_sample_inside(&plan, width, height)) {
                fprintf(stderr, "  at %dx%d\n", width, height);
                return false;
            }
            if (!quads_tile_the_video_rect(&plan)) {
                fprintf(stderr, "  quads do not tile at %dx%d\n",
                        width, height);
                return false;
            }
            checked++;
        }
    }
    CHECK(checked > 10000);
    printf("  swept %d admitted geometries\n", checked);
    return true;
}

static bool test_refuses_impossible_geometry(void)
{
    PspMediaPresentPlan plan;
    CHECK(!psp_media_present_plan(&plan, 0, 240, 512, 480, 272));
    CHECK(!psp_media_present_plan(&plan, 426, 0, 512, 480, 272));
    CHECK(!psp_media_present_plan(&plan, -8, 240, 512, 480, 272));
    /* A stride narrower than the picture would sample the next row. */
    CHECK(!psp_media_present_plan(&plan, 426, 240, 400, 480, 272));
    CHECK(!psp_media_present_plan(NULL, 426, 240, 512, 480, 272));
    CHECK(!psp_media_present_plan(&plan, 426, 240, 512, 0, 272));
    return true;
}

/*
 * Geometry the graphics engine cannot express is not geometry that cannot be
 * presented. The rectangle and the bands still come back and still tile the
 * panel; only the quads are withheld, which routes the frame to the software
 * scaler with the identical destination.
 */
static bool test_ungeable_geometry_still_plans_the_rect(void)
{
    PspMediaPresentPlan plan;
    /* Taller than one texture: the vertical axis has no split. */
    CHECK(psp_media_present_plan(&plan, 426, 544, 512, 480, 272));
    CHECK(plan.quad_count == 0);
    CHECK(plan.video.width > 0 && plan.video.height > 0);
    CHECK(bands_tile_the_panel(&plan));
    /* Wider than two textures can span. */
    CHECK(psp_media_present_plan(&plan, 1200, 240, 1280, 480, 272));
    CHECK(plan.quad_count == 0);
    CHECK(bands_tile_the_panel(&plan));
    return true;
}

/*
 * The identity skip. A present may be dropped only when this buffer already
 * holds this picture at this rectangle and nothing will draw over it.
 */
static bool test_identity_skip_semantics(void)
{
    PspMediaPresentRecord records[2];
    psp_media_present_records_reset(records, 2);
    PspMediaPresentRect video = {0, 1, 480, 270};
    PspMediaPresentRect moved = {0, 0, 480, 272};

    /* Nothing has been presented yet. */
    CHECK(!psp_media_present_skip_allowed(&records[0], 7, 1, &video, false));

    psp_media_present_record(&records[0], 7, 1, &video);
    CHECK(psp_media_present_skip_allowed(&records[0], 7, 1, &video, false));
    /* The other buffer is a different buffer. Double buffering means a frame
       presented once has been presented into exactly one of the two. */
    CHECK(!psp_media_present_skip_allowed(&records[1], 7, 1, &video, false));

    /* A newly decoded picture. */
    CHECK(!psp_media_present_skip_allowed(&records[0], 8, 1, &video, false));
    /* A different stream that happens to restart its picture count. */
    CHECK(!psp_media_present_skip_allowed(&records[0], 7, 2, &video, false));
    /* A geometry change moves the rectangle and the bands with it. */
    CHECK(!psp_media_present_skip_allowed(&records[0], 7, 1, &moved, false));
    /* The chrome overlay blends at opacity 3, so compositing twice over one
       presented frame darkens it. Never skip while it paints. */
    CHECK(!psp_media_present_skip_allowed(&records[0], 7, 1, &video, true));

    /* Anything that is not this presenter writing the buffer must clear
       every record, not just the one it wrote. */
    psp_media_present_record(&records[1], 7, 1, &video);
    psp_media_present_records_reset(records, 2);
    CHECK(!psp_media_present_skip_allowed(&records[0], 7, 1, &video, false));
    CHECK(!psp_media_present_skip_allowed(&records[1], 7, 1, &video, false));

    CHECK(!psp_media_present_skip_allowed(NULL, 7, 1, &video, false));
    CHECK(!psp_media_present_skip_allowed(&records[0], 7, 1, NULL, false));
    psp_media_present_records_reset(NULL, 2);
    psp_media_present_record(NULL, 7, 1, &video);
    return true;
}

static bool test_mode_names(void)
{
    CHECK(strcmp(psp_media_present_mode_name(
        PSP_MEDIA_PRESENT_MODE_GE_SMOOTH), "ge-smooth") == 0);
    CHECK(strcmp(psp_media_present_mode_name(
        PSP_MEDIA_PRESENT_MODE_SOFTWARE_SHARP), "software-sharp") == 0);
    CHECK(strcmp(psp_media_present_mode_name(
        PSP_MEDIA_PRESENT_MODE_SOFTWARE_FALLBACK),
        "software-fallback") == 0);
    return true;
}

/* The host build must link the same call the device takes and answer that
   there is no graphics engine, so a host caller can never believe one drew. */
static bool test_host_has_no_graphics_engine(void)
{
    PspMediaPresentPlan plan;
    CHECK(psp_media_present_plan(
        &plan, 426, 240, 512, SCREEN_WIDTH, SCREEN_HEIGHT));
    static unsigned char source[512 * 272 * 4];
    static uint32_t destination[512 * 272];
    PspMediaPresentTexture texture = {
        .pixels = source, .stride_pixels = 512, .staged = false
    };
    PspMediaPresentGeCost cost = {1, 1};
    CHECK(!psp_media_present_ge_draw(&plan, &texture, destination, &cost));
    CHECK(cost.submit_us == 0 && cost.sync_us == 0);
    CHECK(psp_media_present_ge_reason() != NULL);
    /* The two halves the presenter now splits the draw into, so the decoder
       can be fed through the wait. A host has no engine, so a submit that
       answers false must be followed by a complete that also does -- a caller
       that believed the pair had succeeded would publish rows nothing wrote. */
    PspMediaPresentGeCost split = {1, 1};
    CHECK(!psp_media_present_ge_submit(
        &plan, &texture, destination, &split));
    CHECK(split.submit_us == 0 && split.sync_us == 0);
    CHECK(!psp_media_present_ge_complete(&split));
    /* And the check the caller runs before it commits the panel to the video
       surface: a host must never be told the passthrough holds. */
    CHECK(!psp_media_present_ge_passthrough_check(destination));
    uint32_t drawn = 1;
    uint32_t drawn_source = 1;
    psp_media_present_ge_passthrough(&drawn, &drawn_source);
    CHECK(drawn == 0 && drawn_source == 0);
    return true;
}

/*
 * The staged layout, against its definition rather than against itself.
 *
 * The device spent 47.4ms of wait per frame reading a linear 2 KiB-pitch
 * texture out of main memory; the staged layout is what the texture unit is
 * built for. Getting it subtly wrong would not fail to draw -- it would draw
 * a scrambled picture -- so the test computes each texel's address from the
 * layout's own formula and requires the stager to have put it there.
 */
static bool test_stage_is_a_linear_copy_into_the_texture(void)
{
    /* The stage is now a plain sequential copy into a contiguous EDRAM
       texture, not a block reorder: a device probe proved the engine draws a
       linear EDRAM texture at the same cost as a swizzled one, so the reorder
       was pure per-picture overhead. Row y column x of the source lands at
       row y column x of the texture, at the texture's own width. */
    enum { STRIDE = 512, WIDTH = 512, ROWS = 240 };
    static uint32_t source[STRIDE * ROWS];
    static uint32_t staged[WIDTH * ROWS];
    for (int y = 0; y < ROWS; y++)
        for (int x = 0; x < STRIDE; x++)
            source[(size_t) y * STRIDE + x] =
                (uint32_t) ((y << 16) | x) ^ UINT32_C(0xa5a50000);
    memset(staged, 0, sizeof(staged));
    psp_media_present_stage(staged, source, STRIDE, WIDTH, ROWS);
    for (int y = 0; y < ROWS; y++)
        for (int x = 0; x < WIDTH; x++)
            CHECK(staged[(size_t) y * WIDTH + x]
                  == source[(size_t) y * STRIDE + x]);

    /* A narrower texture than its source stride copies only the texture's
       columns per row, leaving the source's tail untouched -- the 360p seam's
       geometry, though 360p is drawn from the source rather than staged. */
    enum { NARROW = 256 };
    static uint32_t narrow[NARROW * ROWS];
    memset(narrow, 0xff, sizeof(narrow));
    psp_media_present_stage(narrow, source, STRIDE, NARROW, ROWS);
    for (int y = 0; y < ROWS; y++)
        for (int x = 0; x < NARROW; x++)
            CHECK(narrow[(size_t) y * NARROW + x]
                  == source[(size_t) y * STRIDE + x]);

    CHECK(psp_media_present_stage_bytes(WIDTH, ROWS)
          == (size_t) WIDTH * ROWS * 4u);

    /* Bad geometry is refused rather than half-copied, leaving the guard the
       zeros it started with. */
    static uint32_t guard[64];
    memset(guard, 0, sizeof(guard));
    psp_media_present_stage(guard, source, 8, 512, 8);   /* wider than source */
    psp_media_present_stage(NULL, source, STRIDE, WIDTH, ROWS);
    psp_media_present_stage(guard, NULL, STRIDE, WIDTH, ROWS);
    for (size_t at = 0; at < sizeof(guard) / sizeof(guard[0]); at++)
        CHECK(guard[at] == 0);

    /* Rows to copy is the picture height itself -- a linear copy has no
       block-alignment to round up to. */
    CHECK(psp_media_present_stage_rows(240) == 240
          && psp_media_present_stage_rows(241) == 241
          && psp_media_present_stage_rows(1) == 1
          && psp_media_present_stage_rows(0) == 0);
    return true;
}

/* The shipping geometry stages; the split wide-frame plan does not, because a
   second texture starting part way along a row is not something a staged
   layout can express. */
static bool test_stage_admission_follows_the_plan(void)
{
    PspMediaPresentPlan narrow;
    CHECK(psp_media_present_plan(
        &narrow, 426, 240, 512, SCREEN_WIDTH, SCREEN_HEIGHT));
    CHECK(narrow.quad_count == 1);
    CHECK(psp_media_present_stage_fits(
        &narrow, 240, PSP_MEDIA_PRESENT_STAGE_BYTES));
    /* 512 columns by 240 rows of 32-bit texels is what it costs. */
    CHECK(psp_media_present_stage_bytes(
              narrow.quads[0].texture_width,
              psp_media_present_stage_rows(240))
          == (size_t) 512 * 240 * 4);
    CHECK(!psp_media_present_stage_fits(&narrow, 240, 1024));

    PspMediaPresentPlan wide;
    CHECK(psp_media_present_plan(
        &wide, 640, 360, 768, SCREEN_WIDTH, SCREEN_HEIGHT));
    CHECK(wide.quad_count == 2);
    CHECK(!psp_media_present_stage_fits(
        &wide, 360, PSP_MEDIA_PRESENT_STAGE_BYTES));
    CHECK(!psp_media_present_stage_fits(NULL, 240, 1u << 20));
    return true;
}

static bool test_360p_wide_strip_plan(void)
{
    PspMediaPresentPlan full;
    CHECK(psp_media_present_plan(
        &full, 640, 360, 768, SCREEN_WIDTH, SCREEN_HEIGHT));
    PspMediaPresentStripPlan strips;
    CHECK(psp_media_present_wide_strip_plan(
        &strips, &full, 640, 360, 768,
        PSP_MEDIA_PRESENT_STAGE_BYTES));
    CHECK(strips.strip_count == 2);
    CHECK((size_t) 768 * 181 * 4 == 556032u);
    CHECK((size_t) 768 * 181 * 4 <= PSP_MEDIA_PRESENT_STAGE_BYTES);

    CHECK(strips.strips[0].source_row == 0);
    CHECK(strips.strips[1].source_row == 179);
    CHECK(strips.strips[0].copy_rows == 181
          && strips.strips[1].copy_rows == 181);
    CHECK(strips.strips[0].draw.video.y == full.video.y);
    CHECK(strips.strips[0].draw.video.height == 135);
    CHECK(strips.strips[1].draw.video.y == full.video.y + 135);
    CHECK(strips.strips[1].draw.video.y
              + strips.strips[1].draw.video.height
          == full.video.y + full.video.height);

    for (size_t strip_at = 0; strip_at < strips.strip_count; strip_at++) {
        const PspMediaPresentStrip *strip = &strips.strips[strip_at];
        CHECK(strip->draw.quad_count == 2);
        for (size_t quad_at = 0; quad_at < strip->draw.quad_count;
             quad_at++) {
            const PspMediaPresentQuad *quad = &strip->draw.quads[quad_at];
            CHECK(quad->texture_height == 256);
            CHECK(psp_media_present_quad_samples_inside(
                quad, 640, strip->copy_rows, quad->texture_column));
        }
    }

    /* A non-widescreen 360p rendition uses the same padded decoder surface.
       Its 408x272 output still divides exactly into the two proven source
       strips and must not fall back to the much slower direct GE texture. */
    PspMediaPresentPlan narrow_360;
    CHECK(psp_media_present_plan(
        &narrow_360, 540, 360, 768, SCREEN_WIDTH, SCREEN_HEIGHT));
    CHECK(narrow_360.video.width == 408
          && narrow_360.video.height == 272
          && narrow_360.quad_count == 2);
    CHECK(psp_media_present_wide_strip_plan(
        &strips, &narrow_360, 540, 360, 768,
        PSP_MEDIA_PRESENT_STAGE_BYTES));
    CHECK(strips.strips[0].draw.video.height == 136
          && strips.strips[1].draw.video.height == 136);
    for (size_t strip_at = 0; strip_at < strips.strip_count; strip_at++) {
        const PspMediaPresentStrip *strip = &strips.strips[strip_at];
        for (size_t quad_at = 0; quad_at < strip->draw.quad_count;
             quad_at++) {
            const PspMediaPresentQuad *quad = &strip->draw.quads[quad_at];
            CHECK(psp_media_present_quad_samples_inside(
                quad, 540, strip->copy_rows, quad->texture_column));
        }
    }

    /* Nominal 360p is not always authored at exactly 640 visible pixels.
       The common 638-pixel crop must retain this same staged path. */
    PspMediaPresentPlan cropped;
    CHECK(psp_media_present_plan(
        &cropped, 638, 360, 768, SCREEN_WIDTH, SCREEN_HEIGHT));
    CHECK(cropped.video.width == 480 && cropped.video.height == 270);
    CHECK(psp_media_present_wide_strip_plan(
        &strips, &cropped, 638, 360, 768,
        PSP_MEDIA_PRESENT_STAGE_BYTES));
    for (size_t strip_at = 0; strip_at < strips.strip_count; strip_at++) {
        const PspMediaPresentStrip *strip = &strips.strips[strip_at];
        for (size_t quad_at = 0; quad_at < strip->draw.quad_count;
             quad_at++) {
            const PspMediaPresentQuad *quad = &strip->draw.quads[quad_at];
            CHECK(psp_media_present_quad_samples_inside(
                quad, 638, strip->copy_rows, quad->texture_column));
        }
    }

    /* One byte less than the exact padded copy is a clean refusal. */
    CHECK(!psp_media_present_wide_strip_plan(
        &strips, &full, 640, 360, 768, 556031u));
    /* Most importantly, the proven 240p layout can never enter this path. */
    PspMediaPresentPlan narrow;
    CHECK(psp_media_present_plan(
        &narrow, 426, 240, 512, SCREEN_WIDTH, SCREEN_HEIGHT));
    CHECK(!psp_media_present_wide_strip_plan(
        &strips, &narrow, 426, 240, 512,
        PSP_MEDIA_PRESENT_STAGE_BYTES));
    return true;
}

int main(void)
{
    struct { const char *name; bool (*run)(void); } cases[] = {
        {"stage-is-a-linear-copy-into-the-texture",
         test_stage_is_a_linear_copy_into_the_texture},
        {"stage-admission-follows-the-plan",
         test_stage_admission_follows_the_plan},
        {"360p-wide-strip-plan", test_360p_wide_strip_plan},
        {"device-240p-geometry", test_device_240p_geometry},
        {"360p-splits-into-two-quads", test_360p_splits_into_two_quads},
        {"split-handles-a-fractional-seam",
         test_split_handles_a_fractional_seam},
        {"pillarbox-bands", test_pillarbox_bands},
        {"exact-fit-has-no-bands", test_exact_fit_has_no_bands},
        {"no-geometry-samples-padding", test_no_geometry_samples_padding},
        {"refuses-impossible-geometry", test_refuses_impossible_geometry},
        {"ungeable-geometry-still-plans-the-rect",
         test_ungeable_geometry_still_plans_the_rect},
        {"identity-skip-semantics", test_identity_skip_semantics},
        {"mode-names", test_mode_names},
        {"host-has-no-graphics-engine", test_host_has_no_graphics_engine}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (!cases[i].run()) {
            fprintf(stderr, "FAIL %s\n", cases[i].name);
            return EXIT_FAILURE;
        }
        printf("ok %s\n", cases[i].name);
    }
    return EXIT_SUCCESS;
}
