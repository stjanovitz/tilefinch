#include "tilefinch_test_common.h"
#include "../src/image_svg_decode_internal.h"

static int test_svg_default_aspect_ratio(Budget *budget)
{
    static const char *const alignment[] = {
        "", "preserveAspectRatio='xMidYMid meet'",
        "preserveAspectRatio='none'", "preserveAspectRatio='xMidYMid slice'",
        "preserveAspectRatio='xMinYMin meet'",
        "preserveAspectRatio='xMaxYMax meet'"
    };
    /* A repository-owned rectangle exposes viewport alignment without
       borrowed logos or page material. The 2:1 viewBox sits in a square. */
    for (size_t mode = 0;
         mode < sizeof(alignment) / sizeof(alignment[0]); mode++) {
        char source[256];
        int length = snprintf(source, sizeof(source),
            "<svg xmlns='http://www.w3.org/2000/svg' width='16' height='16' "
            "viewBox='0 0 8 4' %s><rect width='8' height='4' "
            "fill='#ff0000'/></svg>", alignment[mode]);
        CHECK(length > 0 && (size_t) length < sizeof(source));
        for (int size = 16; size >= 8; size /= 2) {
            int width = size, height = size;
            unsigned char *pixels = image_svg_decode(
                source, (size_t) length, budget, 1024u, &width, &height);
            CHECK(pixels != NULL && width == size && height == size);
            for (int y = 0; y < size; y++) {
                bool painted = mode == 2u || mode == 3u
                    || (mode == 4u ? y < size / 2
                        : mode == 5u ? y >= size / 2
                        : y >= size / 4 && y < 3 * size / 4);
                for (int x = 0; x < size; x++) {
                    const unsigned char *pixel = pixels
                        + 4u * (size_t) (y * size + x);
                    if (pixel[3] != (painted ? 255u : 0u)) {
                        fprintf(stderr, "SVG alignment mode=%zu size=%d "
                                "pixel=%d/%d alpha=%u expected=%u\n",
                                mode, size, x, y, pixel[3],
                                painted ? 255u : 0u);
                    }
                    CHECK(pixel[3] == (painted ? 255u : 0u));
                    if (painted) CHECK(pixel[0] == 255u
                                       && pixel[1] == 0u && pixel[2] == 0u);
                }
            }
            budget_free(budget, pixels);
            CHECK(budget->current == 0u);
        }
        for (size_t refusal = 0; refusal < 24u; refusal++) {
            int width = 16, height = 16;
            budget_inject_failure_after(budget, refusal);
            unsigned char *pixels = image_svg_decode(
                source, (size_t) length, budget, 1024u, &width, &height);
            budget_clear_failure_injection(budget);
            budget_free(budget, pixels);
            CHECK(budget->current == 0u);
        }
    }
    static const char nested[] =
        "<svg xmlns='http://www.w3.org/2000/svg' width='16' height='16' "
        "viewBox='0 0 16 16' preserveAspectRatio='none'>"
        "<svg x='4' y='4' width='8' height='8' viewBox='0 0 8 4'>"
        "<rect width='8' height='4' fill='#ff0000'/></svg></svg>";
    int width = 16, height = 16;
    unsigned char *pixels = image_svg_decode(
        nested, sizeof(nested) - 1u, budget, 1024u, &width, &height);
    CHECK(pixels != NULL && width == 16 && height == 16);
    /* A child's missing alignment takes its initial value, not the root's
       explicitly non-uniform alignment. */
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            CHECK(pixels[4u * (size_t) (y * 16 + x) + 3u]
                  == (x >= 4 && x < 12 && y >= 6 && y < 10 ? 255u : 0u));
        }
    }
    budget_free(budget, pixels);
    CHECK(budget->current == 0u);
    return 0;
}

static int test_scaled_background_geometry(Budget *budget)
{
    static const char page[] =
        "<!doctype html><style>html,body{margin:0;background:#fff}"
        "div{position:absolute;left:8px;width:8px;height:4px;"
        "background-image:url(https://example.test/checker.png);"
        "transform:scale(2);transform-origin:left top}"
        "#single{top:8px;background-size:8px 4px;"
        "background-position:2px 1px;background-repeat:no-repeat}"
        "#repeat{top:24px;background-size:4px 2px}"
        "#natural{top:40px}"
        "#small{top:56px;background-size:8px 4px;"
        "background-position:-2px -2px;transform:scale(.5)}"
        "</style><div id=single></div><div id=repeat></div>"
        "<div id=natural></div><div id=small></div>";
    static unsigned char pixels[] = {
        255,0,0,255, 255,0,0,255, 0,255,0,255, 0,255,0,255,
        0,0,255,255, 0,0,255,255, 0,0,0,255, 0,0,0,255
    };
    PocDocument document = {0};
    Stylesheet sheet = {0};
    LayoutDocument layout = {0};
    CHECK(document_parse(&document, budget, page, sizeof(page) - 1u, 24)
          && stylesheet_build(&sheet, budget, &document, 64));
    const char *const ids[] = {"single", "repeat", "natural", "small"};
    ImageResource items[4] = {0};
    for (size_t i = 0; i < 4u; i++) {
        items[i] = (ImageResource) {
            .node = find_id(lxb_dom_interface_node(document.html), ids[i]),
            .pixels = pixels, .width = 4, .height = 2,
            .is_background = true
        };
    }
    ImageResources images = {.items = items, .count = 4};
    CHECK(layout_build(&layout, budget, &document, &sheet,
                       NULL, &images, 64));
    const int widths[] = {16, 8, 8, 4};
    const int heights[] = {8, 4, 4, 2};
    const int offsets_x[] = {4, 0, 0, -1};
    const int offsets_y[] = {2, 0, 0, -1};
    for (size_t item = 0; item < 4u; item++) {
        const DrawCommand *image = NULL;
        for (size_t at = 0; at < layout.count; at++) {
            if (layout.commands[at].type == DRAW_IMAGE
                && layout.commands[at].image == &items[item]) {
                CHECK(image == NULL);
                image = &layout.commands[at];
            }
        }
        CHECK(image != NULL
              && draw_command_image_sprite_width(image) == widths[item]
              && draw_command_image_sprite_height(image) == heights[item]
              && draw_command_image_offset_x(image) == offsets_x[item]
              && draw_command_image_offset_y(image) == offsets_y[item]);
    }
    uint16_t frame[64u * 64u];
    TileCache cache = {0};
    CHECK(tile_cache_init(&cache, budget, &layout, 4)
          && tile_cache_set_frame(&cache, frame, 64u * 64u)
          && tile_cache_render_frame(&cache, 0, 64, 64, NULL));
    /* The shifted non-repeating image starts four/two scaled pixels into
       the box. Repeating and natural-size tiles both retain an 8x4 cycle. */
    CHECK(frame[8u * 64u + 8u] == UINT16_C(0xffff)
          && frame[10u * 64u + 12u] == UINT16_C(0xf800)
          && frame[10u * 64u + 20u] == UINT16_C(0x07e0)
          && frame[14u * 64u + 12u] == UINT16_C(0x001f));
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 16; x++) {
            static const uint16_t colors[] = {0xf800, 0x07e0, 0x001f, 0};
            uint16_t expected = colors[(y % 4 >= 2 ? 2 : 0)
                                       + (x % 8 >= 4 ? 1 : 0)];
            CHECK(frame[(size_t) (24 + y) * 64u + (size_t) (8 + x)]
                      == expected
                  && frame[(size_t) (40 + y) * 64u + (size_t) (8 + x)]
                      == expected);
        }
    }
    tile_cache_destroy(&cache);
    layout_destroy(&layout);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0u);
    return 0;
}

static int test_gradient_mask_paint(Budget *budget)
{
    static const char page[] =
        "<!doctype html><style>body{margin:0;background:white}"
        ".paint{position:absolute;width:16px;height:8px;"
        "background:linear-gradient(90deg,red 0%,red 50%,blue 50%,blue 100%)}"
        "#reference{left:0;top:0}#alpha{left:24px;top:0;opacity:.5}"
        "#mask{left:0;top:16px;mask:url(mask.svg)}"
        "#half{left:24px;top:16px;mask:url(mask.svg)}"
        "#pseudo{position:absolute;left:0;top:32px;width:16px;height:8px}"
        "#pseudo::before{content:'';display:block;width:16px;height:8px;"
        "background:linear-gradient(90deg,red 0%,red 50%,blue 50%,blue 100%);"
        "mask:url(mask.svg)}"
        "</style><div id=reference class=paint></div>"
        "<div id=alpha class=paint></div><div id=mask class=paint></div>"
        "<div id=half class=paint></div><div id=pseudo></div>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    LayoutDocument layout = {0};
    CHECK(document_parse(&document, budget, page, sizeof(page) - 1u, 17)
          && stylesheet_build(&sheet, budget, &document, 64));
    unsigned char pixels[16u * 8u * 4u], half[sizeof(pixels)];
    memset(pixels, 255, sizeof(pixels));
    pixels[4u * (16u + 1u) + 3u] = 0;
    memcpy(half, pixels, sizeof(pixels));
    /* Opacity .5 uses 128/256 of opaque alpha, truncated to 127. */
    for (size_t i = 3; i < sizeof(half); i += 4) half[i] = 127;
    half[4u * (16u + 1u) + 3u] = 0;
    ImageResource items[] = {
        {.node=find_id(lxb_dom_interface_node(document.html), "mask"),
         .pixels=pixels,.width=16,.height=8,.is_mask=true},
        {.node=find_id(lxb_dom_interface_node(document.html), "half"),
         .pixels=half,.width=16,.height=8,.is_mask=true},
        {.node=find_id(lxb_dom_interface_node(document.html), "pseudo"),
         .pixels=pixels,.width=16,.height=8,.is_mask=true,.pseudo=PSEUDO_BEFORE}
    };
    ImageResources images = {.items=items,.count=3};
    CHECK(layout_build(&layout, budget, &document, &sheet, NULL, &images, 64));
    size_t masks = 0;
    for (size_t i = 0; i < layout.count; i++) {
        if (layout.commands[i].type == DRAW_IMAGE) {
            CHECK(draw_command_mask_gradient(&layout.commands[i], NULL));
            masks++;
        }
    }
    CHECK(masks == 3);
    TileCache cache = {0};
    CHECK(tile_cache_init(&cache, budget, &layout, 4));
    uint16_t frame[64u * 48u];
    CHECK(tile_cache_set_frame(&cache, frame, 64u * 48u)
          && tile_cache_render_frame(&cache, 0, 64, 48, NULL));
    for (size_t y = 0; y < 8; y++) {
        for (size_t x = 0; x < 16; x++) {
            bool hole = x == 1 && y == 1;
            uint16_t reference = hole ? UINT16_C(0xffff) : frame[y * 64u + x];
            if (frame[(y + 16u) * 64u + x] != reference
                || frame[(y + 32u) * 64u + x] != reference)
                fprintf(stderr, "mask mismatch %zu/%zu %04x/%04x/%04x\n",
                        x, y, reference, frame[(y + 16u) * 64u + x],
                        frame[(y + 32u) * 64u + x]);
            CHECK(frame[(y + 16u) * 64u + x] == reference
                  && frame[(y + 32u) * 64u + x] == reference);
            CHECK(frame[(y + 16u) * 64u + x + 24u]
                  == (hole ? UINT16_C(0xffff) : frame[y * 64u + x + 24u]));
        }
    }
    tile_cache_destroy(&cache);
    layout_destroy(&layout);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget->current == 0u);
    return 0;
}

static int test_enlarged_photo_and_transparent_card(Budget *budget, bool photo_only)
{
    static const char page[] =
        "<!doctype html><style>html,body{margin:0;background:white}"
        "#card{position:absolute;left:8px;top:8px;width:24px;height:24px;"
        "border-radius:4px;box-shadow:1px 2px 4px #0008}"
        "img{display:block;width:8px;height:8px}"
        "</style><div id=card><img id=photo></div>"
        "<img id=alpha style='position:absolute;top:36px;left:0'>";
    PocDocument document = {0}; Stylesheet sheet = {0};
    LayoutDocument layout = {0}; TileCache cache = {0};
    CHECK(document_parse(&document,budget,page,sizeof(page)-1,12)
          && stylesheet_build(&sheet,budget,&document,48));
    unsigned char pixels[] = {0,0,0,255, 255,255,255,255,
                              0,0,0,255, 255,255,255,255};
    unsigned char transparent[] = {255,0,0,255, 0,0,255,0,
                                   255,0,0,255, 0,0,255,0};
    ImageResource items[] = {
        {.node=find_id(lxb_dom_interface_node(document.html),"photo"),
         .pixels=pixels,.width=2,.height=2},
        {.node=find_id(lxb_dom_interface_node(document.html),"alpha"),
         .pixels=transparent,.width=2,.height=2}
    };
    ImageResources images = {.items=items,.count=2};
    CHECK(layout_build(&layout,budget,&document,&sheet,NULL,&images,48));
    uint16_t frame[48u*48u];
    CHECK(tile_cache_init(&cache,budget,&layout,4)
          && tile_cache_set_frame(&cache,frame,48u*48u)
          && tile_cache_render_frame(&cache,0,48,48,NULL));
    /* A transparent card never receives its own outer shadow. */
    if (!photo_only) CHECK(frame[20u*48u+20u]==UINT16_C(0xffff));
    /* Pixel-center interpolation creates intermediate greys, not blocks. */
    unsigned left=(frame[10u*48u+11u]>>11)&31u;
    unsigned right=(frame[10u*48u+12u]>>11)&31u;
    CHECK(left>8u && left<16u && right>16u && right<24u);
    /* Interpolate premultiplied color: invisible blue must not tint red. */
    uint16_t alpha = frame[38u*48u+3u];
    CHECK((alpha>>11)==31u && (alpha&31u)>8u && (alpha&31u)<16u);
    tile_cache_destroy(&cache);layout_destroy(&layout);
    stylesheet_destroy(&sheet);document_destroy(&document);
    CHECK(budget->current==0u);
    return 0;
}

static int test_shadow_offset_visual_clone(Budget *budget)
{
    DrawCommand shadow = {.type=DRAW_SHADOW_RECT,
        .image_fit=LAYOUT_SHADOW_OUTSIDE_BOX,.x=8,.y=8,.width=32,.height=32,
        .scale=4,.radius=4,.opacity_scale=256};
    draw_command_set_shadow_offset(&shadow,6,-8);
    LayoutDocument source = {.budget=budget,.commands=&shadow,.count=1,
        .capacity=1,.width=96,.scroll_width=96,.height=96};
    CHECK(viewport_context_init(&source.viewport,96,96,48,48));
    LayoutDocument visual = {0};
    CHECK(layout_clone_visual(&visual,&source));
    CHECK((int16_t)visual.commands[0].text_length==3
          && (int16_t)(visual.commands[0].text_length>>16)==-4);
    layout_destroy(&visual);
    CHECK(budget->current==0u);
    return 0;
}

int main(int argc, char **argv)
{
    Budget budget;
    budget_init(&budget, 16u * MIB);
    budget_install_lexbor(&budget);
    if(argc==2 && strcmp(argv[1],"--shadow-scale-only")==0)
        return test_shadow_offset_visual_clone(&budget);
    CHECK(test_shadow_offset_visual_clone(&budget)==0);
    CHECK(test_enlarged_photo_and_transparent_card(&budget,
        argc == 2 && strcmp(argv[1], "--photo-only") == 0)==0);
    if (argc == 2) return 0;
    CHECK(test_svg_default_aspect_ratio(&budget) == 0);
    CHECK(test_scaled_background_geometry(&budget) == 0);
    CHECK(test_gradient_mask_paint(&budget) == 0);
    puts("SVG aspect ratio and transformed background geometry passed");
    return 0;
}
