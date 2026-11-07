#ifndef TILEFINCH_VIEWPORT_H
#define TILEFINCH_VIEWPORT_H

#include <stdbool.h>

#include "tilefinch/document.h"

typedef struct {
    int css_width;
    int css_height;
    int device_width;
    int device_height;
    int scale_numerator;
    int scale_denominator;
    bool declared;
    bool device_width_declared;
    /* An explicit numeric meta width, distinct from device-width or a
       viewport derived from initial-scale, even when numerically equal. */
    bool fixed_width_declared;
} ViewportContext;

bool viewport_context_init(ViewportContext *viewport,
                           int css_width, int css_height,
                           int device_width, int device_height);
bool viewport_context_resolve(ViewportContext *viewport,
                              const PocDocument *document,
                              int device_width, int device_height,
                              int legacy_width);
bool viewport_context_is_scaled(const ViewportContext *viewport);
int viewport_scale_floor(int value, int numerator, int denominator);
int viewport_scale_ceil(int value, int numerator, int denominator);
int viewport_css_to_device(const ViewportContext *viewport, int value);
int viewport_device_to_css(const ViewportContext *viewport, int value);
void viewport_css_box_to_device(const ViewportContext *viewport,
                                int *position, int *length);
int viewport_max_scroll_css(const ViewportContext *viewport,
                            int content_height);

/* Page zoom: the user's choice of how large CSS pixels are drawn, as a
   percentage. 100 leaves the resolved viewport alone. */
#define VIEWPORT_PAGE_ZOOM_DEFAULT 100u
#define VIEWPORT_PAGE_ZOOM_STEP_COUNT 9u
extern const unsigned viewport_page_zoom_steps[VIEWPORT_PAGE_ZOOM_STEP_COUNT];
bool viewport_page_zoom_valid(unsigned percent);
/* The next step in `direction` (-1 or 1), wrapping at either end; an
   invalid `percent` steps from the default. */
unsigned viewport_page_zoom_step(unsigned percent, int direction);
/* Applies page zoom to a viewport resolved from the page's own metadata.
   The zoomed screen is device_width * 100 / percent CSS pixels wide.
   - Below 100% the layout widens to the zoomed screen width unless the
     page already lays out at least that wide, so a wide declared viewport
     or the legacy desktop width is never shrunk a second time.
   - Above 100% only a page that follows the device width narrows to the
     zoomed screen; a page with its own fixed width keeps fitting the
     screen, which has no sideways scrolling.
   The page is always drawn at device_width / css_width. False for an
   invalid percent or viewport, which is left unchanged. */
bool viewport_apply_page_zoom(ViewportContext *viewport, unsigned percent);

#endif
