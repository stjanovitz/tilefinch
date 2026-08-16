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
