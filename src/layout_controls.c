/* Native form-control classification and bounded retained painting.
   Text editing and activation remain controller/runtime responsibilities. */

#include "layout_internal.h"

#include <math.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

ControlType layout_input_control_type(lxb_dom_node_t *node)
{
    if (attribute_is(node, "type", "checkbox")
        || attribute_is(node, "type", "radio")) {
        return CONTROL_TOGGLE;
    }
    if (attribute_is(node, "type", "range")) return CONTROL_RANGE;
    if (attribute_is(node, "type", "button")
        || attribute_is(node, "type", "submit")
        || attribute_is(node, "type", "reset")
        || attribute_is(node, "type", "image")
        || attribute_is(node, "type", "file")) {
        return CONTROL_BUTTON;
    }
    return CONTROL_INPUT;
}

static int control_ascii_attribute_integer(lxb_dom_node_t *node,
                                           const char *name)
{
    size_t length = 0;
    const char *text = document_attribute(node, name, &length);
    if (text == NULL || length == 0 || length > 5u) return 0;
    int value = 0;
    for (size_t i = 0; i < length; i++) {
        if (text[i] < '0' || text[i] > '9') return 0;
        unsigned digit = (unsigned) (text[i] - '0');
        if (value > (INT_MAX - (int) digit) / 10) return 0;
        value = value * 10 + (int) digit;
    }
    return value;
}

static int control_button_label_width(lxb_dom_node_t *node)
{
    size_t length = 0;
    const char *value = document_attribute(node, "value", &length);
    if (value == NULL || length == 0 || length > 64u) return 96;
    /* Native HTML controls size their anonymous label, not an arbitrary
       fixed box. This bounded approximation tracks the PSP's 13px control
       face closely and avoids stretching short submit labels. */
    size_t characters = 0;
    for (size_t at = 0; at < length && characters < 64u; characters++) {
        unsigned char lead = (unsigned char) value[at++];
        if ((lead & 0xe0u) == 0xc0u && at < length) at++;
        else if ((lead & 0xf0u) == 0xe0u && at + 1u < length) at += 2u;
        else if ((lead & 0xf8u) == 0xf0u && at + 2u < length) at += 3u;
    }
    int width = 16 + (int) characters * 7;
    return width < 32 ? 32 : (width > 480 ? 480 : width);
}

int layout_control_default_width(lxb_dom_node_t *node)
{
    if (layout_node_name_is(node, "textarea")) return 220;
    if (!layout_node_name_is(node, "input")) return 0;
    switch (layout_input_control_type(node)) {
    case CONTROL_TOGGLE: return 18;
    case CONTROL_RANGE: return 160;
    case CONTROL_BUTTON: return control_button_label_width(node);
    default: {
        int characters = control_ascii_attribute_integer(node, "size");
        if (characters <= 0) return 220;
        if (characters > 64) characters = 64;
        /* HTML size= is a character-cell hint. Keep it bounded and use the
           native control face's measured average advance plus chrome. */
        int width = characters * 7 + 12;
        return width < 32 ? 32 : (width > 480 ? 480 : width);
    }
    }
}

int layout_control_default_height(lxb_dom_node_t *node)
{
    if (layout_node_name_is(node, "textarea")) return 64;
    if (!layout_node_name_is(node, "input")) return 0;
    switch (layout_input_control_type(node)) {
    case CONTROL_TOGGLE: return 18;
    case CONTROL_RANGE: return 20;
    default: return 0;
    }
}

static bool input_numeric_text(const char *text, size_t length, double *value)
{
    if (text == NULL || length == 0 || length >= 48 || value == NULL) {
        return false;
    }
    char copy[48];
    memcpy(copy, text, length);
    copy[length] = '\0';
    char *end = NULL;
    double parsed = strtod(copy, &end);
    if (end == copy || *end != '\0' || !isfinite(parsed)) return false;
    *value = parsed;
    return true;
}

static bool input_numeric_attribute(lxb_dom_node_t *node, const char *name,
                                    double *value)
{
    size_t length = 0;
    const char *text = document_attribute(node, name, &length);
    return input_numeric_text(text, length, value);
}

bool layout_paint_special_input(
    LayoutContext *context, lxb_dom_node_t *node,
    const ComputedStyle *style, int x, int y, int width, int height,
    bool *handled)
{
    if (handled != NULL) *handled = false;
    if (context == NULL || node == NULL || style == NULL
        || width <= 0 || height <= 0) {
        return context != NULL && node != NULL && style != NULL;
    }
    ControlType type = layout_input_control_type(node);
    if (type != CONTROL_TOGGLE && type != CONTROL_RANGE) return true;
    if ((style->appearance & STYLE_APPEARANCE_MASK) == APPEARANCE_NONE) {
        return true;
    }
    if (handled != NULL) *handled = true;

    unsigned border = style->border_color != 0
        ? style->border_color : 0x5f6368;
    unsigned surface = style->has_background
        ? style->background : 0xf7f7f8;
    unsigned accent = 0x1a73e8;
    if (type == CONTROL_TOGGLE) {
        bool radio = attribute_is(node, "type", "radio");
        bool checked = lxb_dom_element_has_attribute(
            lxb_dom_interface_element(node),
            (const lxb_char_t *) "checked", 7);
        bool indeterminate = lxb_dom_element_has_attribute(
            lxb_dom_interface_element(node),
            (const lxb_char_t *) "data-tilefinch-indeterminate", 28);
        int side = width < height ? width : height;
        if (side < 4) side = 4;
        int left = x + (width - side) / 2;
        int top = y + (height - side) / 2;
        DrawCommand shell = {
            .type = DRAW_FILL_RECT, .x = left, .y = top,
            .width = side, .height = side,
            .color = radio ? surface
                           : (checked || indeterminate ? accent : surface),
            .radius = radio ? side / 2 : (side >= 12 ? 3 : 1),
            .opacity_scale = 256
        };
        if (layout_add_command(context->layout, shell) == NULL) return false;
        DrawCommand stroke = {
            .type = DRAW_STROKE_RECT, .x = left, .y = top,
            .width = side, .height = side,
            .color = checked || indeterminate ? accent : border,
            .scale = side >= 12 ? 2 : 1, .radius = shell.radius,
            .opacity_scale = 256, .image_fit = LAYOUT_STROKE_SOLID
        };
        if (layout_add_command(context->layout, stroke) == NULL) return false;
        if (checked || indeterminate) {
            int inset = side >= 14 ? 4 : 3;
            int inner = side - inset * 2;
            if (inner < 2) inner = 2;
            if (checked && !indeterminate && !radio && side >= 12) {
                static const int check_x[] = {4, 6, 8, 10, 12};
                static const int check_y[] = {8, 10, 10, 8, 6};
                for (size_t i = 0;
                     i < sizeof(check_x) / sizeof(check_x[0]); i++) {
                    DrawCommand pixel = {
                        .type = DRAW_FILL_RECT,
                        .x = left + check_x[i] * side / 18,
                        .y = top + check_y[i] * side / 18,
                        .width = side >= 16 ? 2 : 1,
                        .height = side >= 16 ? 2 : 1,
                        .color = 0xffffff,
                        .radius = 1,
                        .opacity_scale = 256
                    };
                    if (layout_add_command(context->layout, pixel) == NULL) {
                        return false;
                    }
                }
                return true;
            }
            DrawCommand mark = {
                .type = DRAW_FILL_RECT,
                .x = left + (side - inner) / 2,
                .y = top + (side - (indeterminate ? 2 : inner)) / 2,
                .width = inner,
                .height = indeterminate ? 2 : inner,
                .color = radio ? accent : 0xffffff,
                .radius = radio ? inner / 2 : 1,
                .opacity_scale = 256
            };
            if (layout_add_command(context->layout, mark) == NULL) return false;
        }
        return true;
    }

    int track_height = height >= 16 ? 4 : 2;
    int thumb = height >= 18 ? 14 : (height > 6 ? height - 4 : height);
    if (thumb > width) thumb = width;
    int track_x = x + thumb / 2;
    int track_width = width - thumb;
    if (track_width < 1) track_width = 1;
    int track_y = y + (height - track_height) / 2;
    DrawCommand track = {
        .type = DRAW_FILL_RECT, .x = track_x, .y = track_y,
        .width = track_width, .height = track_height,
        .color = 0xb6bbc2, .radius = track_height / 2,
        .opacity_scale = 256
    };
    if (layout_add_command(context->layout, track) == NULL) return false;
    double minimum = 0.0, maximum = 100.0, value = 50.0;
    (void) input_numeric_attribute(node, "min", &minimum);
    if (!input_numeric_attribute(node, "max", &maximum)
        || maximum < minimum) {
        maximum = minimum + 100.0;
    }
    size_t value_length = 0;
    const char *value_text = document_control_value(node, &value_length);
    if (value_text == NULL) {
        value_text = document_attribute(node, "value", &value_length);
    }
    if (!input_numeric_text(value_text, value_length, &value)) {
        value = minimum + (maximum - minimum) / 2.0;
    }
    if (value < minimum) value = minimum;
    if (value > maximum) value = maximum;
    double fraction = maximum > minimum
        ? (value - minimum) / (maximum - minimum) : 0.0;
    int filled_width = (int) (fraction * track_width + 0.5);
    if (filled_width > 0) {
        DrawCommand filled = track;
        filled.width = filled_width;
        filled.color = accent;
        if (layout_add_command(context->layout, filled) == NULL) return false;
    }
    int thumb_x = track_x + filled_width - thumb / 2;
    if (thumb_x < x) thumb_x = x;
    if (thumb_x + thumb > x + width) thumb_x = x + width - thumb;
    DrawCommand knob = {
        .type = DRAW_FILL_RECT, .x = thumb_x,
        .y = y + (height - thumb) / 2,
        .width = thumb, .height = thumb,
        .color = accent,
        .radius = thumb / 2, .opacity_scale = 256
    };
    return layout_add_command(context->layout, knob) != NULL;
}

bool layout_paint_select_indicator(
    LayoutContext *context, lxb_dom_node_t *node,
    const ComputedStyle *style, int x, int y, int width, int height)
{
    if (context == NULL || node == NULL || style == NULL
        || !layout_node_name_is(node, "select") || width < 12 || height < 8
        || (style->appearance & STYLE_APPEARANCE_MASK) == APPEARANCE_NONE) {
        return context != NULL && node != NULL && style != NULL;
    }
    int center_x = x + width - 10;
    int top = y + (height - 4) / 2;
    for (int row = 0; row < 4; row++) {
        DrawCommand triangle = {
            .type = DRAW_FILL_RECT,
            .x = center_x - 3 + row,
            .y = top + row,
            .width = 7 - row * 2,
            .height = 1,
            .color = style->color,
            .opacity_scale = alpha_opacity_scale(style->color_alpha)
        };
        if (layout_add_command(context->layout, triangle) == NULL) {
            return false;
        }
    }
    return true;
}

bool layout_paint_audio_control(
    LayoutContext *context, const ComputedStyle *style,
    int x, int y, int width, int height)
{
    if (context == NULL || style == NULL) return false;
    if (width < 44 || height < 24) return true;
    int button = height - 16;
    if (button > 30) button = 30;
    if (button < 16) button = 16;
    int button_x = x + 8;
    int button_y = y + (height - button) / 2;
    DrawCommand face = {
        .type = DRAW_FILL_RECT, .x = button_x, .y = button_y,
        .width = button, .height = button,
        .color = 0x343a40, .radius = button / 2, .opacity_scale = 256
    };
    if (layout_add_command(context->layout, face) == NULL) return false;
    int triangle_height = button / 2;
    if (triangle_height < 8) triangle_height = 8;
    int triangle_x = button_x + button / 2 - 2;
    int triangle_y = button_y + (button - triangle_height) / 2;
    for (int row = 0; row < triangle_height; row++) {
        int half = row < triangle_height / 2
            ? row : triangle_height - row - 1;
        DrawCommand mark = {
            .type = DRAW_FILL_RECT,
            .x = triangle_x,
            .y = triangle_y + row,
            .width = 2 + half,
            .height = 1,
            .color = 0xffffff,
            .opacity_scale = 256
        };
        if (layout_add_command(context->layout, mark) == NULL) return false;
    }
    int track_x = button_x + button + 10;
    int track_width = x + width - track_x - 10;
    if (track_width > 0) {
        DrawCommand track = {
            .type = DRAW_FILL_RECT,
            .x = track_x, .y = y + height / 2 - 1,
            .width = track_width, .height = 3,
            .color = 0xa3a8af, .radius = 1, .opacity_scale = 256
        };
        if (layout_add_command(context->layout, track) == NULL) return false;
    }
    return true;
}
