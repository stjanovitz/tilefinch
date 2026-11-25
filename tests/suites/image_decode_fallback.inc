/* Paint-time decodes the Budget refuses: a full-scale JPEG decode falls back
   to half (or a quarter) of its source, painted larger, and a refused decode
   waits for free memory instead of running again on every paint. Included by
   test_image_decode_scaled.c. */

typedef struct {
    Bytes encoded;
    ImageResource image;
    DrawCommand command;
    LayoutDocument layout;
    TileCache cache;
    uint16_t frame[96u * 64u];
} FallbackPaint;

/* A size x size picture held encoded (`encoded`, freed with the paint),
   painted at painted x painted (for a JPEG, a full-scale IDCT) into a 96x64
   frame. */
static bool fallback_paint_init(FallbackPaint *paint, Budget *budget,
                                Bytes encoded, int size, int painted)
{
    memset(paint, 0, sizeof(*paint));
    paint->encoded = encoded;
    paint->image = (ImageResource) {
        .encoded = encoded.data, .encoded_length = encoded.length,
        .source_width = size, .source_height = size,
        .width = size, .height = size
    };
    paint->command = (DrawCommand) {
        .type = DRAW_IMAGE, .image = &paint->image, .width = painted,
        .height = painted, .opacity_scale = 256,
        .image_fit = LAYOUT_IMAGE_FIT_STRETCH
    };
    paint->layout = (LayoutDocument) {
        .budget = budget, .commands = &paint->command, .count = 1,
        .width = 96, .scroll_width = 96, .height = 64,
        .page_background = 0x102040
    };
    return tile_cache_init(&paint->cache, budget, &paint->layout, 1)
        && tile_cache_set_frame(&paint->cache, paint->frame, 96u * 64u);
}

static bool fallback_paint_render(FallbackPaint *paint)
{
    tile_cache_invalidate_rect(&paint->cache, 0, 0, 96, 64);
    return tile_cache_render_frame(&paint->cache, 0, 96, 64, NULL);
}

static void fallback_paint_destroy(FallbackPaint *paint)
{
    tile_cache_destroy(&paint->cache);
    free(paint->encoded.data);
}

/* A 720x720 4:4:4 photograph painted at 712x712 needs about 3.6 MB; with
   2.8 MiB free the half-size decode (518 KB kept beside the 2 MiB reserve, within
   the paint cache) is painted instead. */
static int test_paint_falls_back_to_half(Budget *budget)
{
    static FallbackPaint paint, reference;
    CHECK(fallback_paint_init(&reference, budget,
                              jpeg_synthetic(720, 720, 3, 95), 720, 712));
    CHECK(fallback_paint_render(&reference));
    fallback_paint_destroy(&reference);
    CHECK(fallback_paint_init(&paint, budget,
                              jpeg_synthetic(720, 720, 3, 95), 720, 712));
    size_t limit = budget->limit;
    budget->limit = budget->current + 2u * MIB + MIB * 4u / 5u;
    CHECK(fallback_paint_render(&paint));
    budget->limit = limit;
    bool reduced = false;
    for (size_t i = 0; i < TILEFINCH_IMAGE_CACHE_ENTRIES; i++) {
        const ScaledImageCacheEntry *entry = &paint.cache.scaled_images[i];
        reduced |= entry->valid && entry->retry_free_bytes != 0
            && entry->width == 712 && entry->pixel_width == 360
            && entry->pixels != NULL;
    }
    CHECK(reduced);
    /* Painted (no page background shows through), and close to the
       painted-size decode: within six levels per 565 channel on average. */
    uint16_t background = tilefinch_rgb565_pack_u8(0x10, 0x20, 0x40);
    uint64_t difference = 0;
    for (size_t i = 0; i < 96u * 64u; i++) {
        CHECK(paint.frame[i] != background);
        int a = paint.frame[i], b = reference.frame[i];
        difference += (uint64_t) (abs((a >> 11) - (b >> 11))
            + abs(((a >> 5) & 63) - ((b >> 5) & 63)) + abs((a & 31) - (b & 31)));
    }
    fprintf(stderr, "half-size fallback: mean 565 difference %.2f\n",
            (double) difference / (96.0 * 64.0 * 3.0));
    CHECK(difference <= 96u * 64u * 3u * 6u);
    fallback_paint_destroy(&paint);
    CHECK(budget->current == 0);
    return 0;
}

/* A 1000x1000 photograph painted at 800x800 with 1.5 MiB free: nothing
   fits, the decode is refused once, and paints at about the same pressure
   do not try it again; once the memory it needs is free, it decodes at the
   painted size. */
static int test_refused_paint_waits_for_memory(Budget *budget)
{
    static FallbackPaint paint;
    CHECK(fallback_paint_init(&paint, budget,
                              jpeg_synthetic(1000, 1000, 3, 85), 1000, 800));
    size_t limit = budget->limit;
    budget->limit = budget->current + MIB + MIB / 2u;
    CHECK(fallback_paint_render(&paint));
    size_t failures = paint.cache.decoded_image_failures;
    CHECK(failures != 0);
    for (int paint_turn = 0; paint_turn < 4; paint_turn++) {
        budget->limit += 32u * 1024u;
        CHECK(fallback_paint_render(&paint));
        CHECK(paint.cache.decoded_image_failures == failures);
    }
    budget->limit = limit;
    CHECK(fallback_paint_render(&paint));
    CHECK(paint.cache.decoded_image_failures == failures);
    for (size_t i = 0; i < TILEFINCH_IMAGE_CACHE_ENTRIES; i++)
        CHECK(!paint.cache.scaled_images[i].valid
              || paint.cache.scaled_images[i].retry_free_bytes == 0);
    uint16_t background = tilefinch_rgb565_pack_u8(0x10, 0x20, 0x40);
    for (size_t i = 0; i < 96u * 64u; i++)
        CHECK(paint.frame[i] != background);
    fallback_paint_destroy(&paint);
    CHECK(budget->current == 0);
    return 0;
}

/* The scaled-cache entry of a paint, or NULL. */
static const ScaledImageCacheEntry *fallback_paint_entry(
    const FallbackPaint *paint)
{
    for (size_t i = 0; i < TILEFINCH_IMAGE_CACHE_ENTRIES; i++)
        if (paint->cache.scaled_images[i].valid)
            return &paint->cache.scaled_images[i];
    return NULL;
}

static bool fallback_paint_covers(const FallbackPaint *paint)
{
    uint16_t background = tilefinch_rgb565_pack_u8(0x10, 0x20, 0x40);
    for (size_t i = 0; i < 96u * 64u; i++)
        if (paint->frame[i] == background) return false;
    return true;
}

/* A worker decode left uncollected: until it is, every other decode is
   refused as busy (not for memory). */
typedef struct {
    Bytes jpeg;
    BrowserSharedBody *body;
    uint32_t token;
} FallbackWorkerHold;

static bool fallback_hold_worker(Budget *budget, FallbackWorkerHold *hold)
{
    memset(hold, 0, sizeof(*hold));
    hold->jpeg = jpeg_synthetic(64, 64, 3, 85);
    unsigned char *copy = budget_malloc(budget, hold->jpeg.length);
    if (copy == NULL) return false;
    memcpy(copy, hold->jpeg.data, hold->jpeg.length);
    hold->body = browser_shared_body_take(budget, copy, hold->jpeg.length);
    if (hold->body == NULL) return false;
    ImageResource image = {
        .encoded = hold->body->data, .encoded_body = hold->body,
        .encoded_length = hold->jpeg.length,
        .source_width = 64, .source_height = 64, .width = 32, .height = 32
    };
    size_t refused = 0;
    return image_decode_worker_submit(&image, budget, &hold->token, &refused)
        == IMAGE_DECODE_SUBMIT_ACCEPTED;
}

static bool fallback_release_worker(Budget *budget, FallbackWorkerHold *hold)
{
    ImageDecodeWorkerResult result = {0};
    bool collected = image_decode_worker_collect(hold->token, &result);
    budget_free(budget, result.pixels);
    browser_shared_body_release(hold->body);
    free(hold->jpeg.data);
    return collected && images_decode_worker_shutdown(budget);
}

/* A reduced decode refused as busy rather than for memory is tried again
   at the next paint at the same pressure. */
static int test_busy_reduced_decode_retries(Budget *budget)
{
    static FallbackPaint paint;
    FallbackWorkerHold hold;
    CHECK(fallback_paint_init(&paint, budget,
                              jpeg_synthetic(720, 720, 3, 95), 720, 712));
    size_t limit = budget->limit;
    CHECK(fallback_hold_worker(budget, &hold));
    budget->limit = budget->current + 2u * MIB + MIB * 4u / 5u;
    CHECK(fallback_paint_render(&paint));
    CHECK(!fallback_paint_covers(&paint));
    budget->limit = limit;
    CHECK(fallback_release_worker(budget, &hold));
    budget->limit = budget->current + 2u * MIB + MIB * 4u / 5u;
    CHECK(fallback_paint_render(&paint));
    const ScaledImageCacheEntry *entry = fallback_paint_entry(&paint);
    CHECK(entry != NULL && entry->pixels != NULL && entry->pixel_width == 360);
    CHECK(fallback_paint_covers(&paint));
    budget->limit = limit;
    fallback_paint_destroy(&paint);
    CHECK(budget->current == 0);
    return 0;
}

/* A busy attempt to replace a kept reduced decode paints the reduced one
   and does not spend the one replacement. */
static int test_busy_upgrade_keeps_reduced(Budget *budget)
{
    static FallbackPaint paint;
    FallbackWorkerHold hold;
    CHECK(fallback_paint_init(&paint, budget,
                              jpeg_synthetic(720, 720, 3, 95), 720, 712));
    size_t limit = budget->limit;
    budget->limit = budget->current + 2u * MIB + MIB * 4u / 5u;
    CHECK(fallback_paint_render(&paint));
    budget->limit = limit;
    const ScaledImageCacheEntry *entry = fallback_paint_entry(&paint);
    CHECK(entry != NULL && entry->pixel_width == 360);
    CHECK(fallback_hold_worker(budget, &hold));
    CHECK(fallback_paint_render(&paint));
    CHECK(fallback_paint_covers(&paint));
    CHECK(fallback_release_worker(budget, &hold));
    /* The painted-size decode (larger than the cache keeps, so painted
       as a temporary) replaces it: no record is left. */
    CHECK(fallback_paint_render(&paint) && fallback_paint_covers(&paint));
    CHECK(fallback_paint_entry(&paint) == NULL);
    fallback_paint_destroy(&paint);
    CHECK(budget->current == 0);
    return 0;
}

/* A PNG painted smaller than its cached full-size decode, with too little
   free for the smaller decode, is painted from the cached one. */
static int test_refused_png_paints_cached_decode(Budget *budget)
{
    static FallbackPaint paint;
    CHECK(fallback_paint_init(&paint, budget,
                              png_synthetic(200, 200, 8, 2, false), 200, 200));
    CHECK(fallback_paint_render(&paint) && fallback_paint_covers(&paint));
    paint.command.width = paint.command.height = 100;
    size_t limit = budget->limit;
    size_t failures = budget->failure_count;
    budget->limit = budget->current + 24u * 1024u;
    CHECK(fallback_paint_render(&paint));
    budget->limit = limit;
    CHECK(budget->failure_count != failures);
    CHECK(fallback_paint_covers(&paint));
    fallback_paint_destroy(&paint);
    CHECK(budget->current == 0);
    return 0;
}

/* A quarter-size decode the paint cache cannot keep is not used (it would
   be decoded for every tile and paint); nothing is decoded again until
   memory frees. */
static int test_uncacheable_reduced_decode_waits(Budget *budget)
{
    static FallbackPaint paint;
    CHECK(fallback_paint_init(&paint, budget,
                              jpeg_synthetic(1500, 1500, 3, 85), 1500, 1400));
    size_t limit = budget->limit;
    budget->limit = budget->current + 3u * MIB;
    CHECK(fallback_paint_render(&paint));
    size_t builds = paint.cache.decoded_image_builds;
    size_t failures = paint.cache.decoded_image_failures;
    for (int turn = 0; turn < 3; turn++) {
        CHECK(fallback_paint_render(&paint));
        CHECK(paint.cache.decoded_image_builds == builds
              && paint.cache.decoded_image_failures == failures);
    }
    budget->limit = limit;
    CHECK(fallback_paint_render(&paint) && fallback_paint_covers(&paint));
    fallback_paint_destroy(&paint);
    CHECK(budget->current == 0);
    return 0;
}

/* The worker fallback says half fits exactly when the worker accepts the
   half-size decode: its arena aligns the output. */
static int test_worker_fallback_matches_submit(Budget *budget)
{
    Bytes jpeg = jpeg_synthetic(721, 721, 3, 85);
    unsigned char *copy = budget_malloc(budget, jpeg.length);
    CHECK(copy != NULL);
    memcpy(copy, jpeg.data, jpeg.length);
    BrowserSharedBody *body = browser_shared_body_take(budget, copy,
                                                       jpeg.length);
    CHECK(body != NULL);
    ImageResource image = {
        .encoded = body->data, .encoded_body = body,
        .encoded_length = jpeg.length,
        .source_width = 721, .source_height = 721,
        .width = 712, .height = 712
    };
    int width = 712, height = 712;
    size_t needed = 0;
    CHECK(image_decode_reduced_fallback(&image, budget, true, SIZE_MAX,
                                        &width, &height, &needed)
          && width == 361 && height == 361);
    ImageResource half = image;
    half.width = width;
    half.height = height;
    uint32_t token = 0;
    size_t refused = 0;
    /* Start the worker, then measure the charge it refuses. */
    CHECK(image_decode_worker_submit(&half, budget, &token, &refused)
              == IMAGE_DECODE_SUBMIT_ACCEPTED);
    ImageDecodeWorkerResult result = {0};
    CHECK(image_decode_worker_collect(token, &result));
    budget_free(budget, result.pixels);
    size_t limit = budget->limit, base = budget->current;
    budget->limit = base;
    CHECK(image_decode_worker_submit(&half, budget, &token, &refused)
              == IMAGE_DECODE_SUBMIT_REJECTED && refused != 0);
    for (size_t edge = refused - 32u; edge <= refused; edge++) {
        budget->limit = base + edge;
        width = height = 712;
        bool fits = image_decode_reduced_fallback(&image, budget, true,
                                                  SIZE_MAX, &width, &height,
                                                  &needed)
            && width == 361;
        CHECK(fits == (edge == refused));
    }
    budget->limit = limit;
    browser_shared_body_release(body);
    free(jpeg.data);
    CHECK(images_decode_worker_shutdown(budget) && budget->current == 0);
    return 0;
}

/* A JPEG its load decoded at half its size grows back to that size through
   display retargeting once the decode fits, and is not decoded while it
   cannot. */
static int test_reduced_load_grows_when_it_fits(Budget *budget)
{
    Bytes jpeg = jpeg_synthetic(720, 720, 3, 95);
    ImageResources images = {.budget = budget};
    images.items = budget_calloc(budget, 1, sizeof(*images.items));
    unsigned char *encoded = budget_malloc(budget, jpeg.length);
    unsigned char *pixels = budget_malloc_category(
        budget, BUDGET_CATEGORY_RESOURCE, 360u * 360u * 4u);
    CHECK(images.items != NULL && encoded != NULL && pixels != NULL);
    memcpy(encoded, jpeg.data, jpeg.length);
    memset(pixels, 0x80, 360u * 360u * 4u);
    images.capacity = images.count = 1;
    images.items[0] = (ImageResource) {
        .pixels = pixels, .owns_pixels = true,
        .encoded = encoded, .encoded_length = jpeg.length,
        .owns_encoded = true,
        .source_width = 720, .source_height = 720,
        .width = 360, .height = 360, .full_width = 720, .full_height = 720
    };
    DrawCommand command = {
        .type = DRAW_IMAGE, .image = &images.items[0], .width = 700,
        .height = 700, .image_fit = LAYOUT_IMAGE_FIT_STRETCH
    };
    LayoutDocument layout = {.commands = &command, .count = 1};
    size_t limit = budget->limit, failures = budget->failure_count;
    budget->limit = budget->current + MIB;
    for (size_t generation = 1; generation <= 3; generation++)
        CHECK(images_retarget_display_step(&images, &layout, NULL,
                                           generation, NULL)
              != IMAGE_RETARGET_RETRY);
    CHECK(budget->failure_count == failures
          && images.items[0].width == 360);
    budget->limit = limit;
    CHECK(images_retarget_display_step(&images, &layout, NULL, 4, NULL)
              == IMAGE_RETARGET_CHANGED);
    CHECK(images.items[0].width == 720 && images.items[0].height == 720
          && images.items[0].pixels != NULL);
    images_destroy(&images);
    free(jpeg.data);
    CHECK(budget->current == 0);
    return 0;
}
