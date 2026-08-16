static void experimental_print_store_timing(
    const CompressedSectionStore *store)
{
    printf("experimental-store-timing compression-us=%llu index-us=%llu "
           "progress-callback-us=%llu selector-bloom-us=%llu "
           "anchor-sort-us=%llu blocks=%zu "
           "stored-bytes=%zu anchor-bytes=%zu index-work=%zu "
           "index-yields=%zu index-max-slice-us=%llu "
           "first-section-ready-us=%llu selector-index-work=%zu "
           "selector-index-yields=%zu selector-index-max-slice-us=%llu\n",
           (unsigned long long) store->compression_us,
           (unsigned long long) store->index_us,
           (unsigned long long) store->progress_callback_us,
           (unsigned long long) store->selector_bloom_us,
           (unsigned long long) store->anchor_sort_us,
           store->block_count, store->stored_length,
           store->anchor_count * sizeof(*store->anchors),
           store->index_work_units, store->index_cooperative_yields,
           (unsigned long long) store->index_max_slice_us,
           (unsigned long long) store->first_section_ready_us,
           store->selector_index_work_units,
           store->selector_index_cooperative_yields,
           (unsigned long long) store->selector_index_max_slice_us);
}

static bool host_platform_read_asset(void *opaque, Budget *budget,
                                     const char *path, size_t maximum_bytes,
                                     unsigned char **data, size_t *length)
{
    HostPlatformSimulator *simulator = opaque;
    FILE *file = fopen(path, "rb");
    if (file == NULL || fseek(file, 0, SEEK_END) != 0) {
        if (file != NULL) fclose(file);
        return false;
    }
    long end = ftell(file);
    if (end < 0 || (size_t) end > maximum_bytes
        || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }
    unsigned char *contents = budget_malloc_category(
        budget, BUDGET_CATEGORY_RESOURCE, (size_t) end + 1);
    if (contents == NULL) { fclose(file); return false; }
    size_t received = fread(contents, 1, (size_t) end, file);
    bool ok = received == (size_t) end && ferror(file) == 0;
    fclose(file);
    if (!ok) {
        budget_free(budget, contents);
        return false;
    }
    contents[received] = '\0';
    *data = contents;
    *length = received;
    simulator->asset_reads++;
    return true;
}

static bool host_platform_poll_input(void *opaque, TilefinchPlatformInput *input)
{
    HostPlatformSimulator *simulator = opaque;
    simulator->input_polls++;
    if (!simulator->input_pending) return false;
    *input = simulator->pending_input;
    simulator->input_pending = false;
    return true;
}

static bool host_platform_present(void *opaque, const uint16_t *pixels,
                                  size_t width, size_t height, size_t stride)
{
    HostPlatformSimulator *simulator = opaque;
    if (width != 480 || height != 272 || stride < width) return false;
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t y = 0; y < height; y++) {
        for (size_t x = 0; x < width; x++) {
            uint16_t pixel = pixels[y * stride + x];
            hash ^= pixel & 0xffu;
            hash *= UINT64_C(1099511628211);
            hash ^= pixel >> 8;
            hash *= UINT64_C(1099511628211);
        }
    }
    if (simulator->frames_presented == 0
        && simulator->first_frame_output != NULL) {
        simulator->first_frame_written = render_write_frame_ppm(
            simulator->first_frame_output, pixels, (int) width, (int) height);
        if (!simulator->first_frame_written) return false;
    }
    simulator->last_frame_hash = hash;
    simulator->frames_presented++;
    return true;
}

static bool host_platform_button(uint32_t button)
{
    if (host_platform_simulator == NULL) return true;
    host_platform_simulator->pending_input = (TilefinchPlatformInput) {
        .buttons = button
    };
    host_platform_simulator->input_pending = true;
    TilefinchPlatformInput input = {0};
    return tilefinch_platform_poll_input(&input) && input.buttons == button;
}

static bool text_metrics_write_json_string(FILE *file, const char *text,
                                           size_t length)
{
    if (file == NULL || (text == NULL && length != 0) || fputc('"', file) == EOF) {
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        unsigned char byte = (unsigned char) text[i];
        if (byte == '"' || byte == '\\') {
            if (fputc('\\', file) == EOF || fputc(byte, file) == EOF) {
                return false;
            }
        } else if (byte == '\b' || byte == '\f' || byte == '\n'
                   || byte == '\r' || byte == '\t') {
            const char escaped = byte == '\b' ? 'b'
                : (byte == '\f' ? 'f' : (byte == '\n' ? 'n'
                   : (byte == '\r' ? 'r' : 't')));
            if (fputc('\\', file) == EOF || fputc(escaped, file) == EOF) {
                return false;
            }
        } else if (byte < 0x20) {
            if (fprintf(file, "\\u%04x", (unsigned) byte) < 0) return false;
        } else if (fputc(byte, file) == EOF) {
            return false;
        }
    }
    return fputc('"', file) != EOF;
}

static const char *text_metrics_backend_name(const FontFace *face)
{
    if (face == NULL) return "bitmap-fallback";
    if (face->backend == FONT_FACE_BACKEND_FREETYPE_WEB) return "freetype-web";
    if (face->backend == FONT_FACE_BACKEND_STB_TRUSTED) return "stb-trusted";
    return "none";
}

static int text_metrics_advance_fixed(const LayoutDocument *layout,
                                      const DrawCommand *command,
                                      const FontFace *face,
                                      bool synthetic_bold)
{
    if (layout == NULL || command == NULL || face == NULL) return -1;
    FontFamily metric_family = font_context_metric_family(
        layout->web_fonts, draw_command_font_family(command), face);
    int advance = font_text_width_for_family_at_size_fixed(
        face, metric_family, command->text, command->text_length,
        draw_command_text_font_size_fixed(command), synthetic_bold,
        draw_command_font_weight_code(command) >= 65);
    if (advance < 0) return advance;
    size_t characters = 0;
    for (size_t at = 0; at < command->text_length;) {
        unsigned codepoint = 0;
        size_t used = font_utf8_next(
            command->text + at, command->text_length - at, &codepoint);
        if (used == 0) break;
        characters++;
        at += used;
    }
    int64_t spaced = advance;
    if (characters > 1) {
        spaced += (int64_t) command->letter_spacing * 64
                  * (int64_t) (characters - 1);
    }
    return spaced > INT_MAX ? INT_MAX
           : (spaced < INT_MIN ? INT_MIN : (int) spaced);
}

static bool text_metrics_command_eligible(const LayoutDocument *layout,
                                          const DrawCommand *command)
{
    if (layout == NULL || command == NULL || command->type != DRAW_TEXT
        || command->text == NULL || command->text_length == 0
        || command->width <= 0 || command->height <= 0) return false;
    int64_t right = (int64_t) command->x + command->width;
    return right > 0 && command->x < layout->viewport.css_width;
}

static bool dump_text_metrics(const char *path, const LayoutDocument *layout)
{
    if (path == NULL) return true;
    if (layout == NULL) return false;
    FILE *file = fopen(path, "w");
    if (file == NULL) return false;
    size_t eligible = 0, written = 0, skipped_long = 0;
    for (size_t i = 0; i < layout->count; i++) {
        const DrawCommand *command = &layout->commands[i];
        if (!text_metrics_command_eligible(layout, command)) continue;
        eligible++;
        if (command->text_length > TEXT_METRICS_TEXT_LIMIT) skipped_long++;
    }
    bool truncated = eligible > TEXT_METRICS_RUN_LIMIT || skipped_long != 0;
    bool ok = fprintf(
        file,
        "{\n  \"schema\": \"tilefinch-text-metrics-v1\",\n"
        "  \"source\": \"tilefinch\",\n"
        "  \"viewport\": {\"width\": %d, \"height\": %d},\n"
        "  \"document_height\": %d,\n"
        "  \"eligible_runs\": %zu,\n"
        "  \"truncated\": %s,\n"
        "  \"runs\": [\n",
        layout->viewport.css_width, layout->viewport.css_height,
        layout->height, eligible, truncated ? "true" : "false") >= 0;
    for (size_t i = 0; ok && i < layout->count
         && written < TEXT_METRICS_RUN_LIMIT; i++) {
        const DrawCommand *command = &layout->commands[i];
        if (!text_metrics_command_eligible(layout, command)
            || command->text_length > TEXT_METRICS_TEXT_LIMIT) continue;
        uint8_t weight_code = draw_command_font_weight_code(command);
        unsigned weight = weight_code == 0
            ? 400u : (unsigned) weight_code * 10u;
        bool bold_face = weight >= 650u;
        const FontFace *face = font_context_face_variant(
            layout->fonts, layout->web_fonts,
            draw_command_font_family(command),
            draw_command_font_italic(command), bold_face);
        bool synthetic_bold = weight >= 550u
            && (weight < 650u
                || !font_context_face_is_bold(
                    layout->fonts, layout->web_fonts, face));
        int advance_fixed = text_metrics_advance_fixed(
            layout, command, face, synthetic_bold);
        int line_baseline = font_line_baseline_at_size(
            face, draw_command_text_font_size_fixed(command),
            command->height);
        int64_t baseline = line_baseline < 0 ? -1
            : (int64_t) command->y + line_baseline;
        if (written != 0 && fputs(",\n", file) == EOF) ok = false;
        if (!ok || fprintf(
                file,
                "    {\"index\": %zu, \"text\": ", i) < 0
            || !text_metrics_write_json_string(
                file, command->text, command->text_length)
            || fprintf(
                file,
                ", \"x_26_6\": %d, \"y\": %d, "
                "\"advance_26_6\": %d, \"coverage_width\": %d, "
                "\"line_height\": %d, \"baseline\": %lld, "
                "\"font_size\": %d, \"font_size_26_6\": %d, "
                "\"font_family\": %u, "
                "\"font_weight\": %u, \"font_italic\": %s, "
                "\"letter_spacing\": %d, \"face_backend\": \"%s\"}",
                draw_command_text_x_fixed(command), command->y,
                advance_fixed, command->width, command->height,
                (long long) baseline, command->font_size,
                draw_command_text_font_size_fixed(command),
                (unsigned) draw_command_font_family(command), weight,
                draw_command_font_italic(command) ? "true" : "false",
                command->letter_spacing, text_metrics_backend_name(face)) < 0) {
            ok = false;
            break;
        }
        written++;
    }
    if (ok) {
        ok = fprintf(file, "\n  ],\n  \"run_count\": %zu\n}\n", written) >= 0;
    }
    if (fclose(file) != 0) ok = false;
    if (!ok) remove(path);
    return ok;
}
