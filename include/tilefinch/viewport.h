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

#endif
