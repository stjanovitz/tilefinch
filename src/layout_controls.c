/* Native form-control classification and bounded retained painting.
   Text editing and activation remain controller/runtime responsibilities. */

#include "layout_internal.h"
#include "tilefinch/media_discovery.h"

#include <math.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

bool layout_control_text_style(LayoutContext *context, lxb_dom_node_t *node,
    const ComputedStyle *style, bool placeholder, DrawCommand *text)
{
    ComputedStyle resolved;
    if (placeholder) {
        resolved = style_for_pseudo(context->sheet, node, PSEUDO_PLACEHOLDER, style);
        style = &resolved;
        if (style->opacity == 0 || style->visibility_hidden) return false;
    }
    text->color = style->color;
    text->scale = style->font_scale;
    text->font_size = style->font_size;
    draw_command_set_text_font_family(text, style->font_family);
    text->font_weight = draw_font_weight_code_for_style(style);
    text->font_italic = style->font_italic;
    draw_command_set_text_letter_spacing_fixed(text, style->letter_spacing_fixed);
    text->radius |= (int) style->font_size_fraction << LAYOUT_TEXT_FONT_SIZE_FRACTION_SHIFT
        | (computed_style_kerning_none(style) ? LAYOUT_TEXT_KERNING_NONE : 0);
    text->image_fit = text_decoration_bits(style);
    text->opacity_scale = alpha_opacity_scale((unsigned) style->color_alpha
        * (placeholder ? style->opacity : 255u) / 255u);
    return true;
}

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
    if (document_node_is_tag(node, LXB_TAG_TEXTAREA)) return 220;
    if (!document_node_is_tag(node, LXB_TAG_INPUT)) return 0;
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

int layout_textarea_rows_content_height(
    LayoutContext *context, lxb_dom_node_t *node, const ComputedStyle *style)
{
    if (context == NULL || style == NULL
        || !document_node_is_tag(node, LXB_TAG_TEXTAREA)) return 0;
    int rows = control_ascii_attribute_integer(node, "rows");
    if (rows <= 0) return 0;
    if (rows > 64) rows = 64;
    int line = layout_fixed_ceil(
        layout_inline_style_line_height_fixed(context, style));
    if (line <= 0) return 0;
    return rows * line;
}

int layout_control_default_height(lxb_dom_node_t *node)
{
    if (document_node_is_tag(node, LXB_TAG_TEXTAREA)) return 64;
    if (!document_node_is_tag(node, LXB_TAG_INPUT)) return 0;
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
        || !document_node_is_tag(node, LXB_TAG_SELECT) || width < 12 || height < 8
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

bool layout_paint_video_control(
    LayoutContext *context, lxb_dom_node_t *node,
    int x, int y, int width, int height)
{
    if (context == NULL) return false;
    if (width < 32 || height < 24) return true;
    int button = width < height ? width : height;
    button /= 3;
    if (button < 24) button = 24;
    if (button > 42) button = 42;
    int button_x = x + (width - button) / 2;
    int button_y = y + (height - button) / 2;
    DrawCommand face = {
        .type = DRAW_FILL_RECT, .x = button_x, .y = button_y,
        .width = button, .height = button,
        .color = 0x162431, .radius = button / 2,
        .opacity_scale = 224
    };
    if (layout_add_command(context->layout, face) == NULL) return false;
    int triangle_height = button / 2;
    int triangle_x = button_x + button / 2 - 2;
    int triangle_y = button_y + (button - triangle_height) / 2;
    for (int row = 0; row < triangle_height; row++) {
        int half = row < triangle_height / 2
            ? row : triangle_height - row - 1;
        DrawCommand mark = {
            .type = DRAW_FILL_RECT,
            .x = triangle_x, .y = triangle_y + row,
            .width = 2 + half, .height = 1,
            .color = 0xffffff, .opacity_scale = 256
        };
        if (layout_add_command(context->layout, mark) == NULL) return false;
    }
    bool engine_card = document_is_declared_video_card(
        context->document, node);
    size_t title_length = 0u, duration_length = 0u;
    const char *title = NULL;
    const char *duration = NULL;
    if (engine_card) {
        title = document_attribute(node, "title", &title_length);
        duration = document_attribute(
            node, "data-tilefinch-media-duration", &duration_length);
    } else if (node != NULL && !context->declared_video_scanned) {
        context->declared_video_scanned = true;
        const MediaDeclaredVideo *declared =
            media_declared_video_cached(context->document);
        if (declared != NULL) {
            size_t length = strlen(declared->title);
            context->declared_video_title = layout_retain_generated_text(
                context->layout, declared->title, length);
            if (context->declared_video_title != NULL)
                context->declared_video_title_length = length;
            length = strlen(declared->duration);
            context->declared_video_duration = layout_retain_generated_text(
                context->layout, declared->duration, length);
            if (context->declared_video_duration != NULL)
                context->declared_video_duration_length = length;
        }
    }
    if (!engine_card) {
        title = context->declared_video_title;
        title_length = context->declared_video_title_length;
        duration = context->declared_video_duration;
        duration_length = context->declared_video_duration_length;
    }
    if ((title_length != 0u || duration_length != 0u) && height >= 52) {
        DrawCommand bar = {
            .type = DRAW_FILL_RECT, .x = x, .y = y + height - 28,
            .width = width, .height = 28, .color = 0x162431,
            .opacity_scale = 232
        };
        if (layout_add_command(context->layout, bar) == NULL) return false;
        if (title != NULL && title_length != 0u) {
            if (title_length > MEDIA_DECLARED_VIDEO_TITLE_CAPACITY - 1u)
                title_length = MEDIA_DECLARED_VIDEO_TITLE_CAPACITY - 1u;
            DrawCommand label = {
                .type = DRAW_TEXT, .text = title,
                .text_length = (uint32_t) title_length,
                .x = x + 8, .y = y + height - 22,
                .width = width > 80 ? width - 80 : width - 16,
                .height = 16, .color = 0xffffff,
                .font_size = 12, .font_family = FONT_SANS,
                .font_weight = 70, .opacity_scale = 256
            };
            if (engine_card) label.radius |= LAYOUT_TEXT_FIND_EXCLUDED;
            if (layout_add_command(context->layout, label) == NULL)
                return false;
        }
        if (duration != NULL && duration_length != 0u) {
            if (duration_length > MEDIA_DECLARED_VIDEO_DURATION_CAPACITY - 1u)
                duration_length = MEDIA_DECLARED_VIDEO_DURATION_CAPACITY - 1u;
            DrawCommand time = {
                .type = DRAW_TEXT, .text = duration,
                .text_length = (uint32_t) duration_length,
                .x = x + (width > 68 ? width - 64 : 4),
                .y = y + height - 22, .width = 60, .height = 16,
                .color = 0xdce8f3, .font_size = 11,
                .font_family = FONT_SANS, .opacity_scale = 256
            };
            if (engine_card) time.radius |= LAYOUT_TEXT_FIND_EXCLUDED;
            if (layout_add_command(context->layout, time) == NULL)
                return false;
        }
    }
    return true;
}


/* The text a text-entry control paints, for both the block and the inline
   control arm: the live edited value, else the authored value (input) or
   first text (textarea, editing host), else the placeholder. Toggles and
   ranges paint none, a push button without a value attribute paints its UA
   label, and a
   password value -- never the author's placeholder -- is masked. */
const char *layout_control_value_text(lxb_dom_node_t *node, bool input,
                                      ControlType input_type,
                                      size_t *length, bool *placeholder)
{
    size_t value_length = 0;
    bool placeholder_value = false;
    size_t type_length = 0;
    const char *type = input
        ? document_attribute(node, "type", &type_length) : NULL;
    /* A file input's value is the chosen file, never its label, and no file
       can be chosen here: it always paints its UA label. */
    bool file_input = type != NULL && type_length == 4
        && strncasecmp(type, "file", 4) == 0;
    const char *value = file_input
        ? NULL : document_control_value(node, &value_length);
    if (value == NULL && !file_input) {
        value = input
            ? document_attribute(node, "value", &value_length)
            : first_text_data(node, &value_length);
    }
    /* A push button's label is its value attribute when present, even an
       empty one (HTML 4.10.5.1.18-20); only a missing one takes the UA
       label. */
    bool authored_value = value != NULL;
    if (input_type == CONTROL_TOGGLE || input_type == CONTROL_RANGE) {
        value = NULL;
        value_length = 0;
    } else if ((value == NULL || value_length == 0) && !file_input) {
        value = document_attribute(node, "placeholder", &value_length);
        placeholder_value = value != NULL && value_length != 0;
    }
    if (!authored_value && input_type == CONTROL_BUTTON && type != NULL) {
        if (file_input) {
            value = "CHOOSE FILE";
            value_length = 11;
        } else if ((type_length == 6 && strncasecmp(type, "submit", 6) == 0)
                   || (type_length == 5
                       && strncasecmp(type, "image", 5) == 0)) {
            value = "SUBMIT";
            value_length = 6;
        } else if (type_length == 5 && strncasecmp(type, "reset", 5) == 0) {
            value = "RESET";
            value_length = 5;
        }
    }
    static const char password_mask[] = "********************************";
    if (!placeholder_value && type != NULL && type_length == 8
        && strncasecmp(type, "password", 8) == 0
        && value != NULL && value_length != 0) {
        if (value_length > sizeof(password_mask) - 1) {
            value_length = sizeof(password_mask) - 1;
        }
        value = password_mask;
    }
    if (value == NULL) value_length = 0;
    *length = value_length;
    *placeholder = placeholder_value;
    return value;
}

/* A resizable textarea's 12px CONTROL_RESIZE hit region and the three-line
   grip drawn in the bottom-right corner of its control box, as browsers
   paint one whatever the textarea's display type. */
bool layout_control_add_resize_grip(LayoutContext *context,
                                    lxb_dom_node_t *node,
                                    const ComputedStyle *style,
                                    int x, int y, int width, int height)
{
    int handle = 12;
    if (!layout_add_control(
            context->layout,
            x + (width > handle ? width - handle : 0),
            y + (height > handle ? height - handle : 0),
            width < handle ? width : handle,
            height < handle ? height : handle,
            CONTROL_RESIZE, node)) return false;
    for (int line = 0; line < 3; line++) {
        int size = 2 + line * 3;
        DrawCommand grip = {
            .type = DRAW_FILL_RECT,
            .x = x + width - size - 2,
            .y = y + height - 2 - line * 3,
            .width = size,
            .height = 1,
            .color = style->color,
            .opacity_scale = alpha_opacity_scale(style->color_alpha)
        };
        if (grip.width > 0
            && layout_add_command(context->layout, grip) == NULL) {
            return false;
        }
    }
    return true;
}

/* The block-level form-control arm of layout_block_impl: value text, native
   control faces, control regions and the textarea resize grip.  Out of line
   so that an ordinary block neither runs it nor reserves its locals in the
   recursive layout_block_impl frame; the caller admits only the elements it
   can act on (layout_block_may_paint_control). */
__attribute__((noinline))
bool layout_block_control_paint(LayoutContext *context, lxb_dom_node_t *node,
                                const ComputedStyle *style, int outer_x,
                                int outer_y, int outer_width,
                                int border_height, int content_bottom)
{
    bool audio_controls = layout_audio_has_controls(node);
    size_t block_editable_length = 0;
    const char *block_editable = document_attribute(
        node, "contenteditable", &block_editable_length);
    bool block_editable_control = lxb_dom_element_has_attribute(
        lxb_dom_interface_element(node),
        (const lxb_char_t *) "contenteditable", 15)
        && !(block_editable != NULL && block_editable_length == 5
             && strncasecmp(block_editable, "false", 5) == 0);
    bool block_input = document_node_is_tag(node, LXB_TAG_INPUT);
    bool block_textarea = document_node_is_tag(node, LXB_TAG_TEXTAREA);
    bool block_button = document_node_is_tag(node, LXB_TAG_BUTTON);
    bool block_select = document_node_is_tag(node, LXB_TAG_SELECT);
    bool block_label = document_node_is_tag(node, LXB_TAG_LABEL);
    bool block_summary = document_node_is_tag(node, LXB_TAG_SUMMARY);
    bool block_video = document_node_is_tag(node, LXB_TAG_VIDEO);
    bool block_frame = document_node_is_tag(node, LXB_TAG_IFRAME);
    bool block_audio = audio_controls;
    bool block_declared_media_card = document_is_declared_video_card(
        context->document, node);
    ControlType block_input_type = block_input
        ? layout_input_control_type(node) : CONTROL_INPUT;
    if ((block_input || block_textarea)
        && block_input_type != CONTROL_TOGGLE
        && block_input_type != CONTROL_RANGE) {
        size_t value_length = 0;
        bool placeholder_value = false;
        const char *value = layout_control_value_text(
            node, block_input, block_input_type, &value_length,
            &placeholder_value);
        if (value != NULL && value_length != 0) {
            if (value_length > UINT32_MAX) return false;
            int text_height = computed_style_font_size_fixed(style);
            text_height = text_height > 0
                ? (text_height + 63) / 64 : 7 * style->font_scale;
            /* A textarea's first line is a line box: its text is placed in
               the full line-height like ordinary text, not in a font-height
               box at the top (which drew it above centre). */
            if (block_textarea) {
                int line_height = layout_fixed_ceil(
                    layout_inline_style_line_height_fixed(context, style));
                if (line_height > text_height) text_height = line_height;
            }
            int text_width = outer_width - style->border.left
                             - style->border.right - style->padding.left
                             - style->padding.right;
            if (text_width < 0) text_width = 0;
            int text_indent = 0;
            if (!style_length_resolve(
                    context->sheet, style->text_indent,
                    text_width, &text_indent)) {
                text_indent = 0;
            }
            DrawCommand text = {
                .type = DRAW_TEXT,
                .x = layout_add_coordinate(
                    outer_x + style->border.left + style->padding.left,
                    text_indent),
                .y = block_textarea
                     ? outer_y + style->border.top + style->padding.top
                     : outer_y + (border_height - text_height) / 2,
                .width = text_width,
                .height = text_height,
                .text = value,
                .text_length = (uint32_t) value_length,
            };
            if (text.width > 0
                && layout_control_text_style(context, node, style, placeholder_value, &text)
                && (!layout_add_text_shadow_commands(
                        context, style, &text)
                    || layout_add_command(
                           context->layout, text) == NULL)) {
                return false;
            }
        }
    }
    if (block_input
        && (block_input_type == CONTROL_TOGGLE
            || block_input_type == CONTROL_RANGE)) {
        bool special_input = false;
        if (!layout_paint_special_input(
                context, node, style, outer_x, outer_y,
                outer_width, border_height, &special_input)) {
            return false;
        }
    }
    if (block_select
        && !layout_paint_select_indicator(
            context, node, style, outer_x, outer_y,
            outer_width, border_height)) {
        return false;
    }
    if (block_audio
        && !layout_paint_audio_control(
            context, style, outer_x, outer_y,
            outer_width, border_height)) return false;
    if ((block_video || block_declared_media_card)
        && !layout_paint_video_control(
            context, node, outer_x, outer_y, outer_width, border_height)) {
        return false;
    }
    if (block_input || block_textarea || block_button || block_select
        || block_label || block_summary || block_video || block_audio
        || block_declared_media_card || block_frame) {
        ControlType type = block_input ? block_input_type
                           : (block_textarea ? CONTROL_TEXTAREA
                              : (block_button || block_label || block_summary
                                 || block_video || block_audio
                                 || block_declared_media_card ? CONTROL_BUTTON
                                 : (block_frame ? CONTROL_FRAME
                                              : CONTROL_SELECT)));
        int minimum_control_height = block_summary ? 24
            : (block_video || block_audio || block_declared_media_card
               || block_frame)
                ? border_height : 30;
        int control_height = border_height < minimum_control_height
                             ? minimum_control_height : border_height;
        if (!layout_add_control(context->layout, outer_x, outer_y, outer_width,
                         control_height, type, node)) return false;
        if (block_textarea && style->resize_mode != STYLE_RESIZE_NONE) {
            if (!layout_control_add_resize_grip(
                    context, node, style, outer_x, outer_y,
                    outer_width, control_height)) return false;
        }
    }
    if (block_editable_control) {
        int control_height = content_bottom - outer_y;
        if (control_height < 32) control_height = 32;
        if (!layout_add_control(context->layout, outer_x, outer_y, outer_width,
                         control_height, CONTROL_EDITABLE, node)) {
            return false;
        }
    }
    return true;
}
