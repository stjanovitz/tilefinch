/*
 * The media chrome, over a 32-bit video buffer.
 *
 * During fullscreen video the panel scans out 8888 so the decoder's bytes
 * reach it uninterpreted (see src/psp_display.c and
 * src/psp_media_present_ge.c). The player's overlay -- title bar, scrubber,
 * hints, the play control, the seek preview and the failed panel -- is drawn
 * by psp_ui_media_composite_with_preview, which is sixteen-bit code, several
 * thousand instructions of rounded rectangles, antialiased glyphs and blends
 * that this file has no business duplicating in a second pixel format.
 *
 * So it is not duplicated. The overlay is composed exactly as it always was,
 * into a scratch surface, and only the rows it touches make the round trip:
 * the video's 32-bit pixels are narrowed into the scratch as a blend backdrop,
 * the existing composite runs over them at its ordinary coordinates, and the
 * result is widened back. Everything outside those rows -- the picture itself,
 * which is most of the frame -- never passes through sixteen bits at all.
 *
 * The mapping follows the PSP's native formats: byte 0 is red in 8888 and the
 * low five-bit field is red in 5650. The shared target-aware RGB565 helpers
 * keep this round trip aligned with the page/chrome rasterizer, while the
 * present probe's channel-map line records both halves on device.
 *
 * Out of line in its own translation unit so psp_ui_composite -- the ratcheted
 * page compositor -- is not asked to carry any of it.
 */

#include "tilefinch/psp_ui.h"

#include <stddef.h>

#include "psp_media_pixels.h"

/*
 * The narrowing is the same one the software scaler performs on a decoded
 * pixel, and the widening is its inverse with the low bits replicated so a
 * full-scale channel stays full-scale. A pixel that makes the round trip loses
 * the low three bits of red and blue and the low two of green -- exactly the
 * quantization the overlay's backdrop has always had, because the whole frame
 * used to be 16-bit -- and only inside the overlay's own rows.
 */
static uint16_t ui_media_narrow(uint32_t pixel)
{
    const unsigned char *bytes = (const unsigned char *) &pixel;
    return psp_media_rgba565(bytes);
}

static uint32_t ui_media_widen(uint16_t pixel)
{
    unsigned red = tilefinch_rgb565_red_code(pixel);
    unsigned green = tilefinch_rgb565_green_code(pixel);
    unsigned blue = tilefinch_rgb565_blue_code(pixel);
    red = (red << 3) | (red >> 2);
    green = (green << 2) | (green >> 4);
    blue = (blue << 3) | (blue >> 2);
    /* Opaque, though the panel ignores the fourth byte in 8888; the letterbox
       bands are cleared to zero and neither choice is visible. */
    return (uint32_t) red | ((uint32_t) green << 8)
        | ((uint32_t) blue << 16) | UINT32_C(0xff000000);
}

static void ui_media_composite_8888(
    const PspUiMediaState *media, const PspUiMediaPreview *preview,
    uint32_t *pixels, int width, int height, int stride,
    uint16_t *scratch, bool controls_only)
{
    if (media == NULL || pixels == NULL || scratch == NULL
        || width <= 0 || height <= 0 || stride < width) return;
    PspUiRowBand bands[PSP_UI_MEDIA_OVERLAY_BAND_LIMIT];
    size_t count = psp_ui_media_overlay_bands(
        media, width, height, bands, PSP_UI_MEDIA_OVERLAY_BAND_LIMIT);
    if (count == 0) return;
    if (controls_only) {
        size_t bottom = count;
        while (bottom != 0u) {
            bottom--;
            if (bands[bottom].bottom == height) break;
        }
        if (bands[bottom].bottom != height) return;
        bands[0] = bands[bottom];
        count = 1u;
    }
    for (size_t band = 0; band < count; band++) {
        for (int y = bands[band].top; y < bands[band].bottom; y++) {
            const uint32_t *source = pixels + (size_t) y * (size_t) stride;
            uint16_t *destination = scratch + (size_t) y * (size_t) stride;
            for (int x = 0; x < width; x++)
                destination[x] = ui_media_narrow(source[x]);
        }
    }
    /* The scratch is stride-wide and full-height, so every coordinate the
       composite computes -- centres, panel rectangles, the bottom edge -- is
       the coordinate it would have used on the real surface. */
    if (controls_only) {
        psp_ui_media_composite_controls(
            media, scratch, width, height, stride);
    } else {
        psp_ui_media_composite_with_preview(
            media, preview, scratch, width, height, stride);
    }
    for (size_t band = 0; band < count; band++) {
        for (int y = bands[band].top; y < bands[band].bottom; y++) {
            const uint16_t *source = scratch + (size_t) y * (size_t) stride;
            uint32_t *destination = pixels + (size_t) y * (size_t) stride;
            for (int x = 0; x < width; x++)
                destination[x] = ui_media_widen(source[x]);
        }
    }
}

void psp_ui_media_composite_8888(
    const PspUiMediaState *media, const PspUiMediaPreview *preview,
    uint32_t *pixels, int width, int height, int stride,
    uint16_t *scratch)
{
    ui_media_composite_8888(
        media, preview, pixels, width, height, stride, scratch, false);
}

void psp_ui_media_composite_controls_8888(
    const PspUiMediaState *media, uint32_t *pixels,
    int width, int height, int stride, uint16_t *scratch)
{
    ui_media_composite_8888(
        media, NULL, pixels, width, height, stride, scratch, true);
}
