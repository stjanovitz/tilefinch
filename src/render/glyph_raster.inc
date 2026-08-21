static void glyph_cache_evict(TileCache *cache, size_t index)
{
    GlyphCacheEntry *entry = &cache->glyph_cache[index];
    if (!entry->valid) return;
    font_glyph_destroy(entry->face, &entry->glyph);
    cache->glyph_cache_bytes = cache->glyph_cache_bytes >= entry->bytes
                               ? cache->glyph_cache_bytes - entry->bytes : 0;
    cache->glyph_cache_count--;
    cache->glyph_cache_evictions++;
    memset(entry, 0, sizeof(*entry));
}

static void glyph_cache_clear(TileCache *cache, bool count_evictions)
{
    size_t evictions = cache->glyph_cache_evictions;
    for (size_t i = 0; i < cache->glyph_cache_capacity; i++) {
        glyph_cache_evict(cache, i);
    }
    if (!count_evictions) cache->glyph_cache_evictions = evictions;
}

static size_t glyph_cache_set(const TileCache *cache, const FontFace *face,
                              unsigned codepoint, int pixel_height, bool bold)
{
    (void) face;
    /*
     * Keep set placement independent of allocator addresses.  The full face
     * pointer remains part of the equality key, while address-independent set
     * selection makes collision behavior and PSP frame cost reproducible.
     */
    uint64_t hash = ((uint64_t) codepoint
                     * UINT64_C(0x9e3779b185ebca87))
                    ^ ((uint64_t) (unsigned) pixel_height
                       * UINT64_C(0xc2b2ae3d27d4eb4f))
                    ^ (bold ? UINT64_C(0x165667b19e3779f9) : 0);
    hash ^= hash >> 33;
    hash *= UINT64_C(0xff51afd7ed558ccd);
    hash ^= hash >> 33;
    size_t set_count = cache->glyph_cache_capacity
                       / TILEFINCH_GLYPH_CACHE_WAYS;
    return set_count == 0 ? 0 : (size_t) hash & (set_count - 1u);
}

static size_t glyph_cache_lru(const TileCache *cache, size_t begin,
                              size_t end)
{
    size_t victim = cache->glyph_cache_capacity;
    for (size_t i = begin; i < end; i++) {
        if (!cache->glyph_cache[i].valid) return i;
        if (victim == cache->glyph_cache_capacity
            || cache->glyph_cache[i].last_used
               < cache->glyph_cache[victim].last_used) victim = i;
    }
    return victim;
}

static size_t glyph_cache_oldest_valid(const TileCache *cache)
{
    size_t victim = cache->glyph_cache_capacity;
    for (size_t i = 0; i < cache->glyph_cache_capacity; i++) {
        if (!cache->glyph_cache[i].valid) continue;
        if (victim == cache->glyph_cache_capacity
            || cache->glyph_cache[i].last_used
               < cache->glyph_cache[victim].last_used) victim = i;
    }
    return victim;
}

static bool glyph_prefilter(TileCache *cache, const FontFace *face,
                            FontGlyph *glyph)
{
    if (glyph->colors != NULL) return true;
    if (glyph->pixels == NULL || glyph->width <= 0 || glyph->height <= 0) {
        return true;
    }
    /*
     * Preserve exact coverage for solid rectangular outline glyphs.  The
     * general-purpose filter below deliberately grows glyph coverage by one
     * pixel, which improves ordinary text but turns a fully covered bitmap
     * (including CSS test fonts and icon-font rectangles) into a translucent
     * skirt.  Returning true tells the rasterizer that this bitmap is already
     * in its final, directly consumable form.
     */
    size_t source_pixels = (size_t) glyph->width * (size_t) glyph->height;
    bool exact_coverage = true;
    for (size_t i = 0; i < source_pixels; i++) {
        if (glyph->pixels[i] != 255u) {
            exact_coverage = false;
            break;
        }
    }
    if (exact_coverage) return true;
    size_t width = (size_t) glyph->width + 2u;
    size_t height = (size_t) glyph->height + 2u;
    if (height > SIZE_MAX / width
        || width * height > TILEFINCH_GLYPH_CACHE_BYTES) return false;
    unsigned char *pixels = budget_malloc(cache->budget, width * height);
    if (pixels == NULL) return false;
    for (int row = -1; row <= glyph->height; row++) {
        for (int column = -1; column <= glyph->width; column++) {
            /* Measured against the Chrome oracle: this 8/12 kernel scores
               best on SSIM; raw coverage renders thinner than Chrome and
               a lighter 12/16 kernel was a wash (docs/FIDELITY.md). */
            unsigned center = row >= 0 && row < glyph->height
                              && column >= 0 && column < glyph->width
                ? glyph->pixels[row * glyph->width + column] : 0;
            unsigned north = row > 0 && row <= glyph->height
                             && column >= 0 && column < glyph->width
                ? glyph->pixels[(row - 1) * glyph->width + column] : 0;
            unsigned south = row >= -1 && row + 1 < glyph->height
                             && column >= 0 && column < glyph->width
                ? glyph->pixels[(row + 1) * glyph->width + column] : 0;
            unsigned west = column > 0 && column <= glyph->width
                            && row >= 0 && row < glyph->height
                ? glyph->pixels[row * glyph->width + column - 1] : 0;
            unsigned east = column >= -1 && column + 1 < glyph->width
                            && row >= 0 && row < glyph->height
                ? glyph->pixels[row * glyph->width + column + 1] : 0;
            pixels[(size_t) (row + 1) * width + (size_t) (column + 1)] =
                (unsigned char) ((center * 8u + north + south + west + east)
                                 / 12u);
        }
    }
    int old_width = glyph->width;
    int old_height = glyph->height;
    int old_x_offset = glyph->x_offset;
    int old_y_offset = glyph->y_offset;
    int old_advance = glyph->advance;
    int old_advance_fixed = glyph->advance_fixed;
    bool old_provider_pending = glyph->provider_pending;
    font_glyph_destroy(face, glyph);
    *glyph = (FontGlyph) {
        .pixels = pixels,
        .budget = cache->budget,
        .width = old_width + 2,
        .height = old_height + 2,
        .x_offset = old_x_offset - 1,
        .y_offset = old_y_offset - 1,
        .advance = old_advance,
        .advance_fixed = old_advance_fixed,
        .provider_pending = old_provider_pending
    };
    return true;
}

static const FontGlyph *glyph_cache_acquire(TileCache *cache,
                                            const FontFace *face,
                                            unsigned codepoint,
                                            int pixel_height, bool bold,
                                            FontGlyph *temporary,
                                            bool *smoothed)
{
    *smoothed = false;
    size_t set = glyph_cache_set(cache, face, codepoint, pixel_height, bold);
    size_t begin = set * TILEFINCH_GLYPH_CACHE_WAYS;
    size_t end = begin + TILEFINCH_GLYPH_CACHE_WAYS;
    if (end > cache->glyph_cache_capacity) end = cache->glyph_cache_capacity;
    for (size_t i = begin; i < end; i++) {
        GlyphCacheEntry *entry = &cache->glyph_cache[i];
        if (entry->valid && entry->face == face
            && entry->codepoint == codepoint
            && entry->pixel_height == pixel_height && entry->bold == bold) {
            entry->last_used = ++cache->clock;
            cache->glyph_cache_hits++;
            *smoothed = entry->smoothed;
            return &entry->glyph;
        }
    }
    cache->glyph_cache_misses++;
    if (!font_glyph_load_at_size(
            face, codepoint, pixel_height, bold, temporary)) {
        return NULL;
    }
    /* A first useful viewport values latency over the final expansion
       filter. FreeType coverage is already antialiased; treating it as
       ready avoids an allocation and a five-tap pass for every cold glyph.
       The authoritative renderer retains the normal fidelity path. */
    *smoothed = cache->fast_text_raster
        || glyph_prefilter(cache, face, temporary);
    size_t bytes = budget_usable_size(temporary->pixels);
    size_t color_bytes = budget_usable_size(temporary->colors);
    if (bytes <= SIZE_MAX - color_bytes) bytes += color_bytes;
    else bytes = SIZE_MAX;
    if (temporary->provider_pending) return temporary;
    if (bytes > TILEFINCH_GLYPH_CACHE_BYTES
        || cache->glyph_cache_capacity < TILEFINCH_GLYPH_CACHE_WAYS) {
        return temporary;
    }
    while ((cache->glyph_cache_bytes > TILEFINCH_GLYPH_CACHE_BYTES
            || bytes > TILEFINCH_GLYPH_CACHE_BYTES
                       - cache->glyph_cache_bytes)
           && cache->glyph_cache_count != 0) {
        size_t victim = glyph_cache_oldest_valid(cache);
        if (victim == cache->glyph_cache_capacity) break;
        glyph_cache_evict(cache, victim);
    }
    if (cache->glyph_cache_bytes > TILEFINCH_GLYPH_CACHE_BYTES
        || bytes > TILEFINCH_GLYPH_CACHE_BYTES - cache->glyph_cache_bytes) {
        return temporary;
    }
    size_t slot = glyph_cache_lru(cache, begin, end);
    if (slot == cache->glyph_cache_capacity) return temporary;
    if (cache->glyph_cache[slot].valid) glyph_cache_evict(cache, slot);
    GlyphCacheEntry *entry = &cache->glyph_cache[slot];
    entry->face = face;
    entry->codepoint = codepoint;
    entry->pixel_height = pixel_height;
    entry->bold = bold;
    entry->valid = true;
    entry->smoothed = *smoothed;
    entry->last_used = ++cache->clock;
    entry->bytes = bytes;
    entry->glyph = *temporary;
    memset(temporary, 0, sizeof(*temporary));
    cache->glyph_cache_count++;
    cache->glyph_cache_bytes += bytes;
    return &entry->glyph;
}

static uint32_t authored_glyph_color(const FontGlyph *glyph, size_t at)
{
    uint16_t packed = glyph->colors[at];
    return (tilefinch_rgb5_to_u8(
                tilefinch_rgb565_red_code(packed)) << 16)
        | (tilefinch_rgb6_to_u8(
               tilefinch_rgb565_green_code(packed)) << 8)
        | tilefinch_rgb5_to_u8(tilefinch_rgb565_blue_code(packed));
}

/* Color emoji are image-like authored pixels. CSS filters still apply, but
   forced-dark page remapping must not invert or recolor them. */
static void blend_authored_glyph_at_index(
    RasterTarget *target, size_t index, uint32_t color, unsigned alpha)
{
    bool forced_dark = target->forced_dark;
    bool resolved = target->resolved_color_valid;
    target->forced_dark = false;
    target->resolved_color_valid = false;
    blend_at_index(target, index, color, alpha);
    target->resolved_color_valid = resolved;
    target->forced_dark = forced_dark;
}

static void blend_authored_glyph_pixel(
    RasterTarget *target, int x, int y, uint32_t color, unsigned alpha)
{
    size_t index = 0;
    if (!raster_target_index(target, x, y, &index)) return;
    blend_authored_glyph_at_index(target, index, color, alpha);
}

static void rasterize_truetype_text(TileCache *cache, RasterTarget *target,
                                    const DrawCommand *command)
{
    bool bold_face = draw_uses_bold_face(command);
    const FontFace *face = font_context_face_variant(
        cache->layout->fonts, cache->layout->web_fonts,
        draw_command_font_family(command), draw_command_font_italic(command),
        bold_face);
    if (face == NULL) return;
    bool synthetic_italic = draw_command_font_italic(command)
        && face == font_context_face_variant(
            cache->layout->fonts, cache->layout->web_fonts,
            draw_command_font_family(command), false, bold_face);
    unsigned weight = draw_font_weight(command);
    bool synthetic_bold = weight >= 550u
        && (weight < 650u
            || !font_context_face_is_bold(cache->layout->fonts,
                                          cache->layout->web_fonts, face));
    bool metric_bold = bold_face;
    FontFamily metric_family = font_context_metric_family(
        cache->layout->web_fonts, draw_command_font_family(command), face);
    int pixel_height_fixed = draw_command_text_font_size_fixed(command);
    if (pixel_height_fixed > TILEFINCH_FONT_RASTER_PIXEL_LIMIT * 64) {
        pixel_height_fixed = TILEFINCH_FONT_RASTER_PIXEL_LIMIT * 64;
    }
    if (pixel_height_fixed <= 0) return;
    unsigned rotation = draw_command_rotation_quadrants(command);
    int source_width = (rotation & 1u) != 0
        ? command->height : command->width;
    int source_height = (rotation & 1u) != 0
        ? command->width : command->height;
    int pen_x_fixed = draw_command_text_x_fixed(command);
    int64_t baseline_wide = (int64_t) command->y
        + font_line_baseline_at_size(
            face, pixel_height_fixed, source_height);
    int baseline = baseline_wide > INT_MAX ? INT_MAX
                   : (baseline_wide < INT_MIN ? INT_MIN
                                               : (int) baseline_wide);
    unsigned previous = 0;
    bool word_start = true;
    bool rtl = draw_command_text_rtl(command);
    size_t rtl_number_at = 0, rtl_number_end = 0;
    for (size_t at = rtl ? command->text_length : 0;
         rtl ? (at > 0 || rtl_number_at < rtl_number_end)
             : at < command->text_length;) {
        size_t character_at = at;
        if (rtl && rtl_number_at < rtl_number_end) {
            character_at = rtl_number_at;
        } else if (rtl) {
            character_at--;
            while (character_at > 0
                   && (((unsigned char) command->text[character_at]
                        & 0xc0u) == 0x80u)) {
                character_at--;
            }
            if ((unsigned char) command->text[character_at] >= '0'
                && (unsigned char) command->text[character_at] <= '9') {
                size_t start = character_at;
                while (start > 0
                       && (unsigned char) command->text[start - 1] >= '0'
                       && (unsigned char) command->text[start - 1] <= '9') {
                    start--;
                }
                rtl_number_at = start;
                rtl_number_end = at;
                at = start;
                continue;
            }
        }
        unsigned codepoint = 0;
        size_t used = 0;
        bool provider_sequence = false;
        if (!rtl) {
            provider_sequence = font_optional_glyph_match_sequence(
                command->text + character_at,
                command->text_length - character_at, &used, &codepoint);
        } else if (!(rtl_number_at < rtl_number_end)) {
            size_t sequence_start = 0;
            provider_sequence = font_optional_glyph_match_sequence_ending_at(
                command->text, at, &sequence_start, &codepoint);
            if (provider_sequence) {
                character_at = sequence_start;
                used = at - sequence_start;
            }
        }
        if (!provider_sequence)
            used = font_utf8_next(command->text + character_at,
                                  command->text_length - character_at,
                                  &codepoint);
        if (used == 0) break;
        if (!provider_sequence
            && font_codepoint_default_ignorable(codepoint)) {
            if (rtl_number_at < rtl_number_end) {
                rtl_number_at = character_at + used;
                if (rtl_number_at >= rtl_number_end) {
                    rtl_number_at = rtl_number_end = 0;
                }
            } else {
                at = rtl ? character_at : character_at + used;
            }
            continue;
        }
        if (!provider_sequence)
            codepoint = draw_command_transform_codepoint(
                command, codepoint, word_start);
        word_start = false;
        if (previous != 0) {
            pen_x_fixed += command->letter_spacing * 64;
        }
        if (!provider_sequence && !draw_command_text_kerning_none(command))
            pen_x_fixed += font_kerning_at_size_fixed(
                face, previous, codepoint, pixel_height_fixed);
        FontGlyph temporary;
        memset(&temporary, 0, sizeof(temporary));
        bool smoothed = false;
        const FontGlyph *glyph = glyph_cache_acquire(
            cache, face, codepoint, pixel_height_fixed, synthetic_bold,
            &temporary, &smoothed);
        if (glyph == NULL) break;
        int first_row = smoothed ? 0 : -1;
        int last_row = smoothed ? glyph->height - 1 : glyph->height;
        int first_column = smoothed ? 0 : -1;
        int last_column = smoothed ? glyph->width - 1 : glyph->width;
        int pen_x = (pen_x_fixed + 32) / 64;
        for (int row = first_row; row <= last_row; row++) {
            int64_t glyph_y = (int64_t) baseline + glyph->y_offset + row;
            if (rotation == 0
                && (glyph_y < target->top || glyph_y >= target->bottom)) {
                continue;
            }
            int y = (int) glyph_y;
            int italic_distance = smoothed
                ? glyph->height - row - 1 : glyph->height - row;
            /* CSS oblique leans the glyph forward: upper rows move toward
               the positive inline direction. */
            int italic_shift = synthetic_italic ? italic_distance / 7 : 0;
            unsigned italic_fraction = synthetic_italic
                ? (unsigned) italic_distance % 7u : 0u;
            int row_base = pen_x + glyph->x_offset + italic_shift;
            int span_x0 = 0, span_x1 = 0;
            size_t row_index = 0;
            bool rounded = false;
            if (rotation == 0
                && !raster_target_span(
                    target, y,
                    row_base + first_column,
                    row_base + last_column + (synthetic_bold ? 2 : 1)
                        + (italic_fraction != 0u ? 1 : 0),
                    &span_x0, &span_x1, &row_index, &rounded)) continue;
            for (int column = first_column; column <= last_column; column++) {
                unsigned alpha = 0;
                size_t glyph_at = 0;
                bool authored_color = glyph->colors != NULL
                    && row >= 0 && row < glyph->height
                    && column >= 0 && column < glyph->width;
                if (authored_color)
                    glyph_at = (size_t) row * (size_t) glyph->width
                        + (size_t) column;
                if (smoothed) {
                    alpha = glyph->pixels[row * glyph->width + column];
                } else {
                    unsigned center = row >= 0 && row < glyph->height
                                      && column >= 0
                                      && column < glyph->width
                        ? glyph->pixels[row * glyph->width + column] : 0;
                    unsigned north = row > 0 && row <= glyph->height
                                     && column >= 0
                                     && column < glyph->width
                        ? glyph->pixels[(row - 1) * glyph->width + column] : 0;
                    unsigned south = row >= -1 && row + 1 < glyph->height
                                     && column >= 0
                                     && column < glyph->width
                        ? glyph->pixels[(row + 1) * glyph->width + column] : 0;
                    unsigned west = column > 0 && column <= glyph->width
                                    && row >= 0 && row < glyph->height
                        ? glyph->pixels[row * glyph->width + column - 1] : 0;
                    unsigned east = column >= -1
                                    && column + 1 < glyph->width
                                    && row >= 0 && row < glyph->height
                        ? glyph->pixels[row * glyph->width + column + 1] : 0;
                    alpha = (center * 8u + north + south + west + east) / 12u;
                }
                alpha = alpha * command->opacity_scale / 256u;
                if (alpha == 0) continue;
                unsigned secondary_alpha = italic_fraction == 0u ? 0u
                    : (alpha * italic_fraction + 3u) / 7u;
                unsigned primary_alpha = alpha - secondary_alpha;
                for (unsigned sample = 0;
                     sample < (secondary_alpha != 0u ? 2u : 1u); sample++) {
                    unsigned sample_alpha = sample == 0u
                        ? primary_alpha : secondary_alpha;
                    if (sample_alpha == 0u) continue;
                    int x = row_base + column + (sample == 0u ? 0 : 1);
                    if (rotation != 0) {
                        int local_x = x - command->x;
                        int local_y = y - command->y;
                        int draw_x = local_x, draw_y = local_y;
                        if (rotation == 1u) {
                            draw_x = source_height - 1 - local_y;
                            draw_y = local_x;
                        } else if (rotation == 2u) {
                            draw_x = source_width - 1 - local_x;
                            draw_y = source_height - 1 - local_y;
                        } else {
                            draw_x = local_y;
                            draw_y = source_width - 1 - local_x;
                        }
                        uint32_t glyph_color = authored_color
                            ? authored_glyph_color(glyph, glyph_at)
                            : command->color;
                        if (authored_color)
                            blend_authored_glyph_pixel(
                                target, command->x + draw_x,
                                command->y + draw_y,
                                glyph_color, sample_alpha);
                        else
                            blend_pixel(target, command->x + draw_x,
                                        command->y + draw_y,
                                        glyph_color, sample_alpha);
                        if (synthetic_bold && !authored_color
                            && sample_alpha >= 2u) {
                            /* Synthetic bold advances in the source inline
                               direction, then the quarter-turn maps that
                               offset with the glyph. */
                            local_x++;
                            if (rotation == 1u) {
                                draw_x = source_height - 1 - local_y;
                                draw_y = local_x;
                            } else if (rotation == 2u) {
                                draw_x = source_width - 1 - local_x;
                                draw_y = source_height - 1 - local_y;
                            } else {
                                draw_x = local_y;
                                draw_y = source_width - 1 - local_x;
                            }
                            blend_pixel(target, command->x + draw_x,
                                        command->y + draw_y,
                                        glyph_color, sample_alpha / 2u);
                        }
                    } else if (x >= span_x0 && x < span_x1
                        && (!rounded
                            || point_in_rounded_clips(
                                   target->rounded_clips,
                                   target->rounded_clip_count, x, y))) {
                        uint32_t glyph_color = command->color;
                        if (authored_color) {
                            glyph_color = authored_glyph_color(
                                glyph, glyph_at);
                            blend_authored_glyph_at_index(
                                target, row_index + (size_t) (x - span_x0),
                                glyph_color, sample_alpha);
                        } else {
                            blend_at_index(
                                target, row_index + (size_t) (x - span_x0),
                                glyph_color, sample_alpha);
                        }
                    }
                    if (rotation == 0 && synthetic_bold && !authored_color) {
                        int bold_x = x + 1;
                        unsigned bold_alpha = sample_alpha / 2u;
                        if (bold_alpha != 0 && bold_x >= span_x0
                            && bold_x < span_x1
                            && (!rounded
                                || point_in_rounded_clips(
                                       target->rounded_clips,
                                       target->rounded_clip_count,
                                       bold_x, y))) {
                            blend_at_index(
                                target,
                                row_index + (size_t) (bold_x - span_x0),
                                command->color, bold_alpha);
                        }
                    }
                }
            }
        }
        pen_x_fixed += font_glyph_advance_for_family_at_size_fixed(
            metric_family, codepoint, pixel_height_fixed,
            synthetic_bold, metric_bold, glyph->advance_fixed);
        previous = provider_sequence ? 0 : codepoint;
        font_glyph_destroy(face, &temporary);
        if (rtl_number_at < rtl_number_end) {
            rtl_number_at = character_at + used;
            if (rtl_number_at >= rtl_number_end) {
                rtl_number_at = rtl_number_end = 0;
            }
        } else {
            at = rtl ? character_at : character_at + used;
        }
    }
}

static void rasterize_text_underline(TileCache *cache, RasterTarget *target,
                                     const DrawCommand *command)
{
    if (!draw_command_text_has_underline(command)
        || command->width <= 0 || command->height <= 0) return;
    const FontFace *face = font_context_face_variant(
        cache->layout->fonts, cache->layout->web_fonts,
        draw_command_font_family(command), draw_command_font_italic(command),
        draw_uses_bold_face(command));
    int pixel_height_fixed = draw_command_text_font_size_fixed(command);
    if (pixel_height_fixed > TILEFINCH_FONT_RASTER_PIXEL_LIMIT * 64) {
        pixel_height_fixed = TILEFINCH_FONT_RASTER_PIXEL_LIMIT * 64;
    }
    if (pixel_height_fixed <= 0) return;
    unsigned rotation = draw_command_rotation_quadrants(command);
    int source_width = (rotation & 1u) != 0
        ? command->height : command->width;
    int source_height = (rotation & 1u) != 0
        ? command->width : command->height;
    int baseline = font_line_baseline_at_size(
        face, pixel_height_fixed, source_height);
    int thickness = pixel_height_fixed >= 32 * 64 ? 2 : 1;
    int explicit_offset = 0;
    bool has_explicit_offset = draw_command_text_underline_offset(
        command, &explicit_offset);
    int64_t offset_wide;
    if (has_explicit_offset) {
        /* CSS lengths are measured from the underline zero position (the
           alphabetic baseline for the supported horizontal auto position),
           not from the UA-chosen automatic underline location. */
        offset_wide = (baseline > 0
                       ? (int64_t) baseline
                       : (int64_t) source_height - thickness)
                      + explicit_offset;
    } else {
        offset_wide = baseline > 0
            ? (int64_t) baseline + (pixel_height_fixed >= 24 * 64 ? 2 : 1)
            : (int64_t) source_height - thickness;
    }
    int offset = offset_wide > INT_MAX ? INT_MAX
                 : (offset_wide < INT_MIN ? INT_MIN : (int) offset_wide);
    if (!has_explicit_offset) {
        if ((int64_t) offset + thickness > source_height) {
            offset = source_height - thickness;
        }
        if (offset < 0) offset = 0;
    }
    int64_t y0_wide = (int64_t) command->y + offset;
    int64_t y1_wide = y0_wide + thickness;
    int64_t x0_wide = command->x;
    int64_t command_right = (int64_t) command->x + source_width;
    if (rotation != 0) {
        unsigned alpha = 255u * command->opacity_scale / 256u;
        for (int source_y = offset;
             alpha != 0 && source_y < offset + thickness; source_y++) {
            for (int source_x = 0; source_x < source_width; source_x++) {
                int draw_x = source_x, draw_y = source_y;
                if (rotation == 1u) {
                    draw_x = source_height - 1 - source_y;
                    draw_y = source_x;
                } else if (rotation == 2u) {
                    draw_x = source_width - 1 - source_x;
                    draw_y = source_height - 1 - source_y;
                } else {
                    draw_x = source_y;
                    draw_y = source_width - 1 - source_x;
                }
                blend_pixel(target, command->x + draw_x,
                            command->y + draw_y, command->color, alpha);
            }
        }
        return;
    }
    if (y1_wide <= target->top || y0_wide >= target->bottom
        || command_right <= target->left || x0_wide >= target->right) {
        return;
    }
    int y0 = y0_wide > target->top ? (int) y0_wide : target->top;
    int y1 = y1_wide < target->bottom ? (int) y1_wide : target->bottom;
    int x0 = x0_wide > target->left ? (int) x0_wide : target->left;
    int x1 = command_right < target->right
             ? (int) command_right : target->right;
    unsigned alpha = 255u * command->opacity_scale / 256u;
    if (alpha == 0) return;
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
            blend_at_index(target, index, command->color, alpha);
        }
    }
}

static void rasterize_blurred_text_shadow(
    TileCache *cache, RasterTarget *target, const DrawCommand *command)
{
    int blur = draw_command_text_shadow_blur(command);
    if (blur <= 0) {
        rasterize_truetype_text(cache, target, command);
        rasterize_text_underline(cache, target, command);
        return;
    }
    /* A bounded five-tap approximation avoids a glyph-sized scratch bitmap
       and separable blur allocation on the PSP. The center carries half the
       energy; four cardinal samples retain the soft extent. Glyph cache hits
       make the uncommon shadow path deterministic and allocation-free. */
    int distance = (blur + 1) / 2;
    static const int8_t dx[5] = {0, -1, 1, 0, 0};
    static const int8_t dy[5] = {0, 0, 0, -1, 1};
    static const uint8_t weight[5] = {4, 1, 1, 1, 1};
    for (size_t i = 0; i < 5; i++) {
        DrawCommand sample = *command;
        sample.radius &= ~(LAYOUT_TEXT_SHADOW
                           | LAYOUT_TEXT_SHADOW_BLUR_MASK);
        int64_t shifted_x = (int64_t) draw_command_text_x_fixed(command)
                            + (int64_t) dx[i] * distance * 64;
        if (shifted_x > INT_MAX) shifted_x = INT_MAX;
        if (shifted_x < INT_MIN) shifted_x = INT_MIN;
        draw_command_set_text_x_fixed(
            &sample, (int) shifted_x);
        int64_t shifted_y = (int64_t) command->y + dy[i] * distance;
        sample.y = shifted_y > INT_MAX ? INT_MAX
                   : (shifted_y < INT_MIN ? INT_MIN : (int) shifted_y);
        sample.opacity_scale = (uint16_t) (
            (unsigned) command->opacity_scale * weight[i] / 8u);
        if (sample.opacity_scale == 0) continue;
        rasterize_truetype_text(cache, target, &sample);
        rasterize_text_underline(cache, target, &sample);
    }
}
