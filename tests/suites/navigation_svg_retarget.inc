/* Display retargeting of SVG rasters: they keep their markup and are
   rasterized again at the size they are painted. Every picture is markup
   written here: vertical stripes (32 of 256 units each, so their edges fall
   on whole pixels at 20 and 40 px) and a two-colour sprite sheet. */

#define SVG_RETARGET_STRIPES(fill)                                          \
    "<rect width='256' height='256' fill='#ffffff'/>"                       \
    "<rect x='0' width='32' height='256' fill='" fill "'/>"                 \
    "<rect x='64' width='32' height='256' fill='" fill "'/>"                \
    "<rect x='128' width='32' height='256' fill='" fill "'/>"               \
    "<rect x='192' width='32' height='256' fill='" fill "'/>"

/* Red left half, blue right half, percent-encoded for a data: URL. */
#define SVG_RETARGET_SHEET                                                  \
    "data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' "        \
    "width='400' height='200' viewBox='0 0 400 200'%3E"                     \
    "%3Crect width='200' height='200' fill='%23ff0000'/%3E"                 \
    "%3Crect x='200' width='200' height='200' fill='%230000ff'/%3E"         \
    "%3C/svg%3E"

static const unsigned char *svg_retarget_pixel(const ImageResource *item,
                                                int x, int y)
{
    return item->pixels + ((size_t) y * (size_t) item->width
                           + (size_t) x) * 4u;
}

/* The surface is its markup rasterized straight at the surface's size (not
   a scaled copy of another raster), and a stripe edge that falls on a whole
   pixel is hard: one side fully dark, the other fully light. */
static bool svg_retarget_sharp(Budget *budget, const ImageResource *item,
                               int edge)
{
    if (item == NULL || item->pixels == NULL || item->encoded == NULL
        || item->width <= edge) return false;
    int width = item->width, height = item->height;
    unsigned char *direct = image_svg_decode(
        item->encoded, item->encoded_length, budget,
        (size_t) width * (size_t) height * 4u, &width, &height);
    bool same = direct != NULL && width == item->width
        && height == item->height
        && memcmp(direct, item->pixels,
                  (size_t) width * (size_t) height * 4u) == 0;
    budget_free(budget, direct);
    const unsigned char *before =
        svg_retarget_pixel(item, edge - 1, item->height / 2);
    const unsigned char *after =
        svg_retarget_pixel(item, edge, item->height / 2);
    int step = (int) after[0] - (int) before[0];
    return same && (step > 239 || step < -239)
        && before[3] == 255 && after[3] == 255;
}

static void svg_retarget_report(NavigationSession *navigation)
{
    const ImageResources *images = &navigation->page.images;
    fprintf(stderr, "  decoded=%zu shrinks=%zu grows=%zu drops=%zu "
            "rasters=%zu inline=%zu reused=%zu\n", images->stats.decoded_bytes,
            images->retarget.shrinks, images->retarget.grows,
            images->retarget.drops, images->retarget.rasters,
            images->stats.inline_svg_rasterized,
            images->stats.inline_svg_refresh_reused);
    for (size_t c = 0; c < navigation->page.layout.count; c++) {
        const DrawCommand *command = &navigation->page.layout.commands[c];
        if (command->type != DRAW_IMAGE || command->image == NULL) continue;
        fprintf(stderr, "  command image %d %dx%d fit %d tile %dx%d\n",
                (int) (command->image - images->items), command->width,
                command->height, (int) command->image_fit,
                draw_command_image_sprite_width(command),
                draw_command_image_sprite_height(command));
    }
    for (size_t i = 0; i < images->count; i++) {
        const ImageResource *item = &images->items[i];
        fprintf(stderr, "  image %zu: %dx%d full %dx%d source %dx%d "
                "pixels=%d markup=%zu flags=%u\n", i, item->width,
                item->height, item->full_width, item->full_height,
                item->source_width, item->source_height,
                item->pixels != NULL, item->encoded_length,
                (unsigned) item->retarget_flags);
    }
}

/* The <svg> inside the element with this id (its own attributes, and so
   its markup, never change). */
static const ImageResource *svg_retarget_icon(NavigationSession *navigation,
                                              const char *id)
{
    lxb_dom_node_t *root =
        lxb_dom_interface_node(navigation->page.document.html);
    lxb_dom_node_t *node = root == NULL ? NULL : test_find_id(root, id);
    node = node == NULL ? NULL : node->first_child;
    return node == NULL ? NULL
        : images_find_node(&navigation->page.images, node);
}

/* Icons with a 256-unit viewBox (two inline, one a data: <img>) painted at
   20 px keep 20x20 rasters, and a sprite sheet painted through a 20 px tile
   of its 40x20 background size keeps the whole sheet at 40x20. A hidden
   icon releases its raster and keeps its markup. Painting an icon at 40 px
   rasterizes the markup at 40 px (the same pixels as rasterizing it there
   directly, with hard stripe edges a 20 px raster enlarged cannot have),
   and alternating sizes never reduces it again. Showing the hidden icon
   rasterizes it at 20 px. A mutation that rebuilds the image table takes
   the resized rasters over without rasterizing anything, and zooming in 2x
   rasterizes a 20 px icon at 40 device pixels. Teardown leaves no bytes
   behind. */
static bool test_svg_retarget_rasterizes_at_painted_size(void)
{
    static const char html[] =
        "<!doctype html><title>SVG retarget</title>"
        "<meta name=viewport content=\"width=device-width\">"
        "<style>html,body{margin:0}svg,img{display:block;width:20px;"
        "height:20px}#big svg{width:40px;height:40px}"
        "#hidden svg,#shut img{display:none}"
        "#sprite{width:20px;height:20px;background-image:url(\""
        SVG_RETARGET_SHEET "\");background-position:-20px 0;"
        "background-size:40px 20px;background-repeat:no-repeat}</style>"
        "<div id=icon><svg width=256 height=256 viewBox='0 0 256 256'>"
        SVG_RETARGET_STRIPES("#000000") "</svg></div>"
        "<div id=toggle><svg width=256 height=256 viewBox='0 0 256 256'>"
        SVG_RETARGET_STRIPES("#008000") "</svg></div>"
        "<div id=shelf><img id=fetched src=\"data:image/svg+xml,%3Csvg "
        "xmlns='http://www.w3.org/2000/svg' width='256' height='256' "
        "viewBox='0 0 256 256'%3E%3Ccircle cx='128' cy='128' r='120' "
        "fill='%23000080'/%3E%3C/svg%3E\"></div>"
        "<div id=sprite></div><div id=box><img src=\"data:image/png;base64,"
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwH"
        "wAFAAH/iZk9HQAAAABJRU5ErkJggg==\"></div>";
    Budget budget;
    budget_init(&budget, 24 * MIB);
    bool installed = budget_install_lexbor(&budget);
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool browser_ready = installed
        && browser_session_init(&browser, &budget, 64 * 1024);
    bool ready = browser_ready && navigation_init(&navigation, &budget, 4);
    if (ready) {
        navigation_attach_browser_session(&navigation, &browser);
        navigation_enable_scripts(&navigation, 4 * MIB, 1000);
        navigation_enable_external_resources(
            &navigation, 2, 32 * 1024, 16 * 1024,
            8, 512 * 1024, 256 * 1024, 4 * MIB, 1000);
    }
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    bool committed = ready && navigation_commit_html(
        &navigation, generation, "https://svg-retarget.test/page",
        html, sizeof(html) - 1u, 480, NULL, NULL, true);
    if (committed) retarget_pump(&navigation);
    const ImageResource *icon = committed
        ? svg_retarget_icon(&navigation, "icon") : NULL;
    const ImageResource *toggle = committed
        ? svg_retarget_icon(&navigation, "toggle") : NULL;
    const ImageResource *fetched = committed
        ? retarget_image(&navigation, "fetched") : NULL;
    const ImageResource *sheet = NULL;
    for (size_t i = 0; committed && i < navigation.page.images.count; i++) {
        if (navigation.page.images.items[i].is_background)
            sheet = &navigation.page.images.items[i];
    }
    const size_t icon_bytes = 20u * 20u * 4u;
    bool reduced = icon != NULL && toggle != NULL && fetched != NULL
        && sheet != NULL && icon->pixels != NULL
        && icon->width == 20 && icon->height == 20
        && icon->source_width == 256 && icon->full_width == 256
        && (icon->retarget_flags & IMAGE_RETARGET_VECTOR) != 0
        && (icon->retarget_flags & IMAGE_RETARGET_SHRUNK) != 0
        && toggle->width == 20 && fetched->width == 20
        && fetched->height == 20 && fetched->source_width == 256
        && sheet->width == 40 && sheet->height == 20
        && sheet->source_width == 400
        && svg_retarget_pixel(sheet, 10, 10)[0] == 255
        && svg_retarget_pixel(sheet, 30, 10)[2] == 255
        && svg_retarget_pixel(sheet, 30, 10)[0] == 0
        && navigation.page.images.stats.decoded_bytes
               == 3u * icon_bytes + 40u * 20u * 4u + 4u
        && navigation.page.images.retarget.rasters == 4;
    if (!reduced) svg_retarget_report(&navigation);

    bool dropped = committed
        && retarget_set_id(&navigation, "toggle", "hidden")
        && retarget_relayout_at(&navigation, 480);
    const ImageResource *hidden = dropped
        ? svg_retarget_icon(&navigation, "hidden") : NULL;
    dropped = hidden != NULL && hidden->pixels == NULL
        && hidden->encoded != NULL && image_resource_available(hidden)
        && (hidden->retarget_flags & IMAGE_RETARGET_DROPPED) != 0
        && navigation.page.images.stats.decoded_bytes
               == 2u * icon_bytes + 40u * 20u * 4u + 4u;
    if (!dropped) svg_retarget_report(&navigation);

    bool grown = committed && retarget_set_id(&navigation, "icon", "big")
        && retarget_relayout_at(&navigation, 480);
    icon = grown ? svg_retarget_icon(&navigation, "big") : NULL;
    grown = icon != NULL && icon->width == 40 && icon->height == 40
        && (icon->retarget_flags & IMAGE_RETARGET_GROWN) != 0
        && svg_retarget_sharp(&budget, icon, 5)
        && navigation.page.images.retarget.rasters == 5;
    if (!grown) svg_retarget_report(&navigation);
    bool stable = grown;
    size_t rasters = navigation.page.images.retarget.rasters;
    for (int round = 0; stable && round < 3; round++) {
        stable = retarget_set_id(&navigation, "big", "icon")
            && retarget_relayout_at(&navigation, 480)
            && retarget_set_id(&navigation, "icon", "big")
            && retarget_relayout_at(&navigation, 480);
    }
    icon = stable ? svg_retarget_icon(&navigation, "big") : NULL;
    stable = icon != NULL && icon->width == 40
        && navigation.page.images.retarget.rasters == rasters;

    bool restored = committed
        && retarget_set_id(&navigation, "hidden", "toggle")
        && retarget_relayout_at(&navigation, 480);
    toggle = restored ? svg_retarget_icon(&navigation, "toggle") : NULL;
    restored = toggle != NULL && toggle->pixels != NULL
        && toggle->width == 20 && toggle->height == 20
        && svg_retarget_sharp(&budget, toggle, 5);

    /* Hide the data: icon (released), then remove an image-bearing subtree
       from script: the rebuilt table takes over the 40 px icon, the 20 px
       one and the sheet with their markup and sizes instead of rasterizing
       them at their own sizes again (the hidden icon is not loaded). */
    bool rebuilt = restored
        && retarget_set_id(&navigation, "shelf", "shut")
        && retarget_relayout_at(&navigation, 480);
    fetched = rebuilt ? retarget_image(&navigation, "fetched") : NULL;
    rebuilt = fetched != NULL && fetched->pixels == NULL;
    rasters = navigation.page.images.retarget.rasters;
    size_t image_rebuilds = navigation.performance.mutation_image_rebuilds;
    rebuilt = rebuilt && append_order_mutate(
        &navigation, "document.getElementById('box').remove()",
        "<svg-retarget-rebuild>");
    retarget_pump(&navigation);
    icon = rebuilt ? svg_retarget_icon(&navigation, "big") : NULL;
    toggle = rebuilt ? svg_retarget_icon(&navigation, "toggle") : NULL;
    fetched = rebuilt ? retarget_image(&navigation, "fetched") : NULL;
    rebuilt = icon != NULL && toggle != NULL && fetched == NULL
        && navigation.performance.mutation_image_rebuilds
               == image_rebuilds + 1
        && navigation.page.images.stats.inline_svg_rasterized == 0
        && navigation.page.images.stats.inline_svg_refresh_reused == 3
        && icon->width == 40 && icon->encoded != NULL
        && toggle->width == 20 && toggle->pixels != NULL
        && navigation.page.images.stats.decoded_bytes
               == 40u * 40u * 4u + icon_bytes + 40u * 20u * 4u
        && navigation.page.images.retarget.rasters == rasters;
    if (!rebuilt) svg_retarget_report(&navigation);

    /* Zoomed in 2x (480 device pixels show 240 CSS pixels), the 20 px icon
       is painted at 40 device pixels and rasterized there. */
    bool zoomed = rebuilt
        && viewport_context_init(&navigation.viewport, 240, 136, 480, 272)
        && navigation_relayout(&navigation);
    if (zoomed) retarget_pump(&navigation);
    toggle = zoomed ? svg_retarget_icon(&navigation, "toggle") : NULL;
    zoomed = toggle != NULL && toggle->width == 40 && toggle->height == 40
        && svg_retarget_sharp(&budget, toggle, 5);
    if (!zoomed) svg_retarget_report(&navigation);

    bool ok = committed && reduced && dropped && grown && stable && restored
        && zoomed && rebuilt;
    if (!ok) {
        fprintf(stderr, "svg-retarget committed=%d reduced=%d dropped=%d "
                "grown=%d stable=%d restored=%d zoomed=%d rebuilt=%d "
                "error=\"%s\"\n", committed, reduced, dropped, grown, stable,
                restored, zoomed, rebuilt, navigation.last_error);
    }
    if (ready) navigation_destroy(&navigation);
    if (browser_ready) browser_session_destroy(&browser);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
    if (!clean) fprintf(stderr, "svg-retarget: %zu bytes after teardown\n",
                        budget.current);
    return ok && clean;
}
