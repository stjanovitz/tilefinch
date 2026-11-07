#include "tilefinch/viewport.h"
#include "tilefinch/integer_math.h"

#include <stdint.h>
#include <string.h>

bool viewport_context_init(ViewportContext *viewport,
                           int css_width, int css_height,
                           int device_width, int device_height)
{
    if (viewport == NULL || css_width <= 0 || css_height <= 0
        || device_width <= 0 || device_height <= 0) return false;
    *viewport = (ViewportContext) {
        .css_width = css_width,
        .css_height = css_height,
        .device_width = device_width,
        .device_height = device_height,
        .scale_numerator = device_width,
        .scale_denominator = css_width
    };
    return true;
}

bool viewport_context_resolve(ViewportContext *viewport,
                              const PocDocument *document,
                              int device_width, int device_height,
                              int legacy_width)
{
    if (viewport == NULL || document == NULL || device_width <= 0
        || device_height <= 0 || legacy_width < device_width) return false;
    MobileViewport mobile = {0};
    if (!document_mobile_viewport(document, device_width, legacy_width,
                                  &mobile)) return false;
    uint64_t css_height = ((uint64_t) device_height
                           * (uint64_t) mobile.layout_width
                           + (uint64_t) device_width - 1u)
                          / (uint64_t) device_width;
    if (css_height == 0 || css_height > INT32_MAX
        || !viewport_context_init(viewport, mobile.layout_width,
                                  (int) css_height, device_width,
                                  device_height)) return false;
    viewport->declared = mobile.declared;
    viewport->device_width_declared = mobile.device_width;
    viewport->fixed_width_declared = mobile.fixed_width;
    viewport->scale_numerator = mobile.scale_numerator;
    viewport->scale_denominator = mobile.scale_denominator;
    return true;
}

bool viewport_context_is_scaled(const ViewportContext *viewport)
{
    return viewport != NULL && viewport->scale_numerator > 0
           && viewport->scale_denominator > 0
           && viewport->scale_numerator != viewport->scale_denominator;
}

int viewport_scale_floor(int value, int numerator, int denominator)
{
    if (numerator <= 0 || denominator <= 0) return value;
    return tilefinch_mul_div_floor_int(value, numerator, denominator);
}

int viewport_scale_ceil(int value, int numerator, int denominator)
{
    if (numerator <= 0 || denominator <= 0) return value;
    return tilefinch_mul_div_ceil_int(value, numerator, denominator);
}

int viewport_css_to_device(const ViewportContext *viewport, int value)
{
    if (viewport == NULL) return value;
    return viewport_scale_floor(value, viewport->scale_numerator,
                                viewport->scale_denominator);
}

int viewport_device_to_css(const ViewportContext *viewport, int value)
{
    if (viewport == NULL) return value;
    return viewport_scale_floor(value, viewport->scale_denominator,
                                viewport->scale_numerator);
}

void viewport_css_box_to_device(const ViewportContext *viewport,
                                int *position, int *length)
{
    if (viewport == NULL || position == NULL || length == NULL) return;
    int start = viewport_css_to_device(viewport, *position);
    int end = viewport_scale_ceil(*position + *length,
                                  viewport->scale_numerator,
                                  viewport->scale_denominator);
    *position = start;
    *length = end > start ? end - start : (*length > 0 ? 1 : 0);
}

int viewport_max_scroll_css(const ViewportContext *viewport,
                            int content_height)
{
    if (viewport == NULL || content_height <= viewport->css_height) return 0;
    return content_height - viewport->css_height;
}

const unsigned viewport_page_zoom_steps[VIEWPORT_PAGE_ZOOM_STEP_COUNT] = {
    50u, 67u, 75u, 80u, 90u, 100u, 110u, 125u, 150u
};

bool viewport_page_zoom_valid(unsigned percent)
{
    for (size_t i = 0; i < VIEWPORT_PAGE_ZOOM_STEP_COUNT; i++)
        if (viewport_page_zoom_steps[i] == percent) return true;
    return false;
}

unsigned viewport_page_zoom_step(unsigned percent, int direction)
{
    size_t index = 0;
    while (index < VIEWPORT_PAGE_ZOOM_STEP_COUNT
           && viewport_page_zoom_steps[index] != percent) index++;
    if (index == VIEWPORT_PAGE_ZOOM_STEP_COUNT) {
        index = 0;
        while (viewport_page_zoom_steps[index] != VIEWPORT_PAGE_ZOOM_DEFAULT)
            index++;
    }
    if (direction < 0)
        index = index == 0 ? VIEWPORT_PAGE_ZOOM_STEP_COUNT - 1u : index - 1u;
    else if (direction > 0)
        index = index + 1u == VIEWPORT_PAGE_ZOOM_STEP_COUNT ? 0u : index + 1u;
    return viewport_page_zoom_steps[index];
}

bool viewport_apply_page_zoom(ViewportContext *viewport, unsigned percent)
{
    if (viewport == NULL || !viewport_page_zoom_valid(percent)
        || viewport->device_width <= 0 || viewport->device_height <= 0
        || viewport->css_width <= 0) return false;
    if (percent == VIEWPORT_PAGE_ZOOM_DEFAULT) return true;
    int device_width = viewport->device_width;
    /* Rounded to the nearest pixel; the steps keep this at most twice the
       device width, no wider than the legacy desktop viewport. */
    int zoomed = (int) (((uint64_t) device_width * 100u + percent / 2u)
                        / percent);
    int width = viewport->css_width;
    int next = width;
    if (percent < VIEWPORT_PAGE_ZOOM_DEFAULT) {
        if (zoomed > width) next = zoomed;
    } else if (width == device_width && !viewport->fixed_width_declared) {
        next = zoomed;
    }
    if (next == width) return true;
    uint64_t css_height = ((uint64_t) viewport->device_height
                           * (uint64_t) next + (uint64_t) device_width - 1u)
                          / (uint64_t) device_width;
    if (css_height == 0 || css_height > INT32_MAX) return false;
    viewport->css_width = next;
    viewport->css_height = (int) css_height;
    viewport->scale_numerator = device_width;
    viewport->scale_denominator = next;
    return true;
}
