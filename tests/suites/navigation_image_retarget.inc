/* Display retargeting: decoded rasters follow the committed layout's painted
   size. The image is a PNG generated here (a 400x200 gradient), loaded as a
   data: URL so no fixture or network is involved. */

static char *retarget_png_data_url(int width, int height, int seed)
{
    size_t row = (size_t) width * 3u + 1u;
    size_t raw_length = row * (size_t) height;
    unsigned char *raw = malloc(raw_length);
    if (raw == NULL) return NULL;
    for (int y = 0; y < height; y++) {
        unsigned char *line = raw + (size_t) y * row;
        line[0] = 0;
        for (int x = 0; x < width; x++) {
            line[1 + x * 3] = (unsigned char) (x * 255 / width);
            line[2 + x * 3] = (unsigned char) (y * 255 / height);
            line[3 + x * 3] = (unsigned char) ((x / 50 + y / 50 + seed) % 2 ? 200 : 40);
        }
    }
    uLongf packed_length = compressBound(raw_length);
    unsigned char *packed = malloc(packed_length);
    if (packed == NULL
        || compress2(packed, &packed_length, raw, raw_length, 6) != Z_OK) {
        free(raw);
        free(packed);
        return NULL;
    }
    free(raw);
    size_t png_length = 8u + 25u + 12u + packed_length + 12u;
    unsigned char *png = malloc(png_length);
    if (png == NULL) {
        free(packed);
        return NULL;
    }
    size_t at = 0;
    static const unsigned char signature[8] = {
        137, 80, 78, 71, 13, 10, 26, 10
    };
    memcpy(png, signature, 8);
    at = 8;
    const struct {
        const char *type;
        const unsigned char *data;
        size_t length;
    } chunks[3] = {
        {"IHDR", (const unsigned char[13]) {
            (unsigned char) (width >> 24), (unsigned char) (width >> 16),
            (unsigned char) (width >> 8), (unsigned char) width,
            (unsigned char) (height >> 24), (unsigned char) (height >> 16),
            (unsigned char) (height >> 8), (unsigned char) height,
            8, 2, 0, 0, 0}, 13},
        {"IDAT", packed, packed_length},
        {"IEND", NULL, 0}
    };
    for (int i = 0; i < 3; i++) {
        size_t length = chunks[i].length;
        png[at++] = (unsigned char) (length >> 24);
        png[at++] = (unsigned char) (length >> 16);
        png[at++] = (unsigned char) (length >> 8);
        png[at++] = (unsigned char) length;
        memcpy(png + at, chunks[i].type, 4);
        if (length != 0) memcpy(png + at + 4, chunks[i].data, length);
        uLong crc = crc32(0, png + at, (uInt) (length + 4u));
        at += 4u + length;
        png[at++] = (unsigned char) (crc >> 24);
        png[at++] = (unsigned char) (crc >> 16);
        png[at++] = (unsigned char) (crc >> 8);
        png[at++] = (unsigned char) crc;
    }
    free(packed);
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    static const char prefix[] = "data:image/png;base64,";
    size_t encoded_length = (at + 2u) / 3u * 4u;
    char *url = malloc(sizeof(prefix) + encoded_length);
    if (url == NULL) {
        free(png);
        return NULL;
    }
    memcpy(url, prefix, sizeof(prefix) - 1u);
    char *out = url + sizeof(prefix) - 1u;
    for (size_t i = 0; i < at; i += 3) {
        uint32_t value = (uint32_t) png[i] << 16;
        if (i + 1 < at) value |= (uint32_t) png[i + 1] << 8;
        if (i + 2 < at) value |= png[i + 2];
        *out++ = alphabet[(value >> 18) & 63];
        *out++ = alphabet[(value >> 12) & 63];
        *out++ = i + 1 < at ? alphabet[(value >> 6) & 63] : '=';
        *out++ = i + 2 < at ? alphabet[value & 63] : '=';
    }
    *out = '\0';
    free(png);
    return url;
}

static void retarget_pump(NavigationSession *navigation)
{
    for (size_t pumps = 0;
         navigation_background_resources_pending(navigation) && pumps < 64;
         pumps++) {
        if (!navigation_run_background_resources(navigation)) break;
    }
    for (size_t steps = 0;
         navigation_image_retarget_pending(navigation) && steps < 64;
         steps++) {
        (void) navigation_run_image_retarget_work(navigation);
    }
}

static void retarget_dump(NavigationSession *navigation, const char *stage)
{
    if (getenv("RETARGET_TEST_TRACE") == NULL) return;
    fprintf(stderr, "stage %s viewport css=%d device=%d scale=%d/%d\n", stage,
            navigation->page.layout.viewport.css_width,
            navigation->page.layout.viewport.device_width,
            navigation->page.layout.viewport.scale_numerator,
            navigation->page.layout.viewport.scale_denominator);
    for (size_t c = 0; c < navigation->page.layout.count; c++) {
        const DrawCommand *command = &navigation->page.layout.commands[c];
        if (command->type == DRAW_IMAGE)
            fprintf(stderr, "  command %dx%d\n", command->width, command->height);
    }
    for (size_t i = 0; i < navigation->page.images.count; i++) {
        const ImageResource *item = &navigation->page.images.items[i];
        fprintf(stderr, "  image %zu: %dx%d pixels=%d flags=%u\n", i,
                item->width, item->height, item->pixels != NULL,
                (unsigned) item->retarget_flags);
    }
}

static bool retarget_relayout_at(NavigationSession *navigation, int width)
{
    if (!viewport_context_init(&navigation->viewport, width, 272,
                               width, 272)) return false;
    bool ok = navigation_relayout(navigation);
    retarget_pump(navigation);
    retarget_dump(navigation, "relayout");
    return ok;
}

static const ImageResource *retarget_image(NavigationSession *navigation,
                                           const char *id)
{
    lxb_dom_node_t *root =
        lxb_dom_interface_node(navigation->page.document.html);
    lxb_dom_node_t *node = root == NULL ? NULL : test_find_id(root, id);
    return node == NULL ? NULL
        : images_find_node(&navigation->page.images, node);
}

static bool retarget_set_id(NavigationSession *navigation, const char *from,
                            const char *to)
{
    lxb_dom_node_t *root =
        lxb_dom_interface_node(navigation->page.document.html);
    lxb_dom_node_t *node = root == NULL ? NULL : test_find_id(root, from);
    return node != NULL
        && lxb_dom_element_set_attribute(
               lxb_dom_interface_element(node), (const lxb_char_t *) "id", 2,
               (const lxb_char_t *) to, strlen(to)) != NULL;
}

/* Two 400x200 images (plus a shared-source alias) laid out 96px wide have no height before they load,
   so they are decoded at source size; once layout paints them at 96x48 the
   surfaces are reduced to that. Hiding one releases it to its encoded bytes.
   Painting the other at 240x120 decodes it again at full size from its kept
   encoded bytes, after which alternating sizes never reduce or decode it
   again. Showing the hidden one decodes it at its painted size, and pinning
   it for page script restores and keeps the full decode. Teardown leaves no
   bytes behind. */
static bool test_display_retarget_follows_painted_size(void)
{
    char *data_url = retarget_png_data_url(400, 200, 0);
    char *other_url = retarget_png_data_url(400, 200, 1);
    size_t html_capacity = data_url == NULL || other_url == NULL ? 0
        : 2u * strlen(data_url) + strlen(other_url) + 512;
    char *html = html_capacity == 0 ? NULL : malloc(html_capacity);
    int written = html == NULL ? -1 : snprintf(
        html, html_capacity,
        "<!doctype html><title>Retarget</title>"
        "<meta name=viewport content=\"width=device-width\">"
        "<style>html,body{margin:0}img{display:block;width:96px}"
        "#big{width:240px}#hidden{display:none}</style>"
        "<img id=shown src=\"%s\"><img id=toggle src=\"%s\">"
        "<img id=alias src=\"%s\">",
        data_url, other_url, data_url);
    free(data_url);
    free(other_url);
    if (written <= 0 || (size_t) written >= html_capacity) {
        fprintf(stderr, "display-retarget: page build failed\n");
        free(html);
        return false;
    }
    Budget budget;
    budget_init(&budget, 24 * MIB);
    bool installed = budget_install_lexbor(&budget);
    NavigationSession navigation = {0};
    bool ready = installed && navigation_init(&navigation, &budget, 4);
    if (ready) {
        navigation_enable_external_resources(
            &navigation, 2, 32 * 1024, 16 * 1024,
            4, 512 * 1024, 256 * 1024, 4 * MIB, 1000);
    }
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    bool committed = ready && navigation_commit_static_html(
        &navigation, generation, "https://retarget.test/page",
        html, (size_t) written, 480, NULL, NULL, true);
    if (committed) retarget_pump(&navigation);
    if (committed) retarget_dump(&navigation, "load");
    const ImageResource *shown = committed
        ? retarget_image(&navigation, "shown") : NULL;
    const ImageResource *toggle = committed
        ? retarget_image(&navigation, "toggle") : NULL;
    bool reduced = shown != NULL && toggle != NULL
        && shown->pixels != NULL && toggle->pixels != NULL
        && shown->width == 96 && shown->height == 48
        && toggle->width == 96 && toggle->height == 48
        && shown->source_width == 400 && shown->full_width == 400
        && (shown->retarget_flags & IMAGE_RETARGET_SHRUNK) != 0
        && navigation.page.images.stats.decoded_bytes
               == 2u * 96u * 48u * 4u;
    /* Both surfaces and the alias must share one command scan, not one per resize plus
       another when the plan settles. A later relayout starts a new plan. */
    reduced = reduced && navigation.page.images.retarget.scans == 1;

    bool dropped = committed && retarget_set_id(&navigation, "toggle", "hidden")
        && retarget_relayout_at(&navigation, 480);
    const ImageResource *hidden = dropped
        ? retarget_image(&navigation, "hidden") : NULL;
    dropped = hidden != NULL && hidden->pixels == NULL
        && image_resource_available(hidden)
        && (hidden->retarget_flags & IMAGE_RETARGET_DROPPED) != 0
        && navigation.page.images.stats.decoded_bytes == 96u * 48u * 4u;
    size_t shrinks = navigation.page.images.retarget.shrinks;

    bool grown = committed && retarget_set_id(&navigation, "shown", "big")
        && retarget_relayout_at(&navigation, 480);
    shown = grown ? retarget_image(&navigation, "big") : NULL;
    grown = shown != NULL && shown->width == 400 && shown->height == 200
        && (shown->retarget_flags & IMAGE_RETARGET_GROWN) != 0
        && navigation.page.images.retarget.grows == 1;
    bool stable = grown;
    for (int round = 0; stable && round < 3; round++) {
        stable = retarget_set_id(&navigation, "big", "shown")
            && retarget_relayout_at(&navigation, 480)
            && retarget_set_id(&navigation, "shown", "big")
            && retarget_relayout_at(&navigation, 480);
    }
    shown = stable ? retarget_image(&navigation, "big") : NULL;
    stable = shown != NULL && shown->width == 400
        && navigation.page.images.retarget.shrinks == shrinks
        && navigation.page.images.retarget.grows == 1;

    bool restored = committed && retarget_set_id(&navigation, "hidden", "toggle")
        && retarget_relayout_at(&navigation, 480);
    toggle = restored ? retarget_image(&navigation, "toggle") : NULL;
    restored = toggle != NULL && toggle->pixels != NULL
        && toggle->width == 96 && toggle->height == 48
        && navigation.page.images.retarget.grows == 2;

    /* Page script reading the pixels (canvas drawImage) gets the full
       decode, which no later layout reduces. */
    bool pinned = toggle != NULL && images_retarget_pin_full(
        &navigation.page.images,
        (size_t) (toggle - navigation.page.images.items),
        navigation.browser_session)
        && retarget_relayout_at(&navigation, 480);
    toggle = pinned ? retarget_image(&navigation, "toggle") : NULL;
    pinned = toggle != NULL && toggle->width == 400 && toggle->height == 200
        && (toggle->retarget_flags & IMAGE_RETARGET_PINNED) != 0;

    bool ok = committed && reduced && dropped && grown && stable && restored
        && pinned;
    if (!ok) {
        fprintf(stderr,
                "display-retarget committed=%d reduced=%d dropped=%d "
                "grown=%d stable=%d restored=%d pinned=%d decoded=%zu "
                "shrinks=%zu grows=%zu drops=%zu error=\"%s\"\n",
                committed, reduced, dropped, grown, stable, restored, pinned,
                navigation.page.images.stats.decoded_bytes,
                navigation.page.images.retarget.shrinks,
                navigation.page.images.retarget.grows,
                navigation.page.images.retarget.drops,
                navigation.last_error);
        for (size_t i = 0; i < navigation.page.images.count; i++) {
            const ImageResource *item = &navigation.page.images.items[i];
            fprintf(stderr, "  image %zu: %dx%d full %dx%d pixels=%d "
                    "flags=%u\n", i, item->width, item->height,
                    item->full_width, item->full_height,
                    item->pixels != NULL, (unsigned) item->retarget_flags);
        }
    }
    if (installed) navigation_destroy(&navigation);
    free(html);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
    if (!clean) fprintf(stderr, "display-retarget: %zu bytes after teardown\n",
                        budget.current);
    return ok && clean;
}
