/* Display retargeting of SVG rasters: they keep their markup and are
   rasterized again at the size they are painted. Every picture is markup
   written here: vertical stripes (32 of 256 units each, so their edges fall
   on whole pixels at 20 and 40 px) and a two-colour sprite sheet. */

static bool test_nested_svg_viewports_preserve_atlas_geometry(void)
{
    static const char markup[] =
        "<svg xmlns='http://www.w3.org/2000/svg' width='80' height='80' "
        "viewBox='0 0 80 80'>"
        "<svg x='10' y='5' width='20' height='20' viewBox='-10 -10 20 20'>"
        "<rect x='-10' y='-10' width='20' height='20' fill='#ff0000'/></svg>"
        "<svg x='50%' y='40' width='30' height='20' viewBox='0 0 10 10' "
        "preserveAspectRatio='none'><rect width='10' height='10' fill='#0000ff'/></svg>"
        "<svg x='10' y='60' width='30' height='10' viewBox='0 0 10 10'>"
        "<rect width='10' height='10' fill='#00ff00'/></svg>"
        "<g transform='translate(2 2)'><svg x='40' y='60' width='20' height='10' "
        "viewBox='0 0 20 10'><svg x='4' y='2' width='6' height='6' "
        "viewBox='0 0 6 6'><rect width='6' height='6' fill='#00ffff'/>"
        "</svg></svg></g>"
        "<rect x='70' y='70' width='5' height='5' fill='#ff00ff'/>"
        "</svg>";
    Budget budget;
    budget_init(&budget, 4u * MIB);
    int width = 0, height = 0;
    unsigned char *pixels = image_svg_decode(
        markup, sizeof(markup) - 1, &budget, MIB, &width, &height);
    bool okay = pixels != NULL && width == 80 && height == 80;
    static const int samples[][5] = {
        {15, 10, 255, 0, 0}, {45, 45, 0, 0, 255},
        {25, 65, 0, 255, 0}, {72, 72, 255, 0, 255}, {48, 66, 0, 255, 255}
    };
    for (size_t i = 0; okay && i < sizeof(samples) / sizeof(samples[0]); i++) {
        const unsigned char *p = pixels
            + ((size_t) samples[i][1] * (size_t) width
               + (size_t) samples[i][0]) * 4u;
        okay = p[0] == samples[i][2] && p[1] == samples[i][3]
              && p[2] == samples[i][4] && p[3] == 255;
    }
    okay = okay && pixels[((size_t) 65 * 80u + 12u) * 4u + 3u] == 0;
    budget_free(&budget, pixels);
    if (!okay) fprintf(stderr, "nested SVG geometry mismatch %dx%d\n", width, height);
    /* Refuse source, viewport-context, parser, and early shape scratch
       allocations independently; every failed decode unwinds its owner. */
    for (size_t failure = 0; okay && failure < 24; failure++) {
        budget_inject_failure_after(&budget, failure);
        width = height = 0;
        pixels = image_svg_decode(
            markup, sizeof(markup) - 1, &budget, MIB, &width, &height);
        budget_clear_failure_injection(&budget);
        if (failure < 2 && pixels != NULL) okay = false;
        budget_free(&budget, pixels);
        okay = okay && budget.current == 0;
    }
    char nested[4096];
    for (unsigned depth = 32; okay && depth <= 33; depth++) {
        size_t length = 0;
        for (unsigned i = 0; i < depth; i++) {
            length += (size_t) snprintf(nested + length, sizeof(nested) - length,
                "<svg width='8' height='8' viewBox='0 0 8 8'>");
        }
        length += (size_t) snprintf(nested + length, sizeof(nested) - length,
            "<rect width='8' height='8' fill='#00ff00'/>");
        for (unsigned i = 0; i < depth; i++) {
            length += (size_t) snprintf(nested + length, sizeof(nested) - length, "</svg>");
        }
        width = height = 0;
        pixels = image_svg_decode(nested, length, &budget, MIB, &width, &height);
        okay = depth == 32 ? pixels != NULL && width == 8 && height == 8
                           : pixels == NULL;
        budget_free(&budget, pixels);
        okay = okay && budget.current == 0;
    }
    return okay && budget.current == 0;
}

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
static bool svg_oversize_test_target(void *opaque, int source_width,
                                      int source_height, int *width,
                                      int *height)
{
    size_t *calls = opaque;
    (*calls)++;
    if (source_width != 3000 || source_height != 1000) return false;
    *width = 72;
    *height = 24;
    return true;
}

static bool test_svg_oversize_initial_raster_preserves_intrinsic_size(void)
{
    static const char html[] =
        "<!doctype html><meta name=viewport content='width=device-width'>"
        "<style>img{display:block;width:72px;height:24px}</style>"
        "<img id=large src=\"data:image/svg+xml,%3Csvg "
        "xmlns='http://www.w3.org/2000/svg' viewBox='0 0 3000 1000'%3E"
        "%3Crect width='1500' height='1000' fill='%23ff0000'/%3E"
        "%3Crect x='1500' width='1500' height='1000' fill='%230000ff'/%3E"
        "%3C/svg%3E\">";
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
        navigation_enable_external_resources(
            &navigation, 2, 32 * 1024, 16 * 1024,
            8, 512 * 1024, 256 * 1024, MIB, 1000);
    }
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    bool committed = ready && navigation_commit_html(
        &navigation, generation, "https://svg-sized.test/page",
        html, sizeof(html) - 1u, 480, NULL, NULL, true);
    if (committed) retarget_pump(&navigation);
    const ImageResource *image = committed
        ? retarget_image(&navigation, "large") : NULL;
    bool sized = image != NULL && image->pixels != NULL
        && image->source_width == 3000 && image->source_height == 1000
        && image->width == 72 && image->height == 24
        && image->encoded != NULL
        && (image->retarget_flags & IMAGE_RETARGET_VECTOR) != 0
        && svg_retarget_pixel(image, 10, 12)[0] == 255
        && svg_retarget_pixel(image, 60, 12)[2] == 255;
    if (sized) {
        int width = 72, height = 24;
        unsigned char *direct = image_svg_decode(
            image->encoded, image->encoded_length, &budget,
            72u * 24u * 4u, &width, &height);
        sized = direct != NULL
            && memcmp(direct, image->pixels, 72u * 24u * 4u) == 0;
        budget_free(&budget, direct);
    }
    if (!sized) svg_retarget_report(&navigation);
    if (ready) navigation_destroy(&navigation);
    if (browser_ready) browser_session_destroy(&browser);
    bool released = budget.current == 0;
    if (installed) released = budget_uninstall_lexbor(&budget) && released;
    static const char svg[] =
        "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 3000 1000'>"
        "<rect width='3000' height='1000' fill='red'/></svg>";
    bool refusal_clean = true, fallback_succeeded = false;
    /* Each refused parse/raster allocation must return all scratch, leave
       output parameters unchanged, and permit the subsequent retry. */
    for (size_t fail = 0; fail < 40; fail++) {
        Budget scratch;
        budget_init(&scratch, MIB);
        budget_inject_failure_after(&scratch, fail);
        size_t calls = 0;
        int width = 0, height = 0, source_width = -1, source_height = -1;
        unsigned char *pixels = image_svg_decode_bounded(
            svg, sizeof(svg) - 1u, &scratch, 72u * 24u * 4u,
            &width, &height, svg_oversize_test_target, &calls,
            &source_width, &source_height);
        if (pixels != NULL) {
            fallback_succeeded = true;
            refusal_clean = refusal_clean && calls == 1 && width == 72
                && height == 24 && source_width == 3000 && source_height == 1000;
        } else {
            refusal_clean = refusal_clean && width == 0 && height == 0
                && source_width == -1 && source_height == -1;
        }
        budget_free(&scratch, pixels);
        refusal_clean = refusal_clean && scratch.current == 0
            && !image_svg_decode_busy();
    }
    /* An explicit raster target remains the old API's behavior and never
       calls the oversized-intrinsic fallback. */
    size_t calls = 0;
    int width = 72, height = 24;
    unsigned char *pixels = image_svg_decode_bounded(
        svg, sizeof(svg) - 1u, &budget, 72u * 24u * 4u,
        &width, &height, svg_oversize_test_target, &calls, NULL, NULL);
    refusal_clean = refusal_clean && pixels != NULL && calls == 0;
    budget_free(&budget, pixels);
    static const char small_svg[] =
        "<svg xmlns='http://www.w3.org/2000/svg' width='64' height='32'>"
        "<circle cx='32' cy='16' r='14' fill='blue'/></svg>";
    width = height = 0;
    pixels = image_svg_decode_bounded(
        small_svg, sizeof(small_svg) - 1u, &budget, 64u * 32u * 4u,
        &width, &height, svg_oversize_test_target, &calls, NULL, NULL);
    int direct_width = 0, direct_height = 0;
    unsigned char *direct = image_svg_decode(
        small_svg, sizeof(small_svg) - 1u, &budget, 64u * 32u * 4u,
        &direct_width, &direct_height);
    refusal_clean = refusal_clean && pixels != NULL && direct != NULL
        && calls == 0 && width == 64 && height == 32
        && memcmp(pixels, direct, 64u * 32u * 4u) == 0;
    budget_free(&budget, pixels);
    budget_free(&budget, direct);
    return sized && released && refusal_clean && fallback_succeeded
        && budget.current == 0;
}

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
