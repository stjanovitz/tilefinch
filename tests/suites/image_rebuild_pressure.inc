static bool test_reused_svg_keeps_attempt_budget(Budget *budget)
{
    static const char html[] = "<svg id=kept width=96 height=96>"
        "<rect width=96 height=96 fill='blue'/></svg>";
    size_t baseline = budget->current;
    PocDocument document = {0};
    Stylesheet sheet = {0};
    ImageResources images = {0};
    bool ok = document_parse(&document, budget, html, sizeof(html)-1, 17)
        && stylesheet_build(&sheet, budget, &document, 480)
        && images_load_external(&document, &sheet, &images, budget,
            "https://reuse.test/", "https://reuse.test/", NULL,
            1, 65536, 65536, MIB, 1000, NULL, NULL);
    lxb_dom_node_t *node = ok ? find_id(lxb_dom_interface_node(document.html), "kept") : NULL;
    const ImageResource *initial = images_find_node(&images, node);
    const unsigned char *pixels = initial == NULL ? NULL : initial->pixels;
    ok = ok && pixels != NULL && images.stats.attempted == 1
        && images.stats.count_exempt == 0;
    for (unsigned i = 0; ok && i < 3; i++) {
        ok = images_rebuild_external_reusing_rasters(&document, &sheet, &images,
            budget, "https://reuse.test/", "https://reuse.test/", NULL,
            1, 65536, 65536, MIB, 1000, NULL, NULL, NULL, NULL, 480, NULL, 0);
        const ImageResource *kept = images_find_node(&images, node);
        ok = ok && kept != NULL && kept->pixels == pixels
            && images.stats.attempted == 1 && images.stats.count_exempt == 1
            && images.stats.inline_svg_refresh_reused == 1
            && images.stats.inline_svg_rasterized == 0;
    }
    for (unsigned i = 0; ok && i < 3; i++) {
        ok = svg_refresh(&document, &sheet, &images, budget, &node, 1)
            && images.stats.attempted == 1 && images.stats.count_exempt == 1
            && images_find_node(&images, node)->pixels == pixels;
    }
    size_t admitted_bytes = images.stats.decoded_bytes;
    if (ok) {
        ok = images_refresh_external_nodes(&document, &sheet, &images,
            &node, 1u, budget, "https://reuse.test/", "https://reuse.test/",
            NULL, 1, 65536, 65536, 1, 1000, NULL, NULL);
        const ImageResource *kept = images_find_node(&images, node);
        ok = ok && kept != NULL && kept->pixels == pixels
            && images.stats.decoded_bytes == admitted_bytes;
    }
    if (!ok) fprintf(stderr, "reuse admission: nodes=%zu attempted=%zu exempt=%zu loaded=%zu reused=%zu rasterized=%zu\n",
        images.count, images.stats.attempted, images.stats.count_exempt,
        images.stats.loaded, images.stats.inline_svg_refresh_reused,
        images.stats.inline_svg_rasterized);
    images_destroy(&images);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    if (!ok) fprintf(stderr, "reused SVG spent a fresh attempt or lost its surface\n");
    return ok && budget->current == baseline;
}

/* Separate entry point for sanitizer checks without unrelated VM benchmarks. */
static int test_image_rebuild_pressure(void)
{
    Budget budget;
    budget_init(&budget, 16 * MIB);
    budget_install_lexbor(&budget);
    CHECK(test_reused_svg_keeps_attempt_budget(&budget));
    puts("test: a real image-rebuild refusal preserves static content");
    static const char page[] =
        "<!doctype html><style>body{margin:0}p{height:200px}</style>"
        "<body><a id=keep href=/next>Keep</a><p>Readable</p>"
        "<svg id=art width=8 height=8 viewBox='0 0 8 8'>"
        "<rect width='8' height='8' fill='blue'/></svg><p>Last paragraph</p>"
        "<x-held id=component></x-held><script>"
        "const retainedRoot=document.getElementById('component').attachShadow({mode:'open'});"
        "retainedRoot.innerHTML='<p class=held>Styled</p>';"
        "const retainedSheet=new CSSStyleSheet();retainedSheet.replaceSync('.held{color:#123456}');"
        "retainedRoot.adoptedStyleSheets=[retainedSheet];</script>";
    static NavigationSession session;
    CHECK(navigation_init(&session, &budget, 2));
    navigation_enable_scripts(&session, 4 * MIB, 1000);
    navigation_enable_external_resources(&session, 2, 32768, 32768,
                                        4, 65536, 65536, 4 * MIB, 1000);
    CHECK(navigation_commit_html(&session, navigation_begin(&session),
        "https://image-pressure.test/", page, strlen(page),
        480, NULL, NULL, true));
    BrowserController controller;
    CHECK(controller_init(&controller, &session));
    CHECK(late_init_text_color(&session.page.layout, "Styled") == 0x123456);
    lxb_dom_node_t *keep = find_id(
        lxb_dom_interface_node(session.page.document.html), "keep");
    CHECK(controller_focus_node(&controller, keep));
    navigation_set_scroll(&session, 100);
    CHECK(script_runtime_evaluate_diagnostic(session.page.runtime,
        "const art=document.getElementById('art');"
        "art.setAttribute('width','768');art.setAttribute('height','768')",
        "<image-pressure-update>", &session.page.script_result));
    session.style_rebuild_required = true;
    size_t old_limit = budget.limit, failures = budget.failure_count;
    CHECK(budget_remaining(&budget) > 128 * 1024);
    budget.limit = budget.current + 128 * 1024;
    CHECK(navigation_relayout(&session));
    CHECK(budget.failure_count > failures && session.page.loaded
          && session.page.runtime == NULL && session.page.script_memory_limited
          && !session.page.stylesheet.static_custom_element_fallback
          && navigation_current(&session)->scroll_y == 100
          && controller_focused_node(&controller) == keep);
    const ImageResource *image = images_find_node(&session.page.images,
        find_id(lxb_dom_interface_node(session.page.document.html), "art"));
    CHECK(image_resource_available(image));
    /* Recovery may retain reproducible source rather than a provisional
       full raster. Verify visible output, not which representation owns it. */
    DrawCommand image_command = {
        .type = DRAW_IMAGE, .image = image, .width = 8, .height = 8,
        .opacity_scale = 256, .image_fit = LAYOUT_IMAGE_FIT_STRETCH
    };
    LayoutDocument image_layout = {
        .budget = &budget, .commands = &image_command, .count = 1,
        .width = 8, .height = 8, .scroll_width = 8,
        .page_background = 0xffffff
    };
    uint16_t image_frame[64] = {0};
    TileCache image_cache;
    CHECK(tile_cache_init(&image_cache, &budget, &image_layout, 1)
          && tile_cache_set_frame(&image_cache, image_frame, 64)
          && tile_cache_render_frame(&image_cache, 0, 8, 8, NULL));
    for (size_t i = 0; i < 64; i++) CHECK(image_frame[i] == test_rgb565(0x0000ff));
    tile_cache_destroy(&image_cache);
    CHECK(late_init_text_color(&session.page.layout, "Readable") != UINT32_MAX);
    CHECK(late_init_text_color(&session.page.layout, "Styled") == 0x123456);
    CHECK(controller_focus_node(&controller, keep));
    navigation_destroy(&session);
    budget.limit = old_limit;
    CHECK(budget.current == 0);
    return 0;
}
