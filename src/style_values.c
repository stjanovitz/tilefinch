/* CSS value micro-parsers: colors, images, backgrounds, transforms,
   box/border shorthands, fonts, grid tracks, and generated text.
   Split out of style.c. */

#include "style_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static bool style_parse_background_size_values(
    const Stylesheet *sheet, const char *text, size_t length,
    uint16_t *width, uint16_t *height, uint8_t *fit, uint8_t *size_flags)
{
    char resolved[96];
    if (width == NULL || height == NULL || fit == NULL || size_flags == NULL
        || !style_resolve_value(sheet, text, length, resolved,
                          sizeof(resolved), 0)) return false;
    const char *value = resolved;
    size_t value_length = strlen(value);
    trim(&value, &value_length);
    if (value_length == 5 && memcmp(value, "cover", 5) == 0) {
        *fit = 1;
        *size_flags = 0;
        return true;
    }
    if (value_length == 7 && memcmp(value, "contain", 7) == 0) {
        *fit = 2;
        *size_flags = 0;
        return true;
    }
    const char *parts[2] = {0};
    size_t part_lengths[2] = {0};
    size_t count = 0;
    for (size_t at = 0; at < value_length && count < 2;) {
        while (at < value_length
               && isspace((unsigned char) value[at])) at++;
        if (at == value_length) break;
        size_t start = at;
        while (at < value_length
               && !isspace((unsigned char) value[at])) at++;
        parts[count] = value + start;
        part_lengths[count++] = at - start;
    }
    size_t trailing = count == 0 ? 0
        : (size_t) ((parts[count - 1] + part_lengths[count - 1]) - value);
    while (trailing < value_length
           && isspace((unsigned char) value[trailing])) trailing++;
    if (count == 0 || trailing != value_length) return false;
    uint8_t flags = STYLE_BACKGROUND_SIZE_EXPLICIT;
    uint16_t dimensions[2] = {0};
    for (size_t i = 0; i < 2; i++) {
        if (i >= count
            || (part_lengths[i] == 4
                && memcmp(parts[i], "auto", 4) == 0)) {
            flags |= i == 0 ? STYLE_BACKGROUND_WIDTH_AUTO
                            : STYLE_BACKGROUND_HEIGHT_AUTO;
            continue;
        }
        bool percent = false;
        int parsed = style_parse_length(sheet, parts[i], part_lengths[i], -1,
                                  &percent);
        if (parsed < 0) return false;
        if (parsed > UINT16_MAX) parsed = UINT16_MAX;
        dimensions[i] = (uint16_t) parsed;
        if (percent) {
            flags |= i == 0 ? STYLE_BACKGROUND_WIDTH_PERCENT
                            : STYLE_BACKGROUND_HEIGHT_PERCENT;
        }
    }
    *fit = 0;
    *width = dimensions[0];
    *height = dimensions[1];
    *size_flags = flags;
    return true;
}

bool style_parse_background_size(const Stylesheet *sheet,
                                  const char *text, size_t length,
                                  ComputedStyle *style)
{
    if (style == NULL) return false;
    return style_parse_background_size_values(
        sheet, text, length, &style->background_width,
        &style->background_height, &style->background_fit,
        &style->background_size_flags);
}

static bool background_position_component(const char *text, size_t length,
                                           bool horizontal, int *percent)
{
    if (percent == NULL) return false;
    if (length == 6 && memcmp(text, "center", 6) == 0) {
        *percent = 50;
        return true;
    }
    if (horizontal && length == 4 && memcmp(text, "left", 4) == 0) {
        *percent = 0;
        return true;
    }
    if (horizontal && length == 5 && memcmp(text, "right", 5) == 0) {
        *percent = 100;
        return true;
    }
    if (!horizontal && length == 3 && memcmp(text, "top", 3) == 0) {
        *percent = 0;
        return true;
    }
    if (!horizontal && length == 6 && memcmp(text, "bottom", 6) == 0) {
        *percent = 100;
        return true;
    }
    char copy[32];
    if (length == 0 || length >= sizeof(copy)) return false;
    memcpy(copy, text, length);
    copy[length] = '\0';
    char *end = NULL;
    long parsed = strtol(copy, &end, 10);
    if (end == copy || strcmp(end, "%") != 0
        || parsed < INT8_MIN || parsed > INT8_MAX) return false;
    *percent = (int) parsed;
    return true;
}

static bool background_position_pixel_component(
    const Stylesheet *sheet, const char *text, size_t length, int *pixels)
{
    trim(&text, &length);
    /* `0%` is dimensionally equal to zero pixels, but its authored unit must
       not switch the other axis from percentage alignment to sprite pixels. */
    if (length != 0 && text[length - 1] == '%') return false;
    bool percent = false;
    if (pixels == NULL) return false;
    int parsed = style_parse_length(sheet, text, length, INT_MIN, &percent);
    if (parsed == INT_MIN || percent) return false;
    *pixels = parsed;
    if (*pixels > INT16_MAX) *pixels = INT16_MAX;
    if (*pixels < INT16_MIN) *pixels = INT16_MIN;
    return true;
}

bool style_parse_background_position(const Stylesheet *sheet,
                                      const char *text, size_t length,
                                      ComputedStyle *style)
{
    char resolved[64];
    if (style == NULL
        || !style_resolve_value(sheet, text, length, resolved,
                          sizeof(resolved), 0)) return false;
    const char *parts[2] = {0};
    size_t part_lengths[2] = {0};
    size_t count = 0, value_length = strlen(resolved);
    size_t at = 0;
    for (; at < value_length && count < 2;) {
        while (at < value_length
               && isspace((unsigned char) resolved[at])) at++;
        if (at == value_length) break;
        size_t start = at;
        while (at < value_length
               && !isspace((unsigned char) resolved[at])) at++;
        parts[count] = resolved + start;
        part_lengths[count++] = at - start;
    }
    while (at < value_length
           && isspace((unsigned char) resolved[at])) at++;
    if (at != value_length) return false;
    if (count == 0) return false;
    /* Sprite sheets crop with device-pixel offsets (0px -1323px); detect
       any px component and record exact pixels instead of percentages. */
    bool pixel_mode = false;
    int pixel_values[2] = {0, 0};
    bool pixel_components[2] = {false, false};
    for (size_t i = 0; i < count; i++) {
        pixel_components[i] = background_position_pixel_component(
            sheet, parts[i], part_lengths[i], &pixel_values[i]);
        if (pixel_components[i]) {
            pixel_mode = true;
        }
    }
    if (pixel_mode) {
        long values[2] = {0, 0};
        for (size_t i = 0; i < count; i++) {
            if (pixel_components[i]) values[i] = pixel_values[i];
            /* Keyword/percent components in a pixel pair resolve to the
               axis origin, which matches the sprite idiom (left/top). */
        }
        style->background_position_x = (int16_t) values[0];
        style->background_position_y = (int16_t) (count > 1 ? values[1] : 0);
        style->background_size_flags |= STYLE_BACKGROUND_POSITION_PIXELS;
        return true;
    }
    int x = 50, y = 50;
    bool first_vertical = (part_lengths[0] == 3
                           && memcmp(parts[0], "top", 3) == 0)
        || (part_lengths[0] == 6
            && memcmp(parts[0], "bottom", 6) == 0);
    if (count == 1) {
        if (first_vertical) {
            if (!background_position_component(
                    parts[0], part_lengths[0], false, &y)) return false;
        } else if (!background_position_component(
                       parts[0], part_lengths[0], true, &x)) return false;
    } else if (first_vertical) {
        if (!background_position_component(
                parts[0], part_lengths[0], false, &y)
            || !background_position_component(
                parts[1], part_lengths[1], true, &x)) return false;
    } else if (!background_position_component(
                   parts[0], part_lengths[0], true, &x)
               || !background_position_component(
                   parts[1], part_lengths[1], false, &y)) return false;
    style->background_position_x = (int16_t) x;
    style->background_position_y = (int16_t) y;
    return true;
}

static size_t transform_arguments(const char *text, size_t length,
                                  const char **parts, size_t *part_lengths,
                                  size_t capacity)
{
    size_t count = 0, start = 0;
    unsigned depth = 0;
    for (size_t i = 0; i <= length; i++) {
        char c = i < length ? text[i] : ',';
        if (c == '(') depth++;
        else if (c == ')' && depth != 0) depth--;
        bool separator = depth == 0
            && (c == ',' || isspace((unsigned char) c));
        if (!separator) continue;
        size_t end = i;
        while (start < end && isspace((unsigned char) text[start])) start++;
        while (end > start && isspace((unsigned char) text[end - 1])) end--;
        if (end > start) {
            if (count >= capacity) return capacity + 1;
            parts[count] = text + start;
            part_lengths[count++] = end - start;
        }
        start = i + 1;
    }
    return count;
}

static bool transform_number(const char *text, size_t length, double *number)
{
    char buffer[40];
    if (length == 0 || length >= sizeof(buffer)) return false;
    memcpy(buffer, text, length);
    buffer[length] = '\0';
    char *end = NULL;
    *number = strtod(buffer, &end);
    while (end != NULL && isspace((unsigned char) *end)) end++;
    return end != buffer && *end == '\0' && isfinite(*number);
}

static bool transform_scale_number(const char *text, size_t length,
                                   double *number)
{
    char buffer[40];
    if (length == 0 || length >= sizeof(buffer)) return false;
    memcpy(buffer, text, length);
    buffer[length] = '\0';
    char *end = NULL;
    *number = strtod(buffer, &end);
    if (end == buffer || !isfinite(*number)) return false;
    if (*end == '%') {
        *number /= 100.0;
        end++;
    }
    while (isspace((unsigned char) *end)) end++;
    return *end == '\0';
}

static bool transform_angle_quadrants(const char *text, size_t length,
                                      unsigned *quadrants)
{
    char buffer[40];
    if (length == 0 || length >= sizeof(buffer)) return false;
    memcpy(buffer, text, length);
    buffer[length] = '\0';
    char *end = NULL;
    double angle = strtod(buffer, &end);
    if (end == buffer || !isfinite(angle)) return false;
    if (strcasecmp(end, "turn") == 0) angle *= 360.0;
    else if (strcasecmp(end, "grad") == 0) angle *= 0.9;
    else if (strcasecmp(end, "rad") == 0) angle *= 57.29577951308232;
    else if (*end != '\0' && strcasecmp(end, "deg") != 0) return false;
    else if (*end == '\0' && angle != 0.0) return false;
    double turns = angle / 90.0;
    double rounded = turns < 0.0 ? ceil(turns - 0.5) : floor(turns + 0.5);
    if (fabs(turns - rounded) > 0.000001) return false;
    int result = (int) rounded % 4;
    if (result < 0) result += 4;
    *quadrants = (unsigned) result;
    return true;
}

/* Compose the practical 2D subset into one uniform-scale/quarter-turn/
   translation tuple.  Parsing the complete function list fixes the former
   first-function-only behaviour while preserving the display list's exact,
   axis-aligned representation.  Non-uniform scale, skew and arbitrary-angle
   rotation fail closed instead of silently painting the wrong transform. */
void style_parse_transform_translation(const Stylesheet *sheet,
                                        const char *text, size_t length,
                                        ComputedStyle *style)
{
    char value[256];
    style->has_transform = false;
    style->transform_scale_q6 = 64;
    style->individual_rotate_quadrants = 0;
    style->transform_x = style->transform_y = 0;
    style->transform_x_percent = style->transform_y_percent = false;
    if (!style_resolve_value(sheet, text, length, value, sizeof(value), 0)
        || strcmp(value, "none") == 0) return;

    double a = 1.0, b = 0.0, c = 0.0, d = 1.0;
    double tx = 0.0, ty = 0.0;
    double pxw = 0.0, pxh = 0.0, pyw = 0.0, pyh = 0.0;
    const char *cursor = value;
    bool any = false;
    while (*cursor != '\0') {
        while (isspace((unsigned char) *cursor)) cursor++;
        if (*cursor == '\0') break;
        const char *name = cursor;
        while (isalnum((unsigned char) *cursor)) cursor++;
        size_t name_length = (size_t) (cursor - name);
        if (name_length == 0 || *cursor != '(') return;
        const char *arguments = ++cursor;
        unsigned depth = 1;
        while (*cursor != '\0' && depth != 0) {
            if (*cursor == '(') depth++;
            else if (*cursor == ')') depth--;
            if (depth != 0) cursor++;
        }
        if (depth != 0) return;
        size_t arguments_length = (size_t) (cursor - arguments);
        cursor++;
        const char *parts[16];
        size_t part_lengths[16];
        size_t count = transform_arguments(arguments, arguments_length,
                                           parts, part_lengths, 16);
        if (count > 16) return;

        bool translate_x = name_length == 10
                           && strncasecmp(name, "translatex", 10) == 0;
        bool translate_y = name_length == 10
                           && strncasecmp(name, "translatey", 10) == 0;
        bool translate = (name_length == 9
                          && strncasecmp(name, "translate", 9) == 0)
                         || (name_length == 11
                             && strncasecmp(name, "translate3d", 11) == 0);
        if (translate || translate_x || translate_y) {
            if (count == 0 || count > (translate ? 3u : 1u)) return;
            int x = 0, y = 0;
            bool xp = false, yp = false;
            if (!translate_y
                && (x = style_parse_length(sheet, parts[0], part_lengths[0],
                                           INT_MIN, &xp)) == INT_MIN) return;
            if (translate_y) {
                if ((y = style_parse_length(sheet, parts[0], part_lengths[0],
                                            INT_MIN, &yp)) == INT_MIN) return;
            } else if (translate && count >= 2
                       && (y = style_parse_length(
                               sheet, parts[1], part_lengths[1], INT_MIN,
                               &yp)) == INT_MIN) return;
            if (translate && count == 3) {
                bool zp = false;
                int z = style_parse_length(sheet, parts[2], part_lengths[2],
                                           INT_MIN, &zp);
                if (z != 0 || zp) return;
            }
            if (xp) { pxw += a * x; pyw += b * x; }
            else { tx += a * x; ty += b * x; }
            if (yp) { pxh += c * y; pyh += d * y; }
            else { tx += c * y; ty += d * y; }
        } else if ((name_length == 5
                    && strncasecmp(name, "scale", 5) == 0)
                   || (name_length == 7
                       && strncasecmp(name, "scale3d", 7) == 0)) {
            if (count == 0 || count > (name_length == 7 ? 3u : 2u)) return;
            double sx = 0.0, sy = 0.0;
            if (!transform_scale_number(parts[0], part_lengths[0], &sx)) return;
            sy = sx;
            if (count >= 2
                && !transform_scale_number(parts[1], part_lengths[1], &sy))
                return;
            if (count == 3) {
                double sz = 0.0;
                if (!transform_number(parts[2], part_lengths[2], &sz)
                    || sz != 1.0) return;
            }
            if (fabs(sx - sy) > 0.000001) return;
            a *= sx; b *= sx; c *= sy; d *= sy;
        } else if ((name_length == 6
                    && strncasecmp(name, "rotate", 6) == 0)
                   || (name_length == 7
                       && strncasecmp(name, "rotatez", 7) == 0)) {
            unsigned q = 0;
            if (count != 1
                || !transform_angle_quadrants(parts[0], part_lengths[0], &q))
                return;
            static const int cosine[4] = {1, 0, -1, 0};
            static const int sine[4] = {0, 1, 0, -1};
            double na = a * cosine[q] + c * sine[q];
            double nb = b * cosine[q] + d * sine[q];
            double nc = -a * sine[q] + c * cosine[q];
            double nd = -b * sine[q] + d * cosine[q];
            a = na; b = nb; c = nc; d = nd;
        } else if ((name_length == 6
                    && strncasecmp(name, "matrix", 6) == 0)
                   || (name_length == 8
                       && strncasecmp(name, "matrix3d", 8) == 0)) {
            double m[6];
            if (name_length == 6) {
                if (count != 6) return;
                for (size_t i = 0; i < 6; i++) {
                    if (!transform_number(parts[i], part_lengths[i], &m[i]))
                        return;
                }
            } else {
                double values[16];
                if (count != 16) return;
                for (size_t i = 0; i < 16; i++) {
                    if (!transform_number(parts[i], part_lengths[i],
                                          &values[i])) return;
                }
                static const unsigned zero_indices[] = {
                    2, 3, 6, 7, 8, 9, 11, 14
                };
                for (size_t i = 0;
                     i < sizeof(zero_indices) / sizeof(zero_indices[0]); i++) {
                    if (fabs(values[zero_indices[i]]) > 0.000001) return;
                }
                if (fabs(values[10] - 1.0) > 0.000001
                    || fabs(values[15] - 1.0) > 0.000001) return;
                m[0] = values[0]; m[1] = values[1];
                m[2] = values[4]; m[3] = values[5];
                m[4] = values[12]; m[5] = values[13];
            }
            double na = a * m[0] + c * m[1];
            double nb = b * m[0] + d * m[1];
            double nc = a * m[2] + c * m[3];
            double nd = b * m[2] + d * m[3];
            tx += a * m[4] + c * m[5];
            ty += b * m[4] + d * m[5];
            a = na; b = nb; c = nc; d = nd;
        } else return;
        any = true;
    }
    if (!any || fabs(pxh) > 0.000001 || fabs(pyw) > 0.000001) return;
    double scale = hypot(a, b);
    unsigned quadrants = 0;
    if (scale > 0.000001) {
        double na = a / scale, nb = b / scale;
        if (fabs(na - 1.0) < 0.000001 && fabs(nb) < 0.000001) quadrants = 0;
        else if (fabs(na) < 0.000001 && fabs(nb - 1.0) < 0.000001) quadrants = 1;
        else if (fabs(na + 1.0) < 0.000001 && fabs(nb) < 0.000001) quadrants = 2;
        else if (fabs(na) < 0.000001 && fabs(nb + 1.0) < 0.000001) quadrants = 3;
        else return;
        double expected_c = quadrants == 1 ? -scale
                            : (quadrants == 3 ? scale : 0.0);
        double expected_d = quadrants == 2 ? -scale
                            : (quadrants == 0 ? scale : 0.0);
        if (fabs(c - expected_c) > 0.000001
            || fabs(d - expected_d) > 0.000001) return;
    } else if (fabs(c) > 0.000001 || fabs(d) > 0.000001) return;
    if ((fabs(tx) > 0.000001 && fabs(pxw) > 0.000001)
        || (fabs(ty) > 0.000001 && fabs(pyh) > 0.000001)) return;
    if (scale > 255.0 / 64.0) scale = 255.0 / 64.0;
    style->transform_scale_q6 = (uint8_t) (scale * 64.0 + 0.5);
    style->individual_rotate_quadrants = (uint8_t) quadrants;
    style->transform_x_percent = fabs(pxw) > 0.000001;
    style->transform_y_percent = fabs(pyh) > 0.000001;
    double final_x = style->transform_x_percent ? pxw : tx;
    double final_y = style->transform_y_percent ? pyh : ty;
    if (final_x < -32767.0) final_x = -32767.0;
    if (final_x > 32767.0) final_x = 32767.0;
    if (final_y < -32767.0) final_y = -32767.0;
    if (final_y > 32767.0) final_y = 32767.0;
    style->transform_x = (int) (final_x < 0 ? final_x - 0.5 : final_x + 0.5);
    style->transform_y = (int) (final_y < 0 ? final_y - 0.5 : final_y + 0.5);
    style->has_transform = style->transform_scale_q6 != 64
        || quadrants != 0 || style->transform_x != 0 || style->transform_y != 0;
}

static void skip_color_separator(const char **cursor)
{
    while (isspace((unsigned char) **cursor) || **cursor == ',') (*cursor)++;
}

static bool parse_color_number(const char **cursor, double *number,
                               bool *percent)
{
    skip_color_separator(cursor);
    char *end = NULL;
    *number = strtod(*cursor, &end);
    if (end == *cursor) return false;
    *cursor = end;
    *percent = **cursor == '%';
    if (*percent) (*cursor)++;
    return true;
}

static double clamp_unit(double value)
{
    return value < 0.0 ? 0.0 : (value > 1.0 ? 1.0 : value);
}

static unsigned color_byte(double value)
{
    value = clamp_unit(value);
    return (unsigned) (value * 255.0 + 0.5);
}

static double hue_degrees(const char **cursor, double hue)
{
    if (strncmp(*cursor, "turn", 4) == 0) {
        *cursor += 4;
        hue *= 360.0;
    } else if (strncmp(*cursor, "grad", 4) == 0) {
        *cursor += 4;
        hue *= 0.9;
    } else if (strncmp(*cursor, "rad", 3) == 0) {
        *cursor += 3;
        hue *= 57.29577951308232;
    } else if (strncmp(*cursor, "deg", 3) == 0) {
        *cursor += 3;
    }
    while (hue < 0.0) hue += 360.0;
    while (hue >= 360.0) hue -= 360.0;
    return hue;
}

static double hsl_channel(double p, double q, double hue)
{
    while (hue < 0.0) hue += 1.0;
    while (hue > 1.0) hue -= 1.0;
    if (hue < 1.0 / 6.0) return p + (q - p) * 6.0 * hue;
    if (hue < 1.0 / 2.0) return q;
    if (hue < 2.0 / 3.0) return p + (q - p) * (2.0 / 3.0 - hue) * 6.0;
    return p;
}

static double linear_to_srgb(double value)
{
    value = clamp_unit(value);
    return value <= 0.0031308 ? value * 12.92
           : 1.055 * pow(value, 1.0 / 2.4) - 0.055;
}

static double srgb_to_linear(double value)
{
    value = clamp_unit(value);
    return value <= 0.04045 ? value / 12.92
           : pow((value + 0.055) / 1.055, 2.4);
}

static double lab_inverse(double value)
{
    double cube = value * value * value;
    return cube > 216.0 / 24389.0 ? cube
           : (116.0 * value - 16.0) / (24389.0 / 27.0);
}

static double lab_forward(double value)
{
    return value > 216.0 / 24389.0 ? cbrt(value)
           : (24389.0 / 27.0 * value + 16.0) / 116.0;
}

static void color_to_space(uint32_t color, unsigned space, double output[3])
{
    double red = ((color >> 16) & 0xffu) / 255.0;
    double green = ((color >> 8) & 0xffu) / 255.0;
    double blue = (color & 0xffu) / 255.0;
    if (space == 0) {
        output[0] = red; output[1] = green; output[2] = blue;
        return;
    }
    red = srgb_to_linear(red);
    green = srgb_to_linear(green);
    blue = srgb_to_linear(blue);
    if (space == 1) {
        output[0] = red; output[1] = green; output[2] = blue;
    } else if (space == 2) {
        double l = cbrt(0.4122214708 * red + 0.5363325363 * green
                        + 0.0514459929 * blue);
        double m = cbrt(0.2119034982 * red + 0.6806995451 * green
                        + 0.1073969566 * blue);
        double s = cbrt(0.0883024619 * red + 0.2817188376 * green
                        + 0.6299787005 * blue);
        output[0] = 0.2104542553 * l + 0.7936177850 * m
                    - 0.0040720468 * s;
        output[1] = 1.9779984951 * l - 2.4285922050 * m
                    + 0.4505937099 * s;
        output[2] = 0.0259040371 * l + 0.7827717662 * m
                    - 0.8086757660 * s;
    } else {
        double x65 = 0.4124564 * red + 0.3575761 * green + 0.1804375 * blue;
        double y65 = 0.2126729 * red + 0.7151522 * green + 0.0721750 * blue;
        double z65 = 0.0193339 * red + 0.1191920 * green + 0.9503041 * blue;
        double x50 = 1.0478112 * x65 + 0.0228866 * y65 - 0.0501270 * z65;
        double y50 = 0.0295424 * x65 + 0.9904844 * y65 - 0.0170491 * z65;
        double z50 = -0.0092345 * x65 + 0.0150436 * y65 + 0.7521316 * z65;
        double fx = lab_forward(x50 / 0.96422);
        double fy = lab_forward(y50);
        double fz = lab_forward(z50 / 0.82521);
        output[0] = 116.0 * fy - 16.0;
        output[1] = 500.0 * (fx - fy);
        output[2] = 200.0 * (fy - fz);
    }
}

static uint32_t color_from_space(const double input[3], unsigned space)
{
    double red = input[0], green = input[1], blue = input[2];
    if (space == 2) {
        double l_root = input[0] + 0.3963377774 * input[1]
                        + 0.2158037573 * input[2];
        double m_root = input[0] - 0.1055613458 * input[1]
                        - 0.0638541728 * input[2];
        double s_root = input[0] - 0.0894841775 * input[1]
                        - 1.2914855480 * input[2];
        double l = l_root * l_root * l_root;
        double m = m_root * m_root * m_root;
        double s = s_root * s_root * s_root;
        red = 4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s;
        green = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s;
        blue = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s;
    } else if (space == 3) {
        double fy = (input[0] + 16.0) / 116.0;
        double x50 = 0.96422 * lab_inverse(fy + input[1] / 500.0);
        double y50 = lab_inverse(fy);
        double z50 = 0.82521 * lab_inverse(fy - input[2] / 200.0);
        double x = 0.9555766 * x50 - 0.0230393 * y50 + 0.0631636 * z50;
        double y = -0.0282895 * x50 + 1.0099416 * y50 + 0.0210077 * z50;
        double z = 0.0122982 * x50 - 0.0204830 * y50 + 1.3299098 * z50;
        red = 3.2404542 * x - 1.5371385 * y - 0.4985314 * z;
        green = -0.9692660 * x + 1.8760108 * y + 0.0415560 * z;
        blue = 0.0556434 * x - 0.2040259 * y + 1.0572252 * z;
    }
    if (space != 0) {
        red = linear_to_srgb(red);
        green = linear_to_srgb(green);
        blue = linear_to_srgb(blue);
    }
    return (color_byte(red) << 16) | (color_byte(green) << 8)
           | color_byte(blue);
}

static bool parse_function_color(const char *value, uint32_t *color,
                                 uint8_t *alpha)
{
    bool rgb = strncmp(value, "rgb(", 4) == 0
               || strncmp(value, "rgba(", 5) == 0;
    bool hsl = strncmp(value, "hsl(", 4) == 0
               || strncmp(value, "hsla(", 5) == 0;
    bool oklch = strncmp(value, "oklch(", 6) == 0;
    bool oklab = strncmp(value, "oklab(", 6) == 0;
    bool lab = strncmp(value, "lab(", 4) == 0;
    if (!(rgb || hsl || oklch || oklab || lab)) return false;
    const char *cursor = strchr(value, '(') + 1;
    double first = 0.0, second = 0.0, third = 0.0;
    bool first_percent = false, second_percent = false, third_percent = false;
    if (!parse_color_number(&cursor, &first, &first_percent)) return false;
    if (hsl) first = hue_degrees(&cursor, first);
    if (!parse_color_number(&cursor, &second, &second_percent)
        || !parse_color_number(&cursor, &third, &third_percent)) return false;
    if (oklch) third = hue_degrees(&cursor, third);
    skip_color_separator(&cursor);
    double alpha_value = 1.0;
    bool alpha_percent = false;
    if (*cursor == '/') {
        cursor++;
        if (!parse_color_number(&cursor, &alpha_value, &alpha_percent)) {
            return false;
        }
    } else if (*cursor != ')' && *cursor != '\0') {
        if (!parse_color_number(&cursor, &alpha_value, &alpha_percent)) {
            return false;
        }
    }
    skip_color_separator(&cursor);
    if (*cursor != ')') return false;
    if (alpha_percent) alpha_value /= 100.0;
    *alpha = (uint8_t) color_byte(alpha_value);
    double red = 0.0, green = 0.0, blue = 0.0;
    if (rgb) {
        red = first_percent ? first / 100.0 : first / 255.0;
        green = second_percent ? second / 100.0 : second / 255.0;
        blue = third_percent ? third / 100.0 : third / 255.0;
    } else if (hsl) {
        if (!second_percent || !third_percent) return false;
        double hue = first / 360.0;
        double saturation = clamp_unit(second / 100.0);
        double lightness = clamp_unit(third / 100.0);
        if (saturation == 0.0) {
            red = green = blue = lightness;
        } else {
            double q = lightness < 0.5
                       ? lightness * (1.0 + saturation)
                       : lightness + saturation - lightness * saturation;
            double p = 2.0 * lightness - q;
            red = hsl_channel(p, q, hue + 1.0 / 3.0);
            green = hsl_channel(p, q, hue);
            blue = hsl_channel(p, q, hue - 1.0 / 3.0);
        }
    } else if (oklch || oklab) {
        double lightness = first_percent ? first / 100.0 : first;
        double a = second_percent ? second * 0.004 : second;
        double b = third_percent ? third * 0.004 : third;
        if (oklch) {
            double chroma = second_percent ? second / 100.0 : second;
            double radians = third * 0.017453292519943295;
            a = chroma * cos(radians);
            b = chroma * sin(radians);
        }
        double l_root = lightness + 0.3963377774 * a + 0.2158037573 * b;
        double m_root = lightness - 0.1055613458 * a - 0.0638541728 * b;
        double s_root = lightness - 0.0894841775 * a - 1.2914855480 * b;
        double l = l_root * l_root * l_root;
        double m = m_root * m_root * m_root;
        double s = s_root * s_root * s_root;
        red = linear_to_srgb(4.0767416621 * l - 3.3077115913 * m
                             + 0.2309699292 * s);
        green = linear_to_srgb(-1.2684380046 * l + 2.6097574011 * m
                               - 0.3413193965 * s);
        blue = linear_to_srgb(-0.0041960863 * l - 0.7034186147 * m
                              + 1.7076147010 * s);
    } else {
        double lightness = first_percent ? first : first;
        double a = second_percent ? second * 1.25 : second;
        double b = third_percent ? third * 1.25 : third;
        double fy = (lightness + 16.0) / 116.0;
        double x50 = 0.96422 * lab_inverse(fy + a / 500.0);
        double y50 = lab_inverse(fy);
        double z50 = 0.82521 * lab_inverse(fy - b / 200.0);
        double x = 0.9555766 * x50 - 0.0230393 * y50 + 0.0631636 * z50;
        double y = -0.0282895 * x50 + 1.0099416 * y50 + 0.0210077 * z50;
        double z = 0.0122982 * x50 - 0.0204830 * y50 + 1.3299098 * z50;
        red = linear_to_srgb(3.2404542 * x - 1.5371385 * y - 0.4985314 * z);
        green = linear_to_srgb(-0.9692660 * x + 1.8760108 * y + 0.0415560 * z);
        blue = linear_to_srgb(0.0556434 * x - 0.2040259 * y + 1.0572252 * z);
    }
    *color = (color_byte(red) << 16) | (color_byte(green) << 8)
             | color_byte(blue);
    return true;
}

static bool style_parse_color_with_alpha_depth(const Stylesheet *sheet,
                                                const char *text,
                                                size_t length,
                                                uint32_t *color,
                                                uint8_t *alpha,
                                                unsigned depth);

static bool style_parse_color_mix(const Stylesheet *sheet, const char *value,
                                  uint32_t *color, uint8_t *alpha,
                                  unsigned depth)
{
    size_t length = strlen(value);
    static const char prefix[] = "color-mix(";
    if (depth >= 3 || length <= sizeof(prefix)
        || strncasecmp(value, prefix, sizeof(prefix) - 1) != 0
        || value[length - 1] != ')') return false;
    const char *inner = value + sizeof(prefix) - 1;
    size_t inner_length = length - sizeof(prefix);
    size_t cursor = 0, start = 0, end = 0, segment_index = 0;
    uint32_t colors[2] = {0, 0};
    uint8_t alphas[2] = {255, 255};
    double weights[2] = {-1.0, -1.0};
    unsigned interpolation_space = 0;
    while (style_next_top_level(inner, inner_length, ',', &cursor,
                                &start, &end)) {
        const char *segment = inner + start;
        size_t segment_length = end - start;
        trim(&segment, &segment_length);
        if (segment_index == 0) {
            if (span_case_equal(segment, segment_length, "in srgb"))
                interpolation_space = 0;
            else if (span_case_equal(segment, segment_length,
                                     "in srgb-linear"))
                interpolation_space = 1;
            else if (span_case_equal(segment, segment_length, "in oklab"))
                interpolation_space = 2;
            else if (span_case_equal(segment, segment_length, "in lab"))
                interpolation_space = 3;
            else return false;
            segment_index++;
            continue;
        }
        size_t color_index = segment_index - 1;
        if (color_index >= 2) return false;
        size_t split = segment_length;
        while (split != 0 && !isspace((unsigned char) segment[split - 1]))
            split--;
        if (split != 0) {
            char percentage[24];
            size_t percentage_length = segment_length - split;
            if (percentage_length < sizeof(percentage)) {
                memcpy(percentage, segment + split, percentage_length);
                percentage[percentage_length] = '\0';
                char *parsed_end = NULL;
                double parsed = strtod(percentage, &parsed_end);
                if (parsed_end != percentage && *parsed_end == '%'
                    && parsed >= 0.0) {
                    parsed_end++;
                    while (isspace((unsigned char) *parsed_end)) parsed_end++;
                    if (*parsed_end == '\0') {
                        weights[color_index] = parsed;
                        segment_length = split;
                        while (segment_length != 0
                               && isspace((unsigned char)
                                          segment[segment_length - 1])) {
                            segment_length--;
                        }
                    }
                }
            }
        }
        if (!style_parse_color_with_alpha_depth(
                sheet, segment, segment_length, &colors[color_index],
                &alphas[color_index], depth + 1)) return false;
        segment_index++;
    }
    if (segment_index != 3) return false;
    if (weights[0] < 0.0 && weights[1] < 0.0) {
        weights[0] = weights[1] = 50.0;
    } else if (weights[0] < 0.0) {
        weights[0] = 100.0 - weights[1];
    } else if (weights[1] < 0.0) {
        weights[1] = 100.0 - weights[0];
    }
    if (weights[0] < 0.0) weights[0] = 0.0;
    if (weights[1] < 0.0) weights[1] = 0.0;
    double total = weights[0] + weights[1];
    if (total <= 0.0) return false;
    double mix = weights[1] / total;
    double first_alpha = alphas[0] / 255.0;
    double second_alpha = alphas[1] / 255.0;
    double mixed_alpha = first_alpha * (1.0 - mix) + second_alpha * mix;
    unsigned output_alpha = (unsigned) (mixed_alpha * 255.0 + 0.5);
    if (total < 100.0) output_alpha = (unsigned) (output_alpha * total / 100.0);
    if (output_alpha > 255) output_alpha = 255;
    double first_channels[3], second_channels[3], mixed_channels[3];
    color_to_space(colors[0], interpolation_space, first_channels);
    color_to_space(colors[1], interpolation_space, second_channels);
    for (unsigned channel = 0; channel < 3; channel++) {
        mixed_channels[channel] = mixed_alpha <= 0.0 ? 0.0
            : (first_channels[channel] * first_alpha * (1.0 - mix)
               + second_channels[channel] * second_alpha * mix) / mixed_alpha;
    }
    *color = color_from_space(mixed_channels, interpolation_space);
    *alpha = (uint8_t) output_alpha;
    return true;
}

static bool style_parse_light_dark(const Stylesheet *sheet, const char *value,
                                   uint32_t *color, uint8_t *alpha,
                                   unsigned depth)
{
    size_t length = strlen(value);
    static const char prefix[] = "light-dark(";
    if (depth >= 3 || length <= sizeof(prefix)
        || strncasecmp(value, prefix, sizeof(prefix) - 1) != 0
        || value[length - 1] != ')') return false;
    const char *inner = value + sizeof(prefix) - 1;
    size_t inner_length = length - sizeof(prefix);
    size_t cursor = 0, start = 0, end = 0, count = 0;
    uint32_t parsed_color = 0;
    uint8_t parsed_alpha = 255;
    while (style_next_top_level(inner, inner_length, ',', &cursor,
                                &start, &end)) {
        if (count >= 2
            || !style_parse_color_with_alpha_depth(
                sheet, inner + start, end - start,
                &parsed_color, &parsed_alpha, depth + 1)) return false;
        if (count == 0) {
            *color = parsed_color;
            *alpha = parsed_alpha;
        }
        count++;
    }
    /* Tilefinch's computed page scheme is light. Forced night mode is a
       final RGB565 transform, so selecting the light arm here avoids
       double-darkening while still retaining valid fallback declarations. */
    return count == 2;
}

static bool style_parse_color_with_alpha_depth(const Stylesheet *sheet,
                                                const char *text,
                                                size_t length,
                                                uint32_t *color,
                                                uint8_t *alpha,
                                                unsigned depth)
{
    char value[256];
    if (!style_resolve_value(sheet, text, length, value, sizeof(value), 0)) return false;
    text = value;
    length = strlen(value);
    trim(&text, &length);
    *alpha = 255;
    if ((length == 4 || length == 5 || length == 7 || length == 9)
        && text[0] == '#') {
        uint32_t result = 0;
        size_t digits = length <= 5 ? 3 : 6;
        for (size_t i = 0; i < digits; i++) {
            unsigned char c = (unsigned char) text[i + 1];
            unsigned digit;
            if (c >= '0' && c <= '9') digit = c - '0';
            else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
            else return false;
            result = length <= 5 ? (result << 8) | (digit << 4) | digit
                                 : (result << 4) | digit;
        }
        if (length == 5) {
            unsigned high = (unsigned) text[4];
            high = high >= '0' && high <= '9' ? high - '0'
                   : (high >= 'a' && high <= 'f' ? high - 'a' + 10
                      : (high >= 'A' && high <= 'F' ? high - 'A' + 10 : 16));
            if (high > 15) return false;
            *alpha = (uint8_t) ((high << 4) | high);
        } else if (length == 9) {
            unsigned parsed = 0;
            for (size_t i = 7; i < 9; i++) {
                unsigned char c = (unsigned char) text[i];
                unsigned digit = c >= '0' && c <= '9' ? c - '0'
                                 : (c >= 'a' && c <= 'f' ? c - 'a' + 10
                                    : (c >= 'A' && c <= 'F'
                                       ? c - 'A' + 10 : 16));
                if (digit > 15) return false;
                parsed = (parsed << 4) | digit;
            }
            *alpha = (uint8_t) parsed;
        }
        *color = result;
        return true;
    }
    if (parse_function_color(value, color, alpha)) return true;
    if (style_parse_color_mix(sheet, value, color, alpha, depth)) return true;
    if (style_parse_light_dark(sheet, value, color, alpha, depth)) return true;
    struct NamedColor { const char *name; uint32_t value; };
    static const struct NamedColor colors[] = {
        {"aliceblue", 0xf0f8ff}, {"antiquewhite", 0xfaebd7},
        {"aqua", 0x00ffff}, {"aquamarine", 0x7fffd4},
        {"azure", 0xf0ffff}, {"beige", 0xf5f5dc},
        {"bisque", 0xffe4c4}, {"black", 0x000000},
        {"blanchedalmond", 0xffebcd}, {"blue", 0x0000ff},
        {"blueviolet", 0x8a2be2}, {"brown", 0xa52a2a},
        {"burlywood", 0xdeb887}, {"cadetblue", 0x5f9ea0},
        {"chartreuse", 0x7fff00}, {"chocolate", 0xd2691e},
        {"coral", 0xff7f50}, {"cornflowerblue", 0x6495ed},
        {"cornsilk", 0xfff8dc}, {"crimson", 0xdc143c},
        {"cyan", 0x00ffff}, {"darkblue", 0x00008b},
        {"darkcyan", 0x008b8b}, {"darkgoldenrod", 0xb8860b},
        {"darkgray", 0xa9a9a9}, {"darkgreen", 0x006400},
        {"darkgrey", 0xa9a9a9}, {"darkkhaki", 0xbdb76b},
        {"darkmagenta", 0x8b008b}, {"darkolivegreen", 0x556b2f},
        {"darkorange", 0xff8c00}, {"darkorchid", 0x9932cc},
        {"darkred", 0x8b0000}, {"darksalmon", 0xe9967a},
        {"darkseagreen", 0x8fbc8f}, {"darkslateblue", 0x483d8b},
        {"darkslategray", 0x2f4f4f}, {"darkslategrey", 0x2f4f4f},
        {"darkturquoise", 0x00ced1}, {"darkviolet", 0x9400d3},
        {"deeppink", 0xff1493}, {"deepskyblue", 0x00bfff},
        {"dimgray", 0x696969}, {"dimgrey", 0x696969},
        {"dodgerblue", 0x1e90ff}, {"firebrick", 0xb22222},
        {"floralwhite", 0xfffaf0}, {"forestgreen", 0x228b22},
        {"fuchsia", 0xff00ff}, {"gainsboro", 0xdcdcdc},
        {"ghostwhite", 0xf8f8ff}, {"gold", 0xffd700},
        {"goldenrod", 0xdaa520}, {"gray", 0x808080},
        {"green", 0x008000}, {"greenyellow", 0xadff2f},
        {"grey", 0x808080}, {"honeydew", 0xf0fff0},
        {"hotpink", 0xff69b4}, {"indianred", 0xcd5c5c},
        {"indigo", 0x4b0082}, {"ivory", 0xfffff0},
        {"khaki", 0xf0e68c}, {"lavender", 0xe6e6fa},
        {"lavenderblush", 0xfff0f5}, {"lawngreen", 0x7cfc00},
        {"lemonchiffon", 0xfffacd}, {"lightblue", 0xadd8e6},
        {"lightcoral", 0xf08080}, {"lightcyan", 0xe0ffff},
        {"lightgoldenrodyellow", 0xfafad2}, {"lightgray", 0xd3d3d3},
        {"lightgreen", 0x90ee90}, {"lightgrey", 0xd3d3d3},
        {"lightpink", 0xffb6c1}, {"lightsalmon", 0xffa07a},
        {"lightseagreen", 0x20b2aa}, {"lightskyblue", 0x87cefa},
        {"lightslategray", 0x778899}, {"lightslategrey", 0x778899},
        {"lightsteelblue", 0xb0c4de}, {"lightyellow", 0xffffe0},
        {"lime", 0x00ff00}, {"limegreen", 0x32cd32},
        {"linen", 0xfaf0e6}, {"magenta", 0xff00ff},
        {"maroon", 0x800000}, {"mediumaquamarine", 0x66cdaa},
        {"mediumblue", 0x0000cd}, {"mediumorchid", 0xba55d3},
        {"mediumpurple", 0x9370db}, {"mediumseagreen", 0x3cb371},
        {"mediumslateblue", 0x7b68ee}, {"mediumspringgreen", 0x00fa9a},
        {"mediumturquoise", 0x48d1cc}, {"mediumvioletred", 0xc71585},
        {"midnightblue", 0x191970}, {"mintcream", 0xf5fffa},
        {"mistyrose", 0xffe4e1}, {"moccasin", 0xffe4b5},
        {"navajowhite", 0xffdead}, {"navy", 0x000080},
        {"oldlace", 0xfdf5e6}, {"olive", 0x808000},
        {"olivedrab", 0x6b8e23}, {"orange", 0xffa500},
        {"orangered", 0xff4500}, {"orchid", 0xda70d6},
        {"palegoldenrod", 0xeee8aa}, {"palegreen", 0x98fb98},
        {"paleturquoise", 0xafeeee}, {"palevioletred", 0xdb7093},
        {"papayawhip", 0xffefd5}, {"peachpuff", 0xffdab9},
        {"peru", 0xcd853f}, {"pink", 0xffc0cb},
        {"plum", 0xdda0dd}, {"powderblue", 0xb0e0e6},
        {"purple", 0x800080}, {"rebeccapurple", 0x663399},
        {"red", 0xff0000}, {"rosybrown", 0xbc8f8f},
        {"royalblue", 0x4169e1}, {"saddlebrown", 0x8b4513},
        {"salmon", 0xfa8072}, {"sandybrown", 0xf4a460},
        {"seagreen", 0x2e8b57}, {"seashell", 0xfff5ee},
        {"sienna", 0xa0522d}, {"silver", 0xc0c0c0},
        {"skyblue", 0x87ceeb}, {"slateblue", 0x6a5acd},
        {"slategray", 0x708090}, {"slategrey", 0x708090},
        {"snow", 0xfffafa}, {"springgreen", 0x00ff7f},
        {"steelblue", 0x4682b4}, {"tan", 0xd2b48c},
        {"teal", 0x008080}, {"thistle", 0xd8bfd8},
        {"tomato", 0xff6347}, {"transparent", 0xffffff},
        {"turquoise", 0x40e0d0}, {"violet", 0xee82ee},
        {"wheat", 0xf5deb3}, {"white", 0xffffff},
        {"whitesmoke", 0xf5f5f5}, {"yellow", 0xffff00},
        {"yellowgreen", 0x9acd32}
    };
    size_t low = 0;
    size_t high = sizeof(colors) / sizeof(colors[0]);
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        const char *name = colors[middle].name;
        size_t name_length = strlen(name);
        size_t compared = length < name_length ? length : name_length;
        int order = 0;
        for (size_t i = 0; i < compared && order == 0; i++) {
            order = tolower((unsigned char) text[i])
                    - (unsigned char) name[i];
        }
        if (order == 0) {
            order = length < name_length ? -1 : (length > name_length ? 1 : 0);
        }
        if (order == 0) {
            *color = colors[middle].value;
            if (span_case_equal(text, length, "transparent")) *alpha = 0;
            return true;
        }
        if (order < 0) high = middle;
        else low = middle + 1;
    }
    return false;
}

bool style_parse_color_with_alpha(const Stylesheet *sheet, const char *text,
                                   size_t length, uint32_t *color,
                                   uint8_t *alpha)
{
    return style_parse_color_with_alpha_depth(
        sheet, text, length, color, alpha, 0);
}

bool style_parse_color(const Stylesheet *sheet, const char *text,
                        size_t length, uint32_t *color)
{
    uint8_t alpha = 255;
    return style_parse_color_with_alpha(sheet, text, length, color, &alpha);
}

bool style_color_parse(const char *text, size_t length,
                       uint32_t *color, uint8_t *alpha)
{
    if (text == NULL || color == NULL || alpha == NULL) return false;
    /* Standalone consumers have no cascade in which to resolve custom
       properties. Reject var() before the internal resolver can consult a
       stylesheet. */
    for (size_t at = 0; at + 4 <= length; at++) {
        if (text[at] == 'v' && text[at + 1] == 'a'
            && text[at + 2] == 'r' && text[at + 3] == '(') return false;
    }
    return style_parse_color_with_alpha(NULL, text, length, color, alpha);
}

static const char *style_image_referrer_policy_name(uint8_t policy)
{
    static const char *const names[] = {
        "", "no-referrer", "no-referrer-when-downgrade", "origin",
        "origin-when-cross-origin", "same-origin", "strict-origin",
        "strict-origin-when-cross-origin", "unsafe-url"
    };
    return policy < sizeof(names) / sizeof(names[0]) ? names[policy] : NULL;
}

static bool style_image_referrer_policy_parse(const char *policy,
                                              uint8_t *parsed)
{
    if (policy == NULL || parsed == NULL) return false;
    size_t length = strlen(policy);
    for (uint8_t candidate = STYLE_IMAGE_REFERRER_DEFAULT;
         candidate <= STYLE_IMAGE_REFERRER_UNSAFE_URL; candidate++) {
        const char *name = style_image_referrer_policy_name(candidate);
        size_t name_length = name == NULL ? 0 : strlen(name);
        bool equal = name != NULL && name_length == length;
        for (size_t i = 0; equal && i < length; i++) {
            equal = tolower((unsigned char) policy[i])
                    == tolower((unsigned char) name[i]);
        }
        if (equal) {
            *parsed = candidate;
            return true;
        }
    }
    return false;
}

bool stylesheet_current_image_source_slot(Stylesheet *sheet,
                                                 uint8_t *slot)
{
    if (sheet == NULL || sheet->budget == NULL || slot == NULL) return false;
    if (sheet->resolve_scratch->current_image_source_base == NULL) {
        sheet->resolve_scratch->current_image_source_slot = 0;
        *slot = 0;
        return true;
    }
    if (sheet->resolve_scratch->current_image_source_slot != 0) {
        if (sheet->image_sources == NULL
            || sheet->resolve_scratch->current_image_source_slot
                   > sheet->image_sources->count) return false;
        *slot = sheet->resolve_scratch->current_image_source_slot;
        return true;
    }
    const char *base = sheet->resolve_scratch->current_image_source_base;
    size_t base_length = strlen(base);
    uint8_t policy = 0;
    if (base_length == 0 || base_length >= 4096
        || !style_image_referrer_policy_parse(
               sheet->resolve_scratch->current_image_source_referrer_policy, &policy)) {
        return false;
    }
    StyleImageSources *sources = sheet->image_sources;
    if (sources != NULL) {
        for (size_t i = 0; i < sources->count; i++) {
            if (sources->items[i].referrer_policy == policy
                && strcmp(sources->items[i].base_url, base) == 0) {
                sheet->resolve_scratch->current_image_source_slot = (uint8_t) (i + 1u);
                *slot = sheet->resolve_scratch->current_image_source_slot;
                return true;
            }
        }
    }
    if (sources == NULL) {
        sources = budget_calloc(sheet->budget, 1, sizeof(*sources));
        if (sources == NULL) return false;
        sheet->image_sources = sources;
    }
    if (sources->count >= STYLE_IMAGE_SOURCE_LIMIT) return false;
    if (sources->count == sources->capacity) {
        size_t capacity = sources->capacity == 0 ? 4u
                                                 : sources->capacity * 2u;
        if (capacity > STYLE_IMAGE_SOURCE_LIMIT) {
            capacity = STYLE_IMAGE_SOURCE_LIMIT;
        }
        StyleImageSourceContext *items = budget_realloc(
            sheet->budget, sources->items, capacity * sizeof(*items));
        if (items == NULL) return false;
        sources->items = items;
        sources->capacity = capacity;
    }
    char *base_copy = budget_malloc(sheet->budget, base_length + 1u);
    if (base_copy == NULL) return false;
    memcpy(base_copy, base, base_length + 1u);
    size_t index = sources->count++;
    sources->items[index] = (StyleImageSourceContext) {
        .base_url = base_copy,
        .referrer_policy = policy
    };
    sources->retained_base_bytes += base_length + 1u;
    sheet->resolve_scratch->current_image_source_slot = (uint8_t) (index + 1u);
    *slot = sheet->resolve_scratch->current_image_source_slot;
    return true;
}

bool stylesheet_select_image_source_slot(Stylesheet *sheet,
                                                uint8_t slot)
{
    if (sheet == NULL) return false;
    sheet->resolve_scratch->current_image_source_slot = slot;
    if (slot == 0) {
        sheet->resolve_scratch->current_image_source_base = NULL;
        sheet->resolve_scratch->current_image_source_referrer_policy = NULL;
        return true;
    }
    if (sheet->image_sources == NULL
        || slot > sheet->image_sources->count) return false;
    const StyleImageSourceContext *source =
        &sheet->image_sources->items[slot - 1u];
    const char *policy = style_image_referrer_policy_name(
        source->referrer_policy);
    if (source->base_url == NULL || policy == NULL) return false;
    sheet->resolve_scratch->current_image_source_base = source->base_url;
    sheet->resolve_scratch->current_image_source_referrer_policy = policy;
    return true;
}

static const char *store_image_url(Stylesheet *sheet, const char *text,
                                   size_t length)
{
    if (sheet == NULL || text == NULL || length == 0
        || length >= STYLE_IMAGE_URL_CAPACITY) return NULL;
    uint8_t source_slot = 0;
    if (!stylesheet_current_image_source_slot(sheet, &source_slot)) {
        return NULL;
    }
    for (size_t i = 0; i < sheet->image_url_count; i++) {
        const char *stored = sheet->image_urls[i];
        const StyleImageReference *reference =
            (const StyleImageReference *)
                ((const unsigned char *) stored
                 - offsetof(StyleImageReference, reference));
        if (reference->source_slot == source_slot
            && reference->length == length
            && memcmp(stored, text, length) == 0) {
            return stored;
        }
    }
    if (sheet->image_url_count >= STYLE_IMAGE_REFERENCE_LIMIT) return NULL;
    if (sheet->image_url_count == sheet->image_url_capacity) {
        size_t capacity = sheet->image_url_capacity == 0
                          ? 8 : sheet->image_url_capacity * 2u;
        if (capacity > STYLE_IMAGE_REFERENCE_LIMIT) {
            capacity = STYLE_IMAGE_REFERENCE_LIMIT;
        }
        if (capacity < sheet->image_url_capacity
            || capacity > SIZE_MAX / sizeof(*sheet->image_urls)) return NULL;
        char **urls = budget_realloc(
            sheet->budget, sheet->image_urls,
            capacity * sizeof(*sheet->image_urls));
        if (urls == NULL) return NULL;
        sheet->image_urls = urls;
        sheet->image_url_capacity = capacity;
    }
    size_t bytes = sizeof(StyleImageReference) + length + 1u;
    StyleImageReference *reference = budget_malloc(sheet->budget, bytes);
    if (reference == NULL) return NULL;
    reference->source_slot = source_slot;
    reference->length = (uint8_t) length;
    memcpy(reference->reference, text, length);
    reference->reference[length] = '\0';
    if (sheet->image_url_bytes > SIZE_MAX - bytes) {
        budget_free(sheet->budget, reference);
        return NULL;
    }
    sheet->image_urls[sheet->image_url_count++] = reference->reference;
    sheet->image_url_bytes += bytes;
    return reference->reference;
}

bool stylesheet_image_url_source(const Stylesheet *sheet,
                                 const char *reference,
                                 const char **source_base_url,
                                 const char **source_referrer_policy)
{
    if (source_base_url != NULL) *source_base_url = NULL;
    if (source_referrer_policy != NULL) *source_referrer_policy = NULL;
    if (sheet == NULL || reference == NULL) return false;
    for (size_t i = 0; i < sheet->image_url_count; i++) {
        if (sheet->image_urls[i] != reference) continue;
        const StyleImageReference *stored =
            (const StyleImageReference *)
                ((const unsigned char *) reference
                 - offsetof(StyleImageReference, reference));
        if (stored->length != strlen(reference)) return false;
        if (stored->source_slot == 0) return true;
        if (sheet->image_sources == NULL
            || stored->source_slot > sheet->image_sources->count) {
            return false;
        }
        const StyleImageSourceContext *source =
            &sheet->image_sources->items[stored->source_slot - 1u];
        const char *policy = style_image_referrer_policy_name(
            source->referrer_policy);
        if (source->base_url == NULL || policy == NULL) return false;
        if (source_base_url != NULL) *source_base_url = source->base_url;
        if (source_referrer_policy != NULL) {
            *source_referrer_policy = policy;
        }
        return true;
    }
    return false;
}

bool style_parse_image_url(Stylesheet *sheet, const char *text,
                            size_t length, const char **output)
{
    if (output == NULL) return false;
    char resolved[512];
    if (!style_resolve_value(sheet, text, length, resolved, sizeof(resolved), 0)) {
        return false;
    }
    const char *start = strstr(resolved, "url(");
    if (start == NULL) {
        if (strcmp(resolved, "none") == 0) {
            *output = NULL;
            return true;
        }
        return false;
    }
    start += 4;
    while (isspace((unsigned char) *start)) start++;
    char quote = (*start == '\'' || *start == '"') ? *start++ : '\0';
    char decoded[STYLE_IMAGE_URL_CAPACITY];
    size_t url_length = 0;
    const char *at = start;
    while (*at != '\0'
           && ((quote != '\0' && *at != quote)
               || (quote == '\0' && *at != ')'))) {
        uint32_t codepoint = (unsigned char) *at++;
        if (codepoint == '\\') {
            if (*at == '\0') return false;
            if (*at == '\n' || *at == '\f') {
                at++;
                continue;
            }
            if (*at == '\r') {
                at++;
                if (*at == '\n') at++;
                continue;
            }
            if (isxdigit((unsigned char) *at)) {
                codepoint = 0;
                size_t digits = 0;
                while (digits < 6 && isxdigit((unsigned char) *at)) {
                    unsigned char digit = (unsigned char) *at++;
                    codepoint = codepoint * 16u
                        + (digit >= '0' && digit <= '9'
                           ? (unsigned) (digit - '0')
                           : (unsigned) (tolower(digit) - 'a' + 10));
                    digits++;
                }
                if (isspace((unsigned char) *at)) {
                    if (*at == '\r' && at[1] == '\n') at++;
                    at++;
                }
                if (codepoint == 0 || codepoint > 0x10ffffu
                    || (codepoint >= 0xd800u && codepoint <= 0xdfffu)) {
                    codepoint = 0xfffdu;
                }
            } else {
                codepoint = (unsigned char) *at++;
            }
        }
        unsigned char encoded[4];
        size_t encoded_length;
        if (codepoint <= 0x7fu) {
            encoded[0] = (unsigned char) codepoint;
            encoded_length = 1;
        } else if (codepoint <= 0x7ffu) {
            encoded[0] = (unsigned char) (0xc0u | (codepoint >> 6));
            encoded[1] = (unsigned char) (0x80u | (codepoint & 0x3fu));
            encoded_length = 2;
        } else if (codepoint <= 0xffffu) {
            encoded[0] = (unsigned char) (0xe0u | (codepoint >> 12));
            encoded[1] = (unsigned char) (
                0x80u | ((codepoint >> 6) & 0x3fu));
            encoded[2] = (unsigned char) (0x80u | (codepoint & 0x3fu));
            encoded_length = 3;
        } else {
            encoded[0] = (unsigned char) (0xf0u | (codepoint >> 18));
            encoded[1] = (unsigned char) (
                0x80u | ((codepoint >> 12) & 0x3fu));
            encoded[2] = (unsigned char) (
                0x80u | ((codepoint >> 6) & 0x3fu));
            encoded[3] = (unsigned char) (0x80u | (codepoint & 0x3fu));
            encoded_length = 4;
        }
        if (encoded_length > sizeof(decoded) - 1u - url_length) {
            return false;
        }
        memcpy(decoded + url_length, encoded, encoded_length);
        url_length += encoded_length;
    }
    if (quote != '\0' && *at != quote) return false;
    if (quote == '\0') {
        while (url_length != 0
               && isspace((unsigned char) decoded[url_length - 1])) {
            url_length--;
        }
    }
    if (url_length == 0 || url_length >= STYLE_IMAGE_URL_CAPACITY) return false;
    decoded[url_length] = '\0';
    const char *stored = store_image_url(sheet, decoded, url_length);
    if (stored == NULL) return false;
    *output = stored;
    return true;
}

bool style_parse_background_shorthand_color(const Stylesheet *sheet,
                                             const char *text, size_t length,
                                             uint32_t *color,
                                             uint8_t *alpha,
                                             bool *transparent)
{
    char resolved[512];
    if (!style_resolve_value(sheet, text, length, resolved, sizeof(resolved), 0)) {
        return false;
    }
    length = strlen(resolved);
    bool found = false;
    *transparent = false;
    for (size_t at = 0; at < length;) {
        while (at < length && isspace((unsigned char) resolved[at])) at++;
        size_t end = at;
        int parentheses = 0;
        while (end < length) {
            if (resolved[end] == '(') parentheses++;
            else if (resolved[end] == ')' && parentheses > 0) parentheses--;
            if (parentheses == 0
                && isspace((unsigned char) resolved[end])) break;
            end++;
        }
        if (end > at) {
            uint32_t candidate = 0;
            uint8_t candidate_alpha = 255;
            if (style_parse_color_with_alpha(sheet, resolved + at, end - at,
                                       &candidate, &candidate_alpha)) {
                *color = candidate;
                *alpha = candidate_alpha;
                *transparent = candidate_alpha == 0;
                found = true;
            }
        }
        at = end;
    }
    return found;
}

static bool background_position_keyword(const char *token, size_t length)
{
    return (length == 4 && memcmp(token, "left", 4) == 0)
        || (length == 5 && memcmp(token, "right", 5) == 0)
        || (length == 3 && memcmp(token, "top", 3) == 0)
        || (length == 6 && memcmp(token, "bottom", 6) == 0)
        || (length == 6 && memcmp(token, "center", 6) == 0);
}

static bool background_length_token(const char *token, size_t length)
{
    if (length == 0) return false;
    char c = token[0];
    return c == '-' || c == '+' || c == '.' || isdigit((unsigned char) c);
}

bool style_parse_background_shorthand_position(
    const Stylesheet *sheet, const char *text, size_t length,
    ComputedStyle *style)
{
    if (sheet == NULL || text == NULL || style == NULL) return false;
    char resolved[512];
    if (!style_resolve_value(
            sheet, text, length, resolved, sizeof(resolved), 0)) {
        return false;
    }
    text = resolved;
    length = strlen(resolved);
    char position[96];
    size_t position_length = 0;
    bool after_slash = false;
    for (size_t at = 0; at < length;) {
        while (at < length && isspace((unsigned char) text[at])) at++;
        size_t start = at;
        int parentheses = 0;
        char quote = 0;
        while (at < length) {
            char c = text[at];
            if (quote != 0) {
                if (c == quote) quote = 0;
                at++;
                continue;
            }
            if (c == '\'' || c == '"') {
                quote = c;
                at++;
                continue;
            }
            if (c == '(') parentheses++;
            else if (c == ')' && parentheses > 0) parentheses--;
            else if (parentheses == 0 && isspace((unsigned char) c)) break;
            at++;
        }
        const char *token = text + start;
        size_t token_length = at - start;
        if (token_length == 0) continue;
        if (token_length == 1 && token[0] == '/') {
            after_slash = true;
            continue;
        }
        if (after_slash || (token_length >= 4
                            && memcmp(token, "url(", 4) == 0)) {
            continue;
        }
        if (background_position_keyword(token, token_length)
            || background_length_token(token, token_length)) {
            if (position_length != 0) {
                if (position_length + 1 >= sizeof(position)) return false;
                position[position_length++] = ' ';
            }
            if (position_length + token_length >= sizeof(position)) {
                return false;
            }
            memcpy(position + position_length, token, token_length);
            position_length += token_length;
        }
    }
    if (position_length == 0) return false;
    position[position_length] = '\0';
    return style_parse_background_position(
        sheet, position, position_length, style);
}

/* Paren/quote-aware top-level list iterator, shared by the multi-layer
   background shorthand and by gradient argument parsing so the engine keeps
   exactly one CSS list splitter.  *cursor starts at zero; each call writes
   the next whitespace-trimmed segment and returns false once the list is
   exhausted.  A separator inside url()/rgb()/linear-gradient() or inside a
   quoted string never splits. */
bool style_next_top_level(const char *text, size_t length, char separator,
                          size_t *cursor, size_t *start, size_t *end)
{
    if (text == NULL || cursor == NULL || start == NULL || end == NULL) {
        return false;
    }
    if (*cursor > length) return false;
    size_t at = *cursor;
    size_t segment_start = at;
    int depth = 0;
    char quote = 0;
    while (at < length) {
        char c = text[at];
        if (quote != 0) {
            if (c == quote) quote = 0;
            at++;
            continue;
        }
        if (c == '\'' || c == '"') { quote = c; at++; continue; }
        if (c == '(') depth++;
        else if (c == ')') { if (depth > 0) depth--; }
        else if (c == separator && depth == 0) break;
        at++;
    }
    size_t segment_end = at;
    *cursor = at + 1;
    while (segment_start < segment_end
           && isspace((unsigned char) text[segment_start])) segment_start++;
    while (segment_end > segment_start
           && isspace((unsigned char) text[segment_end - 1])) segment_end--;
    *start = segment_start;
    *end = segment_end;
    return true;
}

/* `to <side>` / `to <corner>`.  Corners are approximated with the 45-degree
   diagonals: the true CSS "magic corner" angle depends on the box aspect
   ratio, which is not known at parse time and is not worth a second
   resolution pass on this device. */
static bool gradient_side_angle(const char *text, size_t length,
                                uint16_t *angle)
{
    static const struct { const char *name; uint16_t angle; } sides[] = {
        {"to top", 0}, {"to right", 90}, {"to bottom", 180}, {"to left", 270},
        {"to top right", 45}, {"to right top", 45},
        {"to bottom right", 135}, {"to right bottom", 135},
        {"to bottom left", 225}, {"to left bottom", 225},
        {"to top left", 315}, {"to left top", 315}
    };
    /* Collapse internal whitespace runs so "to  top   right" compares. */
    char normalized[24];
    size_t out = 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char) text[i];
        if (isspace(c)) {
            if (out == 0 || normalized[out - 1] == ' ') continue;
            c = ' ';
        }
        if (out + 1 >= sizeof(normalized)) return false;
        normalized[out++] = (char) tolower(c);
    }
    while (out != 0 && normalized[out - 1] == ' ') out--;
    normalized[out] = '\0';
    for (size_t i = 0; i < sizeof(sides) / sizeof(sides[0]); i++) {
        if (strcmp(normalized, sides[i].name) == 0) {
            *angle = sides[i].angle;
            return true;
        }
    }
    return false;
}

static bool gradient_angle_value(const char *text, size_t length,
                                 uint16_t *angle)
{
    char buffer[32];
    if (length == 0 || length >= sizeof(buffer)) return false;
    memcpy(buffer, text, length);
    buffer[length] = '\0';
    char *end = NULL;
    double parsed = strtod(buffer, &end);
    if (end == buffer || !isfinite(parsed)) return false;
    while (isspace((unsigned char) *end)) end++;
    if (strcasecmp(end, "turn") == 0) parsed *= 360.0;
    else if (strcasecmp(end, "grad") == 0) parsed *= 0.9;
    else if (strcasecmp(end, "rad") == 0) parsed *= 57.29577951308232;
    else if (*end != '\0' && strcasecmp(end, "deg") != 0) return false;
    if (*end == '\0' && parsed != 0.0) return false;
    double normalized = fmod(parsed, 360.0);
    if (normalized < 0.0) normalized += 360.0;
    *angle = (uint16_t) (normalized + 0.5);
    if (*angle >= 360) *angle = 0;
    return true;
}

/* A stop offset, restricted to percentages and a bare zero.  Lengths would
   need the gradient line's used length, which is not available at parse
   time, so they are left unparseable. */
static bool gradient_stop_position(const char *text, size_t length,
                                   uint8_t *position)
{
    char buffer[32];
    if (length == 0 || length >= sizeof(buffer)) return false;
    memcpy(buffer, text, length);
    buffer[length] = '\0';
    char *end = NULL;
    double parsed = strtod(buffer, &end);
    if (end == buffer || !isfinite(parsed)) return false;
    while (isspace((unsigned char) *end)) end++;
    if (*end == '%') {
        end++;
        while (isspace((unsigned char) *end)) end++;
    } else if (!(*end == '\0' && parsed == 0.0)) {
        return false;
    }
    if (*end != '\0') return false;
    if (parsed < 0.0) parsed = 0.0;
    if (parsed > 100.0) parsed = 100.0;
    *position = (uint8_t) (parsed * 255.0 / 100.0 + 0.5);
    return true;
}

static bool gradient_radial_header(const char *text, size_t length,
                                   bool *circle)
{
    char normalized[96];
    size_t out = 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char) text[i];
        if (isspace(c)) {
            if (out == 0 || normalized[out - 1] == ' ') continue;
            c = ' ';
        }
        if (out + 1 >= sizeof(normalized)) return false;
        normalized[out++] = (char) tolower(c);
    }
    while (out != 0 && normalized[out - 1] == ' ') out--;
    normalized[out] = '\0';
    static const char *const ellipse_headers[] = {
        "ellipse", "farthest-corner", "ellipse farthest-corner",
        "at center", "at 50% 50%",
        "ellipse at center", "ellipse at 50% 50%",
        "ellipse farthest-corner at center",
        "ellipse farthest-corner at 50% 50%"
    };
    static const char *const circle_headers[] = {
        "circle", "circle farthest-corner", "circle at center",
        "circle at 50% 50%", "circle farthest-corner at center",
        "circle farthest-corner at 50% 50%"
    };
    for (size_t i = 0;
         i < sizeof(ellipse_headers) / sizeof(ellipse_headers[0]); i++) {
        if (strcmp(normalized, ellipse_headers[i]) == 0) {
            *circle = false;
            return true;
        }
    }
    for (size_t i = 0;
         i < sizeof(circle_headers) / sizeof(circle_headers[0]); i++) {
        if (strcmp(normalized, circle_headers[i]) == 0) {
            *circle = true;
            return true;
        }
    }
    return false;
}

/* Keep parsing bounded independently of the retained paint representation.
   Long generated ramps are reduced to evenly sampled stops instead of
   invalidating the entire background and exposing content that expected a
   darkening/readability overlay. */
#define STYLE_GRADIENT_PARSE_STOP_LIMIT 32u

/* Static linear and centred radial gradients with a hard retained stop cap.
   Colour interpolation clauses are accepted and resolved into the engine's
   sRGB ramp; custom radial centres and length stops remain outside this
   bounded representation. */
bool style_parse_gradient(const Stylesheet *sheet, const char *text,
                          size_t length, StyleGradient *gradient)
{
    if (gradient == NULL) return false;
    char resolved[512];
    if (!style_resolve_value(sheet, text, length, resolved, sizeof(resolved),
                             0)) return false;
    const char *value = resolved;
    size_t value_length = strlen(resolved);
    trim(&value, &value_length);
    const char *prefix = NULL;
    bool radial = false, repeating = false;
    if (strncasecmp(value, "linear-gradient(", 16) == 0) {
        prefix = "linear-gradient(";
    } else if (strncasecmp(value, "repeating-linear-gradient(", 26) == 0) {
        prefix = "repeating-linear-gradient("; repeating = true;
    } else if (strncasecmp(value, "radial-gradient(", 16) == 0) {
        prefix = "radial-gradient("; radial = true;
    } else if (strncasecmp(value, "repeating-radial-gradient(", 26) == 0) {
        prefix = "repeating-radial-gradient("; radial = repeating = true;
    } else return false;
    size_t prefix_length = strlen(prefix);
    if (value_length <= prefix_length || value[value_length - 1] != ')')
        return false;
    const char *inner = value + prefix_length;
    size_t inner_length = value_length - prefix_length - 1;

    uint16_t angle = 180;
    bool radial_circle = false;
    uint8_t positions[STYLE_GRADIENT_PARSE_STOP_LIMIT];
    bool positioned[STYLE_GRADIENT_PARSE_STOP_LIMIT];
    uint32_t colors[STYLE_GRADIENT_PARSE_STOP_LIMIT];
    size_t count = 0;
    bool first_segment = true;
    size_t cursor = 0, start = 0, end = 0;
    while (style_next_top_level(inner, inner_length, ',', &cursor,
                                &start, &end)) {
        const char *segment = inner + start;
        size_t segment_length = end - start;
        if (segment_length == 0) return false;
        if (first_segment) {
            first_segment = false;
            if (segment_length > 3
                && strncasecmp(segment, "in ", 3) == 0) continue;
            if (!radial) {
                if (gradient_side_angle(segment, segment_length, &angle)) continue;
                if (gradient_angle_value(segment, segment_length, &angle)) continue;
            } else if (gradient_radial_header(
                           segment, segment_length, &radial_circle)) {
                continue;
            }
        }
        if (count >= STYLE_GRADIENT_PARSE_STOP_LIMIT) return false;
        /* Split the stop into <color> and an optional trailing offset.  A
           segment that is nothing but an offset is a colour interpolation
           hint, which this pass does not support. */
        size_t split = segment_length;
        while (split != 0 && !isspace((unsigned char) segment[split - 1])) {
            split--;
        }
        uint8_t position = 0;
        bool has_position = false;
        size_t color_length = segment_length;
        if (split != 0
            && gradient_stop_position(segment + split,
                                      segment_length - split, &position)) {
            has_position = true;
            color_length = split;
            while (color_length != 0
                   && isspace((unsigned char) segment[color_length - 1])) {
                color_length--;
            }
        }
        uint8_t second_position = 0;
        bool has_second_position = false;
        if (has_position) {
            size_t second_split = color_length;
            while (second_split != 0
                   && !isspace((unsigned char) segment[second_split - 1])) {
                second_split--;
            }
            if (second_split != 0
                && gradient_stop_position(segment + second_split,
                                          color_length - second_split,
                                          &second_position)) {
                has_second_position = true;
                color_length = second_split;
                while (color_length != 0
                       && isspace((unsigned char)
                                  segment[color_length - 1])) color_length--;
            }
        }
        if (color_length == 0) return false;
        uint32_t color = 0;
        uint8_t alpha = 255;
        if (!style_parse_color_with_alpha(sheet, segment, color_length,
                                          &color, &alpha)) {
            return false;
        }
        colors[count] = ((uint32_t) alpha << 24) | (color & 0x00ffffffu);
        positions[count] = position;
        positioned[count] = has_position;
        count++;
        if (has_second_position) {
            if (count >= STYLE_GRADIENT_PARSE_STOP_LIMIT) return false;
            colors[count] = color;
            positions[count] = position;
            positioned[count] = true;
            positions[count - 1] = second_position;
            count++;
        }
    }
    if (count < 2) return false;

    /* Fill in omitted offsets: the ends default to 0% and 100%, interior
       runs are distributed evenly between their bracketing neighbours, and
       the whole list is forced non-decreasing (CSS 'Coloring the Gradient
       Line'). */
    if (!positioned[0]) { positions[0] = 0; positioned[0] = true; }
    if (!positioned[count - 1]) {
        positions[count - 1] = 255;
        positioned[count - 1] = true;
    }
    for (size_t i = 1; i + 1 < count; i++) {
        if (positioned[i]) continue;
        size_t next = i + 1;
        while (next < count && !positioned[next]) next++;
        int low = positions[i - 1];
        int high = positions[next];
        size_t gaps = next - (i - 1);
        for (size_t j = i; j < next; j++) {
            positions[j] = (uint8_t) (low
                + (high - low) * (int) (j - (i - 1)) / (int) gaps);
            positioned[j] = true;
        }
        i = next - 1;
    }
    for (size_t i = 1; i < count; i++) {
        if (positions[i] < positions[i - 1]) positions[i] = positions[i - 1];
    }

    memset(gradient, 0, sizeof(*gradient));
    gradient->angle = angle
        | (radial ? STYLE_GRADIENT_RADIAL : 0)
        | (radial_circle ? STYLE_GRADIENT_RADIAL_CIRCLE : 0)
        | (repeating ? STYLE_GRADIENT_REPEATING : 0);
    size_t retained = count < STYLE_GRADIENT_STOP_LIMIT
        ? count : STYLE_GRADIENT_STOP_LIMIT;
    gradient->stop_count = (uint8_t) retained;
    for (size_t i = 0; i < retained; i++) {
        size_t source = retained == 1 ? 0
            : i * (count - 1u) / (retained - 1u);
        gradient->stop_argb[i] = colors[source];
        gradient->stop_position[i] = positions[source];
    }
    return true;
}

/* A shadow length component. Font-relative and CSS math lengths are resolved
   against the cascade's current font basis; percentages remain invalid for
   both box-shadow and text-shadow. Anything outside the explicit unit list is
   unparseable, so the P1 fallback keeps the previous declaration. */
static bool box_shadow_length(const Stylesheet *sheet, const char *text,
                              size_t length, double *value)
{
    char buffer[128];
    if (length == 0 || length >= sizeof(buffer)) return false;
    memcpy(buffer, text, length);
    buffer[length] = '\0';
    bool math = (strncasecmp(buffer, "calc(", 5) == 0
                 || strncasecmp(buffer, "min(", 4) == 0
                 || strncasecmp(buffer, "max(", 4) == 0
                 || strncasecmp(buffer, "clamp(", 6) == 0)
                && buffer[length - 1] == ')';
    if (math) {
        bool percent = false;
        int parsed = style_parse_length(
            sheet, text, length, INT_MIN, &percent);
        if (parsed == INT_MIN || percent) return false;
        *value = parsed;
        return true;
    }
    char *end = NULL;
    double parsed = strtod(buffer, &end);
    if (end == buffer || !isfinite(parsed)) return false;
    while (isspace((unsigned char) *end)) end++;
    if (*end == '\0') {
        if (parsed != 0.0) return false;
        *value = 0.0;
        return true;
    }
    static const char *units[] = {
        "px", "em", "rem", "ch", "pt", "pc", "in", "cm", "mm", "q",
        "vw", "vh", "vi", "vb",
        "dvw", "dvh", "dvi", "dvb",
        "svw", "svh", "svi", "svb",
        "lvw", "lvh", "lvi", "lvb",
        "cqw", "cqh", "cqi", "cqb", "cqmin", "cqmax"
    };
    bool known = false;
    for (size_t i = 0; i < sizeof(units) / sizeof(units[0]); i++) {
        if (strcasecmp(end, units[i]) == 0) {
            known = true;
            break;
        }
    }
    if (!known) return false;
    bool percent = false;
    int pixels = style_parse_length(sheet, text, length, INT_MIN, &percent);
    if (pixels == INT_MIN || percent) return false;
    *value = pixels;
    return true;
}

static int8_t box_shadow_clamp(double value, int limit)
{
    if (!(value > -(double) limit)) return (int8_t) -limit;
    if (!(value < (double) limit)) return (int8_t) limit;
    return (int8_t) (value < 0.0 ? value - 0.5 : value + 0.5);
}

/* `box-shadow: <offset-x> <offset-y> [blur] [spread] [color] | inset`, as a
   comma-separated list capped at STYLE_BOX_SHADOW_LIMIT layers.  Geometry is
   clamped (see the limit comments in style.h) rather than rejected; an
   over-cap layer list, a bad unit, a negative blur or a second colour in one
   layer all make the declaration unparseable.  `none` parses to zero layers
   so an author can turn an inherited-looking shadow off. */
bool style_parse_box_shadow(const Stylesheet *sheet, const char *text,
                            size_t length, StyleBoxShadow *shadows,
                            uint8_t *count)
{
    if (shadows == NULL || count == NULL) return false;
    char resolved[512];
    if (!style_resolve_value(sheet, text, length, resolved, sizeof(resolved),
                             0)) return false;
    const char *value = resolved;
    size_t value_length = strlen(resolved);
    trim(&value, &value_length);
    if (value_length == 0) return false;
    if (strcasecmp(value, "none") == 0) {
        memset(shadows, 0, sizeof(*shadows) * STYLE_BOX_SHADOW_LIMIT);
        *count = 0;
        return true;
    }

    StyleBoxShadow parsed[STYLE_BOX_SHADOW_LIMIT];
    memset(parsed, 0, sizeof(parsed));
    size_t layers = 0;
    size_t cursor = 0, start = 0, end = 0;
    while (style_next_top_level(value, value_length, ',', &cursor,
                                &start, &end)) {
        if (end == start) return false;
        if (layers >= STYLE_BOX_SHADOW_LIMIT) return false;
        const char *layer = value + start;
        size_t layer_length = end - start;

        double lengths[4] = {0.0, 0.0, 0.0, 0.0};
        size_t length_count = 0;
        bool inset = false;
        bool has_color = false;
        bool current_color = false;
        uint32_t color = 0;
        uint8_t alpha = 255;
        size_t token_cursor = 0, token_start = 0, token_end = 0;
        while (style_next_top_level(layer, layer_length, ' ', &token_cursor,
                                    &token_start, &token_end)) {
            if (token_end == token_start) continue;
            const char *token = layer + token_start;
            size_t token_length = token_end - token_start;
            if (token_length == 5
                && strncasecmp(token, "inset", 5) == 0) {
                if (inset) return false;
                inset = true;
                continue;
            }
            if (token_length == 12
                && strncasecmp(token, "currentcolor", 12) == 0) {
                if (has_color) return false;
                has_color = true;
                current_color = true;
                continue;
            }
            double number = 0.0;
            if (box_shadow_length(sheet, token, token_length, &number)) {
                /* The lengths are positional among themselves, but CSS lets
                   the colour sit on either side of them. */
                if (length_count >= 4) return false;
                lengths[length_count++] = number;
                continue;
            }
            if (has_color) return false;
            if (!style_parse_color_with_alpha(sheet, token, token_length,
                                              &color, &alpha)) {
                return false;
            }
            has_color = true;
        }
        if (length_count < 2) return false;
        /* CSS forbids a negative blur radius; a negative spread is legal and
           shrinks the shadow. */
        if (lengths[2] < 0.0) return false;

        StyleBoxShadow *slot = &parsed[layers++];
        slot->offset_x = box_shadow_clamp(lengths[0],
                                          STYLE_BOX_SHADOW_OFFSET_LIMIT);
        slot->offset_y = box_shadow_clamp(lengths[1],
                                          STYLE_BOX_SHADOW_OFFSET_LIMIT);
        slot->spread = box_shadow_clamp(lengths[3],
                                        STYLE_BOX_SHADOW_SPREAD_LIMIT);
        int blur = box_shadow_clamp(lengths[2], STYLE_BOX_SHADOW_BLUR_LIMIT);
        if (blur < 0) blur = 0;
        slot->blur = (uint8_t) (blur & STYLE_BOX_SHADOW_BLUR_MASK);
        if (inset) slot->blur |= STYLE_BOX_SHADOW_INSET;
        if (has_color && !current_color) {
            slot->argb = ((uint32_t) alpha << 24) | (color & 0x00ffffffu);
        } else {
            /* The CSS initial shadow colour is currentColor, which is not
               settled until the cascade finishes. */
            slot->blur |= STYLE_BOX_SHADOW_CURRENT_COLOR;
        }
    }
    if (layers == 0) return false;

    memset(shadows, 0, sizeof(*shadows) * STYLE_BOX_SHADOW_LIMIT);
    for (size_t i = 0; i < layers; i++) shadows[i] = parsed[i];
    *count = (uint8_t) layers;
    return true;
}

bool style_parse_text_shadow(const Stylesheet *sheet, const char *text,
                             size_t length, StyleBoxShadow *shadows,
                             uint8_t *count)
{
    if (!style_parse_box_shadow(sheet, text, length, shadows, count)) {
        return false;
    }
    for (size_t i = 0; i < *count; i++) {
        /* text-shadow has no spread or inset grammar. Reusing the compact
           box-shadow representation is safe only after rejecting both. */
        if (shadows[i].spread != 0
            || style_box_shadow_is_inset(&shadows[i])) {
            return false;
        }
    }
    return true;
}

/* Comma-separated multi-layer background shorthand.  The engine paints a
   single background-image slot, so keep the bottommost (last) layer that
   carries a url -- that is the base background beneath any foreground
   overlay layers, and the one authors size to fill the box -- and apply
   THAT layer's own background-position and repeat.  A common banner shape is
   a small "left top" decoration layered over a 1800px scene positioned
   "-970px" with the fill color in the final
   layer.  Returns true only when the value held more than one top-level
   layer; single-layer values leave *style untouched so the caller's
   existing scalar path is a byte-for-byte no-op. */
static StylePaintStack *style_paint_storage_prepare_slot(
    Stylesheet *sheet, StylePaintStorage *storage, size_t index)
{
    if (sheet == NULL || sheet->budget == NULL || storage == NULL
        || index >= STYLE_PAINT_STACK_LIMIT) return NULL;
    size_t block_index = index / STYLE_PAINT_STACK_BLOCK_SIZE;
    if (block_index >= storage->capacity) {
        size_t capacity = storage->capacity == 0
            ? 4u : storage->capacity * 2u;
        if (capacity <= block_index) capacity = block_index + 1u;
        if (capacity > STYLE_PAINT_STACK_BLOCK_COUNT) {
            capacity = STYLE_PAINT_STACK_BLOCK_COUNT;
        }
        size_t old_capacity = storage->capacity;
        StylePaintStack **blocks = budget_realloc(
            sheet->budget, storage->blocks,
            capacity * sizeof(*blocks));
        if (blocks == NULL) return NULL;
        memset(blocks + old_capacity, 0,
               (capacity - old_capacity) * sizeof(*blocks));
        storage->blocks = blocks;
        storage->capacity = capacity;
    }
    if (storage->blocks[block_index] == NULL) {
        storage->blocks[block_index] = budget_calloc(
            sheet->budget, STYLE_PAINT_STACK_BLOCK_SIZE,
            sizeof(*storage->blocks[block_index]));
        if (storage->blocks[block_index] == NULL) return NULL;
    }
    return style_paint_storage_slot(storage, index);
}

StylePaintInternResult style_intern_paint_stack(
    Stylesheet *sheet, const StylePaintStack *stack, uint8_t *id)
{
    if (id != NULL) *id = 0;
    if (sheet == NULL || sheet->budget == NULL || stack == NULL
        || id == NULL) {
        return STYLE_PAINT_INTERN_INVALID;
    }
    StylePaintStorage *storage = sheet->paint_storage;
    for (size_t i = 0; storage != NULL && i < storage->count; i++) {
        const StylePaintStack *candidate =
            style_paint_storage_const_slot(storage, i);
        if (candidate != NULL
            && memcmp(candidate, stack, sizeof(*stack)) == 0) {
            *id = (uint8_t) (i + 1u);
            return STYLE_PAINT_INTERN_RETAINED;
        }
    }
    if (storage != NULL && storage->count >= STYLE_PAINT_STACK_LIMIT) {
        return STYLE_PAINT_INTERN_LIMIT;
    }
    if (storage == NULL) {
        storage = budget_calloc(sheet->budget, 1, sizeof(*storage));
        if (storage == NULL) return STYLE_PAINT_INTERN_OOM;
        sheet->paint_storage = storage;
    }
    size_t index = storage->count;
    StylePaintStack *retained =
        style_paint_storage_prepare_slot(sheet, storage, index);
    if (retained == NULL) return STYLE_PAINT_INTERN_OOM;
    *retained = *stack;
    storage->count++;
    *id = (uint8_t) (index + 1u);
    return STYLE_PAINT_INTERN_RETAINED;
}

StylePaintInternResult style_intern_text_shadow(
    Stylesheet *sheet, const StylePaintStack *source, uint8_t *id)
{
    if (id != NULL) *id = 0;
    if (sheet == NULL || sheet->budget == NULL || source == NULL
        || id == NULL
        || (source->components & STYLE_PAINT_COMPONENT_TEXT_SHADOW) == 0) {
        return STYLE_PAINT_INTERN_INVALID;
    }
    size_t shadow_bytes = sizeof(source->text_shadows);
    StylePaintStorage *storage = sheet->paint_storage;
    for (size_t i = 0; storage != NULL && i < storage->count; i++) {
        const StylePaintStack *candidate =
            style_paint_storage_const_slot(storage, i);
        if (candidate == NULL) continue;
        if (candidate->components == STYLE_PAINT_COMPONENT_TEXT_SHADOW
            && candidate->text_shadow_count == source->text_shadow_count
            && memcmp(candidate->text_shadows, source->text_shadows,
                      shadow_bytes) == 0) {
            *id = (uint8_t) (i + 1u);
            return STYLE_PAINT_INTERN_RETAINED;
        }
    }
    if (storage != NULL && storage->count >= STYLE_PAINT_STACK_LIMIT) {
        return STYLE_PAINT_INTERN_LIMIT;
    }
    if (storage == NULL) {
        storage = budget_calloc(sheet->budget, 1, sizeof(*storage));
        if (storage == NULL) return STYLE_PAINT_INTERN_OOM;
        sheet->paint_storage = storage;
    }
    size_t index = storage->count;
    StylePaintStack *retained =
        style_paint_storage_prepare_slot(sheet, storage, index);
    if (retained == NULL) return STYLE_PAINT_INTERN_OOM;
    memset(retained, 0, sizeof(*retained));
    retained->components = STYLE_PAINT_COMPONENT_TEXT_SHADOW;
    retained->text_shadow_count = source->text_shadow_count;
    memcpy(retained->text_shadows, source->text_shadows, shadow_bytes);
    storage->count++;
    *id = (uint8_t) (index + 1u);
    return STYLE_PAINT_INTERN_RETAINED;
}

/* A full optional-paint table is a bounded-quality limit, not a stylesheet
   failure.  Keep the previously retained paint in that case.  A real
   allocation failure remains distinguishable and propagates to the caller. */
bool style_apply_paint_stack(
    Stylesheet *sheet, ComputedStyle *style, const StylePaintStack *stack,
    bool *retained)
{
    static const StylePaintStack empty;
    if (stack != NULL && memcmp(stack, &empty, sizeof(*stack)) == 0) {
        computed_style_set_paint_stack_id(style, 0);
        if (retained != NULL) *retained = true;
        return true;
    }
    uint8_t id = 0;
    StylePaintInternResult result =
        style_intern_paint_stack(sheet, stack, &id);
    if (retained != NULL) {
        *retained = result == STYLE_PAINT_INTERN_RETAINED;
    }
    if (result == STYLE_PAINT_INTERN_RETAINED) {
        computed_style_set_paint_stack_id(style, id);
        return true;
    }
    return result == STYLE_PAINT_INTERN_LIMIT;
}

StylePaintStack style_paint_stack_copy(
    const Stylesheet *sheet, const ComputedStyle *style)
{
    uint8_t id = computed_style_paint_stack_id(style);
    if (sheet == NULL || sheet->paint_storage == NULL
        || id == 0 || id > sheet->paint_storage->count) {
        return (StylePaintStack) {0};
    }
    const StylePaintStack *stack =
        style_paint_storage_const_slot(sheet->paint_storage, id - 1u);
    return stack == NULL ? (StylePaintStack) {0} : *stack;
}

static StylePaintBox style_parse_paint_box_keyword(
    const char *text, size_t length, bool *valid)
{
    while (length != 0 && isspace((unsigned char) *text)) {
        text++;
        length--;
    }
    while (length != 0 && isspace((unsigned char) text[length - 1])) {
        length--;
    }
    *valid = true;
    if (length == 10 && memcmp(text, "border-box", 10) == 0) {
        return STYLE_PAINT_BOX_BORDER;
    }
    if (length == 11 && memcmp(text, "padding-box", 11) == 0) {
        return STYLE_PAINT_BOX_PADDING;
    }
    if (length == 11 && memcmp(text, "content-box", 11) == 0) {
        return STYLE_PAINT_BOX_CONTENT;
    }
    *valid = false;
    return STYLE_PAINT_BOX_BORDER;
}

bool style_parse_background_box(Stylesheet *sheet, const char *text,
                                size_t length, ComputedStyle *style,
                                bool set_origin, bool set_clip)
{
    char resolved[128];
    if (sheet == NULL || style == NULL
        || !style_resolve_value(sheet, text, length, resolved,
                                sizeof(resolved), 0)) {
        return false;
    }
    StylePaintStack stack = style_paint_stack_copy(sheet, style);
    if (stack.background_count == 0) {
        stack.background_count = 1;
        stack.backgrounds[0].origin = STYLE_PAINT_BOX_PADDING;
        stack.backgrounds[0].clip = STYLE_PAINT_BOX_BORDER;
    }
    size_t cursor = 0, start = 0, end = 0, layer = 0;
    bool any = false;
    while (layer < STYLE_PAINT_LAYER_LIMIT
           && style_next_top_level(resolved, strlen(resolved), ',',
                                   &cursor, &start, &end)) {
        bool valid = false;
        StylePaintBox box = style_parse_paint_box_keyword(
            resolved + start, end - start, &valid);
        if (!valid) return false;
        if (layer >= stack.background_count) {
            /* A longhand list may introduce geometry for layers not yet
               represented by background-image. Retain the CSS initial
               values for the other box component rather than inheriting the
               enum's zero-valued border-box for background-origin. */
            stack.backgrounds[layer].origin = STYLE_PAINT_BOX_PADDING;
            stack.backgrounds[layer].clip = STYLE_PAINT_BOX_BORDER;
        }
        if (set_origin) stack.backgrounds[layer].origin = box;
        if (set_clip) stack.backgrounds[layer].clip = box;
        any = true;
        layer++;
    }
    if (!any) return false;
    if (stack.background_count < layer) {
        stack.background_count = (uint8_t) layer;
    }
    stack.components |= STYLE_PAINT_COMPONENT_BACKGROUND_BOX;
    return style_apply_paint_stack(sheet, style, &stack, NULL);
}

bool style_parse_mask_image_layers(Stylesheet *sheet, const char *text,
                                   size_t length, ComputedStyle *style)
{
    char resolved[512];
    if (sheet == NULL || style == NULL
        || !style_resolve_value(sheet, text, length, resolved,
                                sizeof(resolved), 0)) {
        return false;
    }
    StylePaintStack stack = style_paint_stack_copy(sheet, style);
    memset(stack.masks, 0, sizeof(stack.masks));
    stack.mask_count = 0;
    size_t cursor = 0, start = 0, end = 0;
    while (stack.mask_count < STYLE_PAINT_LAYER_LIMIT
           && style_next_top_level(resolved, strlen(resolved), ',',
                                   &cursor, &start, &end)) {
        StylePaintLayer *layer = &stack.masks[stack.mask_count];
        layer->origin = STYLE_PAINT_BOX_BORDER;
        layer->clip = STYLE_PAINT_BOX_BORDER;
        if (style_parse_gradient(sheet, resolved + start, end - start,
                                 &layer->gradient)) {
            layer->kind = STYLE_PAINT_IMAGE_GRADIENT;
        } else if (style_parse_image_url(
                       sheet, resolved + start, end - start,
                       &layer->image)) {
            layer->kind = layer->image == NULL
                ? STYLE_PAINT_IMAGE_NONE : STYLE_PAINT_IMAGE_URL;
        } else {
            return false;
        }
        stack.mask_count++;
    }
    if (stack.mask_count == 0) return false;
    stack.components |= STYLE_PAINT_COMPONENT_MASK_IMAGE;
    bool retained = false;
    if (!style_apply_paint_stack(
            sheet, style, &stack, &retained)) return false;
    if (!retained) return true;
    style->mask_image = stack.masks[0].kind == STYLE_PAINT_IMAGE_URL
        ? stack.masks[0].image : NULL;
    return true;
}

static bool mask_shorthand_word(const char *word, size_t length,
                                const char *wanted)
{
    return strlen(wanted) == length && memcmp(word, wanted, length) == 0;
}

static bool style_parse_paint_position_value(
    Stylesheet *sheet, const char *value, size_t length,
    StylePaintLayer *layer);

static bool mask_shorthand_append(char *output, size_t capacity,
                                  size_t *length, const char *word,
                                  size_t word_length)
{
    size_t separator = *length == 0 ? 0u : 1u;
    if (*length + separator >= capacity
        || word_length > capacity - 1u - *length - separator) {
        return false;
    }
    if (separator != 0) output[(*length)++] = ' ';
    memcpy(output + *length, word, word_length);
    *length += word_length;
    output[*length] = '\0';
    return true;
}

/* Parse the bounded, commonly shipped part of the mask shorthand: an image,
   position/size and repeat. Keeping it as one parse
   operation also avoids interning four transient paint-stack variants before
   the final value is retained on generated stylesheets. */
bool style_parse_mask_shorthand(Stylesheet *sheet, const char *text,
                                size_t length, ComputedStyle *style)
{
    char resolved[512];
    if (sheet == NULL || style == NULL
        || !style_resolve_value(sheet, text, length, resolved,
                                sizeof(resolved), 0)) {
        return false;
    }
    if (strcmp(resolved, "initial") == 0
        || strcmp(resolved, "unset") == 0) {
        memcpy(resolved, "none", sizeof("none"));
    }
    length = strlen(resolved);
    StylePaintStack stack = style_paint_stack_copy(sheet, style);
    memset(stack.masks, 0, sizeof(stack.masks));
    stack.mask_count = 0;

    size_t cursor = 0, start = 0, end = 0;
    while (stack.mask_count < STYLE_PAINT_LAYER_LIMIT
           && style_next_top_level(resolved, length, ',', &cursor,
                                   &start, &end)) {
        StylePaintLayer *layer = &stack.masks[stack.mask_count];
        layer->origin = STYLE_PAINT_BOX_BORDER;
        layer->clip = STYLE_PAINT_BOX_BORDER;

        size_t image_start = SIZE_MAX, image_end = SIZE_MAX;
        for (size_t at = start; at < end;) {
            while (at < end && isspace((unsigned char) resolved[at])) at++;
            if (at < end && resolved[at] == '/') { at++; continue; }
            size_t token_start = at;
            unsigned depth = 0;
            char quote = 0;
            while (at < end) {
                char c = resolved[at];
                if (quote != 0) {
                    if (c == quote) quote = 0;
                    at++;
                    continue;
                }
                if (c == '\'' || c == '"') {
                    quote = c;
                    at++;
                    continue;
                }
                if (c == '(') depth++;
                else if (c == ')' && depth != 0) depth--;
                else if (depth == 0
                         && (isspace((unsigned char) c) || c == '/')) {
                    break;
                }
                at++;
            }
            size_t token_length = at - token_start;
            bool image_token = token_length == 4
                && memcmp(resolved + token_start, "none", 4) == 0;
            if (!image_token && token_length >= 5) {
                image_token = memcmp(resolved + token_start, "url(", 4) == 0
                    || (token_length >= 16
                        && memcmp(resolved + token_start,
                                  "linear-gradient(", 16) == 0)
                    || (token_length >= 16
                        && memcmp(resolved + token_start,
                                  "radial-gradient(", 16) == 0)
                    || (token_length >= 26
                        && memcmp(resolved + token_start,
                                  "repeating-linear-gradient(", 26) == 0)
                    || (token_length >= 26
                        && memcmp(resolved + token_start,
                                  "repeating-radial-gradient(", 26) == 0);
            }
            if (image_token) {
                if (image_start != SIZE_MAX) return false;
                image_start = token_start;
                image_end = at;
            }
        }
        if (image_start == SIZE_MAX) return false;
        if (style_parse_gradient(sheet, resolved + image_start,
                                 image_end - image_start,
                                 &layer->gradient)) {
            layer->kind = STYLE_PAINT_IMAGE_GRADIENT;
        } else {
            const char *url = NULL;
            if (!style_parse_image_url(sheet, resolved + image_start,
                                       image_end - image_start, &url)) {
                return false;
            }
            layer->kind = url == NULL ? STYLE_PAINT_IMAGE_NONE
                                      : STYLE_PAINT_IMAGE_URL;
            layer->image = url;
        }

        char position[96] = {0}, size[96] = {0};
        size_t position_length = 0, size_length = 0;
        bool after_slash = false;
        const char *repeat_words[2] = {NULL, NULL};
        size_t repeat_lengths[2] = {0, 0}, repeat_count = 0;
        for (size_t at = start; at < end;) {
            while (at < end && isspace((unsigned char) resolved[at])) at++;
            if (at < end && resolved[at] == '/') {
                after_slash = true;
                at++;
                continue;
            }
            size_t token_start = at;
            unsigned depth = 0;
            char quote = 0;
            while (at < end) {
                char c = resolved[at];
                if (quote != 0) {
                    if (c == quote) quote = 0;
                    at++;
                    continue;
                }
                if (c == '\'' || c == '"') {
                    quote = c;
                    at++;
                    continue;
                }
                if (c == '(') depth++;
                else if (c == ')' && depth != 0) depth--;
                else if (depth == 0
                         && (isspace((unsigned char) c) || c == '/')) {
                    break;
                }
                at++;
            }
            size_t token_length = at - token_start;
            if (token_length == 0) continue;
            if (token_start == image_start && at == image_end) continue;
            const char *token = resolved + token_start;
            bool valid_box = false;
            StylePaintBox box = style_parse_paint_box_keyword(
                token, token_length, &valid_box);
            if (valid_box) {
                /* The renderer's bounded mask path currently uses the
                   border box. Accept only that initial shorthand value;
                   dropping a non-default origin/clip would misplace masks. */
                if (box != STYLE_PAINT_BOX_BORDER) return false;
                continue;
            }
            bool repeat = mask_shorthand_word(token, token_length, "repeat")
                || mask_shorthand_word(token, token_length, "no-repeat")
                || mask_shorthand_word(token, token_length, "repeat-x")
                || mask_shorthand_word(token, token_length, "repeat-y");
            if (repeat) {
                if (repeat_count >= 2) return false;
                repeat_words[repeat_count] = token;
                repeat_lengths[repeat_count++] = token_length;
                continue;
            }
            /* These are the initial practical modes supported by the alpha
               mask raster path. Non-default Porter-Duff operators are not
               accepted because silently treating them as add is unsafe. */
            if (mask_shorthand_word(token, token_length, "alpha")
                || mask_shorthand_word(token, token_length, "match-source")
                || mask_shorthand_word(token, token_length, "add")) {
                continue;
            }
            if (mask_shorthand_word(token, token_length, "luminance")
                || mask_shorthand_word(token, token_length, "subtract")
                || mask_shorthand_word(token, token_length, "intersect")
                || mask_shorthand_word(token, token_length, "exclude")) {
                return false;
            }
            char *target = after_slash ? size : position;
            size_t *target_length = after_slash
                ? &size_length : &position_length;
            if (!mask_shorthand_append(target, 96, target_length,
                                       token, token_length)) return false;
        }
        if (position_length != 0
            && !style_parse_paint_position_value(
                   sheet, position, position_length, layer)) return false;
        if (size_length != 0) {
            uint8_t flags = 0;
            if (!style_parse_background_size_values(
                    sheet, size, size_length, &layer->width, &layer->height,
                    &layer->fit, &flags)) return false;
            layer->flags |= flags;
        }
        if (repeat_count == 1
            && mask_shorthand_word(repeat_words[0], repeat_lengths[0],
                                   "no-repeat")) {
            layer->flags |= STYLE_BACKGROUND_NO_REPEAT_X
                            | STYLE_BACKGROUND_NO_REPEAT_Y;
        } else if (repeat_count == 1
                   && mask_shorthand_word(repeat_words[0], repeat_lengths[0],
                                          "repeat-x")) {
            layer->flags |= STYLE_BACKGROUND_NO_REPEAT_Y;
        } else if (repeat_count == 1
                   && mask_shorthand_word(repeat_words[0], repeat_lengths[0],
                                          "repeat-y")) {
            layer->flags |= STYLE_BACKGROUND_NO_REPEAT_X;
        } else if (repeat_count == 2) {
            if (mask_shorthand_word(repeat_words[0], repeat_lengths[0],
                                    "no-repeat")) {
                layer->flags |= STYLE_BACKGROUND_NO_REPEAT_X;
            }
            if (mask_shorthand_word(repeat_words[1], repeat_lengths[1],
                                    "no-repeat")) {
                layer->flags |= STYLE_BACKGROUND_NO_REPEAT_Y;
            }
        }
        stack.mask_count++;
    }
    if (stack.mask_count == 0) return false;
    stack.components |= STYLE_PAINT_COMPONENT_MASK_IMAGE;
    bool retained = false;
    if (!style_apply_paint_stack(sheet, style, &stack, &retained)) {
        return false;
    }
    if (retained) {
        style->mask_image = stack.masks[0].kind == STYLE_PAINT_IMAGE_URL
            ? stack.masks[0].image : NULL;
    }
    return true;
}

static bool paint_position_edge_keyword(
    const char *text, size_t length, bool *horizontal, bool *from_end)
{
    if (length == 4 && memcmp(text, "left", 4) == 0) {
        *horizontal = true;
        *from_end = false;
        return true;
    }
    if (length == 5 && memcmp(text, "right", 5) == 0) {
        *horizontal = true;
        *from_end = true;
        return true;
    }
    if (length == 3 && memcmp(text, "top", 3) == 0) {
        *horizontal = false;
        *from_end = false;
        return true;
    }
    if (length == 6 && memcmp(text, "bottom", 6) == 0) {
        *horizontal = false;
        *from_end = true;
        return true;
    }
    return false;
}

static bool paint_position_number(const char *text, size_t length,
                                  int *value, bool *pixels)
{
    char token[32];
    if (length == 0 || length >= sizeof(token)) return false;
    memcpy(token, text, length);
    token[length] = '\0';
    char *unit = NULL;
    double number = strtod(token, &unit);
    if (unit == token || !isfinite(number)) return false;
    if (strcmp(unit, "px") == 0
        || (*unit == '\0' && number == 0.0)) {
        *pixels = true;
    } else if (strcmp(unit, "%") == 0) {
        *pixels = false;
    } else {
        return false;
    }
    if (number > INT16_MAX) number = INT16_MAX;
    if (number < INT16_MIN) number = INT16_MIN;
    *value = (int) number;
    return true;
}

static bool style_parse_paint_position_value(
    Stylesheet *sheet, const char *value, size_t length,
    StylePaintLayer *layer)
{
    const char *parts[4] = {0};
    size_t lengths[4] = {0};
    size_t count = 0;
    for (size_t at = 0; at < length && count < 4;) {
        while (at < length && isspace((unsigned char) value[at])) at++;
        if (at == length) break;
        size_t start = at;
        while (at < length && !isspace((unsigned char) value[at])) at++;
        parts[count] = value + start;
        lengths[count++] = at - start;
    }
    size_t trailing = count == 0 ? 0
        : (size_t) ((parts[count - 1] + lengths[count - 1]) - value);
    while (trailing < length
           && isspace((unsigned char) value[trailing])) trailing++;
    if (count == 0 || trailing != length) return false;
    if (count <= 2) {
        ComputedStyle parsed = {0};
        if (!style_parse_background_position(
                sheet, value, length, &parsed)) return false;
        layer->position_x = parsed.background_position_x;
        layer->position_y = parsed.background_position_y;
        layer->position_edges = 0;
        layer->flags = (uint8_t) (
            (layer->flags & ~STYLE_BACKGROUND_POSITION_PIXELS)
            | (parsed.background_size_flags
               & STYLE_BACKGROUND_POSITION_PIXELS));
        layer->position_edges &= (uint8_t) ~(
            STYLE_PAINT_POSITION_FROM_RIGHT
            | STYLE_PAINT_POSITION_FROM_BOTTOM
            | STYLE_PAINT_POSITION_X_PIXELS
            | STYLE_PAINT_POSITION_Y_PIXELS);
        if ((parsed.background_size_flags
             & STYLE_BACKGROUND_POSITION_PIXELS) != 0) {
            layer->position_edges |= STYLE_PAINT_POSITION_X_PIXELS
                                     | STYLE_PAINT_POSITION_Y_PIXELS;
        }
        return true;
    }

    uint8_t edges = 0;
    int positions[2] = {50, 50};
    bool axes[2] = {false, false};
    size_t centers = 0;
    for (size_t at = 0; at < count;) {
        if (lengths[at] == 6
            && memcmp(parts[at], "center", 6) == 0) {
            centers++;
            at++;
            continue;
        }
        bool horizontal = false, from_end = false;
        if (!paint_position_edge_keyword(
                parts[at], lengths[at], &horizontal, &from_end)) {
            return false;
        }
        size_t axis = horizontal ? 0u : 1u;
        if (axes[axis] || at + 1 >= count) return false;
        bool pixels = false;
        if (!paint_position_number(
                parts[at + 1], lengths[at + 1],
                &positions[axis], &pixels)) return false;
        axes[axis] = true;
        if (from_end && horizontal) {
            edges |= STYLE_PAINT_POSITION_FROM_RIGHT;
        } else if (from_end) {
            edges |= STYLE_PAINT_POSITION_FROM_BOTTOM;
        }
        if (pixels) {
            edges |= horizontal ? STYLE_PAINT_POSITION_X_PIXELS
                                : STYLE_PAINT_POSITION_Y_PIXELS;
        }
        at += 2;
    }
    if (!axes[0] && centers != 0) {
        axes[0] = true;
        centers--;
    }
    if (!axes[1] && centers != 0) {
        axes[1] = true;
        centers--;
    }
    if (!axes[0] || !axes[1] || centers != 0) return false;
    layer->position_x = (int16_t) positions[0];
    layer->position_y = (int16_t) positions[1];
    layer->position_edges = edges;
    layer->flags &= (uint8_t) ~STYLE_BACKGROUND_POSITION_PIXELS;
    return true;
}

static bool style_parse_layer_geometry(
    Stylesheet *sheet, const char *text, size_t length, ComputedStyle *style,
    unsigned component)
{
    char resolved[384];
    if (sheet == NULL || style == NULL
        || !style_resolve_value(sheet, text, length, resolved,
                                sizeof(resolved), 0)) {
        return false;
    }
    StylePaintStack stack = style_paint_stack_copy(sheet, style);
    if (stack.mask_count == 0) stack.mask_count = 1;
    size_t cursor = 0, start = 0, end = 0, index = 0;
    while (index < STYLE_PAINT_LAYER_LIMIT
           && style_next_top_level(resolved, strlen(resolved), ',',
                                   &cursor, &start, &end)) {
        char value[96];
        size_t span = end - start;
        if (span >= sizeof(value)) return false;
        memcpy(value, resolved + start, span);
        value[span] = '\0';
        ComputedStyle parsed = {0};
        if (component == 0) {
            if (!style_parse_background_size(
                    sheet, value, span, &parsed)) return false;
            stack.masks[index].width = parsed.background_width;
            stack.masks[index].height = parsed.background_height;
            stack.masks[index].fit = parsed.background_fit;
            stack.masks[index].flags = parsed.background_size_flags;
        } else if (component == 1) {
            if (!style_parse_paint_position_value(
                    sheet, value, span, &stack.masks[index])) return false;
        } else {
            while (span != 0
                   && isspace((unsigned char) value[span - 1])) {
                value[--span] = '\0';
            }
            uint8_t flags = stack.masks[index].flags
                & (uint8_t) ~(STYLE_BACKGROUND_NO_REPEAT_X
                              | STYLE_BACKGROUND_NO_REPEAT_Y);
            if (strcmp(value, "no-repeat") == 0
                || strcmp(value, "no-repeat no-repeat") == 0) {
                flags |= STYLE_BACKGROUND_NO_REPEAT_X
                         | STYLE_BACKGROUND_NO_REPEAT_Y;
            } else if (strcmp(value, "repeat-x") == 0
                       || strcmp(value, "repeat no-repeat") == 0) {
                flags |= STYLE_BACKGROUND_NO_REPEAT_Y;
            } else if (strcmp(value, "repeat-y") == 0
                       || strcmp(value, "no-repeat repeat") == 0) {
                flags |= STYLE_BACKGROUND_NO_REPEAT_X;
            } else if (strcmp(value, "repeat") != 0
                       && strcmp(value, "repeat repeat") != 0) {
                return false;
            }
            stack.masks[index].flags = flags;
        }
        index++;
    }
    if (index == 0) return false;
    return style_apply_paint_stack(sheet, style, &stack, NULL);
}

bool style_parse_mask_layer_size(Stylesheet *sheet, const char *text,
                                 size_t length, ComputedStyle *style)
{
    return style_parse_layer_geometry(sheet, text, length, style, 0);
}

bool style_parse_mask_layer_position(Stylesheet *sheet, const char *text,
                                     size_t length, ComputedStyle *style)
{
    return style_parse_layer_geometry(sheet, text, length, style, 1);
}

bool style_parse_background_layer_position(
    Stylesheet *sheet, const char *text, size_t length,
    ComputedStyle *style)
{
    char resolved[384];
    if (sheet == NULL || style == NULL
        || !style_resolve_value(sheet, text, length, resolved,
                                sizeof(resolved), 0)) {
        return false;
    }
    StylePaintStack stack = style_paint_stack_copy(sheet, style);
    if (stack.background_count == 0) stack.background_count = 1;
    size_t cursor = 0, start = 0, end = 0, index = 0;
    while (index < STYLE_PAINT_LAYER_LIMIT
           && style_next_top_level(resolved, strlen(resolved), ',',
                                   &cursor, &start, &end)) {
        if (!style_parse_paint_position_value(
                sheet, resolved + start, end - start,
                &stack.backgrounds[index])) {
            return false;
        }
        index++;
    }
    if (index == 0) return false;
    return style_apply_paint_stack(sheet, style, &stack, NULL);
}

bool style_parse_background_layer_size(
    Stylesheet *sheet, const char *text, size_t length,
    ComputedStyle *style)
{
    char resolved[384];
    if (sheet == NULL || style == NULL
        || !style_resolve_value(sheet, text, length, resolved,
                                sizeof(resolved), 0)) {
        return false;
    }
    StylePaintStack stack = style_paint_stack_copy(sheet, style);
    if (stack.background_count == 0) stack.background_count = 1;
    size_t cursor = 0, start = 0, end = 0, index = 0;
    while (index < STYLE_PAINT_LAYER_LIMIT
           && style_next_top_level(resolved, strlen(resolved), ',',
                                   &cursor, &start, &end)) {
        ComputedStyle parsed = {0};
        if (!style_parse_background_size(
                sheet, resolved + start, end - start, &parsed)) {
            return false;
        }
        StylePaintLayer *layer = &stack.backgrounds[index];
        layer->width = parsed.background_width;
        layer->height = parsed.background_height;
        layer->fit = parsed.background_fit;
        layer->flags = (uint8_t) (
            (layer->flags
             & ~(STYLE_BACKGROUND_SIZE_EXPLICIT
                 | STYLE_BACKGROUND_WIDTH_AUTO
                 | STYLE_BACKGROUND_HEIGHT_AUTO
                 | STYLE_BACKGROUND_WIDTH_PERCENT
                 | STYLE_BACKGROUND_HEIGHT_PERCENT))
            | (parsed.background_size_flags
               & (STYLE_BACKGROUND_SIZE_EXPLICIT
                  | STYLE_BACKGROUND_WIDTH_AUTO
                  | STYLE_BACKGROUND_HEIGHT_AUTO
                  | STYLE_BACKGROUND_WIDTH_PERCENT
                  | STYLE_BACKGROUND_HEIGHT_PERCENT)));
        index++;
    }
    if (index == 0) return false;
    return style_apply_paint_stack(sheet, style, &stack, NULL);
}

bool style_parse_mask_layer_repeat(Stylesheet *sheet, const char *text,
                                   size_t length, ComputedStyle *style)
{
    return style_parse_layer_geometry(sheet, text, length, style, 2);
}

static void style_parse_background_layer_shorthand_geometry(
    Stylesheet *sheet, const char *text, size_t length,
    StylePaintLayer *layer)
{
    if (sheet == NULL || text == NULL || layer == NULL || length == 0
        || length >= 384) return;
    char copy[384];
    memcpy(copy, text, length);
    copy[length] = '\0';
    ComputedStyle parsed = {0};
    if (style_parse_background_shorthand_position(
            sheet, copy, length, &parsed)) {
        layer->position_x = parsed.background_position_x;
        layer->position_y = parsed.background_position_y;
        layer->flags = (uint8_t) (
            layer->flags
            | (parsed.background_size_flags
               & STYLE_BACKGROUND_POSITION_PIXELS));
    }
    if (strstr(copy, "no-repeat") != NULL) {
        layer->flags |= STYLE_BACKGROUND_NO_REPEAT_X
                        | STYLE_BACKGROUND_NO_REPEAT_Y;
    } else if (strstr(copy, "repeat-x") != NULL) {
        layer->flags |= STYLE_BACKGROUND_NO_REPEAT_Y;
    } else if (strstr(copy, "repeat-y") != NULL) {
        layer->flags |= STYLE_BACKGROUND_NO_REPEAT_X;
    }
    if (strstr(copy, "cover") != NULL) layer->fit = 1;
    else if (strstr(copy, "contain") != NULL) layer->fit = 2;
}

bool style_parse_background_shorthand_image(Stylesheet *sheet,
                                            const char *text, size_t length,
                                            ComputedStyle *style)
{
    if (style == NULL) return false;
    char resolved[512];
    if (!style_resolve_value(sheet, text, length, resolved, sizeof(resolved),
                             0)) return false;
    length = strlen(resolved);
    /* Split into top-level layers and remember the last one holding a url,
       tracking parentheses and quotes so a comma inside url()/rgb() or a
       quoted string never splits a layer. */
    size_t layer_count = 0;
    size_t chosen_start = 0, chosen_end = 0;
    size_t chosen_layer = SIZE_MAX;
    bool chosen_found = false;
    size_t gradient_count = 0;
    StylePaintStack stack = style_paint_stack_copy(sheet, style);
    memset(stack.backgrounds, 0, sizeof(stack.backgrounds));
    stack.background_count = 0;
    stack.components |= STYLE_PAINT_COMPONENT_BACKGROUND_IMAGE;
    size_t cursor = 0, seg_start = 0, seg_end = 0;
    while (style_next_top_level(resolved, length, ',', &cursor,
                                &seg_start, &seg_end)) {
        size_t layer_index = layer_count;
        layer_count++;
        StyleGradient candidate = {0};
        if (style_parse_gradient(sheet, resolved + seg_start,
                                 seg_end - seg_start, &candidate)) {
            gradient_count++;
            if (layer_index < STYLE_PAINT_LAYER_LIMIT) {
                StylePaintLayer *paint = &stack.backgrounds[layer_index];
                paint->kind = STYLE_PAINT_IMAGE_GRADIENT;
                paint->gradient = candidate;
                paint->origin = STYLE_PAINT_BOX_PADDING;
                paint->clip = STYLE_PAINT_BOX_BORDER;
                stack.background_count = (uint8_t) (layer_index + 1u);
            }
        }
        for (size_t i = seg_start; i + 4 <= seg_end; i++) {
            if (memcmp(resolved + i, "url(", 4) == 0) {
                chosen_start = seg_start;
                chosen_end = seg_end;
                chosen_layer = layer_index;
                chosen_found = true;
                if (layer_index < STYLE_PAINT_LAYER_LIMIT) {
                    StylePaintLayer *paint =
                        &stack.backgrounds[layer_index];
                    const char *url = NULL;
                    if (style_parse_image_url(
                            sheet, resolved + seg_start,
                            seg_end - seg_start, &url)) {
                        paint->kind = url == NULL
                            ? STYLE_PAINT_IMAGE_NONE
                            : STYLE_PAINT_IMAGE_URL;
                        paint->image = url;
                        paint->origin = STYLE_PAINT_BOX_PADDING;
                        paint->clip = STYLE_PAINT_BOX_BORDER;
                        stack.background_count =
                            (uint8_t) (layer_index + 1u);
                    }
                }
                break;
            }
        }
        if (layer_index < STYLE_PAINT_LAYER_LIMIT
            && layer_index < stack.background_count) {
            style_parse_background_layer_shorthand_geometry(
                sheet, resolved + seg_start, seg_end - seg_start,
                &stack.backgrounds[layer_index]);
        }
    }
    if (layer_count < 2) return false;
    if (!chosen_found && gradient_count != 0) {
        style->background_image_kind = STYLE_BACKGROUND_IMAGE_GRADIENT;
    }
    if (!chosen_found) {
        bool retained = false;
        if (!style_apply_paint_stack(
                sheet, style, &stack, &retained)) return false;
        return gradient_count != 0 && retained;
    }
    const char *layer = resolved + chosen_start;
    size_t layer_length = chosen_end - chosen_start;
    (void) style_parse_image_url(sheet, layer, layer_length,
                                 &style->background_image);
    /* Under the three-layer PSP cap, preserve a trailing authored bitmap
       rather than spending every retained slot on decorative gradients.
       Hero art and sprites are commonly the final layer beneath several
       translucent overlays; dropping that final URL produces a readable but
       visibly empty panel. The first two (topmost) layers retain ordering and
       the bottom retained slot becomes the asset-bearing layer. */
    size_t retained_chosen_layer = chosen_layer;
    if (chosen_layer >= STYLE_PAINT_LAYER_LIMIT
        && style->background_image != NULL) {
        retained_chosen_layer = STYLE_PAINT_LAYER_LIMIT - 1u;
        StylePaintLayer *paint =
            &stack.backgrounds[retained_chosen_layer];
        memset(paint, 0, sizeof(*paint));
        paint->kind = STYLE_PAINT_IMAGE_URL;
        paint->image = style->background_image;
        paint->origin = STYLE_PAINT_BOX_PADDING;
        paint->clip = STYLE_PAINT_BOX_BORDER;
        stack.background_count = STYLE_PAINT_LAYER_LIMIT;
    }
    /* Tokenize the chosen layer: url()/color are consumed elsewhere, size
       keywords ride behind a slash, repeat keywords toggle the repeat
       flags, and the remaining keyword/length tokens are the position. */
    char position[96];
    size_t position_length = 0;
    bool after_slash = false;
    style->background_fit = 0;
    for (size_t at = 0; at < layer_length;) {
        while (at < layer_length
               && isspace((unsigned char) layer[at])) at++;
        size_t token_start = at;
        int token_depth = 0;
        char token_quote = 0;
        while (at < layer_length) {
            char c = layer[at];
            if (token_quote != 0) {
                if (c == token_quote) token_quote = 0;
                at++;
                continue;
            }
            if (c == '\'' || c == '"') { token_quote = c; at++; continue; }
            if (c == '(') token_depth++;
            else if (c == ')' && token_depth > 0) token_depth--;
            else if (token_depth == 0 && isspace((unsigned char) c)) break;
            at++;
        }
        const char *token = layer + token_start;
        size_t token_length = at - token_start;
        if (token_length == 0) continue;
        if (token_length == 1 && token[0] == '/') { after_slash = true; continue; }
        if (token_length >= 4 && memcmp(token, "url(", 4) == 0) continue;
        if (token_length == 5 && memcmp(token, "cover", 5) == 0) {
            style->background_fit = 1;
            continue;
        }
        if (token_length == 7 && memcmp(token, "contain", 7) == 0) {
            style->background_fit = 2;
            continue;
        }
        if (after_slash) continue;
        if (token_length == 9 && memcmp(token, "no-repeat", 9) == 0) {
            style->background_size_flags |= STYLE_BACKGROUND_NO_REPEAT_X
                                            | STYLE_BACKGROUND_NO_REPEAT_Y;
            continue;
        }
        if (token_length == 8 && memcmp(token, "repeat-x", 8) == 0) {
            style->background_size_flags |= STYLE_BACKGROUND_NO_REPEAT_Y;
            continue;
        }
        if (token_length == 8 && memcmp(token, "repeat-y", 8) == 0) {
            style->background_size_flags |= STYLE_BACKGROUND_NO_REPEAT_X;
            continue;
        }
        if (background_position_keyword(token, token_length)
            || background_length_token(token, token_length)) {
            if (position_length != 0
                && position_length + 1 < sizeof(position)) {
                position[position_length++] = ' ';
            }
            if (position_length + token_length < sizeof(position)) {
                memcpy(position + position_length, token, token_length);
                position_length += token_length;
            }
        }
    }
    if (position_length != 0) {
        position[position_length] = '\0';
        (void) style_parse_background_position(sheet, position,
                                               position_length, style);
    }
    if (retained_chosen_layer < stack.background_count
        && retained_chosen_layer < STYLE_PAINT_LAYER_LIMIT) {
        StylePaintLayer *paint = &stack.backgrounds[retained_chosen_layer];
        paint->width = style->background_width;
        paint->height = style->background_height;
        paint->position_x = style->background_position_x;
        paint->position_y = style->background_position_y;
        paint->fit = style->background_fit;
        paint->flags = style->background_size_flags;
    }
    bool retained = false;
    return style_apply_paint_stack(sheet, style, &stack, &retained)
        && retained;
}

uint64_t style_parse_box(const Stylesheet *sheet, const char *text,
                          size_t length, StyleEdges *edges, bool padding)
{
    char resolved[STYLE_MATH_SOURCE_CAPACITY];
    if (!style_resolve_value(
            sheet, text, length, resolved, sizeof(resolved), 0)) {
        return 0;
    }
    text = resolved;
    length = strlen(resolved);
    int values[4] = {0};
    size_t count = 0;
    for (size_t at = 0; at < length && count < 4;) {
        while (at < length && isspace((unsigned char) text[at])) at++;
        size_t end = at;
        int parentheses = 0;
        while (end < length) {
            if (text[end] == '(') parentheses++;
            else if (text[end] == ')' && parentheses > 0) parentheses--;
            if (parentheses == 0 && isspace((unsigned char) text[end])) break;
            end++;
        }
        if (end > at) values[count++] = style_parse_length(sheet, text + at, end - at, 0, NULL);
        at = end;
    }
    if (count == 0) return 0;
    edges->top = values[0];
    edges->right = count > 1 ? values[1] : values[0];
    edges->bottom = count > 2 ? values[2] : values[0];
    edges->left = count > 3 ? values[3] : edges->right;
    return padding ? S_PADDING_ALL : S_MARGIN_ALL;
}

static bool parse_style_length_components(Stylesheet *sheet,
                                          const char *text, size_t length,
                                          StyleLength *values,
                                          size_t capacity, size_t *count)
{
    if (sheet == NULL || text == NULL || values == NULL || capacity == 0
        || count == NULL) return false;
    char resolved[STYLE_MATH_SOURCE_CAPACITY];
    if (!style_resolve_value(sheet, text, length, resolved, sizeof(resolved), 0)) {
        return false;
    }
    size_t program_count = sheet->math_program_count;
    size_t instruction_count = sheet->math_instruction_count;
    size_t resolved_length = strlen(resolved);
    size_t used = 0;
    for (size_t at = 0; at < resolved_length;) {
        while (at < resolved_length
               && isspace((unsigned char) resolved[at])) at++;
        if (at == resolved_length) break;
        if (used == capacity) {
            style_math_restore(sheet, program_count, instruction_count);
            return false;
        }
        size_t end = at;
        unsigned parentheses = 0;
        while (end < resolved_length) {
            if (resolved[end] == '(') {
                parentheses++;
                if (parentheses > STYLE_MATH_NESTING_LIMIT + 1u) break;
            } else if (resolved[end] == ')') {
                if (parentheses == 0) break;
                parentheses--;
            }
            if (parentheses == 0
                && isspace((unsigned char) resolved[end])) break;
            end++;
        }
        if (end == at || parentheses != 0
            || !parse_style_length(sheet, resolved + at, end - at,
                                   &values[used], NULL)) {
            style_math_restore(sheet, program_count, instruction_count);
            return false;
        }
        used++;
        at = end;
    }
    if (used == 0) {
        style_math_restore(sheet, program_count, instruction_count);
        return false;
    }
    *count = used;
    return true;
}

uint64_t style_parse_padding_box(Stylesheet *sheet, const char *text,
                                  size_t length, StyleEdges *edges)
{
    StyleLength values[4] = {0};
    size_t count = 0;
    if (!parse_style_length_components(sheet, text, length, values, 4,
                                       &count)) return 0;
    edges->top = values[0];
    edges->right = count > 1 ? values[1] : values[0];
    edges->bottom = count > 2 ? values[2] : values[0];
    edges->left = count > 3 ? values[3] : edges->right;
    return S_PADDING_ALL;
}

bool style_parse_padding_pair(Stylesheet *sheet, const char *text,
                               size_t length, int *first, int *second)
{
    StyleLength values[2] = {0};
    size_t count = 0;
    if (!parse_style_length_components(sheet, text, length, values, 2,
                                       &count)) return false;
    *first = values[0];
    *second = count > 1 ? values[1] : values[0];
    return true;
}

bool style_length_is_auto(const Stylesheet *sheet, const char *text,
                           size_t length)
{
    char resolved[32];
    return style_resolve_value(sheet, text, length, resolved, sizeof(resolved), 0)
           && strcmp(resolved, "auto") == 0;
}

bool style_noninherited_length_is_auto(const Stylesheet *sheet,
                                        const char *text, size_t length)
{
    char resolved[32];
    if (!style_resolve_value(sheet, text, length, resolved, sizeof(resolved), 0)) {
        return false;
    }
    return strcmp(resolved, "auto") == 0
           || strcmp(resolved, "initial") == 0
           || strcmp(resolved, "unset") == 0
           || strcmp(resolved, "revert") == 0
           || strcmp(resolved, "revert-layer") == 0
           /* Until declarations retain a CSS-wide-keyword bit, treating an
              inherited dimension as auto is safer than manufacturing a
              definite zero-sized box.  It is also exact when the parent's
              computed dimension is auto, the overwhelmingly common case. */
           || strcmp(resolved, "inherit") == 0;
}

uint64_t style_parse_margin_box(const Stylesheet *sheet, const char *text,
                                 size_t length, ComputedStyle *style)
{
    char resolved[STYLE_MATH_SOURCE_CAPACITY];
    if (!style_resolve_value(
            sheet, text, length, resolved, sizeof(resolved), 0)) {
        return 0;
    }
    text = resolved;
    length = strlen(resolved);
    int values[4] = {0};
    bool automatic[4] = {false};
    bool percentages[4] = {false};
    size_t count = 0;
    for (size_t at = 0; at < length && count < 4;) {
        while (at < length && isspace((unsigned char) text[at])) at++;
        size_t end = at;
        int parentheses = 0;
        while (end < length) {
            if (text[end] == '(') parentheses++;
            else if (text[end] == ')' && parentheses > 0) parentheses--;
            if (parentheses == 0 && isspace((unsigned char) text[end])) break;
            end++;
        }
        if (end > at) {
            automatic[count] = style_length_is_auto(sheet, text + at, end - at);
            values[count] = automatic[count] ? 0 : style_parse_length(
                sheet, text + at, end - at, 0, &percentages[count]);
            count++;
        }
        at = end;
    }
    if (count == 0) return 0;
    style->margin.top = values[0];
    style->margin.right = count > 1 ? values[1] : values[0];
    style->margin.bottom = count > 2 ? values[2] : values[0];
    style->margin.left = count > 3 ? values[3] : style->margin.right;
    style->margin_top_auto = automatic[0];
    style->margin_right_auto = count > 1 ? automatic[1] : automatic[0];
    style->margin_bottom_auto = count > 2 ? automatic[2] : automatic[0];
    style->margin_left_auto = count > 3 ? automatic[3]
                              : style->margin_right_auto;
    style->margin_top_percent = percentages[0];
    style->margin_right_percent = count > 1
        ? percentages[1] : percentages[0];
    style->margin_bottom_percent = count > 2
        ? percentages[2] : percentages[0];
    style->margin_left_percent = count > 3
        ? percentages[3] : style->margin_right_percent;
    return S_MARGIN_ALL;
}

static void style_border_raw_color(
    const Stylesheet *sheet, const ComputedStyle *style,
    StyleBorderSide side, uint32_t *color, uint8_t *alpha)
{
    uint8_t one_based = computed_style_border_color_set(style);
    if (one_based != 0 && sheet != NULL
        && one_based <= sheet->border_color_set_count) {
        const StyleBorderColors *set =
            &sheet->border_color_sets[one_based - 1u];
        *color = set->colors[side];
        *alpha = set->alphas[side];
    } else {
        *color = style->border_color;
        *alpha = style->border_alpha;
    }
}

static uint8_t style_intern_border_colors(
    Stylesheet *sheet, const StyleBorderColors *colors)
{
    if (sheet == NULL || sheet->budget == NULL || colors == NULL) return 0;
    for (size_t i = 0; i < sheet->border_color_set_count; i++) {
        if (memcmp(&sheet->border_color_sets[i], colors, sizeof(*colors))
            == 0) return (uint8_t) (i + 1u);
    }
    if (sheet->border_color_set_count >= UINT8_MAX) return 0;
    if (sheet->border_color_set_count == sheet->border_color_set_capacity) {
        size_t capacity = sheet->border_color_set_capacity == 0
            ? 8u : sheet->border_color_set_capacity * 2u;
        if (capacity > UINT8_MAX) capacity = UINT8_MAX;
        StyleBorderColors *sets = budget_realloc(
            sheet->budget, sheet->border_color_sets,
            capacity * sizeof(*sets));
        if (sets == NULL) return 0;
        sheet->border_color_sets = sets;
        sheet->border_color_set_capacity = capacity;
    }
    size_t index = sheet->border_color_set_count++;
    sheet->border_color_sets[index] = *colors;
    return (uint8_t) (index + 1u);
}

bool style_set_border_color(
    Stylesheet *sheet, ComputedStyle *style, StyleBorderSide side,
    uint32_t color, uint8_t alpha)
{
    if (sheet == NULL || style == NULL || side >= STYLE_BORDER_SIDE_COUNT) {
        return false;
    }
    StyleBorderColors colors = {0};
    for (unsigned at = 0; at < STYLE_BORDER_SIDE_COUNT; at++) {
        style_border_raw_color(
            sheet, style, (StyleBorderSide) at,
            &colors.colors[at], &colors.alphas[at]);
    }
    colors.colors[side] = color;
    colors.alphas[side] = alpha;
    bool uniform = true;
    for (unsigned at = 1; at < STYLE_BORDER_SIDE_COUNT; at++) {
        if (colors.colors[at] != colors.colors[0]
            || colors.alphas[at] != colors.alphas[0]) {
            uniform = false;
            break;
        }
    }
    if (uniform) {
        style->border_color = colors.colors[0];
        style->border_alpha = colors.alphas[0];
        style->border_color_set = 0;
        return true;
    }
    uint8_t one_based = style_intern_border_colors(sheet, &colors);
    if (one_based == 0) return false;
    style->border_color_set = one_based;
    return true;
}

bool style_copy_border_color(
    Stylesheet *sheet, ComputedStyle *target, const ComputedStyle *source,
    StyleBorderSide side)
{
    if (sheet == NULL || target == NULL || source == NULL
        || side >= STYLE_BORDER_SIDE_COUNT) return false;
    uint32_t color = 0;
    uint8_t alpha = 255;
    style_border_raw_color(sheet, source, side, &color, &alpha);
    return style_set_border_color(sheet, target, side, color, alpha);
}

uint32_t stylesheet_border_color(
    const Stylesheet *sheet, const ComputedStyle *style,
    StyleBorderSide side, uint8_t *alpha)
{
    uint32_t color = 0;
    uint8_t resolved_alpha = 255;
    if (style != NULL && side < STYLE_BORDER_SIDE_COUNT) {
        style_border_raw_color(
            sheet, style, side, &color, &resolved_alpha);
        if (color == UINT32_MAX) {
            color = style->color;
            resolved_alpha = style->color_alpha;
        }
    }
    if (alpha != NULL) *alpha = resolved_alpha;
    return color;
}

uint64_t style_parse_border(
    const Stylesheet *sheet, const char *text, size_t length,
    int *width, uint32_t *color, uint8_t *alpha, unsigned *line_style,
    uint64_t edge_mask)
{
    char resolved[STYLE_MATH_SOURCE_CAPACITY];
    if (!style_resolve_value(
            sheet, text, length, resolved, sizeof(resolved), 0)) {
        return 0;
    }
    text = resolved;
    length = strlen(resolved);
    int parsed_width = -1;
    bool has_color = false;
    unsigned parsed_style = STYLE_BORDER_NONE;
    /* CSS drops a declaration it cannot parse, leaving the previous cascaded
       value in force.  Track whether ANY component of this value was
       understood so a wholly unparseable `border` value (an unresolvable
       var(), say) reports "no edges touched" instead of silently rewriting
       the border to the 1px default. */
    bool recognized = false;
    static const char *const border_styles[] = {
        "solid", "dotted", "dashed", "double", "groove", "ridge",
        "inset", "outset", "thin", "medium", "thick"
    };
    for (size_t at = 0; at < length;) {
        while (at < length && isspace((unsigned char) text[at])) at++;
        size_t end = at;
        int parentheses = 0;
        while (end < length) {
            if (text[end] == '(') parentheses++;
            else if (text[end] == ')' && parentheses > 0) parentheses--;
            if (parentheses == 0 && isspace((unsigned char) text[end])) break;
            end++;
        }
        if (end > at) {
            uint32_t candidate;
            uint8_t candidate_alpha = 255;
            if (span_equal(text + at, end - at, "none")
                || span_equal(text + at, end - at, "hidden")) {
                parsed_style = STYLE_BORDER_NONE;
                recognized = true;
            } else if (style_parse_color_with_alpha(sheet, text + at, end - at,
                                       &candidate, &candidate_alpha)) {
                *color = candidate;
                *alpha = candidate_alpha;
                has_color = true;
                recognized = true;
            } else {
                int candidate_width = style_parse_length(sheet, text + at,
                                                   end - at, -1, NULL);
                if (candidate_width >= 0) {
                    parsed_width = candidate_width;
                    recognized = true;
                } else {
                    for (size_t i = 0; i < sizeof(border_styles)
                                             / sizeof(border_styles[0]); i++) {
                        if (span_equal(text + at, end - at,
                                       border_styles[i])) {
                            parsed_style = i == 1 ? STYLE_BORDER_DOTTED
                                : (i == 2 ? STYLE_BORDER_DASHED
                                          : STYLE_BORDER_SOLID);
                            recognized = true;
                            break;
                        }
                    }
                }
            }
        }
        at = end;
    }
    if (!recognized) return 0;
    if (parsed_style == STYLE_BORDER_NONE) parsed_width = 0;
    else if (parsed_width < 0) parsed_width = 1;
    *width = parsed_width;
    *line_style = parsed_style;
    if (!has_color) {
        *color = UINT32_MAX;
        *alpha = 255;
    }
    return edge_mask;
}

void style_parse_pair(const Stylesheet *sheet, const char *text,
                       size_t length, int *first, int *second)
{
    int values[2] = {0, 0};
    size_t count = 0;
    for (size_t at = 0; at < length && count < 2;) {
        while (at < length && isspace((unsigned char) text[at])) at++;
        size_t end = at;
        int parentheses = 0;
        while (end < length) {
            if (text[end] == '(') parentheses++;
            else if (text[end] == ')' && parentheses > 0) parentheses--;
            if (parentheses == 0 && isspace((unsigned char) text[end])) break;
            end++;
        }
        if (end > at) {
            values[count++] = style_parse_length(sheet, text + at, end - at, 0, NULL);
        }
        at = end;
    }
    *first = values[0];
    *second = count > 1 ? values[1] : values[0];
}

bool style_parse_pair_with_auto(const Stylesheet *sheet, const char *text,
                                 size_t length, int values[2],
                                 bool automatic[2], bool percentages[2])
{
    size_t count = 0;
    for (size_t at = 0; at < length && count < 2;) {
        while (at < length && isspace((unsigned char) text[at])) at++;
        size_t end = at;
        int parentheses = 0;
        while (end < length) {
            if (text[end] == '(') parentheses++;
            else if (text[end] == ')' && parentheses > 0) parentheses--;
            if (parentheses == 0 && isspace((unsigned char) text[end])) break;
            end++;
        }
        if (end > at) {
            automatic[count] = style_length_is_auto(sheet, text + at, end - at);
            bool percent = false;
            values[count] = automatic[count] ? 0 : style_parse_length(
                sheet, text + at, end - at, 0, &percent);
            if (percentages != NULL) percentages[count] = percent;
            count++;
        }
        at = end;
    }
    if (count == 0) return false;
    if (count == 1) {
        values[1] = values[0];
        automatic[1] = automatic[0];
        if (percentages != NULL) percentages[1] = percentages[0];
    }
    return true;
}

int style_parse_line_height(const Stylesheet *sheet, const char *text,
                             size_t length)
{
    char value[128];
    if (!style_resolve_value(sheet, text, length, value, sizeof(value), 0)) return 0;
    char *end = NULL;
    double number = strtod(value, &end);
    if (end != value) {
        while (isspace((unsigned char) *end)) end++;
        if (*end == '\0' && number > 0.0 && number <= 10.0) {
            return -(int) (number * 1000.0 + 0.5);
        }
        /* Percentages and em line heights compute relative to the element's
           own font size, just like a unitless multiplier.  Keep them in the
           same bounded thousandth representation until resolve_font_size()
           has produced the used 26.6 font size.  Treating `125%` as the
           generic length value 125 made a 32px heading use 125px lines. */
        if (number > 0.0 && number <= 1000.0
            && end[0] == '%' && end[1] == '\0') {
            return -(int) (number * 10.0 + 0.5);
        }
        if (number > 0.0 && number <= 10.0
            && (end[0] == 'e' || end[0] == 'E')
            && (end[1] == 'm' || end[1] == 'M') && end[2] == '\0') {
            return -(int) (number * 1000.0 + 0.5);
        }
    }
    int thousandths = 0;
    if (style_math_resolve_number_thousandths(
            sheet, value, strlen(value), &thousandths)) {
        return -thousandths;
    }
    return style_parse_length(sheet, text, length, 0, NULL);
}

bool style_parse_text_decoration_underline(const Stylesheet *sheet,
                                            const char *text, size_t length,
                                            bool *underline)
{
    char resolved[128];
    if (underline == NULL
        || !style_resolve_value(sheet, text, length, resolved,
                          sizeof(resolved), 0)) return false;
    bool saw_underline = false;
    bool saw_none = false;
    size_t value_length = strlen(resolved);
    for (size_t at = 0; at < value_length;) {
        while (at < value_length
               && isspace((unsigned char) resolved[at])) at++;
        size_t end = at;
        while (end < value_length
               && !isspace((unsigned char) resolved[end])) end++;
        if (end == at) break;
        if (span_case_equal(resolved + at, end - at, "underline")) {
            saw_underline = true;
        } else if (span_case_equal(resolved + at, end - at, "none")
                   || span_case_equal(resolved + at, end - at, "initial")
                   || span_case_equal(resolved + at, end - at, "unset")
                   || span_case_equal(resolved + at, end - at, "revert")
                   || span_case_equal(resolved + at, end - at,
                                      "revert-layer")) {
            saw_none = true;
        }
        at = end;
    }
    if (!saw_underline && !saw_none) return false;
    *underline = saw_underline;
    return true;
}

/* Draw commands retain five spare text-only bits.  Keep this first
   implementation deliberately exact and bounded: auto, unitless zero, and
   absolute pixel lengths rounded into the representable -15..15px range. */
bool style_parse_text_underline_offset(const Stylesheet *sheet,
                                        const char *text, size_t length,
                                        unsigned *offset_code)
{
    char resolved[128];
    if (offset_code == NULL
        || !style_resolve_value(sheet, text, length, resolved,
                          sizeof(resolved), 0)) return false;
    const char *value = resolved;
    size_t value_length = strlen(resolved);
    trim(&value, &value_length);
    if (span_case_equal(value, value_length, "auto")
        || span_case_equal(value, value_length, "initial")) {
        *offset_code = 0;
        return true;
    }
    if (value_length == 0) return false;
    char *end = NULL;
    double number = strtod(value, &end);
    if (end == value || !isfinite(number)) return false;
    const char *value_end = value + value_length;
    bool unitless_zero = end == value_end && number == 0.0;
    bool pixels = end + 2 == value_end
                  && tolower((unsigned char) end[0]) == 'p'
                  && tolower((unsigned char) end[1]) == 'x';
    if (!unitless_zero && !pixels) return false;
    int rounded = (int) (number < 0.0 ? number - 0.5 : number + 0.5);
    if (rounded < -15 || rounded > 15) return false;
    *offset_code = (unsigned) (rounded + 16);
    return true;
}

int style_font_size_from_fixed(int fixed, uint8_t *fraction)
{
    if (fixed < 0) fixed = 0;
    *fraction = (uint8_t) (fixed & 63);
    return fixed / 64;
}

static bool font_size_pixels_to_fixed(double pixels, int *fixed)
{
    if (fixed == NULL || !isfinite(pixels) || pixels < 0.0
        || pixels > (double) INT_MAX / 64.0) return false;
    *fixed = (int) floor(pixels * 64.0 + 0.5);
    return true;
}

int style_parse_font_size(const Stylesheet *sheet, const char *text,
                           size_t length, uint8_t *unit,
                           uint8_t *fraction)
{
    char value[128];
    *unit = FONT_SIZE_UNIT_ABSOLUTE;
    *fraction = 0;
    if (!style_resolve_value(sheet, text, length, value, sizeof(value), 0)) return 14;
    const char *math = value;
    size_t math_length = strlen(value);
    trim(&math, &math_length);
    const char *opening = memchr(math, '(', math_length);
    if (opening != NULL && math_length != 0
        && math[math_length - 1] == ')') {
        size_t name_length = (size_t) (opening - math);
        bool function = style_math_identifier_equal(math, name_length,
                                                    "min")
            || style_math_identifier_equal(math, name_length, "max")
            || style_math_identifier_equal(math, name_length, "clamp")
            || style_math_identifier_equal(math, name_length, "calc");
        if (function) {
            const StyleResolveScratch *scratch =
                sheet != NULL ? sheet->resolve_scratch : NULL;
            int inherited = scratch != NULL && scratch->font_resolution_active
                            ? scratch->font_resolution_inherited : 16;
            int root_basis = scratch != NULL && scratch->font_resolution_active
                             ? scratch->font_resolution_root : 16;
            StyleMathParser parser;
            StyleMathCandidate candidate;
            int root = -1;
            int parsed = 14;
            if (style_math_candidate(sheet, math, math_length, &parser,
                                     &candidate, &root, inherited, root_basis,
                                     (inherited + 1) / 2)
                && style_math_resolve_instructions(
                    candidate.instructions, candidate.count,
                    candidate.stack_depth, inherited, &parsed)) {
                return parsed;
            }
            return 14;
        }
    }
    char *end = NULL;
    double number = strtod(value, &end);
    if (end == value || !isfinite(number) || number < 0.0) return 14;
    while (isspace((unsigned char) *end)) end++;
    if (strcmp(end, "%") == 0) {
        *unit = FONT_SIZE_UNIT_PERCENT;
        return (int) (number * 10.0 + 0.5);
    }
    if (strcmp(end, "em") == 0) {
        *unit = FONT_SIZE_UNIT_EM;
        return (int) (number * 1000.0 + 0.5);
    }
    if (strcmp(end, "rem") == 0) {
        *unit = FONT_SIZE_UNIT_REM;
        return (int) (number * 1000.0 + 0.5);
    }
    if (strcmp(end, "pt") == 0) number *= 4.0 / 3.0;
    else if (strcmp(end, "pc") == 0) number *= 16.0;
    else if (strcmp(end, "in") == 0) number *= 96.0;
    else if (strcmp(end, "cm") == 0) number *= 96.0 / 2.54;
    else if (strcmp(end, "mm") == 0) number *= 96.0 / 25.4;
    else if (strcmp(end, "q") == 0) number *= 96.0 / 101.6;
    int fixed = 0;
    if (!font_size_pixels_to_fixed(number, &fixed)) return 14;
    return style_font_size_from_fixed(fixed, fraction);
}

bool style_font_span_equal(const char *text, size_t length,
                            const char *wanted)
{
    size_t wanted_length = strlen(wanted);
    if (length != wanted_length) return false;
    for (size_t i = 0; i < length; i++) {
        if (tolower((unsigned char) text[i])
            != tolower((unsigned char) wanted[i])) return false;
    }
    return true;
}

/* Return one whitespace-delimited font component.  A top-level slash is a
   component of its own; quoted strings and functions remain intact. */
static bool next_font_component(const char **cursor, const char *limit,
                                const char **component, size_t *length)
{
    const char *at = *cursor;
    while (at < limit && isspace((unsigned char) *at)) at++;
    if (at == limit) {
        *cursor = at;
        *component = NULL;
        *length = 0;
        return true;
    }
    if (*at == '/') {
        *cursor = at + 1;
        *component = at;
        *length = 1;
        return true;
    }
    const char *start = at;
    char quote = 0;
    unsigned parentheses = 0;
    while (at < limit) {
        unsigned char character = (unsigned char) *at;
        if (quote != 0) {
            if (character == '\\' && at + 1 < limit) {
                at += 2;
                continue;
            }
            if (character == (unsigned char) quote) quote = 0;
            at++;
            continue;
        }
        if (character == '\'' || character == '"') {
            quote = (char) character;
        } else if (character == '(') {
            parentheses++;
        } else if (character == ')') {
            if (parentheses == 0) return false;
            parentheses--;
        } else if (parentheses == 0
                   && (isspace(character) || character == '/')) {
            break;
        }
        at++;
    }
    if (quote != 0 || parentheses != 0 || at == start) return false;
    *cursor = at;
    *component = start;
    *length = (size_t) (at - start);
    return true;
}

static bool parse_font_number(const char *text, size_t length,
                              double *number, const char **suffix)
{
    if (length == 0 || length >= 96) return false;
    char value[96];
    memcpy(value, text, length);
    value[length] = '\0';
    char *end = NULL;
    double parsed = strtod(value, &end);
    if (end == value || !isfinite(parsed)) return false;
    size_t offset = (size_t) (end - value);
    *number = parsed;
    *suffix = text + offset;
    return true;
}

static bool font_suffix_equal(const char *suffix, const char *end,
                              const char *wanted)
{
    return style_font_span_equal(suffix, (size_t) (end - suffix), wanted);
}

static bool parse_font_size_component(const Stylesheet *sheet,
                                      const char *text, size_t length,
                                      int *size, uint8_t *unit,
                                      uint8_t *fraction)
{
    *unit = FONT_SIZE_UNIT_ABSOLUTE;
    *fraction = 0;
    static const struct {
        const char *name;
        int pixels;
    } keywords[] = {
        {"xx-small", 9}, {"x-small", 10}, {"small", 13},
        {"medium", 16}, {"large", 18}, {"x-large", 24},
        {"xx-large", 32}, {"xxx-large", 48}
    };
    for (size_t i = 0; i < sizeof(keywords) / sizeof(keywords[0]); i++) {
        if (style_font_span_equal(text, length, keywords[i].name)) {
            *size = keywords[i].pixels;
            return true;
        }
    }
    if (style_font_span_equal(text, length, "larger")
        || style_font_span_equal(text, length, "smaller")) {
        *size = style_font_span_equal(text, length, "larger") ? 1200 : 800;
        *unit = FONT_SIZE_UNIT_PERCENT;
        return true;
    }
    if ((length > 5 && text[length - 1] == ')')
        && (style_font_span_equal(text, 4, "min(")
            || style_font_span_equal(text, 4, "max(")
            || style_font_span_equal(text, 5, "calc(")
            || style_font_span_equal(text, 6, "clamp("))) {
        int parsed = style_parse_font_size(sheet, text, length, unit, fraction);
        if (parsed < 0) return false;
        *size = parsed;
        return true;
    }
    double number = 0.0;
    const char *suffix = NULL;
    if (!parse_font_number(text, length, &number, &suffix) || number < 0.0) {
        return false;
    }
    const char *end = text + length;
    if (suffix == end) {
        if (number != 0.0) return false;
        *size = 0;
        return true;
    }
    if (font_suffix_equal(suffix, end, "%")) {
        if (number > 10000.0) return false;
        *unit = FONT_SIZE_UNIT_PERCENT;
        *size = (int) (number * 10.0 + 0.5);
        return true;
    }
    if (font_suffix_equal(suffix, end, "em")) {
        if (number > 100.0) return false;
        *unit = FONT_SIZE_UNIT_EM;
        *size = (int) (number * 1000.0 + 0.5);
        return true;
    }
    if (font_suffix_equal(suffix, end, "rem")) {
        if (number > 100.0) return false;
        *unit = FONT_SIZE_UNIT_REM;
        *size = (int) (number * 1000.0 + 0.5);
        return true;
    }
    static const char *fixed_units[] = {
        "px", "vw", "vh", "vi", "vb", "dvw", "dvh", "dvi", "dvb",
        "svw", "svh", "svi", "svb", "lvw", "lvh", "lvi", "lvb"
    };
    for (size_t i = 0; i < sizeof(fixed_units) / sizeof(fixed_units[0]); i++) {
        if (font_suffix_equal(suffix, end, fixed_units[i])) {
            if (font_suffix_equal(suffix, end, "px")) {
                int fixed = 0;
                if (!font_size_pixels_to_fixed(number, &fixed)) return false;
                *size = style_font_size_from_fixed(fixed, fraction);
                return true;
            }
            int parsed = style_parse_length(sheet, text, length, INT_MIN, NULL);
            if (parsed == INT_MIN || parsed < 0) return false;
            *size = parsed;
            return true;
        }
    }
    static const struct {
        const char *name;
        double pixels;
    } physical_units[] = {
        {"pt", 4.0 / 3.0}, {"pc", 16.0}, {"in", 96.0},
        {"cm", 96.0 / 2.54}, {"mm", 96.0 / 25.4},
        {"q", 96.0 / 101.6}
    };
    for (size_t i = 0;
         i < sizeof(physical_units) / sizeof(physical_units[0]); i++) {
        if (!font_suffix_equal(suffix, end, physical_units[i].name)) continue;
        double pixels = number * physical_units[i].pixels;
        int fixed = 0;
        if (!font_size_pixels_to_fixed(pixels, &fixed)) return false;
        *size = style_font_size_from_fixed(fixed, fraction);
        return true;
    }
    return false;
}

static bool parse_font_line_height_component(const Stylesheet *sheet,
                                             const char *text, size_t length,
                                             int *line_height)
{
    if (style_font_span_equal(text, length, "normal")) {
        *line_height = 0;
        return true;
    }
    double number = 0.0;
    const char *suffix = NULL;
    if (!parse_font_number(text, length, &number, &suffix) || number < 0.0) {
        return false;
    }
    const char *end = text + length;
    if (suffix == end) {
        if (number > 100.0) return false;
        *line_height = number == 0.0
                       ? 0 : -(int) (number * 1000.0 + 0.5);
        return true;
    }
    if (font_suffix_equal(suffix, end, "%")) {
        if (number > 10000.0) return false;
        *line_height = number == 0.0
                       ? 0 : -(int) (number * 10.0 + 0.5);
        return true;
    }
    if (font_suffix_equal(suffix, end, "em")) {
        if (number > 100.0) return false;
        *line_height = number == 0.0
                       ? 0 : -(int) (number * 1000.0 + 0.5);
        return true;
    }
    static const char *fixed_units[] = {
        "px", "rem", "vw", "vh", "vi", "vb", "dvw", "dvh", "dvi",
        "dvb", "svw", "svh", "svi", "svb", "lvw", "lvh", "lvi",
        "lvb"
    };
    for (size_t i = 0; i < sizeof(fixed_units) / sizeof(fixed_units[0]); i++) {
        if (font_suffix_equal(suffix, end, fixed_units[i])) {
            int parsed = style_parse_line_height(sheet, text, length);
            if (parsed < 0) return false;
            *line_height = parsed;
            return true;
        }
    }
    return false;
}

bool style_parse_font_weight_component(const char *text, size_t length,
                                        uint16_t *weight)
{
    if (weight == NULL) return false;
    if (style_font_span_equal(text, length, "normal")) {
        *weight = 400;
        return true;
    }
    if (style_font_span_equal(text, length, "bold")
        || style_font_span_equal(text, length, "bolder")) {
        *weight = 700;
        return true;
    }
    if (style_font_span_equal(text, length, "lighter")) {
        *weight = 300;
        return true;
    }
    double number = 0.0;
    const char *suffix = NULL;
    if (!parse_font_number(text, length, &number, &suffix)
        || suffix != text + length || number < 1.0 || number > 1000.0) {
        return false;
    }
    *weight = (uint16_t) (number + 0.5);
    return true;
}

static bool font_stretch_component(const char *text, size_t length)
{
    static const char *values[] = {
        "ultra-condensed", "extra-condensed", "condensed",
        "semi-condensed", "semi-expanded", "expanded",
        "extra-expanded", "ultra-expanded"
    };
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        if (style_font_span_equal(text, length, values[i])) return true;
    }
    return false;
}

static bool font_angle_component(const char *text, size_t length)
{
    double number = 0.0;
    const char *suffix = NULL;
    if (!parse_font_number(text, length, &number, &suffix)) return false;
    const char *end = text + length;
    return font_suffix_equal(suffix, end, "deg")
        || font_suffix_equal(suffix, end, "grad")
        || font_suffix_equal(suffix, end, "rad")
        || font_suffix_equal(suffix, end, "turn");
}

static bool select_platform_font_family(const char *text, size_t length,
                                        FontFamily *family)
{
    if (text == NULL || family == NULL) return false;
    trim(&text, &length);
    if (style_font_span_equal(text, length, "monospace")
        || style_font_span_equal(text, length, "ui-monospace")) {
        *family = FONT_MONOSPACE;
        return true;
    }
    if (style_font_span_equal(text, length, "serif")
        || style_font_span_equal(text, length, "ui-serif")) {
        *family = FONT_SERIF;
        return true;
    }
    if (style_font_span_equal(text, length, "arial")
        || style_font_span_equal(text, length, "helvetica")
        || style_font_span_equal(text, length, "helvetica neue")
        || style_font_span_equal(text, length, "-apple-system")
        || style_font_span_equal(text, length, "blinkmacsystemfont")
        || style_font_span_equal(text, length, "liberation sans")
        || style_font_span_equal(text, length, "nimbus sans")
        || style_font_span_equal(text, length, "nimbus sans l")) {
        *family = FONT_METRIC_SANS;
        return true;
    }
    if (style_font_span_equal(text, length, "verdana")) {
        *family = FONT_HUMANIST_SANS;
        return true;
    }
    if (style_font_span_equal(text, length, "sans-serif")
        || style_font_span_equal(text, length, "ui-sans-serif")
        || style_font_span_equal(text, length, "system-ui")) {
        *family = FONT_SANS;
        return true;
    }
    return false;
}

bool style_css_hex_digit(unsigned char character, unsigned *value)
{
    if (character >= '0' && character <= '9') {
        *value = character - '0';
        return true;
    }
    character = (unsigned char) tolower(character);
    if (character >= 'a' && character <= 'f') {
        *value = character - 'a' + 10u;
        return true;
    }
    return false;
}

bool style_font_family_append_codepoint(char *output, size_t capacity,
                                         size_t *written,
                                         unsigned codepoint)
{
    unsigned char bytes[4];
    size_t count = 0;
    if (codepoint <= 0x7fu) {
        bytes[count++] = (unsigned char) codepoint;
    } else if (codepoint <= 0x7ffu) {
        bytes[count++] = (unsigned char) (0xc0u | (codepoint >> 6));
        bytes[count++] = (unsigned char) (0x80u | (codepoint & 0x3fu));
    } else if (codepoint <= 0xffffu) {
        bytes[count++] = (unsigned char) (0xe0u | (codepoint >> 12));
        bytes[count++] = (unsigned char) (0x80u
                                          | ((codepoint >> 6) & 0x3fu));
        bytes[count++] = (unsigned char) (0x80u | (codepoint & 0x3fu));
    } else {
        bytes[count++] = (unsigned char) (0xf0u | (codepoint >> 18));
        bytes[count++] = (unsigned char) (0x80u
                                          | ((codepoint >> 12) & 0x3fu));
        bytes[count++] = (unsigned char) (0x80u
                                          | ((codepoint >> 6) & 0x3fu));
        bytes[count++] = (unsigned char) (0x80u | (codepoint & 0x3fu));
    }
    if (count > capacity - 1u - *written) return false;
    memcpy(output + *written, bytes, count);
    *written += count;
    return true;
}

bool style_canonical_font_family_name(const char *text, size_t length,
                                       char output[
                                           STYLE_WEB_FONT_NAME_CAPACITY])
{
    if (text == NULL || output == NULL) return false;
    trim(&text, &length);
    if (length >= 2 && (text[0] == '\'' || text[0] == '"')
        && text[length - 1] == text[0]) {
        text++;
        length -= 2;
    }
    size_t written = 0;
    bool pending_space = false;
    for (size_t at = 0; at < length;) {
        unsigned char character = (unsigned char) text[at++];
        if (character == '\\' && at < length) {
            unsigned digit = 0;
            if (style_css_hex_digit((unsigned char) text[at], &digit)) {
                unsigned codepoint = 0;
                size_t digits = 0;
                while (at < length && digits < 6
                       && style_css_hex_digit((unsigned char) text[at], &digit)) {
                    codepoint = codepoint * 16u + digit;
                    at++;
                    digits++;
                }
                if (at < length
                    && isspace((unsigned char) text[at])) at++;
                if (codepoint == 0 || codepoint > 0x10ffffu
                    || (codepoint >= 0xd800u && codepoint <= 0xdfffu)) {
                    codepoint = 0xfffdu;
                }
                if (pending_space && written != 0) {
                    if (written + 1 >= STYLE_WEB_FONT_NAME_CAPACITY) {
                        return false;
                    }
                    output[written++] = ' ';
                }
                pending_space = false;
                if (!style_font_family_append_codepoint(
                        output, STYLE_WEB_FONT_NAME_CAPACITY, &written,
                        codepoint)) return false;
                continue;
            }
            character = (unsigned char) text[at++];
        }
        if (isspace(character)) {
            pending_space = true;
            continue;
        }
        if (pending_space && written != 0) {
            if (written + 1 >= STYLE_WEB_FONT_NAME_CAPACITY) return false;
            output[written++] = ' ';
        }
        pending_space = false;
        if (written + 1 >= STYLE_WEB_FONT_NAME_CAPACITY) return false;
        output[written++] = character < 0x80u
                           ? (char) tolower(character) : (char) character;
    }
    if (written == 0) return false;
    output[written] = '\0';
    return true;
}

static bool stylesheet_web_font_slot_named(const Stylesheet *sheet,
                                            const char *name,
                                            size_t name_length,
                                            unsigned *slot)
{
    if (sheet == NULL || sheet->web_fonts == NULL || slot == NULL) {
        return false;
    }
    char canonical[STYLE_WEB_FONT_NAME_CAPACITY];
    if (!style_canonical_font_family_name(name, name_length, canonical)) {
        return false;
    }
    for (size_t i = 0; i < sheet->web_fonts->family_count; i++) {
        if (strcmp(sheet->web_fonts->families[i].name, canonical) == 0) {
            *slot = (unsigned) i;
            return true;
        }
    }
    return false;
}

static void consider_font_family(const Stylesheet *sheet,
                                 const char *name, size_t name_length,
                                 bool *selected, bool *selected_web,
                                 bool *fallback_selected,
                                 unsigned *web_slot, FontFamily *family)
{
    FontFamily platform = FONT_SANS;
    unsigned slot = 0;
    if (!*selected
        && stylesheet_web_font_slot_named(sheet, name, name_length, &slot)) {
        *selected = true;
        *selected_web = true;
        *web_slot = slot;
        return;
    }
    if (!select_platform_font_family(name, name_length, &platform)) return;
    if (!*selected) {
        *selected = true;
        *family = platform;
    } else if (*selected_web && !*fallback_selected) {
        *fallback_selected = true;
        *family = platform;
    }
}

bool style_font_family_component(const Stylesheet *sheet,
                                  const char *text, size_t length,
                                  FontFamily *generic)
{
    const char *value = text;
    trim(&value, &length);
    if (length == 0) return false;
    bool selected = false;
    bool selected_web = false;
    bool fallback_selected = false;
    unsigned web_slot = 0;
    *generic = FONT_SANS;
    for (size_t at = 0; at < length;) {
        while (at < length && isspace((unsigned char) value[at])) at++;
        if (at == length) return false;
        size_t start = at;
        bool quoted = value[at] == '\'' || value[at] == '"';
        if (quoted) {
            char quote = value[at++];
            size_t name_start = at;
            bool closed = false;
            while (at < length) {
                if (value[at] == '\\' && at + 1 < length) {
                    at += 2;
                    continue;
                }
                if (value[at++] == quote) {
                    closed = true;
                    break;
                }
            }
            if (!closed || at - start <= 2) return false;
            consider_font_family(
                sheet, value + name_start, at - name_start - 1,
                &selected, &selected_web, &fallback_selected,
                &web_slot, generic);
            while (at < length && isspace((unsigned char) value[at])) at++;
            if (at < length && value[at] != ',') return false;
        } else {
            while (at < length && value[at] != ',') at++;
            size_t end = at;
            while (end > start
                   && isspace((unsigned char) value[end - 1])) end--;
            if (end == start) return false;
            for (size_t i = start; i < end; i++) {
                unsigned char character = (unsigned char) value[i];
                if (character == '\\' && i + 1 < end) {
                    i++;
                    continue;
                }
                if (!(isalnum(character) || character == '-' || character == '_'
                      || isspace(character) || character >= 0x80)) return false;
            }
            size_t family_length = end - start;
            if (style_font_span_equal(value + start, family_length, "inherit")
                || style_font_span_equal(value + start, family_length, "initial")
                || style_font_span_equal(value + start, family_length, "unset")
                || style_font_span_equal(value + start, family_length, "revert")
                || style_font_span_equal(value + start, family_length,
                                   "revert-layer")) return false;
            consider_font_family(
                sheet, value + start, family_length,
                &selected, &selected_web, &fallback_selected,
                &web_slot, generic);
        }
        if (at == length) break;
        at++;
        size_t remaining = at;
        while (remaining < length
               && isspace((unsigned char) value[remaining])) remaining++;
        if (remaining == length) return false;
    }
    if (selected_web) {
        FontFamily encoded = FONT_SANS;
        if (!font_family_web(web_slot, *generic, &encoded)) return false;
        *generic = encoded;
    }
    return true;
}

bool style_parse_font_shorthand(const Stylesheet *sheet, const char *text,
                                 size_t length, ComputedStyle *font)
{
    char resolved[512];
    if (!style_resolve_value(sheet, text, length, resolved, sizeof(resolved), 0)) {
        return false;
    }
    const char *cursor = resolved;
    const char *limit = resolved + strlen(resolved);
    ComputedStyle parsed = {0};
    parsed.font_family = FONT_SANS;
    parsed.font_weight = 400;
    unsigned categories = 0;
    unsigned normals = 0;
    unsigned oblique_angles = 0;
    bool style_seen = false;
    bool oblique_style = false;
    bool variant_seen = false;
    bool weight_seen = false;
    bool stretch_seen = false;
    for (;;) {
        const char *component = NULL;
        size_t component_length = 0;
        if (!next_font_component(&cursor, limit, &component,
                                 &component_length)
            || component == NULL || component_length == 0
            || (component_length == 1 && component[0] == '/')) return false;
        int size = 0;
        uint8_t unit = FONT_SIZE_UNIT_ABSOLUTE;
        uint8_t fraction = 0;
        if (parse_font_size_component(sheet, component, component_length,
                                      &size, &unit, &fraction)) {
            parsed.font_size = size;
            parsed.font_size_fraction = fraction;
            parsed.font_size_unit = unit;
            break;
        }
        if (style_font_span_equal(component, component_length, "normal")) {
            if (++normals + categories > 4) return false;
            continue;
        }
        if (style_font_span_equal(component, component_length, "italic")
            || style_font_span_equal(component, component_length, "oblique")) {
            if (style_seen) return false;
            style_seen = true;
            oblique_style = style_font_span_equal(component, component_length,
                                             "oblique");
            parsed.font_italic = true;
            categories++;
            if (normals + categories > 4) return false;
            continue;
        }
        if (oblique_style && oblique_angles < 2
            && font_angle_component(component, component_length)) {
            oblique_angles++;
            continue;
        }
        if (style_font_span_equal(component, component_length, "small-caps")) {
            if (variant_seen) return false;
            variant_seen = true;
            categories++;
        } else if (font_stretch_component(component, component_length)) {
            if (stretch_seen) return false;
            stretch_seen = true;
            categories++;
        } else {
            uint16_t weight = 400;
            if (weight_seen
                || !style_parse_font_weight_component(component, component_length,
                                                &weight)) return false;
            weight_seen = true;
            parsed.font_weight = weight;
            parsed.font_bold = weight >= 600;
            categories++;
        }
        if (normals + categories > 4) return false;
    }
    while (cursor < limit && isspace((unsigned char) *cursor)) cursor++;
    if (cursor < limit && *cursor == '/') {
        cursor++;
        const char *line_height = NULL;
        size_t line_height_length = 0;
        if (!next_font_component(&cursor, limit, &line_height,
                                 &line_height_length)
            || line_height == NULL || line_height_length == 0
            || (line_height_length == 1 && line_height[0] == '/')
            || !parse_font_line_height_component(
                   sheet, line_height, line_height_length,
                   &parsed.line_height)) return false;
    }
    while (cursor < limit && isspace((unsigned char) *cursor)) cursor++;
    if (cursor == limit
        || !style_font_family_component(sheet, cursor, (size_t) (limit - cursor),
                                  &parsed.font_family)) return false;
    if (parsed.font_size_unit == FONT_SIZE_UNIT_ABSOLUTE) {
        int fixed = computed_style_font_size_fixed(&parsed);
        if (fixed < 6 * 64) fixed = 6 * 64;
        if (fixed > STYLE_FONT_MAX_PX * 64) {
            fixed = STYLE_FONT_MAX_PX * 64;
        }
        parsed.font_size = style_font_size_from_fixed(
            fixed, &parsed.font_size_fraction);
    }
    int fixed = computed_style_font_size_fixed(&parsed);
    parsed.font_scale = fixed >= 20 * 64 ? 3
                        : (fixed >= 13 * 64 ? 2 : 1);
    *font = parsed;
    return true;
}

bool style_parse_grid_track(const Stylesheet *sheet, const char *text,
                            size_t length, uint8_t *type,
                            unsigned *value, unsigned *minimum)
{
    trim(&text, &length);
    *type = GRID_TRACK_AUTO;
    *value = 0;
    *minimum = 0;
    if (length == 0) return false;
    if (length > 8 && memcmp(text, "minmax(", 7) == 0
        && text[length - 1] == ')') {
        const char *inside = text + 7;
        size_t inside_length = length - 8;
        size_t comma = 0;
        int depth = 0;
        while (comma < inside_length) {
            if (inside[comma] == '(') depth++;
            else if (inside[comma] == ')' && depth > 0) depth--;
            else if (inside[comma] == ',' && depth == 0) break;
            comma++;
        }
        if (comma < inside_length) {
            const char *floor_text = inside;
            size_t floor_length = comma;
            trim(&floor_text, &floor_length);
            bool percent = false;
            int floor = style_parse_length(sheet, floor_text, floor_length, 0,
                                     &percent);
            if (!percent && floor > 0) *minimum = (unsigned) floor;
            uint8_t maximum_type = GRID_TRACK_AUTO;
            unsigned maximum_value = 0;
            unsigned ignored_minimum = 0;
            if (style_parse_grid_track(
                    sheet, inside + comma + 1,
                    inside_length - comma - 1,
                    &maximum_type, &maximum_value,
                    &ignored_minimum)) {
                *type = maximum_type;
                *value = maximum_value;
                if (*type == GRID_TRACK_FIXED && *minimum > *value) {
                    *minimum = *value;
                }
                return true;
            }
        }
    }
    char token[96];
    if (length >= sizeof(token)) return false;
    memcpy(token, text, length);
    token[length] = '\0';
    char *end = NULL;
    double number = strtod(token, &end);
    while (end != NULL && isspace((unsigned char) *end)) end++;
    if (end != NULL && end != token && strcmp(end, "fr") == 0) {
        if (number < 0.001) number = 0.001;
        if (number > 65.535) number = 65.535;
        *type = GRID_TRACK_FLEX;
        *value = (unsigned) (number * 1000.0 + 0.5);
        return true;
    }
    if (end != NULL && end != token && strcmp(end, "%") == 0) {
        if (number < 0.0) number = 0.0;
        if (number > 655.35) number = 655.35;
        *type = GRID_TRACK_PERCENT;
        *value = (unsigned) (number * 100.0 + 0.5);
        return true;
    }
    bool percent = false;
    int fixed = style_parse_length(sheet, text, length, -1, &percent);
    if (!percent && fixed >= 0) {
        *type = GRID_TRACK_FIXED;
        *value = (unsigned) fixed;
        *minimum = (unsigned) fixed;
        return true;
    }
    if (strcmp(token, "auto") == 0) {
        *value = GRID_TRACK_AUTO_VALUE;
        return true;
    }
    if (strcmp(token, "min-content") == 0) {
        *value = GRID_TRACK_MIN_CONTENT_VALUE;
        return true;
    }
    if (strcmp(token, "max-content") == 0) {
        *value = GRID_TRACK_MAX_CONTENT_VALUE;
        return true;
    }
    if (length > 13 && memcmp(text, "fit-content(", 12) == 0
        && text[length - 1] == ')') {
        bool fit_percent = false;
        int cap = style_parse_length(
            sheet, text + 12, length - 13, -1, &fit_percent);
        if (fit_percent || cap < 0) {
            return false;
        }
        if (cap > GRID_TRACK_FIT_CONTENT_MASK) {
            cap = GRID_TRACK_FIT_CONTENT_MASK;
        }
        *value = GRID_TRACK_FIT_CONTENT_FLAG | (unsigned) cap;
        return true;
    }
    return false;
}

static bool style_parse_grid_track_template(
    Stylesheet *sheet, const char *value, bool rows, ComputedStyle *style);

bool style_parse_grid_columns(Stylesheet *sheet, const char *text,
                               size_t length, ComputedStyle *style)
{
    char value[192];
    /* Resolve BEFORE clearing: a value that cannot be resolved is an invalid
       declaration, which CSS drops at parse time, so the previously cascaded
       track list must survive untouched. */
    if (!style_resolve_value(sheet, text, length, value, sizeof(value), 0)) {
        return false;
    }
    ComputedStyle candidate = *style;
    computed_style_set_grid_column_count(&candidate, 0);
    computed_style_set_grid_subgrid_columns(&candidate, false);
    computed_style_set_grid_column_template_id(&candidate, 0);
    candidate.grid_min_column_width = 0;
    if (strcmp(value, "none") == 0) {
        *style = candidate;
        return true;
    }
    const char *repeat = strstr(value, "repeat(");
    if (repeat == value) {
        const char *argument = repeat + 7;
        if (strncmp(argument, "auto-fit", 8) == 0
            || strncmp(argument, "auto-fill", 9) == 0) {
            const char *minimum = strstr(argument, "minmax(");
            if (minimum != NULL) {
                minimum += 7;
                const char *comma = strchr(minimum, ',');
                if (comma != NULL) {
                    int parsed = style_parse_length(sheet, minimum,
                                              (size_t) (comma - minimum),
                                              0, NULL);
                    if (parsed > 0) {
                        candidate.grid_min_column_width = parsed;
                    }
                }
            }
            computed_style_set_grid_column_count(&candidate, 0);
            *style = candidate;
            return true;
        }
    }
    if (!style_parse_grid_track_template(
            sheet, value, false, &candidate)) return false;
    *style = candidate;
    return true;
}

bool style_parse_grid_line(const Stylesheet *sheet, const char *text,
                            size_t length, int *line, int *span)
{
    char value[64];
    *line = 0;
    *span = 0;
    if (!style_resolve_value(sheet, text, length, value, sizeof(value), 0)) {
        return false;
    }
    char *start = value;
    while (isspace((unsigned char) *start)) start++;
    char *tail = start + strlen(start);
    while (tail > start && isspace((unsigned char) tail[-1])) *--tail = '\0';
    if (strcmp(start, "auto") == 0) return true;
    if (strncmp(start, "span", 4) == 0
        && isspace((unsigned char) start[4])) {
        char *end = NULL;
        long parsed = strtol(start + 5, &end, 10);
        while (end != NULL && isspace((unsigned char) *end)) end++;
        if (end != NULL && end != start + 5 && *end == '\0' && parsed > 0) {
            if (parsed > 8) parsed = 8;
            *span = (int) parsed;
            return true;
        }
        return false;
    }
    char *end = NULL;
    long parsed = strtol(start, &end, 10);
    while (end != NULL && isspace((unsigned char) *end)) end++;
    if (end == NULL || end == start || *end != '\0' || parsed == 0) {
        return false;
    }
    if (parsed < -6) parsed = -6;
    if (parsed > 9) parsed = 9;
    *line = (int) parsed;
    return true;
}

static bool grid_area_name_byte(unsigned char character)
{
    return isalnum(character) || character == '_' || character == '-'
           || character >= 0x80;
}

static StyleGridAreas *style_grid_areas(Stylesheet *sheet)
{
    if (sheet == NULL || sheet->budget == NULL) return NULL;
    if (sheet->grid_areas == NULL) {
        sheet->grid_areas = budget_calloc(
            sheet->budget, 1, sizeof(*sheet->grid_areas));
    }
    return sheet->grid_areas;
}

static uint8_t style_intern_grid_area_name(
    Stylesheet *sheet, const char *name, size_t length)
{
    if (sheet == NULL || name == NULL || length == 0
        || length >= STYLE_GRID_AREA_NAME_CAPACITY) return 0;
    StyleGridAreas *areas = style_grid_areas(sheet);
    if (areas == NULL) return 0;
    for (uint8_t i = 0; i < areas->name_count; i++) {
        if (strlen(areas->names[i]) == length
            && memcmp(areas->names[i], name, length) == 0) {
            return (uint8_t) (i + 1u);
        }
    }
    if (areas->name_count >= STYLE_GRID_AREA_NAME_LIMIT) return 0;
    uint8_t index = areas->name_count++;
    memcpy(areas->names[index], name, length);
    areas->names[index][length] = '\0';
    return (uint8_t) (index + 1u);
}

static uint8_t style_find_grid_area_name(
    const StyleGridAreas *areas, const char *name, size_t length)
{
    if (areas == NULL || name == NULL || length == 0) return 0;
    for (uint8_t i = 0; i < areas->name_count; i++) {
        if (strlen(areas->names[i]) == length
            && memcmp(areas->names[i], name, length) == 0) {
            return (uint8_t) (i + 1u);
        }
    }
    return 0;
}

static uint8_t style_find_grid_line_name(
    const StyleGridAreas *areas, const char *name, size_t length)
{
    if (areas == NULL || name == NULL || length == 0) return 0;
    for (uint8_t i = 0; i < areas->line_name_count; i++) {
        if (strlen(areas->line_names[i]) == length
            && memcmp(areas->line_names[i], name, length) == 0) {
            return (uint8_t) (i + 1u);
        }
    }
    return 0;
}

static uint8_t style_intern_grid_line_name(
    Stylesheet *sheet, const char *name, size_t length)
{
    if (sheet == NULL || name == NULL || length == 0
        || length >= STYLE_GRID_AREA_NAME_CAPACITY) return 0;
    StyleGridAreas *areas = style_grid_areas(sheet);
    if (areas == NULL) return 0;
    uint8_t existing = style_find_grid_line_name(areas, name, length);
    if (existing != 0) return existing;
    if (areas->line_name_count >= STYLE_GRID_LINE_NAME_LIMIT) return 0;
    uint8_t index = areas->line_name_count++;
    memcpy(areas->line_names[index], name, length);
    areas->line_names[index][length] = '\0';
    return (uint8_t) (index + 1u);
}

static bool grid_line_name_valid(const char *name, size_t length)
{
    if (name == NULL || length == 0
        || length >= STYLE_GRID_AREA_NAME_CAPACITY) return false;
    size_t first = name[0] == '-' ? 1u : 0u;
    if (first >= length || isdigit((unsigned char) name[first])) return false;
    for (size_t i = 0; i < length; i++) {
        if (!grid_area_name_byte((unsigned char) name[i])) return false;
    }
    return !(length == 4 && memcmp(name, "auto", 4) == 0)
        && !(length == 4 && memcmp(name, "span", 4) == 0)
        && !(length == 7 && memcmp(name, "initial", 7) == 0)
        && !(length == 7 && memcmp(name, "inherit", 7) == 0)
        && !(length == 5 && memcmp(name, "unset", 5) == 0)
        && !(length == 6 && memcmp(name, "revert", 6) == 0)
        && !(length == 12 && memcmp(name, "revert-layer", 12) == 0);
}

bool style_parse_grid_line_or_name(
    Stylesheet *sheet, const char *text, size_t length,
    int *line, int *span, uint8_t *name_id)
{
    if (line == NULL || span == NULL || name_id == NULL) return false;
    *name_id = 0;
    if (style_parse_grid_line(sheet, text, length, line, span)) return true;
    char resolved[STYLE_GRID_AREA_NAME_CAPACITY];
    if (sheet == NULL
        || !style_resolve_value(
               sheet, text, length, resolved, sizeof(resolved), 0)) {
        return false;
    }
    const char *name = resolved;
    size_t name_length = strlen(resolved);
    trim(&name, &name_length);
    if (!grid_line_name_valid(name, name_length)) return false;
    uint8_t id = style_intern_grid_line_name(sheet, name, name_length);
    if (id == 0) return false;
    *line = 0;
    *span = 0;
    *name_id = id;
    return true;
}

static bool grid_append_track(
    const Stylesheet *sheet, StyleGridTrackTemplate *parsed,
    const char *text, size_t length, unsigned limit)
{
    if (parsed == NULL || parsed->track_count >= limit) return false;
    uint8_t type = GRID_TRACK_AUTO;
    unsigned value = 0, minimum = 0;
    if (!style_parse_grid_track(
            sheet, text, length, &type, &value, &minimum)) return false;
    unsigned index = parsed->track_count++;
    parsed->track_types[index] = type;
    parsed->track_values[index] =
        value > UINT16_MAX ? UINT16_MAX : (uint16_t) value;
    parsed->track_minimums[index] =
        minimum > UINT16_MAX ? UINT16_MAX : (uint16_t) minimum;
    return true;
}

static bool grid_expand_uniform_repeat(
    const Stylesheet *sheet, StyleGridTrackTemplate *parsed,
    const char *text, size_t length, unsigned limit)
{
    if (length < 10 || memcmp(text, "repeat(", 7) != 0
        || text[length - 1] != ')') return false;
    const char *argument = text + 7;
    char *end = NULL;
    long count = strtol(argument, &end, 10);
    if (end == argument || count <= 0 || *end != ',') return false;
    const char *track = end + 1;
    size_t track_length = (size_t) (text + length - 1 - track);
    trim(&track, &track_length);
    if ((unsigned long) count > limit - parsed->track_count) {
        count = (long) (limit - parsed->track_count);
    }
    if (count <= 0) return false;
    uint8_t type = GRID_TRACK_AUTO;
    unsigned value = 0, minimum = 0;
    if (!style_parse_grid_track(
            sheet, track, track_length, &type, &value, &minimum)) return false;
    for (long i = 0; i < count; i++) {
        unsigned index = parsed->track_count++;
        parsed->track_types[index] = type;
        parsed->track_values[index] =
            value > UINT16_MAX ? UINT16_MAX : (uint16_t) value;
        parsed->track_minimums[index] =
            minimum > UINT16_MAX ? UINT16_MAX : (uint16_t) minimum;
    }
    return true;
}

static StyleGridTrackTemplate *style_grid_track_storage_prepare_slot(
    Stylesheet *sheet, StyleGridTrackStorage *storage, size_t index)
{
    if (sheet == NULL || sheet->budget == NULL || storage == NULL
        || index >= STYLE_GRID_TRACK_TEMPLATE_LIMIT) return NULL;
    size_t block_index = index / STYLE_GRID_TRACK_BLOCK_SIZE;
    if (block_index >= storage->capacity) {
        size_t capacity = storage->capacity == 0
            ? 2u : storage->capacity * 2u;
        if (capacity <= block_index) capacity = block_index + 1u;
        if (capacity > STYLE_GRID_TRACK_BLOCK_COUNT) {
            capacity = STYLE_GRID_TRACK_BLOCK_COUNT;
        }
        size_t old_capacity = storage->capacity;
        StyleGridTrackTemplate **blocks = budget_realloc(
            sheet->budget, storage->blocks,
            capacity * sizeof(*blocks));
        if (blocks == NULL) return NULL;
        memset(blocks + old_capacity, 0,
               (capacity - old_capacity) * sizeof(*blocks));
        storage->blocks = blocks;
        storage->capacity = capacity;
    }
    if (storage->blocks[block_index] == NULL) {
        storage->blocks[block_index] = budget_calloc(
            sheet->budget, STYLE_GRID_TRACK_BLOCK_SIZE,
            sizeof(*storage->blocks[block_index]));
        if (storage->blocks[block_index] == NULL) return NULL;
    }
    return style_grid_track_storage_slot(storage, index);
}

static bool style_parse_grid_track_template(
    Stylesheet *sheet, const char *value, bool rows, ComputedStyle *style)
{
    if (sheet == NULL || value == NULL || style == NULL) return false;
    unsigned track_limit = rows ? STYLE_GRID_AREA_ROW_LIMIT
                                : GRID_TRACK_REPEAT_LIMIT;
    StyleGridTrackTemplate parsed = {0};
    char local_names[GRID_TRACK_REPEAT_LIMIT + 1]
                    [STYLE_GRID_LINE_NAMES_PER_LINE]
                    [STYLE_GRID_AREA_NAME_CAPACITY] = {{{0}}};
    bool has_line_names = false;
    bool subgrid = strncmp(value, "subgrid", 7) == 0
        && (value[7] == '\0' || isspace((unsigned char) value[7])
            || value[7] == '[');
    parsed.subgrid = subgrid;
    size_t value_length = strlen(value);
    size_t at = subgrid ? 7u : 0u;
    unsigned subgrid_line = 0;
    while (at < value_length) {
        while (at < value_length
               && isspace((unsigned char) value[at])) at++;
        if (at >= value_length) break;
        if (value[at] == '[') {
            unsigned line = subgrid ? subgrid_line++ : parsed.track_count;
            if (line > track_limit) return false;
            at++;
            unsigned slot = 0;
            while (at < value_length && value[at] != ']') {
                while (at < value_length
                       && isspace((unsigned char) value[at])) at++;
                if (at >= value_length || value[at] == ']') break;
                size_t start = at;
                while (at < value_length && value[at] != ']'
                       && !isspace((unsigned char) value[at])) at++;
                size_t length = at - start;
                if (slot >= STYLE_GRID_LINE_NAMES_PER_LINE
                    || !grid_line_name_valid(value + start, length)) {
                    return false;
                }
                memcpy(local_names[line][slot], value + start, length);
                local_names[line][slot][length] = '\0';
                slot++;
                has_line_names = true;
            }
            if (at >= value_length || value[at] != ']') return false;
            at++;
            continue;
        }
        if (subgrid) return false;
        size_t end = at;
        int parentheses = 0;
        while (end < value_length) {
            char character = value[end];
            if (character == '(') parentheses++;
            else if (character == ')' && parentheses > 0) parentheses--;
            else if (parentheses == 0
                     && (isspace((unsigned char) character)
                         || character == '[')) break;
            end++;
        }
        if (end == at) return false;
        if (!grid_expand_uniform_repeat(
                sheet, &parsed, value + at, end - at, track_limit)
            && !grid_append_track(
                   sheet, &parsed, value + at, end - at, track_limit)) {
            return false;
        }
        at = end;
    }
    if (!subgrid && parsed.track_count == 0) return false;

    ComputedStyle candidate = *style;
    if (rows) {
        computed_style_set_grid_subgrid_rows(&candidate, subgrid);
        computed_style_set_grid_row_template_id(&candidate, 0);
    } else {
        computed_style_set_grid_subgrid_columns(&candidate, subgrid);
        computed_style_set_grid_column_template_id(&candidate, 0);
        computed_style_set_grid_column_count(&candidate, parsed.track_count);
    }

    StyleGridTrackStorage *storage = sheet->grid_tracks;
    if (storage == NULL) {
        storage = budget_calloc(sheet->budget, 1, sizeof(*storage));
        if (storage == NULL) return false;
        sheet->grid_tracks = storage;
    }
    StyleGridTrackTemplate *retained = storage->count
        < STYLE_GRID_TRACK_TEMPLATE_LIMIT
        ? style_grid_track_storage_prepare_slot(
              sheet, storage, storage->count)
        : NULL;
    if (storage->count < STYLE_GRID_TRACK_TEMPLATE_LIMIT
        && retained == NULL) return false;

    if (has_line_names) {
        StyleGridAreas *areas = style_grid_areas(sheet);
        if (areas == NULL) return false;
        unsigned new_names = 0;
        for (unsigned line = 0; line <= track_limit; line++) {
            for (unsigned slot = 0; slot < STYLE_GRID_LINE_NAMES_PER_LINE;
                 slot++) {
                const char *name = local_names[line][slot];
                size_t length = strlen(name);
                if (length == 0
                    || style_find_grid_line_name(areas, name, length) != 0) {
                    continue;
                }
                bool seen_earlier = false;
                for (unsigned prior_line = 0;
                     prior_line <= line && !seen_earlier; prior_line++) {
                    unsigned prior_slots = prior_line == line ? slot : 2u;
                    for (unsigned prior = 0; prior < prior_slots; prior++) {
                        if (strcmp(
                                local_names[prior_line][prior], name) == 0) {
                            seen_earlier = true;
                            break;
                        }
                    }
                }
                if (!seen_earlier) new_names++;
            }
        }
        if (new_names > (unsigned) STYLE_GRID_LINE_NAME_LIMIT
                        - areas->line_name_count) return false;
        if (retained == NULL && new_names != 0) return false;
        for (unsigned line = 0; line <= track_limit; line++) {
            for (unsigned slot = 0; slot < STYLE_GRID_LINE_NAMES_PER_LINE;
                 slot++) {
                const char *name = local_names[line][slot];
                if (name[0] == '\0') continue;
                uint8_t id = style_intern_grid_line_name(
                    sheet, name, strlen(name));
                if (id == 0) return false;
                parsed.line_names[line][slot] = id;
            }
        }
    }

    for (size_t i = 0; i < storage->count; i++) {
        const StyleGridTrackTemplate *existing =
            style_grid_track_storage_const_slot(storage, i);
        if (existing != NULL && memcmp(existing, &parsed, sizeof(parsed)) == 0) {
            if (rows) computed_style_set_grid_row_template_id(
                &candidate, (uint8_t) (i + 1u));
            else computed_style_set_grid_column_template_id(
                &candidate, (uint8_t) (i + 1u));
            *style = candidate;
            return true;
        }
    }
    if (retained == NULL) return false;
    size_t index = storage->count++;
    *retained = parsed;
    if (rows) computed_style_set_grid_row_template_id(
        &candidate, (uint8_t) (index + 1u));
    else computed_style_set_grid_column_template_id(
        &candidate, (uint8_t) (index + 1u));
    *style = candidate;
    return true;
}

bool style_parse_grid_rows(Stylesheet *sheet, const char *text,
                           size_t length, ComputedStyle *style)
{
    char value[192];
    if (sheet == NULL || style == NULL
        || !style_resolve_value(
               sheet, text, length, value, sizeof(value), 0)) return false;
    ComputedStyle candidate = *style;
    computed_style_set_grid_subgrid_rows(&candidate, false);
    computed_style_set_grid_row_template_id(&candidate, 0);
    if (strcmp(value, "none") == 0) {
        *style = candidate;
        return true;
    }
    if (!style_parse_grid_track_template(
            sheet, value, true, &candidate)) return false;
    *style = candidate;
    return true;
}

static int local_grid_area_name(
    char names[STYLE_GRID_AREA_RECT_LIMIT][STYLE_GRID_AREA_NAME_CAPACITY],
    unsigned *count, const char *name, size_t length)
{
    if (count == NULL || name == NULL || length == 0
        || length >= STYLE_GRID_AREA_NAME_CAPACITY) return -1;
    for (unsigned i = 0; i < *count; i++) {
        if (strlen(names[i]) == length
            && memcmp(names[i], name, length) == 0) return (int) i;
    }
    if (*count >= STYLE_GRID_AREA_RECT_LIMIT) return -1;
    unsigned index = (*count)++;
    memcpy(names[index], name, length);
    names[index][length] = '\0';
    return (int) index;
}

bool style_parse_grid_template_areas(
    Stylesheet *sheet, const char *text, size_t length, ComputedStyle *style)
{
    char resolved[512];
    if (sheet == NULL || style == NULL
        || !style_resolve_value(
               sheet, text, length, resolved, sizeof(resolved), 0)) {
        return false;
    }
    const char *value = resolved;
    size_t value_length = strlen(value);
    trim(&value, &value_length);
    if (value_length == 4 && memcmp(value, "none", 4) == 0) {
        computed_style_set_grid_template_area_id(style, 0);
        return true;
    }

    uint8_t cells[STYLE_GRID_AREA_ROW_LIMIT][GRID_TRACK_REPEAT_LIMIT] = {{0}};
    char names[STYLE_GRID_AREA_RECT_LIMIT][STYLE_GRID_AREA_NAME_CAPACITY] = {{0}};
    unsigned name_count = 0;
    unsigned rows = 0;
    unsigned columns = 0;
    size_t at = 0;
    while (at < value_length) {
        while (at < value_length
               && isspace((unsigned char) value[at])) at++;
        if (at >= value_length) break;
        char quote = value[at];
        if ((quote != '"' && quote != '\'')
            || rows >= STYLE_GRID_AREA_ROW_LIMIT) return false;
        at++;
        unsigned row_columns = 0;
        bool closed = false;
        while (at < value_length) {
            while (at < value_length
                   && isspace((unsigned char) value[at])) at++;
            if (at >= value_length) break;
            if (value[at] == quote) {
                at++;
                closed = true;
                break;
            }
            if (row_columns >= GRID_TRACK_REPEAT_LIMIT) return false;
            if (value[at] == '.') {
                while (at < value_length && value[at] == '.') at++;
                cells[rows][row_columns++] = 0;
                continue;
            }
            size_t start = at;
            while (at < value_length && value[at] != quote
                   && value[at] != '.'
                   && !isspace((unsigned char) value[at])) {
                if (!grid_area_name_byte((unsigned char) value[at])) {
                    return false;
                }
                at++;
            }
            int local = local_grid_area_name(
                names, &name_count, value + start, at - start);
            if (local < 0) return false;
            cells[rows][row_columns++] = (uint8_t) (local + 1);
        }
        if (!closed || row_columns == 0
            || (rows != 0 && row_columns != columns)) return false;
        columns = row_columns;
        rows++;
    }
    if (rows == 0 || columns == 0) return false;

    StyleGridAreaRect local_rects[STYLE_GRID_AREA_RECT_LIMIT] = {{0}};
    for (unsigned name = 0; name < name_count; name++) {
        unsigned first_row = rows, first_column = columns;
        unsigned last_row = 0, last_column = 0;
        bool found = false;
        for (unsigned row = 0; row < rows; row++) {
            for (unsigned column = 0; column < columns; column++) {
                if (cells[row][column] != name + 1u) continue;
                if (row < first_row) first_row = row;
                if (column < first_column) first_column = column;
                if (row + 1u > last_row) last_row = row + 1u;
                if (column + 1u > last_column) last_column = column + 1u;
                found = true;
            }
        }
        if (!found) return false;
        for (unsigned row = first_row; row < last_row; row++) {
            for (unsigned column = first_column;
                 column < last_column; column++) {
                if (cells[row][column] != name + 1u) return false;
            }
        }
        local_rects[name] = (StyleGridAreaRect) {
            .row_start = (uint8_t) first_row,
            .row_end = (uint8_t) last_row,
            .column_start = (uint8_t) first_column,
            .column_end = (uint8_t) last_column
        };
    }

    StyleGridAreas *areas = style_grid_areas(sheet);
    if (areas == NULL) return false;
    unsigned new_names = 0;
    for (unsigned name = 0; name < name_count; name++) {
        if (style_find_grid_area_name(
                areas, names[name], strlen(names[name])) == 0) {
            new_names++;
        }
    }
    if (new_names
        > (unsigned) STYLE_GRID_AREA_NAME_LIMIT - areas->name_count) {
        return false;
    }
    if (areas->template_count >= STYLE_GRID_AREA_TEMPLATE_LIMIT
        && new_names != 0) {
        return false;
    }
    for (unsigned name = 0; name < name_count; name++) {
        uint8_t id = style_intern_grid_area_name(
            sheet, names[name], strlen(names[name]));
        if (id == 0) return false;
        local_rects[name].name_id = id;
    }
    StyleGridAreaTemplate candidate = {
        .rows = (uint8_t) rows,
        .columns = (uint8_t) columns,
        .area_count = (uint8_t) name_count
    };
    memcpy(candidate.areas, local_rects,
           name_count * sizeof(local_rects[0]));
    for (uint8_t i = 0; i < areas->template_count; i++) {
        if (memcmp(&areas->templates[i], &candidate, sizeof(candidate)) == 0) {
            computed_style_set_grid_template_area_id(
                style, (uint8_t) (i + 1u));
            return true;
        }
    }
    if (areas->template_count >= STYLE_GRID_AREA_TEMPLATE_LIMIT) {
        return false;
    }
    uint8_t index = areas->template_count++;
    areas->templates[index] = candidate;
    computed_style_set_grid_template_area_id(
        style, (uint8_t) (index + 1u));
    return true;
}

bool style_parse_grid_area_name(
    Stylesheet *sheet, const char *text, size_t length, ComputedStyle *style)
{
    char resolved[STYLE_GRID_AREA_NAME_CAPACITY];
    if (sheet == NULL || style == NULL
        || !style_resolve_value(
               sheet, text, length, resolved, sizeof(resolved), 0)) {
        return false;
    }
    const char *name = resolved;
    size_t name_length = strlen(name);
    trim(&name, &name_length);
    if (name_length == 0
        || (name_length == 4 && memcmp(name, "auto", 4) == 0)
        || (name_length == 4 && memcmp(name, "span", 4) == 0)
        || (name_length == 7 && memcmp(name, "initial", 7) == 0)
        || (name_length == 7 && memcmp(name, "inherit", 7) == 0)
        || (name_length == 5 && memcmp(name, "unset", 5) == 0)
        || (name_length == 6 && memcmp(name, "revert", 6) == 0)
        || (name_length == 12
            && memcmp(name, "revert-layer", 12) == 0)) {
        return false;
    }
    size_t first = name[0] == '-' ? 1u : 0u;
    if (first >= name_length
        || isdigit((unsigned char) name[first])
        || (name[first] == '-' && first + 1u >= name_length)) return false;
    for (size_t i = 0; i < name_length; i++) {
        if (!grid_area_name_byte((unsigned char) name[i])) return false;
    }
    uint8_t id = style_intern_grid_area_name(sheet, name, name_length);
    if (id == 0) return false;
    computed_style_set_grid_named_area_id(style, id);
    return true;
}

static const StyleGridAreaTemplate *style_grid_area_template(
    const Stylesheet *sheet, const ComputedStyle *container)
{
    uint8_t id = computed_style_grid_template_area_id(container);
    if (sheet == NULL || sheet->grid_areas == NULL || id == 0
        || id > sheet->grid_areas->template_count) return NULL;
    return &sheet->grid_areas->templates[id - 1u];
}

unsigned stylesheet_grid_template_area_rows(
    const Stylesheet *sheet, const ComputedStyle *container)
{
    const StyleGridAreaTemplate *areas =
        style_grid_area_template(sheet, container);
    return areas == NULL ? 0 : areas->rows;
}

unsigned stylesheet_grid_template_area_columns(
    const Stylesheet *sheet, const ComputedStyle *container)
{
    const StyleGridAreaTemplate *areas =
        style_grid_area_template(sheet, container);
    return areas == NULL ? 0 : areas->columns;
}

bool stylesheet_resolve_named_grid_area(
    const Stylesheet *sheet, const ComputedStyle *container,
    ComputedStyle *item)
{
    uint8_t name_id = computed_style_grid_named_area_id(item);
    const StyleGridAreaTemplate *areas =
        style_grid_area_template(sheet, container);
    if (name_id == 0 || areas == NULL) return false;
    for (uint8_t i = 0; i < areas->area_count; i++) {
        const StyleGridAreaRect *area = &areas->areas[i];
        if (area->name_id != name_id) continue;
        computed_style_set_grid_named_area_id(item, 0);
        computed_style_set_grid_row_start(
            item, computed_style_encode_grid_line(area->row_start + 1));
        computed_style_set_grid_row_end(
            item, computed_style_encode_grid_line(area->row_end + 1));
        computed_style_set_grid_column_start(
            item, computed_style_encode_grid_line(area->column_start + 1));
        computed_style_set_grid_column_end(
            item, computed_style_encode_grid_line(area->column_end + 1));
        return true;
    }
    return false;
}

static bool append_grid_area_serialization(
    char *output, size_t capacity, size_t *used,
    const char *text, size_t length)
{
    if (output == NULL || used == NULL || text == NULL || capacity == 0
        || length >= capacity - *used) return false;
    memcpy(output + *used, text, length);
    *used += length;
    output[*used] = '\0';
    return true;
}

bool stylesheet_serialize_grid_template_areas(
    const Stylesheet *sheet, const ComputedStyle *container,
    char *output, size_t capacity)
{
    if (output == NULL || capacity == 0) return false;
    output[0] = '\0';
    const StyleGridAreaTemplate *areas =
        style_grid_area_template(sheet, container);
    if (areas == NULL || sheet == NULL || sheet->grid_areas == NULL) {
        size_t used = 0;
        return append_grid_area_serialization(
            output, capacity, &used, "none", 4);
    }
    size_t used = 0;
    for (uint8_t row = 0; row < areas->rows; row++) {
        if (row != 0
            && !append_grid_area_serialization(
                   output, capacity, &used, " ", 1)) return false;
        if (!append_grid_area_serialization(
                output, capacity, &used, "\"", 1)) return false;
        for (uint8_t column = 0; column < areas->columns; column++) {
            if (column != 0
                && !append_grid_area_serialization(
                       output, capacity, &used, " ", 1)) return false;
            const char *name = ".";
            for (uint8_t i = 0; i < areas->area_count; i++) {
                const StyleGridAreaRect *area = &areas->areas[i];
                if (row >= area->row_start && row < area->row_end
                    && column >= area->column_start
                    && column < area->column_end
                    && area->name_id != 0
                    && area->name_id <= sheet->grid_areas->name_count) {
                    name = sheet->grid_areas->names[area->name_id - 1u];
                    break;
                }
            }
            if (!append_grid_area_serialization(
                    output, capacity, &used, name, strlen(name))) return false;
        }
        if (!append_grid_area_serialization(
                output, capacity, &used, "\"", 1)) return false;
    }
    return true;
}

static const StyleGridTrackTemplate *style_grid_track_template(
    const Stylesheet *sheet, const ComputedStyle *container, bool rows)
{
    uint8_t id = rows
        ? computed_style_grid_row_template_id(container)
        : computed_style_grid_column_template_id(container);
    if (sheet == NULL || sheet->grid_tracks == NULL || id == 0
        || id > sheet->grid_tracks->count) return NULL;
    return style_grid_track_storage_const_slot(sheet->grid_tracks, id - 1u);
}

unsigned stylesheet_grid_track_count(
    const Stylesheet *sheet, const ComputedStyle *container, bool rows)
{
    const StyleGridTrackTemplate *template =
        style_grid_track_template(sheet, container, rows);
    if (template != NULL && !template->subgrid) return template->track_count;
    if (!rows) return computed_style_grid_column_count(container);
    return 0;
}

uint8_t stylesheet_grid_track_type(
    const Stylesheet *sheet, const ComputedStyle *container,
    bool rows, unsigned index)
{
    const StyleGridTrackTemplate *template =
        style_grid_track_template(sheet, container, rows);
    if (template != NULL && !template->subgrid
        && index < template->track_count) return template->track_types[index];
    return GRID_TRACK_AUTO;
}

unsigned stylesheet_grid_track_value(
    const Stylesheet *sheet, const ComputedStyle *container,
    bool rows, unsigned index)
{
    const StyleGridTrackTemplate *template =
        style_grid_track_template(sheet, container, rows);
    if (template != NULL && !template->subgrid
        && index < template->track_count) return template->track_values[index];
    return 0;
}

unsigned stylesheet_grid_track_minimum(
    const Stylesheet *sheet, const ComputedStyle *container,
    bool rows, unsigned index)
{
    const StyleGridTrackTemplate *template =
        style_grid_track_template(sheet, container, rows);
    if (template != NULL && !template->subgrid
        && index < template->track_count) {
        return template->track_minimums[index];
    }
    return 0;
}

uint8_t stylesheet_grid_track_line_name(
    const Stylesheet *sheet, const ComputedStyle *container,
    bool rows, unsigned line, unsigned slot)
{
    const StyleGridTrackTemplate *template =
        style_grid_track_template(sheet, container, rows);
    if (template == NULL || line > GRID_TRACK_REPEAT_LIMIT
        || slot >= STYLE_GRID_LINE_NAMES_PER_LINE) return 0;
    return template->line_names[line][slot];
}

static int style_grid_named_line(
    const Stylesheet *sheet, const ComputedStyle *container,
    bool rows, uint8_t name_id)
{
    if (sheet == NULL || sheet->grid_areas == NULL || name_id == 0
        || name_id > sheet->grid_areas->line_name_count) return 0;
    const StyleGridTrackTemplate *template =
        style_grid_track_template(sheet, container, rows);
    if (template != NULL) {
        for (unsigned line = 0; line <= GRID_TRACK_REPEAT_LIMIT; line++) {
            for (unsigned slot = 0; slot < STYLE_GRID_LINE_NAMES_PER_LINE;
                 slot++) {
                if (template->line_names[line][slot] == name_id) {
                    return (int) line + 1;
                }
            }
        }
    }

    const char *wanted = sheet->grid_areas->line_names[name_id - 1u];
    size_t wanted_length = strlen(wanted);
    const StyleGridAreaTemplate *areas =
        style_grid_area_template(sheet, container);
    if (areas == NULL) return 0;
    for (uint8_t i = 0; i < areas->area_count; i++) {
        const StyleGridAreaRect *area = &areas->areas[i];
        if (area->name_id == 0
            || area->name_id > sheet->grid_areas->name_count) continue;
        const char *area_name =
            sheet->grid_areas->names[area->name_id - 1u];
        size_t area_length = strlen(area_name);
        if (wanted_length == area_length + 6u
            && memcmp(wanted, area_name, area_length) == 0
            && memcmp(wanted + area_length, "-start", 6) == 0) {
            return (rows ? area->row_start : area->column_start) + 1;
        }
        if (wanted_length == area_length + 4u
            && memcmp(wanted, area_name, area_length) == 0
            && memcmp(wanted + area_length, "-end", 4) == 0) {
            return (rows ? area->row_end : area->column_end) + 1;
        }
    }
    return 0;
}

bool stylesheet_resolve_named_grid_lines(
    const Stylesheet *sheet, const ComputedStyle *container,
    ComputedStyle *item)
{
    if (item == NULL) return false;
    bool resolved = false;
#define RESOLVE_GRID_LINE_NAME(axis, field, rows_value)                     \
    do {                                                                    \
        uint8_t name = computed_style_grid_##axis##_##field##_name(item);    \
        if (name != 0) {                                                     \
            int line = style_grid_named_line(                                \
                sheet, container, rows_value, name);                         \
            if (line != 0) {                                                 \
                computed_style_set_grid_##axis##_##field(                    \
                    item, computed_style_encode_grid_line(line));            \
                resolved = true;                                             \
                computed_style_set_grid_##axis##_##field##_name(item, 0);    \
            }                                                               \
        }                                                                   \
    } while (0)
    RESOLVE_GRID_LINE_NAME(row, start, true);
    RESOLVE_GRID_LINE_NAME(row, end, true);
    RESOLVE_GRID_LINE_NAME(column, start, false);
    RESOLVE_GRID_LINE_NAME(column, end, false);
#undef RESOLVE_GRID_LINE_NAME
    return resolved;
}

static bool append_grid_track_token(
    char *output, size_t capacity, size_t *used,
    uint8_t type, unsigned value, unsigned minimum)
{
    char token[64];
    if (type == GRID_TRACK_FIXED) {
        snprintf(token, sizeof(token), "%upx", value);
    } else if (type == GRID_TRACK_PERCENT || type == GRID_TRACK_FLEX) {
        unsigned scale = type == GRID_TRACK_PERCENT ? 100u : 1000u;
        unsigned digits = type == GRID_TRACK_PERCENT ? 2u : 3u;
        const char *suffix = type == GRID_TRACK_PERCENT ? "%" : "fr";
        unsigned remainder = value % scale;
        if (remainder == 0) {
            snprintf(token, sizeof(token), "%u%s", value / scale, suffix);
        } else {
            char fractional[4];
            snprintf(fractional, sizeof(fractional), "%0*u",
                     (int) digits, remainder);
            while (digits > 1u && fractional[digits - 1u] == '0') {
                fractional[--digits] = '\0';
            }
            snprintf(token, sizeof(token), "%u.%s%s",
                     value / scale, fractional, suffix);
        }
    } else if (value == GRID_TRACK_MIN_CONTENT_VALUE) {
        snprintf(token, sizeof(token), "min-content");
    } else if (value == GRID_TRACK_MAX_CONTENT_VALUE) {
        snprintf(token, sizeof(token), "max-content");
    } else if ((value & GRID_TRACK_FIT_CONTENT_FLAG) != 0) {
        snprintf(token, sizeof(token), "fit-content(%upx)",
                 value & GRID_TRACK_FIT_CONTENT_MASK);
    } else {
        snprintf(token, sizeof(token), "auto");
    }
    if (minimum != 0 && type != GRID_TRACK_FIXED) {
        char wrapped[96];
        snprintf(wrapped, sizeof(wrapped), "minmax(%upx,%s)",
                 minimum, token);
        return append_grid_area_serialization(
            output, capacity, used, wrapped, strlen(wrapped));
    }
    return append_grid_area_serialization(
        output, capacity, used, token, strlen(token));
}

bool stylesheet_serialize_grid_template_tracks(
    const Stylesheet *sheet, const ComputedStyle *container, bool rows,
    char *output, size_t capacity)
{
    if (output == NULL || capacity == 0) return false;
    output[0] = '\0';
    const StyleGridTrackTemplate *template =
        style_grid_track_template(sheet, container, rows);
    bool subgrid = rows ? computed_style_grid_subgrid_rows(container)
                        : computed_style_grid_subgrid_columns(container);
    size_t used = 0;
    if (subgrid) {
        if (!append_grid_area_serialization(
                output, capacity, &used, "subgrid", 7)) return false;
    }
    unsigned count = subgrid ? 0
        : stylesheet_grid_track_count(sheet, container, rows);
    unsigned lines = subgrid && template != NULL
        ? GRID_TRACK_REPEAT_LIMIT + 1u : count + 1u;
    for (unsigned line = 0; line < lines; line++) {
        bool has_names = false;
        for (unsigned slot = 0; slot < STYLE_GRID_LINE_NAMES_PER_LINE;
             slot++) {
            if (template != NULL
                && template->line_names[line][slot] != 0) {
                has_names = true;
            }
        }
        if (has_names) {
            if (used != 0 && !append_grid_area_serialization(
                    output, capacity, &used, " ", 1)) return false;
            if (!append_grid_area_serialization(
                    output, capacity, &used, "[", 1)) return false;
            bool first = true;
            for (unsigned slot = 0; slot < STYLE_GRID_LINE_NAMES_PER_LINE;
                 slot++) {
                uint8_t id = template->line_names[line][slot];
                if (id == 0 || sheet == NULL || sheet->grid_areas == NULL
                    || id > sheet->grid_areas->line_name_count) continue;
                if (!first && !append_grid_area_serialization(
                        output, capacity, &used, " ", 1)) return false;
                const char *name = sheet->grid_areas->line_names[id - 1u];
                if (!append_grid_area_serialization(
                        output, capacity, &used, name, strlen(name))) {
                    return false;
                }
                first = false;
            }
            if (!append_grid_area_serialization(
                    output, capacity, &used, "]", 1)) return false;
        }
        if (line >= count) continue;
        if (used != 0 && !append_grid_area_serialization(
                output, capacity, &used, " ", 1)) return false;
        if (!append_grid_track_token(
                output, capacity, &used,
                stylesheet_grid_track_type(sheet, container, rows, line),
                stylesheet_grid_track_value(sheet, container, rows, line),
                stylesheet_grid_track_minimum(
                    sheet, container, rows, line))) return false;
    }
    if (used == 0) {
        return append_grid_area_serialization(
            output, capacity, &used, "none", 4);
    }
    return true;
}

size_t style_append_css_codepoint(char *output, size_t length,
                                   size_t capacity, unsigned codepoint)
{
    unsigned char bytes[4];
    size_t count = 0;
    if (codepoint <= 0x7f) {
        bytes[count++] = (unsigned char) codepoint;
    } else if (codepoint <= 0x7ff) {
        bytes[count++] = (unsigned char) (0xc0u | (codepoint >> 6));
        bytes[count++] = (unsigned char) (0x80u | (codepoint & 0x3fu));
    } else if (codepoint <= 0xffff) {
        bytes[count++] = (unsigned char) (0xe0u | (codepoint >> 12));
        bytes[count++] = (unsigned char) (0x80u | ((codepoint >> 6) & 0x3fu));
        bytes[count++] = (unsigned char) (0x80u | (codepoint & 0x3fu));
    } else if (codepoint <= 0x10ffff) {
        bytes[count++] = (unsigned char) (0xf0u | (codepoint >> 18));
        bytes[count++] = (unsigned char) (0x80u | ((codepoint >> 12) & 0x3fu));
        bytes[count++] = (unsigned char) (0x80u | ((codepoint >> 6) & 0x3fu));
        bytes[count++] = (unsigned char) (0x80u | (codepoint & 0x3fu));
    }
    if (count == 0 || length + count >= capacity) return length;
    memcpy(output + length, bytes, count);
    return length + count;
}

size_t style_decode_generated_text(const char *value, char *output,
                                    size_t capacity)
{
    if (capacity == 0 || value == NULL) return 0;
    output[0] = '\0';
    while (isspace((unsigned char) *value)) value++;
    char quote = *value;
    if (quote != '\'' && quote != '"') return 0;
    value++;
    size_t written = 0;
    while (*value != '\0' && *value != quote && written + 1 < capacity) {
        if (*value != '\\') {
            output[written++] = *value++;
            continue;
        }
        value++;
        if (*value == '\n' || *value == '\r' || *value == '\f') {
            if (*value == '\r' && value[1] == '\n') value++;
            value++;
            continue;
        }
        unsigned codepoint = 0;
        size_t digits = 0;
        while (digits < 6 && isxdigit((unsigned char) *value)) {
            unsigned char character = (unsigned char) *value++;
            unsigned digit = isdigit(character) ? character - '0'
                             : (unsigned) (tolower(character) - 'a' + 10);
            codepoint = (codepoint << 4) | digit;
            digits++;
        }
        if (digits != 0) {
            if (isspace((unsigned char) *value)) value++;
            if (codepoint == 0 || (codepoint >= 0xd800 && codepoint <= 0xdfff)
                || codepoint > 0x10ffff) codepoint = 0xfffd;
            written = style_append_css_codepoint(output, written, capacity,
                                           codepoint);
        } else if (*value != '\0') {
            output[written++] = *value++;
        }
    }
    output[written] = '\0';
    return written;
}

const char *style_store_generated_text(Stylesheet *sheet, const char *text,
                                        size_t length)
{
    if (sheet == NULL || text == NULL || length == 0
        || length >= STYLE_GENERATED_TEXT_CAPACITY) return NULL;
    for (size_t i = 0; i < sheet->generated_text_count; i++) {
        if (strlen(sheet->generated_texts[i]) == length
            && memcmp(sheet->generated_texts[i], text, length) == 0) {
            return sheet->generated_texts[i];
        }
    }
    if (sheet->generated_text_count >= STYLE_GENERATED_TEXT_LIMIT) return NULL;
    if (sheet->generated_text_count == sheet->generated_text_capacity) {
        size_t capacity = sheet->generated_text_capacity == 0
                          ? 8 : sheet->generated_text_capacity * 2;
        if (capacity > STYLE_GENERATED_TEXT_LIMIT) {
            capacity = STYLE_GENERATED_TEXT_LIMIT;
        }
        char **texts = budget_realloc(sheet->budget, sheet->generated_texts,
                                      capacity * sizeof(*texts));
        if (texts == NULL) return NULL;
        sheet->generated_texts = texts;
        sheet->generated_text_capacity = capacity;
    }
    char *copy = budget_malloc(sheet->budget, length + 1);
    if (copy == NULL) return NULL;
    memcpy(copy, text, length);
    copy[length] = '\0';
    sheet->generated_texts[sheet->generated_text_count++] = copy;
    sheet->generated_text_bytes += length + 1;
    return copy;
}

static bool style_counter_operations_equal(
    const StyleCounterOperations *left,
    const StyleCounterOperations *right)
{
    if (left == NULL || right == NULL || left->count != right->count) {
        return false;
    }
    for (size_t i = 0; i < left->count; i++) {
        if (left->operations[i].value != right->operations[i].value
            || left->operations[i].name == NULL
            || right->operations[i].name == NULL
            || strcmp(left->operations[i].name,
                      right->operations[i].name) != 0) {
            return false;
        }
    }
    return true;
}

uint8_t style_store_counter_operations(
    Stylesheet *sheet, const StyleCounterOperations *operations)
{
    if (sheet == NULL || operations == NULL || operations->count == 0
        || operations->count > STYLE_COUNTER_OPERATION_LIMIT) return 0;
    for (size_t i = 0; i < sheet->counter_operation_set_count; i++) {
        if (style_counter_operations_equal(
                &sheet->counter_operation_sets[i], operations)) {
            return (uint8_t) (i + 1);
        }
    }
    if (sheet->counter_operation_set_count
        >= STYLE_COUNTER_OPERATION_SET_LIMIT) return 0;
    if (sheet->counter_operation_set_count
        == sheet->counter_operation_set_capacity) {
        size_t capacity = sheet->counter_operation_set_capacity == 0
            ? 8 : sheet->counter_operation_set_capacity * 2;
        if (capacity > STYLE_COUNTER_OPERATION_SET_LIMIT) {
            capacity = STYLE_COUNTER_OPERATION_SET_LIMIT;
        }
        StyleCounterOperations *sets = budget_realloc(
            sheet->budget, sheet->counter_operation_sets,
            capacity * sizeof(*sets));
        if (sets == NULL) return 0;
        sheet->counter_operation_sets = sets;
        sheet->counter_operation_set_capacity = capacity;
    }
    sheet->counter_operation_sets[sheet->counter_operation_set_count] =
        *operations;
    sheet->counter_operation_set_count++;
    return (uint8_t) sheet->counter_operation_set_count;
}

const StyleCounterOperations *style_counter_operations(
    const Stylesheet *sheet, uint8_t id)
{
    if (sheet == NULL || id == 0
        || (size_t) id > sheet->counter_operation_set_count) return NULL;
    return &sheet->counter_operation_sets[id - 1];
}
