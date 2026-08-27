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
 * into a scratch surface, and only the regions it touches make the round trip:
 * the video's 32-bit pixels are narrowed into the scratch as a blend backdrop,
 * the existing composite runs over them at its ordinary coordinates, and the
 * result is widened back. Everything outside those exact rectangles -- the
 * picture itself, which is most of the frame -- never passes through sixteen
 * bits at all.
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
#include <string.h>

#include "psp_media_pixels.h"

/*
 * The narrowing is the same one the software scaler performs on a decoded
 * pixel, and the widening is its inverse with the low bits replicated so a
 * full-scale channel stays full-scale. A pixel that makes the round trip loses
 * the low three bits of red and blue and the low two of green -- exactly the
 * quantization the overlay's backdrop has always had, because the whole frame
 * used to be 16-bit -- and only inside the overlay's own regions.
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
    uint16_t *scratch, bool controls_only, bool without_track_menu)
{
    if (media == NULL || pixels == NULL || scratch == NULL
        || width <= 0 || height <= 0 || stride < width) return;
    PspUiOverlayRegion regions[PSP_UI_MEDIA_OVERLAY_REGION_LIMIT];
    size_t count = without_track_menu
        ? psp_ui_media_overlay_regions_without_track_menu(
              media, preview, width, height, regions,
              PSP_UI_MEDIA_OVERLAY_REGION_LIMIT)
        : psp_ui_media_overlay_regions(
              media, preview, width, height, regions,
              PSP_UI_MEDIA_OVERLAY_REGION_LIMIT);
    if (controls_only) {
        size_t bottom = 0u;
        while (bottom < count
               && !(regions[bottom].left == 0
                    && regions[bottom].right == width
                    && regions[bottom].bottom == height
                    && !regions[bottom].needs_backdrop)) bottom++;
        if (bottom == count) return;
        regions[0] = regions[bottom];
        count = 1u;
    }
    if (count == 0u) return;
    for (size_t region = 0; region < count; region++) {
        const PspUiOverlayRegion *bounds = &regions[region];
        for (int y = bounds->top; y < bounds->bottom; y++) {
            const uint32_t *source = pixels + (size_t) y * (size_t) stride;
            uint16_t *destination = scratch + (size_t) y * (size_t) stride;
            size_t pixels_wide = (size_t) (bounds->right - bounds->left);
            if (!bounds->needs_backdrop) {
                memset(
                    destination + bounds->left, 0,
                    pixels_wide * sizeof(*destination));
            } else {
                for (int x = bounds->left; x < bounds->right; x++)
                    destination[x] = ui_media_narrow(source[x]);
            }
        }
    }
    /* The scratch is stride-wide and full-height, so every coordinate the
       composite computes -- centres, panel rectangles, the bottom edge -- is
       the coordinate it would have used on the real surface. */
    if (controls_only) {
        psp_ui_media_composite_controls(
            media, scratch, width, height, stride);
    } else if (without_track_menu) {
        psp_ui_media_composite_without_track_menu(
            media, preview, scratch, width, height, stride);
    } else {
        psp_ui_media_composite_with_preview(
            media, preview, scratch, width, height, stride);
    }
    for (size_t region = 0; region < count; region++) {
        const PspUiOverlayRegion *bounds = &regions[region];
        for (int y = bounds->top; y < bounds->bottom; y++) {
            const uint16_t *source = scratch + (size_t) y * (size_t) stride;
            uint32_t *destination = pixels + (size_t) y * (size_t) stride;
            for (int x = bounds->left; x < bounds->right; x++)
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
        media, preview, pixels, width, height, stride, scratch, false, false);
}

void psp_ui_media_composite_controls_8888(
    const PspUiMediaState *media, uint32_t *pixels,
    int width, int height, int stride, uint16_t *scratch)
{
    ui_media_composite_8888(
        media, NULL, pixels, width, height, stride, scratch, true, false);
}

static uint32_t ui_media_menu_hash_byte(uint32_t hash, unsigned char value)
{
    return (hash ^ value) * UINT32_C(16777619);
}

static uint32_t ui_media_menu_hash_string(uint32_t hash, const char *text,
                                          size_t capacity)
{
    if (text == NULL) return ui_media_menu_hash_byte(hash, 0u);
    size_t at = 0u;
    while (at < capacity && text[at] != '\0')
        hash = ui_media_menu_hash_byte(hash, (unsigned char) text[at++]);
    return ui_media_menu_hash_byte(hash, 0u);
}

static uint32_t ui_media_menu_signature(
    const PspUiMediaPresentation *presentation)
{
    uint32_t hash = UINT32_C(2166136261);
    /* A retained opaque menu is invalid when the process theme changes even
       if its track catalog is byte-identical. */
    const unsigned char *palette =
        (const unsigned char *) psp_ui_theme_active_palette;
    for (size_t at = 0; at < sizeof(*psp_ui_theme_active_palette); at++)
        hash = ui_media_menu_hash_byte(hash, palette[at]);
    if (presentation == NULL) return hash;
    hash = ui_media_menu_hash_byte(hash, presentation->track_menu_tab);
    hash = ui_media_menu_hash_byte(hash, presentation->track_menu_selection);
    hash = ui_media_menu_hash_byte(hash, presentation->audio_track_count);
    hash = ui_media_menu_hash_byte(hash, presentation->subtitle_track_count);
    hash = ui_media_menu_hash_byte(
        hash, (unsigned char) (presentation->selected_audio_track + 1));
    hash = ui_media_menu_hash_byte(
        hash, (unsigned char) (presentation->selected_subtitle_track + 1));
    size_t audio_count = presentation->audio_track_count;
    if (audio_count > PSP_UI_MEDIA_TRACK_LIMIT)
        audio_count = PSP_UI_MEDIA_TRACK_LIMIT;
    for (size_t at = 0; at < audio_count; at++) {
        hash = ui_media_menu_hash_string(
            hash, presentation->audio_tracks[at].label,
            sizeof(presentation->audio_tracks[at].label));
    }
    size_t subtitle_count = presentation->subtitle_track_count;
    if (subtitle_count > PSP_UI_MEDIA_TRACK_LIMIT)
        subtitle_count = PSP_UI_MEDIA_TRACK_LIMIT;
    for (size_t at = 0; at < subtitle_count; at++) {
        hash = ui_media_menu_hash_string(
            hash, presentation->subtitle_tracks[at].label,
            sizeof(presentation->subtitle_tracks[at].label));
    }
    return hash;
}

void psp_ui_media_composite_8888_cached(
    const PspUiMediaState *media, const PspUiMediaPreview *preview,
    uint32_t *pixels, int width, int height, int stride,
    uint16_t *scratch, uint16_t *menu_pixels, size_t menu_pixel_capacity,
    uint32_t edram_epoch, PspUiMediaTrackMenuCache *menu_cache,
    PspUiMediaTrackMenuBlit menu_blit, void *menu_blit_context)
{
    const PspUiMediaPresentation *presentation = media == NULL
        ? NULL : media->presentation;
    size_t required = (size_t) PSP_UI_MEDIA_TRACK_MENU_WIDTH
        * (size_t) PSP_UI_MEDIA_TRACK_MENU_HEIGHT;
    if (presentation == NULL || !presentation->track_menu_open
        || menu_pixels == NULL || menu_pixel_capacity < required
        || menu_cache == NULL
        || width < (int) PSP_UI_MEDIA_TRACK_MENU_WIDTH
        || height < 48 + (int) PSP_UI_MEDIA_TRACK_MENU_HEIGHT) {
        psp_ui_media_composite_8888(
            media, preview, pixels, width, height, stride, scratch);
        return;
    }

    /* Paint the moving controls first, excluding the opaque menu rectangle.
       The retained menu is copied last, preserving the ordinary draw order. */
    ui_media_composite_8888(
        media, preview, pixels, width, height, stride,
        scratch, false, true);
    uint32_t signature = ui_media_menu_signature(presentation);
    bool source_changed = !menu_cache->valid
        || menu_cache->signature != signature
        || menu_cache->edram_epoch != edram_epoch;
    if (source_changed) {
        memset(menu_pixels, 0, required * sizeof(*menu_pixels));
        psp_ui_media_raster_track_menu(
            media, menu_pixels,
            (int) PSP_UI_MEDIA_TRACK_MENU_WIDTH,
            (int) PSP_UI_MEDIA_TRACK_MENU_HEIGHT,
            (int) PSP_UI_MEDIA_TRACK_MENU_WIDTH);
        menu_cache->signature = signature;
        menu_cache->edram_epoch = edram_epoch;
        menu_cache->valid = true;
    }
    int left = width / 2 - (int) PSP_UI_MEDIA_TRACK_MENU_WIDTH / 2;
    int top = 48;
    if (menu_blit != NULL && menu_blit(
            menu_blit_context, menu_pixels,
            (int) PSP_UI_MEDIA_TRACK_MENU_WIDTH,
            (int) PSP_UI_MEDIA_TRACK_MENU_HEIGHT,
            (int) PSP_UI_MEDIA_TRACK_MENU_WIDTH, source_changed,
            pixels, stride, left, top)) {
        return;
    }
    for (int y = 0; y < (int) PSP_UI_MEDIA_TRACK_MENU_HEIGHT; y++) {
        const uint16_t *source = menu_pixels
            + (size_t) y * PSP_UI_MEDIA_TRACK_MENU_WIDTH;
        uint32_t *destination = pixels
            + (size_t) (top + y) * (size_t) stride + (size_t) left;
        for (int x = 0; x < (int) PSP_UI_MEDIA_TRACK_MENU_WIDTH; x++)
            destination[x] = ui_media_widen(source[x]);
    }
}
