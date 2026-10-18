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
#include "tilefinch/font.h"

#include <stddef.h>
#include <string.h>

#include "psp_media_pixels.h"

_Static_assert(sizeof(PspUiMediaTrackMenuCache) == 12,
               "menu focus metadata must use existing cache padding");

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
    uint16_t *scratch, bool controls_only, bool without_track_menu,
    PspUiMediaTrackMenuBlit opaque_blit, void *blit_context,
    uint16_t *retained_title, bool title_changed)
{
    if (media == NULL || pixels == NULL || scratch == NULL
        || width <= 0 || height <= 0 || stride < width) return;
    bool clockwise = media->presentation != NULL
        && media->presentation->clockwise;
    int physical_width = width;
    int physical_stride = stride;
    if (clockwise) {
        int swap = width;
        width = height;
        height = swap;
        stride = width;
    }
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
        if (retained_title != NULL && !title_changed && bounds->top == 0
            && bounds->bottom == PSP_UI_MEDIA_TITLE_BAR_HEIGHT
            && bounds->left == 0 && bounds->right == width
            && !bounds->needs_backdrop) continue;
        for (int y = bounds->top; y < bounds->bottom; y++) {
            const uint32_t *source = clockwise
                ? pixels + (size_t) bounds->left * physical_stride
                    + physical_width - 1 - y
                : pixels + (size_t) y * (size_t) stride + bounds->left;
            uint16_t *destination = scratch + (size_t) y * (size_t) stride;
            size_t pixels_wide = (size_t) (bounds->right - bounds->left);
            if (!bounds->needs_backdrop) {
                memset(
                    destination + bounds->left, 0,
                    pixels_wide * sizeof(*destination));
            } else {
                int source_step = clockwise ? physical_stride : 1;
                for (int x = bounds->left; x < bounds->right; x++) {
                    destination[x] = ui_media_narrow(*source);
                    /* Do not form an out-of-allocation pointer after the
                       last logical column in the rotated view. */
                    if (x + 1 < bounds->right) source += source_step;
                }
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
    } else if (retained_title != NULL && !title_changed) {
        psp_ui_media_composite_layers(
            media, preview, scratch, width, height, stride, true, false);
    } else {
        psp_ui_media_composite_with_preview(
            media, preview, scratch, width, height, stride);
    }
    for (size_t region = 0; region < count; region++) {
        const PspUiOverlayRegion *bounds = &regions[region];
        bool title = retained_title != NULL && bounds->top == 0
            && bounds->bottom == PSP_UI_MEDIA_TITLE_BAR_HEIGHT
            && bounds->left == 0 && bounds->right == width
            && !bounds->needs_backdrop;
        if (title && title_changed) {
            for (unsigned y = 0; y < PSP_UI_MEDIA_TITLE_BAR_HEIGHT; y++)
                memcpy(retained_title + (size_t) y * width,
                       scratch + (size_t) y * stride,
                       (size_t) width * sizeof(*retained_title));
        }
        const uint16_t *raster = title ? retained_title : scratch;
        int raster_stride = title ? width : stride;
        /* Full-width opaque bars need no video backdrop. Their finished 565
           pixels can use the same synchronous GE expansion as the chooser,
           avoiding a CPU widening pass over most of the visible controls.
           Keep rotated/translucent regions and any refusal on the exact
           scalar path. Full rows also preserve texture alignment/padding. */
        if (!clockwise && !bounds->needs_backdrop
            && bounds->left == 0 && bounds->right == width
            && opaque_blit != NULL && opaque_blit(blit_context,
                raster + (size_t) bounds->top * raster_stride,
                width, bounds->bottom - bounds->top, raster_stride, true,
                pixels, stride, 0, bounds->top)) continue;
        for (int y = bounds->top; y < bounds->bottom; y++) {
            const uint16_t *source = raster + (size_t) y * (size_t) raster_stride;
            uint32_t *destination = clockwise
                ? pixels + (size_t) bounds->left * physical_stride
                    + physical_width - 1 - y
                : pixels + (size_t) y * (size_t) stride + bounds->left;
            int destination_step = clockwise ? physical_stride : 1;
            for (int x = bounds->left; x < bounds->right; x++) {
                *destination = ui_media_widen(source[x]);
                if (x + 1 < bounds->right) destination += destination_step;
            }
        }
    }
}

void psp_ui_media_composite_8888(
    const PspUiMediaState *media, const PspUiMediaPreview *preview,
    uint32_t *pixels, int width, int height, int stride,
    uint16_t *scratch)
{
    ui_media_composite_8888(
        media, preview, pixels, width, height, stride, scratch, false, false,
        NULL, NULL, NULL, false);
}

void psp_ui_media_composite_controls_8888(
    const PspUiMediaState *media, uint32_t *pixels,
    int width, int height, int stride, uint16_t *scratch)
{
    ui_media_composite_8888(
        media, NULL, pixels, width, height, stride, scratch, true, false,
        NULL, NULL, NULL, false);
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
    const PspUiMediaState *media)
{
    const PspUiMediaPresentation *presentation = media->presentation;
    uint32_t hash = UINT32_C(2166136261);
    /* A retained opaque menu is invalid when the process theme changes even
       if its track catalog is byte-identical. */
    const unsigned char *palette =
        (const unsigned char *) psp_ui_theme_active_palette;
    for (size_t at = 0; at < sizeof(*psp_ui_theme_active_palette); at++)
        hash = ui_media_menu_hash_byte(hash, palette[at]);
    if (presentation == NULL) return hash;
    hash = ui_media_menu_hash_byte(hash, presentation->track_menu_tab);
    hash = ui_media_menu_hash_byte(hash, presentation->audio_track_count);
    hash = ui_media_menu_hash_byte(hash, presentation->subtitle_track_count);
    hash = ui_media_menu_hash_byte(hash, presentation->chapter_count);
    hash = ui_media_menu_hash_byte(hash, presentation->queue_active);
    /* Counts/capabilities affect the tab bar, but only the active catalog's
       contents affect this opaque raster. Hidden feeds must not repaint it. */
    hash = ui_media_menu_hash_byte(hash, media->seek_enabled);
    hash = ui_media_menu_hash_byte(hash, media->live);
    hash = ui_media_menu_hash_byte(hash, media->duration_us != 0);
    uintptr_t face = (uintptr_t) presentation->title_font;
    for (unsigned byte = 0; byte < sizeof(face); byte++)
        hash = ui_media_menu_hash_byte(hash, (unsigned char) (face >> (byte * 8u)));
    if (presentation->track_menu_tab == 3 && presentation->queue_active) {
        hash = ui_media_menu_hash_byte(hash, presentation->queue_count);
        hash = ui_media_menu_hash_byte(hash, presentation->queue_loading);
        hash = ui_media_menu_hash_byte(hash, presentation->queue_failed);
        hash = ui_media_menu_hash_byte(hash, presentation->queue_retry);
        hash = ui_media_menu_hash_byte(hash, media->ended);
        if (presentation->queue_failed)
            hash = ui_media_menu_hash_string(hash, presentation->queue_error, sizeof(presentation->queue_error));
        hash = ui_media_menu_hash_byte(hash, presentation->queue_next);
        for (unsigned byte = 0; byte < 4; byte++)
            hash = ui_media_menu_hash_byte(hash, (unsigned char) (presentation->queue_position >> (byte * 8)));
        for (unsigned i = 0; i < presentation->queue_count && i < 6u; i++)
            hash = ui_media_menu_hash_string(hash, presentation->queue_titles[i], sizeof(presentation->queue_titles[i]));
        if (presentation->queue_count == 0)
            hash = ui_media_menu_hash_string(hash, media->title, sizeof(media->title));
    }
    unsigned current_chapter = 0;
    uint64_t time = media->seek_preview_active
        ? media->seek_preview_time_us : media->current_time_us;
    for (size_t at = 0; presentation->track_menu_tab == 2
         && at < presentation->chapter_count && at < MEDIA_CHAPTER_LIMIT; at++) {
        uint32_t seconds = presentation->chapters[at].seconds;
        if ((uint64_t) seconds * 1000000u <= time) current_chapter = (unsigned) at + 1u;
        for (unsigned byte = 0; byte < 4u; byte++)
            hash = ui_media_menu_hash_byte(hash, (unsigned char) (seconds >> (byte * 8u)));
        hash = ui_media_menu_hash_string(hash, presentation->chapters[at].title,
                                        sizeof(presentation->chapters[at].title));
    }
    if (presentation->track_menu_tab == 2)
        hash = ui_media_menu_hash_byte(hash, (unsigned char) current_chapter);
    if (presentation->track_menu_tab == 0)
        hash = ui_media_menu_hash_byte(hash, (unsigned char) (presentation->selected_audio_track + 1));
    if (presentation->track_menu_tab == 1)
        hash = ui_media_menu_hash_byte(hash, (unsigned char) (presentation->selected_subtitle_track + 1));
    size_t audio_count = presentation->audio_track_count;
    if (audio_count > PSP_UI_MEDIA_TRACK_LIMIT)
        audio_count = PSP_UI_MEDIA_TRACK_LIMIT;
    for (size_t at = 0; presentation->track_menu_tab == 0 && at < audio_count; at++) {
        hash = ui_media_menu_hash_string(
            hash, presentation->audio_tracks[at].label,
            sizeof(presentation->audio_tracks[at].label));
    }
    size_t subtitle_count = presentation->subtitle_track_count;
    if (subtitle_count > PSP_UI_MEDIA_TRACK_LIMIT)
        subtitle_count = PSP_UI_MEDIA_TRACK_LIMIT;
    for (size_t at = 0; presentation->track_menu_tab == 1 && at < subtitle_count; at++) {
        hash = ui_media_menu_hash_string(
            hash, presentation->subtitle_tracks[at].label,
            sizeof(presentation->subtitle_tracks[at].label));
    }
    return hash;
}

static uint32_t ui_media_title_signature(
    const PspUiMediaState *media, int width, uint32_t raster_generation)
{
    uint32_t hash = UINT32_C(2166136261);
    const unsigned char *palette =
        (const unsigned char *) psp_ui_theme_active_palette;
    for (size_t at = 0; at < sizeof(*psp_ui_theme_active_palette); at++)
        hash = ui_media_menu_hash_byte(hash, palette[at]);
    uintptr_t face = media->presentation == NULL
        ? 0 : (uintptr_t) media->presentation->title_font;
    for (unsigned byte = 0; byte < sizeof(face); byte++)
        hash = ui_media_menu_hash_byte(hash, (unsigned char) (face >> (byte * 8u)));
    uint64_t generation = font_generation();
    for (unsigned byte = 0; byte < sizeof(generation); byte++)
        hash = ui_media_menu_hash_byte(hash, (unsigned char) (generation >> (byte * 8u)));
    for (unsigned byte = 0; byte < sizeof(width); byte++)
        hash = ui_media_menu_hash_byte(hash, (unsigned char) ((unsigned) width >> (byte * 8u)));
    for (unsigned byte = 0; byte < sizeof(raster_generation); byte++)
        hash = ui_media_menu_hash_byte(hash, (unsigned char) (raster_generation >> (byte * 8u)));
    return ui_media_menu_hash_string(hash, media->title, sizeof(media->title));
}

uint32_t psp_ui_media_menu_feedback_key(const PspUiMediaState *media)
{
    const PspUiMediaPresentation *p = media == NULL ? NULL : media->presentation;
    if (p == NULL || !media->visible || p->clockwise || media->failed
        || media->resolving || media->seek_in_progress || media->seek_preview_active)
        return 0;
    if (!p->track_menu_open) return 1u;
    if (!media->controls_visible) return 0;
    return ((uint32_t) psp_ui_media_track_menu_height(media) << 16)
        | ((uint32_t) p->track_menu_tab << 10)
        | ((uint32_t) p->track_menu_selection << 2)
        | ((uint32_t) p->automatic_next << 1) | 1u;
}

bool psp_ui_media_menu_feedback_reuse(uint32_t previous, uint32_t current)
{
    return previous != 0 && current != previous && (current >> 16) != 0
        && (current >> 16) >= (previous >> 16);
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
    /* The menu and normal title never coexist. Reuse the reserved opaque
       menu raster for the title, without allocating another surface. The
       impossible menu selection UINT8_MAX tags this use of the shared bytes. */
    if (media != NULL && media->visible && media->controls_visible
        && (presentation == NULL
            || (!presentation->clockwise && !presentation->track_menu_open))
        && !media->failed && !media->resolving && !media->seek_in_progress
        && !media->seek_preview_active && pixels != NULL && scratch != NULL
        && width > 0 && height >= 272 && stride >= width
        && menu_pixels != NULL && menu_cache != NULL
        && menu_pixel_capacity / (size_t) width >= PSP_UI_MEDIA_TITLE_BAR_HEIGHT) {
        uint32_t raster_generation = psp_ui_media_raster_generation();
        uint32_t signature = ui_media_title_signature(media, width, raster_generation);
        bool changed = raster_generation == 0 || !menu_cache->valid
            || menu_cache->selection != UINT8_MAX
            || menu_cache->signature != signature
            || menu_cache->edram_epoch != edram_epoch;
        ui_media_composite_8888(
            media, preview, pixels, width, height, stride, scratch, false, false,
            menu_blit, menu_blit_context, menu_pixels, changed);
        if (changed) {
            menu_cache->valid = psp_ui_media_raster_generation() != 0;
            menu_cache->signature = signature;
            menu_cache->edram_epoch = edram_epoch;
            menu_cache->selection = UINT8_MAX;
        }
        return;
    }
    int menu_height = (int) psp_ui_media_track_menu_height(media);
    int top = 48 - (menu_height - (int) PSP_UI_MEDIA_TRACK_MENU_HEIGHT) / 2;
    size_t required = (size_t) PSP_UI_MEDIA_TRACK_MENU_WIDTH * (size_t) menu_height;
    if (presentation == NULL || presentation->clockwise
        || !presentation->track_menu_open
        || menu_pixels == NULL || menu_pixel_capacity < required
        || menu_cache == NULL
        || width < (int) PSP_UI_MEDIA_TRACK_MENU_WIDTH
        || height < top + menu_height) {
        ui_media_composite_8888(
            media, preview, pixels, width, height, stride, scratch, false, false,
            menu_blit, menu_blit_context, NULL, false);
        return;
    }

    /* Paint the moving controls first, excluding the opaque menu rectangle.
       The retained menu is copied last, preserving the ordinary draw order. */
    ui_media_composite_8888(
        media, preview, pixels, width, height, stride,
        scratch, false, true, menu_blit, menu_blit_context, NULL, false);
    uint32_t signature = ui_media_menu_signature(media);
    bool content_changed = !menu_cache->valid
        || menu_cache->selection == UINT8_MAX
        || menu_cache->signature != signature
        || menu_cache->edram_epoch != edram_epoch;
    bool source_changed = content_changed
        || menu_cache->selection != presentation->track_menu_selection
        || (presentation->track_menu_tab == 3
            && menu_cache->automatic_next != presentation->automatic_next);
    if (source_changed) {
        /* The painter fills the entire opaque rectangle before any ink;
           clearing it first was a redundant 124–143 KiB memory pass. */
        if (content_changed || !psp_ui_media_raster_menu_update(media,
                menu_cache->selection, menu_cache->automatic_next,
                menu_pixels, (int) PSP_UI_MEDIA_TRACK_MENU_WIDTH,
                menu_height, (int) PSP_UI_MEDIA_TRACK_MENU_WIDTH))
            psp_ui_media_raster_track_menu(
                media, menu_pixels,
                (int) PSP_UI_MEDIA_TRACK_MENU_WIDTH,
                menu_height,
                (int) PSP_UI_MEDIA_TRACK_MENU_WIDTH);
        menu_cache->signature = signature;
        menu_cache->edram_epoch = edram_epoch;
        menu_cache->valid = true;
        menu_cache->selection = presentation->track_menu_selection;
        menu_cache->automatic_next = presentation->automatic_next;
    }
    int left = width / 2 - (int) PSP_UI_MEDIA_TRACK_MENU_WIDTH / 2;
    if (menu_blit != NULL && menu_blit(
            menu_blit_context, menu_pixels,
            (int) PSP_UI_MEDIA_TRACK_MENU_WIDTH,
            menu_height,
            (int) PSP_UI_MEDIA_TRACK_MENU_WIDTH, source_changed,
            pixels, stride, left, top)) {
        return;
    }
    for (int y = 0; y < menu_height; y++) {
        const uint16_t *source = menu_pixels
            + (size_t) y * PSP_UI_MEDIA_TRACK_MENU_WIDTH;
        uint32_t *destination = pixels
            + (size_t) (top + y) * (size_t) stride + (size_t) left;
        for (int x = 0; x < (int) PSP_UI_MEDIA_TRACK_MENU_WIDTH; x++)
            destination[x] = ui_media_widen(source[x]);
    }
}
