/* Per-node style resolution: UA defaults, presentational attributes,
   cascade application over matched rules, font-size resolution, and the
   public style_for_node / style_for_pseudo entry points.
   Split out of style.c. */

#include "style_budget_category.h"
#include "style_internal.h"
#include "style_cache_internal.h"
#include "tilefinch_test_faults.h"
#include "tilefinch/platform.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* Exact checked common path for bounded style arithmetic. Authored values can
   still reach the wide fallback, but ordinary font and geometry values stay
   on Allegrex's hardware 32-bit divider. */
static int style_multiply_add_divide(int value, int multiplier, int addition,
                                     int denominator)
{
    if (denominator <= 0) return 0;
    int product = 0;
    int adjusted = 0;
#if defined(__GNUC__) || defined(__clang__)
    if (!__builtin_mul_overflow(value, multiplier, &product)
        && !__builtin_add_overflow(product, addition, &adjusted)) {
        return adjusted / denominator;
    }
#endif
    int64_t wide = (int64_t) value * multiplier + addition;
    wide /= denominator;
    if (wide > INT_MAX) return INT_MAX;
    if (wide < INT_MIN) return INT_MIN;
    return (int) wide;
}

static bool presentational_nonnegative_pixels(lxb_dom_node_t *node,
                                              const char *name,
                                              int *pixels)
{
    if (node == NULL || name == NULL || pixels == NULL) return false;
    size_t length = 0;
    const char *value = document_attribute(node, name, &length);
    if (value == NULL || length == 0 || length >= 32) return false;
    char text[32];
    memcpy(text, value, length);
    text[length] = '\0';
    char *end = NULL;
    long parsed = strtol(text, &end, 10);
    while (end != NULL && isspace((unsigned char) *end)) end++;
    if (end == text || end == NULL || *end != '\0' || parsed < 0) {
        return false;
    }
    if (parsed > 256) parsed = 256;
    *pixels = (int) parsed;
    return true;
}

static bool style_element_name_is(const char *name, size_t length,
                                  const char *wanted);

static bool style_input_is_push_button(lxb_dom_node_t *node)
{
    size_t length = 0;
    const char *type = document_attribute(node, "type", &length);
    return type != NULL
        && ((length == 6 && strncasecmp(type, "submit", 6) == 0)
            || (length == 6 && strncasecmp(type, "button", 6) == 0)
            || (length == 5 && strncasecmp(type, "reset", 5) == 0));
}

static bool style_input_has_special_appearance(lxb_dom_node_t *node)
{
    size_t length = 0;
    const char *value = document_attribute(node, "type", &length);
    return value != NULL
           && ((length == 8 && strncasecmp(value, "checkbox", 8) == 0)
               || (length == 5 && strncasecmp(value, "radio", 5) == 0)
               || (length == 5 && strncasecmp(value, "range", 5) == 0));
}

static bool retained_modern_value(const Stylesheet *sheet,
                                  lxb_dom_node_t *node,
                                  const char *name,
                                  char output[STYLE_RETAINED_VALUE_CAPACITY])
{
    return style_retained_property_value(
        sheet, node, name, strlen(name), output,
        STYLE_RETAINED_VALUE_CAPACITY);
}

static bool modern_keyword(const char *value, const char *keyword)
{
    return value != NULL && strcasecmp(value, keyword) == 0;
}

static bool modern_length_component(const char **cursor, int *value,
                                    bool *percent)
{
    while (isspace((unsigned char) **cursor)) (*cursor)++;
    char *end = NULL;
    double number = strtod(*cursor, &end);
    if (end == *cursor || !isfinite(number)) return false;
    bool percentage = *end == '%';
    if (percentage) end++;
    else if (strncasecmp(end, "px", 2) == 0) end += 2;
    else if (number != 0.0 && *end != '\0'
             && !isspace((unsigned char) *end)) return false;
    if (number < -32767.0) number = -32767.0;
    if (number > 32767.0) number = 32767.0;
    *value = (int) (number < 0 ? number - 0.5 : number + 0.5);
    *percent = percentage;
    *cursor = end;
    return true;
}

bool style_scale_value_q14(const char *value, uint16_t *scale_q14)
{
    if (modern_keyword(value, "none")) {
        *scale_q14 = 16384;
        return true;
    }
    char *end = NULL;
    double first = strtod(value, &end);
    if (end == value || !isfinite(first) || first < 0.0) return false;
    if (*end == '%') { first /= 100.0; end++; }
    while (isspace((unsigned char) *end)) end++;
    double second = first;
    if (*end != '\0') {
        char *second_end = NULL;
        second = strtod(end, &second_end);
        if (second_end == end || !isfinite(second) || second < 0.0) {
            return false;
        }
        if (*second_end == '%') { second /= 100.0; second_end++; }
        while (isspace((unsigned char) *second_end)) second_end++;
        if (*second_end != '\0') return false;
    }
    /* The retained display list has one exact scale. Reject non-uniform
       values rather than silently distorting one axis. */
    if (fabs(first - second) > 0.000001) return false;
    double fixed = first * (double) STYLE_TRANSFORM_SCALE_Q14_ONE;
    if (fixed > (double) STYLE_TRANSFORM_SCALE_Q14_MAX)
        fixed = (double) STYLE_TRANSFORM_SCALE_Q14_MAX;
    *scale_q14 = (uint16_t) (fixed + 0.5);
    return true;
}

static bool modern_translation_component(const Stylesheet *sheet,
    const char **cursor, int *value, bool *percent)
{
    const char *start = *cursor;
    while (isspace((unsigned char) *start)) start++;
    const char *end = start;
    unsigned depth = 0;
    while (*end != '\0' && (depth != 0 || !isspace((unsigned char) *end))) {
        if ((size_t) (end - start) >= STYLE_RETAINED_VALUE_CAPACITY) return false;
        if (*end == '(') {
            if (++depth > 16u) return false;
        } else if (*end == ')') {
            if (depth == 0) return false;
            depth--;
        }
        end++;
    }
    StyleLength length = 0;
    if (depth != 0 || end == start
        || !parse_dimension_length((Stylesheet *) sheet, start,
                                   (size_t) (end - start), &length, percent))
        return false;
    *value = length;
    *cursor = end;
    return true;
}

bool style_rotate_value_quadrants(const char *value, uint8_t *quadrants)
{
    if (modern_keyword(value, "none")) {
        *quadrants = 0;
        return true;
    }
    while (isspace((unsigned char) *value)) value++;
    if ((value[0] == 'z' || value[0] == 'Z')
        && isspace((unsigned char) value[1])) {
        value += 2;
    }
    double angle = 0.0;
    if (!style_parse_angle_degrees(value, strlen(value), true, &angle))
        return false;
    double quarter = angle / 90.0;
    long rounded = lround(quarter);
    /* Quarter turns are exact in the compact axis-aligned display list.
       Other angles remain unsupported rather than being approximated. */
    if (fabs(quarter - (double) rounded) > 0.000001) return false;
    *quadrants = (uint8_t) (((rounded % 4) + 4) % 4);
    return true;
}

static bool modern_radius_value(const char *value, int *radius)
{
    const char *cursor = value;
    bool percent = false;
    int parsed = 0;
    if (!modern_length_component(&cursor, &parsed, &percent) || percent) {
        return false;
    }
    while (isspace((unsigned char) *cursor)) cursor++;
    if (*cursor != '\0' || parsed < 0) return false;
    if (parsed > 127) parsed = 127;
    *radius = parsed;
    return true;
}

static unsigned modern_logical_corner(const ComputedStyle *style,
                                      bool block_start, bool inline_start)
{
    unsigned mode = computed_style_writing_mode(style);
    bool rtl = computed_style_direction_rtl(style);
    unsigned block_edge = mode == STYLE_WRITING_HORIZONTAL_TB
        ? (block_start ? 0u : 2u)
        : (mode == STYLE_WRITING_VERTICAL_RL
           ? (block_start ? 1u : 3u)
           : (block_start ? 3u : 1u));
    unsigned inline_edge = mode == STYLE_WRITING_HORIZONTAL_TB
        ? ((inline_start ^ rtl) ? 3u : 1u)
        : ((inline_start ^ rtl) ? 0u : 2u);
    /* Physical corners are TL, TR, BR, BL. */
    if ((block_edge == 0u && inline_edge == 3u)
        || (block_edge == 3u && inline_edge == 0u)) return 0u;
    if ((block_edge == 0u && inline_edge == 1u)
        || (block_edge == 1u && inline_edge == 0u)) return 1u;
    if ((block_edge == 2u && inline_edge == 1u)
        || (block_edge == 1u && inline_edge == 2u)) return 2u;
    return 3u;
}

static void style_apply_modern_properties(const Stylesheet *sheet,
                                          lxb_dom_node_t *node,
                                          const ComputedStyle *parent,
                                          ComputedStyle *style,
                                          bool root_element)
{
    if (sheet == NULL || node == NULL || style == NULL
        || sheet->modern_property_mask == 0) return;
    uint32_t mask = sheet->modern_property_mask;
    char value[STYLE_RETAINED_VALUE_CAPACITY];
    bool paint_dirty = false;
    StylePaintStack paint = {0};
    if ((mask & (STYLE_MODERN_MIX_BLEND
                 | STYLE_MODERN_BACKDROP_FILTER)) != 0) {
        paint = style_paint_stack_copy(sheet, style);
    }

    if ((mask & STYLE_MODERN_TYPOGRAPHY) != 0) {
        uint8_t typography = sheet->modern_typography_mask;
        if ((typography & STYLE_MODERN_TYPOGRAPHY_HYPHENS) != 0
            && retained_modern_value(sheet, node, "hyphens", value)) {
            if (modern_keyword(value, "none")) {
                style->word_break_mode |= STYLE_HYPHENS_NONE;
            } else if (modern_keyword(value, "manual")
                       || modern_keyword(value, "auto")
                       || modern_keyword(value, "initial")) {
                style->word_break_mode &= (uint8_t) ~STYLE_HYPHENS_NONE;
            }
        }
        if ((typography & STYLE_MODERN_TYPOGRAPHY_KERNING) != 0
            && retained_modern_value(sheet, node, "font-kerning", value)) {
            if (modern_keyword(value, "none")
                || modern_keyword(value, "optimizeSpeed")) {
                style->word_break_mode |= STYLE_FONT_KERNING_NONE;
            } else if (modern_keyword(value, "normal")
                       || modern_keyword(value, "auto")
                       || modern_keyword(value, "initial")) {
                style->word_break_mode &= (uint8_t) ~STYLE_FONT_KERNING_NONE;
            }
        }
        if ((typography & STYLE_MODERN_TYPOGRAPHY_TEXT_RENDERING) != 0
            && retained_modern_value(sheet, node, "text-rendering", value)) {
            if (modern_keyword(value, "optimizeSpeed")) {
                style->word_break_mode |= STYLE_FONT_KERNING_NONE;
            } else if (modern_keyword(value, "optimizeLegibility")
                       || modern_keyword(value, "geometricPrecision")) {
                style->word_break_mode &= (uint8_t) ~STYLE_FONT_KERNING_NONE;
            }
        }
        unsigned tab_size = parent == NULL
            ? 8u : computed_style_tab_size(parent);
        if ((typography & STYLE_MODERN_TYPOGRAPHY_TAB_SIZE) != 0
            && retained_modern_value(sheet, node, "tab-size", value)) {
            char *end = NULL;
            unsigned long parsed = strtoul(value, &end, 10);
            if (end != value && *end == '\0') {
                if (parsed < 1u) parsed = 1u;
                if (parsed > 8u) parsed = 8u;
                tab_size = (unsigned) parsed;
            } else if (modern_keyword(value, "initial")) {
                tab_size = 8u;
            }
        }
        unsigned tab_code = tab_size == 8u ? 0u : tab_size;
        style->line_clamp = (uint8_t) (
            (style->line_clamp & STYLE_LINE_CLAMP_MASK)
            | (tab_code << STYLE_TAB_SIZE_SHIFT));
    }

    if ((mask & STYLE_MODERN_MIX_BLEND) != 0) {
        unsigned mode = STYLE_MIX_BLEND_NORMAL;
        if (retained_modern_value(sheet, node, "mix-blend-mode", value)) {
            if (modern_keyword(value, "multiply")) {
                mode = STYLE_MIX_BLEND_MULTIPLY;
            } else if (modern_keyword(value, "screen")) {
                mode = STYLE_MIX_BLEND_SCREEN;
            } else if (modern_keyword(value, "darken")) {
                mode = STYLE_MIX_BLEND_DARKEN;
            }
        }
        paint.reserved = (uint8_t) (
            (paint.reserved & ~STYLE_PAINT_MIX_BLEND_MASK) | mode);
        paint_dirty |= mode != STYLE_MIX_BLEND_NORMAL;
    }
    if ((mask & STYLE_MODERN_BACKDROP_FILTER) != 0) {
        /* -webkit-backdrop-filter is retained under this name. */
        bool found = retained_modern_value(
            sheet, node, "backdrop-filter", value);
        unsigned radius = 0;
        if (found) {
            const char *blur = strstr(value, "blur(");
            if (blur != NULL) {
                char *end = NULL;
                double parsed = strtod(blur + 5, &end);
                if (end != blur + 5 && isfinite(parsed) && parsed > 0.0) {
                    radius = parsed >= 3.0 ? 3u
                        : (parsed >= 2.0 ? 2u : 1u);
                }
            }
        }
        paint.reserved = (uint8_t) (
            (paint.reserved & ~STYLE_PAINT_BACKDROP_BLUR_MASK)
            | (radius << STYLE_PAINT_BACKDROP_BLUR_SHIFT));
        if (radius != 0u) {
            style->has_filter = true;
            paint_dirty = true;
        }
    }

    if ((mask & STYLE_MODERN_USER_SELECT) != 0) {
        /* `user-select` is not inherited. Its `auto` used value may be
           constrained by an ancestor, but getComputedStyle still reports
           the computed `auto`; keep that distinction for the interaction
           policy rather than baking it into every descendant. */
        StyleUserSelect mode = STYLE_USER_SELECT_AUTO;
        if (retained_modern_value(sheet, node, "user-select", value)) {
            if (modern_keyword(value, "auto")) mode = STYLE_USER_SELECT_AUTO;
            else if (modern_keyword(value, "text")) mode = STYLE_USER_SELECT_TEXT;
            else if (modern_keyword(value, "none")) mode = STYLE_USER_SELECT_NONE;
            else if (modern_keyword(value, "all")) mode = STYLE_USER_SELECT_ALL;
            else if (modern_keyword(value, "initial")) mode = STYLE_USER_SELECT_AUTO;
            else if (modern_keyword(value, "inherit") && parent != NULL) {
                mode = computed_style_user_select(parent);
            }
        }
        style->overflow_wrap = (uint8_t) (
            (style->overflow_wrap & ~STYLE_USER_SELECT_MASK)
            | ((mode << STYLE_USER_SELECT_SHIFT) & STYLE_USER_SELECT_MASK));
    }
    if ((mask & STYLE_MODERN_TEXT_WRAP) != 0) {
        StyleTextWrap mode = parent == NULL ? STYLE_TEXT_WRAP_NORMAL
            : computed_style_text_wrap(parent);
        bool found = retained_modern_value(
            sheet, node, "text-wrap-style", value);
        if (!found) found = retained_modern_value(
            sheet, node, "text-wrap", value);
        if (found) {
            if (modern_keyword(value, "balance")) mode = STYLE_TEXT_WRAP_BALANCE;
            else if (modern_keyword(value, "pretty")) mode = STYLE_TEXT_WRAP_PRETTY;
            else if (modern_keyword(value, "wrap")
                     || modern_keyword(value, "auto")
                     || modern_keyword(value, "initial")) {
                mode = STYLE_TEXT_WRAP_NORMAL;
            } else if ((modern_keyword(value, "inherit")
                        || modern_keyword(value, "unset")) && parent != NULL) {
                mode = computed_style_text_wrap(parent);
            }
        }
        style->overflow_wrap = (uint8_t) (
            (style->overflow_wrap & ~STYLE_TEXT_WRAP_MASK)
            | ((mode << STYLE_TEXT_WRAP_SHIFT) & STYLE_TEXT_WRAP_MASK));
    }
    if ((mask & STYLE_MODERN_TOUCH_ACTION) != 0
        && retained_modern_value(sheet, node, "touch-action", value)) {
        style->touch_action = modern_keyword(value, "none")
            ? STYLE_TOUCH_ACTION_NONE
            : (modern_keyword(value, "pan-x") ? STYLE_TOUCH_ACTION_PAN_X
               : (modern_keyword(value, "pan-y") ? STYLE_TOUCH_ACTION_PAN_Y
                                                   : STYLE_TOUCH_ACTION_AUTO));
    }
    if ((mask & STYLE_MODERN_RESIZE) != 0
        && retained_modern_value(sheet, node, "resize", value)) {
        style->resize_mode = modern_keyword(value, "both")
            ? STYLE_RESIZE_BOTH
            : ((modern_keyword(value, "horizontal")
                || modern_keyword(value, "inline"))
               ? STYLE_RESIZE_HORIZONTAL
               : ((modern_keyword(value, "vertical")
                   || modern_keyword(value, "block"))
                  ? STYLE_RESIZE_VERTICAL : STYLE_RESIZE_NONE));
    }
    if ((mask & STYLE_MODERN_ISOLATION) != 0
        && retained_modern_value(sheet, node, "isolation", value)) {
        if (modern_keyword(value, "isolate")) {
            style->overflow_wrap |= STYLE_ISOLATION_ISOLATE;
        } else if (modern_keyword(value, "auto")
                   || modern_keyword(value, "initial")) {
            style->overflow_wrap &= (uint8_t) ~STYLE_ISOLATION_ISOLATE;
        }
    }
    if ((mask & STYLE_MODERN_TRANSLATE) != 0
        && retained_modern_value(sheet, node, "translate", value)
        && !modern_keyword(value, "none")) {
        const char *cursor = value;
        int x = 0, y = 0;
        bool xp = false, yp = false;
        if (modern_translation_component(sheet, &cursor, &x, &xp)) {
            while (isspace((unsigned char) *cursor)) cursor++;
            if (*cursor != '\0'
                && !modern_translation_component(sheet, &cursor, &y, &yp)) {
                x = y = 0;
            }
            while (isspace((unsigned char) *cursor)) cursor++;
            if (*cursor == '\0') {
                bool compatible_x = style->transform_x <= STYLE_LENGTH_DIRECT_LIMIT
                    && (x <= STYLE_LENGTH_DIRECT_LIMIT || style->transform_x == 0)
                    && (style->transform_x == 0 || style->transform_x_percent == xp);
                bool compatible_y = style->transform_y <= STYLE_LENGTH_DIRECT_LIMIT
                    && (y <= STYLE_LENGTH_DIRECT_LIMIT || style->transform_y == 0)
                    && (style->transform_y == 0 || style->transform_y_percent == yp);
                /* The compact transform record holds one unit kind per
                   axis. Preserve an earlier transform instead of adding a
                   percentage to pixels when an uncommon mixed-unit
                   composition cannot be represented exactly. */
                if (compatible_x && compatible_y) {
                    style->transform_x += x;
                    style->transform_y += y;
                    style->transform_x_percent |= xp;
                    style->transform_y_percent |= yp;
                    style->has_transform = true;
                }
            }
        }
    }
    if ((mask & STYLE_MODERN_SCALE) != 0
        && retained_modern_value(sheet, node, "scale", value)) {
        uint16_t scale = 16384;
        if (style_scale_value_q14(value, &scale)) {
            unsigned combined = (unsigned) style->transform_scale_q14 * scale
                / STYLE_TRANSFORM_SCALE_Q14_ONE;
            style->transform_scale_q14 = (uint16_t) (
                combined > STYLE_TRANSFORM_SCALE_Q14_MAX
                    ? STYLE_TRANSFORM_SCALE_Q14_MAX : combined);
            style->has_transform |= scale != 16384;
        }
    }
    /* Tilefinch's retained display list is intrinsically flat. Accepting
       preserve-3d/backface declarations while resolving them to the front
       face gives unsupported 3D scenes one stable, visible 2D state instead
       of dropping the entire declaration block. No per-node state is needed. */
    if ((mask & STYLE_MODERN_COLOR_SCHEME) != 0 && root_element
        && retained_modern_value(sheet, node, "color-scheme", value)
        && modern_keyword(value, "dark")) {
        if (!style->has_background) {
            style->background = UINT32_C(0x121212);
            style->background_alpha = 255u;
            style->has_background = true;
        }
        if (style->color == 0u && style->color_alpha != 0u) {
            style->color = UINT32_C(0xe8e8e8);
        }
    }
    if ((mask & STYLE_MODERN_BORDER_IMAGE) != 0) {
        bool found = retained_modern_value(
            sheet, node, "border-image-source", value);
        if (!found) found = retained_modern_value(
            sheet, node, "border-image", value);
        if (found) {
            size_t gradient_length = strlen(value);
            const char *open = strchr(value, '(');
            if (open != NULL) {
                unsigned nesting = 0;
                for (const char *at = open;
                     *at != '\0'; at++) {
                    if (*at == '(') nesting++;
                    else if (*at == ')' && nesting != 0
                             && --nesting == 0) {
                        gradient_length = (size_t) (at - value) + 1u;
                        break;
                    }
                }
            }
            StyleGradient gradient = {0};
            if (style_parse_gradient(
                    sheet, value, gradient_length, &gradient)
                && gradient.stop_count != 0) {
                /* The compact display list has solid borders, not nine-slice
                   images. Preserve the authored geometry and use the first
                   ramp stop as a stable bounded approximation instead of
                   dropping the border-image declaration entirely. */
                uint32_t argb = gradient.stop_argb[0];
                style->border_color = argb & UINT32_C(0x00ffffff);
                style->border_alpha = (uint8_t) (argb >> 24);
                style->border_color_set = 0;
            }
        }
    }
    if ((mask & STYLE_MODERN_ROTATE) != 0
        && retained_modern_value(sheet, node, "rotate", value)) {
        uint8_t quadrants = 0;
        if (style_rotate_value_quadrants(value, &quadrants)) {
            style->individual_rotate_quadrants = (uint8_t) (
                (style->individual_rotate_quadrants + quadrants) & 3u);
            style->has_transform |= quadrants != 0;
        }
    }
    if ((mask & STYLE_MODERN_LOGICAL_RADIUS) != 0) {
        static const char *const names[4] = {
            "border-start-start-radius", "border-start-end-radius",
            "border-end-start-radius", "border-end-end-radius"
        };
        for (unsigned logical = 0; logical < 4; logical++) {
            if (!retained_modern_value(sheet, node, names[logical], value)) {
                continue;
            }
            int radius = 0;
            if (!modern_radius_value(value, &radius)) continue;
            unsigned corner = modern_logical_corner(
                style, logical < 2u, (logical & 1u) == 0);
            uint8_t present = style_border_radius_present(style->border_radius);
            style->border_radius = style_border_radius_set_corner_present(
                style->border_radius, present, corner, radius);
        }
    }
    if ((mask & STYLE_MODERN_TEXT_SIZE_ADJUST) != 0) {
        /* resolve_font_size has already consumed the ordinary unit enum.
           Reuse the byte afterward as a 1-based text-size-adjust percentage
           so 0% remains distinguishable from the inactive `auto` state. */
        unsigned percent = parent != NULL && parent->font_size_unit != 0u
            ? (unsigned) parent->font_size_unit - 1u : 100u;
        bool active = parent != NULL && parent->font_size_unit != 0u;
        if (retained_modern_value(sheet, node, "text-size-adjust", value)) {
            if (modern_keyword(value, "auto")
                || modern_keyword(value, "none")
                || modern_keyword(value, "initial")) {
                active = false;
                percent = 100u;
            } else {
                char *end = NULL;
                double parsed = strtod(value, &end);
                if (end != value && *end == '%' && end[1] == '\0'
                    && isfinite(parsed) && parsed >= 0.0) {
                    if (parsed > 250.0) parsed = 250.0;
                    percent = (unsigned) (parsed + 0.5);
                    active = true;
                }
            }
        }
        int current = computed_style_font_size_fixed(style);
        int inherited = parent == NULL ? STYLE_DEFAULT_FONT_PX * 64
            : computed_style_font_size_fixed(parent);
        if (active && (root_element || current != inherited)) {
            /* current is already clamped to 128px in 26.6 fixed point and
               percent is capped at 250 above. */
            int adjusted = current * (int) percent / 100;
            if (adjusted < STYLE_FONT_MIN_PX * 64) {
                adjusted = STYLE_FONT_MIN_PX * 64;
            }
            if (adjusted > STYLE_FONT_MAX_PX * 64) {
                adjusted = STYLE_FONT_MAX_PX * 64;
            }
            style->font_size = style_font_size_from_fixed(
                adjusted, &style->font_size_fraction);
            style->font_scale = adjusted >= 20 * 64 ? 3
                : (adjusted >= 13 * 64 ? 2 : 1);
        }
        style->font_size_unit = active
            ? (uint8_t) (1u + (percent > 250u ? 250u : percent)) : 0u;
    }
    if (paint_dirty) {
        (void) style_apply_paint_stack(
            (Stylesheet *) sheet, style, &paint, NULL);
    }
}

/* Bounded first-strong resolution for the HTML `dir=auto` presentation
   hint. Digits and punctuation are neutral for this decision. */
static int presentational_auto_direction(lxb_dom_node_t *root)
{
    lxb_dom_node_t *node = root == NULL ? NULL : root->first_child;
    for (size_t visited = 0; node != NULL && visited < 512; visited++) {
        if (node->type == LXB_DOM_NODE_TYPE_TEXT) {
            size_t length = 0;
            const char *text = document_text_data(node, &length);
            for (size_t at = 0; text != NULL && at < length;) {
                unsigned codepoint = 0;
                size_t used = font_utf8_next(
                    text + at, length - at, &codepoint);
                if (used == 0) break;
                if ((codepoint >= 0x0590u && codepoint <= 0x08ffu)
                    || (codepoint >= 0xfb1du && codepoint <= 0xfdffu)
                    || (codepoint >= 0xfe70u && codepoint <= 0xfeffu)) {
                    return 1;
                }
                if ((codepoint >= 'A' && codepoint <= 'Z')
                    || (codepoint >= 'a' && codepoint <= 'z')
                    || (codepoint >= 0x00c0u && codepoint <= 0x02afu)
                    || (codepoint >= 0x0370u && codepoint <= 0x058fu)
                    || (codepoint >= 0x0900u && codepoint <= 0x1fffu)
                    || (codepoint >= 0x2e80u && codepoint <= 0xd7a3u)) {
                    return -1;
                }
                at += used;
            }
        }
        if (node->first_child != NULL) {
            node = node->first_child;
            continue;
        }
        while (node != NULL && node != root && node->next == NULL) {
            node = node->parent;
        }
        node = node == NULL || node == root ? NULL : node->next;
    }
    return 0;
}

/* HTML's rules for parsing dimension values: optional leading whitespace,
   decimal digits with an optional fraction, then "%" for a percentage;
   anything else after the number is ignored. Returns 0 when there is no
   number. Clamped like the CSS length parser. */
static int presentational_dimension(const char *text, size_t length,
                                    bool *percent)
{
    size_t at = 0;
    *percent = false;
    while (at < length && (text[at] == ' ' || text[at] == '\t'
                           || text[at] == '\n' || text[at] == '\f'
                           || text[at] == '\r')) at++;
    double number = 0.0;
    size_t digits = 0;
    while (at < length && text[at] >= '0' && text[at] <= '9') {
        if (number < 32767.0) number = number * 10.0 + (text[at] - '0');
        at++;
        digits++;
    }
    if (digits == 0) return 0;
    if (at < length && text[at] == '.') {
        double scale = 0.1;
        for (at++; at < length && text[at] >= '0' && text[at] <= '9'; at++) {
            number += (text[at] - '0') * scale;
            scale *= 0.1;
        }
    }
    if (at < length && text[at] == '%') *percent = true;
    if (number > 32767.0) number = 32767.0;
    return (int) (number + 0.5);
}

static void apply_presentational_attributes(const Stylesheet *sheet,
                                            lxb_dom_node_t *node,
                                            ComputedStyle *style)
{
    size_t node_name_length = 0;
    const char *node_name = document_element_name(
        node, &node_name_length);
#define PRESENTATION_TAG_IS(wanted) \
    style_element_name_is(node_name, node_name_length, (wanted))
    size_t length = 0;
    /* UA rule [hidden] { display: none } -- mobile section togglers can
       collapse article sections purely via the hidden
       attribute.  Author display declarations still override, matching
       the spec's UA-origin placement. */
    if (lxb_dom_element_has_attribute(lxb_dom_interface_element(node),
                                      (const lxb_char_t *) "hidden", 6)) {
        style->display = DISPLAY_NONE;
    }
    const char *value = document_attribute(node, "bgcolor", &length);
    uint32_t color = 0;
    if (value != NULL && style_parse_color(sheet, value, length, &color)) {
        style->background = color;
        style->background_alpha = 255;
        style->has_background = true;
    }
    value = document_attribute(node, PRESENTATION_TAG_IS("body")
                               ? "text" : "color",
                               &length);
    if (value != NULL && style_parse_color(sheet, value, length, &color)) {
        style->color = color;
        style->color_alpha = 255;
    }
    /* HTML maps width/height to the dimension properties only on replaced
       and table elements, and parses them with the "rules for parsing
       dimension values": digits, then an optional %, anything after
       ignored ("10 em" is 10px). Applying them to every element broke
       Cloudflare's Turnstile widget, whose <div height="10 em"> became a
       140px flex container and pushed its checkbox out of the 65px frame.
       SVG width/height are SVG presentation attributes and take CSS
       lengths. */
    bool replaced = PRESENTATION_TAG_IS("img")
                    || PRESENTATION_TAG_IS("iframe")
                    || PRESENTATION_TAG_IS("embed")
                    || PRESENTATION_TAG_IS("object")
                    || PRESENTATION_TAG_IS("video")
                    || PRESENTATION_TAG_IS("canvas")
                    || PRESENTATION_TAG_IS("input");
    bool svg = PRESENTATION_TAG_IS("svg");
    bool cell = PRESENTATION_TAG_IS("td") || PRESENTATION_TAG_IS("th");
    bool width_mapped = replaced || svg || cell
                        || PRESENTATION_TAG_IS("table")
                        || PRESENTATION_TAG_IS("col")
                        || PRESENTATION_TAG_IS("colgroup")
                        || PRESENTATION_TAG_IS("hr");
    bool height_mapped = replaced || svg || cell
                         || PRESENTATION_TAG_IS("table")
                         || PRESENTATION_TAG_IS("tr")
                         || PRESENTATION_TAG_IS("thead")
                         || PRESENTATION_TAG_IS("tbody")
                         || PRESENTATION_TAG_IS("tfoot");
    value = width_mapped ? document_attribute(node, "width", &length) : NULL;
    if (value != NULL && length != 0) {
        style->width = svg
            ? style_parse_length(sheet, value, length, 0,
                                 &style->width_percent)
            : presentational_dimension(value, length, &style->width_percent);
        style->has_width = style->width > 0;
    }
    value = height_mapped
            ? document_attribute(node, "height", &length) : NULL;
    if (value != NULL && length != 0) {
        style->height = svg
            ? style_parse_length(sheet, value, length, 0,
                                 &style->height_percent)
            : presentational_dimension(value, length,
                                       &style->height_percent);
        style->has_height = style->height > 0;
    }
    value = document_attribute(node, "align", &length);
    if (value != NULL) {
        if (span_equal(value, length, "center")
            || span_equal(value, length, "middle")) {
            style->text_align = TEXT_ALIGN_CENTER;
        } else if (span_equal(value, length, "right")) {
            style->text_align = TEXT_ALIGN_RIGHT;
        } else if (span_equal(value, length, "left")) {
            style->text_align = TEXT_ALIGN_LEFT;
        }
    }
    value = document_attribute(node, "dir", &length);
    if (value != NULL) {
        if (span_case_equal(value, length, "rtl")) {
            style->filter_code |= STYLE_DIRECTION_RTL;
        } else if (span_case_equal(value, length, "ltr")) {
            style->filter_code &= (uint8_t) ~STYLE_DIRECTION_RTL;
        } else if (span_case_equal(value, length, "auto")) {
            int direction = presentational_auto_direction(node);
            if (direction > 0) {
                style->filter_code |= STYLE_DIRECTION_RTL;
            } else if (direction < 0) {
                style->filter_code &= (uint8_t) ~STYLE_DIRECTION_RTL;
            }
        }
    }
    bool table_vertical_hint = PRESENTATION_TAG_IS("thead")
                               || PRESENTATION_TAG_IS("tbody")
                               || PRESENTATION_TAG_IS("tfoot")
                               || PRESENTATION_TAG_IS("tr")
                               || PRESENTATION_TAG_IS("td")
                               || PRESENTATION_TAG_IS("th");
    value = table_vertical_hint
            ? document_attribute(node, "valign", &length) : NULL;
    if (value != NULL) {
        if (span_equal(value, length, "top")) {
            style->vertical_align = VERTICAL_TOP;
        } else if (span_equal(value, length, "middle")) {
            style->vertical_align = VERTICAL_MIDDLE;
        } else if (span_equal(value, length, "bottom")) {
            style->vertical_align = VERTICAL_BOTTOM;
        } else if (span_equal(value, length, "baseline")) {
            style->vertical_align = VERTICAL_BASELINE;
        }
    }
    if (PRESENTATION_TAG_IS("td") || PRESENTATION_TAG_IS("th")) {
        for (lxb_dom_node_t *ancestor = node->parent;
             ancestor != NULL; ancestor = ancestor->parent) {
            if (!style_tag_is(ancestor, "table")) continue;
            int cell_padding = 0;
            if (presentational_nonnegative_pixels(
                    ancestor, "cellpadding", &cell_padding)) {
                style->padding = (StyleEdges) {
                    cell_padding, cell_padding,
                    cell_padding, cell_padding
                };
            }
            break;
        }
    }
#undef PRESENTATION_TAG_IS
}

static bool style_element_name_is(const char *name, size_t length,
                                  const char *wanted)
{
    size_t wanted_length = strlen(wanted);
    return name != NULL && length == wanted_length
        && memcmp(name, wanted, length) == 0;
}

static bool style_element_is_ua_block(const char *name, size_t length)
{
    if (name == NULL) return false;
    switch (length) {
        case 1:
            return name[0] == 'p';
        case 2:
            return (name[0] == 'h' && name[1] >= '1' && name[1] <= '6')
                || style_element_name_is(name, length, "ul")
                || style_element_name_is(name, length, "ol")
                || style_element_name_is(name, length, "li")
                || style_element_name_is(name, length, "hr")
                || style_element_name_is(name, length, "dl")
                || style_element_name_is(name, length, "dt")
                || style_element_name_is(name, length, "dd");
        case 3:
            return style_element_name_is(name, length, "nav")
                || style_element_name_is(name, length, "div")
                || style_element_name_is(name, length, "pre")
                || style_element_name_is(name, length, "dir");
        case 4:
            return style_element_name_is(name, length, "menu")
                || style_element_name_is(name, length, "html")
                || style_element_name_is(name, length, "body")
                || style_element_name_is(name, length, "main")
                || style_element_name_is(name, length, "form");
        case 5:
            return style_element_name_is(name, length, "aside");
        case 6:
            return style_element_name_is(name, length, "header")
                || style_element_name_is(name, length, "footer")
                || style_element_name_is(name, length, "figure")
                || style_element_name_is(name, length, "legend")
                || style_element_name_is(name, length, "center");
        case 7:
            return style_element_name_is(name, length, "section")
                || style_element_name_is(name, length, "address")
                || style_element_name_is(name, length, "article")
                || style_element_name_is(name, length, "details")
                || style_element_name_is(name, length, "summary");
        case 8:
            return style_element_name_is(name, length, "fieldset");
        case 10:
            return style_element_name_is(name, length, "figcaption")
                || style_element_name_is(name, length, "blockquote");
        default:
            return false;
    }
}

static void inherit_text_shadow(const Stylesheet *sheet,
                                ComputedStyle *style,
                                const ComputedStyle *parent)
{
    if (sheet == NULL || style == NULL || parent == NULL) return;
    const StylePaintStack *parent_paint = stylesheet_paint_stack(
        sheet, computed_style_paint_stack_id(parent));
    if (parent_paint == NULL
        || (parent_paint->components
            & STYLE_PAINT_COMPONENT_TEXT_SHADOW) == 0) {
        return;
    }
    uint8_t id = 0;
    if (style_intern_text_shadow(
            (Stylesheet *) sheet, parent_paint, &id)
        == STYLE_PAINT_INTERN_RETAINED) {
        computed_style_set_paint_stack_id(style, id);
    }
}

static bool style_element_is_ua_list(const char *name, size_t length)
{
    return style_element_name_is(name, length, "ul")
        || style_element_name_is(name, length, "ol")
        || style_element_name_is(name, length, "menu")
        || style_element_name_is(name, length, "dir")
        || style_element_name_is(name, length, "dl");
}

/* HTML's UA sheet drops the block margins of a list nested anywhere inside
   another list (":is(dir, dl, menu, ol, ul) :is(dir, dl, menu, ol, ul)"). */
static bool style_ua_list_is_nested(lxb_dom_node_t *node)
{
    unsigned depth = 0;
    for (lxb_dom_node_t *ancestor = node == NULL ? NULL : node->parent;
         ancestor != NULL && depth < 256u;
         ancestor = ancestor->parent, depth++) {
        if (ancestor->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        size_t length = 0;
        const char *name = document_element_name(ancestor, &length);
        if (style_element_is_ua_list(name, length)) return true;
    }
    return false;
}

static ComputedStyle default_style(const Stylesheet *sheet,
                                   lxb_dom_node_t *node,
                                   const ComputedStyle *parent)
{
    size_t node_name_length = 0;
    const char *node_name = document_element_name(
        node, &node_name_length);
#define NODE_TAG_IS(wanted) \
    style_element_name_is(node_name, node_name_length, (wanted))
    ComputedStyle style = {.display = DISPLAY_INLINE, .color = 0x000000,
                           .color_alpha = 255, .background_alpha = 255,
                           .font_scale = 2, .font_size = 16,
                           .root_font_size = 16,
                           .font_family = FONT_SANS,
                           .font_weight = 400,
                           .border_color = UINT32_MAX,
                           .border_alpha = 255,
                           .opacity = 255,
                           .transform_scale_q14 = 16384,
                           .min_width_auto = true,
                           .min_height_auto = true,
                           .max_width = STYLE_LENGTH_NONE,
                           .max_height = STYLE_LENGTH_NONE,
                           .object_position_x =
                               style_object_position_encode(50, 0),
                           .object_position_y =
                               style_object_position_encode(50, 0),
                           .flex_direction = FLEX_ROW, .flex_shrink = 512,
                           .align_items = ALIGN_STRETCH,
                           .justify_items = ALIGN_STRETCH,
                           .align_content = JUSTIFY_STRETCH};
    if (NODE_TAG_IS("input") || NODE_TAG_IS("textarea")
        || NODE_TAG_IS("select") || NODE_TAG_IS("button")) {
        style.appearance = APPEARANCE_AUTO;
    }
    if (parent != NULL) {
        style.color = parent->color;
        style.color_alpha = parent->color_alpha;
        style.font_scale = parent->font_scale;
        style.font_size = parent->font_size;
        style.font_size_fraction = parent->font_size_fraction;
        style.font_size_unit = parent->font_size_unit;
        style.root_font_size = parent->root_font_size;
        style.root_font_size_fraction = parent->root_font_size_fraction;
        style.font_family = parent->font_family;
        style.font_weight = parent->font_weight;
        style.font_bold = parent->font_bold;
        style.font_italic = parent->font_italic;
        style.text_transform = parent->text_transform;
        unsigned inherited_offset = style_text_offset_code(
            parent, STYLE_TEXT_OFFSET_SHIFT, STYLE_TEXT_OFFSET_MASK);
        style_set_text_offset_code(&style, inherited_offset,
                                   STYLE_TEXT_OFFSET_SHIFT,
                                   STYLE_TEXT_OFFSET_MASK);
        if (computed_style_has_text_underline(parent)) {
            style_set_ancestor_text_decoration(
                &style, true, inherited_offset);
        } else if (computed_style_has_ancestor_text_underline(parent)) {
            style_set_ancestor_text_decoration(
                &style, true, style_text_offset_code(
                    parent, STYLE_ANCESTOR_TEXT_OFFSET_SHIFT,
                    STYLE_ANCESTOR_TEXT_OFFSET_MASK));
        }
        style.word_spacing = parent->word_spacing;
        style.letter_spacing_fixed = parent->letter_spacing_fixed;
        style.line_height = parent->line_height_factor != 0
            ? -(int) parent->line_height_factor : parent->line_height;
        style.line_height_factor = parent->line_height_factor;
        style.text_indent = parent->text_indent;
        style.text_align = parent->text_align;
        style.white_space_mode = parent->white_space_mode;
        style.overflow_wrap = computed_style_overflow_wrap(parent);
        style.word_break_mode = parent->word_break_mode;
        style.list_style_none = parent->list_style_none;
        style.list_style_type = parent->list_style_type;
        style.list_style_inside = parent->list_style_inside;
        style.visibility_hidden = parent->visibility_hidden;
        style.table_border_collapse = parent->table_border_collapse;
        style.filter_code = (uint8_t) (
            (style.filter_code
             & ~(STYLE_WRITING_MODE_MASK | STYLE_DIRECTION_RTL))
            | (parent->filter_code
               & (STYLE_WRITING_MODE_MASK | STYLE_DIRECTION_RTL)));
        inherit_text_shadow(sheet, &style, parent);
        if (parent->display == DISPLAY_TABLE_ROW_GROUP
            || parent->display == DISPLAY_TABLE_HEADER_GROUP
            || parent->display == DISPLAY_TABLE_FOOTER_GROUP) {
            style.order = parent->order;
            if (parent->has_background) {
                style.has_background = true;
                style.background = parent->background;
                style.background_alpha = parent->background_alpha;
            }
        }
    }
    if (style_element_is_ua_block(node_name, node_name_length)) {
        style.display = DISPLAY_BLOCK;
        style.list_item = NODE_TAG_IS("li");
    }
    if (NODE_TAG_IS("pre")) {
        style.white_space_mode = WHITE_SPACE_PRE;
    }
    if (NODE_TAG_IS("option") && !option_is_displayed_by_default(node)) {
        style.display = DISPLAY_NONE;
    }
    if (NODE_TAG_IS("table")) {
        style.display = DISPLAY_TABLE;
        style.box_sizing_border_box = true;
        /* Browser UA styles reset the inherited alignment established by the
           obsolete <center> element at a table boundary.  Its cells then
           inherit start alignment unless author or presentational rules say
           otherwise. */
        style.text_align = TEXT_ALIGN_START;
    }
    if (NODE_TAG_IS("dialog")) {
        bool open = lxb_dom_element_has_attribute(
            lxb_dom_interface_element(node),
            (const lxb_char_t *) "open", 4);
        bool modal = lxb_dom_element_has_attribute(
            lxb_dom_interface_element(node),
            (const lxb_char_t *) "data-tilefinch-modal",
            sizeof("data-tilefinch-modal") - 1);
        style.display = open ? DISPLAY_BLOCK : DISPLAY_NONE;
        if (open && modal) {
            style.fixed_position = true;
            style.has_top = true;
            style.top = 24;
            style.has_left = true;
            style.left = 24;
            style.has_right = true;
            style.right = 24;
            style.has_z_index = true;
            style.z_index = 10000;
            style.padding = (StyleEdges) {12, 12, 12, 12};
            style.has_background = true;
            style.background = 0xffffff;
        }
    }
    bool element_node = node != NULL
                        && node->type == LXB_DOM_NODE_TYPE_ELEMENT;
    bool popover = element_node && lxb_dom_element_has_attribute(
        lxb_dom_interface_element(node),
        (const lxb_char_t *) "popover", 7);
    bool popover_open = element_node && lxb_dom_element_has_attribute(
        lxb_dom_interface_element(node),
        (const lxb_char_t *) "data-tilefinch-popover-open",
        sizeof("data-tilefinch-popover-open") - 1);
    if (popover) {
        style.display = popover_open ? DISPLAY_BLOCK : DISPLAY_NONE;
        if (popover_open) {
            style.fixed_position = true;
            style.has_top = true;
            style.top = 8;
            style.has_right = true;
            style.right = 8;
            style.has_z_index = true;
            style.z_index = 10001;
        }
    }
    if (NODE_TAG_IS("caption")) {
        style.display = DISPLAY_BLOCK;
    }
    if (NODE_TAG_IS("thead") || NODE_TAG_IS("tbody")
        || NODE_TAG_IS("tfoot")) {
        style.display = NODE_TAG_IS("thead") ? DISPLAY_TABLE_HEADER_GROUP
                        : (NODE_TAG_IS("tfoot")
                           ? DISPLAY_TABLE_FOOTER_GROUP
                           : DISPLAY_TABLE_ROW_GROUP);
        style.order = NODE_TAG_IS("thead") ? -100000
                      : (NODE_TAG_IS("tfoot") ? 100000 : 0);
        /* The HTML UA sheet centers table-row groups on the block axis.
           These nodes are flattened during layout, so retain the used value
           for the row and cell styles resolved through that flat ancestry. */
        style.vertical_align = VERTICAL_MIDDLE;
    }
    if (NODE_TAG_IS("colgroup") || NODE_TAG_IS("col")) {
        style.display = DISPLAY_TABLE_COLUMN;
    }
    if (NODE_TAG_IS("tr")) {
        style.display = DISPLAY_TABLE_ROW;
        /* vertical-align itself is not inherited.  The HTML UA sheet makes
           row and cell boxes inherit it explicitly from their table
           ancestry; inline descendants return to the initial baseline. */
        if (parent != NULL) style.vertical_align = parent->vertical_align;
        if (parent != NULL
            && (parent->display == DISPLAY_TABLE_ROW_GROUP
                || parent->display == DISPLAY_TABLE_HEADER_GROUP
                || parent->display == DISPLAY_TABLE_FOOTER_GROUP)) {
            style.order = parent->order;
            if (parent->has_background) {
                /* Row-group backgrounds form a layer behind their rows.
                   Painting the same solid layer on each non-overlapping row
                   is exact and avoids retaining a separate group box. */
                style.has_background = true;
                style.background = parent->background;
                style.background_alpha = parent->background_alpha;
            }
        }
    }
    if (NODE_TAG_IS("td") || NODE_TAG_IS("th")) {
        style.display = DISPLAY_TABLE_CELL;
        if (parent != NULL) style.vertical_align = parent->vertical_align;
    }
    if (NODE_TAG_IS("slot")) {
        /* HTML's UA sheet: a slot generates no box; its assigned nodes (or
           fallback content) take its place in the flat tree. */
        style.display = DISPLAY_CONTENTS;
    }
    if (NODE_TAG_IS("button")) {
        /* Replaced form controls are atomic inline-level boxes in the UA
           display sheet.  Treating them as ordinary inline containers lets
           large text indents and positioned icons leak into surrounding
           line layout. */
        style.display = DISPLAY_INLINE_BLOCK;
    }
    if (NODE_TAG_IS("video") || NODE_TAG_IS("audio")
        || NODE_TAG_IS("canvas")
        || NODE_TAG_IS("iframe")) {
        /* Both are replaced inline-level elements.  Hiding video in the
           historical UA defaults also hid its poster and made intrinsic
           media sizing impossible before playback began. */
        style.display = DISPLAY_INLINE_BLOCK;
    }
    if (NODE_TAG_IS("audio")) {
        bool controls = lxb_dom_element_has_attribute(
            lxb_dom_interface_element(node),
            (const lxb_char_t *) "controls", 8);
        style.display = controls ? DISPLAY_INLINE_BLOCK : DISPLAY_NONE;
        if (controls) {
            style.box_sizing_border_box = true;
            style.has_background = true;
            style.background = 0xe8eaed;
            style.border = (StyleEdges) {1, 1, 1, 1};
            style.border_color = 0x8a9099;
            for (unsigned side = 0; side < STYLE_BORDER_SIDE_COUNT; side++) {
                computed_style_set_border_line(
                    &style, (StyleBorderSide) side, STYLE_BORDER_SOLID);
            }
        }
    }
    if (NODE_TAG_IS("input") || NODE_TAG_IS("textarea")
        || NODE_TAG_IS("select") || NODE_TAG_IS("button")) {
        style.box_sizing_border_box = true;
    }
    if (NODE_TAG_IS("script") || NODE_TAG_IS("style")
        || NODE_TAG_IS("head") || NODE_TAG_IS("meta")
        || NODE_TAG_IS("link") || NODE_TAG_IS("title")
        || NODE_TAG_IS("source")
        || (NODE_TAG_IS("noscript")
            && (sheet == NULL || !sheet->noscript_rendered))) {
        style.display = DISPLAY_NONE;
    }
    /* The HTML UA sheet gives body an 8px margin, not padding.  Keeping the
       two distinct matters when an author resets body margin to let page
       backgrounds and full-bleed headers reach the viewport edge. */
    if (NODE_TAG_IS("body")) style.margin = (StyleEdges) {8, 8, 8, 8};
    if (NODE_TAG_IS("input") || NODE_TAG_IS("textarea")
        || NODE_TAG_IS("select") || NODE_TAG_IS("button")) {
        style.padding = (StyleEdges) {
            .top = 1, .right = 2, .bottom = 1, .left = 2
        };
        style.border = (StyleEdges) {2, 2, 2, 2};
        style.border_color = 0x767676;
        for (unsigned side = 0; side < STYLE_BORDER_SIDE_COUNT; side++) {
            computed_style_set_border_line(
                &style, (StyleBorderSide) side, STYLE_BORDER_SOLID);
        }
        style.has_background = true;
        style.background = 0xffffff;
        /* Chromium's UA sheet gives controls `font: -webkit-small-control`:
           13.333px with a normal line-height rather than the page's;
           `font: inherit` (Bootstrap and most resets) restores both. */
        style.font_size = style_font_size_from_fixed(
            853, &style.font_size_fraction);
        style.font_scale = 2;
        style.line_height = 0;
        style.line_height_factor = 0;
    }
    if (NODE_TAG_IS("button")
        || (NODE_TAG_IS("input") && style_input_is_push_button(node))) {
        /* input[type=submit|button|reset] shares the button face and its
           1px 6px padding (HTML 15.5.x, Chromium's UA sheet). */
        style.background = 0xefefef;
        style.padding.left = 6;
        style.padding.right = 6;
    }
    if (NODE_TAG_IS("select")) {
        /* Reserve the native menulist affordance without changing authored
           padding once the author cascade runs. */
        style.padding.right = 20;
        /* A menulist is an atomic inline-block (Chromium's UA sheet), which
           is also the layout path that paints its drop-down affordance. */
        style.display = DISPLAY_INLINE_BLOCK;
    }
    if (NODE_TAG_IS("input") && style_input_has_special_appearance(node)) {
        /* Toggle/range appearance paints its own shell. Generic text-field
           borders around that shell produce the familiar tiny boxed slider
           seen when these controls become flex/grid items. */
        style.padding = (StyleEdges) {0, 0, 0, 0};
        style.border = (StyleEdges) {0, 0, 0, 0};
        style.has_background = false;
    }
    if ((NODE_TAG_IS("input") || NODE_TAG_IS("textarea")
         || NODE_TAG_IS("select") || NODE_TAG_IS("button"))
        && lxb_dom_element_has_attribute(
               lxb_dom_interface_element(node),
               (const lxb_char_t *) "disabled", 8)) {
        style.opacity = 128;
    }
    /* Chromium's quirks-mode UA sheet retains the historical one-em
       block-end form margin.  Standards and limited-quirks documents do
       not.  Keep an unresolved sentinel until this form's own font size has
       cascaded; any author margin declaration replaces it normally. */
    if (NODE_TAG_IS("form") && node != NULL
        && node->owner_document != NULL
        && node->owner_document->compat_mode
               == LXB_DOM_DOCUMENT_CMODE_QUIRKS) {
        style.margin.bottom = STYLE_LENGTH_NONE;
    }
    if (NODE_TAG_IS("li")) {
        style.margin = (StyleEdges) {0, 0, 0, 0};
    }
    /* HTML 15.3.7/15.3.8: lists get one em of block spacing (none when
       nested in another list) and a 40px inline-start padding, so outside
       markers hang in the padding beside the text. dd, blockquote and
       figure indent by a 40px margin. Sentinels let author margins win and
       resolve against the element's final font size. */
    bool ua_rtl = (style.filter_code & STYLE_DIRECTION_RTL) != 0;
    if (style_element_is_ua_list(node_name, node_name_length)) {
        if (!style_ua_list_is_nested(node)) {
            style.margin.top = STYLE_LENGTH_NONE;
            style.margin.bottom = STYLE_LENGTH_NONE;
        }
        if (!NODE_TAG_IS("dl")) {
            if (ua_rtl) style.padding.right = 40;
            else style.padding.left = 40;
        }
    } else if (NODE_TAG_IS("dd")) {
        if (ua_rtl) style.margin.right = 40;
        else style.margin.left = 40;
    } else if (NODE_TAG_IS("blockquote") || NODE_TAG_IS("figure")) {
        style.margin = (StyleEdges) {
            .top = STYLE_LENGTH_NONE, .right = 40,
            .bottom = STYLE_LENGTH_NONE, .left = 40
        };
    }
    /* UA spacing for paragraphs and headings is block-axis spacing.  Inline
       margins here would indent the element even when the author has made a
       full-width mobile content column. */
    if (NODE_TAG_IS("p")) {
        /* HTML's UA sheet gives paragraphs one em of block spacing.  Retain
           sentinels through the author cascade so an author margin still
           replaces them, then resolve against this paragraph's final font
           size below. */
        style.margin = (StyleEdges) {
            .top = STYLE_LENGTH_NONE, .bottom = STYLE_LENGTH_NONE
        };
    }
    if (NODE_TAG_IS("h1")) {
        style.font_scale = 3; style.font_size = 28;
        style.font_size_fraction = 0;
        style.font_weight = 700; style.font_bold = true;
        style.margin = (StyleEdges) {
            .top = STYLE_LENGTH_NONE, .bottom = STYLE_LENGTH_NONE};
    }
    else if (NODE_TAG_IS("h2") || NODE_TAG_IS("h3")) {
        style.font_scale = 2; style.font_size = 20;
        style.font_size_fraction = 0;
        style.font_weight = 700; style.font_bold = true;
        style.margin = (StyleEdges) {
            .top = STYLE_LENGTH_NONE, .bottom = STYLE_LENGTH_NONE};
    }
    if (NODE_TAG_IS("code") || NODE_TAG_IS("pre")) {
        style.font_family = FONT_MONOSPACE;
    }
    if (NODE_TAG_IS("b") || NODE_TAG_IS("strong") || NODE_TAG_IS("th")) {
        style.font_weight = 700;
        style.font_bold = true;
    }
    if (NODE_TAG_IS("a")) {
        size_t href_length = 0;
        if (document_attribute(node, "href", &href_length) != NULL) {
            /* The UA :link color is a declaration on the anchor, not an
               inherited body color. Author color (including inherit) is
               applied later by the normal cascade. No history-dependent
               visited state is exposed here. */
            style.color = 0x0000ff;
            style.color_alpha = 255;
            style_set_text_underline(&style, true);
        }
    }
    if (NODE_TAG_IS("center")) style.text_align = TEXT_ALIGN_CENTER;
    if (NODE_TAG_IS("i") || NODE_TAG_IS("em")) style.font_italic = true;
    if (NODE_TAG_IS("sup") || NODE_TAG_IS("sub")) {
        style.vertical_align = NODE_TAG_IS("sup")
                               ? VERTICAL_SUPER : VERTICAL_SUB;
        int fixed = computed_style_font_size_fixed(&style) * 3 / 4;
        if (fixed < 6 * 64) fixed = 6 * 64;
        style.font_size = style_font_size_from_fixed(
            fixed, &style.font_size_fraction);
        style.font_scale = fixed >= 13 * 64 ? 2 : 1;
    }
    #undef NODE_TAG_IS
    return style;
}

typedef struct {
    uint32_t begin;
    uint32_t end;
} StyleRuleIndexRange;

/* Utility-class elements (atomic CSS: one rule per class) can carry more
   keyed classes than `ranges` holds. Their class buckets' rule indices are
   then merged, ascending and unique, into a caller's buffer of this many
   entries instead of scanning the whole rule range. */
#define STYLE_RULE_INDEX_LIST_CAPACITY 256u

typedef struct {
    StyleRuleIndexRange ranges[STYLE_RULE_INDEX_MAX_SOURCES];
    size_t count;
    /* The class candidates merged into the caller's buffer when the
       subject's buckets overflow `ranges`, or NULL. */
    const uint32_t *list;
    size_t list_count;
    StyleTokenBloom compound_bloom;
    StyleTokenBloom ancestor_bloom;
    bool ancestor_bloom_ready;
    bool ready;
} StyleRuleIndexPlan;

enum {
    STYLE_RULE_ANCESTOR_FILTER_MIN_CANDIDATES = 64,
    STYLE_RULE_ANCESTOR_FILTER_SMALL_SHEET = 128
};

static StyleTokenBloom style_match_subject_bloom(
    const StyleMatchSubject *subject)
{
    if (subject == NULL) return style_token_bloom_empty();
    StyleTokenBloom bloom = style_token_bloom_empty();
    if (subject->tag != NULL) {
        /* One token per tag: compiled numeric tags use this same canonical
           name in rule filters. Duplicating IDs saturates ancestor summaries. */
        style_token_bloom_merge(
            &bloom, style_compound_token_bloom(
                STYLE_SELECTOR_TAG, subject->tag, subject->tag_length));
    }
    if (subject->id != NULL) {
        style_token_bloom_merge(
            &bloom, style_compound_token_bloom(
                STYLE_SELECTOR_ID, subject->id, subject->id_length));
    }
    size_t at = 0;
    while (subject->classes != NULL && at < subject->classes_length) {
        while (at < subject->classes_length
               && isspace((unsigned char) subject->classes[at])) at++;
        size_t begin = at;
        while (at < subject->classes_length
               && !isspace((unsigned char) subject->classes[at])) at++;
        if (at != begin) {
            style_token_bloom_merge(
                &bloom, style_compound_token_bloom(
                    STYLE_SELECTOR_CLASS, subject->classes + begin,
                    at - begin));
        }
    }
    return bloom;
}

static size_t style_ancestor_cache_slot(const lxb_dom_node_t *node)
{
    uintptr_t key = (uintptr_t) node >> 4;
    key ^= key >> 11;
    return key & (STYLE_ANCESTOR_BLOOM_CACHE_CAPACITY - 1u);
}

static StyleTokenBloom style_match_ancestor_bloom(
    const Stylesheet *sheet, const StyleMatchSubject *subject)
{
    lxb_dom_node_t *at = subject == NULL || subject->node == NULL
        ? NULL : subject->node->parent;
    lxb_dom_node_t *first = at;
    StyleAncestorBloomCache *cache = sheet->resolve_scratch == NULL
        ? NULL : sheet->resolve_scratch->ancestor_bloom_cache;
    StyleTokenBloom bloom = style_token_bloom_empty();
    size_t visits = 0;
    for (; at != NULL && visits < 64u; at = at->parent, visits++) {
        if (cache != NULL) {
            size_t slot = style_ancestor_cache_slot(at);
            if (cache->entries[slot].node == at) {
                bloom.words[0] |= cache->entries[slot].words[0];
                bloom.words[1] |= cache->entries[slot].words[1];
                at = NULL;
                break;
            }
        }
        if (at->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        StyleMatchSubject ancestor = {0};
        style_match_subject_prepare(at, &ancestor);
        style_token_bloom_merge(
            &bloom, style_match_subject_bloom(&ancestor));
    }
    /* Truncation must fail open: a required token may exist above the bound. */
    if (at != NULL) bloom = (StyleTokenBloom) {{UINT32_MAX, UINT32_MAX}};
    /* Only enabled inside an immutable layout/cooperation scope. No DOM or
       attribute pointers survive the build; collisions merely lose a hit. */
    if (cache != NULL && first != NULL) {
        size_t slot = style_ancestor_cache_slot(first);
        cache->entries[slot].node = first;
        cache->entries[slot].words[0] = bloom.words[0];
        cache->entries[slot].words[1] = bloom.words[1];
    }
    return bloom;
}

/* The bits of the sheet's exact ancestor tokens that `subject` carries. */
static uint64_t style_subject_ancestor_token_bits(
    const struct StyleRuleAncestorTokens *tokens,
    const StyleMatchSubject *subject)
{
    uint64_t bits = 0;
    if (subject->id != NULL) {
        unsigned found = style_rule_ancestor_token_find(
            tokens, style_token_hash(STYLE_SELECTOR_ID, subject->id,
                                     subject->id_length));
        if (found != 0)
            bits |= UINT64_C(1) << (style_rule_ancestor_token_bit(found) - 1u);
    }
    size_t at = 0;
    while (subject->classes != NULL && at < subject->classes_length) {
        while (at < subject->classes_length
               && isspace((unsigned char) subject->classes[at])) at++;
        size_t begin = at;
        while (at < subject->classes_length
               && !isspace((unsigned char) subject->classes[at])) at++;
        if (at == begin) continue;
        unsigned found = style_rule_ancestor_token_find(
            tokens, style_token_hash(STYLE_SELECTOR_CLASS,
                                     subject->classes + begin, at - begin));
        if (found != 0)
            bits |= UINT64_C(1) << (style_rule_ancestor_token_bit(found) - 1u);
    }
    return bits;
}

/* Which of the sheet's exact ancestor tokens some ancestor of `node`
   carries; every bit past the walk bound (the matchers' own descendant
   walks are bounded no tighter, and a truncated answer must fail open).
   Cached per ancestor chain like the Bloom above, inside a build scope. */
static uint64_t style_ancestor_token_mask(const Stylesheet *sheet,
                                          const lxb_dom_node_t *node)
{
    const struct StyleRuleAncestorTokens *tokens = sheet->rule_ancestor_tokens;
    lxb_dom_node_t *at = node->parent;
    lxb_dom_node_t *first = at;
    StyleAncestorBloomCache *cache = sheet->resolve_scratch == NULL
        ? NULL : sheet->resolve_scratch->ancestor_bloom_cache;
    uint64_t mask = 0;
    size_t visits = 0;
    for (; at != NULL && visits < 64u; at = at->parent, visits++) {
        if (cache != NULL) {
            size_t slot = style_ancestor_cache_slot(at);
            if (cache->token_entries[slot].node == at
                && cache->token_entries[slot].stamp == tokens->stamp) {
                mask |= cache->token_entries[slot].mask;
                at = NULL;
                break;
            }
        }
        if (at->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        StyleMatchSubject ancestor = {0};
        style_match_subject_prepare(at, &ancestor);
        mask |= style_subject_ancestor_token_bits(tokens, &ancestor);
    }
    if (at != NULL) mask = UINT64_MAX;
    if (cache != NULL && first != NULL) {
        size_t slot = style_ancestor_cache_slot(first);
        cache->token_entries[slot].node = first;
        cache->token_entries[slot].mask = mask;
        cache->token_entries[slot].stamp = tokens->stamp;
    }
    return mask;
}

/* False when `candidate` names an exact ancestor token no ancestor of
   `node` carries; `mask` is computed on first need. */
static bool style_candidate_ancestor_token_possible(
    const Stylesheet *sheet, uint32_t candidate, lxb_dom_node_t *node,
    uint64_t *mask, bool *ready)
{
    if (sheet->rule_ancestor_tokens == NULL || sheet->rule_filters == NULL)
        return true;
    unsigned token = sheet->rule_filters[candidate].ancestor_token;
    if (token == 0) return true;
    if (!*ready) {
        *mask = style_ancestor_token_mask(sheet, node);
        *ready = true;
    }
    return (*mask & (UINT64_C(1) << (token - 1u))) != 0;
}

static void apply_style_rule(const Stylesheet *sheet, const StyleRule *rule,
                             ComputedStyle *style,
                             const ComputedStyle *parent);

static uint32_t style_rule_range_lower_bound(const uint32_t *entries,
                                             uint32_t begin, uint32_t end,
                                             size_t wanted)
{
    while (begin < end) {
        uint32_t middle = begin + (end - begin) / 2u;
        if ((size_t) entries[middle] < wanted) begin = middle + 1u;
        else end = middle;
    }
    return begin;
}

static bool style_rule_add_range(StyleRuleIndexPlan *plan, uint32_t begin,
                                 uint32_t end)
{
    if (begin >= end) return true;
    for (size_t i = 0; i < plan->count; i++) {
        if (plan->ranges[i].begin == begin && plan->ranges[i].end == end) {
            return true;
        }
    }
    if (plan->count >= STYLE_RULE_INDEX_MAX_SOURCES) return false;
    plan->ranges[plan->count++] = (StyleRuleIndexRange) {
        .begin = begin,
        .end = end
    };
    return true;
}

/* A bucket's candidates: its unscoped rules, and its shadow-scoped ones
   (a separately sorted range) unless the subject is outside every
   adopting carrier. */
static bool style_rule_add_key_source(const Stylesheet *sheet,
                                      StyleRuleIndexPlan *plan,
                                      SelectorType type, const char *text,
                                      size_t length, PseudoElement pseudo,
                                      bool outside_scopes)
{
    if (text == NULL || length == 0) return true;
    StyleRuleIndexBucket *bucket = style_rule_find_bucket(
        sheet, type, text, length, false, pseudo);
    if (bucket == NULL) return true;
    if (!style_rule_add_range(plan, bucket->first,
                              bucket->first + bucket->unscoped)) return false;
    return outside_scopes || style_rule_add_range(
        plan, bucket->first + bucket->unscoped,
        bucket->first + bucket->count);
}

static StyleMatchedRangeCacheEntry *style_matched_range_slot(
    const Stylesheet *sheet, const lxb_dom_node_t *node,
    PseudoElement pseudo, size_t start, size_t end)
{
    if (pseudo == PSEUDO_NONE) return NULL;
    StyleAncestorBloomCache *cache = sheet->resolve_scratch == NULL
        ? NULL : sheet->resolve_scratch->ancestor_bloom_cache;
    /* Container geometry changes during a build, unlike selector subjects. */
    if (cache == NULL || stylesheet_has_container_queries(sheet)
        || sheet->selector_cooperate_cancelled || end > UINT32_MAX) return NULL;
    size_t slot = (((uintptr_t) node >> 4) ^ (start * 97u)
        ^ ((unsigned) pseudo * 193u)) & (STYLE_MATCHED_RANGE_CACHE_CAPACITY - 1u);
    return &cache->matched_ranges[slot];
}

static bool style_matched_range_current(const StyleMatchedRangeCacheEntry *entry,
    const lxb_dom_node_t *node, PseudoElement pseudo, size_t start, size_t end)
{
    return entry != NULL && entry->node == node && entry->start == start
        && entry->end == end && entry->pseudo == pseudo;
}

/* A declaration a ::before/::after rule applies can create its box. */
static bool style_declaration_may_generate(const StyleDeclaration *declaration)
{
    return declaration != NULL
        && ((declaration->inherit_mask & S_CONTENT) != 0
            || ((declaration->mask & S_CONTENT) != 0
                && (declaration->values_flags
                    & STYLE_DECLARATION_VALUES_GENERATED_CONTENT) != 0)
            || declaration->deferred_declarations != NULL);
}

bool style_pseudo_may_generate(const Stylesheet *sheet, PseudoElement pseudo)
{
    if (sheet == NULL || (pseudo != PSEUDO_BEFORE && pseudo != PSEUDO_AFTER))
        return true;
    Stylesheet *mutable_sheet = (Stylesheet *) sheet;
    if (!sheet->pseudo_content_ready
        || sheet->pseudo_content_generation != sheet->build_generation
        || sheet->pseudo_content_rules != sheet->count) {
        uint8_t bits = 0;
        for (size_t i = 0; i < sheet->count; i++) {
            const StyleRule *rule = &sheet->rules[i];
            if ((rule->pseudo == PSEUDO_BEFORE || rule->pseudo == PSEUDO_AFTER)
                && style_declaration_may_generate(
                       stylesheet_rule_declaration(sheet, rule)))
                bits |= (uint8_t) (1u << rule->pseudo);
        }
        mutable_sheet->pseudo_content_bits = bits;
        mutable_sheet->pseudo_content_generation = sheet->build_generation;
        mutable_sheet->pseudo_content_rules = sheet->count;
        mutable_sheet->pseudo_content_ready = true;
    }
    return (sheet->pseudo_content_bits & (1u << pseudo)) != 0;
}

bool style_pseudo_known_absent(const Stylesheet *sheet,
    const lxb_dom_node_t *node, PseudoElement pseudo)
{
    if (sheet == NULL || node == NULL || !sheet->rule_index_ready
        || sheet->resolve_scratch == NULL
        || sheet->resolve_scratch->ancestor_bloom_cache == NULL
        || (pseudo != PSEUDO_BEFORE && pseudo != PSEUDO_AFTER)) return false;
    if (stylesheet_has_container_queries(sheet) || sheet->selector_cooperate_cancelled)
        return false;
    for (size_t phase = 0; phase < 4; phase++) {
        size_t start = sheet->cascade_starts[phase], end = sheet->cascade_ends[phase];
        if (start >= end) continue;
        const StyleMatchedRangeCacheEntry *entry =
            style_matched_range_slot(sheet, node, pseudo, start, end);
        if (!style_matched_range_current(entry, node, pseudo, start, end)) return false;
        for (size_t i = 0; i < entry->count; i++) {
            if (style_declaration_may_generate(stylesheet_rule_declaration(
                    sheet, &sheet->rules[entry->rules[i]]))) return false;
        }
    }
    return true;
}

/* Every rule index of the subject's class buckets, ascending and unique,
   into `list`; false when they do not fit. */
static bool style_rule_gather_class_candidates(
    const Stylesheet *sheet, const StyleMatchSubject *subject,
    PseudoElement pseudo, bool outside_scopes, uint32_t *list,
    size_t capacity, size_t *count)
{
    size_t used = 0;
    size_t at = 0;
    while (subject->classes != NULL && at < subject->classes_length) {
        while (at < subject->classes_length
               && isspace((unsigned char) subject->classes[at])) at++;
        size_t begin = at;
        while (at < subject->classes_length
               && !isspace((unsigned char) subject->classes[at])) at++;
        if (at == begin) continue;
        StyleRuleIndexBucket *bucket = style_rule_find_bucket(
            sheet, SELECTOR_CLASS, subject->classes + begin, at - begin,
            false, pseudo);
        if (bucket == NULL) continue;
        uint32_t end = bucket->first
            + (outside_scopes ? bucket->unscoped : bucket->count);
        for (uint32_t entry = bucket->first; entry < end; entry++) {
            uint32_t index = sheet->rule_index_entries[entry];
            /* Insertion sort: a bucket is usually one rule here. */
            size_t slot = used;
            while (slot != 0 && list[slot - 1] > index) slot--;
            if (slot != 0 && list[slot - 1] == index) continue;
            if (used == capacity) return false;
            memmove(list + slot + 1, list + slot,
                    (used - slot) * sizeof(*list));
            list[slot] = index;
            used++;
        }
    }
    *count = used;
    return true;
}

static StyleRuleIndexPlan style_rule_index_plan(
    const Stylesheet *sheet, const StyleMatchSubject *subject,
    PseudoElement pseudo, uint32_t *list, size_t list_capacity)
{
    StyleRuleIndexPlan plan = {0};
    bool retained = pseudo != PSEUDO_NONE;
    for (size_t phase = 0; retained && phase < 4; phase++) {
        size_t start = sheet->cascade_starts[phase], end = sheet->cascade_ends[phase];
        if (start < end && !style_matched_range_current(
                style_matched_range_slot(sheet, subject->node, pseudo, start, end),
                subject->node, pseudo, start, end)) {
            retained = false;
            break;
        }
    }
    /* A cache hit needs neither bucket merging nor bloom preparation. If a
       nested resolution evicts a range later, the unready plan safely falls
       back to the ordinary bounded rule scan. */
    if (retained) return plan;
    if (!sheet->rule_index_ready || sheet->rule_index_entries == NULL
        || (unsigned) pseudo >= PSEUDO_COUNT) {
        return plan;
    }
    /* Shadow-scoped rules are left out for an element outside every
       adopting carrier: the matcher would refuse each of them anyway. */
    bool outside = style_subject_outside_adopted_scopes(sheet, subject);
    uint32_t universal_begin = pseudo == PSEUDO_NONE ? 0
        : sheet->rule_index_universal_ends[pseudo - 1];
    uint32_t universal_unscoped =
        sheet->rule_index_universal_unscoped_ends[pseudo];
    if (!style_rule_add_range(&plan, universal_begin, universal_unscoped)
        || (!outside
            && !style_rule_add_range(
                   &plan, universal_unscoped,
                   sheet->rule_index_universal_ends[pseudo]))
        || !style_rule_add_key_source(
            sheet, &plan, SELECTOR_TAG, subject->tag, subject->tag_length,
            pseudo, outside)
        || !style_rule_add_key_source(
            sheet, &plan, SELECTOR_ID, subject->id,
            subject->id_length, pseudo, outside))
        return (StyleRuleIndexPlan) {0};
    size_t keyed_count = plan.count;
    size_t at = 0;
    while (subject->classes != NULL && at < subject->classes_length) {
        while (at < subject->classes_length
               && isspace((unsigned char) subject->classes[at])) at++;
        size_t begin = at;
        while (at < subject->classes_length
               && !isspace((unsigned char) subject->classes[at])) at++;
        if (at != begin && !style_rule_add_key_source(
                sheet, &plan, SELECTOR_CLASS, subject->classes + begin,
                at - begin, pseudo, outside)) {
            /* Too many class buckets for `ranges`: merge them all into the
               caller's list, keeping the universal/tag/id ranges. */
            size_t listed = 0;
            if (list == NULL || !style_rule_gather_class_candidates(
                    sheet, subject, pseudo, outside, list, list_capacity,
                    &listed))
                return (StyleRuleIndexPlan) {0};
            plan.count = keyed_count;
            plan.list = list;
            plan.list_count = listed;
            break;
        }
    }
    if (plan.count == 0 && plan.list_count == 0) {
        /* Negative answers are common for pseudos. Preserve the empty rule
           set too, while still constructing inherited values on each call. */
        for (size_t phase = 0; phase < 4; phase++) {
            size_t start = sheet->cascade_starts[phase];
            size_t end = sheet->cascade_ends[phase];
            StyleMatchedRangeCacheEntry *entry =
                style_matched_range_slot(sheet, subject->node, pseudo, start, end);
            if (start < end && entry != NULL) {
                *entry = (StyleMatchedRangeCacheEntry) {
                    .node = subject->node, .start = (uint32_t) start,
                    .end = (uint32_t) end, .pseudo = (uint8_t) pseudo
                };
            }
        }
        plan.ready = true;
        return plan;
    }
    plan.compound_bloom = style_match_subject_bloom(subject);
    size_t candidate_upper_bound = plan.list_count;
    for (size_t i = 0; i < plan.count; i++) {
        size_t range_count = plan.ranges[i].end - plan.ranges[i].begin;
        if (range_count > SIZE_MAX - candidate_upper_bound) {
            candidate_upper_bound = SIZE_MAX;
            break;
        }
        candidate_upper_bound += range_count;
    }
    /* Walking the ancestor chain costs more than it saves for a sparse
       bucket on a large stylesheet. Small sheets remain enabled because the
       exact-selector work and the walk are both tightly bounded. This gate
       depends only on representation density, never on a site or selector. */
    if (sheet->rule_ancestor_filter_active
        && ((sheet->resolve_scratch != NULL
             && sheet->resolve_scratch->ancestor_bloom_cache != NULL)
            || sheet->count <= STYLE_RULE_ANCESTOR_FILTER_SMALL_SHEET
            || candidate_upper_bound
                >= STYLE_RULE_ANCESTOR_FILTER_MIN_CANDIDATES)) {
        plan.ancestor_bloom = style_match_ancestor_bloom(sheet, subject);
        plan.ancestor_bloom_ready = true;
    }
    plan.ready = true;
    return plan;
}

static void style_trace_position_match(const Stylesheet *sheet,
                                       const StyleRule *rule,
                                       lxb_dom_node_t *node)
{
    const StyleDeclaration *declaration = stylesheet_rule_declaration(
        sheet, rule);
    if (!STYLE_TRACE(sheet, LAYOUT)
        || declaration == NULL
        || (declaration->mask & S_POSITION) == 0) return;
    ComputedStyle values;
    stylesheet_declaration_values(sheet, declaration, &values);
    size_t class_length = 0;
    const char *class_name = document_attribute(node, "class",
                                                &class_length);
    if (class_name == NULL
        || strstr(class_name, "wm-fallback-layout") == NULL) return;
    fprintf(stderr,
            "style-position-match selector=%s fixed=%d out=%d relative=%d\n",
            rule->selector, values.fixed_position, values.out_of_flow,
            values.relative_position);
}

static void style_retained_note(const Stylesheet *sheet, size_t rule_index)
{
    StyleResolveScratch *scratch = sheet->resolve_scratch;
    if (scratch == NULL || !scratch->retained_pending_valid) return;
    if (scratch->retained_pending_count >= STYLE_RETAINED_MATCH_RULE_LIMIT
        || rule_index > UINT16_MAX) {
        scratch->retained_pending_valid = false;
        return;
    }
    scratch->retained_pending_rules[scratch->retained_pending_count++] =
        (uint16_t) rule_index;
}

typedef struct {
    uint32_t rules[128];
    uint64_t orders[128];
    size_t count;
    bool overflow;
    StyleAdoptedOrderContext adopted;
} StyleCascadeMatches;

static void style_apply_or_collect(const Stylesheet *sheet,
    const StyleRule *rule, ComputedStyle *style, const ComputedStyle *parent,
    lxb_dom_node_t *node, StyleCascadeMatches *matches)
{
    if (matches == NULL) {
        apply_style_rule(sheet, rule, style, parent);
    } else if (matches->count < 128u) {
        size_t at = matches->count++;
        matches->rules[at] = (uint32_t) (rule - sheet->rules);
        matches->orders[at] = style_adopted_cascade_order_in(
            sheet, node, &matches->adopted, rule->order);
    } else matches->overflow = true;
}

static void style_apply_matching_ordered_range(const Stylesheet *sheet,
                                       const ComputedStyle *parent,
                                       lxb_dom_node_t *node,
                                       const StyleMatchSubject *subject,
                                       const StyleRuleIndexPlan *plan,
                                       PseudoElement pseudo,
                                       size_t start, size_t end,
                                       ComputedStyle *style,
                                       bool trace_position,
                                       const StyleRetainedMatchEntry *retained_entry,
                                       bool record,
                                       StyleCascadeMatches *collected)
{
    if (sheet == NULL || start >= end) return;
    if (retained_entry != NULL) {
        /* Selector answers are exact under the same DOM/cascade. Container
           admission is not retained: geometry may change between passes. */
        for (size_t i = 0; i < retained_entry->count; i++) {
            size_t index = retained_entry->rules[i];
            if (index < start || index >= end || index >= sheet->count) continue;
            const StyleRule *rule = &sheet->rules[index];
            if (!style_container_query_matches(
                    sheet, style_rule_container_query(rule), node)) continue;
            if (trace_position) style_trace_position_match(sheet, rule, node);
            style_apply_or_collect(sheet, rule, style, parent, node, collected);
        }
        return;
    }
    if (plan != NULL && plan->ready && plan->count == 0
        && plan->list_count == 0) return;
    /* Diagnostics counters only; the match result never depends on them. */
    Stylesheet *mutable_sheet = (Stylesheet *) sheet;
    /* Container geometry can change during a build. Do not memoize its
       admission, or publish partial results after a cooperative cancel. */
    StyleMatchedRangeCacheEntry *retained =
        style_matched_range_slot(sheet, node, pseudo, start, end);
    StyleMatchedRangeCacheEntry matches = {0};
    if (retained != NULL) {
        if (style_matched_range_current(retained, node, pseudo, start, end)) {
            matches = *retained;
            for (size_t i = 0; i < matches.count; i++) {
                const StyleRule *rule = &sheet->rules[matches.rules[i]];
                if (trace_position) style_trace_position_match(sheet, rule, node);
                style_apply_or_collect(sheet, rule, style, parent, node, collected);
                if (record) style_retained_note(sheet, matches.rules[i]);
            }
            return;
        }
        matches.node = node;
        matches.start = (uint32_t) start;
        matches.end = (uint32_t) end;
        matches.pseudo = (uint8_t) pseudo;
    }
    mutable_sheet->rule_index_queries++;
    TILEFINCH_WORK_ADD(style_rule_queries, 1);
    StyleRuleIndexSource sources[STYLE_RULE_INDEX_MAX_SOURCES + 1u];
    size_t source_count = plan != NULL && plan->ready
        ? plan->count + (plan->list_count != 0) : 0;
    if (source_count == 0) {
        mutable_sheet->rule_index_fallbacks++;
        mutable_sheet->rule_index_candidates += end - start;
        TILEFINCH_WORK_ADD(style_rule_candidates, end - start);
        for (size_t i = start; i < end; i++) {
            const StyleRule *rule = &sheet->rules[i];
            if (rule->pseudo != pseudo) continue;
            if (record) {
                if (!rule_fast_matches(sheet, rule, subject)
                    || !style_rule_selector_matches_subject(
                        sheet, i, node, subject)) continue;
                /* Keep inactive conditions too: a later resize can activate
                   this rule without changing its selector answer. */
                style_retained_note(sheet, i);
                if (!style_container_query_matches(
                        sheet, style_rule_container_query(rule), node)) continue;
            } else if (!rule_matches(sheet, i, rule, node, subject)) continue;
            if (trace_position) style_trace_position_match(sheet, rule, node);
            style_apply_or_collect(sheet, rule, style, parent, node, collected);
            if (retained != NULL) {
                if (matches.count < STYLE_MATCHED_RANGE_RULE_LIMIT)
                    matches.rules[matches.count++] = (uint32_t) i;
                else retained = NULL;
            }
        }
        if (retained != NULL && !sheet->selector_cooperate_cancelled)
            *retained = matches;
        return;
    }
    for (size_t i = 0; i < plan->count; i++) {
        sources[i] = (StyleRuleIndexSource) {
            .entries = sheet->rule_index_entries,
            .at = style_rule_range_lower_bound(
                sheet->rule_index_entries, plan->ranges[i].begin,
                plan->ranges[i].end, start),
            .end = plan->ranges[i].end
        };
    }
    if (plan->list_count != 0) {
        sources[plan->count] = (StyleRuleIndexSource) {
            .entries = plan->list,
            .at = style_rule_range_lower_bound(
                plan->list, 0, (uint32_t) plan->list_count, start),
            .end = (uint32_t) plan->list_count
        };
    }

    uint32_t previous = STYLE_RULE_INDEX_EMPTY;
    uint64_t ancestor_tokens = 0;
    bool ancestor_tokens_ready = false;
    for (;;) {
        uint32_t candidate = STYLE_RULE_INDEX_EMPTY;
        for (size_t i = 0; i < source_count; i++) {
            if (sources[i].at >= sources[i].end) continue;
            uint32_t index = sources[i].entries[sources[i].at];
            if ((size_t) index >= end) continue;
            if (candidate == STYLE_RULE_INDEX_EMPTY || index < candidate) {
                candidate = index;
            }
        }
        if (candidate == STYLE_RULE_INDEX_EMPTY) break;
        for (size_t i = 0; i < source_count; i++) {
            while (sources[i].at < sources[i].end
                   && sources[i].entries[sources[i].at] == candidate) {
                sources[i].at++;
            }
        }
        if (candidate == previous || (size_t) candidate < start
            || (size_t) candidate >= end || candidate >= sheet->count) {
            previous = candidate;
            continue;
        }
        previous = candidate;
        mutable_sheet->rule_index_candidates++;
        TILEFINCH_WORK_ADD(style_rule_candidates, 1);
        if (sheet->rule_filters != NULL
            && style_token_bloom_missing(
                sheet->rule_filters[candidate].compound,
                plan->compound_bloom)) {
#ifndef TILEFINCH_NO_TRACE
            mutable_sheet->rule_compound_filter_rejections++;
#endif
            continue;
        }
        if ((plan->ancestor_bloom_ready
             && style_token_bloom_missing(
                 sheet->rule_filters[candidate].ancestors,
                 plan->ancestor_bloom))
            || !style_candidate_ancestor_token_possible(
                   sheet, candidate, node, &ancestor_tokens,
                   &ancestor_tokens_ready)) {
#ifndef TILEFINCH_NO_TRACE
            mutable_sheet->rule_ancestor_filter_rejections++;
#endif
            continue;
        }
        const StyleRule *rule = &sheet->rules[candidate];
        /* Every indexed rule came either from the universal range or from a
           tag/id/class bucket already selected for this subject. Repeating
           rule_fast_matches here only rescans the element's class list. */
        if (rule->pseudo != pseudo) continue;
        if (record) {
            if (!style_rule_selector_matches_subject(
                    sheet, candidate, node, subject)) continue;
            style_retained_note(sheet, candidate);
            if (!style_container_query_matches(
                    sheet, style_rule_container_query(rule), node)) continue;
        } else if (!style_rule_selector_matches_subject(
                sheet, candidate, node, subject)
            || !style_container_query_matches(
                sheet, style_rule_container_query(rule), node)) continue;
        if (trace_position) style_trace_position_match(sheet, rule, node);
        style_apply_or_collect(sheet, rule, style, parent, node, collected);
        if (retained != NULL) {
            if (matches.count < STYLE_MATCHED_RANGE_RULE_LIMIT)
                matches.rules[matches.count++] = candidate;
            else retained = NULL;
        }
    }
    if (retained != NULL && !sheet->selector_cooperate_cancelled)
        *retained = matches;
}

/* Rules are stored once, globally sorted. Only roots whose list differs
   need this path: reorder contiguous source spans inside equal-priority
   groups, never across specificity, origin, importance or layer. The
   existing matcher/retained lists still visit each candidate once. */
static bool style_rules_same_priority(const StyleRule *a, const StyleRule *b)
{
    return a->origin == b->origin && a->important == b->important
        && a->specificity == b->specificity
        && (a->layer == b->layer
            || (a->layer != UINT_MAX && b->layer != UINT_MAX
                && (a->layer >> 8) == (b->layer >> 8)));
}

static void style_apply_matching_range(const Stylesheet *sheet,
                                       const ComputedStyle *parent,
                                       lxb_dom_node_t *node,
                                       const StyleMatchSubject *subject,
                                       const StyleRuleIndexPlan *plan,
                                       PseudoElement pseudo,
                                       size_t start, size_t end,
                                       ComputedStyle *style,
                                       bool trace_position,
                                       const StyleRetainedMatchEntry *retained_entry,
                                       bool record)
{
    if (sheet == NULL || !sheet->scopes.adopted_order_varies) {
        style_apply_matching_ordered_range(sheet, parent, node, subject, plan,
            pseudo, start, end, style, trace_position, retained_entry, record, NULL);
        return;
    }
    StyleCascadeMatches collected = {0};
    style_apply_matching_ordered_range(sheet, parent, node, subject, plan,
        pseudo, start, end, style, trace_position, retained_entry, record,
        &collected);
    if (!collected.overflow) {
        /* Sort only actual matches, preserving the index and retained fast
           paths. Equal-priority matches use this root's sheet order. */
        for (size_t i = 1; i < collected.count; i++) {
            size_t j = i;
            while (j != 0 && style_rules_same_priority(
                    &sheet->rules[collected.rules[j]],
                    &sheet->rules[collected.rules[j - 1u]])
                && collected.orders[j] < collected.orders[j - 1u]) {
                uint32_t rule = collected.rules[j];
                uint64_t order = collected.orders[j];
                collected.rules[j] = collected.rules[j - 1u];
                collected.orders[j] = collected.orders[j - 1u];
                collected.rules[j - 1u] = rule;
                collected.orders[j - 1u] = order;
                j--;
            }
        }
        for (size_t i = 0; i < collected.count; i++)
            apply_style_rule(sheet, &sheet->rules[collected.rules[i]], style, parent);
        return;
    }
    /* An exceptionally dense match set uses bounded source spans instead.
       The collection pass applied nothing and recorded each match once. */
    record = false;
    while (start < end) {
        const StyleRule *first = &sheet->rules[start];
        size_t stop = start + 1u;
        while (stop < end) {
            const StyleRule *next = &sheet->rules[stop];
            if (!style_rules_same_priority(next, first)) break;
            stop++;
        }
        struct { size_t begin, end; uint64_t order; } spans[65];
        size_t count = 0;
        bool overflow = false;
        for (size_t at = start; at < stop;) {
            uint64_t source = style_adopted_source_bit(sheet,
                                                       sheet->rules[at].order);
            size_t next = at + 1u;
            while (next < stop && source == style_adopted_source_bit(
                    sheet, sheet->rules[next].order)) next++;
            if (count == sizeof(spans) / sizeof(spans[0])) {
                overflow = true;
                break;
            }
            spans[count].begin = at;
            spans[count].end = next;
            spans[count].order = style_adopted_cascade_order(
                sheet, node, sheet->rules[at].order);
            count++;
            at = next;
        }
        /* Valid sorted groups fit the source bound. If an inconsistent
           order ever violates it, apply this whole group in global order;
           no rule has been applied from this group yet. */
        if (overflow) {
            style_apply_matching_ordered_range(sheet, parent, node, subject,
                plan, pseudo, start, stop, style, trace_position,
                retained_entry, record, NULL);
            start = stop;
            continue;
        }
        for (size_t i = 1; i < count; i++) {
            size_t j = i;
            while (j != 0 && spans[j].order < spans[j - 1u].order) {
                size_t begin = spans[j].begin, finish = spans[j].end;
                uint64_t order = spans[j].order;
                spans[j] = spans[j - 1u];
                spans[j - 1u].begin = begin;
                spans[j - 1u].end = finish;
                spans[j - 1u].order = order;
                j--;
            }
        }
        for (size_t i = 0; i < count; i++)
            style_apply_matching_ordered_range(sheet, parent, node, subject,
                plan, pseudo, spans[i].begin, spans[i].end, style,
                trace_position, retained_entry, record, NULL);
        start = stop;
    }
}

static void apply_paint_values(Stylesheet *sheet, ComputedStyle *style,
                               const ComputedStyle *values,
                               uint64_t mask, uint64_t mask_high)
{
    uint64_t paint_high = S2_BACKGROUND_LAYERS | S2_BACKGROUND_BOX
        | S2_BACKGROUND_POSITION | S2_BACKGROUND_REPEAT
        | S2_MASK_POSITION | S2_MASK_REPEAT | S2_MASK_SIZE
        | S2_BOX_SHADOW | S2_FILTER;
    if ((mask & (S_BACKGROUND_IMAGE | S_MASK_IMAGE | S_BACKGROUND_SIZE
                 | S_TRANSFORM)) == 0
        && (mask_high & paint_high) == 0) {
        return;
    }
    const StylePaintStack *current = stylesheet_paint_stack(
        sheet, computed_style_paint_stack_id(style));
    const StylePaintStack *incoming = stylesheet_paint_stack(
        sheet, computed_style_paint_stack_id(values));
    StylePaintStack compact;
    if (incoming == NULL) {
        if ((mask & S_MASK_IMAGE) != 0) style->mask_image = NULL;
        if ((mask & (S_BACKGROUND_IMAGE | S_TRANSFORM)) == 0
            && (mask_high & (S2_BOX_SHADOW | S2_FILTER
                             | S2_BACKGROUND_REPEAT)) == 0
            && current == NULL) return;
        compact = style_paint_stack_copy(sheet, values);
        /* Zero is also a complete initial geometry value (0% 0% / auto). */
        if (compact.background_count == 0) compact.background_count = 1;
        incoming = &compact;
    }
    StylePaintStack merged = style_paint_stack_copy(sheet, style);
    if ((mask & S_TRANSFORM) != 0) {
        merged.skew_x_q10 = incoming == NULL ? 0 : incoming->skew_x_q10;
        merged.rotation_q6 = incoming == NULL ? 0 : incoming->rotation_q6;
        merged.components = (uint8_t) (
            (merged.components & ~STYLE_PAINT_COMPONENT_SKEW)
            | (incoming == NULL ? 0
               : incoming->components & STYLE_PAINT_COMPONENT_SKEW));
    }
    if ((mask_high & S2_FILTER) != 0) {
        merged.reserved = (uint8_t) (
            (merged.reserved & ~STYLE_PAINT_FILTER_LOW_AMOUNT)
            | (incoming == NULL ? 0
               : incoming->reserved & STYLE_PAINT_FILTER_LOW_AMOUNT));
    }
    if ((mask_high & S2_BACKGROUND_LAYERS) != 0
        || (mask & S_BACKGROUND_IMAGE) != 0) {
        if (incoming != NULL
            && (incoming->components
                & STYLE_PAINT_COMPONENT_BACKGROUND_IMAGE) != 0) {
            /* background-image is an independent longhand.  Replacing its
               comma-separated list must not replace background-clip,
               origin, position, size, or repeat values won by other rules.
               Repeat the existing geometry list to the new image count as
               CSS list matching requires, then copy only image payloads.
               When no earlier layer exists, the parser's layer supplies the
               initial padding-box/border-box defaults. */
            size_t old_count = merged.background_count;
            size_t new_count = incoming->background_count;
            for (size_t i = 0; i < new_count
                               && i < STYLE_PAINT_LAYER_LIMIT; i++) {
                StylePaintLayer layer = old_count != 0
                    ? merged.backgrounds[i % old_count]
                    : incoming->backgrounds[i];
                layer.image = incoming->backgrounds[i].image;
                layer.gradient = incoming->backgrounds[i].gradient;
                layer.kind = incoming->backgrounds[i].kind;
                merged.backgrounds[i] = layer;
            }
            for (size_t i = new_count; i < STYLE_PAINT_LAYER_LIMIT; i++) {
                merged.backgrounds[i] = (StylePaintLayer) {0};
            }
            merged.background_count = (uint8_t) new_count;
            merged.components |= STYLE_PAINT_COMPONENT_BACKGROUND_IMAGE;
        } else {
            for (size_t i = 0; i < STYLE_PAINT_LAYER_LIMIT; i++) {
                merged.backgrounds[i].image = NULL;
                memset(&merged.backgrounds[i].gradient, 0,
                       sizeof(merged.backgrounds[i].gradient));
                merged.backgrounds[i].kind = STYLE_PAINT_IMAGE_NONE;
            }
            merged.components &= (uint8_t)
                ~STYLE_PAINT_COMPONENT_BACKGROUND_IMAGE;
        }
    }
    if (incoming != NULL && (mask_high & S2_BACKGROUND_BOX) != 0) {
        if ((incoming->components
             & STYLE_PAINT_COMPONENT_BACKGROUND_BOX) != 0) {
            size_t count = incoming->background_count;
            if (count == 0) count = 1;
            if (merged.background_count < count) {
                merged.background_count = (uint8_t) count;
            }
            for (size_t i = 0; i < count
                               && i < STYLE_PAINT_LAYER_LIMIT; i++) {
                merged.backgrounds[i].origin =
                    incoming->backgrounds[i].origin;
                merged.backgrounds[i].clip =
                    incoming->backgrounds[i].clip;
            }
            merged.components |= STYLE_PAINT_COMPONENT_BACKGROUND_BOX;
        }
        if ((incoming->components
             & STYLE_PAINT_COMPONENT_TABLE_SPACING) != 0) {
            merged.table_spacing_x = incoming->table_spacing_x;
            merged.table_spacing_y = incoming->table_spacing_y;
            merged.components |= STYLE_PAINT_COMPONENT_TABLE_SPACING;
        }
        if ((incoming->components
             & STYLE_PAINT_COMPONENT_TRANSFORM_ORIGIN) != 0) {
            merged.transform_origin_x = incoming->transform_origin_x;
            merged.transform_origin_y = incoming->transform_origin_y;
            merged.components |= STYLE_PAINT_COMPONENT_TRANSFORM_ORIGIN;
        }
        if ((incoming->components
             & STYLE_PAINT_COMPONENT_TEXT_SHADOW) != 0) {
            memcpy(merged.text_shadows, incoming->text_shadows,
                   sizeof(merged.text_shadows));
            merged.text_shadow_count = incoming->text_shadow_count;
            merged.components |= STYLE_PAINT_COMPONENT_TEXT_SHADOW;
        }
    }
    if (incoming != NULL && (mask_high & S2_BACKGROUND_POSITION) != 0) {
        size_t count = incoming->background_count;
        if (count == 0) count = 1;
        if (merged.background_count < count) {
            merged.background_count = (uint8_t) count;
        }
        for (size_t i = 0; i < merged.background_count
                           && i < STYLE_PAINT_LAYER_LIMIT; i++) {
            const StylePaintLayer *source = &incoming->backgrounds[i % count];
            merged.backgrounds[i].position_x =
                source->position_x;
            merged.backgrounds[i].position_y =
                source->position_y;
            merged.backgrounds[i].position_edges =
                source->position_edges;
            merged.backgrounds[i].flags = (uint8_t) (
                (merged.backgrounds[i].flags
                 & ~STYLE_BACKGROUND_POSITION_PIXELS)
                | (source->flags
                   & STYLE_BACKGROUND_POSITION_PIXELS));
        }
    }
    if ((mask_high & S2_BACKGROUND_REPEAT) != 0) {
        for (size_t i = 0; i < merged.background_count
                           && i < STYLE_PAINT_LAYER_LIMIT; i++) {
            /* The legacy repeat longhand has no retained layer of its own. */
            uint8_t flags = values->background_size_flags;
            if (incoming != NULL && incoming->background_count != 0) {
                flags = incoming->backgrounds[i % incoming->background_count].flags;
            }
            merged.backgrounds[i].flags = (uint8_t) (
                (merged.backgrounds[i].flags
                 & ~(STYLE_BACKGROUND_NO_REPEAT_X | STYLE_BACKGROUND_NO_REPEAT_Y))
                | (flags & (STYLE_BACKGROUND_NO_REPEAT_X | STYLE_BACKGROUND_NO_REPEAT_Y)));
        }
    }
    if (incoming != NULL && (mask & S_BACKGROUND_SIZE) != 0) {
        size_t count = incoming->background_count;
        if (count == 0) count = 1;
        if (merged.background_count < count) {
            merged.background_count = (uint8_t) count;
        }
        for (size_t i = 0; i < merged.background_count
                           && i < STYLE_PAINT_LAYER_LIMIT; i++) {
            const StylePaintLayer *source = &incoming->backgrounds[i % count];
            merged.backgrounds[i].width =
                source->width;
            merged.backgrounds[i].height =
                source->height;
            merged.backgrounds[i].fit =
                source->fit;
            merged.backgrounds[i].flags = (uint8_t) (
                (merged.backgrounds[i].flags
                 & ~(STYLE_BACKGROUND_SIZE_EXPLICIT
                     | STYLE_BACKGROUND_WIDTH_AUTO
                     | STYLE_BACKGROUND_HEIGHT_AUTO
                     | STYLE_BACKGROUND_WIDTH_PERCENT
                     | STYLE_BACKGROUND_HEIGHT_PERCENT))
                | (source->flags
                   & (STYLE_BACKGROUND_SIZE_EXPLICIT
                      | STYLE_BACKGROUND_WIDTH_AUTO
                      | STYLE_BACKGROUND_HEIGHT_AUTO
                      | STYLE_BACKGROUND_WIDTH_PERCENT
                      | STYLE_BACKGROUND_HEIGHT_PERCENT)));
        }
    }
    if ((mask & S_MASK_IMAGE) != 0) {
        size_t count = incoming == NULL ? 0 : incoming->mask_count;
        /* mask-image replaces its whole comma-separated list. Keeping the
           previous longer count would leave stale trailing masks active
           after `mask-image:none` or any shorter winning declaration. */
        merged.mask_count = (uint8_t) count;
        for (size_t i = 0; i < count
                           && i < STYLE_PAINT_LAYER_LIMIT; i++) {
            merged.masks[i].image = incoming->masks[i].image;
            merged.masks[i].gradient = incoming->masks[i].gradient;
            merged.masks[i].kind = incoming->masks[i].kind;
        }
        for (size_t i = count; i < STYLE_PAINT_LAYER_LIMIT; i++) {
            merged.masks[i] = (StylePaintLayer) {0};
        }
        merged.components |= STYLE_PAINT_COMPONENT_MASK_IMAGE;
        style->mask_image = incoming != NULL && incoming->mask_count != 0
            && incoming->masks[0].kind == STYLE_PAINT_IMAGE_URL
            ? incoming->masks[0].image : NULL;
    }
    for (size_t i = 0; incoming != NULL && i < incoming->mask_count
                       && i < STYLE_PAINT_LAYER_LIMIT; i++) {
        if ((mask_high & S2_MASK_SIZE) != 0) {
            merged.masks[i].width = incoming->masks[i].width;
            merged.masks[i].height = incoming->masks[i].height;
            merged.masks[i].fit = incoming->masks[i].fit;
            merged.masks[i].flags = (uint8_t) (
                (merged.masks[i].flags
                 & ~(STYLE_BACKGROUND_SIZE_EXPLICIT
                     | STYLE_BACKGROUND_WIDTH_AUTO
                     | STYLE_BACKGROUND_HEIGHT_AUTO
                     | STYLE_BACKGROUND_WIDTH_PERCENT
                     | STYLE_BACKGROUND_HEIGHT_PERCENT))
                | (incoming->masks[i].flags
                   & (STYLE_BACKGROUND_SIZE_EXPLICIT
                      | STYLE_BACKGROUND_WIDTH_AUTO
                      | STYLE_BACKGROUND_HEIGHT_AUTO
                      | STYLE_BACKGROUND_WIDTH_PERCENT
                      | STYLE_BACKGROUND_HEIGHT_PERCENT)));
        }
        if ((mask_high & S2_MASK_POSITION) != 0) {
            merged.masks[i].position_x = incoming->masks[i].position_x;
            merged.masks[i].position_y = incoming->masks[i].position_y;
            merged.masks[i].position_edges =
                incoming->masks[i].position_edges;
            merged.masks[i].flags = (uint8_t) (
                (merged.masks[i].flags
                 & ~STYLE_BACKGROUND_POSITION_PIXELS)
                | (incoming->masks[i].flags
                   & STYLE_BACKGROUND_POSITION_PIXELS));
        }
        if ((mask_high & S2_MASK_REPEAT) != 0) {
            merged.masks[i].flags = (uint8_t) (
                (merged.masks[i].flags
                 & ~(STYLE_BACKGROUND_NO_REPEAT_X
                     | STYLE_BACKGROUND_NO_REPEAT_Y))
                | (incoming->masks[i].flags
                   & (STYLE_BACKGROUND_NO_REPEAT_X
                      | STYLE_BACKGROUND_NO_REPEAT_Y)));
        }
    }
    if ((mask_high & S2_BOX_SHADOW) != 0) {
        memset(merged.box_shadows, 0, sizeof(merged.box_shadows));
        merged.box_shadow_count = 0;
        if (incoming != NULL
            && (incoming->components
                & STYLE_PAINT_COMPONENT_BOX_SHADOW) != 0) {
            memcpy(merged.box_shadows, incoming->box_shadows,
                   sizeof(merged.box_shadows));
            merged.box_shadow_count = incoming->box_shadow_count;
        }
        merged.components |= STYLE_PAINT_COMPONENT_BOX_SHADOW;
    }
    (void) style_apply_paint_stack(sheet, style, &merged, NULL);
}

static void apply_values(Stylesheet *sheet, ComputedStyle *style,
                         const ComputedStyle *values,
                         uint64_t mask, uint64_t mask_high,
                         const ComputedStyle *parent)
{
    apply_paint_values(sheet, style, values, mask, mask_high);
    if (mask & S_DISPLAY) {
        style->display = values->display;
        style->list_item = values->list_item;
    }
    if (mask & S_COLOR) {
        style->color = values->color;
        style->color_alpha = values->color_alpha;
    }
    if (mask & S_BACKGROUND) {
        style->background = values->background;
        style->background_alpha = values->background_alpha;
        style->has_background = values->has_background;
        style->background_current_color = values->background_current_color;
    }
    if (mask & S_BACKGROUND_IMAGE) {
        /* Gradient payloads travel through the shared paint stack. */
        style->background_image = values->background_image;
        style->background_image_kind = values->background_image_kind;
    }
    if (mask & S_BACKGROUND_SIZE) {
        style->background_fit = values->background_fit;
        style->background_width = values->background_width;
        style->background_height = values->background_height;
        const uint8_t size_flags = STYLE_BACKGROUND_SIZE_EXPLICIT
            | STYLE_BACKGROUND_WIDTH_AUTO | STYLE_BACKGROUND_HEIGHT_AUTO
            | STYLE_BACKGROUND_WIDTH_PERCENT | STYLE_BACKGROUND_HEIGHT_PERCENT;
        style->background_size_flags = (uint8_t) (
            (style->background_size_flags & ~size_flags)
            | (values->background_size_flags & size_flags));
    }
    if (mask & S_OBJECT_FIT) style->object_fit = values->object_fit;
    if (mask_high & S2_OBJECT_POSITION) {
        style->object_position_x = values->object_position_x;
        style->object_position_y = values->object_position_y;
    }
    if (mask & S_FONT_SCALE) {
        style->font_scale = values->font_scale;
        style->font_size = values->font_size;
        style->font_size_fraction = values->font_size_fraction;
        style->font_size_unit = values->font_size_unit;
    }
    if (mask & S_FONT_FAMILY) style->font_family = values->font_family;
    if (mask & S_FONT_WEIGHT) {
        style->font_weight = values->font_weight;
        style->font_bold = values->font_bold;
    }
    if (mask & S_FONT_STYLE) style->font_italic = values->font_italic;
    if (mask & S_VERTICAL_ALIGN) {
        style->vertical_align = values->vertical_align;
    }
    if (mask & S_WORD_SPACING) style->word_spacing = values->word_spacing;
    if (mask & S_LETTER_SPACING) {
        style->letter_spacing_fixed = values->letter_spacing_fixed;
    }
    if (mask & S_LINE_HEIGHT) {
        style->line_height = values->line_height;
        style->line_height_factor = 0;
    }
    if (mask & S_TEXT_ALIGN) style->text_align = values->text_align;
    if (mask & S_WHITE_SPACE) {
        style->white_space_mode = values->white_space_mode;
    }
    if (mask & S_OVERFLOW_WRAP) {
        style->overflow_wrap = (OverflowWrap) (
            (style->overflow_wrap & ~STYLE_OVERFLOW_WRAP_MASK)
            | computed_style_overflow_wrap(values));
    }
    if (mask & S_WORD_BREAK) {
        style->word_break_mode = (uint8_t) (
            (style->word_break_mode & ~STYLE_WORD_BREAK_MASK)
            | (values->word_break_mode & STYLE_WORD_BREAK_MASK));
    }
    if (mask & S_MARGIN_TOP) {
        style->margin.top = values->margin.top;
        style->margin_top_auto = values->margin_top_auto;
        style->margin_top_percent = values->margin_top_percent;
    }
    if (mask & S_MARGIN_RIGHT) {
        style->margin.right = values->margin.right;
        style->margin_right_auto = values->margin_right_auto;
        style->margin_right_percent = values->margin_right_percent;
    }
    if (mask & S_MARGIN_BOTTOM) {
        style->margin.bottom = values->margin.bottom;
        style->margin_bottom_auto = values->margin_bottom_auto;
        style->margin_bottom_percent = values->margin_bottom_percent;
    }
    if (mask & S_MARGIN_LEFT) {
        style->margin.left = values->margin.left;
        style->margin_left_auto = values->margin_left_auto;
        style->margin_left_percent = values->margin_left_percent;
    }
    if (mask & S_PADDING_TOP) style->padding.top = values->padding.top;
    if (mask & S_PADDING_RIGHT) style->padding.right = values->padding.right;
    if (mask & S_PADDING_BOTTOM) style->padding.bottom = values->padding.bottom;
    if (mask & S_PADDING_LEFT) style->padding.left = values->padding.left;
    if (mask & S_BORDER_TOP) style->border.top = values->border.top;
    if (mask & S_BORDER_RIGHT) style->border.right = values->border.right;
    if (mask & S_BORDER_BOTTOM) style->border.bottom = values->border.bottom;
    if (mask & S_BORDER_LEFT) style->border.left = values->border.left;
    if (mask & S_BORDER_COLOR) {
        style->border_color = values->border_color;
        style->border_alpha = values->border_alpha;
    }
    if ((mask_high & S2_BORDER_COLOR_ALL) == S2_BORDER_COLOR_ALL
        && computed_style_border_color_set(values) == 0) {
        style->border_color = values->border_color;
        style->border_alpha = values->border_alpha;
        style->border_color_set = 0;
    } else {
        if (mask_high & S2_BORDER_COLOR_TOP) {
            style_copy_border_color(
                sheet, style, values, STYLE_BORDER_TOP);
        }
        if (mask_high & S2_BORDER_COLOR_RIGHT) {
            style_copy_border_color(
                sheet, style, values, STYLE_BORDER_RIGHT);
        }
        if (mask_high & S2_BORDER_COLOR_BOTTOM) {
            style_copy_border_color(
                sheet, style, values, STYLE_BORDER_BOTTOM);
        }
        if (mask_high & S2_BORDER_COLOR_LEFT) {
            style_copy_border_color(
                sheet, style, values, STYLE_BORDER_LEFT);
        }
    }
    if (mask_high & S2_BORDER_LINE_TOP) {
        computed_style_set_border_line(
            style, STYLE_BORDER_TOP,
            computed_style_border_line(values, STYLE_BORDER_TOP));
    }
    if (mask_high & S2_BORDER_LINE_RIGHT) {
        computed_style_set_border_line(
            style, STYLE_BORDER_RIGHT,
            computed_style_border_line(values, STYLE_BORDER_RIGHT));
    }
    if (mask_high & S2_BORDER_LINE_BOTTOM) {
        computed_style_set_border_line(
            style, STYLE_BORDER_BOTTOM,
            computed_style_border_line(values, STYLE_BORDER_BOTTOM));
    }
    if (mask_high & S2_BORDER_LINE_LEFT) {
        computed_style_set_border_line(
            style, STYLE_BORDER_LEFT,
            computed_style_border_line(values, STYLE_BORDER_LEFT));
    }
    if (mask & S_BORDER_RADIUS) {
        style->border_radius = style_border_radius_merge(
            style->border_radius, values->border_radius);
    }
    if (mask_high & S2_APPEARANCE) {
        style->appearance = (AppearanceMode) (
            (style->appearance & STYLE_JUSTIFY_SELF_MASK)
            | (values->appearance & STYLE_APPEARANCE_MASK));
    }
    if (mask_high & S2_TEXT_TRANSFORM) {
        style->text_transform = values->text_transform;
    }
    if (mask_high & S2_GRID_UNIFORM_ROWS) {
        computed_style_set_grid_subgrid_rows(
            style, computed_style_grid_subgrid_rows(values));
        computed_style_set_grid_row_template_id(
            style, computed_style_grid_row_template_id(values));
    }
    if (mask_high & S2_TABLE_LAYOUT) {
        style->table_layout_fixed = values->table_layout_fixed;
    }
    if (mask_high & S2_PERSPECTIVE) {
        style->has_perspective = values->has_perspective;
    }
    if (mask_high & S2_FILTER) {
        style->has_filter = values->has_filter;
        style->filter_code = (uint8_t) (
            (style->filter_code & ~STYLE_FILTER_CODE_MASK)
            | (values->filter_code & STYLE_FILTER_CODE_MASK));
    }
    if (mask_high & S2_CONTAIN) {
        if (values->content_visibility != STYLE_CONTENT_VISIBILITY_VISIBLE) {
            style->content_visibility = values->content_visibility
                    == STYLE_CONTENT_VISIBILITY_EXPLICIT_VISIBLE
                ? STYLE_CONTENT_VISIBILITY_VISIBLE
                : values->content_visibility;
        } else {
            style->has_layout_containment = values->has_layout_containment;
        }
    }
    if (mask_high & S2_WILL_CHANGE) {
        style->will_change_transform = values->will_change_transform;
    }
    if (mask_high & S2_POINTER_EVENTS) {
        style->pointer_events_none = values->pointer_events_none;
    }
    if (mask_high & S2_OVERFLOW_CLIP_MARGIN) {
        style->overflow_clip_margin = values->overflow_clip_margin;
    }
    if (mask_high & S2_TEXT_OVERFLOW) {
        bool clamp_authored = (values->overflow_wrap
            & STYLE_LINE_CLAMP_SPECIFIED) != 0;
        /* A line-clamp declaration must not clear an independently-cascaded
           ellipsis. If the same declaration explicitly asks for ellipsis,
           retain both. */
        if (!clamp_authored
            || (values->overflow_wrap & STYLE_TEXT_OVERFLOW_ELLIPSIS) != 0) {
            style->overflow_wrap = (OverflowWrap) (
                (style->overflow_wrap & ~STYLE_TEXT_OVERFLOW_ELLIPSIS)
                | (values->overflow_wrap & STYLE_TEXT_OVERFLOW_ELLIPSIS));
        }
        if (clamp_authored) {
            style->line_clamp = (uint8_t) (
                (style->line_clamp & STYLE_TAB_SIZE_MASK)
                | (values->line_clamp & STYLE_LINE_CLAMP_MASK));
        }
    }
    if (mask_high & S2_OUTLINE_COLOR) {
        style->outline_color = values->outline_color;
        style->outline_alpha = values->outline_alpha;
        style->outline_state = (uint16_t) (
            (style->outline_state & ~STYLE_OUTLINE_CURRENT_COLOR)
            | (values->outline_state & STYLE_OUTLINE_CURRENT_COLOR));
    }
    if (mask_high & S2_OUTLINE_STYLE) {
        uint16_t declarations = (uint16_t) (
            values->outline_state
            & (STYLE_OUTLINE_DECL_WIDTH | STYLE_OUTLINE_DECL_STYLE));
        /* CSS-wide declarations arrive through the generic cascade path and
           have no declaration marker; they reset the complete packed pair.
           Ordinary longhands carry a marker and replace only their field. */
        if (declarations == 0
            || (declarations & STYLE_OUTLINE_DECL_WIDTH) != 0) {
            style->outline_state = (uint16_t) (
                (style->outline_state & ~STYLE_OUTLINE_WIDTH_MASK)
                | (values->outline_state & STYLE_OUTLINE_WIDTH_MASK));
        }
        if (declarations == 0
            || (declarations & STYLE_OUTLINE_DECL_STYLE) != 0) {
            style->outline_state = (uint16_t) (
                (style->outline_state & ~STYLE_OUTLINE_STYLE_MASK)
                | (values->outline_state & STYLE_OUTLINE_STYLE_MASK));
        }
    }
    if (mask_high & S2_OUTLINE_OFFSET) {
        style->outline_state = (uint16_t) (
            (style->outline_state & ~STYLE_OUTLINE_OFFSET_MASK)
            | (values->outline_state & STYLE_OUTLINE_OFFSET_MASK));
    }
    if (mask_high & S2_CLIP_PATH) {
        style->clip_path_state = values->clip_path_state;
    }
    if (mask_high & S2_BORDER_COLLAPSE) {
        style->table_border_collapse = values->table_border_collapse;
    }
    if (mask_high & S2_WRITING_MODE) {
        style->filter_code = (uint8_t) (
            (style->filter_code & ~STYLE_WRITING_MODE_MASK)
            | (values->filter_code & STYLE_WRITING_MODE_MASK));
    }
    if (mask_high & S2_DIRECTION) {
        style->filter_code = (uint8_t) (
            (style->filter_code & ~STYLE_DIRECTION_RTL)
            | (values->filter_code & STYLE_DIRECTION_RTL));
    }
    if (mask_high & S2_UNICODE_BIDI) {
        style->unicode_bidi = values->unicode_bidi;
    }
    if (mask & S_GAP) {
        style->gap = values->gap;
        computed_style_set_grid_column_gap_specified(
            style, computed_style_grid_column_gap_specified(values));
    }
    if (mask & S_ROW_GAP) {
        style->row_gap = values->row_gap;
        computed_style_set_grid_row_gap_specified(
            style, computed_style_grid_row_gap_specified(values));
    }
    if (mask & S_GRID_COLUMNS) {
        computed_style_set_grid_column_count(
            style, computed_style_grid_column_count(values));
        computed_style_set_grid_subgrid_columns(
            style, computed_style_grid_subgrid_columns(values));
        computed_style_set_grid_column_template_id(
            style, computed_style_grid_column_template_id(values));
        style->grid_min_column_width = values->grid_min_column_width;
    }
    if (mask & S_FLEX_DIRECTION) style->flex_direction = values->flex_direction;
    if (mask & S_FLEX_WRAP) {
        style->flex_wrap = values->flex_wrap;
        style->flex_wrap_reverse = values->flex_wrap_reverse;
    }
    if (mask & S_FLEX_GROW) style->flex_grow = values->flex_grow;
    if (mask & S_FLEX_SHRINK) style->flex_shrink = values->flex_shrink;
    if (mask & S_FLEX_BASIS) {
        style->has_flex_basis = values->has_flex_basis;
        style->flex_basis = values->flex_basis;
        style->flex_basis_percent = values->flex_basis_percent;
        style->flex_basis_offset = values->flex_basis_offset;
    }
    if (mask & S_WIDTH) {
        style->has_width = values->has_width;
        style->width_max_content = values->width_max_content;
        style->width = values->width;
        style->width_percent = values->width_percent;
        style->width_offset = values->width_offset;
    }
    if (mask & S_HEIGHT) {
        style->has_height = values->has_height;
        style->height_percent = values->height_percent;
        style->height = values->height;
    }
    if (mask & S_ASPECT_RATIO) {
        style->aspect_width = values->aspect_width;
        style->aspect_height = values->aspect_height;
    }
    if (mask & S_MIN_WIDTH) {
        style->min_width = values->min_width;
        style->min_width_auto = values->min_width_auto;
        style->min_width_percent = values->min_width_percent;
        style->min_width_offset = values->min_width_offset;
    }
    if (mask & S_MIN_HEIGHT) {
        style->min_height = values->min_height;
        style->min_height_auto = values->min_height_auto;
        style->min_height_percent = values->min_height_percent;
    }
    if (mask & S_OVERFLOW_X) {
        style->overflow_x_scroll = values->overflow_x_scroll;
        style->overflow_x_clip_only = values->overflow_x_clip_only;
        style->filter_code = (uint8_t) (
            (style->filter_code & ~STYLE_OVERFLOW_X_HIDDEN)
            | (values->filter_code & STYLE_OVERFLOW_X_HIDDEN));
    }
    if (mask & S_OVERFLOW_Y) {
        style->overflow_y_scroll = values->overflow_y_scroll;
        style->overflow_y_clip_only = values->overflow_y_clip_only;
        style->overflow_y_hidden = values->overflow_y_hidden;
    }
    if (mask & S_MAX_WIDTH) {
        style->max_width = values->max_width;
        style->max_width_percent = values->max_width_percent;
        style->max_width_offset = values->max_width_offset;
    }
    if (mask & S_MAX_HEIGHT) {
        style->max_height = values->max_height;
        style->max_height_percent = values->max_height_percent;
    }
    if (mask & S_BOX_SIZING) {
        style->box_sizing_border_box = values->box_sizing_border_box;
    }
    if (mask & S_Z_INDEX) {
        style->has_z_index = values->has_z_index;
        style->z_index = values->z_index;
    }
    if (mask & S_OPACITY) style->opacity = values->opacity;
    if (mask & S_TRANSFORM) {
        style->has_transform = values->has_transform;
        style->transform_scale_q14 = values->transform_scale_q14;
        style->individual_rotate_quadrants =
            values->individual_rotate_quadrants;
        style->transform_x_percent = values->transform_x_percent;
        style->transform_y_percent = values->transform_y_percent;
        style->transform_x = values->transform_x;
        style->transform_y = values->transform_y;
    }
    if (mask & S_ALIGN_ITEMS) style->align_items = values->align_items;
    if (mask_high & S2_JUSTIFY_ITEMS) {
        style->justify_items = values->justify_items;
    }
    if (mask_high & S2_ALIGN_SELF) style->align_self = values->align_self;
    if (mask_high & S2_JUSTIFY_SELF) {
        computed_style_set_justify_self(
            style, computed_style_justify_self(values));
    }
    if (mask_high & S2_GRID_AUTO_FLOW) {
        style->containing_block_reserved =
            values->containing_block_reserved;
    }
    if (mask_high & S2_SCROLLBAR_GUTTER) {
        style->scrollbar_gutter_stable =
            values->scrollbar_gutter_stable;
    }
    if (mask_high & S2_GRID_AUTO_COLUMNS) {
        style->grid_auto_column_type = values->grid_auto_column_type;
        style->grid_auto_column_value = values->grid_auto_column_value;
        style->grid_auto_column_second = values->grid_auto_column_second;
    }
    if (mask_high & S2_GRID_AUTO_ROWS) {
        style->grid_auto_row_type = values->grid_auto_row_type;
        style->grid_auto_row_value = values->grid_auto_row_value;
        style->grid_auto_row_second = values->grid_auto_row_second;
    }
    if (mask_high & S2_ALIGN_CONTENT) {
        style->align_content = values->align_content;
    }
    if (mask_high & S2_ORDER) style->order = values->order;
    if (mask_high & S2_GRID_COLUMN_START) {
        computed_style_set_grid_column_start(
            style, computed_style_grid_column_start(values));
        computed_style_set_grid_column_start_name(
            style, computed_style_grid_column_start_name(values));
    }
    if (mask_high & S2_GRID_COLUMN_END) {
        computed_style_set_grid_column_end(
            style, computed_style_grid_column_end(values));
        computed_style_set_grid_column_end_name(
            style, computed_style_grid_column_end_name(values));
    }
    if (mask_high & S2_GRID_COLUMN_SPAN) {
        computed_style_set_grid_column_span(
            style, computed_style_grid_column_span(values));
    }
    if (mask_high & S2_GRID_ROW_START) {
        computed_style_set_grid_row_start(
            style, computed_style_grid_row_start(values));
        computed_style_set_grid_row_start_name(
            style, computed_style_grid_row_start_name(values));
    }
    if (mask_high & S2_GRID_ROW_END) {
        computed_style_set_grid_row_end(
            style, computed_style_grid_row_end(values));
        computed_style_set_grid_row_end_name(
            style, computed_style_grid_row_end_name(values));
    }
    if (mask_high & S2_GRID_ROW_SPAN) {
        computed_style_set_grid_row_span(
            style, computed_style_grid_row_span(values));
    }
    if (mask_high & S2_GRID_TEMPLATE_AREAS) {
        computed_style_set_grid_template_area_id(
            style, computed_style_grid_template_area_id(values));
    }
    if (mask_high & S2_GRID_AREA_NAME) {
        computed_style_set_grid_named_area_id(
            style, computed_style_grid_named_area_id(values));
    }
    if (mask_high & S2_FLOAT) style->float_mode = values->float_mode;
    if (mask_high & S2_CLEAR) style->clear_mode = values->clear_mode;
    if (mask_high & S2_CLIP_RECT) {
        style->clip_rect_empty = values->clip_rect_empty;
    }
    if (mask_high & S2_BACKGROUND_POSITION) {
        style->background_position_x = values->background_position_x;
        style->background_position_y = values->background_position_y;
        /* The pixel/percent interpretation travels with the position. */
        style->background_size_flags = (uint8_t) (
            (style->background_size_flags
             & ~STYLE_BACKGROUND_POSITION_PIXELS)
            | (values->background_size_flags
               & STYLE_BACKGROUND_POSITION_PIXELS));
    }
    /* Optional background/mask layers were merged property-by-property
       before copying the hot first-layer fields above. */
    if (mask_high & S2_BACKGROUND_REPEAT) {
        style->background_size_flags = (uint8_t) (
            (style->background_size_flags
             & ~(STYLE_BACKGROUND_NO_REPEAT_X
                 | STYLE_BACKGROUND_NO_REPEAT_Y))
            | (values->background_size_flags
               & (STYLE_BACKGROUND_NO_REPEAT_X
                  | STYLE_BACKGROUND_NO_REPEAT_Y)));
    }
    if (mask_high & S2_TEXT_INDENT) {
        style->text_indent = values->text_indent == STYLE_LENGTH_NONE
            ? (parent == NULL ? 0 : parent->text_indent) : values->text_indent;
    }
    if (mask_high & S2_TEXT_DECORATION) {
        style_set_text_underline(
            style, (values->text_decoration_state
                    & STYLE_TEXT_DECORATION_INHERIT) != 0
                ? parent != NULL && computed_style_has_text_underline(parent)
                : computed_style_has_text_underline(values));
    }
    if (mask_high & S2_TEXT_UNDERLINE_OFFSET) {
        style_set_text_offset_code(
            style, style_text_offset_code(values, STYLE_TEXT_OFFSET_SHIFT,
                                          STYLE_TEXT_OFFSET_MASK),
            STYLE_TEXT_OFFSET_SHIFT, STYLE_TEXT_OFFSET_MASK);
    }
    if (mask & S_JUSTIFY_CONTENT) {
        style->justify_content = values->justify_content;
    }
    if (mask & S_MASK_IMAGE) {
        style->mask_image = values->mask_image;
        const StylePaintStack *paint = stylesheet_paint_stack(
            sheet, computed_style_paint_stack_id(style));
        if (paint != NULL && paint->mask_count != 0
            && paint->masks[0].kind == STYLE_PAINT_IMAGE_URL) {
            style->mask_image = paint->masks[0].image;
        }
    }
    if (mask & S_LIST_STYLE) {
        style->list_style_none = values->list_style_none;
        style->list_style_type = values->list_style_type;
    }
    if (mask & S_CONTENT) {
        style->generated_content = values->generated_content;
        style->generated_text_length = values->generated_text_length;
        style->generated_text = values->generated_text;
        style->generated_attr_length = values->generated_attr_length;
        memcpy(&style->generated_attr, &values->generated_attr,
               sizeof(style->generated_attr));
        style->generated_expression = values->generated_expression;
        style->generated_image = values->generated_image;
    }
    if (mask_high & S2_COUNTER_RESET) {
        style->counter_reset_id = values->counter_reset_id;
    }
    if (mask_high & S2_COUNTER_INCREMENT) {
        style->counter_increment_id = values->counter_increment_id;
    }
    if (mask_high & S2_COUNTER_SET) {
        style->counter_set_id = values->counter_set_id;
    }
    if (mask_high & S2_LIST_STYLE_TYPE) {
        style->list_style_type = values->list_style_type;
        style->list_style_none =
            values->list_style_type == LIST_STYLE_NONE;
    }
    if (mask_high & S2_LIST_STYLE_POSITION) {
        style->list_style_inside = values->list_style_inside;
    }
    if (mask & S_TOP) {
        style->has_top = values->has_top;
        style->top = values->top;
        style->inset_percent_mask =
            (style->inset_percent_mask & ~STYLE_INSET_TOP_PERCENT)
            | (values->inset_percent_mask & STYLE_INSET_TOP_PERCENT);
    }
    if (mask & S_RIGHT) {
        style->has_right = values->has_right;
        style->right = values->right;
        style->inset_percent_mask =
            (style->inset_percent_mask & ~STYLE_INSET_RIGHT_PERCENT)
            | (values->inset_percent_mask & STYLE_INSET_RIGHT_PERCENT);
    }
    if (mask & S_BOTTOM) {
        style->has_bottom = values->has_bottom;
        style->bottom = values->bottom;
        style->inset_percent_mask =
            (style->inset_percent_mask & ~STYLE_INSET_BOTTOM_PERCENT)
            | (values->inset_percent_mask & STYLE_INSET_BOTTOM_PERCENT);
    }
    if (mask & S_LEFT) {
        style->has_left = values->has_left;
        style->left = values->left;
        style->inset_percent_mask =
            (style->inset_percent_mask & ~STYLE_INSET_LEFT_PERCENT)
            | (values->inset_percent_mask & STYLE_INSET_LEFT_PERCENT);
    }
    if (mask & S_VISIBILITY) {
        style->visibility_hidden = values->visibility_hidden;
    }
    if (mask & S_POSITION) {
        style->out_of_flow = values->out_of_flow;
        style->relative_position = values->relative_position;
        style->sticky_position = values->sticky_position;
        style->fixed_position = values->fixed_position;
    }
}

/* Apply the CSS-wide `inherit` bits recorded on a declaration after its
   concrete values. Shared paint payloads must merge through the same bounded
   interning path as ordinary declarations; copying only their scalar
   discriminants would detach a gradient from its retained ramp. */
static void apply_inherit_mask(Stylesheet *sheet, ComputedStyle *style,
                               const ComputedStyle *parent,
                               uint64_t inherit_mask)
{
    if (inherit_mask == 0 || parent == NULL) return;
    if (inherit_mask & S_FONT_SCALE) {
        style->font_size = parent->font_size;
        style->font_size_fraction = parent->font_size_fraction;
        style->font_size_unit = FONT_SIZE_UNIT_ABSOLUTE;
        style->font_scale = parent->font_scale;
    }
    if (inherit_mask & S_FONT_FAMILY) {
        style->font_family = parent->font_family;
    }
    if (inherit_mask & S_FONT_WEIGHT) {
        style->font_weight = parent->font_weight;
        style->font_bold = parent->font_bold;
    }
    if (inherit_mask & S_FONT_STYLE) {
        style->font_italic = parent->font_italic;
    }
    if (inherit_mask & S_LINE_HEIGHT) {
        style->line_height = parent->line_height_factor != 0
            ? -(int) parent->line_height_factor : parent->line_height;
        style->line_height_factor = parent->line_height_factor;
    }
    if (inherit_mask & S_WHITE_SPACE) {
        style->white_space_mode = parent->white_space_mode;
    }
    if (inherit_mask & S_Z_INDEX) {
        style->has_z_index = parent->has_z_index;
        style->z_index = parent->z_index;
    }
    if (inherit_mask & S_BOX_SIZING) {
        style->box_sizing_border_box = parent->box_sizing_border_box;
    }
    if (inherit_mask & S_COLOR) {
        style->color = parent->color;
        style->color_alpha = parent->color_alpha;
    }
    if (inherit_mask & S_OPACITY) {
        style->opacity = parent->opacity;
    }
    if (inherit_mask & S_BORDER_RADIUS) {
        style->border_radius = parent->border_radius;
    }
    if (inherit_mask & S_VISIBILITY) {
        style->visibility_hidden = parent->visibility_hidden;
    }
    if (inherit_mask & S_BACKGROUND) {
        style->has_background = parent->has_background;
        style->background_current_color = parent->background_current_color;
        style->background = parent->background;
        style->background_alpha = parent->background_alpha;
    }
    if (inherit_mask & S_BACKGROUND_IMAGE) {
        apply_paint_values(
            sheet, style, parent, S_BACKGROUND_IMAGE, 0);
        style->background_image = parent->background_image;
        style->background_image_kind = parent->background_image_kind;
    }
}

static void style_record_logical_axes(const Stylesheet *sheet,
                                      const ComputedStyle *style)
{
    uint8_t axes = sheet->resolve_scratch->logical_axes_locked
        ? sheet->resolve_scratch->logical_axes
        : (uint8_t) (style->filter_code
                     & (STYLE_WRITING_MODE_MASK | STYLE_DIRECTION_RTL));
    if (!sheet->resolve_scratch->logical_applied) {
        sheet->resolve_scratch->logical_axes = axes;
    } else if (sheet->resolve_scratch->logical_axes != axes) {
        sheet->resolve_scratch->logical_axes_mismatch = true;
    }
    sheet->resolve_scratch->logical_applied = true;
}

static void apply_style_rule(const Stylesheet *sheet, const StyleRule *rule,
                             ComputedStyle *style,
                             const ComputedStyle *parent)
{
    const StyleDeclaration *declaration = stylesheet_rule_declaration(
        sheet, rule);
    if (declaration == NULL) return;
    uint64_t revert_rule_mask = 0;
    uint64_t revert_rule_mask_high = 0;
    stylesheet_rule_revert_mask(
        sheet, rule, &revert_rule_mask, &revert_rule_mask_high);
    if ((declaration->deferred_program_reserved
         & STYLE_DEFERRED_FONT_RELATIVE) != 0) {
        sheet->resolve_scratch->font_relative_applied = true;
    }
    if ((declaration->deferred_program_reserved
         & STYLE_DEFERRED_NON_FONT_RELATIVE) != 0) {
        sheet->resolve_scratch->non_font_relative_applied = true;
    }
    if ((declaration->deferred_program_reserved & STYLE_DEFERRED_CH) != 0
        && !sheet->resolve_scratch->font_ch_basis_active) {
        sheet->resolve_scratch->font_ch_pending = true;
    }
    if ((declaration->deferred_program_reserved
         & STYLE_DEFERRED_LOGICAL) != 0) {
        style_record_logical_axes(sheet, style);
    }
    if ((declaration->deferred_program_reserved
         & STYLE_DECLARATION_ALL_INHERIT) != 0
        && parent != NULL) {
        /* CSS `all` excludes direction and unicode-bidi. Direction is the
           only excluded value retained by this compact ComputedStyle. */
        uint8_t direction = style->filter_code & STYLE_DIRECTION_RTL;
        *style = *parent;
        style->filter_code = (uint8_t) (
            (style->filter_code & ~STYLE_DIRECTION_RTL) | direction);
    }
    ComputedStyle declared;
    stylesheet_declaration_values(sheet, declaration, &declared);
    apply_values((Stylesheet *) sheet, style,
                 &declared,
                 declaration->mask & ~revert_rule_mask,
                 declaration->mask_high & ~revert_rule_mask_high, parent);
    apply_inherit_mask(
        (Stylesheet *) sheet, style, parent,
        declaration->inherit_mask & ~revert_rule_mask);
    if (declaration->deferred_declarations == NULL) return;
    if ((declaration->deferred_program_reserved & STYLE_DEFERRED_LOGICAL) != 0
        && (declaration->deferred_program_reserved
            & STYLE_DEFERRED_DYNAMIC) == 0) {
        uint8_t axes = sheet->resolve_scratch->logical_axes_locked
            ? sheet->resolve_scratch->logical_axes
            : (uint8_t) (style->filter_code
                         & (STYLE_WRITING_MODE_MASK | STYLE_DIRECTION_RTL));
        if (axes == 0) return;
    }
    /* Deferred declarations are reparsed against the sheet's append-only
       interning pools; the cast is confined to that parse. */
    Stylesheet *mutable_sheet = (Stylesheet *) sheet;
    mutable_sheet->deferred_rule_applications++;
#ifndef TILEFINCH_NO_TRACE
    uint64_t deferred_started_ns = tilefinch_platform_monotonic_time_ns();
#endif
    StyleResolveScratch saved_scratch = *sheet->resolve_scratch;
    if (!stylesheet_select_image_source_slot(
            mutable_sheet, rule->image_source_slot)) {
        *sheet->resolve_scratch = saved_scratch;
#ifndef TILEFINCH_NO_TRACE
        mutable_sheet->deferred_rule_us +=
            (tilefinch_platform_monotonic_time_ns() - deferred_started_ns)
            / UINT64_C(1000);
#endif
        return;
    }
    ComputedStyle resolved = *style;
    uint64_t mask = 0;
    uint64_t mask_high = 0;
    uint64_t deferred_inherit = 0;
    bool parsed;
    if (declaration->deferred_program_offset != UINT32_MAX
        && !sheet->resolve_scratch->font_ch_basis_active) {
        if (mutable_sheet->deferred_program_executions != UINT64_MAX) {
            mutable_sheet->deferred_program_executions++;
        }
        uint64_t instructions = declaration->deferred_program_count;
        if (instructions
            > UINT64_MAX - mutable_sheet->deferred_program_instructions) {
            mutable_sheet->deferred_program_instructions = UINT64_MAX;
        } else {
            mutable_sheet->deferred_program_instructions += instructions;
        }
        parsed = style_apply_deferred_program_basis(
            mutable_sheet, declaration->deferred_declarations,
            declaration->deferred_length,
            declaration->deferred_program_offset,
            declaration->deferred_program_count, rule->important,
            (declaration->deferred_program_reserved
             & STYLE_DEFERRED_NO_FONT_SIZE) == 0,
            &resolved, &mask, &mask_high, &deferred_inherit);
    } else {
        if (mutable_sheet->deferred_program_fallbacks != UINT64_MAX) {
            mutable_sheet->deferred_program_fallbacks++;
        }
        parsed = style_parse_declarations(
            mutable_sheet, declaration->deferred_declarations,
            declaration->deferred_length, &resolved, &mask, &mask_high,
            &deferred_inherit, false, rule->important ? 1 : 0, NULL, NULL);
    }
    bool font_ch_pending = sheet->resolve_scratch->font_ch_pending;
    if (parsed
        && (declaration->deferred_program_reserved
            & STYLE_DEFERRED_LOGICAL) != 0) {
        unsigned final_axes = resolved.filter_code
            & (STYLE_WRITING_MODE_MASK | STYLE_DIRECTION_RTL);
        resolved = *style;
        resolved.filter_code = (uint8_t) (
            (resolved.filter_code
             & ~(STYLE_WRITING_MODE_MASK | STYLE_DIRECTION_RTL))
            | final_axes);
        mask = 0;
        mask_high = 0;
        deferred_inherit = 0;
        if (declaration->deferred_program_offset != UINT32_MAX
            && !sheet->resolve_scratch->font_ch_basis_active) {
            parsed = style_apply_deferred_program_basis(
                mutable_sheet, declaration->deferred_declarations,
                declaration->deferred_length,
                declaration->deferred_program_offset,
                declaration->deferred_program_count, rule->important,
                (declaration->deferred_program_reserved
                 & STYLE_DEFERRED_NO_FONT_SIZE) == 0,
                &resolved, &mask, &mask_high, &deferred_inherit);
        } else {
            parsed = style_parse_declarations(
                mutable_sheet, declaration->deferred_declarations,
                declaration->deferred_length, &resolved, &mask, &mask_high,
                &deferred_inherit, false, rule->important ? 1 : 0,
                NULL, NULL);
        }
        font_ch_pending =
            font_ch_pending || sheet->resolve_scratch->font_ch_pending;
    }
    *sheet->resolve_scratch = saved_scratch;
    sheet->resolve_scratch->font_ch_pending =
        sheet->resolve_scratch->font_ch_pending || font_ch_pending;
    if (parsed) {
        mask &= ~revert_rule_mask;
        mask_high &= ~revert_rule_mask_high;
        deferred_inherit &= ~revert_rule_mask;
        apply_values((Stylesheet *) sheet, style, &resolved, mask, mask_high,
                     parent);
        /* A CSS-wide 'inherit' inside a var()-deferred rule sets its
           mask bit with no value in the freshly-zeroed scratch style;
           without this, font-size:inherit in such a rule applied zero
           (clamped to the 6px floor).  A load-more control can mix
           color:var(...) with font-size:inherit. */
        apply_inherit_mask(
            mutable_sheet, style, parent, deferred_inherit);
    }
#ifndef TILEFINCH_NO_TRACE
    mutable_sheet->deferred_rule_us +=
        (tilefinch_platform_monotonic_time_ns() - deferred_started_ns)
        / UINT64_C(1000);
#endif
}

static bool style_is_root_element(lxb_dom_node_t *node)
{
    if (!style_tag_is(node, "html")) return false;
    for (lxb_dom_node_t *parent = node->parent; parent != NULL;
         parent = parent->parent) {
        if (parent->type == LXB_DOM_NODE_TYPE_ELEMENT) return false;
    }
    return true;
}

static void resolve_font_size(ComputedStyle *style,
                              const ComputedStyle *parent,
                              bool root_element)
{
    /* Percentages and em on the root use the CSS initial font size.  rem on
       the root does too; descendant rem values use the computed root size
       carried in inherited state. */
    int inherited_fixed = root_element ? STYLE_DEFAULT_FONT_PX * 64
        : computed_style_font_size_fixed(parent);
    int root_fixed = root_element ? STYLE_DEFAULT_FONT_PX * 64
        : (style->root_font_size != 0
           ? computed_style_root_font_size_fixed(style)
           : computed_style_root_font_size_fixed(parent));
    int size_fixed = computed_style_font_size_fixed(style);
    if (style->font_size_unit == FONT_SIZE_UNIT_REM) {
        size_fixed = style_multiply_add_divide(
            root_fixed, style->font_size, 500, 1000);
    } else if (style->font_size_unit == FONT_SIZE_UNIT_PERCENT
               || style->font_size_unit == FONT_SIZE_UNIT_EM) {
        size_fixed = style_multiply_add_divide(
            inherited_fixed, style->font_size, 500, 1000);
    }
    style->font_size_unit = FONT_SIZE_UNIT_ABSOLUTE;
    if (size_fixed < STYLE_FONT_MIN_PX * 64) {
        size_fixed = STYLE_FONT_MIN_PX * 64;
    }
    if (size_fixed > STYLE_FONT_MAX_PX * 64) {
        size_fixed = STYLE_FONT_MAX_PX * 64;
    }
    style->font_size = style_font_size_from_fixed(
        size_fixed, &style->font_size_fraction);
    style->font_scale = size_fixed >= 20 * 64 ? 3
                       : (size_fixed >= 13 * 64 ? 2 : 1);
    if (root_element) root_fixed = size_fixed;
    style->root_font_size = (uint8_t) (root_fixed / 64);
    style->root_font_size_fraction = (uint8_t) (root_fixed & 63);
}

static void resolve_sizing_em_length(int *value, int *coefficient,
                                     int font_size_fixed)
{
    if (value == NULL || coefficient == NULL || *coefficient == 0) return;
    int fixed = style_multiply_add_divide(
        font_size_fixed, *coefficient, 0, 1000);
    int pixels;
    if (fixed > STYLE_LENGTH_DIRECT_LIMIT * 64) {
        pixels = STYLE_LENGTH_DIRECT_LIMIT;
    } else if (fixed < -STYLE_LENGTH_DIRECT_LIMIT * 64) {
        pixels = -STYLE_LENGTH_DIRECT_LIMIT;
    } else {
        pixels = fixed < 0 ? (fixed - 32) / 64
                           : (fixed + 32) / 64;
    }
    if (pixels > STYLE_LENGTH_DIRECT_LIMIT) {
        pixels = STYLE_LENGTH_DIRECT_LIMIT;
    } else if (pixels < -STYLE_LENGTH_DIRECT_LIMIT) {
        pixels = -STYLE_LENGTH_DIRECT_LIMIT;
    }
    *value = (int) pixels;
    *coefficient = 0;
}

static void resolve_sizing_em_lengths(ComputedStyle *style)
{
    int font_size_fixed = computed_style_font_size_fixed(style);
    resolve_sizing_em_length(
        &style->width, &style->width_offset, font_size_fixed);
    resolve_sizing_em_length(
        &style->min_width, &style->min_width_offset, font_size_fixed);
    resolve_sizing_em_length(
        &style->max_width, &style->max_width_offset, font_size_fixed);
}

static void resolve_overflow_clip_margin(ComputedStyle *style)
{
    if (style == NULL
        || (style->overflow_clip_margin
            & STYLE_OVERFLOW_CLIP_MARGIN_EM) == 0) return;
    unsigned em = computed_style_overflow_clip_margin(style);
    int pixels = style_multiply_add_divide(
        computed_style_font_size_fixed(style), (int) em, 32, 64);
    if (pixels > STYLE_OVERFLOW_CLIP_MARGIN_MASK) {
        pixels = STYLE_OVERFLOW_CLIP_MARGIN_MASK;
    }
    style->overflow_clip_margin = (uint8_t) (
        (style->overflow_clip_margin & STYLE_OVERFLOW_CLIP_BOX_MASK)
        | (unsigned) pixels);
}

static void resolve_overflow_axes(ComputedStyle *style)
{
    if (style == NULL) return;
    bool x_non_clip = style->overflow_x_scroll
                      && !style->overflow_x_clip_only;
    bool y_non_clip = style->overflow_y_scroll
                      && !style->overflow_y_clip_only;
    /* CSS Overflow computes visible to auto when the other axis is neither
       visible nor clip. Normalize after the cascade so separately declared
       axes are independent of declaration order. Keep the authored clip bit:
       even where its computed keyword pairs like hidden, it is still the
       signal that script scrolling on that axis is disabled and reset. */
    if (x_non_clip && !style->overflow_y_scroll) {
        style->overflow_y_scroll = true;
    }
    if (y_non_clip && !style->overflow_x_scroll) {
        style->overflow_x_scroll = true;
    }
}

static bool text_decoration_propagation_boundary(
    lxb_dom_node_t *node, const ComputedStyle *style)
{
    if (style == NULL) return false;
    if (style->display == DISPLAY_INLINE_BLOCK
        || style->display == DISPLAY_INLINE_FLEX
        || style->display == DISPLAY_INLINE_GRID
        || style->out_of_flow || style->fixed_position
        || style->float_mode != FLOAT_NONE) return true;
    return style_tag_is(node, "img") || style_tag_is(node, "input")
           || style_tag_is(node, "textarea") || style_tag_is(node, "select")
           || style_tag_is(node, "button") || style_tag_is(node, "video")
           || style_tag_is(node, "canvas") || style_tag_is(node, "svg");
}

static bool style_text_has_font_relative_unit(const char *text, size_t length)
{
    if (text == NULL) return false;
    for (size_t i = 1; i + 2 <= length; i++) {
        size_t unit_length = 0;
        if (i + 3 <= length
            && tolower((unsigned char) text[i]) == 'r'
            && tolower((unsigned char) text[i + 1]) == 'e'
            && tolower((unsigned char) text[i + 2]) == 'm') {
            unit_length = 3;
        } else if (tolower((unsigned char) text[i]) == 'e'
                   && tolower((unsigned char) text[i + 1]) == 'm') {
            unit_length = 2;
        } else if (tolower((unsigned char) text[i]) == 'c'
                   && tolower((unsigned char) text[i + 1]) == 'h') {
            unit_length = 2;
        }
        if (unit_length == 0) continue;
        unsigned char before = (unsigned char) text[i - 1];
        unsigned char after = i + unit_length < length
            ? (unsigned char) text[i + unit_length] : 0;
        if ((isdigit(before) || before == '.' || before == ')')
            && !(isalnum(after) || after == '-' || after == '_'
                 || after >= 0x80u)) return true;
    }
    return false;
}

static bool style_text_has_logical_property(const char *text, size_t length)
{
    if (text == NULL) return false;
    for (size_t at = 0; at < length;) {
        while (at < length && isspace((unsigned char) text[at])) at++;
        size_t end = at;
        int parentheses = 0;
        while (end < length) {
            if (text[end] == '(') parentheses++;
            else if (text[end] == ')' && parentheses > 0) parentheses--;
            if (text[end] == ';' && parentheses == 0) break;
            end++;
        }
        size_t colon = at;
        while (colon < end && text[colon] != ':') colon++;
        if (colon < end) {
            const char *name = text + at;
            size_t name_length = colon - at;
            trim(&name, &name_length);
            if (style_property_name_is_logical(name, name_length)) {
                return true;
            }
        }
        at = end + (end < length);
    }
    return false;
}

/* Scanning a style attribute's text (style_sparse_note_inline and the two
   scans above) depends on nothing but the text, and noting only ever sets
   sheet flags. Each relayout resolves an element afresh on a reuse miss,
   so the same text is scanned again and again: keep the answers for
   recent texts, by exact content. A text found here was already noted on
   this sheet, so noting it again could change nothing. */
#define STYLE_INLINE_SCAN_SLOTS 32u
#define STYLE_INLINE_SCAN_TEXT 96u
enum {
    STYLE_INLINE_SCANNED = 1u << 0,
    STYLE_INLINE_LOGICAL = 1u << 1,
    STYLE_INLINE_FONT_RELATIVE = 1u << 2
};
struct StyleInlineScanSlot {
    uint32_t hash;
    uint8_t length;
    uint8_t facts;
    char text[STYLE_INLINE_SCAN_TEXT];
};

size_t stylesheet_inline_scan_bytes(const Stylesheet *sheet)
{
    return sheet == NULL || sheet->inline_scans == NULL ? 0
        : STYLE_INLINE_SCAN_SLOTS * sizeof(struct StyleInlineScanSlot);
}

size_t stylesheet_drop_inline_scans(Stylesheet *sheet)
{
    size_t released = stylesheet_inline_scan_bytes(sheet);
    if (released == 0) return 0;
    budget_free(sheet->budget, sheet->inline_scans);
    sheet->inline_scans = NULL;
    return released;
}

size_t stylesheet_yield_inline_scans(Stylesheet *sheet)
{
    if (sheet == NULL) return 0;
    sheet->inline_scans_yielded = true;
    return stylesheet_drop_inline_scans(sheet);
}

static unsigned style_inline_text_facts(const Stylesheet *sheet,
                                        const char *text, size_t length)
{
    struct StyleInlineScanSlot *slot = NULL;
    uint32_t hash = UINT32_C(2166136261);
    if (length != 0 && length <= STYLE_INLINE_SCAN_TEXT) {
        for (size_t i = 0; i < length; i++)
            hash = (hash ^ (unsigned char) text[i]) * UINT32_C(16777619);
        Stylesheet *mutable_sheet = (Stylesheet *) sheet;
        if (mutable_sheet->inline_scans == NULL
            && !mutable_sheet->inline_scans_yielded) {
            struct StyleInlineScanSlot *table = budget_calloc(sheet->budget,
                STYLE_INLINE_SCAN_SLOTS, sizeof(*sheet->inline_scans));
            /* The allocation can run the Budget's reclaim hook, which may
               yield this very memo: then it stays off. */
            if (table != NULL && mutable_sheet->inline_scans_yielded)
                budget_free(sheet->budget, table);
            else
                mutable_sheet->inline_scans = table;
        }
        if (sheet->inline_scans != NULL) {
            slot = &sheet->inline_scans[hash & (STYLE_INLINE_SCAN_SLOTS - 1u)];
            if ((slot->facts & STYLE_INLINE_SCANNED) != 0
                && slot->hash == hash && slot->length == length
                && memcmp(slot->text, text, length) == 0) return slot->facts;
        }
    }
    style_sparse_note_inline(sheet, text, length);
    unsigned facts = STYLE_INLINE_SCANNED
        | (style_text_has_logical_property(text, length)
           ? STYLE_INLINE_LOGICAL : 0u)
        | (style_text_has_font_relative_unit(text, length)
           ? STYLE_INLINE_FONT_RELATIVE : 0u);
    /* Nothing above allocates, so the memo the slot points into is still
       the sheet's; test anyway rather than rely on that. */
    if (slot != NULL && sheet->inline_scans != NULL) {
        slot->hash = hash;
        slot->length = (uint8_t) length;
        slot->facts = (uint8_t) facts;
        memcpy(slot->text, text, length);
    }
    return facts;
}

static ComputedStyle style_apply_node_cascade_inner(
    const Stylesheet *sheet, lxb_dom_node_t *node,
    const ComputedStyle *parent, bool root_element, bool trace_position);

/* One node's cascade: the DOM is fixed for its duration, so class lists
   tokenized during it stay valid; a fresh outermost scope forgets them. */
static ComputedStyle style_apply_node_cascade(
    const Stylesheet *sheet, lxb_dom_node_t *node,
    const ComputedStyle *parent, bool root_element, bool trace_position)
{
    style_class_tokens_scope_begin(sheet);
    ComputedStyle style = style_apply_node_cascade_inner(
        sheet, node, parent, root_element, trace_position);
    style_class_tokens_scope_end(sheet);
    return style;
}

static ComputedStyle style_apply_node_cascade_inner(
    const Stylesheet *sheet, lxb_dom_node_t *node,
    const ComputedStyle *parent, bool root_element, bool trace_position)
{
    ComputedStyle style = default_style(sheet, node, parent);
    if (node != NULL && node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
        apply_presentational_attributes(sheet, node, &style);
    }
    if (sheet != NULL && sheet->resolve_scratch->logical_axes_locked) {
        style.filter_code = (uint8_t) (
            (style.filter_code
             & ~(STYLE_WRITING_MODE_MASK | STYLE_DIRECTION_RTL))
            | sheet->resolve_scratch->logical_axes);
    }
    if (sheet == NULL || node == NULL
        || node->type != LXB_DOM_NODE_TYPE_ELEMENT) return style;

    /* A retained exact match list skips subject preparation, index planning
       and every candidate evaluation; the rules are replayed per range. */
    const StyleRetainedMatchEntry *retained =
        style_retained_lookup(sheet, node, PSEUDO_NONE);
    /* Resolving declarations can recurse into another node's style and grow
       the dense payload array. Keep this exact list independent of that
       optional allocation (and of nested cache invalidation). */
    StyleRetainedMatchEntry retained_copy;
    if (retained != NULL) {
        retained_copy = *retained;
        retained = &retained_copy;
    }
    bool record = retained == NULL && style_retained_begin(sheet);
    StyleMatchSubject subject = {0};
    StyleRuleIndexPlan index_plan = {0};
    uint32_t candidate_list[STYLE_RULE_INDEX_LIST_CAPACITY];
    if (retained == NULL) {
        subject = style_match_subject(sheet, node);
        index_plan = style_rule_index_plan(
            sheet, &subject, PSEUDO_NONE, candidate_list,
            STYLE_RULE_INDEX_LIST_CAPACITY);
    }
    style_apply_matching_range(
        sheet, parent, node, &subject, &index_plan, PSEUDO_NONE,
        sheet->cascade_starts[CASCADE_AUTHOR_NORMAL],
        sheet->cascade_ends[CASCADE_AUTHOR_NORMAL], &style, trace_position,
        retained, record);
    size_t length = 0;
    const char *inline_css = sheet->block_inline_style_attributes
            && !document_style_attribute_cssom_authorized(node)
        ? NULL : document_attribute(node, "style", &length);
    uint64_t inline_important_mask = 0;
    uint64_t inline_important_mask_high = 0;
    uint64_t inline_important_inherit = 0;
    uint64_t inline_normal_revert_rule_mask = 0;
    uint64_t inline_normal_revert_rule_mask_high = 0;
    uint64_t inline_important_revert_rule_mask = 0;
    uint64_t inline_important_revert_rule_mask_high = 0;
    /* Only elements with a style attribute need this snapshot of the
       cascade so far (344 bytes); it is read under the same condition. */
    ComputedStyle inline_important_values;
    if (inline_css != NULL) {
        inline_important_values = style;
        unsigned facts = style_inline_text_facts(sheet, inline_css, length);
        if ((facts & STYLE_INLINE_LOGICAL) != 0) {
            style_record_logical_axes(sheet, &style);
        }
        if ((facts & STYLE_INLINE_FONT_RELATIVE) != 0) {
            sheet->resolve_scratch->font_relative_applied = true;
            sheet->resolve_scratch->non_font_relative_applied = true;
        }
        /* Inline non-font em values use the element font selected by the
           cascade. During the correction pass that final basis is locked;
           otherwise use the cascade-so-far basis. The split parser's own
           declaration pre-scan still wins for an absolute font-size in the
           style attribute itself. */
        if (!sheet->resolve_scratch->font_resolution_locked) {
            ComputedStyle inline_basis_style = style;
            resolve_font_size(&inline_basis_style, parent, root_element);
            sheet->resolve_scratch->font_resolution_inherited =
                (computed_style_font_size_fixed(&inline_basis_style) + 32)
                / 64;
            sheet->resolve_scratch->font_resolution_active = true;
        }
        uint64_t mask = 0;
        uint64_t mask_high = 0;
        uint64_t inherit_mask = 0;
        ComputedStyle values = style;
        (void) style_parse_declarations_split(
            (Stylesheet *) sheet, inline_css, length, &values, &mask,
            &mask_high, &inherit_mask, &inline_important_values,
            &inline_important_mask, &inline_important_mask_high,
            &inline_important_inherit,
            &inline_normal_revert_rule_mask,
            &inline_normal_revert_rule_mask_high,
            &inline_important_revert_rule_mask,
            &inline_important_revert_rule_mask_high);
        uint64_t inline_revert_rule_mask =
            inline_important_revert_rule_mask
            | (inline_normal_revert_rule_mask
               & ~(inline_important_mask
                   | inline_important_inherit));
        uint64_t inline_revert_rule_mask_high =
            inline_important_revert_rule_mask_high
            | (inline_normal_revert_rule_mask_high
               & ~inline_important_mask_high);
        mask &= ~inline_revert_rule_mask;
        mask_high &= ~inline_revert_rule_mask_high;
        inherit_mask &= ~inline_revert_rule_mask;
        inline_important_mask &= ~inline_revert_rule_mask;
        inline_important_mask_high &= ~inline_revert_rule_mask_high;
        inline_important_inherit &= ~inline_revert_rule_mask;
        apply_values((Stylesheet *) sheet, &style, &values, mask, mask_high,
                     parent);
        apply_inherit_mask(
            (Stylesheet *) sheet, &style, parent, inherit_mask);
    }
    style_apply_matching_range(
        sheet, parent, node, &subject, &index_plan, PSEUDO_NONE,
        sheet->cascade_starts[CASCADE_USER_NORMAL],
        sheet->cascade_ends[CASCADE_USER_NORMAL], &style, false,
        retained, record);
    style_apply_matching_range(
        sheet, parent, node, &subject, &index_plan, PSEUDO_NONE,
        sheet->cascade_starts[CASCADE_AUTHOR_IMPORTANT],
        sheet->cascade_ends[CASCADE_AUTHOR_IMPORTANT], &style, false,
        retained, record);
    if (inline_css != NULL) {
        apply_values((Stylesheet *) sheet, &style,
                     &inline_important_values,
                     inline_important_mask, inline_important_mask_high, parent);
        apply_inherit_mask(
            (Stylesheet *) sheet, &style, parent,
            inline_important_inherit);
    }
    style_apply_matching_range(
        sheet, parent, node, &subject, &index_plan, PSEUDO_NONE,
        sheet->cascade_starts[CASCADE_USER_IMPORTANT],
        sheet->cascade_ends[CASCADE_USER_IMPORTANT], &style, false,
        retained, record);
    if (record) style_retained_commit(sheet, node, PSEUDO_NONE);
    if (sheet->fullscreen_node == node) {
        /* The PSP has no compositor top layer. Apply the bounded equivalent
           after the author cascade so a fullscreen request always owns the
           existing 480x272 page viewport without rewriting author styles. */
        style.display = DISPLAY_BLOCK;
        style.hidden = false;
        style.visibility_hidden = false;
        style.fixed_position = true;
        style.relative_position = false;
        style.sticky_position = false;
        style.out_of_flow = true;
        style.has_top = true;
        style.has_right = true;
        style.has_bottom = true;
        style.has_left = true;
        style.top = style.right = style.bottom = style.left = 0;
        style.inset_percent_mask = 0;
        style.margin.top = style.margin.right = 0;
        style.margin.bottom = style.margin.left = 0;
        style.has_width = true;
        style.width_percent = true;
        style.width = 100;
        style.has_height = true;
        style.height_percent = true;
        style.height = 100;
        style.has_z_index = true;
        style.z_index = STYLE_LENGTH_DIRECT_LIMIT;
        style.opacity = UINT8_MAX;
    }
    return style;
}

/* True when a candidate rule that can affect discovery actually matches
   `node` for `pseudo`. Flagged candidates are few, so the exact selector
   test is cheaper than resolving the element. */
static bool style_plan_has_discovery_rule(const Stylesheet *sheet,
                                          lxb_dom_node_t *node,
                                          const StyleMatchSubject *subject,
                                          const StyleRuleIndexPlan *plan,
                                          PseudoElement pseudo)
{
    if (!plan->ready) return true;
    uint64_t ancestor_tokens = 0;
    bool ancestor_tokens_ready = false;
    for (size_t i = 0; i <= plan->count; i++) {
        bool listed = i == plan->count;
        uint32_t begin = listed ? 0 : plan->ranges[i].begin;
        uint32_t end = listed ? (uint32_t) plan->list_count
            : plan->ranges[i].end;
        for (uint32_t at = begin; at < end; at++) {
            uint32_t index = listed ? plan->list[at]
                : sheet->rule_index_entries[at];
            if (index >= sheet->count) return true;
            if (!sheet->rule_filters[index].discovery) continue;
            const StyleRule *rule = &sheet->rules[index];
            if (rule->pseudo != pseudo) continue;
            if (style_token_bloom_missing(
                    sheet->rule_filters[index].compound, plan->compound_bloom))
                continue;
            if (plan->ancestor_bloom_ready
                && style_token_bloom_missing(
                    sheet->rule_filters[index].ancestors, plan->ancestor_bloom))
                continue;
            if (!style_candidate_ancestor_token_possible(
                    sheet, index, node, &ancestor_tokens,
                    &ancestor_tokens_ready))
                continue;
            if (style_rule_selector_matches_subject(sheet, index, node, subject))
                return true;
        }
    }
    return false;
}

bool style_node_pseudo_rules_may_affect_discovery(const Stylesheet *sheet,
                                                  lxb_dom_node_t *node)
{
    if (sheet == NULL || node == NULL
        || node->type != LXB_DOM_NODE_TYPE_ELEMENT) return true;
    stylesheet_prepare_rule_index((Stylesheet *) sheet);
    if (!sheet->rule_index_ready || sheet->rule_index_entries == NULL
        || sheet->rule_filters == NULL
        || stylesheet_has_container_queries(sheet)) return true;
    StyleMatchSubject subject = style_match_subject(sheet, node);
    uint32_t list[STYLE_RULE_INDEX_LIST_CAPACITY];
    for (PseudoElement p = PSEUDO_BEFORE; p <= PSEUDO_AFTER; p++) {
        StyleRuleIndexPlan plan = style_rule_index_plan(
            sheet, &subject, p, list, STYLE_RULE_INDEX_LIST_CAPACITY);
        if (style_plan_has_discovery_rule(sheet, node, &subject, &plan, p))
            return true;
    }
    return false;
}

/* A multiplier, percentage or em line-height is parsed as negative
   thousandths and becomes device pixels here, once the element's own used
   font size is known. Shared by elements and pseudo-elements: a consumer sees
   either pixels, zero for `normal`, or STYLE_LINE_HEIGHT_ZERO. */
static void resolve_relative_line_height(ComputedStyle *style)
{
    if (style->line_height >= 0
        || style->line_height == STYLE_LINE_HEIGHT_ZERO) return;
    int64_t factor = -(int64_t) style->line_height;
    style->line_height_factor = (uint16_t) (factor > UINT16_MAX
                                            ? UINT16_MAX : factor);
    /* Computed unitless line-height can land just below an integer after
       its bounded thousandth representation (for example GOV.UK's
       21px * 1.190476...). Round the used device-pixel height instead of
       systematically shaving the final pixel from every such line. */
    style->line_height = style_multiply_add_divide(
        computed_style_font_size_fixed(style), -style->line_height,
        32000, 64000);
}

static ComputedStyle style_for_node_scoped(const Stylesheet *sheet,
                                           lxb_dom_node_t *node,
                                           const ComputedStyle *parent);

static void style_materialize_generated_scalar(
    const Stylesheet *sheet, ComputedStyle *style)
{
    if (style->generated_attr_length != STYLE_GENERATED_SCALAR_LENGTH) return;
    style->generated_text = style_store_generated_text(
        (Stylesheet *) sheet, style->generated_scalar, 3);
    memset(&style->generated_attr, 0, sizeof(style->generated_attr));
    style->generated_attr_length = 0;
    if (style->generated_text == NULL) style->generated_text_length = 0;
}

/* The whole resolution (cascade, retained modern properties, var() walks)
   is one class-token scope: those later steps test the same class lists
   against hundreds of candidate rules. */
ComputedStyle style_for_node(const Stylesheet *sheet, lxb_dom_node_t *node,
                             const ComputedStyle *parent)
{
    style_class_tokens_scope_begin(sheet);
    ComputedStyle style = style_for_node_scoped(sheet, node, parent);
    style_materialize_generated_scalar(sheet, &style);
    style_class_tokens_scope_end(sheet);
    return style;
}

static ComputedStyle style_for_node_scoped(const Stylesheet *sheet,
                                           lxb_dom_node_t *node,
                                           const ComputedStyle *parent)
{
    /* The rule index is deliberately lazy: stylesheet_add_css* invalidates
       it and the next resolution rebuilds it here (tests assert this
       contract).  The cast covers only that idempotent build. */
    if (sheet != NULL) stylesheet_prepare_rule_index((Stylesheet *) sheet);
    if (sheet != NULL) {
        style_relative_selector_cache_begin((Stylesheet *) sheet);
    }
    bool root_element = style_is_root_element(node);
    StyleResolveScratch saved_scratch = {0};
    if (sheet != NULL) {
        saved_scratch = *sheet->resolve_scratch;
        sheet->resolve_scratch->resolution_node = node;
        sheet->resolve_scratch->resolution_pseudo = PSEUDO_NONE;
        sheet->resolve_scratch->font_resolution_parent =
            root_element ? STYLE_DEFAULT_FONT_PX
            : (computed_style_font_size_fixed(parent) + 32) / 64;
        sheet->resolve_scratch->font_resolution_inherited =
            sheet->resolve_scratch->font_resolution_parent;
        sheet->resolve_scratch->font_resolution_root =
            root_element ? STYLE_DEFAULT_FONT_PX
            : (computed_style_root_font_size_fixed(parent) + 32) / 64;
        sheet->resolve_scratch->font_resolution_active = true;
        sheet->resolve_scratch->font_resolution_locked = false;
        sheet->resolve_scratch->font_ch_pending = false;
        sheet->resolve_scratch->font_relative_applied = false;
        sheet->resolve_scratch->non_font_relative_applied = false;
        sheet->resolve_scratch->logical_applied = false;
        sheet->resolve_scratch->logical_axes_locked = false;
        sheet->resolve_scratch->logical_axes_mismatch = false;
        sheet->resolve_scratch->logical_axes = 0;
        /* Style attributes and presentational declarations are document
           sources even if style resolution is nested beneath an external
           deferred declaration. */
        sheet->resolve_scratch->current_image_source_base = NULL;
        sheet->resolve_scratch->current_image_source_referrer_policy = NULL;
        sheet->resolve_scratch->current_image_source_slot = 0;
        style_container_units_for_node((Stylesheet *) sheet, node);
    }
    ComputedStyle style = style_apply_node_cascade(
        sheet, node, parent, root_element, true);
    if (sheet != NULL && sheet->resolve_scratch->logical_applied) {
        uint8_t final_axes = (uint8_t) (
            style.filter_code
            & (STYLE_WRITING_MODE_MASK | STYLE_DIRECTION_RTL));
        if (sheet->resolve_scratch->logical_axes_mismatch
            || sheet->resolve_scratch->logical_axes != final_axes) {
            sheet->resolve_scratch->logical_axes = final_axes;
            sheet->resolve_scratch->logical_axes_locked = true;
            sheet->resolve_scratch->logical_applied = false;
            sheet->resolve_scratch->logical_axes_mismatch = false;
            style = style_apply_node_cascade(
                sheet, node, parent, root_element, false);
        }
    }
    if (sheet != NULL
        && sheet->resolve_scratch->non_font_relative_applied) {
        ComputedStyle resolved_basis = style;
        resolve_font_size(&resolved_basis, parent, root_element);
        int final_basis =
            (computed_style_font_size_fixed(&resolved_basis) + 32) / 64;
        if (final_basis
            != sheet->resolve_scratch->font_resolution_parent) {
            /* Only nodes combining a non-font em/rem property with a changed
               computed font pay this second cascade. Locking the known final
               basis keeps the common inherited-font path single-pass while
               making cross-rule order irrelevant. */
            sheet->resolve_scratch->font_resolution_inherited = final_basis;
            sheet->resolve_scratch->font_resolution_locked = true;
            sheet->resolve_scratch->font_relative_applied = false;
            sheet->resolve_scratch->non_font_relative_applied = false;
            style = style_apply_node_cascade(
                sheet, node, parent, root_element, false);
        }
    }
    resolve_font_size(&style, parent, root_element);
    style_apply_modern_properties(
        sheet, node, parent, &style, root_element);
    resolve_sizing_em_lengths(&style);
    resolve_overflow_clip_margin(&style);
    resolve_overflow_axes(&style);
    int ua_block_margin = style.font_size;
    if ((style.margin.top == STYLE_LENGTH_NONE
         || style.margin.bottom == STYLE_LENGTH_NONE)
        && (style_tag_is(node, "h1") || style_tag_is(node, "h2"))) {
        unsigned ratio = style_tag_is(node, "h1") ? 67u : 83u;
        ua_block_margin = (int) (((int64_t) computed_style_font_size_fixed(&style)
                                 * ratio + 3200) / 6400);
    }
    if (style.margin.top == STYLE_LENGTH_NONE) {
        style.margin.top = ua_block_margin;
    }
    if (style.margin.right == STYLE_LENGTH_NONE) {
        style.margin.right = style.font_size;
    }
    if (style.margin.bottom == STYLE_LENGTH_NONE) {
        style.margin.bottom = ua_block_margin;
    }
    if (style.margin.left == STYLE_LENGTH_NONE) {
        style.margin.left = style.font_size;
    }
    resolve_relative_line_height(&style);
    /* Negative gap values retain an integral percentage until layout has
       the appropriate inline/block percentage basis. */
    unsigned grid_columns = computed_style_grid_column_count(&style);
    if (grid_columns > GRID_TRACK_REPEAT_LIMIT) {
        computed_style_set_grid_column_count(
            &style, GRID_TRACK_REPEAT_LIMIT);
    }
    if (style.grid_min_column_width < 0) style.grid_min_column_width = 0;
    if (style.border.top < 0) style.border.top = 0;
    if (style.border.right < 0) style.border.right = 0;
    if (style.border.bottom < 0) style.border.bottom = 0;
    if (style.border.left < 0) style.border.left = 0;
    if (style.border_color == UINT32_MAX) {
        style.border_color = style.color;
        style.border_alpha = style.color_alpha;
    }
    if (style.background_current_color) {
        style.background = style.color;
        style.background_alpha = style.color_alpha;
        style.has_background = style.color_alpha != 0;
    }
    /* Shadow composition: a host's light child that the flat tree leaves
       out (unassigned, or assigned to a slot that does not render) has no
       box. Like closed <details> content this is a rendering constraint,
       applied after author CSS; getComputedStyle reports it as display
       none, where a browser would report the cascaded value. */
    if (node != NULL && node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
        style.shadow_host =
            document_shadow_carrier_of_host(node) != NULL;
        if (parent != NULL && parent->shadow_host
            && style.display != DISPLAY_NONE) {
            const lxb_dom_node_t *carrier =
                document_shadow_carrier_of_host(node->parent);
            if (carrier != NULL && carrier != node
                && !document_shadow_light_child_rendered(carrier, node))
                style.display = DISPLAY_NONE;
        }
    }
    /* Closed disclosure content cannot be made visible by author display
       or positioning rules. This is a post-cascade rendering constraint. */
    if (node != NULL && node->parent != NULL
        && style_tag_is(node->parent, "details")
        && !lxb_dom_element_has_attribute(
               lxb_dom_interface_element(node->parent),
               (const lxb_char_t *) "open", 4)) {
        lxb_dom_node_t *first_summary = NULL;
        for (lxb_dom_node_t *child = node->parent->first_child;
             child != NULL; child = child->next) {
            if (style_tag_is(child, "summary")) {
                first_summary = child;
                break;
            }
        }
        if (node != first_summary) style.display = DISPLAY_NONE;
    }
    /* `open` is a boolean attribute: Lexbor reports no value for the
       common valueless `<dialog open>`, so test presence, not a value. */
    if ((style_tag_is(node, "dialog")
         && !lxb_dom_element_has_attribute(
                lxb_dom_interface_element(node),
                (const lxb_char_t *) "open", 4))
        || style_tag_is(node, "template")) {
        style.display = DISPLAY_NONE;
    }
    /* SVG metadata elements never generate a rendering box.  This is a
       semantic constraint, rather than a user-agent display declaration, so
       author CSS must not turn them into ordinary HTML flow content. */
    if (style_tag_is(node, "title") || style_tag_is(node, "desc")) {
        for (lxb_dom_node_t *ancestor = node == NULL ? NULL : node->parent;
             ancestor != NULL; ancestor = ancestor->parent) {
            if (style_tag_is(ancestor, "svg")) {
                style.display = DISPLAY_NONE;
                break;
            }
        }
    }
    if (style.min_width < 0) style.min_width = 0;
    if (style.min_height < 0) style.min_height = 0;
    if (style.max_width < 0 && style.max_width != STYLE_LENGTH_NONE
        && style.max_width != STYLE_LENGTH_MIN_CONTENT) {
        style.max_width = 0;
    }
    if (style.max_height < 0 && style.max_height != STYLE_LENGTH_NONE) {
        style.max_height = 0;
    }
    if (style.has_height && style.height < 0
        && style.height != STYLE_LENGTH_NONE) style.height = 0;
    if (style.clip_rect_empty
        && (style.out_of_flow || style.fixed_position)) style.hidden = true;
    if (style.has_transform && style.transform_scale_q14 == 0) {
        style.hidden = true;
    }
    style_compat_post_cascade(sheet, node, parent, &style);
    if (text_decoration_propagation_boundary(node, &style)) {
        style_set_ancestor_text_decoration(&style, false, 0);
    }
    if (sheet != NULL) {
        if (sheet->resolve_scratch->font_ch_pending) {
            style.filter_code |= STYLE_FONT_CH_PENDING;
            unsigned font_slot = font_family_web_slot(style.font_family);
            if (font_slot < TILEFINCH_WEB_FONT_FAMILY_LIMIT) {
                const WebFontSet *fonts = stylesheet_web_font_set(sheet);
                if (fonts != NULL) {
                    ((Stylesheet *)sheet)->metric_web_font_faces |=
                        (uint64_t)font_family_face_usage_mask(
                            &fonts->families[font_slot], computed_style_font_weight(&style))
                        << (font_slot * TILEFINCH_WEB_FONT_WEIGHT_LIMIT);
                    size_t primary = font_family_face_slot_at_weight(
                        &fonts->families[font_slot], computed_style_font_weight(&style), false);
                    if (primary != SIZE_MAX)
                        ((Stylesheet *)sheet)->metric_web_font_primary_faces |= UINT64_C(1)
                            << (font_slot * TILEFINCH_WEB_FONT_WEIGHT_LIMIT + primary);
                }
            }
        }
        *sheet->resolve_scratch = saved_scratch;
        style_relative_selector_cache_end((Stylesheet *) sheet);
    }
    return style;
}

ComputedStyle style_for_node_with_ch_basis(
    const Stylesheet *sheet, lxb_dom_node_t *node,
    const ComputedStyle *parent, int ch_basis)
{
    if (sheet == NULL || sheet->resolve_scratch == NULL || ch_basis <= 0) {
        return style_for_node(sheet, node, parent);
    }
    StyleResolveScratch saved = *sheet->resolve_scratch;
    sheet->resolve_scratch->font_ch_basis = ch_basis;
    sheet->resolve_scratch->font_ch_basis_active = true;
    ComputedStyle style = style_for_node(sheet, node, parent);
    *sheet->resolve_scratch = saved;
    style.filter_code &= ~STYLE_FONT_CH_PENDING;
    return style;
}

static ComputedStyle style_resolve_pseudo(const Stylesheet *sheet, lxb_dom_node_t *node,
                                        PseudoElement pseudo,
                                        const ComputedStyle *parent, bool layout_only)
{
    StyleMatchSubject subject = {0};
    StyleRuleIndexPlan index_plan = {0};
    uint32_t candidate_list[STYLE_RULE_INDEX_LIST_CAPACITY];
    const StyleRetainedMatchEntry *retained = NULL;
    StyleRetainedMatchEntry retained_copy;
    if (sheet != NULL && node != NULL && pseudo != PSEUDO_NONE) {
        retained = style_retained_lookup(sheet, node, pseudo);
        if (retained != NULL) {
            retained_copy = *retained;
            retained = &retained_copy;
        }
    }
    if (layout_only && sheet != NULL && node != NULL && pseudo != PSEUDO_NONE) {
        stylesheet_prepare_rule_index((Stylesheet *) sheet);
        if (retained != NULL) {
            if (style_retained_pseudo_absent(sheet, retained))
                return (ComputedStyle) {0};
        } else {
            if (style_pseudo_known_absent(sheet, node, pseudo))
                return (ComputedStyle) {0};
            subject = style_match_subject(sheet, node);
            index_plan = style_rule_index_plan(
                sheet, &subject, pseudo, candidate_list,
                STYLE_RULE_INDEX_LIST_CAPACITY);
            /* This early exit belongs only to layout. Computed-style callers
               still receive inherited/default values for an absent pseudo.
               No candidate rule exists, so the exact retained list is
               empty: keep that answer across builds. */
            if (index_plan.ready && index_plan.count == 0
                && index_plan.list_count == 0) {
                if (style_retained_begin(sheet)) {
                    style_retained_commit(sheet, node, pseudo);
                }
                return (ComputedStyle) {0};
            }
        }
    }
    ComputedStyle style = {.display = DISPLAY_INLINE,
                           .color = parent != NULL ? parent->color : 0x000000,
                           .color_alpha = parent != NULL
                                          ? parent->color_alpha : 255,
                           .background_alpha = 255,
                           .opacity = 255,
                           .transform_scale_q14 = 16384,
                           .font_scale = parent != NULL ? parent->font_scale : 2,
                           .font_size = parent != NULL ? parent->font_size : 16,
                           .font_size_fraction = parent != NULL
                               ? parent->font_size_fraction : 0,
                           .root_font_size = parent != NULL
                                             && parent->root_font_size != 0
                                             ? parent->root_font_size : 16,
                           .root_font_size_fraction = parent != NULL
                               ? parent->root_font_size_fraction : 0,
                           .font_family = parent != NULL
                                          ? parent->font_family : FONT_SANS,
                           .font_weight = parent != NULL
                                          ? parent->font_weight : 400,
                           .font_bold = parent != NULL && parent->font_bold,
                           .word_spacing = parent != NULL
                                           ? parent->word_spacing : 0,
                           .letter_spacing_fixed = parent != NULL
                                             ? parent->letter_spacing_fixed : 0,
                           .white_space_mode = parent != NULL
                                               ? parent->white_space_mode
                                               : WHITE_SPACE_NORMAL,
                           .overflow_wrap = parent != NULL
                                            ? parent->overflow_wrap
                                            : OVERFLOW_WRAP_NORMAL,
                           .word_break_mode = parent != NULL
                                              ? parent->word_break_mode
                                              : WORD_BREAK_NORMAL,
                           .border_color = UINT32_MAX,
                           .border_alpha = 255,
                           .max_width = STYLE_LENGTH_NONE,
                           .max_height = STYLE_LENGTH_NONE,
                           .object_position_x =
                               style_object_position_encode(50, 0),
                           .object_position_y =
                               style_object_position_encode(50, 0),
                           .flex_direction = FLEX_ROW,
                           .flex_shrink = 512};
    if (parent != NULL) {
        style.filter_code = (uint8_t) (
            (style.filter_code
             & ~(STYLE_WRITING_MODE_MASK | STYLE_DIRECTION_RTL))
            | (parent->filter_code
               & (STYLE_WRITING_MODE_MASK | STYLE_DIRECTION_RTL)));
        unsigned inherited_offset = style_text_offset_code(
            parent, STYLE_TEXT_OFFSET_SHIFT, STYLE_TEXT_OFFSET_MASK);
        style_set_text_offset_code(&style, inherited_offset,
                                   STYLE_TEXT_OFFSET_SHIFT,
                                   STYLE_TEXT_OFFSET_MASK);
        if (computed_style_has_text_underline(parent)) {
            style_set_ancestor_text_decoration(
                &style, true, inherited_offset);
        } else if (computed_style_has_ancestor_text_underline(parent)) {
            style_set_ancestor_text_decoration(
                &style, true, style_text_offset_code(
                    parent, STYLE_ANCESTOR_TEXT_OFFSET_SHIFT,
                    STYLE_ANCESTOR_TEXT_OFFSET_MASK));
        }
        inherit_text_shadow(sheet, &style, parent);
    }
    if (pseudo == PSEUDO_PLACEHOLDER) style.color = 0x70757a;
    if (sheet == NULL || node == NULL || pseudo == PSEUDO_NONE) return style;
    if (!layout_only) stylesheet_prepare_rule_index((Stylesheet *) sheet);
    style_relative_selector_cache_begin((Stylesheet *) sheet);
    StyleResolveScratch saved_scratch = *sheet->resolve_scratch;
    sheet->resolve_scratch->resolution_node = node;
    sheet->resolve_scratch->resolution_pseudo = pseudo;
    sheet->resolve_scratch->font_resolution_inherited =
        (computed_style_font_size_fixed(parent) + 32) / 64;
    sheet->resolve_scratch->font_resolution_root =
        (computed_style_root_font_size_fixed(parent) + 32) / 64;
    sheet->resolve_scratch->font_resolution_active = true;
    sheet->resolve_scratch->current_image_source_base = NULL;
    sheet->resolve_scratch->current_image_source_referrer_policy = NULL;
    sheet->resolve_scratch->current_image_source_slot = 0;
    if (!layout_only && retained == NULL) {
        subject = style_match_subject(sheet, node);
        index_plan = style_rule_index_plan(
            sheet, &subject, pseudo, candidate_list,
            STYLE_RULE_INDEX_LIST_CAPACITY);
    }
    bool record = retained == NULL && style_retained_begin(sheet);
    for (size_t phase = 0; phase < 4; phase++) {
        style_apply_matching_range(
            sheet, parent, node, &subject, &index_plan, pseudo,
            sheet->cascade_starts[phase],
            sheet->cascade_ends[phase], &style, false, retained, record);
    }
    if (record) style_retained_commit(sheet, node, pseudo);
    resolve_font_size(&style, parent, false);
    resolve_sizing_em_lengths(&style);
    resolve_relative_line_height(&style);
    resolve_overflow_clip_margin(&style);
    style_materialize_generated_scalar(sheet, &style);
    if (!style.generated_expression && style.generated_attr != NULL
        && style.generated_attr_length != 0) {
        size_t attribute_length = 0;
        const char *attribute = document_attribute(
            node, style.generated_attr, &attribute_length);
        if (attribute != NULL && attribute_length != 0) {
            if (attribute_length >= STYLE_GENERATED_TEXT_CAPACITY) {
                attribute_length = STYLE_GENERATED_TEXT_CAPACITY - 1;
            }
            style.generated_text = attribute;
            style.generated_text_length = (uint8_t) attribute_length;
        } else {
            style.generated_text = NULL;
            style.generated_text_length = 0;
        }
    }
    if (style.border_color == UINT32_MAX) {
        style.border_color = style.color;
        style.border_alpha = style.color_alpha;
    }
    if (style.background_current_color) {
        style.background = style.color;
        style.background_alpha = style.color_alpha;
        style.has_background = style.color_alpha != 0;
    }
    if (text_decoration_propagation_boundary(NULL, &style)) {
        style_set_ancestor_text_decoration(&style, false, 0);
    }
    *sheet->resolve_scratch = saved_scratch;
    style_relative_selector_cache_end((Stylesheet *) sheet);
    return style;
}

ComputedStyle style_for_pseudo(const Stylesheet *sheet, lxb_dom_node_t *node,
                               PseudoElement pseudo, const ComputedStyle *parent)
{
    style_class_tokens_scope_begin(sheet);
    ComputedStyle style = style_resolve_pseudo(
        sheet, node, pseudo, parent, false);
    style_class_tokens_scope_end(sheet);
    return style;
}

ComputedStyle style_for_layout_pseudo(const Stylesheet *sheet, lxb_dom_node_t *node,
                                      PseudoElement pseudo, const ComputedStyle *parent)
{
    style_class_tokens_scope_begin(sheet);
    ComputedStyle style = style_resolve_pseudo(
        sheet, node, pseudo, parent, true);
    style_class_tokens_scope_end(sheet);
    return style;
}
