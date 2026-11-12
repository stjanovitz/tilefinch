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

static bool test_svg_local_clips_preserve_artwork(void)
{
    static const char markup[] =
        "<svg width='100' height='60' viewBox='0 0 100 60'>"
        "<clipPath id='window'><rect width='20' height='20'/></clipPath>"
        "<g transform='translate(10 5)' clip-path='url(#window)'>"
        "<rect width='40' height='40' fill='#ff0000'/></g>"
        "<defs><clipPath id='half' clipPathUnits='objectBoundingBox'>"
        "<rect width='.5' height='1'/></clipPath></defs>"
        "<rect x='50' y='5' width='40' height='20' fill='#0000ff' "
        "clip-path='url(#half)'/>"
        "<svg x='10' y='35' width='20' height='20' viewBox='0 0 20 20'>"
        "<rect x='-10' y='-10' width='40' height='40' fill='#00ff00'/></svg>"
        "<g clip-path='url(#late)'><rect x='50' y='35' width='40' "
        "height='20' fill='#ff00ff'/></g>"
        "<defs><clipPath id='late'><rect x='50' y='35' width='10' height='20'/>"
        "<rect x='80' y='35' width='10' height='20'/></clipPath></defs>"
        "<g transform='translate(50 30) rotate(90)' style='clip-path:url(\"#half\")'>"
        "<rect width='20' height='10' fill='#ffff00'/></g>"
        "</svg>";
    Budget budget;
    budget_init(&budget, 4u * MIB);
    int width = 0, height = 0;
    unsigned char *pixels = image_svg_decode(markup, sizeof(markup) - 1,
        &budget, MIB, &width, &height);
    bool okay = pixels != NULL && width == 100 && height == 60;
    static const int samples[][6] = {
        {15,10,255,0,0,255}, {35,10,0,0,0,0}, {5,5,0,0,0,0},
        {55,10,0,0,255,255}, {80,10,0,0,0,0},
        {15,40,0,255,0,255}, {5,40,0,0,0,0}, {35,40,0,0,0,0},
        {55,40,255,0,255,255}, {85,40,255,0,255,255}, {70,40,0,0,0,0},
        {45,35,255,255,0,255}, {45,45,0,0,0,0}
    };
    for (size_t i = 0; okay && i < sizeof(samples) / sizeof(samples[0]); i++) {
        const unsigned char *p = pixels
            + ((size_t) samples[i][1] * 100u + (size_t) samples[i][0]) * 4u;
        for (unsigned channel = 0; channel < 4; channel++) {
            if (p[channel] != samples[i][channel + 2]) {
                fprintf(stderr, "SVG clip sample %zu channel %u: %u != %d\n",
                    i, channel, p[channel], samples[i][channel + 2]);
                okay = false;
            }
        }
    }
    budget_free(&budget, pixels);
    for (size_t failure = 0; okay && failure < 80; failure++) {
        budget_inject_failure_after(&budget, failure);
        width = height = 0;
        pixels = image_svg_decode(markup, sizeof(markup) - 1,
            &budget, MIB, &width, &height);
        budget_clear_failure_injection(&budget);
        budget_free(&budget, pixels);
        okay = budget.current == 0;
    }
    return okay && budget.current == 0;
}

static bool test_svg_solid_luminance_mask(void)
{
    static const char markup[] =
        "<svg width='80' height='20' viewBox='0 0 80 20'>"
        "<defs><mask id='ink' fill='white' maskUnits='userSpaceOnUse' "
        "x='0' y='0' width='30' height='20'>"
        "<path d='M0 0h40v20H0z'/><rect x='10' width='10' height='20' fill='black'/>"
        "</mask></defs><g mask='url(#ink)'><rect width='40' height='20' fill='red'/></g>"
        "<mask id='alpha' mask-type='alpha'><rect x='40' width='20' height='20' fill='black'/></mask>"
        "<rect x='40' width='20' height='20' fill='blue' mask='url(#alpha)'/>"
        "<rect x='60' width='20' height='20' fill='green' mask='url(#missing)'/>"
        "</svg>";
    Budget budget;
    budget_init(&budget, 4u * MIB);
    int width = 0, height = 0;
    unsigned char *pixels = image_svg_decode(markup, sizeof(markup) - 1,
        &budget, MIB, &width, &height);
    bool okay = pixels != NULL && width == 80 && height == 20;
    for (int x = 5; okay && x < 40; x += 10) {
        const unsigned char *pixel = pixels + (10u * 80u + (unsigned) x) * 4u;
        okay = pixel[3] == (x == 5 || x == 25 ? 255 : 0);
        if (pixel[3] != 0) okay = okay && pixel[0] == 255 && pixel[1] == 0;
    }
    okay = okay && pixels[(10u * 80u + 45u) * 4u + 2u] == 255
        && pixels[(10u * 80u + 45u) * 4u + 3u] == 255
        && pixels[(10u * 80u + 65u) * 4u + 3u] == 0;
    budget_free(&budget, pixels);
    for (size_t failure = 0; okay && failure < 160; failure++) {
        budget_inject_failure_after(&budget, failure);
        pixels = image_svg_decode(markup, sizeof(markup) - 1,
            &budget, MIB, &width, &height);
        budget_clear_failure_injection(&budget);
        budget_free(&budget, pixels);
        okay = budget.current == 0;
    }
    return okay && budget.current == 0;
}

/* RS-18: a keyword read from `style` ends at its declaration. Each mask
   holds a black rectangle, so only an alpha mask lets the blue through. */
static bool test_svg_style_mask_type_keyword(void)
{
    static const char markup[] =
        "<svg width='100' height='20' viewBox='0 0 100 20'>"
        "<mask id='m0' style='mask-type: alpha;'>"
        "<rect width='20' height='20' fill='black'/></mask>"
        "<mask id='m1' style='mask-type:alpha;fill:none'>"
        "<rect x='20' width='20' height='20' fill='black'/></mask>"
        "<mask id='m2' style='mask-type: alpha !important'>"
        "<rect x='40' width='20' height='20' fill='black'/></mask>"
        "<mask id='m3' style='fill:none; mask-type:alpha'>"
        "<rect x='60' width='20' height='20' fill='black'/></mask>"
        "<mask id='m4' style='mask-type: alphabet;'>"
        "<rect x='80' width='20' height='20' fill='black'/></mask>"
        "<rect width='20' height='20' fill='blue' mask='url(#m0)'/>"
        "<rect x='20' width='20' height='20' fill='blue' mask='url(#m1)'/>"
        "<rect x='40' width='20' height='20' fill='blue' mask='url(#m2)'/>"
        "<rect x='60' width='20' height='20' fill='blue' mask='url(#m3)'/>"
        "<rect x='80' width='20' height='20' fill='blue' mask='url(#m4)'/>"
        "</svg>";
    Budget budget;
    budget_init(&budget, 4u * MIB);
    int width = 0, height = 0;
    unsigned char *pixels = image_svg_decode(markup, sizeof(markup) - 1,
        &budget, MIB, &width, &height);
    bool okay = pixels != NULL && width == 100 && height == 20;
    for (unsigned column = 0; okay && column < 5; column++) {
        const unsigned char *pixel =
            pixels + (10u * 100u + column * 20u + 10u) * 4u;
        bool alpha = column != 4;
        if (alpha ? pixel[2] != 255 || pixel[3] != 255 : pixel[3] != 0) {
            fprintf(stderr, "mask column %u: rgba %u %u %u %u, wanted %s\n",
                    column, pixel[0], pixel[1], pixel[2], pixel[3],
                    alpha ? "opaque blue (alpha mask)"
                          : "transparent (luminance mask)");
            okay = false;
        }
    }
    budget_free(&budget, pixels);
    return okay && budget.current == 0;
}

static size_t svg_test_append(char *buffer, size_t used, size_t capacity,
                              const char *text)
{
    size_t length = strlen(text);
    if (used >= capacity || length >= capacity - used) return capacity;
    memcpy(buffer + used, text, length + 1);
    return used + length;
}

static const unsigned char *svg_test_pixel(const unsigned char *pixels,
                                           int width, int x, int y)
{
    return pixels + ((size_t) y * (size_t) width + (size_t) x) * 4u;
}

/* RS-14: a cooperation hook that allocates from the page Budget during an
   SVG decode keeps its block, whether the decode succeeds or fails. Its
   allocation is neither tagged with the decoder's scratch owner nor swept
   by a rollback after a failed decode. */
enum { SVG_HOOK_BLOCK = 4096 };

typedef struct {
    TestCountingClock clock;
    Budget *budget;
    unsigned char *block;
    size_t svg_checkpoints;
} SvgAllocatingHook;

static uint64_t svg_allocating_hook_clock(void *context)
{
    return test_counting_clock_ns(&((SvgAllocatingHook *) context)->clock);
}

static bool svg_allocating_hook(void *context, const char *phase,
                                size_t completed_work_units)
{
    (void) completed_work_units;
    SvgAllocatingHook *hook = context;
    if (strcmp(phase, "svg-decode") != 0) return true;
    hook->svg_checkpoints++;
    if (hook->block == NULL) {
        hook->block = budget_malloc_category(
            hook->budget, BUDGET_CATEGORY_RESOURCE, SVG_HOOK_BLOCK);
        if (hook->block != NULL) memset(hook->block, 0x5a, SVG_HOOK_BLOCK);
    }
    return true;
}

static void svg_allocating_hook_install(SvgAllocatingHook *hook,
                                        Budget *budget)
{
    *hook = (SvgAllocatingHook) {
        .clock = {.now_ns = UINT64_C(1000000000),
                  .step_ns = UINT64_C(10000000)},
        .budget = budget
    };
    TilefinchPlatformServices services = {
        .context = hook,
        .monotonic_time_ns = svg_allocating_hook_clock,
        .cooperate = svg_allocating_hook
    };
    tilefinch_platform_set_services(&services);
}

/* The block must still be charged and intact; free it if so. */
static bool svg_allocating_hook_kept(SvgAllocatingHook *hook,
                                     size_t other_bytes, const char *label)
{
    Budget *budget = hook->budget;
    bool kept = hook->block != NULL
        && budget->current >= other_bytes + SVG_HOOK_BLOCK;
    for (size_t i = 0; kept && i < SVG_HOOK_BLOCK; i++)
        kept = hook->block[i] == 0x5a;
    if (!kept) {
        fprintf(stderr, "%s: hook block %s after %zu svg checkpoints, "
                "budget %zu bytes (want %zu more than %zu)\n", label,
                hook->block == NULL ? "never allocated" : "reclaimed",
                hook->svg_checkpoints, budget->current,
                (size_t) SVG_HOOK_BLOCK, other_bytes);
        return false;
    }
    budget_free(budget, hook->block);
    hook->block = NULL;
    return true;
}

/* Enough shapes to reach the decoder's cooperation checkpoints; a refused
   one then nests past the parser's depth limit. */
static size_t svg_test_shapes(char *markup, size_t capacity, bool refused)
{
    size_t used = svg_test_append(markup, 0, capacity,
        "<svg width='32' height='32' viewBox='0 0 32 32'>");
    for (unsigned i = 0; i < 96; i++)
        used = svg_test_append(markup, used, capacity,
            "<path d='M0 0L8 0L8 8L0 8Z' fill='blue'/>");
    for (unsigned i = 0; refused && i < 160; i++)
        used = svg_test_append(markup, used, capacity, "<g>");
    for (unsigned i = 0; refused && i < 160; i++)
        used = svg_test_append(markup, used, capacity, "</g>");
    return svg_test_append(markup, used, capacity, "</svg>");
}

static bool test_svg_decode_keeps_cooperation_allocations(void)
{
    char markup[8192];
    Budget budget;
    budget_init(&budget, 4u * MIB);
    SvgAllocatingHook hook;
    bool okay = true;
    for (unsigned pass = 0; okay && pass < 2; pass++) {
        bool sized = pass == 0;
        size_t length = svg_test_shapes(markup, sizeof(markup), !sized);
        okay = length < sizeof(markup);
        svg_allocating_hook_install(&hook, &budget);
        int width = 0, height = 0;
        unsigned char *pixels = okay ? image_svg_decode(markup, length,
            &budget, MIB, &width, &height) : NULL;
        tilefinch_platform_set_services(NULL);
        if (okay && (sized ? pixels == NULL : pixels != NULL))
            fprintf(stderr, "%s svg: decode returned %s (%dx%d)\n",
                    sized ? "decodable" : "refused",
                    pixels == NULL ? "nothing" : "pixels", width, height);
        okay = okay && (sized ? pixels != NULL : pixels == NULL)
            && svg_allocating_hook_kept(&hook, pixels == NULL ? 0
                : (size_t) width * (size_t) height * 4u,
                sized ? "decoded svg" : "refused svg");
        budget_free(&budget, pixels);
        if (hook.block != NULL && budget.current >= SVG_HOOK_BLOCK)
            budget_free(&budget, hook.block);
    }
    return okay && budget.current == 0;
}

/* The same through the image loader, whose failed-decode path rolled the
   whole Budget back to its checkpoint whenever the totals had moved. */
static bool test_svg_image_failure_keeps_cooperation_allocations(void)
{
    char markup[8192];
    size_t length = svg_test_shapes(markup, sizeof(markup), true);
    char html[16384];
    int written = snprintf(html, sizeof(html),
        "<!doctype html><body><img width=32 height=32 "
        "src=\"data:image/svg+xml,%s\"></body>", markup);
    if (length >= sizeof(markup) || written <= 0
        || (size_t) written >= sizeof(html)) return false;
    Budget budget;
    budget_init(&budget, 16u * MIB);
    bool installed = budget_install_lexbor(&budget);
    PocDocument document = {0};
    Stylesheet stylesheet = {0};
    ImageResources images = {0};
    bool okay = installed
        && document_parse(&document, &budget, html, (size_t) written, 17)
        && stylesheet_build(&stylesheet, &budget, &document, 480);
    SvgAllocatingHook hook;
    svg_allocating_hook_install(&hook, &budget);
    okay = okay && images_load_external(
        &document, &stylesheet, &images, &budget,
        "https://svg-hook.test/", "https://svg-hook.test/", NULL,
        8, 64 * 1024, 32 * 1024, 128 * 1024, 100000000, NULL, NULL);
    tilefinch_platform_set_services(NULL);
    images_destroy(&images);
    stylesheet_destroy(&stylesheet);
    document_destroy(&document);
    okay = okay && svg_allocating_hook_kept(&hook, 0, "image loader");
    if (hook.block != NULL && budget.current >= SVG_HOOK_BLOCK)
        budget_free(&budget, hook.block);
    bool clean = budget.current == 0;
    if (!clean) fprintf(stderr, "image loader: %zu bytes left\n", budget.current);
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return okay && clean;
}

/* Exported charts carry one clip-path attribute per element and clipped
   map groups can exceed the bound-shape limit. Past the bookkeeping limits
   the artwork renders unclipped, as it did before clipping support, instead
   of the whole image failing. */
static bool test_svg_clip_limits_degrade_unclipped(void)
{
    enum { CAPACITY = 256u * 1024u };
    char *markup = malloc(CAPACITY);
    if (markup == NULL) return false;
    Budget budget;
    budget_init(&budget, 16u * MIB);
    bool okay = true;
    for (unsigned scenario = 0; okay && scenario < 5; scenario++) {
        size_t used = svg_test_append(markup, 0, CAPACITY,
            "<svg width='40' height='40' viewBox='0 0 40 40'>"
            "<defs><clipPath id='left'><rect width='10' height='40'/>"
            "</clipPath></defs>");
        if (scenario == 0) {
            /* More clip-path uses than SVG_CLIP_LIMIT (128). */
            for (unsigned i = 0; i < 128; i++)
                used = svg_test_append(markup, used, CAPACITY,
                    "<rect width='40' height='20' fill='#ff0000' "
                    "clip-path='url(#left)'/>");
            used = svg_test_append(markup, used, CAPACITY,
                "<rect y='20' width='40' height='20' fill='#0000ff' "
                "clip-path='url(#left)'/>");
        } else if (scenario == 1) {
            /* More definitions than the limit: the excess definition's
               content is not artwork, and references to it are ignored. */
            for (unsigned i = 0; i < 127; i++)
                used = svg_test_append(markup, used, CAPACITY,
                    "<clipPath><rect width='1' height='1'/></clipPath>");
            used = svg_test_append(markup, used, CAPACITY,
                "<clipPath id='extra'><rect width='40' height='40' "
                "fill='#00ff00'/></clipPath>"
                "<rect width='40' height='20' fill='#ff0000' "
                "clip-path='url(#left)'/>"
                "<rect y='20' width='40' height='20' fill='#0000ff' "
                "clip-path='url(#extra)'/>");
        } else if (scenario == 3 || scenario == 4) {
            /* A mask past the definition limit is ignored like a clip,
               since the limit is ours; a mask that never existed still
               hides its artwork. */
            for (unsigned i = 0; i < 127; i++)
                used = svg_test_append(markup, used, CAPACITY,
                    "<mask><rect width='1' height='1' fill='#fff'/></mask>");
            used = svg_test_append(markup, used, CAPACITY,
                "<mask id='extra'><rect width='40' height='40' "
                "fill='#00ff00'/></mask>"
                "<rect width='40' height='20' fill='#ff0000' "
                "clip-path='url(#left)'/>");
            used = svg_test_append(markup, used, CAPACITY, scenario == 3
                ? "<rect y='20' width='40' height='20' fill='#0000ff' "
                  "mask='url(#extra)'/>"
                : "<rect y='20' width='40' height='20' fill='#0000ff' "
                  "mask='url(#absent)'/>");
        } else {
            /* More bound shapes than SVG_CLIP_SHAPE_LIMIT (4096). */
            used = svg_test_append(markup, used, CAPACITY,
                "<g clip-path='url(#left)'>"
                "<rect width='40' height='20' fill='#ff0000'/>");
            for (unsigned i = 0; i < 4095; i++)
                used = svg_test_append(markup, used, CAPACITY,
                    "<rect width='1' height='1' fill='#ff0000'/>");
            used = svg_test_append(markup, used, CAPACITY,
                "<rect y='20' width='40' height='20' fill='#0000ff'/></g>");
        }
        used = svg_test_append(markup, used, CAPACITY, "</svg>");
        okay = used < CAPACITY;
        int width = 0, height = 0;
        unsigned char *pixels = okay ? image_svg_decode(markup, used,
            &budget, MIB, &width, &height) : NULL;
        okay = pixels != NULL && width == 40 && height == 40;
        if (!okay) fprintf(stderr, "SVG clip limit scenario %u refused\n", scenario);
        if (okay) {
            const unsigned char *clipped_in = svg_test_pixel(pixels, 40, 5, 10);
            const unsigned char *clipped_out = svg_test_pixel(pixels, 40, 30, 10);
            const unsigned char *beyond = svg_test_pixel(pixels, 40, 30, 30);
            okay = clipped_in[0] == 255 && clipped_in[3] == 255
                && clipped_out[3] == 0
                && (scenario == 4 ? beyond[3] == 0
                    : beyond[2] == 255 && beyond[1] == 0
                      && beyond[3] == 255);
            if (!okay) fprintf(stderr, "SVG clip limit scenario %u: in=%u/%u "
                "out=%u beyond=%u,%u,%u,%u\n", scenario, clipped_in[0],
                clipped_in[3], clipped_out[3], beyond[0], beyond[1],
                beyond[2], beyond[3]);
        }
        budget_free(&budget, pixels);
        okay = okay && budget.current == 0;
    }
    free(markup);
    return okay;
}

/* Clip and mask coverage is rebuilt only when the clip actually changes:
   repeated identical uses and A, B-inside-A, A alternation reuse the two
   most recent masks. The optional second mask buffer can be refused at any
   point without changing a single pixel. */
static bool test_svg_clip_masks_rebuild_only_on_change(void)
{
    static const char header[] =
        "<svg width='120' height='80' viewBox='0 0 120 80'>"
        "<defs><clipPath id='outer'><rect x='4' y='4' width='100' height='70' rx='9'/>"
        "</clipPath><clipPath id='inner'><circle cx='50' cy='40' r='25'/></clipPath>"
        "<clipPath id='box' clipPathUnits='objectBoundingBox'>"
        "<rect x='.2' y='.2' width='.6' height='.6'/></clipPath>"
        "<mask id='fade' maskUnits='userSpaceOnUse' x='0' y='0' width='120' height='80'>"
        "<rect width='120' height='80' fill='#808080'/></mask></defs>";
    static const char group[] =
        "<g clip-path='url(#outer)'><rect x='0' y='0' width='120' height='30' fill='#ff0000'/>"
        "<g clip-path='url(#inner)'><rect x='20' y='10' width='80' height='60' fill='#00ff00'/></g>"
        "<rect x='0' y='50' width='120' height='30' fill='#0000ff'/>"
        "<g mask='url(#fade)'><rect x='60' y='0' width='20' height='80' fill='#ffff00'/></g>"
        "<path d='M70 20h40v40h-40z' fill='#00ffff' clip-path='url(#box)'/></g>";
    char markup[4096];
    size_t used = svg_test_append(markup, 0, sizeof(markup), header);
    for (unsigned i = 0; i < 6; i++)
        used = svg_test_append(markup, used, sizeof(markup), group);
    used = svg_test_append(markup, used, sizeof(markup), "</svg>");
    if (used >= sizeof(markup)) return false;
    Budget budget;
    budget_init(&budget, 4u * MIB);
    int width = 0, height = 0;
    tilefinch_test_faults()->svg_clip_mask_builds = 0;
    unsigned char *reference = image_svg_decode(markup, used, &budget, MIB,
        &width, &height);
    unsigned builds = tilefinch_test_faults()->svg_clip_mask_builds;
    bool okay = reference != NULL && width == 120 && height == 80;
    /* Per group: outer, inner-in-outer, back to outer (cached), fade,
       box (object bounding box: never shared). The first group also pays
       for the outer mask once more after the fade evicts it. */
    if (okay && builds > 6u * 4u) {
        fprintf(stderr, "SVG clip mask builds %u\n", builds);
        okay = false;
    }
    okay = okay && svg_test_pixel(reference, 120, 10, 10)[0] == 255
        && svg_test_pixel(reference, 120, 110, 10)[3] == 0
        && svg_test_pixel(reference, 120, 50, 40)[1] == 255
        && svg_test_pixel(reference, 120, 90, 40)[2] == 255
        && svg_test_pixel(reference, 120, 10, 40)[3] == 0;
    size_t bytes = (size_t) width * (size_t) height * 4u;
    size_t retained = budget.current;
    bool degraded = false;
    for (size_t failure = 0; okay && failure < 400; failure++) {
        budget_inject_failure_after(&budget, failure);
        int w = 0, h = 0;
        unsigned char *pixels = image_svg_decode(markup, used, &budget, MIB,
            &w, &h);
        bool injected = budget.injected_failure_count != 0;
        budget_clear_failure_injection(&budget);
        if (pixels != NULL) {
            okay = w == width && h == height
                && memcmp(pixels, reference, bytes) == 0;
            if (!okay) fprintf(stderr, "SVG clip refusal %zu changed pixels\n",
                               failure);
            degraded = degraded || injected;
        }
        budget_free(&budget, pixels);
        okay = okay && budget.current == retained;
        if (!injected) break;
    }
    budget_free(&budget, reference);
    return okay && degraded && budget.current == 0;
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

static bool test_inline_svg_css_viewport_preserves_circle(void)
{
    static const char html[] =
        "<!doctype html><style>svg{display:block;width:32px;height:20px}"
        "#ratio svg{width:23px;height:auto}</style>"
        "<div id=icon><svg width=32 height=32 viewBox='0 0 18 18'>"
        "<circle cx=9 cy=9 r=5 fill='blue'/></svg></div>"
        "<div id=ratio><svg viewBox='0 0 158 110'>"
        "<rect width=158 height=110 fill='red'/></svg></div>";
    Budget budget;
    budget_init(&budget, 8u * MIB);
    bool installed = budget_install_lexbor(&budget);
    NavigationSession navigation = {0};
    bool ready = installed && navigation_init(&navigation, &budget, 4);
    if (ready) navigation_enable_external_resources(&navigation,
        2, 32768, 16384, 8, 524288, 262144, MIB, 1000);
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    bool okay = ready && navigation_commit_html(&navigation, generation,
        "https://viewport.test/", html, sizeof(html) - 1u,
        480, NULL, NULL, true);
    const ImageResource *image = okay ? svg_retarget_icon(&navigation, "icon") : NULL;
    okay = image != NULL && image->width == 32 && image->height == 20
        && svg_retarget_pixel(image, 4, 10)[3] == 0
        && svg_retarget_pixel(image, 16, 5)[3] > 240
        && svg_retarget_pixel(image, 16, 14)[3] > 240;
    const ImageResource *ratio = okay
        ? svg_retarget_icon(&navigation, "ratio") : NULL;
    okay = okay && ratio != NULL && ratio->width == 23 && ratio->height == 16;
    if (!okay) fprintf(stderr, "CSS SVG viewport mismatch %dx%d\n",
        image == NULL ? 0 : image->width, image == NULL ? 0 : image->height);
    if (okay) {
        /* Deferred raster materialization can leave the outgoing table above
           its discovery allowance. A same-markup refresh transfers existing
           pixels; it must not erase the icon or allocate a replacement. */
        lxb_dom_node_t *nodes[] = {image->node};
        const void *pixels = image->pixels;
        size_t decoded = navigation.page.images.stats.decoded_bytes;
        okay = images_refresh_external_nodes(
            &navigation.page.document, &navigation.page.stylesheet,
            &navigation.page.images, nodes, 1u, &budget,
            "https://viewport.test/", "https://viewport.test/", NULL,
            8, 524288, 262144, 1, 1000, NULL, NULL);
        image = images_find_node(&navigation.page.images, nodes[0]);
        okay = okay && image != NULL && image->pixels == pixels
            && navigation.page.images.stats.decoded_bytes == decoded;
        /* Changed artwork cannot use this exception to decode over quota. */
        lxb_dom_node_t *circle = nodes[0]->first_child;
        okay = okay && circle != NULL
            && lxb_dom_element_set_attribute(lxb_dom_interface_element(circle),
                (const lxb_char_t *) "fill", 4,
                (const lxb_char_t *) "red", 3) != NULL
            && images_refresh_external_nodes(
                &navigation.page.document, &navigation.page.stylesheet,
                &navigation.page.images, nodes, 1u, &budget,
                "https://viewport.test/", "https://viewport.test/", NULL,
                8, 524288, 262144, 1, 1000, NULL, NULL)
            && images_find_node(&navigation.page.images, nodes[0]) == NULL;
    }
    if (ready) navigation_destroy(&navigation);
    return okay && budget.current == 0;
}

/* Font-relative width/height attributes (the 1em icon idiom) and ones CSS
   overrides rasterize at their computed size: before, the rasterizer read
   "1em" as nothing and fell back to the 256-unit viewBox, a 256 KiB raster
   per 16-24 px icon. */
static bool test_inline_svg_font_relative_attributes_use_computed_size(void)
{
    static const char html[] =
        "<!doctype html><style>.s{width:24px;height:24px}</style>"
        "<div id=em style='font-size:20px'><svg width=1em height=1em "
        "viewBox='0 0 256 256'><rect width=256 height=256 fill='blue'/>"
        "</svg></div>"
        "<div id=css><svg class=s width=1em height=1em "
        "viewBox='0 0 256 256'><rect width=256 height=256 fill='red'/>"
        "</svg></div>"
        "<div id=pct><svg class=s width='100%' height='100%' "
        "viewBox='0 0 256 256'><rect width=256 height=256 fill='green'/>"
        "</svg></div>";
    Budget budget;
    budget_init(&budget, 8u * MIB);
    bool installed = budget_install_lexbor(&budget);
    NavigationSession navigation = {0};
    bool ready = installed && navigation_init(&navigation, &budget, 4);
    if (ready) navigation_enable_external_resources(&navigation,
        2, 32768, 16384, 8, 524288, 262144, MIB, 1000);
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    bool okay = ready && navigation_commit_html(&navigation, generation,
        "https://em-icons.test/", html, sizeof(html) - 1u,
        480, NULL, NULL, true);
    const ImageResource *em = okay ? svg_retarget_icon(&navigation, "em") : NULL;
    const ImageResource *css = okay ? svg_retarget_icon(&navigation, "css") : NULL;
    const ImageResource *pct = okay ? svg_retarget_icon(&navigation, "pct") : NULL;
    okay = em != NULL && em->width == 20 && em->height == 20
        && svg_retarget_pixel(em, 10, 10)[2] > 240
        && css != NULL && css->width == 24 && css->height == 24
        && svg_retarget_pixel(css, 12, 12)[0] > 240
        && pct != NULL && pct->width == 24 && pct->height == 24;
    if (!okay) fprintf(stderr, "font-relative SVG size %dx%d %dx%d %dx%d\n",
        em == NULL ? 0 : em->width, em == NULL ? 0 : em->height,
        css == NULL ? 0 : css->width, css == NULL ? 0 : css->height,
        pct == NULL ? 0 : pct->width, pct == NULL ? 0 : pct->height);
    if (ready) navigation_destroy(&navigation);
    return okay && budget.current == 0;
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
        bool limited = false;
        unsigned char *pixels = image_svg_decode_bounded(
            svg, sizeof(svg) - 1u, &scratch, 72u * 24u * 4u,
            &width, &height, svg_oversize_test_target, &calls,
            &source_width, &source_height, &limited);
        if (pixels != NULL) {
            fallback_succeeded = true;
            refusal_clean = refusal_clean && calls == 1 && width == 72
                && height == 24 && source_width == 3000 && source_height == 1000
                && !limited;
        } else {
            /* A refused allocation is a limit, never the markup's fault. */
            refusal_clean = refusal_clean && width == 0 && height == 0
                && source_width == -1 && source_height == -1 && limited;
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
        &width, &height, svg_oversize_test_target, &calls, NULL, NULL,
        NULL);
    refusal_clean = refusal_clean && pixels != NULL && calls == 0;
    budget_free(&budget, pixels);
    /* Markup no decoder takes, by contrast, is not a limit. */
    static const char not_svg[] = "not an svg";
    bool markup_limited = true;
    width = height = 0;
    pixels = image_svg_decode_bounded(
        not_svg, sizeof(not_svg) - 1u, &budget, MIB, &width, &height,
        NULL, NULL, NULL, NULL, &markup_limited);
    refusal_clean = refusal_clean && pixels == NULL && !markup_limited;
    /* A quota below one pixel, or one the target cannot meet, is. */
    bool quota_limited = false;
    width = height = 0;
    pixels = image_svg_decode_bounded(
        svg, sizeof(svg) - 1u, &budget, 2u, &width, &height,
        NULL, NULL, NULL, NULL, &quota_limited);
    refusal_clean = refusal_clean && pixels == NULL && quota_limited;
    quota_limited = false;
    width = height = 0;
    pixels = image_svg_decode_bounded(
        svg, sizeof(svg) - 1u, &budget, 64u, &width, &height,
        NULL, NULL, NULL, NULL, &quota_limited);
    refusal_clean = refusal_clean && pixels == NULL && quota_limited;
    static const char small_svg[] =
        "<svg xmlns='http://www.w3.org/2000/svg' width='64' height='32'>"
        "<circle cx='32' cy='16' r='14' fill='blue'/></svg>";
    width = height = 0;
    pixels = image_svg_decode_bounded(
        small_svg, sizeof(small_svg) - 1u, &budget, 64u * 32u * 4u,
        &width, &height, svg_oversize_test_target, &calls, NULL, NULL,
        NULL);
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

/* Nesting past the parser's depth limits (32 nested <svg> viewports, or
   NSVG_MAX_ATTR = 128 nested elements) is this engine's limit, not markup
   no decoder takes: the decode reports *limited, so the image counts as a
   limit skip with no error event (image.c). It used to report markup. */
static bool svg_nesting_limited(const char *open, const char *close,
                                unsigned levels)
{
    static char markup[8192];
    size_t used = (size_t) snprintf(
        markup, sizeof(markup),
        "<svg xmlns='http://www.w3.org/2000/svg' width='8' height='8'>");
    for (unsigned i = 0; i < levels; i++)
        used += (size_t) snprintf(markup + used, sizeof(markup) - used,
                                  "%s", open);
    used += (size_t) snprintf(markup + used, sizeof(markup) - used,
                              "<rect width='8' height='8'/>");
    for (unsigned i = 0; i < levels; i++)
        used += (size_t) snprintf(markup + used, sizeof(markup) - used,
                                  "%s", close);
    used += (size_t) snprintf(markup + used, sizeof(markup) - used, "</svg>");
    if (used >= sizeof(markup)) return false;
    Budget budget;
    budget_init(&budget, 4u * MIB);
    int width = 0, height = 0;
    bool limited = false;
    unsigned char *pixels = image_svg_decode_bounded(
        markup, used, &budget, MIB, &width, &height, NULL, NULL, NULL, NULL,
        &limited);
    bool refused = pixels == NULL;
    budget_free(&budget, pixels);
    if (!refused || !limited)
        printf("svg nesting %s x%u: refused=%d limited=%d\n", open, levels,
               refused, limited);
    return refused && limited && budget.current == 0
        && !image_svg_decode_busy();
}

static bool test_svg_nesting_limits_are_limits(void)
{
    return svg_nesting_limited("<svg width='8' height='8'>", "</svg>", 40u)
        && svg_nesting_limited("<g>", "</g>", 140u);
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

/* Large SVG backgrounds far below the reader (tilefinch/image_retarget.h):
   a 400x400 picture (640,000 raster bytes from 200 bytes of markup), one at
   the top of the page and one 6,400 px down. Left half `fill`, right half
   blue. */
#define SVG_DISTANT_ART(fill)                                               \
    "data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' "        \
    "width='400' height='400' viewBox='0 0 400 400'%3E"                     \
    "%3Crect width='200' height='400' fill='%23" fill "'/%3E"               \
    "%3Crect x='200' width='200' height='400' fill='%230000ff'/%3E"         \
    "%3C/svg%3E"
#define SVG_DISTANT_FAR_TOP 6400
#define SVG_DISTANT_BYTES (400u * 400u * 4u)

typedef struct {
    Budget budget;
    BrowserSession browser;
    NavigationSession navigation;
    bool installed, browser_ready, ready, committed;
} SvgDistantPage;

static bool svg_distant_open_html(SvgDistantPage *page, const char *html,
                                  size_t length)
{
    memset(page, 0, sizeof(*page));
    budget_init(&page->budget, 24 * MIB);
    page->installed = budget_install_lexbor(&page->budget);
    page->browser_ready = page->installed
        && browser_session_init(&page->browser, &page->budget, 64 * 1024);
    page->ready = page->browser_ready
        && navigation_init(&page->navigation, &page->budget, 4);
    if (page->ready) {
        navigation_attach_browser_session(&page->navigation, &page->browser);
        navigation_enable_scripts(&page->navigation, 4 * MIB, 1000);
        navigation_enable_external_resources(
            &page->navigation, 2, 32 * 1024, 16 * 1024,
            8, 512 * 1024, 256 * 1024, 4 * MIB, 1000);
    }
    uint64_t generation = page->ready ? navigation_begin(&page->navigation)
                                      : 0;
    page->committed = page->ready && navigation_commit_html(
        &page->navigation, generation, "https://svg-distant.test/page",
        html, length, 480, NULL, NULL, true);
    return page->committed;
}

static bool svg_distant_open(SvgDistantPage *page)
{
    static const char html[] =
        "<!doctype html><title>Distant SVG</title>"
        "<meta name=viewport content=\"width=device-width\">"
        "<style>html,body{margin:0}div{width:400px;height:400px;"
        "background-repeat:no-repeat;background-size:400px 400px}"
        "#top{background-image:url(\"" SVG_DISTANT_ART("ff0000") "\")}"
        "#far{background-image:url(\"" SVG_DISTANT_ART("00ff00") "\")}"
        "#gap{height:6000px}#tail{height:3000px}</style>"
        "<div id=top></div><div id=gap></div><div id=far></div>"
        "<div id=tail></div>";
    return svg_distant_open_html(page, html, sizeof(html) - 1u);
}

static bool svg_distant_close(SvgDistantPage *page, const char *name)
{
    if (page->ready) navigation_destroy(&page->navigation);
    if (page->browser_ready) browser_session_destroy(&page->browser);
    bool clean = page->budget.current == 0
        && budget_active_allocations(&page->budget, NULL) == 0
        && budget_categories_reconcile(&page->budget);
    if (page->installed)
        clean = budget_uninstall_lexbor(&page->budget) && clean;
    if (!clean) fprintf(stderr, "%s: %zu bytes after teardown\n", name,
                        page->budget.current);
    return clean;
}

/* The background entry of the element with this id. */
static const ImageResource *svg_distant_item(SvgDistantPage *page,
                                             const char *id)
{
    lxb_dom_node_t *root =
        lxb_dom_interface_node(page->navigation.page.document.html);
    lxb_dom_node_t *node = root == NULL ? NULL : test_find_id(root, id);
    for (size_t i = 0; node != NULL && i < page->navigation.page.images.count;
         i++) {
        const ImageResource *item = &page->navigation.page.images.items[i];
        if (item->node == node && item->is_background) return item;
    }
    return NULL;
}

/* Held at its first size, and exactly its markup rasterized there. */
static bool svg_distant_held(Budget *budget, const ImageResource *item)
{
    if (item == NULL || item->pixels == NULL || item->width != 400
        || item->height != 400 || item->encoded == NULL) return false;
    int width = 400, height = 400;
    unsigned char *direct = image_svg_decode(
        item->encoded, item->encoded_length, budget, SVG_DISTANT_BYTES,
        &width, &height);
    bool same = direct != NULL && width == 400 && height == 400
        && memcmp(direct, item->pixels, SVG_DISTANT_BYTES) == 0;
    budget_free(budget, direct);
    return same;
}

static bool svg_distant_released(const ImageResource *item)
{
    return item != NULL && item->pixels == NULL && item->encoded != NULL
        && image_resource_available(item)
        && (item->retarget_flags & (IMAGE_RETARGET_VECTOR
                | IMAGE_RETARGET_DROPPED | IMAGE_RETARGET_DISTANT))
               == (IMAGE_RETARGET_VECTOR | IMAGE_RETARGET_DROPPED
                   | IMAGE_RETARGET_DISTANT);
}

/* The page at scroll_y, rendered, shows the far picture's two halves. */
static bool svg_distant_frame_shows_far(SvgDistantPage *page, int scroll_y,
                                        size_t *decodes)
{
    static uint16_t frame[480 * 272];
    TileCache cache;
    if (!tile_cache_init(&cache, &page->budget,
                         &page->navigation.page.layout, 24)) return false;
    bool ok = tile_cache_set_frame(&cache, frame, 480 * 272)
        && tile_cache_render_frame(&cache, scroll_y, 480, 272, NULL);
    int row = SVG_DISTANT_FAR_TOP - scroll_y + 40;
    ok = ok && row >= 0 && row < 272
        && frame[row * 480 + 100] == 0x07e0u
        && frame[row * 480 + 300] == 0x001fu
        && frame[row * 480 + 440] == 0xffffu;
    if (decodes != NULL) *decodes = cache.decoded_image_builds;
    tile_cache_destroy(&cache);
    return ok;
}

/* A large SVG background first painted far below the reader is measured
   but not rasterized: no surface exists for it until the reader comes
   within two viewports, while the one at the top is rasterized before the
   first paint, at the size an eager load gives it. */
static bool test_far_svg_background_allocates_no_surface_until_approached(
    void)
{
    SvgDistantPage page;
    bool committed = svg_distant_open(&page);
    NavigationSession *navigation = &page.navigation;
    const ImageResource *top = committed ? svg_distant_item(&page, "top")
                                         : NULL;
    const ImageResource *far = committed ? svg_distant_item(&page, "far")
                                         : NULL;
    bool deferred = svg_distant_held(&page.budget, top)
        && svg_distant_released(far)
        && far->width == 400 && far->source_width == 400
        && far->full_width == 400
        && navigation->page.images.stats.decoded_bytes == SVG_DISTANT_BYTES;
    if (!deferred && committed) svg_retarget_report(navigation);
    /* Idle work at the top leaves it alone. */
    if (deferred) retarget_pump(navigation);
    far = deferred ? svg_distant_item(&page, "far") : NULL;
    bool idle = svg_distant_released(far)
        && navigation->page.images.stats.decoded_bytes == SVG_DISTANT_BYTES
        && navigation->page.images.retarget.distant_restores == 0;
    if (!idle && deferred) svg_retarget_report(navigation);
    /* 400 px above the picture: inside two viewports (544 px). */
    bool approached = idle && navigation_set_scroll(
        navigation, SVG_DISTANT_FAR_TOP - 272 - 400);
    if (approached) retarget_pump(navigation);
    far = approached ? svg_distant_item(&page, "far") : NULL;
    /* The top picture, now more than four viewports up, is released. */
    approached = svg_distant_held(&page.budget, far)
        && (far->retarget_flags & IMAGE_RETARGET_DISTANT) == 0
        && svg_distant_released(svg_distant_item(&page, "top"))
        && navigation->page.images.retarget.distant_restores == 1
        && navigation->page.images.stats.decoded_bytes == SVG_DISTANT_BYTES;
    if (!approached && idle) svg_retarget_report(navigation);
    bool ok = committed && deferred && idle && approached;
    if (!ok)
        fprintf(stderr, "svg-distant-defer committed=%d deferred=%d "
                "idle=%d approached=%d error=\"%s\"\n", committed, deferred,
                idle, approached, navigation->last_error);
    bool clean = svg_distant_close(&page, "svg-distant-defer");
    return ok && clean;
}

/* Scrolled to, the far picture paints correctly: after idle work
   rasterized it ahead of the reader, and when a scroll reaches it before
   any idle turn (a held D-pad), the input path rasterizes it once before
   the frame instead of each tile rasterizing it from markup. */
static bool test_far_svg_background_renders_once_scrolled_to(void)
{
    SvgDistantPage page;
    bool committed = svg_distant_open(&page);
    NavigationSession *navigation = &page.navigation;
    if (committed) retarget_pump(navigation);
    /* Straight onto the picture with no idle turn in between. */
    bool jumped = committed
        && svg_distant_released(svg_distant_item(&page, "far"))
        && navigation_set_scroll(navigation, SVG_DISTANT_FAR_TOP - 100)
        && navigation_prepare_near_vector_images(navigation);
    const ImageResource *far = jumped ? svg_distant_item(&page, "far")
                                      : NULL;
    size_t decodes = SIZE_MAX;
    jumped = svg_distant_held(&page.budget, far)
        && navigation->page.images.retarget.distant_input_restores == 1
        && svg_distant_frame_shows_far(&page, SVG_DISTANT_FAR_TOP - 100,
                                       &decodes)
        && decodes == 0;
    if (!jumped && committed) svg_retarget_report(navigation);
    /* A press that barely moves does not look again. */
    bool repeat = jumped && navigation_set_scroll(
            navigation, SVG_DISTANT_FAR_TOP - 90)
        && !navigation_prepare_near_vector_images(navigation)
        && navigation->page.images.retarget.distant_input_restores == 1;
    /* Away and back by idle work: rasterized ahead of the reader, so the
       frame that reaches it decodes nothing. */
    bool returned = repeat && navigation_set_scroll(navigation, 0);
    if (returned) retarget_pump(navigation);
    returned = returned
        && svg_distant_released(svg_distant_item(&page, "far"))
        && navigation_set_scroll(navigation, SVG_DISTANT_FAR_TOP - 600);
    if (returned) retarget_pump(navigation);
    returned = returned
        && svg_distant_held(&page.budget, svg_distant_item(&page, "far"))
        && navigation_set_scroll(navigation, SVG_DISTANT_FAR_TOP - 100)
        && !navigation_prepare_near_vector_images(navigation)
        && svg_distant_frame_shows_far(&page, SVG_DISTANT_FAR_TOP - 100,
                                       &decodes)
        && decodes == 0
        && navigation->page.images.retarget.distant_restores == 1;
    if (!returned && repeat) svg_retarget_report(navigation);
    bool ok = committed && jumped && repeat && returned;
    if (!ok)
        fprintf(stderr, "svg-distant-render committed=%d jumped=%d "
                "repeat=%d returned=%d decodes=%zu\n", committed, jumped,
                repeat, returned, decodes);
    bool clean = svg_distant_close(&page, "svg-distant-render");
    return ok && clean;
}

/* Once the reader is four viewports away again, the far picture's surface
   is released: back at the top, decoded bytes and the resource category
   are what they were before the reader went down (the top picture alone),
   and teardown returns every byte. */
static bool test_far_svg_background_memory_returns_to_zero(void)
{
    SvgDistantPage page;
    bool committed = svg_distant_open(&page);
    NavigationSession *navigation = &page.navigation;
    if (committed) retarget_pump(navigation);
    size_t resource_before = committed
        ? page.budget.categories[BUDGET_CATEGORY_RESOURCE].current : 0;
    bool held = committed && navigation_set_scroll(
        navigation, SVG_DISTANT_FAR_TOP - 100);
    if (held) retarget_pump(navigation);
    held = held && svg_distant_held(&page.budget,
                                    svg_distant_item(&page, "far"))
        && navigation->page.images.stats.decoded_bytes == SVG_DISTANT_BYTES;
    /* Three viewports above: still within four, kept. */
    bool kept = held && navigation_set_scroll(
        navigation, SVG_DISTANT_FAR_TOP - 272 - 3 * 272);
    if (kept) retarget_pump(navigation);
    kept = kept && svg_distant_held(&page.budget,
                                    svg_distant_item(&page, "far"))
        && navigation->page.images.retarget.distant_releases == 1;
    bool released = kept && navigation_set_scroll(navigation, 0);
    if (released) retarget_pump(navigation);
    size_t resource_after = released
        ? page.budget.categories[BUDGET_CATEGORY_RESOURCE].current : 0;
    released = released
        && svg_distant_released(svg_distant_item(&page, "far"))
        && svg_distant_held(&page.budget, svg_distant_item(&page, "top"))
        && navigation->page.images.retarget.distant_releases == 2
        && navigation->page.images.stats.decoded_bytes == SVG_DISTANT_BYTES
        && resource_after == resource_before;
    if (!released && kept) svg_retarget_report(navigation);
    bool ok = committed && held && kept && released;
    if (!ok)
        fprintf(stderr, "svg-distant-release committed=%d held=%d kept=%d "
                "released=%d resource %zu->%zu\n", committed, held, kept,
                released, resource_before, resource_after);
    bool clean = svg_distant_close(&page, "svg-distant-release");
    return ok && clean;
}

/* A large SVG background near the top, which the commit's load leaves for
   the first layout to rasterize, is parsed once: the load's parse, which
   measured it, is the one that first raster uses. The far one is parsed
   once for its size and its parse is not held past that layout. */
static bool test_near_svg_background_parses_once(void)
{
    size_t before = image_svg_parse_count();
    SvgDistantPage page;
    bool committed = svg_distant_open(&page);
    size_t parses = image_svg_parse_count() - before;
    NavigationSession *navigation = &page.navigation;
    const ImageResources *images = &navigation->page.images;
    bool none_held = true;
    for (size_t i = 0; i < IMAGE_SVG_KEPT_PARSE_LIMIT; i++)
        none_held = none_held && images->svg_parses[i].parse == NULL;
    bool once = committed && parses == 2 && none_held
        && images->svg_parse_bytes == 0
        && images->retarget.distant_deferred == 2
        && svg_distant_released(svg_distant_item(&page, "far"))
        && svg_distant_held(&page.budget, svg_distant_item(&page, "top"));
    if (!once && committed) svg_retarget_report(navigation);
    bool ok = committed && once;
    if (!ok)
        fprintf(stderr, "svg-near-parse committed=%d once=%d parses=%zu\n",
                committed, once, parses);
    bool clean = svg_distant_close(&page, "svg-near-parse");
    return ok && clean;
}

static bool svg_kept_defer_always(void *opaque, int width, int height)
{
    (void) opaque;
    return width > 0 && height > 0;
}

typedef struct {
    Budget *budget;
    void *ballast;
    size_t bytes;
} SvgParseReclaim;

static size_t svg_parse_reclaim(void *opaque, size_t needed)
{
    (void) needed;
    SvgParseReclaim *reclaim = opaque;
    if (reclaim->ballast == NULL) return 0;
    budget_free(reclaim->budget, reclaim->ballast);
    reclaim->ballast = NULL;
    return reclaim->bytes;
}

/* Freeing an unrelated cache during parsing must not discount the parse's
   retained charge. Test both refusal at the ordinary cache allowance and
   exact accounting when the same parse does fit. */
static bool test_svg_kept_parse_counts_its_owner_after_reclaim(void)
{
    char markup[16000];
    size_t length = (size_t) snprintf(markup, sizeof(markup),
        "<svg xmlns='http://www.w3.org/2000/svg' width='100' height='100'>");
    for (unsigned i = 0; i < 240; i++) {
        int wrote = snprintf(markup + length, sizeof(markup) - length,
            "<rect x='%u' y='%u' width='2' height='2'/>", i % 90, i % 90);
        if (wrote < 0 || (size_t) wrote >= sizeof(markup) - length)
            return false;
        length += (size_t) wrote;
    }
    memcpy(markup + length, "</svg>", 7);
    length += 6;
    bool ok = true;
    for (unsigned pass = 0; pass < 2; pass++) {
        Budget budget;
        budget_init(&budget, 512u * 1024u);
        SvgParseReclaim reclaim = { .budget = &budget,
                                    .bytes = 480u * 1024u };
        reclaim.ballast = budget_malloc(&budget, reclaim.bytes);
        bool allocated = reclaim.ballast != NULL;
        budget_set_reclaim_hook(&budget, svg_parse_reclaim, &reclaim);
        int width = 0, height = 0;
        bool limited = true, deferred = false;
        ImageSvgParse *parse = NULL;
        size_t allowance = pass == 0 ? IMAGE_SVG_KEPT_PARSE_BYTES
                                     : 256u * 1024u;
        unsigned char *pixels = image_svg_decode_deferrable(
            markup, length, &budget, 40000, &width, &height, NULL, NULL,
            svg_kept_defer_always, NULL, NULL, NULL, &limited, &deferred,
            allowance, &parse);
        bool counted = allocated && pixels == NULL && deferred && !limited
            && reclaim.ballast == NULL && budget.reclaim_calls != 0
            && (pass == 0 ? parse == NULL && budget.current == 0
                : parse != NULL && image_svg_parse_bytes(parse)
                       == budget.current && budget.current <= allowance);
        if (!counted) fprintf(stderr,
            "svg-parse-reclaim pass=%u reported=%zu actual=%zu limit=%zu\n",
            pass, image_svg_parse_bytes(parse), budget.current, allowance);
        budget_free(&budget, pixels);
        image_svg_parse_release(parse);
        budget_free(&budget, reclaim.ballast);
        ok = counted && budget.current == 0
            && budget_active_allocations(&budget, NULL) == 0 && ok;
    }
    return ok;
}

/* A parse kept by a deferred decode rasterizes exactly what a fresh decode
   of the same markup does, at the size asked for, parsing nothing again;
   released unused, or failing mid-raster, it returns every byte. */
static bool test_svg_kept_parse_rasterizes_like_a_fresh_decode(void)
{
    static const char markup[] =
        "<svg xmlns='http://www.w3.org/2000/svg' width='100' height='60' "
        "viewBox='0 0 100 60'><clipPath id='c'><rect width='50' "
        "height='60'/></clipPath><rect width='100' height='60' "
        "fill='#0000ff'/><g clip-path='url(#c)'><circle cx='50' cy='30' "
        "r='30' fill='#ff0000'/></g></svg>";
    Budget budget;
    budget_init(&budget, 4u * MIB);
    bool ok = true;
    for (int attempt = 0; ok && attempt < 3; attempt++) {
        int width = 0, height = 0, sw = 0, sh = 0;
        bool limited = true, deferred = false;
        ImageSvgParse *parse = NULL;
        unsigned char *none = image_svg_decode_deferrable(
            markup, sizeof(markup) - 1, &budget, MIB, &width, &height,
            NULL, NULL, svg_kept_defer_always, NULL, &sw, &sh, &limited,
            &deferred, 64u * 1024u, &parse);
        ok = none == NULL && deferred && !limited && width == 100
            && height == 60 && sw == 100 && sh == 60 && parse != NULL
            && image_svg_parse_bytes(parse) >= sizeof(markup) - 1
            && image_svg_parse_bytes(parse) <= budget.current
            && image_svg_parse_matches(parse, markup, sizeof(markup) - 1)
            && !image_svg_parse_matches(parse, markup, sizeof(markup) - 2);
        if (!ok) {
            image_svg_parse_release(parse);
            break;
        }
        if (attempt == 1) {
            /* Never rasterized: released with the table. */
            image_svg_parse_release(parse);
            ok = budget.current == 0;
            continue;
        }
        size_t parses = image_svg_parse_count();
        int kept_width = 80, kept_height = 48;
        if (attempt == 2) budget_inject_failure_after(&budget, 1);
        unsigned char *kept = image_svg_parse_rasterize(
            parse, (size_t) kept_width * (size_t) kept_height * 4u,
            &kept_width, &kept_height);
        budget_clear_failure_injection(&budget);
        ok = image_svg_parse_count() == parses;
        if (attempt == 2) {
            ok = ok && kept == NULL && budget.current == 0;
            budget_free(&budget, kept);
            continue;
        }
        int fresh_width = 80, fresh_height = 48;
        unsigned char *fresh = image_svg_decode(
            markup, sizeof(markup) - 1, &budget, MIB, &fresh_width,
            &fresh_height);
        ok = ok && kept != NULL && fresh != NULL && kept_width == 80
            && kept_height == 48 && fresh_width == 80 && fresh_height == 48
            && memcmp(kept, fresh, 80u * 48u * 4u) == 0;
        budget_free(&budget, kept);
        budget_free(&budget, fresh);
        ok = ok && budget.current == 0;
    }
    if (!ok) fprintf(stderr, "svg-kept-parse mismatch, %zu bytes left\n",
                     budget.current);
    return ok && budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0;
}

/* The same two pictures inside a 272 px scroller at the top of the page:
   #far lies 6,400 px down the scroller's content, so its document position
   is far below the reader whatever the scroller shows. */
static bool svg_scroller_open(SvgDistantPage *page)
{
    static const char html[] =
        "<!doctype html><title>Scrolled SVG</title>"
        "<meta name=viewport content=\"width=device-width\">"
        "<style>html,body{margin:0}#scroller{height:272px;"
        "overflow-y:scroll}.art{width:400px;height:400px;"
        "background-repeat:no-repeat;background-size:400px 400px}"
        "#top{background-image:url(\"" SVG_DISTANT_ART("ff0000") "\")}"
        "#far{background-image:url(\"" SVG_DISTANT_ART("00ff00") "\")}"
        "#gap{height:6000px}#tail{height:3000px}</style>"
        "<div id=scroller><div class=art id=top></div><div id=gap></div>"
        "<div class=art id=far></div></div><div id=tail></div>";
    return svg_distant_open_html(page, html, sizeof(html) - 1u);
}

static lxb_dom_node_t *svg_distant_node(SvgDistantPage *page, const char *id)
{
    lxb_dom_node_t *root =
        lxb_dom_interface_node(page->navigation.page.document.html);
    return root == NULL ? NULL : test_find_id(root, id);
}

/* The page at scroll 0, rendered, shows a picture whose top is at `row`
   with left half `left` (RGB565) and right half blue. */
static bool svg_distant_frame_shows(SvgDistantPage *page, int row,
                                    uint16_t left, size_t *decodes)
{
    static uint16_t frame[480 * 272];
    TileCache cache;
    if (!tile_cache_init(&cache, &page->budget,
                         &page->navigation.page.layout, 24)) return false;
    bool ok = tile_cache_set_frame(&cache, frame, 480 * 272)
        && tile_cache_render_frame(&cache, 0, 480, 272, NULL);
    row += 40;
    ok = ok && row >= 0 && row < 272
        && frame[row * 480 + 100] == left
        && frame[row * 480 + 300] == 0x001fu
        && frame[row * 480 + 420] == 0xffffu;
    if (decodes != NULL) *decodes = cache.decoded_image_builds;
    tile_cache_destroy(&cache);
    return ok;
}

/* The distance rule judges a paint where its scroll containers show it:
   scrolling a scroller at the top of the page onto the picture 6,400 px
   down its content rasterizes that picture (by idle work, or before the
   frame when input got there first) and releases the one it scrolled
   away, although the reader never moved; a load judges the lent boxes the
   same way. */
static bool test_svg_background_distance_follows_scroll_containers(void)
{
    SvgDistantPage page;
    bool committed = svg_scroller_open(&page);
    NavigationSession *navigation = &page.navigation;
    LayoutDocument *layout = &navigation->page.layout;
    lxb_dom_node_t *scroller = committed
        ? svg_distant_node(&page, "scroller") : NULL;
    if (committed) retarget_pump(navigation);
    bool opened = scroller != NULL
        && svg_distant_held(&page.budget, svg_distant_item(&page, "top"))
        && svg_distant_released(svg_distant_item(&page, "far"));
    if (!opened && committed) svg_retarget_report(navigation);
    /* Idle work after the scroller moves onto #far. */
    bool idle = opened
        && layout_scroll_node(layout, scroller, 0, SVG_DISTANT_FAR_TOP - 100);
    if (idle) retarget_pump(navigation);
    size_t decodes = SIZE_MAX;
    idle = idle
        && svg_distant_held(&page.budget, svg_distant_item(&page, "far"))
        && svg_distant_released(svg_distant_item(&page, "top"))
        && navigation->page.images.retarget.distant_restores == 1
        && navigation->page.images.retarget.distant_releases == 1
        && svg_distant_frame_shows(&page, 100, 0x07e0u, &decodes)
        && decodes == 0;
    if (!idle && opened) svg_retarget_report(navigation);
    /* A load lent these boxes judges #far near and #top far. */
    ImageLayoutBoxes boxes;
    bool lent = idle;
    if (lent) {
        layout_lend_image_boxes(layout, &boxes);
        boxes.reader_known = true;
        boxes.reader_top = 0;
        boxes.reader_bottom = 272;
        boxes.reader_near = 2 * 272;
        lent = !layout_image_box_far(&boxes, svg_distant_node(&page, "far"))
            && layout_image_box_far(&boxes, svg_distant_node(&page, "top"));
    }
    /* Input scrolls back to #top with no idle turn in between: it is
       rasterized once before the frame, which then decodes nothing. */
    bool input = lent && layout_scroll_node(layout, scroller, 0, 0)
        && navigation_prepare_near_vector_images(navigation)
        && svg_distant_held(&page.budget, svg_distant_item(&page, "top"))
        && navigation->page.images.retarget.distant_input_restores == 1
        && svg_distant_frame_shows(&page, 0, 0xf800u, &decodes)
        && decodes == 0;
    if (!input && lent) svg_retarget_report(navigation);
    /* A scroller step of a few pixels does not look again. */
    bool repeat = input && layout_scroll_node(layout, scroller, 0, 10)
        && !navigation_prepare_near_vector_images(navigation)
        && navigation->page.images.retarget.distant_input_restores == 1;
    /* Boxes kept after their layout is released are judged the same way. */
    bool taken = repeat
        && layout_scroll_node(layout, scroller, 0, SVG_DISTANT_FAR_TOP - 100);
    if (taken) {
        layout_lend_image_boxes(layout, &boxes);
        layout_keep_image_boxes(layout, &boxes);
        boxes.reader_known = true;
        boxes.reader_top = 0;
        boxes.reader_bottom = 272;
        boxes.reader_near = 2 * 272;
        taken = boxes.layout == NULL && boxes.taken != NULL
            && !layout_image_box_far(&boxes, svg_distant_node(&page, "far"))
            && layout_image_box_far(&boxes, svg_distant_node(&page, "top"));
        layout_release_image_boxes(&boxes);
    }
    bool ok = committed && opened && idle && lent && input && repeat && taken;
    if (!ok)
        fprintf(stderr, "svg-scroller-distance committed=%d opened=%d "
                "idle=%d lent=%d input=%d repeat=%d taken=%d decodes=%zu\n",
                committed, opened, idle, lent, input, repeat, taken, decodes);
    bool clean = svg_distant_close(&page, "svg-scroller-distance");
    return ok && clean;
}
