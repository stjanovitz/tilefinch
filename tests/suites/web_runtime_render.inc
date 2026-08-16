static int test_web_runtime(void)
{
    Budget budget;
    budget_init(&budget, 16 * MIB);
    budget_install_lexbor(&budget);

    puts("test: section-state restore refreshes DOM metadata and lazy caches");
    static const char section_state_page[] =
        "<!doctype html><body><div id=state-target>initial</div></body>";
    static const char section_state_long_text[] =
        "section state restoration refreshes aggregate body metadata";
    PocDocument section_state_document = {0};
    ScriptResult section_state_result = {0};
    CHECK(document_parse(&section_state_document, &budget,
                         section_state_page,
                         sizeof(section_state_page) - 1, 17));
    ScriptRuntime *section_state_runtime = script_runtime_create_deferred(
        &section_state_document, &budget, 2 * MIB, 1000,
        "https://section-state.test/", NULL, &section_state_result);
    size_t saved_section_state = 0;
    bool section_long_ok = section_state_runtime != NULL
          && script_runtime_evaluate_diagnostic(
              section_state_runtime,
              "document.getElementById('state-target').textContent="
              "'section state restoration refreshes aggregate body metadata'",
              "<section-state-long>", &section_state_result);
    bool section_save_ok = section_long_ok
          && script_runtime_save_section_state(
              section_state_runtime, 7, &saved_section_state,
              &section_state_result);
    bool section_short_ok = section_save_ok && saved_section_state == 1
          && script_runtime_evaluate_diagnostic(
              section_state_runtime,
              "document.getElementById('state-target').textContent='x'",
              "<section-state-short>", &section_state_result);
    if (!section_short_ok) {
        fprintf(stderr, "section state setup long=%d save=%d saved=%zu "
                "short=%d error=%s\n", section_long_ok, section_save_ok,
                saved_section_state, section_short_ok,
                section_state_result.error);
    }
    CHECK(section_long_ok && section_save_ok && saved_section_state == 1
          && section_short_ok);
    const char *short_section_body = document_body_text(
        &section_state_document);
    size_t short_section_bytes = section_state_document.text_bytes;
    size_t restored_section_state = 0;
    CHECK(short_section_body != NULL
          && strstr(short_section_body, "x") != NULL
          && script_runtime_restore_section_state(
              section_state_runtime, 7, &restored_section_state,
              &section_state_result)
          && restored_section_state == 1
          && section_state_document.body_text == NULL
          && section_state_document.text_bytes > short_section_bytes);
    const char *restored_section_body = document_body_text(
        &section_state_document);
    CHECK(restored_section_body != NULL
          && strstr(restored_section_body, section_state_long_text) != NULL);
    script_runtime_destroy(section_state_runtime);
    document_destroy(&section_state_document);
    CHECK(budget.current == 0);

    puts("test: bounded lazy raster downsampling");
    static const unsigned char tiny_bmp[] = {
        0x42,0x4d,0x46,0,0,0,0,0,0,0,0x36,0,0,0,
        0x28,0,0,0,2,0,0,0,2,0,0,0,1,0,0x18,0,
        0,0,0,0,0x10,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,
        0xff,0,0,0xff,0xff,0xff,0,0,
        0,0,0xff,0,0xff,0,0,0
    };
    ImageResource downsample_image = {
        .encoded = (unsigned char *) tiny_bmp,
        .encoded_length = sizeof(tiny_bmp),
        .source_width = 2,
        .source_height = 2,
        .width = 1,
        .height = 1
    };
    unsigned char *downsampled = image_resource_decode(&downsample_image,
                                                        &budget);
    CHECK(downsampled != NULL && budget_usable_size(downsampled) == 4
          && downsampled[0] == 255 && downsampled[1] == 0
          && downsampled[2] == 0 && downsampled[3] == 255);
    image_resource_free_decoded(&budget, downsampled);
    CHECK(budget.current == 0);

    puts("test: bounded decode-only WebP raster downsampling");
    /* Project-generated 2x2 lossless blue raster. Keeping the encoded sample
       tiny makes the test independent of external corpora and also exercises
       the PSP path's caller-owned, viewport-sized output buffer. */
    static const unsigned char tiny_webp[] = {
        0x52,0x49,0x46,0x46,0x1e,0,0,0,0x57,0x45,0x42,0x50,
        0x56,0x50,0x38,0x4c,0x11,0,0,0,0x2f,0x01,0x40,0,
        0,0x07,0xd0,0xb2,0xc2,0x34,0xb9,0xff,0x81,0x88,0xe8,0x7f,
        0,0
    };
    ImageResource webp_image = {
        .encoded = (unsigned char *) tiny_webp,
        .encoded_length = sizeof(tiny_webp),
        .source_width = 2,
        .source_height = 2,
        .width = 1,
        .height = 1
    };
    unsigned char *webp_pixel = image_resource_decode(&webp_image, &budget);
    CHECK(webp_pixel != NULL && budget_usable_size(webp_pixel) == 4
          && webp_pixel[2] > webp_pixel[1]
          && webp_pixel[1] > webp_pixel[0]
          && webp_pixel[3] == 255);
    image_resource_free_decoded(&budget, webp_pixel);
    CHECK(budget.current == 0);

    puts("test: layout-sized priority raster decode");
    static const char layout_sized_page[] =
        "<!doctype html><body><img id=thumb width=1 height=1 "
        "src='data:image/webp;base64,"
        "UklGRh4AAABXRUJQVlA4TBEAAAAvAUAAAAfQssI0uf+BiOh/AAA='>";
    PocDocument layout_sized_document = {0};
    Stylesheet layout_sized_stylesheet = {0};
    ImageResources layout_sized_images = {0};
    CHECK(document_parse(
              &layout_sized_document, &budget, layout_sized_page,
              sizeof(layout_sized_page) - 1, 17)
          && stylesheet_build(
              &layout_sized_stylesheet, &budget,
              &layout_sized_document, 480));
    lxb_dom_node_t *layout_sized_node = find_id(
        lxb_dom_interface_node(layout_sized_document.html), "thumb");
    ImagePriorityTarget layout_sized_target = {
        .node = layout_sized_node,
        .display_width = 1,
        .display_height = 1,
        .kind = IMAGE_PRIORITY_KIND_DOCUMENT,
        .pseudo = (uint8_t) PSEUDO_NONE
    };
    CHECK(layout_sized_node != NULL
          && images_load_external_priority_targets(
              &layout_sized_document, &layout_sized_stylesheet,
              &layout_sized_images, &layout_sized_target, 1, &budget,
              "https://layout-sized.test/",
              "https://layout-sized.test/", NULL,
              4, 64 * 1024, 32 * 1024, 128 * 1024,
              1000, NULL, NULL));
    const ImageResource *layout_sized_resource = images_find_node(
        &layout_sized_images, layout_sized_node);
    CHECK(layout_sized_resource != NULL
          && image_resource_available(layout_sized_resource)
          && layout_sized_resource->source_width == 2
          && layout_sized_resource->source_height == 2
          && layout_sized_resource->width == 1
          && layout_sized_resource->height == 1
          && layout_sized_images.stats.decoded_bytes == 4);
    images_destroy(&layout_sized_images);
    stylesheet_destroy(&layout_sized_stylesheet);
    document_destroy(&layout_sized_document);
    CHECK(budget.current == 0);

    puts("test: layout width selects the bounded srcset candidate");
    static const char layout_srcset_page[] =
        "<!doctype html><body><img id=responsive width=1 height=1 "
        "srcset=\"data:image/svg+xml,%3Csvg%20xmlns='http://www.w3.org/"
        "2000/svg'%20width='1'%20height='1'%3E%3Crect%20width='1'"
        "%20height='1'/%3E%3C/svg%3E 1w, "
        "data:image/svg+xml,%3Csvg%20xmlns='http://www.w3.org/2000/svg'"
        "%20width='2'%20height='2'%3E%3Crect%20width='2'%20height='2'"
        "/%3E%3C/svg%3E 2w\">";
    PocDocument layout_srcset_document = {0};
    Stylesheet layout_srcset_stylesheet = {0};
    ImageResources layout_srcset_images = {0};
    CHECK(document_parse(
              &layout_srcset_document, &budget, layout_srcset_page,
              sizeof(layout_srcset_page) - 1, 17)
          && stylesheet_build(
              &layout_srcset_stylesheet, &budget,
              &layout_srcset_document, 480));
    lxb_dom_node_t *layout_srcset_node = find_id(
        lxb_dom_interface_node(layout_srcset_document.html), "responsive");
    ImagePriorityTarget layout_srcset_target = {
        .node = layout_srcset_node,
        .display_width = 1,
        .display_height = 1,
        .kind = IMAGE_PRIORITY_KIND_DOCUMENT,
        .pseudo = (uint8_t) PSEUDO_NONE
    };
    CHECK(layout_srcset_node != NULL
          && images_load_external_priority_targets(
              &layout_srcset_document, &layout_srcset_stylesheet,
              &layout_srcset_images, &layout_srcset_target, 1, &budget,
              "https://layout-srcset.test/",
              "https://layout-srcset.test/", NULL,
              4, 64 * 1024, 32 * 1024, 128 * 1024,
              1000, NULL, NULL)
          && images_load_external(
              &layout_srcset_document, &layout_srcset_stylesheet,
              &layout_srcset_images, &budget,
              "https://layout-srcset.test/",
              "https://layout-srcset.test/", NULL,
              4, 64 * 1024, 32 * 1024, 128 * 1024,
              1000, NULL, NULL));
    const ImageResource *layout_srcset_resource = images_find_node(
        &layout_srcset_images, layout_srcset_node);
    CHECK(layout_srcset_resource != NULL
          && image_resource_available(layout_srcset_resource)
          && layout_srcset_resource->source_width == 1
          && layout_srcset_resource->source_height == 1);
    images_destroy(&layout_srcset_images);
    stylesheet_destroy(&layout_srcset_stylesheet);
    document_destroy(&layout_srcset_document);
    CHECK(budget.current == 0);

    puts("test: forced dark remaps page paint without touching images");
    static unsigned char forced_dark_image_pixels[] = {
        0xf0, 0x40, 0x20, 0xff, 0x20, 0xc0, 0x60, 0xff,
        0x30, 0x60, 0xe0, 0xff, 0xe0, 0xc0, 0x30, 0xff
    };
    ImageResource forced_dark_image = {
        .pixels = forced_dark_image_pixels,
        .source_width = 2, .source_height = 2,
        .width = 2, .height = 2
    };
    static const char forced_dark_text[] = "A";
    DrawCommand forced_dark_commands[] = {
        {.color=0xf5f5f5, .x=0, .y=0, .width=4, .height=4,
         .opacity_scale=256, .type=DRAW_FILL_RECT},
        {.color=0x101820, .x=0, .y=4, .width=4, .height=4,
         .opacity_scale=256, .type=DRAW_FILL_RECT},
        {.text=forced_dark_text, .text_length=1, .color=0x000000,
         .x=4, .y=0, .width=5, .height=7, .scale=1,
         .opacity_scale=256, .type=DRAW_TEXT},
        {.image=&forced_dark_image, .x=10, .y=0, .width=2, .height=2,
         .opacity_scale=256, .type=DRAW_IMAGE,
         .image_fit=LAYOUT_IMAGE_FIT_STRETCH}
    };
    LayoutDocument forced_dark_layout = {
        .budget = &budget,
        .commands = forced_dark_commands,
        .count = sizeof(forced_dark_commands)
                 / sizeof(forced_dark_commands[0]),
        .width = 16, .scroll_width = 16, .height = 8,
        .page_background = 0xffffff
    };
    uint16_t forced_dark_frame[16 * 8] = {0};
    uint16_t light_frame[16 * 8] = {0};
    TileCache forced_dark_cache = {0};
    CHECK(tile_cache_init(
              &forced_dark_cache, &budget, &forced_dark_layout, 1)
          && tile_cache_set_frame(
              &forced_dark_cache, forced_dark_frame,
              sizeof(forced_dark_frame) / sizeof(forced_dark_frame[0]))
          && tile_cache_render_frame(
              &forced_dark_cache, 0, 16, 8, NULL));
    memcpy(light_frame, forced_dark_frame, sizeof(light_frame));
    tile_cache_set_forced_dark(&forced_dark_cache, true);
    CHECK(tile_cache_render_frame(
        &forced_dark_cache, 0, 16, 8, NULL));
    /* Light page surfaces become genuinely dark, while already-dark
       authored surfaces remain stable. */
    CHECK(forced_dark_frame[0] != light_frame[0]
          && forced_dark_frame[15 + 7 * 16] != light_frame[15 + 7 * 16]
          && forced_dark_frame[15 + 7 * 16] == UINT16_C(0x1082)
          && forced_dark_frame[0 + 6 * 16] == light_frame[0 + 6 * 16]);
    /* The black A pixel at row zero/column one is lifted for contrast. */
    CHECK(light_frame[5] == 0 && forced_dark_frame[5] != 0);
    /* All four opaque authored image pixels survive byte-for-byte. */
    CHECK(forced_dark_frame[10] == light_frame[10]
          && forced_dark_frame[11] == light_frame[11]
          && forced_dark_frame[10 + 16] == light_frame[10 + 16]
          && forced_dark_frame[11 + 16] == light_frame[11 + 16]);
    tile_cache_set_forced_dark(&forced_dark_cache, false);
    CHECK(tile_cache_render_frame(
              &forced_dark_cache, 0, 16, 8, NULL)
          && memcmp(forced_dark_frame, light_frame,
                    sizeof(light_frame)) == 0);
    tile_cache_destroy(&forced_dark_cache);
    CHECK(budget.current == 0);

    puts("test: backing-keyed bounded decoded and scaled image caches");
    static const unsigned char invalid_image[] = {0x00, 0x01, 0x02, 0x03};
    ImageResource cache_image_a = {
        .encoded = (unsigned char *) tiny_bmp,
        .encoded_length = sizeof(tiny_bmp),
        .source_width = 2, .source_height = 2, .width = 2, .height = 2
    };
    ImageResource cache_image_alias = cache_image_a;
    ImageResource invalid_cache_image = {
        .encoded = (unsigned char *) invalid_image,
        .encoded_length = sizeof(invalid_image),
        .source_width = 2, .source_height = 2, .width = 2, .height = 2
    };
    DrawCommand image_cache_commands[] = {
        {.image=&cache_image_a, .x=0, .y=0, .width=4, .height=4,
         .opacity_scale=256, .type=DRAW_IMAGE,
         .image_fit=LAYOUT_IMAGE_FIT_STRETCH},
        {.image=&cache_image_alias, .x=5, .y=0, .width=4, .height=4,
         .opacity_scale=256, .type=DRAW_IMAGE,
         .image_fit=LAYOUT_IMAGE_FIT_STRETCH},
        {.image=&cache_image_a, .x=0, .y=5, .width=6, .height=6,
         .opacity_scale=256, .type=DRAW_IMAGE,
         .image_fit=LAYOUT_IMAGE_FIT_STRETCH},
        {.image=&cache_image_alias, .x=7, .y=5, .width=6, .height=6,
         .opacity_scale=256, .type=DRAW_IMAGE,
         .image_fit=LAYOUT_IMAGE_FIT_STRETCH},
        {.image=&invalid_cache_image, .x=0, .y=12, .width=2, .height=2,
         .opacity_scale=256, .type=DRAW_IMAGE,
         .image_fit=LAYOUT_IMAGE_FIT_STRETCH},
        {.image=&invalid_cache_image, .x=3, .y=12, .width=2, .height=2,
         .opacity_scale=256, .type=DRAW_IMAGE,
         .image_fit=LAYOUT_IMAGE_FIT_STRETCH}
    };
    LayoutDocument image_cache_layout = {
        .budget = &budget,
        .commands = image_cache_commands,
        .count = sizeof(image_cache_commands)
                 / sizeof(image_cache_commands[0]),
        .width = 16,
        .scroll_width = 16,
        .height = 16,
        .page_background = 0xffffff
    };
    TileCache image_cache;
    uint16_t *image_cache_frame = budget_malloc(
        &budget, 16u * 16u * sizeof(*image_cache_frame));
    CHECK(image_cache_frame != NULL
          && tile_cache_init(&image_cache, &budget, &image_cache_layout, 1)
          && tile_cache_set_frame(&image_cache, image_cache_frame, 16u * 16u)
          && tile_cache_render_frame(&image_cache, 0, 16, 16, NULL)
          && image_cache.decoded_image_builds == 1
          && image_cache.decoded_image_hits == 1
          && image_cache.decoded_image_failures == 1
          && image_cache.scaled_image_builds == 2
          && image_cache.scaled_image_hits == 2
          && image_cache.decoded_image_cache_bytes
             <= TILEFINCH_DECODED_IMAGE_CACHE_BYTES
          && image_cache.scaled_image_cache_bytes
             <= TILEFINCH_SCALED_IMAGE_CACHE_BYTES
          && image_cache_frame[0] != test_rgb565(0xffffff));
    size_t corrupt_negative_entries = 0;
    for (size_t i = 0; i < TILEFINCH_IMAGE_CACHE_ENTRIES; i++) {
        if (image_cache.decoded_images[i].valid
            && image_cache.decoded_images[i].failed) {
            corrupt_negative_entries++;
        }
    }
    size_t corrupt_decode_failures = image_cache.decoded_image_failures;
    tile_cache_invalidate_rect(&image_cache, 0, 0, 16, 16);
    CHECK(corrupt_negative_entries == 1
          && tile_cache_render_frame(&image_cache, 0, 16, 16, NULL)
          && image_cache.decoded_image_failures == corrupt_decode_failures
          && image_cache_frame[12u * 16u] == test_rgb565(0xffffff)
          && image_cache_frame[12u * 16u + 3u]
             == test_rgb565(0xffffff));
    tile_cache_destroy(&image_cache);
    budget_free(&budget, image_cache_frame);
    CHECK(budget.current == 0);

    puts("test: transient decoded-image OOM retries after tile pressure");
    DrawCommand retry_image_command = {
        .image = &cache_image_a,
        .x = 0, .y = 0, .width = 2, .height = 2,
        .opacity_scale = 256,
        .type = DRAW_IMAGE,
        .image_fit = LAYOUT_IMAGE_FIT_STRETCH
    };
    LayoutDocument retry_image_layout = {
        .budget = &budget,
        .commands = &retry_image_command,
        .count = 1,
        .width = 4,
        .scroll_width = 4,
        .height = 4,
        .page_background = 0xffffff
    };
    TileCache retry_image_cache;
    uint16_t *retry_image_frame = budget_malloc(
        &budget, 4u * 4u * sizeof(*retry_image_frame));
    CHECK(retry_image_frame != NULL
          && tile_cache_init(&retry_image_cache, &budget,
                             &retry_image_layout, 1)
          && tile_cache_set_frame(&retry_image_cache,
                                  retry_image_frame, 4u * 4u));
    budget_inject_failure_after(&budget, 0);
    CHECK(tile_cache_render_frame(&retry_image_cache, 0, 4, 4, NULL)
          && retry_image_cache.decoded_image_failures == 1
          && retry_image_cache.decoded_image_builds == 0
          && retry_image_frame[0] == test_rgb565(0xffffff));
    size_t retry_negative_entries = 0;
    for (size_t i = 0; i < TILEFINCH_IMAGE_CACHE_ENTRIES; i++) {
        if (retry_image_cache.decoded_images[i].valid
            && retry_image_cache.decoded_images[i].failed) {
            retry_negative_entries++;
        }
    }
    budget_clear_failure_injection(&budget);
    tile_cache_invalidate_rect(&retry_image_cache, 0, 0, 4, 4);
    CHECK(retry_negative_entries == 0
          && tile_cache_render_frame(&retry_image_cache, 0, 4, 4, NULL)
          && retry_image_cache.decoded_image_failures == 1
          && retry_image_cache.decoded_image_builds == 1
          && retry_image_frame[0] != test_rgb565(0xffffff));
    tile_cache_invalidate_rect(&retry_image_cache, 0, 0, 4, 4);
    CHECK(tile_cache_render_frame(&retry_image_cache, 0, 4, 4, NULL)
          && retry_image_cache.decoded_image_hits >= 1);
    tile_cache_destroy(&retry_image_cache);
    budget_free(&budget, retry_image_frame);
    CHECK(budget.current == 0);

    puts("test: transient scaled-image OOM retries without a negative entry");
    static unsigned char scaled_retry_pixels[] = {
        255, 0, 0, 255, 0, 255, 0, 255,
        0, 0, 255, 255, 255, 255, 255, 255
    };
    ImageResource scaled_retry_image = {
        .pixels = scaled_retry_pixels,
        .width = 2,
        .height = 2
    };
    DrawCommand scaled_retry_command = {
        .image = &scaled_retry_image,
        .x = 0, .y = 0, .width = 4, .height = 4,
        .opacity_scale = 256,
        .type = DRAW_IMAGE,
        .image_fit = LAYOUT_IMAGE_FIT_STRETCH
    };
    LayoutDocument scaled_retry_layout = {
        .budget = &budget,
        .commands = &scaled_retry_command,
        .count = 1,
        .width = 4,
        .scroll_width = 4,
        .height = 4,
        .page_background = 0xffffff
    };
    TileCache scaled_retry_cache;
    uint16_t *scaled_retry_frame = budget_malloc(
        &budget, 4u * 4u * sizeof(*scaled_retry_frame));
    CHECK(scaled_retry_frame != NULL
          && tile_cache_init(&scaled_retry_cache, &budget,
                             &scaled_retry_layout, 1)
          && tile_cache_set_frame(&scaled_retry_cache,
                                  scaled_retry_frame, 4u * 4u));
    budget_inject_failure_after(&budget, 0);
    CHECK(tile_cache_render_frame(&scaled_retry_cache, 0, 4, 4, NULL)
          && scaled_retry_cache.scaled_image_builds == 0
          && scaled_retry_frame[0] != test_rgb565(0xffffff));
    size_t scaled_negative_entries = 0;
    for (size_t i = 0; i < TILEFINCH_IMAGE_CACHE_ENTRIES; i++) {
        if (scaled_retry_cache.scaled_images[i].valid
            && scaled_retry_cache.scaled_images[i].failed) {
            scaled_negative_entries++;
        }
    }
    budget_clear_failure_injection(&budget);
    tile_cache_invalidate_rect(&scaled_retry_cache, 0, 0, 4, 4);
    CHECK(scaled_negative_entries == 0
          && tile_cache_render_frame(&scaled_retry_cache, 0, 4, 4, NULL)
          && scaled_retry_cache.scaled_image_builds == 1
          && scaled_retry_cache.scaled_image_cache_bytes <=
             TILEFINCH_SCALED_IMAGE_CACHE_BYTES);
    tile_cache_destroy(&scaled_retry_cache);
    budget_free(&budget, scaled_retry_frame);
    CHECK(budget.current == 0);

    puts("test: oversized decodes are command-scoped, not cache residents");
    enum { OVERSIZED_IMAGE_WIDTH = 400, OVERSIZED_IMAGE_HEIGHT = 400 };
    size_t oversized_row_bytes = OVERSIZED_IMAGE_WIDTH * 3u;
    size_t oversized_pixel_bytes = oversized_row_bytes
                                   * OVERSIZED_IMAGE_HEIGHT;
    size_t oversized_bmp_bytes = 54u + oversized_pixel_bytes;
    unsigned char *oversized_bmp = budget_malloc(&budget,
                                                  oversized_bmp_bytes);
    CHECK(oversized_bmp != NULL && oversized_bmp_bytes <= UINT32_MAX);
    memset(oversized_bmp, 0, oversized_bmp_bytes);
    oversized_bmp[0] = 'B';
    oversized_bmp[1] = 'M';
    test_store_le32(oversized_bmp + 2, (uint32_t) oversized_bmp_bytes);
    test_store_le32(oversized_bmp + 10, 54);
    test_store_le32(oversized_bmp + 14, 40);
    test_store_le32(oversized_bmp + 18, OVERSIZED_IMAGE_WIDTH);
    test_store_le32(oversized_bmp + 22, OVERSIZED_IMAGE_HEIGHT);
    oversized_bmp[26] = 1;
    oversized_bmp[28] = 24;
    test_store_le32(oversized_bmp + 34,
                    (uint32_t) oversized_pixel_bytes);
    for (size_t at = 54; at < oversized_bmp_bytes; at += 3u) {
        oversized_bmp[at + 2u] = 255;
    }
    ImageResource oversized_image = {
        .encoded = oversized_bmp,
        .encoded_length = oversized_bmp_bytes,
        .source_width = OVERSIZED_IMAGE_WIDTH,
        .source_height = OVERSIZED_IMAGE_HEIGHT,
        .width = OVERSIZED_IMAGE_WIDTH,
        .height = OVERSIZED_IMAGE_HEIGHT
    };
    DrawCommand oversized_image_command = {
        .image = &oversized_image,
        .x = 0, .y = 0,
        .width = OVERSIZED_IMAGE_WIDTH,
        .height = OVERSIZED_IMAGE_HEIGHT,
        .opacity_scale = 256,
        .type = DRAW_IMAGE,
        .image_fit = LAYOUT_IMAGE_FIT_STRETCH
    };
    LayoutDocument oversized_image_layout = {
        .budget = &budget,
        .commands = &oversized_image_command,
        .count = 1,
        .width = 16,
        .scroll_width = 16,
        .height = 16,
        .page_background = 0xffffff
    };
    TileCache oversized_image_cache;
    uint16_t *oversized_image_frame = budget_malloc(
        &budget, 16u * 16u * sizeof(*oversized_image_frame));
    CHECK(oversized_image_frame != NULL
          && tile_cache_init(&oversized_image_cache, &budget,
                             &oversized_image_layout, 1)
          && tile_cache_set_frame(&oversized_image_cache,
                                  oversized_image_frame, 16u * 16u)
          && tile_cache_render_frame(&oversized_image_cache, 0, 16, 16,
                                     NULL)
          && oversized_image_cache.decoded_image_builds == 1
          && oversized_image_cache.decoded_image_cache_bytes == 0
          && oversized_image_cache.scaled_image_cache_bytes == 0
          && oversized_image_frame[0] == test_rgb565(0xff0000));
    tile_cache_destroy(&oversized_image_cache);
    budget_free(&budget, oversized_image_frame);
    budget_free(&budget, oversized_bmp);
    CHECK(budget.current == 0);

    puts("test: sprite tiling maps device pixels without per-pixel division");
    /* The tiled sprite blit walks its source mapping with a Bresenham
       stepper rather than two 64-bit divisions per output pixel. The walk is
       only correct if it advances on every output pixel, including the ones
       the loop skips, and if the tile reduction stays in step with it. A
       2x2 source with four distinguishable pixels makes both observable:
       the painted box has to be exactly periodic in the source dimensions,
       so any lost or extra step shows up as a shifted or smeared row. */
    enum { TILE_PROBE_W = 13, TILE_PROBE_H = 9 };
    ImageResource tile_probe_image = {
        .encoded = (unsigned char *) tiny_bmp,
        .encoded_length = sizeof(tiny_bmp),
        .source_width = 2, .source_height = 2, .width = 2, .height = 2
    };
    DrawCommand tile_probe_command = {
        .image = &tile_probe_image,
        .x = 0, .y = 0,
        .width = TILE_PROBE_W, .height = TILE_PROBE_H,
        .opacity_scale = 256,
        .type = DRAW_IMAGE,
        .image_fit = LAYOUT_IMAGE_FIT_SPRITE_TILE_XY
    };
    LayoutDocument tile_probe_layout = {
        .budget = &budget,
        .commands = &tile_probe_command,
        .count = 1,
        .width = TILE_PROBE_W,
        .scroll_width = TILE_PROBE_W,
        .height = TILE_PROBE_H,
        .page_background = 0xffffff
    };
    TileCache tile_probe_cache;
    uint16_t *tile_probe_frame = budget_malloc(
        &budget, TILE_PROBE_W * TILE_PROBE_H * sizeof(*tile_probe_frame));
    CHECK(tile_probe_frame != NULL
          && tile_cache_init(&tile_probe_cache, &budget, &tile_probe_layout, 1)
          && tile_cache_set_frame(&tile_probe_cache, tile_probe_frame,
                                  TILE_PROBE_W * TILE_PROBE_H)
          && tile_cache_render_frame(&tile_probe_cache, 0, TILE_PROBE_W,
                                     TILE_PROBE_H, NULL));
    /* Periodicity is only evidence if the tile's own pixels differ. */
    uint16_t tile_00 = tile_probe_frame[0];
    uint16_t tile_10 = tile_probe_frame[1];
    uint16_t tile_01 = tile_probe_frame[TILE_PROBE_W];
    uint16_t tile_11 = tile_probe_frame[TILE_PROBE_W + 1];
    CHECK(tile_00 != tile_10 && tile_00 != tile_01 && tile_00 != tile_11
          && tile_10 != tile_01 && tile_10 != tile_11 && tile_01 != tile_11);
    size_t tile_probe_mismatches = 0;
    for (int y = 0; y < TILE_PROBE_H; y++) {
        for (int x = 0; x < TILE_PROBE_W; x++) {
            uint16_t expected =
                tile_probe_frame[(size_t) (y % 2) * TILE_PROBE_W
                                 + (size_t) (x % 2)];
            if (tile_probe_frame[(size_t) y * TILE_PROBE_W + (size_t) x]
                != expected) tile_probe_mismatches++;
        }
    }
    CHECK(tile_probe_mismatches == 0);
    tile_cache_destroy(&tile_probe_cache);

    /* Same box with a source-pixel offset: the offset shifts the phase of
       the walk without changing its period. */
    draw_command_set_image_offset(&tile_probe_command, 3, 1);
    TileCache tile_offset_cache;
    uint16_t *tile_offset_frame = budget_malloc(
        &budget, TILE_PROBE_W * TILE_PROBE_H * sizeof(*tile_offset_frame));
    CHECK(tile_offset_frame != NULL
          && tile_cache_init(&tile_offset_cache, &budget, &tile_probe_layout,
                             1)
          && tile_cache_set_frame(&tile_offset_cache, tile_offset_frame,
                                  TILE_PROBE_W * TILE_PROBE_H)
          && tile_cache_render_frame(&tile_offset_cache, 0, TILE_PROBE_W,
                                     TILE_PROBE_H, NULL));
    size_t tile_offset_mismatches = 0;
    for (int y = 0; y < TILE_PROBE_H; y++) {
        for (int x = 0; x < TILE_PROBE_W; x++) {
            /* natural = device - offset, so -3 and -1 are +1 modulo 2. */
            uint16_t expected =
                tile_probe_frame[(size_t) ((y + 1) % 2) * TILE_PROBE_W
                                 + (size_t) ((x + 1) % 2)];
            if (tile_offset_frame[(size_t) y * TILE_PROBE_W + (size_t) x]
                != expected) tile_offset_mismatches++;
        }
    }
    CHECK(tile_offset_mismatches == 0);
    tile_cache_destroy(&tile_offset_cache);
    draw_command_set_image_offset(&tile_probe_command, 0, 0);

    /* An explicit CSS background-size scales the sheet inside the command's
       paint box; it must not replace that box and leak the rest of an icon
       atlas over neighboring content.  A 4x4 authored sheet backed by the
       2x2 probe makes every source pixel occupy a 2x2 CSS block. */
    draw_command_set_image_sprite_size(&tile_probe_command, 4, 4);
    tile_probe_command.image_fit = LAYOUT_IMAGE_FIT_SPRITE;
    TileCache sized_sprite_cache;
    uint16_t *sized_sprite_frame = budget_malloc(
        &budget, TILE_PROBE_W * TILE_PROBE_H * sizeof(*sized_sprite_frame));
    CHECK(sized_sprite_frame != NULL
          && tile_cache_init(&sized_sprite_cache, &budget,
                             &tile_probe_layout, 1)
          && tile_cache_set_frame(&sized_sprite_cache, sized_sprite_frame,
                                  TILE_PROBE_W * TILE_PROBE_H)
          && tile_cache_render_frame(&sized_sprite_cache, 0, TILE_PROBE_W,
                                     TILE_PROBE_H, NULL));
    size_t sized_sprite_mismatches = 0;
    for (int y = 0; y < TILE_PROBE_H; y++) {
        for (int x = 0; x < TILE_PROBE_W; x++) {
            uint16_t expected = x < 4 && y < 4
                ? tile_probe_frame[(size_t) (y / 2) * TILE_PROBE_W
                                   + (size_t) (x / 2)]
                : test_rgb565(0xffffff);
            if (sized_sprite_frame[(size_t) y * TILE_PROBE_W + (size_t) x]
                != expected) sized_sprite_mismatches++;
        }
    }
    CHECK(sized_sprite_mismatches == 0);
    tile_cache_destroy(&sized_sprite_cache);
    tile_probe_command.text_length = 0;

    /* Repeat-x only: rows past the source height fall outside the sheet and
       must stay unpainted, which exercises the skip path the stepper has to
       advance across. */
    tile_probe_command.image_fit = LAYOUT_IMAGE_FIT_SPRITE_TILE_X;
    TileCache tile_row_cache;
    uint16_t *tile_row_frame = budget_malloc(
        &budget, TILE_PROBE_W * TILE_PROBE_H * sizeof(*tile_row_frame));
    CHECK(tile_row_frame != NULL
          && tile_cache_init(&tile_row_cache, &budget, &tile_probe_layout, 1)
          && tile_cache_set_frame(&tile_row_cache, tile_row_frame,
                                  TILE_PROBE_W * TILE_PROBE_H)
          && tile_cache_render_frame(&tile_row_cache, 0, TILE_PROBE_W,
                                     TILE_PROBE_H, NULL));
    size_t tile_row_mismatches = 0;
    for (int y = 0; y < TILE_PROBE_H; y++) {
        for (int x = 0; x < TILE_PROBE_W; x++) {
            uint16_t expected = y < 2
                ? tile_probe_frame[(size_t) y * TILE_PROBE_W
                                   + (size_t) (x % 2)]
                : test_rgb565(0xffffff);
            if (tile_row_frame[(size_t) y * TILE_PROBE_W + (size_t) x]
                != expected) tile_row_mismatches++;
        }
    }
    CHECK(tile_row_mismatches == 0);
    tile_cache_destroy(&tile_row_cache);
    tile_probe_command.image_fit = LAYOUT_IMAGE_FIT_SPRITE_TILE_XY;

    /* Under viewport visual scaling the box is in device pixels while the
       crop is in CSS pixels, so the walk advances by denominator/numerator
       per output pixel. At 1:2 each source pixel covers two device columns
       and rows, which doubles the period. */
    LayoutDocument tile_scaled_layout = tile_probe_layout;
    CHECK(viewport_context_init(&tile_scaled_layout.viewport,
                                TILE_PROBE_W, TILE_PROBE_H,
                                TILE_PROBE_W, TILE_PROBE_H));
    tile_scaled_layout.viewport.scale_numerator = 2;
    tile_scaled_layout.viewport.scale_denominator = 1;
    TileCache tile_scaled_cache;
    uint16_t *tile_scaled_frame = budget_malloc(
        &budget, TILE_PROBE_W * TILE_PROBE_H * sizeof(*tile_scaled_frame));
    CHECK(tile_scaled_frame != NULL
          && tile_cache_init(&tile_scaled_cache, &budget,
                             &tile_scaled_layout, 1)
          && tile_cache_set_frame(&tile_scaled_cache, tile_scaled_frame,
                                  TILE_PROBE_W * TILE_PROBE_H)
          && tile_cache_render_frame(&tile_scaled_cache, 0, TILE_PROBE_W,
                                     TILE_PROBE_H, NULL));
    size_t tile_scaled_mismatches = 0;
    for (int y = 0; y < TILE_PROBE_H; y++) {
        for (int x = 0; x < TILE_PROBE_W; x++) {
            uint16_t expected =
                tile_probe_frame[(size_t) ((y / 2) % 2) * TILE_PROBE_W
                                 + (size_t) ((x / 2) % 2)];
            if (tile_scaled_frame[(size_t) y * TILE_PROBE_W + (size_t) x]
                != expected) tile_scaled_mismatches++;
        }
    }
    CHECK(tile_scaled_mismatches == 0);
    tile_cache_destroy(&tile_scaled_cache);
    budget_free(&budget, tile_scaled_frame);
    budget_free(&budget, tile_row_frame);
    budget_free(&budget, sized_sprite_frame);
    budget_free(&budget, tile_offset_frame);
    budget_free(&budget, tile_probe_frame);
    CHECK(budget.current == 0);

    puts("test: adversarial scale dimensions bypass the bounded cache");
    DrawCommand adversarial_scale_command = {
        .image = &cache_image_a,
        .x = 1, .y = 1,
        .width = INT_MAX,
        .height = INT_MAX,
        .opacity_scale = 256,
        .type = DRAW_IMAGE,
        .image_fit = LAYOUT_IMAGE_FIT_STRETCH
    };
    LayoutDocument adversarial_scale_layout = {
        .budget = &budget,
        .commands = &adversarial_scale_command,
        .count = 1,
        .width = 16,
        .scroll_width = 16,
        .height = 16,
        .page_background = 0xffffff
    };
    TileCache adversarial_scale_cache;
    uint16_t *adversarial_scale_frame = budget_malloc(
        &budget, 16u * 16u * sizeof(*adversarial_scale_frame));
    CHECK(adversarial_scale_frame != NULL
          && tile_cache_init(&adversarial_scale_cache, &budget,
                             &adversarial_scale_layout, 1)
          && tile_cache_set_frame(&adversarial_scale_cache,
                                  adversarial_scale_frame, 16u * 16u)
          && tile_cache_render_frame(&adversarial_scale_cache, 0, 16, 16,
                                     NULL)
          && adversarial_scale_cache.scaled_image_builds == 0
          && adversarial_scale_cache.scaled_image_cache_bytes == 0
          && adversarial_scale_cache.decoded_image_cache_bytes
             <= TILEFINCH_DECODED_IMAGE_CACHE_BYTES
          && adversarial_scale_frame[1u * 16u + 1u]
             != test_rgb565(0xffffff));
    tile_cache_destroy(&adversarial_scale_cache);
    budget_free(&budget, adversarial_scale_frame);
    CHECK(budget.current == 0);

    puts("test: legacy presentational HTML hints");
    static const char legacy_page[] =
        "<!doctype html><body id=legacy bgcolor='#f6f6ef' text='#010203'>"
        "<center id=legacy-center>"
        "<table id=legacy-table bgcolor='#ff6600' width='85%' cellpadding=2>"
        "<tr id=legacy-row height=7><td id=legacy-cell width=12 align=right>"
        "LEGACY</td></tr><tr><td>SECOND</td></tr></table></center></body>";
    PocDocument legacy_document;
    Stylesheet legacy_sheet;
    LayoutDocument legacy_layout;
    CHECK(document_parse(&legacy_document, &budget, legacy_page,
                         strlen(legacy_page), 17)
          && stylesheet_build(&legacy_sheet, &budget, &legacy_document, 480));
    lxb_dom_node_t *legacy_body = find_id(
        lxb_dom_interface_node(legacy_document.html), "legacy");
    lxb_dom_node_t *legacy_table = find_id(
        lxb_dom_interface_node(legacy_document.html), "legacy-table");
    lxb_dom_node_t *legacy_center = find_id(
        lxb_dom_interface_node(legacy_document.html), "legacy-center");
    lxb_dom_node_t *legacy_row = find_id(
        lxb_dom_interface_node(legacy_document.html), "legacy-row");
    lxb_dom_node_t *legacy_cell = find_id(
        lxb_dom_interface_node(legacy_document.html), "legacy-cell");
    CHECK(legacy_body != NULL && legacy_center != NULL
          && legacy_table != NULL && legacy_row != NULL
          && legacy_cell != NULL);
    ComputedStyle legacy_body_style = style_for_node(
        &legacy_sheet, legacy_body, NULL);
    ComputedStyle legacy_center_style = style_for_node(
        &legacy_sheet, legacy_center, &legacy_body_style);
    ComputedStyle legacy_table_style = style_for_node(
        &legacy_sheet, legacy_table, &legacy_center_style);
    ComputedStyle legacy_row_style = style_for_node(
        &legacy_sheet, legacy_row, &legacy_table_style);
    ComputedStyle legacy_cell_style = style_for_node(
        &legacy_sheet, legacy_cell, &legacy_row_style);
    CHECK(legacy_body_style.has_background
          && legacy_body_style.background == 0xf6f6ef
          && legacy_body_style.color == 0x010203
          && legacy_center_style.display == DISPLAY_BLOCK
          && legacy_center_style.text_align == TEXT_ALIGN_CENTER
          && legacy_table_style.has_background
          && legacy_table_style.background == 0xff6600
          && legacy_table_style.has_width
          && legacy_table_style.width_percent
          && legacy_table_style.width == 85
          && legacy_row_style.has_height && legacy_row_style.height == 7
          && legacy_cell_style.has_width && legacy_cell_style.width == 12
          && legacy_cell_style.padding.top == 2
          && legacy_cell_style.padding.right == 2
          && legacy_cell_style.padding.bottom == 2
          && legacy_cell_style.padding.left == 2
          && legacy_cell_style.text_align == TEXT_ALIGN_RIGHT
          && layout_build(&legacy_layout, &budget, &legacy_document,
                          &legacy_sheet, NULL, NULL, 480)
          && legacy_layout.page_background == 0xf6f6ef);
    const DrawCommand *legacy_first = text_command(&legacy_layout, "LEGACY");
    const DrawCommand *legacy_second = text_command(&legacy_layout, "SECOND");
    CHECK(legacy_first != NULL && legacy_second != NULL
          && legacy_second->y >= legacy_first->y + legacy_first->height);
    layout_destroy(&legacy_layout);
    stylesheet_destroy(&legacy_sheet);
    document_destroy(&legacy_document);
    CHECK(budget.current == 0);

    puts("test: SVG metadata remains non-rendered under author CSS");
    static const char svg_metadata_page[] =
        "<!doctype html><style>svg,title,desc{display:block}</style><body>"
        "<p>VISIBLE</p><svg><title>SVG TITLE</title>"
        "<desc>SVG DESCRIPTION</desc></svg></body>";
    PocDocument svg_metadata_document;
    Stylesheet svg_metadata_sheet;
    LayoutDocument svg_metadata_layout;
    CHECK(document_parse(&svg_metadata_document, &budget, svg_metadata_page,
                         strlen(svg_metadata_page), 31));
    CHECK(stylesheet_build(&svg_metadata_sheet, &budget,
                           &svg_metadata_document, 480)
          && layout_build(&svg_metadata_layout, &budget,
                          &svg_metadata_document, &svg_metadata_sheet,
                          NULL, NULL, 480)
          && command_x(&svg_metadata_layout, "VISIBLE") >= 0
          && command_x(&svg_metadata_layout, "SVG TITLE") == -1
          && command_x(&svg_metadata_layout, "SVG DESCRIPTION") == -1);
    layout_destroy(&svg_metadata_layout);
    stylesheet_destroy(&svg_metadata_sheet);
    document_destroy(&svg_metadata_document);
    CHECK(budget.current == 0);

    puts("test: affine percentage lengths retain pixel offsets");
    static const char affine_width_page[] =
        "<!doctype html><style>body{margin:0;padding:0}#affine-width{"
        "width:100%;max-width:calc(100% - 32px);margin:0 16px}"
        "#affine-bleed{margin:0 -16px}</style><body>"
        "<div id=affine-width><div id=affine-bleed>BLEED</div></div></body>";
    PocDocument affine_width_document;
    Stylesheet affine_width_sheet;
    LayoutDocument affine_width_layout;
    CHECK(document_parse(&affine_width_document, &budget, affine_width_page,
                         strlen(affine_width_page), 31)
          && stylesheet_build(&affine_width_sheet, &budget,
                              &affine_width_document, 480)
          && layout_build(&affine_width_layout, &budget,
                          &affine_width_document, &affine_width_sheet,
                          NULL, NULL, 480));
    lxb_dom_node_t *affine_width_node = find_id(
        lxb_dom_interface_node(affine_width_document.html), "affine-width");
    lxb_dom_node_t *affine_bleed_node = find_id(
        lxb_dom_interface_node(affine_width_document.html), "affine-bleed");
    const LayoutNodeBox *affine_width_box = layout_box_for_node(
        &affine_width_layout, affine_width_node);
    const LayoutNodeBox *affine_bleed_box = layout_box_for_node(
        &affine_width_layout, affine_bleed_node);
    CHECK(affine_width_box != NULL && affine_width_box->x == 16
          && affine_width_box->width == 448
          && affine_bleed_box != NULL && affine_bleed_box->x == 0
          && affine_bleed_box->width == 480
          && affine_width_layout.scroll_width == 480);
    layout_destroy(&affine_width_layout);
    stylesheet_destroy(&affine_width_sheet);
    document_destroy(&affine_width_document);
    CHECK(budget.current == 0);

    puts("test: flex placement uses assigned size, not painted overflow");
    static const char flex_overflow_page[] =
        "<!doctype html><style>body{margin:0;padding:0}.row{display:flex;"
        "width:100px}.item{display:block;flex:1 1 80px;min-width:0}"
        ".wide{white-space:nowrap}</style><body><div class=row>"
        "<div id=flex-overflow-a class='item wide'>UNBREAKABLEOVERFLOW</div>"
        "<div id=flex-overflow-b class=item>B</div></div></body>";
    PocDocument flex_overflow_document;
    Stylesheet flex_overflow_sheet;
    LayoutDocument flex_overflow_layout;
    CHECK(document_parse(&flex_overflow_document, &budget,
                         flex_overflow_page, strlen(flex_overflow_page), 31)
          && stylesheet_build(&flex_overflow_sheet, &budget,
                              &flex_overflow_document, 480)
          && layout_build(&flex_overflow_layout, &budget,
                          &flex_overflow_document, &flex_overflow_sheet,
                          NULL, NULL, 480));
    lxb_dom_node_t *flex_overflow_a = find_id(
        lxb_dom_interface_node(flex_overflow_document.html),
        "flex-overflow-a");
    lxb_dom_node_t *flex_overflow_b = find_id(
        lxb_dom_interface_node(flex_overflow_document.html),
        "flex-overflow-b");
    const LayoutNodeBox *flex_overflow_a_box = layout_box_for_node(
        &flex_overflow_layout, flex_overflow_a);
    const LayoutNodeBox *flex_overflow_b_box = layout_box_for_node(
        &flex_overflow_layout, flex_overflow_b);
    CHECK(flex_overflow_a_box != NULL && flex_overflow_b_box != NULL
          && flex_overflow_a_box->width == 50
          && flex_overflow_b_box->x == flex_overflow_a_box->x + 50);
    layout_destroy(&flex_overflow_layout);
    stylesheet_destroy(&flex_overflow_sheet);
    document_destroy(&flex_overflow_document);
    CHECK(budget.current == 0);

    puts("test: generated content preserves overflow clip ranges");
    static const char generated_clip_page[] =
        "<!doctype html><style>body{margin:0;padding:0}#generated-clip{"
        "display:block;width:8px;height:14px;overflow:hidden;"
        "white-space:nowrap}#generated-clip::before{content:' '}</style>"
        "<body><span id=generated-clip>SEARCH GOV.UK</span></body>";
    PocDocument generated_clip_document;
    Stylesheet generated_clip_sheet;
    LayoutDocument generated_clip_layout;
    CHECK(document_parse(&generated_clip_document, &budget,
                         generated_clip_page,
                         strlen(generated_clip_page), 31)
          && stylesheet_build(&generated_clip_sheet, &budget,
                              &generated_clip_document, 480)
          && layout_build(&generated_clip_layout, &budget,
                          &generated_clip_document, &generated_clip_sheet,
                          NULL, NULL, 480)
          && generated_clip_layout.scroll_width == 480);
    layout_destroy(&generated_clip_layout);
    stylesheet_destroy(&generated_clip_sheet);
    document_destroy(&generated_clip_document);
    CHECK(budget.current == 0);

    puts("test: layout-backed geometry and nested overflow state");
    static const char geometry_page[] =
        "<!doctype html><meta name=viewport content='width=device-width'>"
        "<style>body{margin:0}.shell{width:240px;"
        "height:90px;overflow:auto;padding:4px;border-radius:20px}"
        ".item{display:block;height:70px}"
        "#first{background:#aa0000}#second{background:#0000aa}"
        "#sized{width:100px;max-width:80px;height:100px;max-height:60px;"
        "padding:10px;border-width:2px;box-sizing:border-box}"
        "#clip-only{width:40px;height:20px;overflow:clip}"
        "#clip-child{height:60px}"
        "#wide-clip{width:50px;overflow:hidden}"
        "#wide-descendant{width:900px;height:10px}"
        "#center-owner{text-align:center}"
        "#center-block{width:100%;height:10px}"
        "#position-host{position:relative;width:100%;height:20px}"
        "#position-affected{width:100%;height:10px}"
        "#position-source{position:absolute;right:0;top:0;width:40px;"
        "height:10px}"
        "#clip-root{position:relative;width:40px;height:40px;overflow:hidden}"
        "#non-containing-clip{height:0;overflow:hidden}"
        "#escaping-positioned{position:absolute;left:0;top:0;width:10px;"
        "height:10px;background:#008000}"
        "#sibling-clip-row{position:relative;height:20px}"
        "#sibling-clip{width:100%;overflow:hidden}"
        "#sibling-positioned{position:absolute;right:0;top:0;width:20px;"
        "height:20px;background:#ff8800;z-index:1}"
        "#sibling-hit{position:absolute;inset:0;width:100%;height:100%;"
        "z-index:10;border:0;background:transparent}"
        "#sibling-mark{position:absolute;left:2px;top:2px;width:4px;"
        "height:4px;background:#00ff00;z-index:20}"
        "</style><body><main id=shell class=shell>"
        "<a id=first class=item href=/rounded>FIRST</a>"
        "<section id=second class=item>SECOND</section>"
        "</main><div id=sized></div><div id=clip-only>"
        "<div id=clip-child>CLIPPED</div></div>"
        "<div id=wide-clip><div id=wide-descendant></div></div>"
        "<div id=center-owner><div id=center-block></div>CENTERED</div>"
        "<div id=position-host><div id=position-affected></div>"
        "<div id=position-source></div></div>"
        "<div id=clip-root><div id=non-containing-clip>"
        "<div id=escaping-positioned></div></div></div>"
        "<div id=sibling-clip-row><div id=sibling-clip>FIELD</div>"
        "<div id=sibling-positioned><input id=sibling-hit type=submit>"
        "<span id=sibling-mark></span></div></div></body>";
    NavigationSession geometry_navigation;
    CHECK(navigation_init(&geometry_navigation, &budget, 4));
    navigation_enable_scripts(&geometry_navigation, 3 * MIB, 1000);
    uint64_t geometry_generation = navigation_begin(&geometry_navigation);
    CHECK(navigation_commit_html(&geometry_navigation, geometry_generation,
                                 "https://geometry.test/", geometry_page,
                                 strlen(geometry_page), 480, NULL, NULL,
                                 true));
    lxb_dom_node_t *shell = find_id(
        lxb_dom_interface_node(geometry_navigation.page.document.html),
        "shell");
    lxb_dom_node_t *second = find_id(
        lxb_dom_interface_node(geometry_navigation.page.document.html),
        "second");
    lxb_dom_node_t *sized = find_id(
        lxb_dom_interface_node(geometry_navigation.page.document.html),
        "sized");
    lxb_dom_node_t *clip_only = find_id(
        lxb_dom_interface_node(geometry_navigation.page.document.html),
        "clip-only");
    lxb_dom_node_t *center_block = find_id(
        lxb_dom_interface_node(geometry_navigation.page.document.html),
        "center-block");
    lxb_dom_node_t *center_owner = find_id(
        lxb_dom_interface_node(geometry_navigation.page.document.html),
        "center-owner");
    lxb_dom_node_t *position_host = find_id(
        lxb_dom_interface_node(geometry_navigation.page.document.html),
        "position-host");
    lxb_dom_node_t *position_affected = find_id(
        lxb_dom_interface_node(geometry_navigation.page.document.html),
        "position-affected");
    lxb_dom_node_t *position_source = find_id(
        lxb_dom_interface_node(geometry_navigation.page.document.html),
        "position-source");
    lxb_dom_node_t *clip_root = find_id(
        lxb_dom_interface_node(geometry_navigation.page.document.html),
        "clip-root");
    lxb_dom_node_t *non_containing_clip = find_id(
        lxb_dom_interface_node(geometry_navigation.page.document.html),
        "non-containing-clip");
    lxb_dom_node_t *escaping_positioned = find_id(
        lxb_dom_interface_node(geometry_navigation.page.document.html),
        "escaping-positioned");
    lxb_dom_node_t *sibling_clip = find_id(
        lxb_dom_interface_node(geometry_navigation.page.document.html),
        "sibling-clip");
    lxb_dom_node_t *sibling_positioned = find_id(
        lxb_dom_interface_node(geometry_navigation.page.document.html),
        "sibling-positioned");
    const LayoutNodeBox *shell_box = layout_box_for_node(
        &geometry_navigation.page.layout, shell);
    const LayoutNodeBox *second_box = layout_box_for_node(
        &geometry_navigation.page.layout, second);
    const LayoutNodeBox *sized_box = layout_box_for_node(
        &geometry_navigation.page.layout, sized);
    const LayoutNodeBox *clip_only_box = layout_box_for_node(
        &geometry_navigation.page.layout, clip_only);
    const LayoutNodeBox *center_block_box = layout_box_for_node(
        &geometry_navigation.page.layout, center_block);
    const LayoutNodeBox *center_owner_box = layout_box_for_node(
        &geometry_navigation.page.layout, center_owner);
    const LayoutNodeBox *position_host_box = layout_box_for_node(
        &geometry_navigation.page.layout, position_host);
    const LayoutNodeBox *position_affected_box = layout_box_for_node(
        &geometry_navigation.page.layout, position_affected);
    const LayoutNodeBox *position_source_box = layout_box_for_node(
        &geometry_navigation.page.layout, position_source);
    const LayoutNodeBox *clip_root_box = layout_box_for_node(
        &geometry_navigation.page.layout, clip_root);
    const LayoutNodeBox *non_containing_clip_box = layout_box_for_node(
        &geometry_navigation.page.layout, non_containing_clip);
    const LayoutNodeBox *escaping_positioned_box = layout_box_for_node(
        &geometry_navigation.page.layout, escaping_positioned);
    const LayoutNodeBox *sibling_clip_box = layout_box_for_node(
        &geometry_navigation.page.layout, sibling_clip);
    const LayoutNodeBox *sibling_positioned_box = layout_box_for_node(
        &geometry_navigation.page.layout, sibling_positioned);
    bool inherited_root_text_opaque = false;
    for (size_t i = 0; i < geometry_navigation.page.layout.count; i++) {
        const DrawCommand *command = &geometry_navigation.page.layout.commands[i];
        if (command->type == DRAW_TEXT && command->opacity_scale == 256) {
            inherited_root_text_opaque = true;
            break;
        }
    }
    CHECK(center_owner_box != NULL && center_block_box != NULL
          && center_block_box->x == center_owner_box->x
          && center_block_box->width == center_owner_box->client_width);
    CHECK(position_host_box != NULL && position_affected_box != NULL
          && position_affected_box->x == position_host_box->x
          && position_affected_box->width == position_host_box->client_width);
    CHECK(position_source_box != NULL && position_source_box->width == 40
          && position_source_box->x
             == position_host_box->x + position_host_box->client_width - 40);
    CHECK(clip_root_box != NULL && non_containing_clip_box != NULL
          && escaping_positioned_box != NULL
          && escaping_positioned_box->positioned_ancestor_distance == 2);
    bool escapes_intermediate_clip = false;
    bool escapes_containing_clip = false;
    for (size_t i = escaping_positioned_box->command_start;
         i < escaping_positioned_box->command_end; i++) {
        escapes_intermediate_clip |= layout_positioned_command_escapes_clip(
            &geometry_navigation.page.layout, i, non_containing_clip_box);
        escapes_containing_clip |= layout_positioned_command_escapes_clip(
            &geometry_navigation.page.layout, i, clip_root_box);
    }
    CHECK(escapes_intermediate_clip && !escapes_containing_clip);
    bool sibling_escapes_clip = true;
    size_t sibling_command_count = 0;
    CHECK(sibling_clip_box != NULL && sibling_positioned_box != NULL);
    for (size_t i = sibling_positioned_box->command_start;
         i < sibling_positioned_box->command_end; i++) {
        sibling_command_count++;
        sibling_escapes_clip &= layout_positioned_command_escapes_clip(
            &geometry_navigation.page.layout, i, sibling_clip_box);
    }
    CHECK(sibling_command_count != 0 && sibling_escapes_clip);
    CHECK(shell_box != NULL && second_box != NULL
          && shell_box->width == 248 && shell_box->height >= 90
          && shell_box->content_height > shell_box->height
          && shell_box->clips_x && shell_box->clips_y
          && shell_box->clip_radius == 20 && sized_box != NULL
          && sized_box->width == 80 && sized_box->height == 60
          && sized_box->client_width == 76
          && sized_box->client_height == 56
          && clip_only_box != NULL && clip_only_box->clips_x
          && clip_only_box->clips_y && clip_only_box->clip_only_x
          && clip_only_box->clip_only_y
          && geometry_navigation.page.layout.scroll_width == 480
          && inherited_root_text_opaque
          && layout_scroll_node(&geometry_navigation.page.layout,
                                clip_only, 0, 999)
          && clip_only_box->scroll_y == 0);
    TileCache overflow_cache;
    uint16_t *overflow_frame = budget_malloc(
        &budget, 480u * 272u * sizeof(*overflow_frame));
    CHECK(overflow_frame != NULL
          && tile_cache_init(&overflow_cache, &budget,
                             &geometry_navigation.page.layout, 4)
          && tile_cache_set_frame(&overflow_cache, overflow_frame,
                                  480u * 272u)
          && layout_scroll_node(&geometry_navigation.page.layout, shell, 0, 0)
          && tile_cache_render_frame(&overflow_cache, 0, 480, 272, NULL));
    CHECK(overflow_frame[4u * 480u + 4u]
              == test_rgb565(geometry_navigation.page.layout.page_background)
          && overflow_frame[20u * 480u + 20u]
              == test_rgb565(0xaa0000)
          && layout_link_at(&geometry_navigation.page.layout, 4, 4) == NULL
          && layout_link_at(&geometry_navigation.page.layout, 20, 20)
              != NULL);
    int sibling_scroll = sibling_positioned_box->y;
    int maximum_document_scroll = geometry_navigation.page.layout.height - 272;
    if (maximum_document_scroll < 0) maximum_document_scroll = 0;
    if (sibling_scroll > maximum_document_scroll) {
        sibling_scroll = maximum_document_scroll;
    }
    if (sibling_scroll < 0) sibling_scroll = 0;
    int sibling_sample_x = sibling_positioned_box->x + 2;
    int sibling_sample_y = sibling_positioned_box->y - sibling_scroll + 2;
    CHECK(sibling_sample_x >= 0 && sibling_sample_x < 480
          && sibling_sample_y >= 0 && sibling_sample_y < 272
          && tile_cache_render_frame(
              &overflow_cache, sibling_scroll, 480, 272, NULL)
          && overflow_frame[(size_t) sibling_sample_y * 480u
                            + (size_t) sibling_sample_x]
             == test_rgb565(0x00ff00)
          && overflow_frame[(size_t) (sibling_sample_y + 8) * 480u
                            + (size_t) (sibling_sample_x + 8)]
             == test_rgb565(0xff8800)
          && tile_cache_render_frame(&overflow_cache, 0, 480, 272, NULL));
    uint16_t overflow_before = overflow_frame[70u * 480u + 20u];
    int nested_maximum = shell_box->content_height - shell_box->height;
    CHECK(layout_scroll_node(&geometry_navigation.page.layout, shell, 0,
                             nested_maximum)
          && tile_cache_render_frame(&overflow_cache, 0, 480, 272, NULL));
    uint16_t overflow_after = overflow_frame[70u * 480u + 20u];
    CHECK(overflow_before == test_rgb565(0xaa0000)
          && overflow_after == test_rgb565(0x0000aa)
          && overflow_cache.overflow_cache_builds == 1
          && overflow_cache.overflow_cache_fallbacks == 0
          && overflow_cache.overflow_cache_bytes > 0
          && overflow_cache.overflow_cache_bytes
             <= TILEFINCH_OVERFLOW_CACHE_BYTES
          && overflow_cache.overflow_direct_commands > 0);
    puts("test: overflow geometry acceleration has an exact OOM fallback");
    TileCache overflow_fallback_cache;
    uint16_t *overflow_fallback_frame = budget_malloc(
        &budget, 480u * 272u * sizeof(*overflow_fallback_frame));
    CHECK(overflow_fallback_frame != NULL
          && tile_cache_init(&overflow_fallback_cache, &budget,
                             &geometry_navigation.page.layout, 4)
          && tile_cache_set_frame(&overflow_fallback_cache,
                                  overflow_fallback_frame, 480u * 272u));
    budget_inject_failure_after(&budget, 0);
    CHECK(tile_cache_render_frame(&overflow_fallback_cache, 0, 480, 272,
                                  NULL)
          && overflow_fallback_cache.overflow_cache_builds == 0
          && overflow_fallback_cache.overflow_cache_fallbacks == 1
          && overflow_fallback_cache.overflow_direct_commands > 0
          && memcmp(overflow_fallback_frame, overflow_frame,
                    480u * 272u * sizeof(*overflow_frame)) == 0);
    budget_clear_failure_injection(&budget);
    tile_cache_destroy(&overflow_fallback_cache);
    budget_free(&budget, overflow_fallback_frame);
    tile_cache_destroy(&overflow_cache);
    budget_free(&budget, overflow_frame);
    puts("test: overflow accelerator obeys its persistent byte cap");
    const size_t oversized_node_box_count = 22000;
    LayoutNodeBox *oversized_node_boxes = budget_calloc(
        &budget, oversized_node_box_count, sizeof(*oversized_node_boxes));
    uint8_t oversized_overflow_flag = LAYOUT_COMMAND_OVERFLOW;
    DrawCommand oversized_overflow_command = {
        .x = 0, .y = 0, .width = 16, .height = 16,
        .color = 0x00aa00, .opacity_scale = 256,
        .type = DRAW_FILL_RECT
    };
    CHECK(oversized_node_boxes != NULL);
    oversized_node_boxes[0] = (LayoutNodeBox) {
        .x = 0, .y = 0, .width = 16, .height = 16,
        .client_width = 16, .client_height = 16,
        .clips_x = true, .clips_y = true,
        .scroll_command_start = 0, .scroll_command_end = 1
    };
    LayoutDocument oversized_overflow_layout = {
        .budget = &budget,
        .commands = &oversized_overflow_command,
        .count = 1,
        .width = 16,
        .scroll_width = 16,
        .height = 16,
        .page_background = 0xffffff,
        .node_boxes = oversized_node_boxes,
        .node_box_count = oversized_node_box_count,
        .command_flags = &oversized_overflow_flag
    };
    TileCache oversized_overflow_cache;
    uint16_t *oversized_overflow_frame = budget_malloc(
        &budget, 16u * 16u * sizeof(*oversized_overflow_frame));
    CHECK(oversized_overflow_frame != NULL
          && tile_cache_init(&oversized_overflow_cache, &budget,
                             &oversized_overflow_layout, 1)
          && tile_cache_set_frame(&oversized_overflow_cache,
                                  oversized_overflow_frame, 16u * 16u)
          && tile_cache_render_frame(&oversized_overflow_cache, 0, 16, 16,
                                     NULL)
          && oversized_overflow_cache.overflow_cache == NULL
          && oversized_overflow_cache.overflow_cache_bytes == 0
          && oversized_overflow_cache.overflow_cache_builds == 0
          && oversized_overflow_cache.overflow_cache_fallbacks == 1
          && oversized_overflow_cache.overflow_direct_commands == 1
          && oversized_overflow_frame[0] == test_rgb565(0x00aa00));
    tile_cache_destroy(&oversized_overflow_cache);
    budget_free(&budget, oversized_overflow_frame);
    budget_free(&budget, oversized_node_boxes);
    CHECK(layout_scroll_node(&geometry_navigation.page.layout, shell, 0, 0));
    static const char geometry_probe[] =
        "(()=>{const shell=document.querySelector('#shell'),"
        "second=document.querySelector('#second'),before=second."
        "getBoundingClientRect().top;shell.scrollTop=999;const after=second."
        "getBoundingClientRect().top;globalThis.pocSummary=[shell.clientWidth,"
        "shell.clientHeight,shell.scrollHeight,shell.scrollTop,before-after]"
        ".join(':');})();";
    size_t geometry_queries_before =
        geometry_navigation.page.script_result.geometry_queries;
    size_t geometry_fast_paths_before =
        geometry_navigation.page.script_result.geometry_retained_fast_paths;
    CHECK(script_runtime_evaluate_diagnostic(
        geometry_navigation.page.runtime, geometry_probe,
        "<geometry-probe>", &geometry_navigation.page.script_result));
    int client_width = 0, client_height = 0, scroll_height = 0;
    int scroll_top = 0, displacement = 0;
    int geometry_fields = sscanf(
        geometry_navigation.page.script_result.summary,
        "%d:%d:%d:%d:%d", &client_width, &client_height,
        &scroll_height, &scroll_top, &displacement);
    if (geometry_fields != 5
        || client_width != shell_box->width
        || client_height != shell_box->height
        || scroll_height != shell_box->content_height
        || scroll_top != shell_box->content_height - shell_box->height
        || displacement != scroll_top) {
        fprintf(stderr, "overflow geometry summary=\"%s\" parsed=%d "
                "expected=%d:%d:%d:%d:%d\n",
                geometry_navigation.page.script_result.summary,
                geometry_fields, shell_box->width, shell_box->height,
                shell_box->content_height,
                shell_box->content_height - shell_box->height,
                shell_box->content_height - shell_box->height);
    }
    CHECK(geometry_fields == 5
          && client_width == shell_box->width
          && client_height == shell_box->height
          && scroll_height == shell_box->content_height
          && scroll_top == shell_box->content_height - shell_box->height
          && displacement == scroll_top
          && geometry_navigation.page.script_result.geometry_queries
                 >= geometry_queries_before + 2
          && geometry_navigation.page.script_result
                 .geometry_retained_fast_paths
                 >= geometry_fast_paths_before + 2);
    navigation_destroy(&geometry_navigation);
    CHECK(budget.current == 0);

    puts("test: deferred capture-free function table semantics");
    static const char deferred_function_page[] =
        "<!doctype html><body><script>(()=>{"
        "function fill(source,key,table){"
        "table[key(source.a)]=function plus(value){return value+1};"
        "table[key(source.b)]=function times(value){return value*2};}"
        "const source={a:7,b:9},key=value=>value,observed={};"
        "fill(source,key,observed);const first=observed[7];"
        "const descriptor=Object.getOwnPropertyDescriptor(observed,'7');"
        "if(typeof first!=='function'||first!==observed[7]||"
        "descriptor.value!==first||first.name!=='plus'||first.length!==1||"
        "first(4)!==5||observed[9](4)!==8)throw new Error('observed table');"
        "const direct={};fill(source,key,direct);"
        "if(direct[7](10)!==11||direct[9](10)!==20)"
        "throw new Error('direct table');"
        "globalThis.pocSummary='deferred:ok';})();</script></body>";
    PocDocument deferred_function_document;
    ScriptResult deferred_function_result;
    CHECK(document_parse(&deferred_function_document, &budget,
                         deferred_function_page,
                         strlen(deferred_function_page), 16));
    CHECK(scripts_run_document(&deferred_function_document, &budget,
                               4 * MIB, 1000,
                               &deferred_function_result));
    CHECK(deferred_function_result.success &&
          strcmp(deferred_function_result.summary, "deferred:ok") == 0);
    document_destroy(&deferred_function_document);
    CHECK(budget.current == 0);

    puts("test: cached integer object literal semantics");
    static const char object_literal_page[] =
        "<!doctype html><body><script>(()=>{"
        "function make(){return {a:1000,b:-2000,c:3000,d:4000,e:5000,"
        "f:6000,g:7000,h:8000,i:9000,j:10000,k:11000,l:12000}}"
        "function mixed(){return {a:true,b:false,c:null,d:undefined,"
        "e:1.5,f:-1,g:0,h:7,i:40000}}"
        "const first=make(),second=make();"
        "if(first===second||first.a!==1000||first.b!==-2000||"
        "Object.keys(first).join(',')!=='a,b,c,d,e,f,g,h,i,j,k,l'||"
        "Object.getOwnPropertyDescriptor(first,'h').writable!==true)"
        "throw new Error('literal layout');"
        "first.a=77;if(second.a!==1000)throw new Error('literal alias');"
        "const mixedA=mixed(),mixedB=mixed();mixedA.a=false;"
        "if(mixedA===mixedB||mixedB.a!==true||mixedB.b!==false||"
        "mixedB.c!==null||mixedB.d!==undefined||mixedB.e!==1.5||"
        "mixedB.f!==-1||mixedB.i!==40000)throw new Error('mixed literal');"
        "let sum=0;for(let n=0;n<1000;n++)sum+=make().l;"
        "const duplicate={a:1,b:2,c:3,d:4,e:5,f:6,g:7,h:8,a:9};"
        "if(sum!==12000000||duplicate.a!==9||"
        "Object.keys(duplicate).join(',')!=='a,b,c,d,e,f,g,h')"
        "throw new Error('literal values');"
        "globalThis.pocSummary='literal:ok';})();</script></body>";
    PocDocument object_literal_document;
    ScriptResult object_literal_result;
    CHECK(document_parse(&object_literal_document, &budget,
                         object_literal_page,
                         strlen(object_literal_page), 16));
    CHECK(scripts_run_document(&object_literal_document, &budget,
                               4 * MIB, 1000,
                               &object_literal_result));
    CHECK(object_literal_result.success &&
          strcmp(object_literal_result.summary, "literal:ok") == 0);
    document_destroy(&object_literal_document);
    CHECK(budget.current == 0);

    puts("test: cached named-function object literal semantics");
    static const char named_function_literal_page[] =
        "<!doctype html><body><script>(()=>{"
        "function make(){return {add:(a,b)=>a+b,sub:(a,b)=>a-b,"
        "mul:(a,b)=>a*b,xor:(a,b)=>a^b,and:(a,b)=>a&b,or:(a,b)=>a|b,"
        "lt:(a,b)=>a<b,gt:(a,b)=>a>b,eq:(a,b)=>a===b,inc:a=>a+1}}"
        "const first=make(),second=make();"
        "if(first===second||first.add(4,5)!==9||first.mul(6,7)!==42||"
        "first.xor(10,3)!==9||first.lt(1,2)!==true||first.eq(3,3)!==true)"
        "throw Error('direct calls');"
        "const observed=first.add;"
        "const descriptor=Object.getOwnPropertyDescriptor(first,'add');"
        "if(typeof observed!=='function'||observed!==first.add||"
        "descriptor.value!==observed||observed.name!=='add'||"
        "observed.length!==2||observed(8,9)!==17)"
        "throw Error('materialization');"
        "first.sub=17;if(typeof second.sub!=='function'||second.sub(9,4)!==5)"
        "throw Error('fresh storage');"
        "if(Object.keys(second).join(',')!=="
        "'add,sub,mul,xor,and,or,lt,gt,eq,inc')throw Error('order');"
        "for(let i=0;i<1000;i++)if(make().inc(i)!==i+1)"
        "throw Error('repeat');"
        "globalThis.pocSummary='named-literal:ok';})();</script></body>";
    PocDocument named_function_literal_document;
    ScriptResult named_function_literal_result;
    CHECK(document_parse(&named_function_literal_document, &budget,
                         named_function_literal_page,
                         strlen(named_function_literal_page), 16));
    CHECK(scripts_run_document(&named_function_literal_document, &budget,
                               4 * MIB, 1000,
                               &named_function_literal_result));
    CHECK(named_function_literal_result.success &&
          strcmp(named_function_literal_result.summary,
                 "named-literal:ok") == 0);
    document_destroy(&named_function_literal_document);
    CHECK(budget.current == 0);

    puts("test: cached primitive string switch semantics");
    static const char string_switch_page[] =
        "<!doctype html><body><script>(()=>{"
        "function dispatch(value){switch(value){"
        "case '0':return 3;case '1':return 5;case '2':return 7;"
        "case '3':return 11;case '4':return 13;case '5':return 17;"
        "default:return 19}}"
        "let sum=0;for(let i=0;i<100000;i++){"
        "const value=String.fromCharCode(48+i%8);sum+=dispatch(value)}"
        "if(sum!==1175000||dispatch(1)!==19||dispatch('1')!==5)"
        "throw Error('string switch');"
        "globalThis.pocSummary='string-switch:ok';})();</script></body>";
    PocDocument string_switch_document;
    ScriptResult string_switch_result;
    CHECK(document_parse(&string_switch_document, &budget,
                         string_switch_page,
                         strlen(string_switch_page), 16));
    CHECK(scripts_run_document(&string_switch_document, &budget,
                               4 * MIB, 1000,
                               &string_switch_result));
    CHECK(string_switch_result.success &&
          strcmp(string_switch_result.summary,
                 "string-switch:ok") == 0);
    document_destroy(&string_switch_document);
    CHECK(budget.current == 0);

    puts("test: portable integer region guards and fallback semantics");
    static const char portable_region_page[] =
        "<!doctype html><body><script>(()=>{"
        "function mix(a,b){let x=(a^85)+b;if(x<0)x=-x;"
        "if((a&1)===0)x=x*3+7;else x=(x<<1)-5;return x>>>1}"
        "function sum(a,b){let x=a+b;return x}"
        "function neg(a){let x=-a;return x}"
        "function product(a,b){let x=a*b;return x}"
        "function unsigned(a,b){let x=a>>>b;return x}"
        "function compare(a,b){let strict=a===b;if(strict)return 7;"
        "if(a==b)return 9;return 11}"
        "let total=0;for(let i=0;i<300;i++){"
        "total+=mix(10,3)+mix(11,3)+mix(-10,3);sum(i,1);"
        "neg(i+1);product(i+1,2);unsigned(i,1);"
        "compare(i,i)}"
        "if(total!==114600||mix(10,3)!==150||mix(11,3)!==94||"
        "mix(-10,3)!==138)throw Error('portable values');"
        "if(sum(2147483647,1)!==2147483648||sum('PSP',1)!=='PSP1'||"
        "sum(true,1)!==2)throw Error('portable guard');"
        "if(!Object.is(neg(0),-0)||!Object.is(product(0,-1),-0)||"
        "unsigned(-1,0)!==4294967295)throw Error('portable number edge');"
        "if(compare(true,true)!==7||compare(true,1)!==9||"
        "compare(false,1)!==11)throw Error('portable equality');"
        "globalThis.pocSummary='portable-region:ok';})();</script></body>";
    PocDocument portable_region_document;
    ScriptResult portable_region_result;
    CHECK(document_parse(&portable_region_document, &budget,
                         portable_region_page,
                         strlen(portable_region_page), 16));
    CHECK(scripts_run_document(&portable_region_document, &budget,
                               4 * MIB, 1000,
                               &portable_region_result));
    CHECK(portable_region_result.success &&
          strcmp(portable_region_result.summary,
                 "portable-region:ok") == 0);
    document_destroy(&portable_region_document);
    CHECK(budget.current == 0);

    puts("test: portable integer loop differential semantics");
    static const char portable_loop_page[] =
        "<!doctype html><body><script>(()=>{"
        "function loop(n,seed){let x=seed;for(let i=0;i<n;i++){"
        "if((i&1)===0)x=x^i;else x=x+i}return x}"
        "const inputs=[[0,7],[1,7],[7,19],[31,12345]];"
        "const expected=inputs.map(pair=>loop(pair[0],pair[1]));"
        "for(let warm=0;warm<160;warm++)loop(24,warm&31);"
        "for(let i=0;i<inputs.length;i++){const pair=inputs[i];"
        "if(loop(pair[0],pair[1])!==expected[i])throw Error('loop diff')}"
        "if(loop(2,2147483647)!==2147483648)"
        "throw Error('loop overflow fallback');"
        "globalThis.pocSummary='portable-loop:ok';})();"
        "</script></body>";
    PocDocument portable_loop_document;
    ScriptResult portable_loop_result;
    CHECK(document_parse(&portable_loop_document, &budget,
                         portable_loop_page,
                         strlen(portable_loop_page), 16));
    CHECK(scripts_run_document(&portable_loop_document, &budget,
                               4 * MIB, 1000,
                               &portable_loop_result));
    if (!portable_loop_result.success ||
        strcmp(portable_loop_result.summary, "portable-loop:ok") != 0) {
        fprintf(stderr, "portable loop failed: error=%s summary=%s\n",
                portable_loop_result.error,
                portable_loop_result.summary);
    }
    CHECK(portable_loop_result.success &&
          strcmp(portable_loop_result.summary,
                 "portable-loop:ok") == 0);
    document_destroy(&portable_loop_document);
    CHECK(budget.current == 0);

    puts("test: portable integer loop preserves watchdog interrupts");
    static const char portable_loop_timeout_page[] =
        "<!doctype html><body><script>(()=>{"
        "function spin(n){let i=0;while(i<n)i++;return i}"
        "for(let warm=0;warm<80;warm++)spin(8);"
        "spin(2147483647);"
        "globalThis.pocSummary='watchdog-missed';})();"
        "</script></body>";
    PocDocument portable_loop_timeout_document;
    ScriptResult portable_loop_timeout_result;
    CHECK(document_parse(&portable_loop_timeout_document, &budget,
                         portable_loop_timeout_page,
                         strlen(portable_loop_timeout_page), 16));
    CHECK(!scripts_run_document(&portable_loop_timeout_document, &budget,
                                4 * MIB, 20,
                                &portable_loop_timeout_result));
    CHECK(portable_loop_timeout_result.interrupted &&
          strstr(portable_loop_timeout_result.error,
                 "interrupted") != NULL);
    document_destroy(&portable_loop_timeout_document);
    CHECK(budget.current == 0);

    puts("test: portable dense and typed array differential semantics");
    static const char portable_array_page[] =
        "<!doctype html><body><script>(()=>{"
        "function checksum(a,n){let x=0;for(let i=0;i<n;i++)"
        "x=(x+a[i])|0;return x}"
        "function mutate(a,n,seed){for(let i=0;i<n;i++)"
        "a[i]=(a[i]+seed+i)&255;return a[0]+a[n-1]}"
        "function writeOne(a,v){a[0]=v;return a[0]}"
        "function lateGuard(a,v){a[0]=a[0]+1;return v+1}"
        "function lateBounds(a){a[0]=a[0]+1;a[99]=5;return a[0]}"
        "const dense=[3,5,7,11,13,17,19,23];"
        "const bytes=new Uint8Array(dense);"
        "for(let warm=0;warm<100;warm++){checksum(dense,8);"
        "checksum(bytes,8)}"
        "if(checksum(dense,8)!==98||checksum(bytes,8)!==98)"
        "throw Error('array read');"
        "for(let warm=0;warm<100;warm++){const a=[1,2,3,4];"
        "mutate(a,4,warm&3)}"
        "const changed=[250,2,3,4];"
        "if(mutate(changed,4,10)!==21||changed.join(',')!=="
        "'4,13,15,17')throw Error('array write');"
        "const u8=new Uint8Array([250]);"
        "for(let warm=0;warm<100;warm++)writeOne(u8,warm&255);"
        "if(writeOne(u8,260)!==4||u8[0]!==4)throw Error('u8 write');"
        "const i8=new Int8Array([0]);"
        "if(writeOne(i8,128)!==-128||i8[0]!==-128)"
        "throw Error('i8 write');"
        "const clamped=new Uint8ClampedArray([0]);"
        "if(writeOne(clamped,300)!==255||clamped[0]!==255)"
        "throw Error('clamped write');"
        "const rollback=[10];"
        "for(let warm=0;warm<100;warm++)lateGuard([1],warm);"
        "if(lateGuard(rollback,'x')!=='x1'||rollback[0]!==11)"
        "throw Error('late type guard');"
        "const large=new Array(100).fill(1);"
        "for(let warm=0;warm<100;warm++)lateBounds(large);"
        "const short=[10];"
        "if(lateBounds(short)!==11||short[0]!==11||short[99]!==5||"
        "short.length!==100)throw Error('late bounds guard');"
        "const upper=new Uint32Array([4294967295]);"
        "if(checksum(upper,1)!==-1)throw Error('uint32 fallback');"
        "globalThis.pocSummary='portable-array:ok';})();"
        "</script></body>";
    PocDocument portable_array_document;
    ScriptResult portable_array_result;
    CHECK(document_parse(&portable_array_document, &budget,
                         portable_array_page,
                         strlen(portable_array_page), 16));
    CHECK(scripts_run_document(&portable_array_document, &budget,
                               4 * MIB, 1000,
                               &portable_array_result));
    if (!portable_array_result.success) {
        fprintf(stderr, "portable array failed: %s\n",
                portable_array_result.error);
    }
    CHECK(portable_array_result.success &&
          strcmp(portable_array_result.summary,
                 "portable-array:ok") == 0);
    document_destroy(&portable_array_document);
    CHECK(budget.current == 0);

    puts("test: decoder memoization invalidation semantics");
    static const char decoder_cache_page[] =
        "<!doctype html><body><script>(()=>{"
        "const data=['alpha','beta','gamma'];"
        "function E(){return data}"
        "function D(V,m,C,L){return V=V-1,C=E(),L=C[V],L}"
        "for(let i=0;i<1000;i++)if(D(1)!=='alpha')throw Error('warm');"
        "data[0]='changed';if(D(1)!=='changed')throw Error('set');"
        "data.splice(1,1,'spliced');if(D(2)!=='spliced')throw Error('splice');"
        "data.copyWithin(0,1,2);if(D(1)!=='spliced')throw Error('copy');"
        "data.push('tail');if(D(4)!=='tail')throw Error('push');"
        "globalThis.pocSummary='decoder-cache:ok';})();</script></body>";
    PocDocument decoder_cache_document;
    ScriptResult decoder_cache_result;
    CHECK(document_parse(&decoder_cache_document, &budget,
                         decoder_cache_page,
                         strlen(decoder_cache_page), 16));
    CHECK(scripts_run_document(&decoder_cache_document, &budget,
                               4 * MIB, 1000,
                               &decoder_cache_result));
    CHECK(decoder_cache_result.success &&
          strcmp(decoder_cache_result.summary, "decoder-cache:ok") == 0);
    document_destroy(&decoder_cache_document);
    CHECK(budget.current == 0);
