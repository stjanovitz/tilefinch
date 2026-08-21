static void decoded_image_evict(TileCache *cache, size_t index)
{
    DecodedImageCacheEntry *entry = &cache->decoded_images[index];
    if (!entry->valid) return;
    if (entry->pixels != NULL) {
        image_resource_free_decoded(cache->budget, entry->pixels);
        cache->decoded_image_cache_bytes =
            cache->decoded_image_cache_bytes >= entry->bytes
            ? cache->decoded_image_cache_bytes - entry->bytes : 0;
        cache->decoded_image_evictions++;
    }
    memset(entry, 0, sizeof(*entry));
}

static void decoded_image_clear(TileCache *cache)
{
    for (size_t i = 0; i < TILEFINCH_IMAGE_CACHE_ENTRIES; i++) {
        decoded_image_evict(cache, i);
    }
}

static size_t decoded_image_lru(const TileCache *cache)
{
    size_t victim = TILEFINCH_IMAGE_CACHE_ENTRIES;
    for (size_t i = 0; i < TILEFINCH_IMAGE_CACHE_ENTRIES; i++) {
        if (!cache->decoded_images[i].valid) return i;
        if (victim == TILEFINCH_IMAGE_CACHE_ENTRIES
            || cache->decoded_images[i].last_used
               < cache->decoded_images[victim].last_used) victim = i;
    }
    return victim;
}

static size_t decoded_image_oldest_valid(const TileCache *cache)
{
    size_t victim = TILEFINCH_IMAGE_CACHE_ENTRIES;
    for (size_t i = 0; i < TILEFINCH_IMAGE_CACHE_ENTRIES; i++) {
        if (!cache->decoded_images[i].valid) continue;
        if (victim == TILEFINCH_IMAGE_CACHE_ENTRIES
            || cache->decoded_images[i].last_used
               < cache->decoded_images[victim].last_used) victim = i;
    }
    return victim;
}

static DecodedImageCacheEntry *decoded_image_insert(
    TileCache *cache, const void *identity, int width, int height,
    unsigned char *pixels, bool failed)
{
    size_t bytes = pixels != NULL ? budget_usable_size(pixels) : 0;
    if (bytes > TILEFINCH_DECODED_IMAGE_CACHE_BYTES) return NULL;
    while (bytes != 0
           && (cache->decoded_image_cache_bytes
                   > TILEFINCH_DECODED_IMAGE_CACHE_BYTES
               || bytes > TILEFINCH_DECODED_IMAGE_CACHE_BYTES
                          - cache->decoded_image_cache_bytes)) {
        size_t victim = decoded_image_oldest_valid(cache);
        if (victim == TILEFINCH_IMAGE_CACHE_ENTRIES) break;
        decoded_image_evict(cache, victim);
    }
    if (cache->decoded_image_cache_bytes > TILEFINCH_DECODED_IMAGE_CACHE_BYTES
        || bytes > TILEFINCH_DECODED_IMAGE_CACHE_BYTES
                   - cache->decoded_image_cache_bytes) return NULL;
    size_t slot = decoded_image_lru(cache);
    if (slot == TILEFINCH_IMAGE_CACHE_ENTRIES) return NULL;
    if (cache->decoded_images[slot].valid) decoded_image_evict(cache, slot);
    DecodedImageCacheEntry *entry = &cache->decoded_images[slot];
    *entry = (DecodedImageCacheEntry) {
        .identity = identity,
        .pixels = pixels,
        .width = width,
        .height = height,
        .bytes = bytes,
        .last_used = ++cache->clock,
        .valid = true,
        .failed = failed
    };
    cache->decoded_image_cache_bytes += bytes;
    return entry;
}

static const unsigned char *decoded_image_pixels(
    TileCache *cache, const ImageResource *image,
    unsigned char **temporary)
{
    *temporary = NULL;
    if (image->pixels != NULL) return image->pixels;
    const void *identity = image_resource_backing_identity(image);
    if (identity == NULL) identity = image;
    for (size_t i = 0; i < TILEFINCH_IMAGE_CACHE_ENTRIES; i++) {
        DecodedImageCacheEntry *entry = &cache->decoded_images[i];
        if (!entry->valid || entry->identity != identity
            || entry->width != image->width || entry->height != image->height) {
            continue;
        }
        entry->last_used = ++cache->clock;
        if (entry->failed) return NULL;
        cache->decoded_image_hits++;
        return entry->pixels;
    }
    unsigned char *pixels = NULL;
    uint64_t decode_started = render_now_us();
    ImageDecodeStatus decode_status = image_resource_decode_checked(
        image, cache->budget, &pixels);
    uint64_t decode_elapsed = render_now_us() - decode_started;
    cache->decoded_image_us += decode_elapsed;
    if (decode_elapsed > cache->max_decoded_image_us) {
        cache->max_decoded_image_us = decode_elapsed;
    }
    if (decode_status != IMAGE_DECODE_SUCCEEDED) {
        cache->decoded_image_failures++;
        if (decode_status == IMAGE_DECODE_DETERMINISTIC_FAILURE) {
            (void) decoded_image_insert(cache, identity, image->width,
                                        image->height, NULL, true);
        }
        return NULL;
    }
    cache->decoded_image_builds++;
    DecodedImageCacheEntry *entry = decoded_image_insert(
        cache, identity, image->width, image->height, pixels, false);
    if (entry == NULL) {
        /*
         * Keep the cache at a real hard cap.  An oversized decode (or a
         * metadata insertion failure) is still rendered exactly, but its
         * ownership is returned to the caller for command-scoped cleanup.
         */
        *temporary = pixels;
        return pixels;
    }
    return entry->pixels;
}

static void scaled_image_evict(TileCache *cache, size_t index)
{
    ScaledImageCacheEntry *entry = &cache->scaled_images[index];
    if (!entry->valid) return;
    if (entry->pixels != NULL) {
        budget_free(cache->budget, entry->pixels);
        cache->scaled_image_cache_bytes =
            cache->scaled_image_cache_bytes >= entry->bytes
            ? cache->scaled_image_cache_bytes - entry->bytes : 0;
        cache->scaled_image_evictions++;
    }
    memset(entry, 0, sizeof(*entry));
}

static void scaled_image_clear(TileCache *cache)
{
    for (size_t i = 0; i < TILEFINCH_IMAGE_CACHE_ENTRIES; i++) {
        scaled_image_evict(cache, i);
    }
}

static size_t scaled_image_lru(const TileCache *cache)
{
    size_t victim = TILEFINCH_IMAGE_CACHE_ENTRIES;
    for (size_t i = 0; i < TILEFINCH_IMAGE_CACHE_ENTRIES; i++) {
        if (!cache->scaled_images[i].valid) return i;
        if (victim == TILEFINCH_IMAGE_CACHE_ENTRIES
            || cache->scaled_images[i].last_used
               < cache->scaled_images[victim].last_used) victim = i;
    }
    return victim;
}

static size_t scaled_image_oldest_valid(const TileCache *cache)
{
    size_t victim = TILEFINCH_IMAGE_CACHE_ENTRIES;
    for (size_t i = 0; i < TILEFINCH_IMAGE_CACHE_ENTRIES; i++) {
        if (!cache->scaled_images[i].valid) continue;
        if (victim == TILEFINCH_IMAGE_CACHE_ENTRIES
            || cache->scaled_images[i].last_used
               < cache->scaled_images[victim].last_used) victim = i;
    }
    return victim;
}

static ScaledImageCacheEntry *scaled_image_insert(
    TileCache *cache, const void *identity, int source_width,
    int source_height, int width, int height, unsigned char *pixels,
    bool failed)
{
    size_t bytes = pixels != NULL ? budget_usable_size(pixels) : 0;
    if (bytes > TILEFINCH_SCALED_IMAGE_CACHE_BYTES) return NULL;
    while (cache->scaled_image_cache_bytes > TILEFINCH_SCALED_IMAGE_CACHE_BYTES
           || bytes > TILEFINCH_SCALED_IMAGE_CACHE_BYTES
                      - cache->scaled_image_cache_bytes) {
        size_t victim = scaled_image_oldest_valid(cache);
        if (victim == TILEFINCH_IMAGE_CACHE_ENTRIES) break;
        scaled_image_evict(cache, victim);
    }
    if (cache->scaled_image_cache_bytes > TILEFINCH_SCALED_IMAGE_CACHE_BYTES
        || bytes > TILEFINCH_SCALED_IMAGE_CACHE_BYTES
                   - cache->scaled_image_cache_bytes) return NULL;
    size_t slot = scaled_image_lru(cache);
    if (slot == TILEFINCH_IMAGE_CACHE_ENTRIES) return NULL;
    if (cache->scaled_images[slot].valid) scaled_image_evict(cache, slot);
    ScaledImageCacheEntry *entry = &cache->scaled_images[slot];
    *entry = (ScaledImageCacheEntry) {
        .identity = identity,
        .pixels = pixels,
        .source_width = source_width,
        .source_height = source_height,
        .width = width,
        .height = height,
        .bytes = bytes,
        .last_used = ++cache->clock,
        .valid = true,
        .failed = failed
    };
    cache->scaled_image_cache_bytes += bytes;
    return entry;
}

static const unsigned char *image_pixels_for_command(
    TileCache *cache, const ImageResource *image, const DrawCommand *command,
    int *pixel_width, int *pixel_height,
    unsigned char **temporary_decoded)
{
    const void *identity = image_resource_backing_identity(image);
    if (identity == NULL) identity = image;
    int scaled_width = command->width;
    int scaled_height = command->height;
    bool wants_scaled = command->image_fit == LAYOUT_IMAGE_FIT_STRETCH
        && (scaled_width != image->width || scaled_height != image->height);
    bool direct_scaled_jpeg = false;
    if (command->image_fit == LAYOUT_IMAGE_FIT_COVER
        && image->pixels == NULL && image->encoded != NULL
        && image->encoded_length >= 2u
        && image->encoded[0] == 0xffu && image->encoded[1] == 0xd8u
        && image->width > 0 && image->height > 0
        && command->width > 0 && command->height > 0
        && (size_t) command->width
               <= SIZE_MAX / (size_t) image->height
        && (size_t) command->height
               <= SIZE_MAX / (size_t) image->width) {
        size_t width_scale = (size_t) command->width
                             * (size_t) image->height;
        size_t height_scale = (size_t) command->height
                              * (size_t) image->width;
        if (width_scale >= height_scale) {
            size_t product = (size_t) image->height
                             * (size_t) command->width;
            scaled_width = command->width;
            scaled_height = (int) (product / (size_t) image->width
                + (product % (size_t) image->width != 0));
        } else {
            size_t product = (size_t) image->width
                             * (size_t) command->height;
            scaled_width = (int) (product / (size_t) image->height
                + (product % (size_t) image->height != 0));
            scaled_height = command->height;
        }
        direct_scaled_jpeg = scaled_width > 0 && scaled_height > 0
            && scaled_width < image->width
            && scaled_height < image->height;
        wants_scaled = direct_scaled_jpeg;
    }
    bool scaled_failed = false;
    if (wants_scaled) {
        for (size_t i = 0; i < TILEFINCH_IMAGE_CACHE_ENTRIES; i++) {
            ScaledImageCacheEntry *entry = &cache->scaled_images[i];
            if (!entry->valid || entry->identity != identity
                || entry->source_width != image->width
                || entry->source_height != image->height
                || entry->width != scaled_width
                || entry->height != scaled_height) continue;
            entry->last_used = ++cache->clock;
            if (entry->failed) {
                scaled_failed = true;
                break;
            }
            cache->scaled_image_hits++;
            *pixel_width = entry->width;
            *pixel_height = entry->height;
            return entry->pixels;
        }
    }

    if (direct_scaled_jpeg && !scaled_failed) {
        ImageResource decode_target = *image;
        decode_target.width = scaled_width;
        decode_target.height = scaled_height;
        unsigned char *pixels = NULL;
        uint64_t decode_started = render_now_us();
        ImageDecodeStatus decode_status = image_resource_decode_checked(
            &decode_target, cache->budget, &pixels);
        uint64_t decode_elapsed = render_now_us() - decode_started;
        cache->decoded_image_us += decode_elapsed;
        if (decode_elapsed > cache->max_decoded_image_us)
            cache->max_decoded_image_us = decode_elapsed;
        if (decode_status == IMAGE_DECODE_SUCCEEDED) {
            ScaledImageCacheEntry *entry = scaled_image_insert(
                cache, identity, image->width, image->height,
                scaled_width, scaled_height, pixels, false);
            if (entry != NULL) {
                cache->decoded_image_builds++;
                cache->scaled_image_builds++;
                *pixel_width = entry->width;
                *pixel_height = entry->height;
                return entry->pixels;
            }
            *temporary_decoded = pixels;
            *pixel_width = scaled_width;
            *pixel_height = scaled_height;
            return pixels;
        }
        cache->decoded_image_failures++;
        if (decode_status == IMAGE_DECODE_DETERMINISTIC_FAILURE) {
            (void) scaled_image_insert(
                cache, identity, image->width, image->height,
                scaled_width, scaled_height, NULL, true);
        }
    }

    const unsigned char *source = decoded_image_pixels(
        cache, image, temporary_decoded);
    if (source == NULL) return NULL;
    if (wants_scaled && !direct_scaled_jpeg && !scaled_failed
        && command->width > 0 && command->height > 0) {
        size_t width = (size_t) scaled_width;
        size_t height = (size_t) scaled_height;
        if (height <= SIZE_MAX / width
            && width * height <= TILEFINCH_SCALED_IMAGE_CACHE_BYTES / 4u) {
            size_t bytes = width * height * 4u;
            unsigned char *scaled = budget_malloc(cache->budget, bytes);
            if (scaled != NULL) {
                uint64_t scale_started = render_now_us();
                ScaleStepper source_y = scale_stepper(
                    0, image->height, scaled_height);
                for (int y = 0; y < scaled_height; y++) {
                    ScaleStepper source_x = scale_stepper(
                        0, image->width, scaled_width);
                    for (int x = 0; x < scaled_width; x++) {
                        memcpy(scaled + ((size_t) y * scaled_width + x) * 4u,
                               source + ((size_t) source_y.value * image->width
                                         + source_x.value) * 4u, 4u);
                        scale_stepper_advance(&source_x);
                    }
                    scale_stepper_advance(&source_y);
                }
                uint64_t scale_elapsed = render_now_us() - scale_started;
                cache->scaled_image_us += scale_elapsed;
                if (scale_elapsed > cache->max_scaled_image_us) {
                    cache->max_scaled_image_us = scale_elapsed;
                }
                ScaledImageCacheEntry *entry = scaled_image_insert(
                    cache, identity, image->width, image->height,
                    scaled_width, scaled_height, scaled, false);
                if (entry != NULL) {
                    cache->scaled_image_builds++;
                    image_resource_free_decoded(cache->budget,
                                                *temporary_decoded);
                    *temporary_decoded = NULL;
                    *pixel_width = entry->width;
                    *pixel_height = entry->height;
                    return entry->pixels;
                }
                budget_free(cache->budget, scaled);
            }
        }
    }
    *pixel_width = image->width;
    *pixel_height = image->height;
    return source;
}

static void rasterize_image(TileCache *cache, RasterTarget *target,
                            const DrawCommand *command)
{
    const ImageResource *image = command->image;
    if (!image_resource_available(image) || image->width <= 0
        || image->height <= 0 || command->width <= 0 || command->height <= 0) {
        return;
    }
    int pixel_width = 0, pixel_height = 0;
    unsigned char *temporary_decoded = NULL;
    const unsigned char *pixels = image_pixels_for_command(
        cache, image, command, &pixel_width, &pixel_height,
        &temporary_decoded);
    if (pixels == NULL) {
        image_resource_free_decoded(cache->budget, temporary_decoded);
        return;
    }
    int image_clip_radius = draw_command_image_clip_radius(command);
    bool image_has_clip_radius =
        style_border_radius_maximum(image_clip_radius) > 0;
    int image_clip_radii[4] = {0, 0, 0, 0};
    if (image_has_clip_radius) {
        style_border_radius_resolve_corners(
            image_clip_radius, command->width, command->height,
            image_clip_radii);
    }
    unsigned rotation = draw_command_rotation_quadrants(command);
    if (rotation != 0 && command->image_fit == LAYOUT_IMAGE_FIT_STRETCH) {
        /* Rotated stretch is the one image span this function clamps for
           itself; the branch below it, and every clamp in
           rasterize_command(), take the far edge in int64_t for the reason
           spelled out there.  Narrowing is safe on the same grounds: the
           cast is reached only where the sum is already inside the target,
           and rasterize_command() has run intersects() before calling. */
        int x0 = command->x > target->left ? command->x : target->left;
        int y0 = command->y > target->top ? command->y : target->top;
        int64_t right = (int64_t) command->x + command->width;
        int64_t bottom = (int64_t) command->y + command->height;
        int x1 = right < target->right ? (int) right : target->right;
        int y1 = bottom < target->bottom ? (int) bottom : target->bottom;
        int source_box_width = (rotation & 1u) != 0
            ? command->height : command->width;
        int source_box_height = (rotation & 1u) != 0
            ? command->width : command->height;
        for (int y = y0; y < y1; y++) {
            int destination_y = y - command->y;
            int span_x0 = 0, span_x1 = 0;
            size_t index = 0;
            bool rounded = false;
            if (!raster_target_span(target, y, x0, x1, &span_x0,
                                    &span_x1, &index, &rounded)) continue;
            ScaleStepper horizontal = scale_stepper(
                span_x0 - command->x,
                rotation == 2u ? pixel_width : pixel_height,
                command->width);
            int fixed_source_x = rotation == 1u
                ? tilefinch_mul_div_int(
                      destination_y, pixel_width, source_box_width)
                : (rotation == 3u
                   ? pixel_width - 1 - tilefinch_mul_div_int(
                         destination_y, pixel_width, source_box_width)
                   : 0);
            int fixed_source_y = rotation == 1u || rotation == 3u
                ? 0
                : pixel_height - 1 - tilefinch_mul_div_int(
                         destination_y, pixel_height, source_box_height);
            for (int x = span_x0; x < span_x1; x++, index++) {
                int source_x = fixed_source_x;
                int source_y = fixed_source_y;
                if (rotation == 1u) {
                    source_y = pixel_height - 1 - horizontal.value;
                } else if (rotation == 2u) {
                    source_x = pixel_width - 1 - horizontal.value;
                } else {
                    source_y = horizontal.value;
                }
                if (source_x < 0) source_x = 0;
                if (source_y < 0) source_y = 0;
                if (source_x >= pixel_width) source_x = pixel_width - 1;
                if (source_y >= pixel_height) source_y = pixel_height - 1;
                const unsigned char *pixel = pixels
                    + ((size_t) source_y * (size_t) pixel_width
                       + (size_t) source_x) * 4u;
                uint32_t color = image->is_mask
                    ? command->color
                    : (((uint32_t) pixel[0] << 16)
                       | ((uint32_t) pixel[1] << 8) | pixel[2]);
                unsigned alpha = (unsigned) pixel[3]
                    * command->opacity_scale / 256u;
                if (image_has_clip_radius) {
                    unsigned coverage = rounded_rect_coverage_quarters(
                        x, y, command->x, command->y,
                        command->width, command->height, image_clip_radii);
                    alpha = (alpha * coverage + 2u) / 4u;
                }
                if (alpha != 0) blend_at_index(target, index, color, alpha);
                scale_stepper_advance(&horizontal);
            }
        }
        image_resource_free_decoded(cache->budget, temporary_decoded);
        return;
    }
    int draw_x = command->x, draw_y = command->y;
    int draw_width = command->width, draw_height = command->height;
    int source_x_offset = 0, source_y_offset = 0;
    int source_width = pixel_width, source_height = pixel_height;
    uint16_t encoded_x = (uint16_t) draw_command_image_offset_x(command);
    uint16_t encoded_y = (uint16_t) draw_command_image_offset_y(command);
    int position_x = style_object_position_percent(encoded_x);
    int position_y = style_object_position_percent(encoded_y);
    int position_offset_x = style_object_position_offset(encoded_x);
    int position_offset_y = style_object_position_offset(encoded_y);
    bool scale_down = command->image_fit == LAYOUT_IMAGE_FIT_SCALE_DOWN;
    bool use_contain = command->image_fit == LAYOUT_IMAGE_FIT_CONTAIN
        || (scale_down
            && (pixel_width > command->width
                || pixel_height > command->height));
    if (use_contain) {
        if ((int64_t) pixel_width * command->height
            > (int64_t) pixel_height * command->width) {
            draw_height = tilefinch_mul_div_int(
                command->width, pixel_height, pixel_width);
            if (draw_height < 1) draw_height = 1;
            draw_y += (command->height - draw_height) * position_y / 100
                      + position_offset_y;
        } else {
            draw_width = tilefinch_mul_div_int(
                command->height, pixel_width, pixel_height);
            if (draw_width < 1) draw_width = 1;
            draw_x += (command->width - draw_width) * position_x / 100
                      + position_offset_x;
        }
    } else if (command->image_fit == LAYOUT_IMAGE_FIT_COVER) {
        if ((int64_t) pixel_width * command->height
            > (int64_t) pixel_height * command->width) {
            source_width = tilefinch_mul_div_int(
                pixel_height, command->width, command->height);
            if (source_width < 1) source_width = 1;
            source_x_offset = (pixel_width - source_width) * position_x / 100
                              - position_offset_x;
        } else {
            source_height = tilefinch_mul_div_int(
                pixel_width, command->height, command->width);
            if (source_height < 1) source_height = 1;
            source_y_offset = (pixel_height - source_height)
                              * position_y / 100 - position_offset_y;
        }
    } else if (command->image_fit == LAYOUT_IMAGE_FIT_NONE || scale_down) {
        draw_width = pixel_width;
        draw_height = pixel_height;
        draw_x += (command->width - draw_width) * position_x / 100
                  + position_offset_x;
        draw_y += (command->height - draw_height) * position_y / 100
                  + position_offset_y;
    }
    int x0 = draw_x > target->left ? draw_x : target->left;
    int y0 = draw_y > target->top ? draw_y : target->top;
    int64_t draw_right = (int64_t) draw_x + draw_width;
    int64_t draw_bottom = (int64_t) draw_y + draw_height;
    int x1 = draw_right < target->right ? (int) draw_right : target->right;
    int y1 = draw_bottom < target->bottom ? (int) draw_bottom
                                          : target->bottom;
    if (command->image_fit >= LAYOUT_IMAGE_FIT_SPRITE
        && command->image_fit <= LAYOUT_IMAGE_FIT_SPRITE_TILE_XY) {
        bool tile_x = command->image_fit == LAYOUT_IMAGE_FIT_SPRITE_TILE_X
                      || command->image_fit
                         == LAYOUT_IMAGE_FIT_SPRITE_TILE_XY;
        bool tile_y = command->image_fit == LAYOUT_IMAGE_FIT_SPRITE_TILE_Y
                      || command->image_fit
                         == LAYOUT_IMAGE_FIT_SPRITE_TILE_XY;
        /* Sprite crop: sample at the authored background-size (or at the
           natural size when it was omitted); pixels outside a non-repeating
           sheet stay transparent. */
        int offset_x = draw_command_image_offset_x(command);
        int offset_y = draw_command_image_offset_y(command);
        /* Offsets address the image's natural (CSS) pixel grid; the decoded
           copy may have been budget-downsampled, so map through the source
           dimensions.  Under viewport visual scaling the command box is in
           device pixels while the crop is in CSS pixels, so device deltas
           convert back through the scale first. */
        int natural_width = image->source_width > 0 ? image->source_width
                                                    : pixel_width;
        int natural_height = image->source_height > 0 ? image->source_height
                                                      : pixel_height;
        int rendered_width = draw_command_image_sprite_width(command);
        int rendered_height = draw_command_image_sprite_height(command);
        if (rendered_width <= 0) rendered_width = natural_width;
        if (rendered_height <= 0) rendered_height = natural_height;
        const ViewportContext *sprite_viewport =
            &cache->source_layout->viewport;
        int scale_numerator = sprite_viewport->scale_numerator > 0
                              ? sprite_viewport->scale_numerator : 1;
        int scale_denominator = sprite_viewport->scale_denominator > 0
                                ? sprite_viewport->scale_denominator : 1;
        /* The device-to-natural mapping is floor(k * denominator /
           numerator) over a contiguous k, which is exactly what ScaleStepper
           walks with one add per step; the non-tiled blit below already uses
           it.  Both k origins are non-negative because x0/y0 are clamped to
           draw_x/draw_y.  The tile reduction follows the same walk by
           compare-and-subtract, and the natural-to-decoded mapping collapses
           to the identity whenever the decoded copy was not downsampled,
           which is the ordinary background-image case.  What is left is two
           64-bit divisions per row instead of two per pixel. */
        ScaleStepper natural_y_step = scale_stepper(
            y0 - draw_y, scale_denominator, scale_numerator);
        int tiled_y = 0, previous_natural_y = 0;
        bool tiled_y_valid = false;
        for (int y = y0; y < y1;
             y++, scale_stepper_advance(&natural_y_step)) {
            int natural_y = natural_y_step.value - offset_y;
            if (tile_y && rendered_height > 0) {
                if (!tiled_y_valid) {
                    tiled_y = natural_y % rendered_height;
                    if (tiled_y < 0) tiled_y += rendered_height;
                    tiled_y_valid = true;
                } else {
                    tiled_y = sprite_tile_wrap(
                        tiled_y, natural_y - previous_natural_y,
                        rendered_height);
                }
                previous_natural_y = natural_y;
                natural_y = tiled_y;
            }
            if (natural_y < 0 || natural_y >= rendered_height) continue;
            int source_y = pixel_height == rendered_height
                ? natural_y
                : tilefinch_mul_div_int(
                      natural_y, pixel_height, rendered_height);
            if (source_y >= pixel_height) source_y = pixel_height - 1;
            int span_x0 = 0, span_x1 = 0;
            size_t index = 0;
            bool rounded = false;
            if (!raster_target_span(target, y, x0, x1, &span_x0, &span_x1,
                                    &index, &rounded)) continue;
            int unclipped_x0 = span_x0;
            if (style_border_radius_maximum(image_clip_radius) > 0
                && !rounded_rect_clip_row_span(
                    command->x, command->y,
                    command->width, command->height, image_clip_radius,
                    y, &span_x0, &span_x1)) continue;
            index += (size_t) (span_x0 - unclipped_x0);
            ScaleStepper natural_x_step = scale_stepper(
                span_x0 - draw_x, scale_denominator, scale_numerator);
            int tiled_x = 0, previous_natural_x = 0;
            bool tiled_x_valid = false;
            for (int x = span_x0; x < span_x1;
                 x++, index++, scale_stepper_advance(&natural_x_step)) {
                int natural_x = natural_x_step.value - offset_x;
                if (tile_x && rendered_width > 0) {
                    if (!tiled_x_valid) {
                        tiled_x = natural_x % rendered_width;
                        if (tiled_x < 0) tiled_x += rendered_width;
                        tiled_x_valid = true;
                    } else {
                        tiled_x = sprite_tile_wrap(
                            tiled_x, natural_x - previous_natural_x,
                            rendered_width);
                    }
                    previous_natural_x = natural_x;
                    natural_x = tiled_x;
                }
                if (natural_x < 0 || natural_x >= rendered_width) continue;
                int source_x = pixel_width == rendered_width
                    ? natural_x
                    : tilefinch_mul_div_int(
                          natural_x, pixel_width, rendered_width);
                if (source_x >= pixel_width) source_x = pixel_width - 1;
                const unsigned char *pixel = pixels
                    + ((size_t) source_y * (size_t) pixel_width
                       + (size_t) source_x) * 4u;
                uint32_t color = image->is_mask
                                 ? command->color
                                 : (((uint32_t) pixel[0] << 16)
                                    | ((uint32_t) pixel[1] << 8) | pixel[2]);
                unsigned alpha = (unsigned) pixel[3]
                                 * command->opacity_scale / 256u;
                if (image_has_clip_radius) {
                    unsigned coverage = rounded_rect_coverage_quarters(
                        x, y, command->x, command->y,
                        command->width, command->height, image_clip_radii);
                    alpha = (alpha * coverage + 2u) / 4u;
                }
                if (alpha == 0) continue;
                blend_at_index(target, index, color, alpha);
            }
        }
        image_resource_free_decoded(cache->budget, temporary_decoded);
        return;
    }
    ScaleStepper source_y_step = scale_stepper(y0 - draw_y, source_height,
                                                draw_height);
    for (int y = y0; y < y1; y++) {
        int source_y = source_y_offset + source_y_step.value;
        if (source_y >= pixel_height) source_y = pixel_height - 1;
        int span_x0 = 0, span_x1 = 0;
        size_t index = 0;
        bool rounded = false;
        if (!raster_target_span(target, y, x0, x1, &span_x0,
                                &span_x1, &index, &rounded)) {
            scale_stepper_advance(&source_y_step);
            continue;
        }
        int unclipped_x0 = span_x0;
        if (style_border_radius_maximum(image_clip_radius) > 0
            && !rounded_rect_clip_row_span(
                command->x, command->y,
                command->width, command->height, image_clip_radius,
                y, &span_x0, &span_x1)) {
            scale_stepper_advance(&source_y_step);
            continue;
        }
        index += (size_t) (span_x0 - unclipped_x0);
        ScaleStepper source_x_step = scale_stepper(
            span_x0 - draw_x, source_width, draw_width);
        for (int x = span_x0; x < span_x1; x++, index++) {
            int source_x = source_x_offset + source_x_step.value;
            if (source_x >= pixel_width) source_x = pixel_width - 1;
            const unsigned char *pixel = pixels
                + ((size_t) source_y * (size_t) pixel_width
                   + (size_t) source_x) * 4u;
            uint32_t color = image->is_mask
                             ? command->color
                             : (((uint32_t) pixel[0] << 16)
                                | ((uint32_t) pixel[1] << 8) | pixel[2]);
            unsigned alpha = (unsigned) pixel[3] * command->opacity_scale
                             / 256u;
            if (image_has_clip_radius) {
                unsigned coverage = rounded_rect_coverage_quarters(
                    x, y, command->x, command->y,
                    command->width, command->height, image_clip_radii);
                alpha = (alpha * coverage + 2u) / 4u;
            }
            if (alpha != 0) blend_at_index(target, index, color, alpha);
            scale_stepper_advance(&source_x_step);
        }
        scale_stepper_advance(&source_y_step);
    }
    image_resource_free_decoded(cache->budget, temporary_decoded);
}

/* Interpolate two 0xAARRGGBB stops in sRGB.  The rest of this rasterizer
   blends in sRGB as well; introducing a second colour-space convention here
   while gamma-correct text blending is still deferred would make the two
   disagree. */
static uint32_t gradient_mix(uint32_t low, uint32_t high, unsigned fraction)
{
    uint32_t mixed = 0;
    for (unsigned shift = 0; shift <= 24; shift += 8) {
        unsigned a = (low >> shift) & 0xffu;
        unsigned b = (high >> shift) & 0xffu;
        unsigned value = a + ((int) (b - a) * (int) fraction + 127) / 255;
        mixed |= (value & 0xffu) << shift;
    }
    return mixed;
}

/* Axial `linear-gradient()` fill.  The ramp is expanded once per command
   into a 256-entry lookup table, then each span walks the gradient line in
   16.16 fixed point: one add per pixel, no per-pixel stop search, and every
   loop bounded by the already-clipped tile rectangle. */
static void gradient_build_lut(const StyleGradient *ramp, uint32_t lut[256])
{
    unsigned count = ramp->stop_count;
    if (count > STYLE_GRADIENT_STOP_LIMIT) count = STYLE_GRADIENT_STOP_LIMIT;
    for (unsigned i = 0; i < 256u; i++) {
        unsigned high = 0;
        while (high < count && ramp->stop_position[high] < i) high++;
        if (high == 0) {
            lut[i] = ramp->stop_argb[0];
        } else if (high >= count) {
            lut[i] = ramp->stop_argb[count - 1];
        } else {
            unsigned low_position = ramp->stop_position[high - 1];
            unsigned high_position = ramp->stop_position[high];
            unsigned span = high_position - low_position;
            unsigned fraction = span == 0 ? 0u
                : (i - low_position) * 255u / span;
            lut[i] = gradient_mix(ramp->stop_argb[high - 1],
                                  ramp->stop_argb[high], fraction);
        }
    }
}

static void gradient_build_geometry(RenderGradientCacheEntry *entry,
                                    const DrawCommand *command,
                                    const StyleGradient *ramp)
{
    if ((ramp->angle & STYLE_GRADIENT_RADIAL) != 0) {
        int width = command->width > 0 ? command->width : 1;
        int height = command->height > 0 ? command->height : 1;
        if ((ramp->angle & STYLE_GRADIENT_RADIAL_CIRCLE) != 0) {
            int large = width > height ? width : height;
            int small = width > height ? height : width;
            int extent = large + (small * 3) / 8;
            entry->step_x = entry->step_y =
                (65536 + extent / 2) / (extent > 0 ? extent : 1);
        } else {
            entry->step_x = (65536 + width / 2) / width;
            entry->step_y = (65536 + height / 2) / height;
        }
        entry->origin = 0;
        return;
    }
    float radians = (float) (ramp->angle & STYLE_GRADIENT_ANGLE_MASK)
                    * 0.017453292f;
    float direction_x = sinf(radians);
    float direction_y = -cosf(radians);
    float line = fabsf((float) command->width * direction_x)
                 + fabsf((float) command->height * direction_y);
    if (line < 1.0f) line = 1.0f;
    float center_x = (float) command->x + (float) command->width / 2.0f;
    float center_y = (float) command->y + (float) command->height / 2.0f;
    entry->step_x = (int32_t) (direction_x / line * 65536.0f);
    entry->step_y = (int32_t) (direction_y / line * 65536.0f);
    entry->origin = (int64_t) ((-center_x * direction_x
                                - center_y * direction_y)
                               / line * 65536.0f) + 32768;
}

static unsigned gradient_lut_index(const StyleGradient *ramp,
                                   int64_t position)
{
    int index = position <= 0 ? 0 : (position >= 65535 ? 255
                                  : (int) (position >> 8));
    if ((ramp->angle & STYLE_GRADIENT_REPEATING) != 0
        && ramp->stop_count >= 2) {
        int first = ramp->stop_position[0];
        int last = ramp->stop_position[ramp->stop_count - 1];
        int period = last - first;
        if (period > 0) {
            int offset = (index - first) % period;
            if (offset < 0) offset += period;
            index = first + offset;
        }
    }
    return (unsigned) index;
}

static int64_t gradient_radial_position(
    const RenderGradientCacheEntry *setup, const DrawCommand *command,
    const StyleGradient *ramp, int x, int y)
{
    int64_t center_x_twice = (int64_t) command->x * 2 + command->width;
    int64_t center_y_twice = (int64_t) command->y * 2 + command->height;
    int64_t dx = (int64_t) x * 2 + 1 - center_x_twice;
    int64_t dy = (int64_t) y * 2 + 1 - center_y_twice;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    dx *= setup->step_x;
    dy *= setup->step_y;
    int64_t large = dx > dy ? dx : dy;
    int64_t small = dx > dy ? dy : dx;
    int64_t approximate = large + (small * 3) / 8;
    if ((ramp->angle & STYLE_GRADIENT_RADIAL_CIRCLE) == 0) {
        approximate = approximate * 186 / 256;
    }
    return approximate;
}

static const RenderGradientCacheEntry *gradient_cached_setup(
    TileCache *cache, const DrawCommand *command, size_t slot,
    const StyleGradient *ramp, RenderGradientCacheEntry *fallback)
{
    if (cache->gradient_cache == NULL) {
        cache->gradient_cache = budget_calloc(
            cache->budget, 1, sizeof(*cache->gradient_cache));
    }
    if (cache->gradient_cache == NULL) {
        memset(fallback, 0, sizeof(*fallback));
        gradient_build_lut(ramp, fallback->lut);
        gradient_build_geometry(fallback, command, ramp);
        cache->gradient_lut_builds++;
        return fallback;
    }
    RenderGradientCacheEntry *victim = NULL;
    for (size_t i = 0; i < RENDER_GRADIENT_CACHE_ENTRIES; i++) {
        RenderGradientCacheEntry *entry = &cache->gradient_cache->entries[i];
        if (entry->valid && entry->slot == slot
            && entry->command == command) {
            entry->last_used = ++cache->gradient_cache->clock;
            cache->gradient_lut_hits++;
            return entry;
        }
        if (!entry->valid
            || victim == NULL || entry->last_used < victim->last_used) {
            victim = entry;
        }
    }
    gradient_build_lut(ramp, victim->lut);
    gradient_build_geometry(victim, command, ramp);
    victim->slot = slot;
    victim->command = command;
    victim->valid = true;
    victim->last_used = ++cache->gradient_cache->clock;
    cache->gradient_lut_builds++;
    return victim;
}

static void rasterize_gradient_fill(TileCache *cache,
                                    RasterTarget *target,
                                    const DrawCommand *command,
                                    const StyleGradient *ramp,
                                    size_t gradient_slot,
                                    int x0, int y0, int x1, int y1,
                                    int radius, unsigned alpha)
{
    RenderGradientCacheEntry fallback;
    const RenderGradientCacheEntry *setup = gradient_cached_setup(
        cache, command, gradient_slot, ramp, &fallback);
    const uint32_t *lut = setup->lut;
    int corner_radii[4];
    style_border_radius_resolve_corners(
        radius, command->width, command->height, corner_radii);
    bool has_radius = style_border_radius_maximum(radius) > 0;
    /* CSS angles: 0deg points to the top of the box and increases
       clockwise, so the direction in screen space (y down) is
       (sin A, -cos A).  The gradient line length is the CSS projection of
       the box onto that direction. */
    /* Single precision throughout: the PSP FPU is single precision, and
       these are per-command, not per-pixel. */
    int32_t step_x = setup->step_x;
    int32_t step_y = setup->step_y;
    int64_t origin = setup->origin;
    bool radial = (ramp->angle & STYLE_GRADIENT_RADIAL) != 0;
    for (int y = y0; y < y1; y++) {
        int span_x0 = 0, span_x1 = 0;
        size_t index = 0;
        bool rounded = false;
        if (!raster_target_span(target, y, x0, x1, &span_x0, &span_x1,
                                &index, &rounded)) continue;
        int64_t position = origin + (int64_t) step_y * y
                           + (int64_t) step_x * span_x0;
        /*
         * Square-cornered gradients keep the pre-antialiasing inner loop.
         * Coverage there is 4/4 for every pixel, and (blended * 4 + 2) / 4
         * is blended, so folding the rounded case in would spend a branch
         * and a multiply-divide per pixel to recompute a constant -- on a
         * 333 MHz Allegrex, across every gradient span in the frame. The
         * two bodies are otherwise the same code and must stay so: the
         * output is bit-identical either way.
         */
        if (!has_radius) {
            for (int x = span_x0; x < span_x1; x++, index++,
                                               position += step_x) {
                if (rounded
                    && !point_in_rounded_clips(target->rounded_clips,
                                               target->rounded_clip_count,
                                               x, y)) continue;
                int64_t sample = radial
                    ? gradient_radial_position(setup, command, ramp, x, y)
                    : position;
                uint32_t argb = lut[gradient_lut_index(ramp, sample)];
                unsigned stop_alpha = (argb >> 24) & 0xffu;
                unsigned blended = alpha * stop_alpha / 255u;
                if (blended == 0) continue;
                uint32_t color =
                    (command->image_fit & LAYOUT_FILL_GRADIENT_AS_MASK) != 0
                    ? target->resolved_color_value
                    : argb & 0x00ffffffu;
                blend_at_index(target, index,
                               rgb565_dither_source(color, x, y), blended);
            }
            continue;
        }
        for (int x = span_x0; x < span_x1; x++, index++,
                                           position += step_x) {
            unsigned coverage = rounded_rect_coverage_quarters(
                x, y, command->x, command->y,
                command->width, command->height, corner_radii);
            if (coverage == 0) continue;
            if (rounded
                && !point_in_rounded_clips(target->rounded_clips,
                                           target->rounded_clip_count,
                                           x, y)) continue;
            int64_t sample = radial
                ? gradient_radial_position(setup, command, ramp, x, y)
                : position;
            uint32_t argb = lut[gradient_lut_index(ramp, sample)];
            unsigned stop_alpha = (argb >> 24) & 0xffu;
            unsigned blended = alpha * stop_alpha / 255u;
            blended = (blended * coverage + 2u) / 4u;
            if (blended == 0) continue;
            uint32_t color =
                (command->image_fit & LAYOUT_FILL_GRADIENT_AS_MASK) != 0
                ? target->resolved_color_value
                : argb & 0x00ffffffu;
            blend_at_index(target, index,
                           rgb565_dither_source(color, x, y), blended);
        }
    }
}

static unsigned integer_sqrt(uint32_t value)
{
    return tilefinch_isqrt_u32(value);
}

/* Bound on command->scale wherever it sizes raster geometry: border
   thickness and box-shadow blur. Style output never comes close -- border
   widths are device pixels and blur is capped at
   STYLE_BOX_SHADOW_BLUR_LIMIT -- but scale is a retained-layout field a
   hostile or corrupted command array can set to INT_MAX, and the stroke and
   shadow arms below derive dash periods and box deflations from it. 2^20
   keeps the largest derived term, the six-thickness dash period plus a tile
   coordinate, far inside int. */
#define RASTER_COMMAND_SCALE_LIMIT (1 << 20)

/* Saturating narrowing for command-box sums that are only ever compared
   against tile-bounded pixel coordinates. Saturation preserves the order of
   the true value against anything an int can hold, so every such comparison
   answers the same as the unbounded sum would. */
static int raster_extent_to_int(int64_t value)
{
    if (value > INT32_MAX) return INT32_MAX;
    if (value < INT32_MIN) return INT32_MIN;
    return (int) value;
}

/* Exact signed distance from a pixel CENTRE to a rounded rectangle, in
   half-pixel units, negative inside.  Half-pixel units keep the pixel centre
   (x + 0.5) on the integer grid, so the whole field is integer arithmetic --
   there is no float unit on the PSP.  The square root only runs in the four
   corner quadrants, where both axes are outside their straight edge. */
static int rounded_rect_distance_half(int x, int y, int rect_x, int rect_y,
                                      int width, int height, int radius)
{
    if (radius < 0) radius = 0;
    int maximum = width < height ? width / 2 : height / 2;
    if (radius > maximum) radius = maximum;
    /* The pixel-to-origin offsets are the one place a hostile shadow box
       still meets a tile coordinate in int: rect_x is command->x plus the
       blur, so the subtraction has to widen before it happens, not after. */
    int64_t cx = 2 * ((int64_t) x - rect_x) + 1 - width;
    int64_t cy = 2 * ((int64_t) y - rect_y) + 1 - height;
    if (cx < 0) cx = -cx;
    if (cy < 0) cy = -cy;
    cx -= width - 2 * radius;
    cy -= height - 2 * radius;
    int64_t qx = cx > 0 ? cx : 0;
    int64_t qy = cy > 0 ? cy : 0;
    int64_t outside;
    if (qx == 0) outside = qy;
    else if (qy == 0) outside = qx;
    else {
        /* Past 2^16 half-pixels either square alone saturates the sum, so
           clamping first answers identically and keeps the widened offsets
           from overflowing the 64-bit products. */
        if (qx > 65536) qx = 65536;
        if (qy > 65536) qy = 65536;
        uint64_t sum = (uint64_t) (qx * qx) + (uint64_t) (qy * qy);
        if (sum > UINT32_MAX) sum = UINT32_MAX;
        outside = integer_sqrt((uint32_t) sum);
    }
    int64_t inside = cx > cy ? cx : cy;
    if (inside > 0) inside = 0;
    int64_t distance = outside + inside - 2 * radius;
    if (distance > INT32_MAX) return INT32_MAX;
    if (distance < INT32_MIN) return INT32_MIN;
    return (int) distance;
}

/* `box-shadow` layer.  A true Gaussian blur is far too costly on MIPS and a
   multi-pass box blur would need a scratch buffer this renderer refuses to
   allocate, so the falloff is analytic: coverage is a smoothstep over the
   signed distance field above, ramping across the blur diameter centred on
   the shadow edge.  That matches the CSS definition of the blur radius
   closely enough at PSP resolution, costs no memory, and composes with
   border-radius for free because the distance field already is the rounded
   rect. */
/* Kept out of rasterize_command: shadows are one command kind among five,
   and inlining this body -- the distance field and its smoothstep -- charges
   every fill, stroke and glyph dispatch the I-cache footprint of the
   rarest arm. The hot-symbol ratchet measures that dispatch. */
__attribute__((noinline))
static void rasterize_shadow(RasterTarget *target, const DrawCommand *command,
                             int x0, int y0, int x1, int y1, unsigned alpha)
{
    int blur = command->scale;
    if (blur < 0) blur = 0;
    if (blur > RASTER_COMMAND_SCALE_LIMIT) blur = RASTER_COMMAND_SCALE_LIMIT;
    /* The deflation sums are unbounded retained coordinates, the same shape
       as the span clamp in rasterize_command: take them in int64_t. The
       origins can only grow, so they saturate; the sizes can only shrink,
       so a positive result always fits int. */
    int64_t shadow_width64 = (int64_t) command->width - 2 * blur;
    int64_t shadow_height64 = (int64_t) command->height - 2 * blur;
    if (shadow_width64 <= 0 || shadow_height64 <= 0) return;
    int shadow_x = raster_extent_to_int((int64_t) command->x + blur);
    int shadow_y = raster_extent_to_int((int64_t) command->y + blur);
    int shadow_width = (int) shadow_width64;
    int shadow_height = (int) shadow_height64;
    int radius = command->radius;
    for (int y = y0; y < y1; y++) {
        int span_x0 = 0, span_x1 = 0;
        size_t index = 0;
        bool rounded = false;
        if (!raster_target_span(target, y, x0, x1, &span_x0, &span_x1,
                                &index, &rounded)) continue;
        for (int x = span_x0; x < span_x1; x++, index++) {
            if (rounded
                && !point_in_rounded_clips(target->rounded_clips,
                                           target->rounded_clip_count,
                                           x, y)) continue;
            int distance = rounded_rect_distance_half(
                x, y, shadow_x, shadow_y, shadow_width, shadow_height,
                radius);
            unsigned coverage;
            if (blur == 0) {
                if (distance >= 0) continue;
                coverage = 256u;
            } else {
                /* CSS defines the blur radius as a Gaussian of standard
                   deviation blur/2, whose visible transition spans roughly
                   one blur radius either side of the shadow edge.  In
                   half-pixel units that is [-2*blur, +2*blur], which is also
                   exactly how far the command box was inflated. */
                int reach = 2 * blur;
                if (distance >= reach) continue;
                if (distance <= -reach) coverage = 256u;
                else {
                    int t = (reach - distance) * 256 / (2 * reach);
                    if (t < 0) t = 0;
                    if (t > 256) t = 256;
                    coverage = (unsigned) (t * t * (768 - 2 * t)) / 65536u;
                    if (coverage > 256u) coverage = 256u;
                }
            }
            unsigned blended = alpha * coverage / 256u;
            if (blended == 0) continue;
            if (blended > 255u) blended = 255u;
            blend_at_index(target, index,
                           rgb565_dither_source(command->color, x, y),
                           blended);
        }
    }
}

/* Kept out of rasterize_command for the same reason rasterize_shadow is:
   the stroke arm derives an inner box, dash phases and the annulus
   coverage of two rounded rectangles, and inlining all of that charges
   every fill, image and glyph dispatch its instruction-cache footprint.
   The hot-symbol ratchet measures that dispatch. */
__attribute__((noinline))
static void rasterize_stroke_rect(RasterTarget *target,
                                  const DrawCommand *command,
                                  int x0, int y0, int x1, int y1)
{
    int radius = command->radius;
    int corner_radii[4];
    style_border_radius_resolve_corners(
        radius, command->width, command->height, corner_radii);
    bool has_radius = style_border_radius_maximum(radius) > 0;
    /*
     * Everything this arm derives -- the inner box, the row band that
     * decides which edge a dash belongs to, the dash period itself --
     * is a sum of command->scale with the unbounded command box. One
     * bound on the scale plus int64_t box sums saturated once per
     * command makes the whole arm defined: the saturated edges are only
     * ever compared against tile-bounded pixel coordinates, where
     * saturation preserves every answer, and the dash phase is reduced
     * modulo its period from the clamp origin, which x0/y0 never
     * precede. Hoisting the edges also spares the pixel loop the two
     * sums it used to redo per pixel.
     */
    int thickness = command->scale > 0 ? command->scale : 1;
    if (thickness > RASTER_COMMAND_SCALE_LIMIT) {
        thickness = RASTER_COMMAND_SCALE_LIMIT;
    }
    int64_t inner_width64 = (int64_t) command->width - 2 * thickness;
    int64_t inner_height64 = (int64_t) command->height - 2 * thickness;
    bool has_inner = inner_width64 > 0 && inner_height64 > 0;
    int inner_x = 0, inner_y = 0, inner_width = 0, inner_height = 0;
    int inner_right = 0, inner_bottom = 0;
    if (has_inner) {
        /* Both extents are positive here, so every edge can only leave
           the int range upward and the sizes never do at all. */
        int64_t inner_x64 = (int64_t) command->x + thickness;
        int64_t inner_y64 = (int64_t) command->y + thickness;
        inner_x = raster_extent_to_int(inner_x64);
        inner_y = raster_extent_to_int(inner_y64);
        inner_width = (int) inner_width64;
        inner_height = (int) inner_height64;
        inner_right = raster_extent_to_int(inner_x64 + inner_width64);
        inner_bottom = raster_extent_to_int(inner_y64 + inner_height64);
    }
    int inner_radius = style_border_radius_adjust(radius, -thickness);
    int inner_corner_radii[4];
    style_border_radius_resolve_corners(
        inner_radius, inner_width, inner_height, inner_corner_radii);
    bool has_inner_radius =
        style_border_radius_maximum(inner_radius) > 0;
    RoundedStrokeGeometry stroke_geometry = {
        .x = command->x,
        .y = command->y,
        .width = command->width,
        .height = command->height,
        .radii = corner_radii,
        .has_inner = has_inner,
        .inner_x = inner_x,
        .inner_y = inner_y,
        .inner_right = inner_right,
        .inner_bottom = inner_bottom,
        .inner_width = inner_width,
        .inner_height = inner_height,
        .has_inner_radius = has_inner_radius,
        .inner_radii = inner_corner_radii
    };
    unsigned alpha = 255u * command->opacity_scale / 256u;
    if (alpha == 0) return;
    bool dashed = command->image_fit == LAYOUT_STROKE_DASHED
                  || command->image_fit == LAYOUT_STROKE_DOTTED;
    int stroke_top = 0, stroke_bottom = 0;
    int period = 1, painted = 1, dash_x = 0, dash_y = 0;
    if (dashed) {
        stroke_top = raster_extent_to_int(
            (int64_t) command->y + thickness);
        stroke_bottom = raster_extent_to_int(
            (int64_t) command->y + command->height - thickness);
        period = command->image_fit == LAYOUT_STROKE_DOTTED
            ? thickness * 2 : thickness * 6;
        painted = command->image_fit == LAYOUT_STROKE_DOTTED
            ? thickness : thickness * 3;
        /* x0 never precedes command->x and their true distance is
           below 2^32, so the wrapping unsigned subtraction is exact
           and a 32-bit remainder replaces the 64-bit one. */
        dash_x = (int) (((uint32_t) x0 - (uint32_t) command->x)
                        % (uint32_t) period);
        dash_y = (int) (((uint32_t) y0 - (uint32_t) command->y)
                        % (uint32_t) period);
    }
    for (int y = y0; y < y1; y++) {
        int span_x0 = 0, span_x1 = 0;
        size_t index = 0;
        bool rounded = false;
        if (!raster_target_span(target, y, x0, x1, &span_x0, &span_x1,
                                &index, &rounded)) continue;
        for (int x = span_x0; x < span_x1; x++, index++) {
            /* Square corners never leave the annulus partially covered,
               so they keep the plain box test and the untouched alpha
               the arm has always used. */
            unsigned coverage = 4u;
            if (has_radius) {
                coverage = rounded_stroke_coverage_quarters(
                    x, y, &stroke_geometry);
                if (coverage == 0) continue;
            } else if (has_inner
                       && x >= inner_x && x < inner_right
                       && y >= inner_y && y < inner_bottom) {
                continue;
            }
            if (dashed) {
                /* The phase only matters modulo the period, so
                   anchoring it at the clamp origin instead of the
                   command corner keeps it in range for any box; x0/y0
                   never precede the corner, so the residue is the one
                   the corner would give. */
                bool horizontal = y < stroke_top || y >= stroke_bottom;
                int phase = horizontal ? x - x0 + dash_x
                                       : y - y0 + dash_y;
                if (phase % period >= painted) continue;
            }
            if (rounded
                && !point_in_rounded_clips(target->rounded_clips,
                                           target->rounded_clip_count,
                                           x, y)) continue;
            blend_at_index(
                target, index, command->color,
                coverage == 4u ? alpha
                               : (alpha * coverage + 2u) / 4u);
        }
    }
    return;
}

/* Tile-local separable box blur for translucent fixed/sticky chrome.  The
   command admission below caps the full region, while this helper processes
   at most one 128-pixel strip at a time with seven RGB565 rows (1.75 KiB).
   Ordinary commands never enter it and pay only the radius test. */
__attribute__((noinline))
static void rasterize_small_backdrop_blur(
    RasterTarget *target, const DrawCommand *command,
    int x0, int y0, int x1, int y1, unsigned radius)
{
    if (target == NULL || command == NULL || radius == 0u || radius > 3u
        || x0 >= x1 || y0 >= y1 || command->width <= 0
        || command->height <= 0 || command->height > 96
        || command->width > 24576 / command->height) return;
    uint16_t rows[7][TILEFINCH_TILE_SIZE];
    int corner_radii[4];
    style_border_radius_resolve_corners(
        command->radius, command->width, command->height, corner_radii);
    bool rounded = style_border_radius_maximum(command->radius) > 0;
    for (int chunk_x = x0; chunk_x < x1;
         chunk_x += TILEFINCH_TILE_SIZE) {
        int chunk_right = chunk_x + TILEFINCH_TILE_SIZE;
        if (chunk_right > x1) chunk_right = x1;
        int chunk_width = chunk_right - chunk_x;
        int prepared_through = y0 - 1;
        for (int y = y0; y < y1; y++) {
            int need = y + (int) radius;
            if (need >= y1) need = y1 - 1;
            while (prepared_through < need) {
                prepared_through++;
                uint16_t *output = rows[prepared_through % 7];
                for (int local_x = 0; local_x < chunk_width; local_x++) {
                    int world_x = chunk_x + local_x;
                    unsigned red = 0, green = 0, blue = 0, samples = 0;
                    for (int dx = -(int) radius;
                         dx <= (int) radius; dx++) {
                        int sample_x = world_x + dx;
                        if (sample_x < chunk_x) sample_x = chunk_x;
                        if (sample_x >= chunk_right) sample_x = chunk_right - 1;
                        size_t index = 0;
                        if (!raster_target_index(
                                target, sample_x, prepared_through,
                                &index)) continue;
                        uint16_t pixel = target->pixels[index];
                        red += tilefinch_rgb565_red_code(pixel);
                        green += tilefinch_rgb565_green_code(pixel);
                        blue += tilefinch_rgb565_blue_code(pixel);
                        samples++;
                    }
                    output[local_x] = samples == 0 ? 0
                        : tilefinch_rgb565_pack_codes(
                              (red + samples / 2u) / samples,
                              (green + samples / 2u) / samples,
                              (blue + samples / 2u) / samples);
                }
            }
            for (int local_x = 0; local_x < chunk_width; local_x++) {
                int world_x = chunk_x + local_x;
                if (rounded
                    && rounded_rect_coverage_quarters(
                           world_x, y, command->x, command->y,
                           command->width, command->height,
                           corner_radii) == 0) continue;
                unsigned red = 0, green = 0, blue = 0, samples = 0;
                for (int dy = -(int) radius; dy <= (int) radius; dy++) {
                    int sample_y = y + dy;
                    if (sample_y < y0) sample_y = y0;
                    if (sample_y >= y1) sample_y = y1 - 1;
                    uint16_t pixel = rows[sample_y % 7][local_x];
                    red += tilefinch_rgb565_red_code(pixel);
                    green += tilefinch_rgb565_green_code(pixel);
                    blue += tilefinch_rgb565_blue_code(pixel);
                    samples++;
                }
                size_t index = 0;
                if (samples != 0
                    && raster_target_index(target, world_x, y, &index)) {
                    target->pixels[index] = tilefinch_rgb565_pack_codes(
                        (red + samples / 2u) / samples,
                        (green + samples / 2u) / samples,
                        (blue + samples / 2u) / samples);
                }
            }
        }
    }
}

static void rasterize_command(TileCache *cache, RasterTarget *target,
                              const DrawCommand *command)
{
    target->filter_code = (uint8_t) draw_command_filter_code(command);
    target->blend_mode = (uint8_t) draw_command_blend_mode(command);
    target->filter_low_amount =
        (command->font_italic & LAYOUT_COMMAND_FILTER_LOW_AMOUNT) != 0;
    target->forced_dark = cache->forced_dark;
    target->forced_dark_role = command->type == DRAW_IMAGE
        ? FORCED_DARK_NONE
        : (command->type == DRAW_FILL_RECT
               || command->type == DRAW_SHADOW_RECT
             ? FORCED_DARK_SURFACE
             : (command->type == DRAW_STROKE_RECT
                    ? FORCED_DARK_BORDER : FORCED_DARK_INK));
    target->resolved_color_source = command->color;
    target->resolved_color_value = raster_filter_color(
        command->color, target->filter_code, target->filter_low_amount,
        target->forced_dark,
        (ForcedDarkRole) target->forced_dark_role);
    target->resolved_color_valid = true;
    if (!intersects(command, target->left, target->top,
                    target->right, target->bottom)) return;
    /*
     * One span clamp for every box-shaped command: the shadow, fill,
     * gradient and stroke arms below all reduce the command box to the tile
     * with these same four values. command->x and command->width are
     * retained-layout coordinates with no bound of their own, so their far
     * edge is widened before comparison and narrowed only after it is known
     * to sit inside the int-bounded target.
     *
     * Keep the work behind the type gate. Images and glyphs clamp spans they
     * derive themselves, and text commands dominate article pages; charging
     * their hot dispatch for two widened sums that they never consume is an
     * avoidable Allegrex cost.
     */
    bool box_command = command->type == DRAW_SHADOW_RECT
        || command->type == DRAW_FILL_RECT
        || command->type == DRAW_STROKE_RECT;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    if (box_command) {
        x0 = command->x > target->left ? command->x : target->left;
        y0 = command->y > target->top ? command->y : target->top;
        int64_t command_right = (int64_t) command->x + command->width;
        int64_t command_bottom = (int64_t) command->y + command->height;
        x1 = command_right < target->right
             ? (int) command_right : target->right;
        y1 = command_bottom < target->bottom
             ? (int) command_bottom : target->bottom;
    }
    if (command->type == DRAW_SHADOW_RECT) {
        unsigned alpha = 255u * command->opacity_scale / 256u;
        if (alpha == 0) return;
        rasterize_shadow(target, command, x0, y0, x1, y1, alpha);
        return;
    }
    if (command->type == DRAW_FILL_RECT) {
        int radius = command->radius;
        int corner_radii[4];
        style_border_radius_resolve_corners(
            radius, command->width, command->height, corner_radii);
        bool has_radius = style_border_radius_maximum(radius) > 0;
        unsigned alpha = 255u * command->opacity_scale / 256u;
        unsigned backdrop_blur = draw_command_backdrop_blur(command);
        if (backdrop_blur != 0u) {
            rasterize_small_backdrop_blur(
                target, command, x0, y0, x1, y1, backdrop_blur);
        }
        size_t gradient_slot = 0;
        if (draw_command_fill_gradient(command, &gradient_slot)
            && cache->layout != NULL
            && gradient_slot < cache->layout->gradient_count) {
            if (alpha == 0) return;
            rasterize_gradient_fill(cache, target, command,
                                    &cache->layout->gradients[gradient_slot],
                                    gradient_slot,
                                    x0, y0, x1, y1, radius, alpha);
            return;
        }
        if (!has_radius && alpha == 255u
            && target->alpha == NULL && target->rounded_clip_count == 0) {
            uint16_t color = rgb565_surface(
                target->resolved_color_value);
            for (int y = y0; y < y1; y++) {
                size_t index = 0;
                if (!raster_target_index(target, x0, y, &index)) continue;
                uint16_t *row = target->pixels + index;
                for (int x = x0; x < x1; x++) *row++ = color;
            }
            return;
        }
        if (alpha == 0) return;
        for (int y = y0; y < y1; y++) {
            int span_x0 = 0, span_x1 = 0;
            size_t index = 0;
            bool rounded = false;
            if (!raster_target_span(target, y, x0, x1, &span_x0, &span_x1,
                                    &index, &rounded)) continue;
            /* Square corners keep the pre-antialiasing inner loop: coverage
               is 4/4 everywhere and (alpha * 4 + 2) / 4 is alpha, so the
               shared body would pay a branch and a multiply-divide per
               pixel to arrive back at the value it started with. Output is
               bit-identical either way. */
            if (!has_radius) {
                for (int x = span_x0; x < span_x1; x++, index++) {
                    if (rounded
                        && !point_in_rounded_clips(
                               target->rounded_clips,
                               target->rounded_clip_count, x, y)) continue;
                    blend_surface_at_index(
                        target, index, command->color, alpha);
                }
                continue;
            }
            for (int x = span_x0; x < span_x1; x++, index++) {
                unsigned coverage = rounded_rect_coverage_quarters(
                    x, y, command->x, command->y,
                    command->width, command->height, corner_radii);
                if (coverage == 0) continue;
                if (rounded
                    && !point_in_rounded_clips(target->rounded_clips,
                                               target->rounded_clip_count,
                                               x, y)) continue;
                blend_surface_at_index(
                    target, index, command->color,
                    (alpha * coverage + 2u) / 4u);
            }
        }
        return;
    }
    if (command->type == DRAW_STROKE_RECT) {
        rasterize_stroke_rect(target, command, x0, y0, x1, y1);
        return;
    }
    if (command->type == DRAW_IMAGE) {
        rasterize_image(cache, target, command);
        return;
    }
    if (font_context_face_variant(cache->layout->fonts,
                                  cache->layout->web_fonts,
                                  draw_command_font_family(command),
                                  draw_command_font_italic(command),
                                  draw_uses_bold_face(command)) != NULL) {
        if (draw_command_text_shadow_blur(command) != 0) {
            rasterize_blurred_text_shadow(cache, target, command);
        } else {
            rasterize_truetype_text(cache, target, command);
            rasterize_text_underline(cache, target, command);
        }
        return;
    }
    int fallback_scale = command->scale;
    if (fallback_scale > TILEFINCH_BITMAP_FONT_SCALE_LIMIT) {
        fallback_scale = TILEFINCH_BITMAP_FONT_SCALE_LIMIT;
    }
    if (fallback_scale <= 0) return;
    unsigned fallback_rotation = draw_command_rotation_quadrants(command);
    int fallback_source_width = (fallback_rotation & 1u) != 0
        ? command->height : command->width;
    int fallback_source_height = (fallback_rotation & 1u) != 0
        ? command->width : command->height;
    int pen_x = command->x;
    for (size_t character = 0; character < command->text_length;) {
        unsigned char glyph = '?';
        size_t advance = utf8_advance(command->text + character,
                                      command->text_length - character,
                                      &glyph);
        character += advance;
        glyph = (unsigned char) draw_command_transform_codepoint(
            command, glyph, character == advance);
        if (glyph >= sizeof(font5x7) / sizeof(font5x7[0])) glyph = '?';
        for (int row = 0; row < 7; row++) {
            uint8_t bits = font5x7[glyph][row];
            for (int column = 0; column < 5; column++) {
                if ((bits & (1u << (4 - column))) == 0) continue;
                for (int sy = 0; sy < fallback_scale; sy++) {
                    for (int sx = 0; sx < fallback_scale; sx++) {
                        int italic_shift = draw_command_font_italic(command)
                                           ? (6 - row) * fallback_scale / 5
                                           : 0;
                        unsigned alpha = 255u * command->opacity_scale / 256u;
                        if (alpha != 0) {
                            int source_x = pen_x - command->x
                                + column * fallback_scale + sx
                                + italic_shift;
                            int source_y = row * fallback_scale + sy;
                            int draw_x = source_x, draw_y = source_y;
                            if (fallback_rotation == 1u) {
                                draw_x = fallback_source_height - 1
                                    - source_y;
                                draw_y = source_x;
                            } else if (fallback_rotation == 2u) {
                                draw_x = fallback_source_width - 1
                                    - source_x;
                                draw_y = fallback_source_height - 1
                                    - source_y;
                            } else if (fallback_rotation == 3u) {
                                draw_x = source_y;
                                draw_y = fallback_source_width - 1
                                    - source_x;
                            }
                            blend_pixel(target, command->x + draw_x,
                                        command->y + draw_y,
                                        command->color, alpha);
                        }
                    }
                }
            }
        }
        pen_x += 6 * fallback_scale;
        if (character < command->text_length) {
            pen_x += command->letter_spacing;
        }
    }
    rasterize_text_underline(cache, target, command);
}
