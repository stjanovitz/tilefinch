/* A picture too large for the paint caches is decoded once per paint pass,
   not once per tile it covers; the pass holds that decode, and memory
   pressure can still take it. Included by test_image_decode_scaled.c. */

enum { HELD_FRAME_WIDTH = 480, HELD_FRAME_HEIGHT = 272 };

typedef struct {
    Bytes encoded;
    ImageResource image;
    DrawCommand command;
    LayoutDocument layout;
    TileCache cache;
    uint16_t frame[HELD_FRAME_WIDTH * HELD_FRAME_HEIGHT];
} HeldPaint;

/* A width x height JPEG painted at painted_width x painted_height from the
   top left of a 480x272 frame (twelve 128 px tiles), with `tiles` tile
   slots: fewer than twelve paint the rest through the frame's scratch
   tile. */
static bool held_paint_init(HeldPaint *paint, Budget *budget, int width,
                            int height, int painted_width, int painted_height,
                            size_t tiles)
{
    memset(paint, 0, sizeof(*paint));
    paint->encoded = jpeg_synthetic(width, height, 3, 85);
    paint->image = (ImageResource) {
        .encoded = paint->encoded.data,
        .encoded_length = paint->encoded.length,
        .source_width = width, .source_height = height,
        .width = width, .height = height
    };
    paint->command = (DrawCommand) {
        .type = DRAW_IMAGE, .image = &paint->image, .width = painted_width,
        .height = painted_height, .opacity_scale = 256,
        .image_fit = LAYOUT_IMAGE_FIT_STRETCH
    };
    paint->layout = (LayoutDocument) {
        .budget = budget, .commands = &paint->command, .count = 1,
        .width = HELD_FRAME_WIDTH, .scroll_width = HELD_FRAME_WIDTH,
        .height = painted_height > HELD_FRAME_HEIGHT
            ? painted_height : HELD_FRAME_HEIGHT,
        .page_background = 0x102040
    };
    if (!tile_cache_init(&paint->cache, budget, &paint->layout, tiles))
        return false;
    tile_cache_hold_decodes(&paint->cache, true);
    return tile_cache_set_frame(&paint->cache, paint->frame,
                                HELD_FRAME_WIDTH * HELD_FRAME_HEIGHT);
}

static bool held_paint_render(HeldPaint *paint)
{
    tile_cache_invalidate_rect(&paint->cache, 0, 0, HELD_FRAME_WIDTH,
                               HELD_FRAME_HEIGHT);
    return tile_cache_render_frame(&paint->cache, 0, HELD_FRAME_WIDTH,
                                   HELD_FRAME_HEIGHT, NULL);
}

static void held_paint_destroy(HeldPaint *paint)
{
    tile_cache_destroy(&paint->cache);
    free(paint->encoded.data);
}

/* Paints twice; each frame decodes the picture once, and nothing is left
   held after a frame. */
static int held_paint_twice(Budget *budget, int width, int height,
                            int painted_width, int painted_height,
                            size_t tiles)
{
    static HeldPaint paint;
    CHECK(held_paint_init(&paint, budget, width, height, painted_width,
                          painted_height, tiles));
    for (size_t frame = 1; frame <= 2; frame++) {
        CHECK(held_paint_render(&paint));
        fprintf(stderr, "held %dx%d at %dx%d, %zu tiles: frame %zu decodes "
                "%zu (%llu us painting)\n", width, height, painted_width,
                painted_height, tiles, frame,
                paint.cache.decoded_image_builds,
                (unsigned long long) paint.cache.frame_us);
        CHECK(paint.cache.decoded_image_builds == frame);
        CHECK(!paint.cache.held_image.valid);
    }
    /* Painted everywhere (a few photo pixels may match the background). */
    uint16_t background = tilefinch_rgb565_pack_u8(0x10, 0x20, 0x40);
    size_t uncovered = 0;
    for (size_t i = 0; i < HELD_FRAME_WIDTH * HELD_FRAME_HEIGHT; i++)
        uncovered += paint.frame[i] == background;
    CHECK(uncovered < 64);
    held_paint_destroy(&paint);
    CHECK(budget->current == 0);
    return 0;
}

/* A 720x540 photograph painted at its size (a 1.5 MB full-size decode, past
   the decoded-image cache), and a 1000x800 one painted at 480x384 (a 737 KB
   painted-size decode, past the scaled-image cache): one decode per frame
   over twelve tiles, resident or through the scratch tile. */
static int test_large_picture_decoded_once_per_pass(Budget *budget)
{
    CHECK(held_paint_twice(budget, 720, 540, 720, 540, 16) == 0);
    CHECK(held_paint_twice(budget, 1000, 800, 480, 384, 4) == 0);
    return 0;
}

static size_t held_reclaim_hook(void *opaque, size_t needed)
{
    (void) needed;
    return tile_cache_reclaim_held_image(opaque);
}

/* A frame painted over bounded slices holds its decode between them. An
   allocation that fits only without it reaches the Budget's reclaim hook,
   which frees it; the remaining tiles decode it again and the frame is the
   one painted with memory to spare. */
static int test_pressure_frees_held_decode(Budget *budget)
{
    static HeldPaint paint;
    static uint16_t reference[HELD_FRAME_WIDTH * HELD_FRAME_HEIGHT];
    CHECK(held_paint_init(&paint, budget, 720, 540, 720, 540, 16));
    /* The reference frame, which also allocates the tiles. */
    CHECK(held_paint_render(&paint));
    memcpy(reference, paint.frame, sizeof(reference));
    memset(paint.frame, 0, sizeof(paint.frame));
    tile_cache_invalidate_rect(&paint.cache, 0, 0, HELD_FRAME_WIDTH,
                               HELD_FRAME_HEIGHT);
    paint.cache.decoded_image_builds = 0;
    size_t base = budget->current;
    CHECK(tile_cache_prepare_frame_bounded(&paint.cache, 0, HELD_FRAME_WIDTH,
                                           HELD_FRAME_HEIGHT, UINT64_MAX, 1)
          == RENDER_FRAME_WORK_PENDING);
    CHECK(paint.cache.decoded_image_builds == 1
          && paint.cache.held_image.valid
          && !paint.cache.held_image.borrowed);
    size_t held = budget->current - base;
    CHECK(held >= 720u * 540u * 4u);
    /* A second slice paints from the held decode. */
    CHECK(tile_cache_prepare_frame_bounded(&paint.cache, 0, HELD_FRAME_WIDTH,
                                           HELD_FRAME_HEIGHT, UINT64_MAX, 1)
          == RENDER_FRAME_WORK_PENDING);
    CHECK(paint.cache.decoded_image_builds == 1
          && paint.cache.held_image_hits != 0);
    size_t limit = budget->limit;
    budget_set_reclaim_hook(budget, held_reclaim_hook, &paint.cache);
    budget->limit = budget->current + held / 2u;
    void *block = budget_malloc(budget, held - held / 4u);
    budget_set_reclaim_hook(budget, NULL, NULL);
    budget->limit = limit;
    CHECK(block != NULL && !paint.cache.held_image.valid
          && paint.cache.held_image_reclaims == 1);
    budget_free(budget, block);
    CHECK(budget->current == base);
    RenderFrameWorkResult work = RENDER_FRAME_WORK_PENDING;
    for (int slice = 0; slice < 16 && work == RENDER_FRAME_WORK_PENDING;
         slice++)
        work = tile_cache_prepare_frame_bounded(
            &paint.cache, 0, HELD_FRAME_WIDTH, HELD_FRAME_HEIGHT,
            UINT64_MAX, 1);
    CHECK(work == RENDER_FRAME_WORK_READY);
    CHECK(tile_cache_render_frame(&paint.cache, 0, HELD_FRAME_WIDTH,
                                  HELD_FRAME_HEIGHT, NULL));
    CHECK(paint.cache.decoded_image_builds == 2
          && !paint.cache.held_image.valid && budget->current == base);
    CHECK(memcmp(paint.frame, reference, sizeof(reference)) == 0);
    held_paint_destroy(&paint);
    CHECK(budget->current == 0);
    return 0;
}

/* Paint-ahead runs between the page's own turns: a run of it holds no
   decode when it returns, even with paint-ahead tiles still to do, so the
   page's memory checks between runs see what they would without it. */
static int test_paint_ahead_holds_no_decode_between_runs(Budget *budget)
{
    static HeldPaint paint;
    /* 720x1080: the picture continues below the first screen. */
    CHECK(held_paint_init(&paint, budget, 720, 1080, 720, 1080, 24));
    CHECK(held_paint_render(&paint) && !paint.cache.held_image.valid);
    size_t builds = paint.cache.decoded_image_builds;
    tile_cache_schedule_prefetch_row(&paint.cache, 384, HELD_FRAME_WIDTH);
    CHECK(tile_cache_run_idle_work(&paint.cache, UINT64_MAX / 2u, 1));
    CHECK(paint.cache.decoded_image_builds == builds + 1);
    CHECK(!paint.cache.held_image.valid);
    /* A synchronous prefetch decodes once for its whole row. */
    tile_cache_cancel_idle_work(&paint.cache);
    builds = paint.cache.decoded_image_builds;
    tile_cache_prefetch_row(&paint.cache, 640, HELD_FRAME_WIDTH);
    CHECK(paint.cache.decoded_image_builds == builds + 1
          && !paint.cache.held_image.valid);
    held_paint_destroy(&paint);
    CHECK(budget->current == 0);
    return 0;
}

/* A cache the Budget's reclaim hook cannot reach (the engine's stack
   previews, a child frame's) holds no decode: it decodes per command, as
   before, rather than keep memory from allocations the hook cannot free. */
static int test_unreached_cache_holds_no_decode(Budget *budget)
{
    static HeldPaint paint;
    CHECK(held_paint_init(&paint, budget, 720, 540, 720, 540, 16));
    tile_cache_hold_decodes(&paint.cache, false);
    CHECK(tile_cache_prepare_frame_bounded(&paint.cache, 0, HELD_FRAME_WIDTH,
                                           HELD_FRAME_HEIGHT, UINT64_MAX, 2)
          == RENDER_FRAME_WORK_PENDING);
    CHECK(paint.cache.decoded_image_builds == 2
          && !paint.cache.held_image.valid);
    held_paint_destroy(&paint);
    CHECK(budget->current == 0);
    return 0;
}

/* A caller that runs page work between bounded slices (the streaming
   preview's fill) ends the pass: the held decode goes although the frame's
   tile work is still pending, and the frame still completes. */
static int test_end_paint_pass_frees_pending_hold(Budget *budget)
{
    static HeldPaint paint;
    CHECK(held_paint_init(&paint, budget, 720, 540, 720, 540, 16));
    CHECK(tile_cache_prepare_frame_bounded(&paint.cache, 0, HELD_FRAME_WIDTH,
                                           HELD_FRAME_HEIGHT, UINT64_MAX, 1)
          == RENDER_FRAME_WORK_PENDING && paint.cache.held_image.valid);
    tile_cache_end_paint_pass(&paint.cache);
    CHECK(!paint.cache.held_image.valid
          && tile_cache_frame_work_pending(&paint.cache));
    CHECK(tile_cache_prepare_frame_bounded(&paint.cache, 0, HELD_FRAME_WIDTH,
                                           HELD_FRAME_HEIGHT, UINT64_MAX,
                                           SIZE_MAX)
          == RENDER_FRAME_WORK_READY);
    CHECK(paint.cache.decoded_image_builds == 2
          && !paint.cache.held_image.valid);
    held_paint_destroy(&paint);
    CHECK(budget->current == 0);
    return 0;
}
