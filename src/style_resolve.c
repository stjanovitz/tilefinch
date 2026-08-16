/* Per-node style resolution: UA defaults, presentational attributes,
   cascade application over matched rules, font-size resolution, and the
   public style_for_node / style_for_pseudo entry points.
   Split out of style.c. */

#include "style_internal.h"
#include "style_cache_internal.h"
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
                                  char output[96])
{
    return style_retained_property_value(
        sheet, node, name, strlen(name), output, 96);
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

static bool modern_scale_value(const char *value, uint8_t *scale_q6)
{
    if (modern_keyword(value, "none")) {
        *scale_q6 = 64;
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
    double fixed = first * 64.0;
    if (fixed > 255.0) fixed = 255.0;
    *scale_q6 = (uint8_t) (fixed + 0.5);
    return true;
}

static bool modern_rotate_value(const char *value, uint8_t *quadrants)
{
    if (modern_keyword(value, "none")) {
        *quadrants = 0;
        return true;
    }
    while (isspace((unsigned char) *value)) value++;
    if ((value[0] == 'z' || value[0] == 'Z')
        && isspace((unsigned char) value[1])) {
        value += 2;
        while (isspace((unsigned char) *value)) value++;
    }
    char *end = NULL;
    double angle = strtod(value, &end);
    if (end == value || !isfinite(angle)) return false;
    if (strncasecmp(end, "deg", 3) == 0) end += 3;
    else if (strncasecmp(end, "turn", 4) == 0) {
        angle *= 360.0;
        end += 4;
    } else if (strncasecmp(end, "grad", 4) == 0) {
        angle *= 0.9;
        end += 4;
    } else if (strncasecmp(end, "rad", 3) == 0) {
        angle *= 57.29577951308232;
        end += 3;
    } else if (angle != 0.0) return false;
    while (isspace((unsigned char) *end)) end++;
    if (*end != '\0') return false;
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
    uint16_t mask = sheet->modern_property_mask;
    char value[96];

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
        if (modern_length_component(&cursor, &x, &xp)) {
            while (isspace((unsigned char) *cursor)) cursor++;
            if (*cursor != '\0'
                && !modern_length_component(&cursor, &y, &yp)) {
                x = y = 0;
            }
            while (isspace((unsigned char) *cursor)) cursor++;
            if (*cursor == '\0') {
                bool compatible_x = style->transform_x == 0
                    || style->transform_x_percent == xp;
                bool compatible_y = style->transform_y == 0
                    || style->transform_y_percent == yp;
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
        uint8_t scale = 64;
        if (modern_scale_value(value, &scale)) {
            unsigned combined = (unsigned) style->transform_scale_q6 * scale;
            style->transform_scale_q6 = (uint8_t) (
                combined / 64u > 255u ? 255u : combined / 64u);
            style->has_transform |= scale != 64;
        }
    }
    if ((mask & STYLE_MODERN_ROTATE) != 0
        && retained_modern_value(sheet, node, "rotate", value)) {
        uint8_t quadrants = 0;
        if (modern_rotate_value(value, &quadrants)) {
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
    value = document_attribute(node, "width", &length);
    if (value != NULL && length != 0) {
        style->width = style_parse_length(sheet, value, length, 0,
                                    &style->width_percent);
        style->has_width = style->width > 0;
    }
    value = document_attribute(node, "height", &length);
    if (value != NULL && length != 0) {
        style->height = style_parse_length(sheet, value, length, 0,
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
                || style_element_name_is(name, length, "hr");
        case 3:
            return style_element_name_is(name, length, "nav")
                || style_element_name_is(name, length, "div")
                || style_element_name_is(name, length, "pre");
        case 4:
            return style_element_name_is(name, length, "html")
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
                || style_element_name_is(name, length, "article")
                || style_element_name_is(name, length, "details")
                || style_element_name_is(name, length, "summary");
        case 8:
            return style_element_name_is(name, length, "fieldset");
        case 10:
            return style_element_name_is(name, length, "figcaption");
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
                           .transform_scale_q6 = 64,
                           .min_width_auto = true,
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
        style.letter_spacing = parent->letter_spacing;
        style.line_height = parent->line_height;
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
    if (NODE_TAG_IS("button")) {
        /* Replaced form controls are atomic inline-level boxes in the UA
           display sheet.  Treating them as ordinary inline containers lets
           large text indents and positioned icons leak into surrounding
           line layout. */
        style.display = DISPLAY_INLINE_BLOCK;
    }
    if (NODE_TAG_IS("video") || NODE_TAG_IS("iframe")) {
        /* Both are replaced inline-level elements.  Hiding video in the
           historical UA defaults also hid its poster and made intrinsic
           media sizing impossible before playback began. */
        style.display = DISPLAY_INLINE_BLOCK;
    }
    if (NODE_TAG_IS("input") || NODE_TAG_IS("textarea")
        || NODE_TAG_IS("select") || NODE_TAG_IS("button")) {
        style.box_sizing_border_box = true;
    }
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
    if (NODE_TAG_IS("script") || NODE_TAG_IS("style")
        || NODE_TAG_IS("head") || NODE_TAG_IS("meta")
        || NODE_TAG_IS("link") || NODE_TAG_IS("title")
        || NODE_TAG_IS("source")
        || NODE_TAG_IS("canvas") || NODE_TAG_IS("noscript")) {
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
    }
    if (NODE_TAG_IS("button")) {
        style.background = 0xefefef;
        style.padding.left = 6;
        style.padding.right = 6;
    }
    if (NODE_TAG_IS("select")) {
        /* Reserve the native menulist affordance without changing authored
           padding once the author cascade runs. */
        style.padding.right = 20;
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
        style.margin = (StyleEdges) {.top = 5, .bottom = 5};
    }
    else if (NODE_TAG_IS("h2") || NODE_TAG_IS("h3")) {
        style.font_scale = 2; style.font_size = 20;
        style.font_size_fraction = 0;
        style.font_weight = 700; style.font_bold = true;
        style.margin = (StyleEdges) {.top = 4, .bottom = 4};
    }
    if (NODE_TAG_IS("code") || NODE_TAG_IS("pre")) {
        style.font_family = FONT_MONOSPACE;
    }
    if (NODE_TAG_IS("b") || NODE_TAG_IS("strong")) {
        style.font_weight = 700;
        style.font_bold = true;
    }
    if (NODE_TAG_IS("a")) {
        size_t href_length = 0;
        if (document_attribute(node, "href", &href_length) != NULL) {
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

typedef struct {
    StyleRuleIndexRange ranges[STYLE_RULE_INDEX_MAX_SOURCES];
    size_t count;
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
        style_token_bloom_merge(
            &bloom, style_compound_token_bloom(
                STYLE_SELECTOR_TAG, subject->tag, subject->tag_length));
        style_token_bloom_merge(
            &bloom, style_compound_tag_id_bloom(subject->tag_id));
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

static StyleTokenBloom style_match_ancestor_bloom(
    const StyleMatchSubject *subject)
{
    lxb_dom_node_t *at = subject == NULL || subject->node == NULL
        ? NULL : subject->node->parent;
    StyleTokenBloom bloom = style_token_bloom_empty();
    size_t visits = 0;
    for (; at != NULL && visits < 64u; at = at->parent, visits++) {
        if (at->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        StyleMatchSubject ancestor = {0};
        style_match_subject_prepare(at, &ancestor);
        style_token_bloom_merge(
            &bloom, style_match_subject_bloom(&ancestor));
    }
    /* Truncation must fail open: a required token may exist above the bound. */
    if (at == NULL) return bloom;
    return (StyleTokenBloom) {{UINT32_MAX, UINT32_MAX}};
}

static StyleMatchSubject style_match_subject(
    const Stylesheet *sheet, lxb_dom_node_t *node)
{
    (void) sheet;
    StyleMatchSubject subject = {0};
    style_match_subject_prepare(node, &subject);
    return subject;
}

static bool rule_fast_matches(const StyleRule *rule,
                              const StyleMatchSubject *subject)
{
    if (!rule->has_fast_key) return true;
    const char *fast_key = style_rule_fast_key(rule);
    if (fast_key == NULL) return true;
    size_t length = rule->fast_key_length;
    if (rule->type == SELECTOR_ID) {
        return subject->id != NULL && subject->id_length == length
               && memcmp(subject->id, fast_key, length) == 0;
    }
    if (rule->type == SELECTOR_CLASS) {
        return subject->classes != NULL
               && class_contains_length(subject->classes,
                                        subject->classes_length, fast_key,
                                        length);
    }
    return subject->tag != NULL && subject->tag_length == length
           && memcmp(subject->tag, fast_key, length) == 0;
}

static bool rule_matches(const Stylesheet *sheet, size_t rule_index,
                         const StyleRule *rule, lxb_dom_node_t *node,
                         const StyleMatchSubject *subject)
{
    return style_container_query_matches(
               sheet, style_rule_container_query(rule), node)
           && rule_fast_matches(rule, subject)
           && style_rule_selector_matches_subject(
               sheet, rule_index, node, subject);
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

static bool style_rule_add_key_source(const Stylesheet *sheet,
                                      StyleRuleIndexPlan *plan,
                                      SelectorType type, const char *text,
                                      size_t length)
{
    if (text == NULL || length == 0) return true;
    StyleRuleIndexBucket *bucket = style_rule_find_bucket(
        sheet, type, text, length, false);
    return bucket == NULL || style_rule_add_range(
        plan, bucket->first, bucket->first + bucket->count);
}

static StyleRuleIndexPlan style_rule_index_plan(
    const Stylesheet *sheet, const StyleMatchSubject *subject)
{
    StyleRuleIndexPlan plan = {0};
    if (!sheet->rule_index_ready || sheet->rule_index_entries == NULL) {
        return plan;
    }
    if (!style_rule_add_range(
            &plan, 0, (uint32_t) sheet->rule_index_universal_count)
        || !style_rule_add_key_source(
            sheet, &plan, SELECTOR_TAG, subject->tag, subject->tag_length)
        || !style_rule_add_key_source(
            sheet, &plan, SELECTOR_ID, subject->id,
            subject->id_length)) return (StyleRuleIndexPlan) {0};
    size_t at = 0;
    while (subject->classes != NULL && at < subject->classes_length) {
        while (at < subject->classes_length
               && isspace((unsigned char) subject->classes[at])) at++;
        size_t begin = at;
        while (at < subject->classes_length
               && !isspace((unsigned char) subject->classes[at])) at++;
        if (at != begin && !style_rule_add_key_source(
                sheet, &plan, SELECTOR_CLASS, subject->classes + begin,
                at - begin)) {
            return (StyleRuleIndexPlan) {0};
        }
    }
    plan.compound_bloom = style_match_subject_bloom(subject);
    size_t candidate_upper_bound = 0;
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
        && (sheet->count <= STYLE_RULE_ANCESTOR_FILTER_SMALL_SHEET
            || candidate_upper_bound
                >= STYLE_RULE_ANCESTOR_FILTER_MIN_CANDIDATES)) {
        plan.ancestor_bloom = style_match_ancestor_bloom(subject);
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
    size_t class_length = 0;
    const char *class_name = document_attribute(node, "class",
                                                &class_length);
    if (class_name == NULL
        || strstr(class_name, "wm-fallback-layout") == NULL) return;
    fprintf(stderr,
            "style-position-match selector=%s fixed=%d out=%d relative=%d\n",
            rule->selector, declaration->values.fixed_position,
            declaration->values.out_of_flow,
            declaration->values.relative_position);
}

static void style_apply_matching_range(const Stylesheet *sheet,
                                       const ComputedStyle *parent,
                                       lxb_dom_node_t *node,
                                       const StyleMatchSubject *subject,
                                       const StyleRuleIndexPlan *plan,
                                       PseudoElement pseudo,
                                       size_t start, size_t end,
                                       ComputedStyle *style,
                                       bool trace_position)
{
    if (sheet == NULL || start >= end) return;
    /* Diagnostics counters only; the match result never depends on them. */
    Stylesheet *mutable_sheet = (Stylesheet *) sheet;
    mutable_sheet->rule_index_queries++;
    StyleRuleIndexSource sources[STYLE_RULE_INDEX_MAX_SOURCES];
    size_t source_count = plan != NULL && plan->ready ? plan->count : 0;
    if (source_count == 0) {
        mutable_sheet->rule_index_fallbacks++;
        mutable_sheet->rule_index_candidates += end - start;
        for (size_t i = start; i < end; i++) {
            const StyleRule *rule = &sheet->rules[i];
            if (rule->pseudo != pseudo
                || !rule_matches(sheet, i, rule, node, subject)) {
                continue;
            }
            if (trace_position) style_trace_position_match(sheet, rule, node);
            apply_style_rule(sheet, rule, style, parent);
        }
        return;
    }
    for (size_t i = 0; i < source_count; i++) {
        sources[i] = (StyleRuleIndexSource) {
            .at = style_rule_range_lower_bound(
                sheet->rule_index_entries, plan->ranges[i].begin,
                plan->ranges[i].end, start),
            .end = plan->ranges[i].end
        };
    }

    uint32_t previous = STYLE_RULE_INDEX_EMPTY;
    for (;;) {
        uint32_t candidate = STYLE_RULE_INDEX_EMPTY;
        for (size_t i = 0; i < source_count; i++) {
            if (sources[i].at >= sources[i].end) continue;
            uint32_t index = sheet->rule_index_entries[sources[i].at];
            if ((size_t) index >= end) continue;
            if (candidate == STYLE_RULE_INDEX_EMPTY || index < candidate) {
                candidate = index;
            }
        }
        if (candidate == STYLE_RULE_INDEX_EMPTY) break;
        for (size_t i = 0; i < source_count; i++) {
            while (sources[i].at < sources[i].end
                   && sheet->rule_index_entries[sources[i].at]
                        == candidate) {
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
        if (sheet->rule_filters != NULL
            && style_token_bloom_missing(
                sheet->rule_filters[candidate].compound,
                plan->compound_bloom)) {
#ifndef TILEFINCH_NO_TRACE
            mutable_sheet->rule_compound_filter_rejections++;
#endif
            continue;
        }
        if (plan->ancestor_bloom_ready
            && style_token_bloom_missing(
                sheet->rule_filters[candidate].ancestors,
                plan->ancestor_bloom)) {
#ifndef TILEFINCH_NO_TRACE
            mutable_sheet->rule_ancestor_filter_rejections++;
#endif
            continue;
        }
        const StyleRule *rule = &sheet->rules[candidate];
        /* Every indexed rule came either from the universal range or from a
           tag/id/class bucket already selected for this subject. Repeating
           rule_fast_matches here only rescans the element's class list. */
        if (rule->pseudo != pseudo
            || !style_container_query_matches(
                sheet, style_rule_container_query(rule), node)
            || !style_rule_selector_matches_subject(
                sheet, candidate, node, subject)) {
            continue;
        }
        if (trace_position) style_trace_position_match(sheet, rule, node);
        apply_style_rule(sheet, rule, style, parent);
    }
}

static void apply_paint_values(Stylesheet *sheet, ComputedStyle *style,
                               const ComputedStyle *values,
                               uint64_t mask, uint64_t mask_high)
{
    uint64_t paint_high = S2_BACKGROUND_LAYERS | S2_BACKGROUND_BOX
        | S2_BACKGROUND_POSITION
        | S2_MASK_POSITION | S2_MASK_REPEAT | S2_MASK_SIZE
        | S2_BOX_SHADOW;
    if ((mask & (S_BACKGROUND_IMAGE | S_MASK_IMAGE | S_BACKGROUND_SIZE)) == 0
        && (mask_high & paint_high) == 0) {
        return;
    }
    const StylePaintStack *current = stylesheet_paint_stack(
        sheet, computed_style_paint_stack_id(style));
    const StylePaintStack *incoming = stylesheet_paint_stack(
        sheet, computed_style_paint_stack_id(values));
    if (incoming == NULL) {
        if ((mask & S_MASK_IMAGE) != 0) style->mask_image = NULL;
        if ((mask & S_BACKGROUND_IMAGE) == 0
            && (mask_high & S2_BOX_SHADOW) == 0) return;
    }
    StylePaintStack merged = current == NULL
        ? (StylePaintStack) {0} : *current;
    if ((mask_high & S2_BACKGROUND_LAYERS) != 0
        || (mask & S_BACKGROUND_IMAGE) != 0) {
        if (incoming != NULL
            && (incoming->components
                & STYLE_PAINT_COMPONENT_BACKGROUND_IMAGE) != 0) {
            memcpy(merged.backgrounds, incoming->backgrounds,
                   sizeof(merged.backgrounds));
            merged.background_count = incoming->background_count;
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
        for (size_t i = 0; i < count
                           && i < STYLE_PAINT_LAYER_LIMIT; i++) {
            merged.backgrounds[i].position_x =
                incoming->backgrounds[i].position_x;
            merged.backgrounds[i].position_y =
                incoming->backgrounds[i].position_y;
            merged.backgrounds[i].position_edges =
                incoming->backgrounds[i].position_edges;
            merged.backgrounds[i].flags = (uint8_t) (
                (merged.backgrounds[i].flags
                 & ~STYLE_BACKGROUND_POSITION_PIXELS)
                | (incoming->backgrounds[i].flags
                   & STYLE_BACKGROUND_POSITION_PIXELS));
        }
    }
    if (incoming != NULL && (mask & S_BACKGROUND_SIZE) != 0) {
        size_t count = incoming->background_count;
        if (count == 0) count = 1;
        if (merged.background_count < count) {
            merged.background_count = (uint8_t) count;
        }
        for (size_t i = 0; i < count
                           && i < STYLE_PAINT_LAYER_LIMIT; i++) {
            merged.backgrounds[i].width =
                incoming->backgrounds[i].width;
            merged.backgrounds[i].height =
                incoming->backgrounds[i].height;
            merged.backgrounds[i].fit =
                incoming->backgrounds[i].fit;
            merged.backgrounds[i].flags = (uint8_t) (
                (merged.backgrounds[i].flags
                 & ~(STYLE_BACKGROUND_SIZE_EXPLICIT
                     | STYLE_BACKGROUND_WIDTH_AUTO
                     | STYLE_BACKGROUND_HEIGHT_AUTO
                     | STYLE_BACKGROUND_WIDTH_PERCENT
                     | STYLE_BACKGROUND_HEIGHT_PERCENT))
                | (incoming->backgrounds[i].flags
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
                         uint64_t mask, uint64_t mask_high)
{
    apply_paint_values(sheet, style, values, mask, mask_high);
    if (mask & S_DISPLAY) style->display = values->display;
    if (mask & S_COLOR) {
        style->color = values->color;
        style->color_alpha = values->color_alpha;
    }
    if (mask & S_BACKGROUND) {
        style->background = values->background;
        style->background_alpha = values->background_alpha;
        style->has_background = values->has_background;
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
        style->background_size_flags = values->background_size_flags;
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
        style->letter_spacing = values->letter_spacing;
    }
    if (mask & S_LINE_HEIGHT) style->line_height = values->line_height;
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
        style->word_break_mode = values->word_break_mode;
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
        if (clamp_authored) style->line_clamp = values->line_clamp;
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
        style->unicode_bidi_override = values->unicode_bidi_override;
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
        style->transform_scale_q6 = values->transform_scale_q6;
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
        style->text_indent = values->text_indent;
    }
    if (mask_high & S2_TEXT_DECORATION) {
        style_set_text_underline(
            style, computed_style_has_text_underline(values));
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
        style->generated_attr = values->generated_attr;
        style->generated_expression = values->generated_expression;
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
        style->line_height = parent->line_height;
    }
    if (inherit_mask & S_WHITE_SPACE) {
        style->white_space_mode = parent->white_space_mode;
    }
    if (inherit_mask & S_Z_INDEX) {
        style->has_z_index = parent->has_z_index;
        style->z_index = parent->z_index;
    }
    if (inherit_mask & S_COLOR) {
        style->color = parent->color;
        style->color_alpha = parent->color_alpha;
    }
    if (inherit_mask & S_BACKGROUND) {
        style->has_background = parent->has_background;
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
    apply_values((Stylesheet *) sheet, style,
                 &declaration->values,
                 declaration->mask & ~revert_rule_mask,
                 declaration->mask_high & ~revert_rule_mask_high);
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
        parsed = style_apply_deferred_program(
            mutable_sheet, declaration->deferred_declarations,
            declaration->deferred_length,
            declaration->deferred_program_offset,
            declaration->deferred_program_count, rule->important,
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
            parsed = style_apply_deferred_program(
                mutable_sheet, declaration->deferred_declarations,
                declaration->deferred_length,
                declaration->deferred_program_offset,
                declaration->deferred_program_count, rule->important,
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
        apply_values((Stylesheet *) sheet, style, &resolved, mask, mask_high);
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

static ComputedStyle style_apply_node_cascade(
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

    StyleMatchSubject subject = style_match_subject(sheet, node);
    StyleRuleIndexPlan index_plan = style_rule_index_plan(sheet, &subject);
    style_apply_matching_range(
        sheet, parent, node, &subject, &index_plan, PSEUDO_NONE,
        sheet->cascade_starts[CASCADE_AUTHOR_NORMAL],
        sheet->cascade_ends[CASCADE_AUTHOR_NORMAL], &style, trace_position);
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
    ComputedStyle inline_important_values = style;
    if (inline_css != NULL) {
        if (style_text_has_logical_property(inline_css, length)) {
            style_record_logical_axes(sheet, &style);
        }
        if (style_text_has_font_relative_unit(inline_css, length)) {
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
        apply_values((Stylesheet *) sheet, &style, &values, mask, mask_high);
        apply_inherit_mask(
            (Stylesheet *) sheet, &style, parent, inherit_mask);
    }
    style_apply_matching_range(
        sheet, parent, node, &subject, &index_plan, PSEUDO_NONE,
        sheet->cascade_starts[CASCADE_USER_NORMAL],
        sheet->cascade_ends[CASCADE_USER_NORMAL], &style, false);
    style_apply_matching_range(
        sheet, parent, node, &subject, &index_plan, PSEUDO_NONE,
        sheet->cascade_starts[CASCADE_AUTHOR_IMPORTANT],
        sheet->cascade_ends[CASCADE_AUTHOR_IMPORTANT], &style, false);
    if (inline_css != NULL) {
        apply_values((Stylesheet *) sheet, &style,
                     &inline_important_values,
                     inline_important_mask, inline_important_mask_high);
        apply_inherit_mask(
            (Stylesheet *) sheet, &style, parent,
            inline_important_inherit);
    }
    style_apply_matching_range(
        sheet, parent, node, &subject, &index_plan, PSEUDO_NONE,
        sheet->cascade_starts[CASCADE_USER_IMPORTANT],
        sheet->cascade_ends[CASCADE_USER_IMPORTANT], &style, false);
    return style;
}

ComputedStyle style_for_node(const Stylesheet *sheet, lxb_dom_node_t *node,
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
    if (style.margin.top == STYLE_LENGTH_NONE) {
        style.margin.top = style.font_size;
    }
    if (style.margin.right == STYLE_LENGTH_NONE) {
        style.margin.right = style.font_size;
    }
    if (style.margin.bottom == STYLE_LENGTH_NONE) {
        style.margin.bottom = style.font_size;
    }
    if (style.margin.left == STYLE_LENGTH_NONE) {
        style.margin.left = style.font_size;
    }
    if (style.line_height < 0) {
        /* Computed unitless line-height can land just below an integer after
           its bounded thousandth representation (for example GOV.UK's
           21px * 1.190476...). Round the used device-pixel height instead of
           systematically shaving the final pixel from every such line. */
        style.line_height = style_multiply_add_divide(
            computed_style_font_size_fixed(&style), -style.line_height,
            32000, 64000);
    }
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
    size_t open_length = 0;
    if ((style_tag_is(node, "dialog")
         && document_attribute(node, "open", &open_length) == NULL)
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
    if (style.has_transform && style.transform_scale_q6 == 0) {
        style.hidden = true;
    }
    /* A common no-script/mobile bootstrap pattern stages a non-interactive
       full-viewport hero at display:none and opacity:0, then reveals it only
       after a script-side image preload. On a bounded runtime where that
       optional bootstrap fails, the page otherwise loses its entire visual
       backdrop. Treat only a URL-backed, zero-inset, non-elevated fixed
       backdrop as final-state content; modal/dialog surfaces normally carry a
       positive stacking level and do not match this narrow compatibility
       fallback. */
    if (style.display == DISPLAY_NONE && style.opacity == 0
        && style.fixed_position && style.background_image_kind
               == STYLE_BACKGROUND_IMAGE_URL
        && style.background_image != NULL
        && style.has_top && style.has_right
        && style.has_bottom && style.has_left
        && style.top == 0 && style.right == 0
        && style.bottom == 0 && style.left == 0
        && (!style.has_z_index || style.z_index <= 0)) {
        style.display = DISPLAY_BLOCK;
        style.opacity = 255;
        /* This fallback is specifically a backdrop. Keep it below ordinary
           page content even though fixed layers are cached separately from
           their DOM-order tile content. */
        style.has_z_index = true;
        style.z_index = -1;
    }
    if (text_decoration_propagation_boundary(node, &style)) {
        style_set_ancestor_text_decoration(&style, false, 0);
    }
    if (sheet != NULL) {
        if (sheet->resolve_scratch->font_ch_pending) {
            style.filter_code |= STYLE_FONT_CH_PENDING;
        }
        *sheet->resolve_scratch = saved_scratch;
        style_relative_selector_cache_end((Stylesheet *) sheet);
    }
    return style;
}

static bool focus_token_at(const char *text, size_t length, size_t at,
                           size_t *token_length, bool *within)
{
    static const char focus[] = ":focus";
    if (text == NULL || at + sizeof(focus) - 1 > length
        || memcmp(text + at, focus, sizeof(focus) - 1) != 0) return false;
    size_t used = sizeof(focus) - 1;
    bool focus_within = false;
    if (at + used + 8 <= length
        && memcmp(text + at + used, "-visible", 8) == 0) {
        used += 8;
    } else if (at + used + 7 <= length
               && memcmp(text + at + used, "-within", 7) == 0) {
        used += 7;
        focus_within = true;
    }
    if (at + used < length) {
        unsigned char next = (unsigned char) text[at + used];
        if (isalnum(next) || next == '_' || next == '-') return false;
    }
    if (token_length != NULL) *token_length = used;
    if (within != NULL) *within = focus_within;
    return true;
}

static bool focus_token_inside_has(const char *text, size_t token_at)
{
    bool has_stack[16] = {0};
    size_t depth = 0;
    char quote = '\0';
    unsigned brackets = 0;
    for (size_t at = 0; at < token_at; at++) {
        char c = text[at];
        if (quote != '\0') {
            if (c == quote && (at == 0 || text[at - 1] != '\\')) quote = '\0';
            continue;
        }
        if (c == '\'' || c == '"') {
            quote = c;
            continue;
        }
        if (c == '[') {
            brackets++;
            continue;
        }
        if (c == ']' && brackets != 0) {
            brackets--;
            continue;
        }
        if (brackets != 0) continue;
        if (c == '(') {
            bool is_has = at >= 4
                && memcmp(text + at - 4, ":has", 4) == 0;
            if (depth < sizeof(has_stack) / sizeof(has_stack[0])) {
                has_stack[depth] = is_has;
            }
            depth++;
        } else if (c == ')' && depth != 0) {
            depth--;
        }
    }
    size_t bounded = depth < sizeof(has_stack) / sizeof(has_stack[0])
        ? depth : sizeof(has_stack) / sizeof(has_stack[0]);
    for (size_t i = 0; i < bounded; i++) {
        if (has_stack[i]) return true;
    }
    return false;
}

static bool focus_selector_probe_range(
    const char *text, size_t length, size_t token_at,
    size_t *probe_at, size_t *probe_length)
{
    if (text == NULL || token_at >= length
        || probe_at == NULL || probe_length == NULL) return false;
    size_t start = 0, end = length;
    size_t depth = 0, focus_depth = 0;
    size_t nested_start[16] = {0};
    unsigned brackets = 0;
    char quote = '\0';
    bool found = false;
    bool focus_in_has = focus_token_inside_has(text, token_at);
    for (size_t at = 0; at < length; at++) {
        char c = text[at];
        if (at == token_at) {
            focus_depth = depth;
            if (focus_in_has && depth != 0
                && depth <= sizeof(nested_start) / sizeof(nested_start[0])) {
                start = nested_start[depth - 1u];
            }
            found = true;
        }
        if (quote != '\0') {
            if (c == quote && (at == 0 || text[at - 1] != '\\')) quote = '\0';
            continue;
        }
        if (c == '\'' || c == '"') {
            quote = c;
            continue;
        }
        if (c == '[') {
            brackets++;
            continue;
        }
        if (c == ']' && brackets != 0) {
            brackets--;
            continue;
        }
        if (brackets != 0) continue;
        if (c == '(') {
            if (depth < sizeof(nested_start) / sizeof(nested_start[0])) {
                nested_start[depth] = at + 1u;
            }
            depth++;
            continue;
        }
        if (c == ')') {
            if (found && focus_in_has && depth == focus_depth) {
                end = at;
                break;
            }
            if (depth != 0) depth--;
            continue;
        }
        bool combinator = c == '>' || c == '+' || c == '~'
            || isspace((unsigned char) c);
        if (!combinator || depth != (found ? focus_depth : depth)) continue;
        if (!found) {
            start = at + 1;
        } else {
            end = at;
            break;
        }
    }
    while (start < token_at && isspace((unsigned char) text[start])) start++;
    if (focus_in_has && start < token_at
        && (text[start] == '>' || text[start] == '+'
            || text[start] == '~')) {
        start++;
        while (start < token_at
               && isspace((unsigned char) text[start])) start++;
    }
    while (end > token_at && isspace((unsigned char) text[end - 1])) end--;
    if (!found || start >= end || token_at < start || token_at >= end) {
        return false;
    }
    *probe_at = start;
    *probe_length = end - start;
    return true;
}

static size_t focus_selector_rightmost_compound(
    const char *text, size_t length)
{
    size_t start = 0;
    int square = 0, round = 0;
    for (size_t i = length; i != 0; i--) {
        char value = text[i - 1];
        if (value == ']') square++;
        else if (value == '[' && square > 0) square--;
        else if (value == ')') round++;
        else if (value == '(' && round > 0) round--;
        else if (square == 0 && round == 0
                 && (value == '>' || value == '+' || value == '~')) {
            start = i;
            break;
        } else if (square == 0 && round == 0
                   && isspace((unsigned char) value)) {
            size_t right = i;
            while (right < length
                   && isspace((unsigned char) text[right])) right++;
            if (right < length) {
                start = right;
                break;
            }
        }
    }
    while (start < length && isspace((unsigned char) text[start])) start++;
    return start;
}

static bool focus_selector_side_effects_are_local(
    const Stylesheet *sheet, lxb_dom_node_t *node)
{
    if (sheet == NULL || node == NULL
        || !stylesheet_prepare_focus_rule_index(
            (Stylesheet *) sheet)) return false;
    for (size_t focus_index = 0;
         focus_index < sheet->focus_rule_count; focus_index++) {
        size_t i = sheet->focus_rule_indices[focus_index];
        if (i >= sheet->count) return false;
        const StyleRule *rule = &sheet->rules[i];
        const char *selector = rule->selector;
        size_t length = rule->selector_length;
        for (size_t at = 0; at < length; at++) {
            size_t token_length = 0;
            bool within = false;
            if (!focus_token_at(
                    selector, length, at, &token_length, &within)) continue;
            if (within) return false;
            bool relational =
                at < style_rule_rightmost_compound(rule)
                || focus_token_inside_has(selector, at);
            if (relational) {
                size_t probe_at = 0, probe_length = 0;
                if (!focus_selector_probe_range(
                        selector, length, at,
                        &probe_at, &probe_length)
                    || style_selector_matches(
                        node, selector + probe_at, probe_length)) {
                    return false;
                }
            } else if (rule->pseudo != PSEUDO_NONE
                       && style_rule_selector_matches(sheet, i, node)) {
                return false;
            }
            at += token_length - 1;
        }
    }
    for (size_t i = 0; i < sheet->custom_rule_count; i++) {
        const StyleCustomRule *rule = &sheet->custom_rules[i];
        const char *selector = rule->selector;
        size_t length = strlen(selector);
        if (strstr(selector, ":focus") == NULL) continue;
        size_t rightmost = focus_selector_rightmost_compound(
            selector, length);
        for (size_t at = 0; at < length; at++) {
            size_t token_length = 0;
            bool within = false;
            if (!focus_token_at(
                    selector, length, at, &token_length, &within)) continue;
            if (within) return false;
            bool relational = at < rightmost
                || focus_token_inside_has(selector, at);
            if (relational) {
                size_t probe_at = 0, probe_length = 0;
                if (!focus_selector_probe_range(
                        selector, length, at,
                        &probe_at, &probe_length)
                    || style_selector_matches(
                        node, selector + probe_at, probe_length)) {
                    return false;
                }
            } else if (style_selector_matches(node, selector, length)) {
                /*
                 * A directly matched custom property can inherit into any
                 * descendant even when this node's concrete ComputedStyle
                 * happens not to consume it. It is never a local paint-only
                 * change. Pseudo-element custom rules are covered as well.
                 */
                return false;
            }
            at += token_length - 1;
        }
    }
    return true;
}

static bool set_focus_marker(lxb_dom_node_t *node, bool focused)
{
    static const lxb_char_t name[] = "data-tilefinch-focus";
    if (node == NULL || node->type != LXB_DOM_NODE_TYPE_ELEMENT) return false;
    lxb_dom_element_t *element = lxb_dom_interface_element(node);
    if (focused) {
        return lxb_dom_element_set_attribute(
            element, name, sizeof(name) - 1,
            (const lxb_char_t *) "", 0) != NULL;
    }
    return lxb_dom_element_remove_attribute(
        element, name, sizeof(name) - 1) == LXB_STATUS_OK;
}

static bool computed_style_equal_without_outline(
    const ComputedStyle *left, const ComputedStyle *right)
{
    if (left == NULL || right == NULL) return false;
    ComputedStyle a = *left, b = *right;
    a.outline_state = b.outline_state = 0;
    a.outline_color = b.outline_color = 0;
    a.outline_alpha = b.outline_alpha = 0;
    return memcmp(&a, &b, sizeof(a)) == 0;
}

static bool focus_style_shadows_are_inset(
    const Stylesheet *sheet, const ComputedStyle *style)
{
    if (style == NULL) return false;
    size_t count = stylesheet_box_shadow_count(sheet, style);
    for (size_t i = 0; i < count; i++) {
        if (!style_box_shadow_is_inset(
                stylesheet_box_shadow(sheet, style, i))) return false;
    }
    return true;
}

static bool focus_paint_equal_without_box_shadow(
    const Stylesheet *sheet, const ComputedStyle *left,
    const ComputedStyle *right)
{
    StylePaintStack a = style_paint_stack_copy(sheet, left);
    StylePaintStack b = style_paint_stack_copy(sheet, right);
    a.components &= (uint8_t) ~STYLE_PAINT_COMPONENT_BOX_SHADOW;
    b.components &= (uint8_t) ~STYLE_PAINT_COMPONENT_BOX_SHADOW;
    a.box_shadow_count = b.box_shadow_count = 0;
    memset(a.box_shadows, 0, sizeof(a.box_shadows));
    memset(b.box_shadows, 0, sizeof(b.box_shadows));
    return memcmp(&a, &b, sizeof(a)) == 0;
}

static bool focus_style_uniform_rounded_border(
    const Stylesheet *sheet, const ComputedStyle *style,
    uint32_t *color, uint8_t *alpha)
{
    if (sheet == NULL || style == NULL
        || style_border_radius_maximum(style->border_radius) <= 0
        || style->border.top <= 0
        || style->border.top != style->border.right
        || style->border.top != style->border.bottom
        || style->border.top != style->border.left) return false;
    uint32_t first_color = 0;
    uint8_t first_alpha = 0;
    for (unsigned side = 0; side < STYLE_BORDER_SIDE_COUNT; side++) {
        if (computed_style_border_line(
                style, (StyleBorderSide) side) != STYLE_BORDER_SOLID) {
            return false;
        }
        uint8_t side_alpha = 0;
        uint32_t side_color = stylesheet_border_color(
            sheet, style, (StyleBorderSide) side, &side_alpha);
        if (side == 0) {
            first_color = side_color;
            first_alpha = side_alpha;
        } else if (side_color != first_color || side_alpha != first_alpha) {
            return false;
        }
    }
    if (color != NULL) *color = first_color;
    if (alpha != NULL) *alpha = first_alpha;
    return true;
}

StyleFocusChange style_focus_change_classify(
    const Stylesheet *sheet, lxb_dom_node_t *node,
    const ComputedStyle *parent, ComputedStyle *normal,
    ComputedStyle *focused)
{
    static const char marker[] = "data-tilefinch-focus";
    if (sheet == NULL || node == NULL || normal == NULL || focused == NULL
        || node->type != LXB_DOM_NODE_TYPE_ELEMENT) {
        return STYLE_FOCUS_CHANGE_UNSAFE;
    }
    size_t original_length = 0;
    const char *original = document_attribute(
        node, marker, &original_length);
    bool originally_focused = original != NULL;
    if (original_length > 63) return STYLE_FOCUS_CHANGE_UNSAFE;
    char original_value[64];
    if (original != NULL && original_length != 0) {
        memcpy(original_value, original, original_length);
    }
    original_value[original_length] = '\0';

    bool focused_set = set_focus_marker(node, true);
    bool ok = focused_set;
    if (focused_set) {
        style_variable_cache_invalidate_node((Stylesheet *) sheet, node);
        *focused = style_for_node(sheet, node, parent);
        /*
         * Probe dependencies while the marker is present. This proves both
         * ends of a move because every journaled old and new focus node is
         * independently restored to focused state here; no unfocused-state
         * selector skip can hide removal of a :has()/sibling effect.
         */
        ok = focus_selector_side_effects_are_local(sheet, node);
    }
    bool normal_set = focused_set && set_focus_marker(node, false);
    ok = ok && normal_set;
    if (normal_set) {
        style_variable_cache_invalidate_node((Stylesheet *) sheet, node);
        *normal = style_for_node(sheet, node, parent);
        ok = ok && focus_selector_side_effects_are_local(sheet, node);
    }
    bool restored = originally_focused
        ? lxb_dom_element_set_attribute(
              lxb_dom_interface_element(node),
              (const lxb_char_t *) marker, sizeof(marker) - 1,
              (const lxb_char_t *) original_value,
              original_length) != NULL
        : (!focused_set || normal_set);
    style_variable_cache_invalidate_node((Stylesheet *) sheet, node);
    if (!ok || !restored) return STYLE_FOCUS_CHANGE_UNSAFE;
    if (computed_style_equal_without_outline(normal, focused)) {
        return STYLE_FOCUS_CHANGE_OUTLINE_ONLY;
    }

    /*
     * The retained display list can recolour a rounded border without
     * changing command geometry. Keep this list intentionally narrower than
     * the set of CSS paint properties: background, foreground, opacity,
     * filters, transforms, outer shadows and every inherited field still
     * force authoritative layout. Inset shadows are normalized only because
     * layout_block.c explicitly retains-but-does-not-paint them; therefore
     * enabled and disabled builds emit the same pixels for that part.
     */
    if (!focus_style_shadows_are_inset(sheet, normal)
        || !focus_style_shadows_are_inset(sheet, focused)
        || !focus_paint_equal_without_box_shadow(sheet, normal, focused)) {
        return STYLE_FOCUS_CHANGE_UNSAFE;
    }
    ComputedStyle a = *normal, b = *focused;
    a.outline_state = b.outline_state = 0;
    a.outline_color = b.outline_color = 0;
    a.outline_alpha = b.outline_alpha = 0;
    a.border_color = b.border_color = 0;
    a.border_alpha = b.border_alpha = 0;
    a.border_color_set = b.border_color_set = 0;
    computed_style_set_paint_stack_id(&a, 0);
    computed_style_set_paint_stack_id(&b, 0);
    if (memcmp(&a, &b, sizeof(a)) != 0) {
        return STYLE_FOCUS_CHANGE_UNSAFE;
    }

    uint32_t normal_color = 0, focused_color = 0;
    uint8_t normal_alpha = 0, focused_alpha = 0;
    if (!focus_style_uniform_rounded_border(
            sheet, normal, &normal_color, &normal_alpha)
        || !focus_style_uniform_rounded_border(
            sheet, focused, &focused_color, &focused_alpha)) {
        return STYLE_FOCUS_CHANGE_UNSAFE;
    }
    return normal_color != focused_color || normal_alpha != focused_alpha
        ? STYLE_FOCUS_CHANGE_BORDER_PAINT_ONLY
        : STYLE_FOCUS_CHANGE_OUTLINE_ONLY;
}

bool style_focus_change_is_outline_only(
    const Stylesheet *sheet, lxb_dom_node_t *node,
    const ComputedStyle *parent, ComputedStyle *normal,
    ComputedStyle *focused)
{
    return style_focus_change_classify(
        sheet, node, parent, normal, focused)
        == STYLE_FOCUS_CHANGE_OUTLINE_ONLY;
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

ComputedStyle style_for_pseudo(const Stylesheet *sheet, lxb_dom_node_t *node,
                               PseudoElement pseudo,
                               const ComputedStyle *parent)
{
    ComputedStyle style = {.display = DISPLAY_INLINE,
                           .color = parent != NULL ? parent->color : 0x000000,
                           .color_alpha = parent != NULL
                                          ? parent->color_alpha : 255,
                           .background_alpha = 255,
                           .transform_scale_q6 = 64,
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
                           .letter_spacing = parent != NULL
                                             ? parent->letter_spacing : 0,
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
    if (sheet == NULL || node == NULL || pseudo == PSEUDO_NONE) return style;
    stylesheet_prepare_rule_index((Stylesheet *) sheet);
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
    StyleMatchSubject subject = style_match_subject(sheet, node);
    StyleRuleIndexPlan index_plan = style_rule_index_plan(sheet, &subject);
    for (size_t phase = 0; phase < 4; phase++) {
        style_apply_matching_range(
            sheet, parent, node, &subject, &index_plan, pseudo,
            sheet->cascade_starts[phase],
            sheet->cascade_ends[phase], &style, false);
    }
    resolve_font_size(&style, parent, false);
    resolve_sizing_em_lengths(&style);
    resolve_overflow_clip_margin(&style);
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
    if (text_decoration_propagation_boundary(NULL, &style)) {
        style_set_ancestor_text_decoration(&style, false, 0);
    }
    *sheet->resolve_scratch = saved_scratch;
    style_relative_selector_cache_end((Stylesheet *) sheet);
    return style;
}

static bool has_ascii_equal(const char *first, const char *second,
                            size_t length)
{
    for (size_t i = 0; i < length; i++) {
        if (tolower((unsigned char) first[i])
            != tolower((unsigned char) second[i])) return false;
    }
    return true;
}

static bool has_selector_contains(const char *selector, size_t length,
                                  const char *needle)
{
    size_t needle_length = strlen(needle);
    if (needle_length == 0 || needle_length > length) return false;
    for (size_t i = 0; i + needle_length <= length; i++) {
        if (memcmp(selector + i, needle, needle_length) == 0) return true;
    }
    return false;
}

static bool has_selector_mentions_attribute(
    const char *selector, size_t length, const char *name,
    size_t name_length)
{
    for (size_t i = 0; i < length; i++) {
        if (selector[i] != '[') continue;
        size_t at = i + 1;
        while (at < length && isspace((unsigned char) selector[at])) at++;
        if (at + name_length > length
            || !has_ascii_equal(selector + at, name, name_length)) continue;
        at += name_length;
        if (at == length || selector[at] == ']'
            || selector[at] == '=' || selector[at] == '~'
            || selector[at] == '|' || selector[at] == '^'
            || selector[at] == '$' || selector[at] == '*'
            || isspace((unsigned char) selector[at])) return true;
    }
    return false;
}

static bool has_css_identifier_byte(unsigned char value)
{
    return value >= 0x80 || isalnum(value) || value == '_' || value == '-';
}

static bool has_selector_mentions_token(
    const char *selector, size_t length, char prefix,
    const char *token, size_t token_length, size_t *match_at)
{
    if (token_length == 0) return false;
    for (size_t i = 0; i + 1 < length; i++) {
        if (selector[i] != prefix) continue;
        size_t at = i + 1, compared = 0;
        bool mismatch = false;
        while (at < length) {
            unsigned char decoded[4];
            size_t decoded_length = 0;
            unsigned char value = (unsigned char) selector[at];
            if (value == '\\') {
                at++;
                if (at == length || selector[at] == '\n'
                    || selector[at] == '\r') {
                    mismatch = true;
                    break;
                }
                unsigned codepoint = 0;
                size_t digits = 0;
                while (at < length && digits < 6
                       && isxdigit((unsigned char) selector[at])) {
                    unsigned char digit = (unsigned char) selector[at++];
                    codepoint = codepoint * 16u
                        + (isdigit(digit)
                           ? (unsigned) (digit - '0')
                           : (unsigned) (tolower(digit) - 'a' + 10));
                    digits++;
                }
                if (digits == 0) {
                    decoded[0] = (unsigned char) selector[at++];
                    decoded_length = 1;
                } else {
                    if (at < length
                        && isspace((unsigned char) selector[at])) at++;
                    if (codepoint <= 0x7f) {
                        decoded[0] = (unsigned char) codepoint;
                        decoded_length = 1;
                    } else if (codepoint <= 0x7ff) {
                        decoded[0] = (unsigned char) (0xc0 | (codepoint >> 6));
                        decoded[1] = (unsigned char) (0x80 | (codepoint & 63));
                        decoded_length = 2;
                    } else if (codepoint <= 0xffff) {
                        decoded[0] = (unsigned char) (0xe0 | (codepoint >> 12));
                        decoded[1] = (unsigned char) (
                            0x80 | ((codepoint >> 6) & 63));
                        decoded[2] = (unsigned char) (
                            0x80 | (codepoint & 63));
                        decoded_length = 3;
                    } else if (codepoint <= 0x10ffff) {
                        decoded[0] = (unsigned char) (0xf0 | (codepoint >> 18));
                        decoded[1] = (unsigned char) (
                            0x80 | ((codepoint >> 12) & 63));
                        decoded[2] = (unsigned char) (
                            0x80 | ((codepoint >> 6) & 63));
                        decoded[3] = (unsigned char) (
                            0x80 | (codepoint & 63));
                        decoded_length = 4;
                    } else {
                        mismatch = true;
                        break;
                    }
                }
            } else {
                if (!has_css_identifier_byte(value)) break;
                decoded[0] = value;
                decoded_length = 1;
                at++;
            }
            if (compared + decoded_length > token_length
                || memcmp(token + compared, decoded, decoded_length) != 0) {
                mismatch = true;
            }
            compared += decoded_length;
        }
        if (!mismatch && compared == token_length) {
            if (match_at != NULL) *match_at = i;
            return true;
        }
    }
    return false;
}

static bool has_selector_token_is_relational(
    const char *selector, size_t length, size_t token_at)
{
    bool has_stack[16] = {0};
    size_t round = 0;
    int square = 0;
    char quote = '\0';
    for (size_t i = 0; i < token_at && i < length; i++) {
        char value = selector[i];
        if (quote != '\0') {
            if (value == '\\' && i + 1 < length) i++;
            else if (value == quote) quote = '\0';
            continue;
        }
        if (value == '\'' || value == '"') quote = value;
        else if (value == '[') square++;
        else if (value == ']' && square > 0) square--;
        else if (square == 0 && value == '(') {
            bool is_has = i >= 4
                && memcmp(selector + i - 4, ":has", 4) == 0;
            if (round == sizeof(has_stack) / sizeof(has_stack[0])) {
                return true;
            }
            has_stack[round++] = is_has;
        } else if (square == 0 && value == ')' && round > 0) {
            round--;
        }
    }
    for (size_t i = 0; i < round; i++) {
        if (has_stack[i]) return true;
    }
    if (square != 0 || quote != '\0') return true;
    for (size_t i = token_at; i < length; i++) {
        char value = selector[i];
        if (quote != '\0') {
            if (value == '\\' && i + 1 < length) i++;
            else if (value == quote) quote = '\0';
            continue;
        }
        if (value == '\'' || value == '"') quote = value;
        else if (value == '[') square++;
        else if (value == ']' && square > 0) square--;
        else if (square == 0 && (value == '+' || value == '~')) return true;
    }
    return false;
}

static bool has_class_list_contains(
    const char *value, size_t length, const char *token, size_t token_length)
{
    if (value == NULL || token_length == 0) return false;
    for (size_t at = 0; at < length;) {
        while (at < length && isspace((unsigned char) value[at])) at++;
        size_t start = at;
        while (at < length && !isspace((unsigned char) value[at])) at++;
        if (at - start == token_length
            && memcmp(value + start, token, token_length) == 0) return true;
    }
    return false;
}

static bool has_selector_mentions_changed_class(
    const char *selector, size_t selector_length,
    const char *old_value, size_t old_length,
    const char *new_value, size_t new_length)
{
    const char *values[2] = {old_value, new_value};
    size_t lengths[2] = {old_length, new_length};
    for (size_t side = 0; side < 2; side++) {
        const char *value = values[side];
        if (value == NULL) continue;
        for (size_t at = 0; at < lengths[side];) {
            while (at < lengths[side]
                   && isspace((unsigned char) value[at])) at++;
            size_t start = at;
            while (at < lengths[side]
                   && !isspace((unsigned char) value[at])) at++;
            size_t token_length = at - start;
            if (token_length == 0
                || has_class_list_contains(
                    values[1 - side], lengths[1 - side],
                    value + start, token_length)) continue;
            size_t search_at = 0;
            while (search_at < selector_length) {
                size_t relative_at = 0;
                if (!has_selector_mentions_token(
                        selector + search_at,
                        selector_length - search_at, '.',
                        value + start, token_length, &relative_at)) break;
                size_t match_at = search_at + relative_at;
                if (has_selector_token_is_relational(
                        selector, selector_length, match_at)) return true;
                search_at = match_at + 1;
            }
        }
    }
    return false;
}

static bool has_values_equal(const char *old_value, size_t old_length,
                             const char *new_value, size_t new_length)
{
    if ((old_value == NULL) != (new_value == NULL)) return false;
    if (old_value == NULL) return true;
    return old_length == new_length
        && memcmp(old_value, new_value, old_length) == 0;
}

static bool has_selector_attribute_change_sensitive(
    const char *selector, size_t selector_length,
    const char *name, size_t name_length,
    const char *old_value, size_t old_length,
    const char *new_value, size_t new_length)
{
    if (!has_selector_contains(selector, selector_length, ":has(")) {
        return false;
    }
    bool is_class = name_length == 5
        && has_ascii_equal(name, "class", 5);
    bool is_id = name_length == 2 && has_ascii_equal(name, "id", 2);
    if (has_selector_mentions_attribute(
            selector, selector_length, name, name_length)) return true;
    if (is_class) {
        return has_selector_mentions_changed_class(
            selector, selector_length, old_value, old_length,
            new_value, new_length);
    }
    if (is_id) {
        return (old_value != NULL && has_selector_mentions_token(
                    selector, selector_length, '#',
                    old_value, old_length, NULL))
            || (new_value != NULL && has_selector_mentions_token(
                    selector, selector_length, '#',
                    new_value, new_length, NULL));
    }
    struct PseudoAttribute {
        const char *attribute;
        const char *pseudo;
    };
    static const struct PseudoAttribute pseudos[] = {
        {"checked", ":checked"}, {"disabled", ":disabled"},
        {"required", ":required"}, {"readonly", ":read-only"},
        {"selected", ":checked"}, {"open", ":open"},
        {"href", ":link"}, {"data-tilefinch-focus", ":focus"}
    };
    for (size_t i = 0; i < sizeof(pseudos) / sizeof(pseudos[0]); i++) {
        size_t attribute_length = strlen(pseudos[i].attribute);
        if (name_length == attribute_length
            && has_ascii_equal(name, pseudos[i].attribute, name_length)
            && has_selector_contains(
                selector, selector_length, pseudos[i].pseudo)) return true;
    }
    return false;
}

bool stylesheet_attribute_change_may_affect_has(
    const Stylesheet *sheet, const char *name, size_t name_length,
    const char *old_value, size_t old_length,
    const char *new_value, size_t new_length)
{
    if (has_values_equal(
            old_value, old_length, new_value, new_length)) return false;
    if (sheet == NULL || name == NULL || name_length == 0) return true;
    for (size_t i = 0; i < sheet->count; i++) {
        const StyleRule *rule = &sheet->rules[i];
        if (rule->selector != NULL
            && has_selector_attribute_change_sensitive(
                rule->selector, rule->selector_length,
                name, name_length, old_value, old_length,
                new_value, new_length)) return true;
    }
    for (size_t i = 0; i < sheet->custom_rule_count; i++) {
        const char *selector = sheet->custom_rules[i].selector;
        if (has_selector_attribute_change_sensitive(
                selector, strlen(selector), name, name_length,
                old_value, old_length, new_value, new_length)) return true;
    }
    return false;
}
