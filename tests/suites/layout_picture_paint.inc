/* Authored, resource-independent picture coverage: discovery chooses an
   image resource, while layout must paint that resource through an inline
   picture without treating its source elements as boxes. */
static int test_picture_replaced_paint(void)
{
    enum { WIDTH = 64, HEIGHT = 48 };
    static const char page[] =
        "<!doctype html><style>html,body{margin:0;background:#fff}"
        "#media{width:24px;min-height:8px;max-height:8px;overflow:hidden;"
        "position:relative;background:#808080}"
        "img{display:block;width:100%;height:12px;object-fit:cover;"
        "object-position:50% 0}</style><body><div id=media><picture>"
        "<source srcset='small.png 1x, large.png 2x'>"
        "<img id=photo src=fallback.png></picture></div></body>";
    static unsigned char pixels[] = {
        255, 0, 0, 255, 0, 255, 0, 255,
        0, 0, 255, 255, 255, 255, 0, 255
    };
    unsigned char original[sizeof(pixels)];
    memcpy(original, pixels, sizeof(pixels));
    Budget budget;
    budget_init(&budget, 4u * MIB);
    budget_install_lexbor(&budget);
    PocDocument document = {0};
    Stylesheet sheet = {0};
    LayoutDocument layout = {0};
    CHECK(document_parse(&document, &budget, page, sizeof(page) - 1u, 64)
          && stylesheet_build(&sheet, &budget, &document, WIDTH));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    lxb_dom_node_t *photo = find_id(root, "photo");
    ImageResource image = {
        .node = photo, .pixels = pixels, .width = 2, .height = 2
    };
    ImageResources images = {.items = &image, .count = 1};
    CHECK(photo != NULL
          && layout_build(&layout, &budget, &document, &sheet,
                          NULL, &images, WIDTH));
    const LayoutNodeBox *box = layout_box_for_node(&layout, photo);
    const LayoutNodeBox *media = layout_box_for_node(
        &layout, find_id(root, "media"));
    CHECK(box != NULL && box->width == 24 && box->height == 12
          && media != NULL && media->width == 24 && media->height == 8);
    size_t painted = 0;
    for (size_t i = 0; i < layout.count; i++) {
        const DrawCommand *command = &layout.commands[i];
        if (command->type != DRAW_IMAGE || command->image != &image) continue;
        CHECK(command->width == 24 && command->height == 12
              && command->image_fit == STYLE_OBJECT_FIT_COVER);
        painted++;
    }
    CHECK(painted == 1u);
    TileCache cache = {0};
    uint16_t frame[WIDTH * HEIGHT];
    CHECK(tile_cache_init(&cache, &budget, &layout, 2)
          && tile_cache_set_frame(&cache, frame, WIDTH * HEIGHT)
          && tile_cache_render_frame(&cache, 0, WIDTH, HEIGHT, NULL));
    /* Cover with top alignment keeps the authored top colors; hidden
       overflow clips the image at the wrapper rather than painting its
       bottom four rows into following content. */
    CHECK(tilefinch_rgb565_red_code(frame[WIDTH + 2]) > 25u
          && tilefinch_rgb565_blue_code(frame[WIDTH + 2]) < 5u
          && tilefinch_rgb565_green_code(frame[WIDTH + 21]) > 50u
          && tilefinch_rgb565_blue_code(frame[WIDTH + 21]) < 5u
          && frame[10u * WIDTH + 2u]
                 == tilefinch_rgb565_pack_codes(31u, 63u, 31u)
          && memcmp(pixels, original, sizeof(pixels)) == 0);
    tile_cache_destroy(&cache);
    layout_destroy(&layout);
    size_t warmed = budget.current;
    for (size_t refused_after = 0; refused_after < 48u; refused_after++) {
        budget_inject_failure_after(&budget, refused_after);
        (void) layout_build(&layout, &budget, &document, &sheet,
                           NULL, &images, WIDTH);
        budget_clear_failure_injection(&budget);
        layout_destroy(&layout);
        CHECK(budget.current == warmed
              && memcmp(pixels, original, sizeof(pixels)) == 0);
    }
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0u);
    return 0;
}
