/* CSS calc()/math expression parsing and evaluation, plus the base
   length parsers built on it.  Split out of style.c. */

#include "style_internal.h"

#include <math.h>
#include <stdlib.h>
#include <stdio.h>



static void style_math_skip_space(StyleMathParser *parser)
{
    while (parser->at < parser->length
           && isspace((unsigned char) parser->text[parser->at])) {
        parser->at++;
    }
}

static bool style_math_q16(double value, int64_t *result)
{
    if (!isfinite(value) || result == NULL
        || value > (double) INT32_MAX / (double) STYLE_MATH_Q16
        || value < (double) INT32_MIN / (double) STYLE_MATH_Q16) {
        return false;
    }
    double scaled = value * (double) STYLE_MATH_Q16;
    *result = (int64_t) (scaled < 0.0 ? scaled - 0.5 : scaled + 0.5);
    return true;
}

static bool style_math_px(double value, int64_t *result)
{
    static const double scale = 256.0;
    if (!isfinite(value) || result == NULL
        || value > (double) STYLE_LENGTH_DIRECT_LIMIT
        || value < -(double) STYLE_LENGTH_DIRECT_LIMIT) return false;
    double scaled = value * scale;
    *result = (int64_t) (scaled < 0.0 ? scaled - 0.5 : scaled + 0.5);
    return true;
}

static int style_math_add_node(StyleMathParser *parser, StyleMathNode node)
{
    if (parser->failed || parser->node_count >= STYLE_MATH_NODE_LIMIT) {
        parser->failed = true;
        return -1;
    }
    parser->nodes[parser->node_count] = node;
    return (int) parser->node_count++;
}

static int style_math_number_node(StyleMathParser *parser, int64_t q16)
{
    if (q16 > INT32_MAX || q16 < INT32_MIN) {
        parser->failed = true;
        return -1;
    }
    return style_math_add_node(parser, (StyleMathNode) {
        .a = q16, .op = STYLE_MATH_NODE_VALUE,
        .type = STYLE_MATH_TYPE_NUMBER, .folded = true
    });
}

static int style_math_length_node(StyleMathParser *parser,
                                  int64_t pixels_q8,
                                  int64_t percent_q16)
{
    if (pixels_q8 > INT32_MAX || pixels_q8 < INT32_MIN
        || percent_q16 > INT32_MAX || percent_q16 < INT32_MIN) {
        parser->failed = true;
        return -1;
    }
    return style_math_add_node(parser, (StyleMathNode) {
        .a = pixels_q8, .b = percent_q16,
        .op = STYLE_MATH_NODE_VALUE, .type = STYLE_MATH_TYPE_LENGTH,
        .folded = true
    });
}

static bool style_math_scaled(int64_t value, int64_t scalar_q16,
                              int64_t *result)
{
    if (result == NULL || scalar_q16 > INT32_MAX
        || scalar_q16 < INT32_MIN) return false;
    /* Both operands are bounded to signed 32-bit values before this helper is
       called, so their product is representable in int64_t.  Keeping the
       multiply whole also avoids a division-by-zero branch for a zero scale. */
    *result = value * scalar_q16 / STYLE_MATH_Q16;
    return true;
}

static bool style_math_divided(int64_t value, int64_t divisor_q16,
                               int64_t *result)
{
    if (result == NULL || divisor_q16 == 0 || divisor_q16 > INT32_MAX
        || divisor_q16 < INT32_MIN) return false;
    /* A retained coefficient and a Q16 divisor are each signed 32-bit;
       multiplying the coefficient by Q16 therefore remains within int64_t. */
    *result = value * STYLE_MATH_Q16 / divisor_q16;
    return true;
}

static int style_math_binary(StyleMathParser *parser, uint8_t op,
                             int left, int right)
{
    if (left < 0 || right < 0) return -1;
    StyleMathNode *a = &parser->nodes[left];
    StyleMathNode *b = &parser->nodes[right];
    if (op == STYLE_MATH_NODE_ADD || op == STYLE_MATH_NODE_SUBTRACT) {
        if (a->type != b->type) {
            parser->failed = true;
            return -1;
        }
        int sign = op == STYLE_MATH_NODE_SUBTRACT ? -1 : 1;
        if (a->folded && b->folded) {
            int64_t first = a->a + sign * b->a;
            int64_t second = a->b + sign * b->b;
            return a->type == STYLE_MATH_TYPE_NUMBER
                ? style_math_number_node(parser, first)
                : style_math_length_node(parser, first, second);
        }
        return style_math_add_node(parser, (StyleMathNode) {
            .op = op, .type = a->type, .left = (uint8_t) left,
            .right = (uint8_t) right
        });
    }
    if (op == STYLE_MATH_NODE_MINIMUM || op == STYLE_MATH_NODE_MAXIMUM) {
        if (a->type != b->type) {
            parser->failed = true;
            return -1;
        }
        if (a->folded && b->folded
            && (a->type == STYLE_MATH_TYPE_NUMBER || a->b == b->b)) {
            int64_t value = op == STYLE_MATH_NODE_MINIMUM
                            ? (a->a < b->a ? a->a : b->a)
                            : (a->a > b->a ? a->a : b->a);
            return a->type == STYLE_MATH_TYPE_NUMBER
                ? style_math_number_node(parser, value)
                : style_math_length_node(parser, value, a->b);
        }
        return style_math_add_node(parser, (StyleMathNode) {
            .op = op, .type = a->type, .left = (uint8_t) left,
            .right = (uint8_t) right
        });
    }
    parser->failed = true;
    return -1;
}

static int style_math_scale(StyleMathParser *parser, int value,
                            int scalar)
{
    if (value < 0 || scalar < 0) return -1;
    StyleMathNode *length = &parser->nodes[value];
    StyleMathNode *number = &parser->nodes[scalar];
    if (number->type != STYLE_MATH_TYPE_NUMBER || !number->folded) {
        parser->failed = true;
        return -1;
    }
    int64_t first = 0, second = 0;
    if (length->folded) {
        if (!style_math_scaled(length->a, number->a, &first)
            || !style_math_scaled(length->b, number->a, &second)) {
            parser->failed = true;
            return -1;
        }
        return length->type == STYLE_MATH_TYPE_NUMBER
            ? style_math_number_node(parser, first)
            : style_math_length_node(parser, first, second);
    }
    return style_math_add_node(parser, (StyleMathNode) {
        .a = number->a, .op = STYLE_MATH_NODE_SCALE,
        .type = length->type, .left = (uint8_t) value
    });
}

static int style_math_divide(StyleMathParser *parser, int value,
                             int divisor)
{
    if (value < 0 || divisor < 0) return -1;
    StyleMathNode *operand = &parser->nodes[value];
    StyleMathNode *number = &parser->nodes[divisor];
    if (number->type != STYLE_MATH_TYPE_NUMBER || !number->folded
        || number->a == 0) {
        parser->failed = true;
        return -1;
    }
    int64_t first = 0, second = 0;
    if (operand->folded) {
        if (!style_math_divided(operand->a, number->a, &first)
            || !style_math_divided(operand->b, number->a, &second)) {
            parser->failed = true;
            return -1;
        }
        return operand->type == STYLE_MATH_TYPE_NUMBER
            ? style_math_number_node(parser, first)
            : style_math_length_node(parser, first, second);
    }
    return style_math_add_node(parser, (StyleMathNode) {
        .a = number->a, .op = STYLE_MATH_NODE_DIVIDE,
        .type = operand->type, .left = (uint8_t) value
    });
}

static int style_math_parse_sum(StyleMathParser *parser);

bool style_math_identifier_equal(const char *text, size_t length,
                                        const char *wanted)
{
    return strlen(wanted) == length
        && strncasecmp(text, wanted, length) == 0;
}

/* Some computed properties accept a unitless math result rather than a
   length. Tailwind's typography variables commonly expose line-height as
   `calc(2.5 / 2.25)`. The ordinary length compiler deliberately rejects a
   non-zero scalar root, so retain this small, allocation-free scalar seam
   instead of coercing a ratio into one device pixel. */
bool style_math_resolve_number_thousandths(
    const Stylesheet *sheet, const char *text, size_t length,
    int *thousandths)
{
    if (text == NULL || length == 0 || thousandths == NULL
        || length >= STYLE_MATH_SOURCE_CAPACITY) return false;
    StyleMathParser parser = {
        .sheet = sheet, .text = text, .length = length,
        .em_basis = STYLE_DEFAULT_FONT_PX,
        .rem_basis = STYLE_DEFAULT_FONT_PX,
        .ch_basis = STYLE_DEFAULT_FONT_PX / 2
    };
    int root = style_math_parse_sum(&parser);
    style_math_skip_space(&parser);
    if (parser.failed || root < 0 || parser.at != parser.length) return false;
    const StyleMathNode *node = &parser.nodes[root];
    if (node->type != STYLE_MATH_TYPE_NUMBER || !node->folded
        || node->a <= 0 || node->a > 10 * STYLE_MATH_Q16) return false;
    int64_t scaled = node->a * 1000;
    int result = (int) ((scaled + STYLE_MATH_Q16 / 2) / STYLE_MATH_Q16);
    if (result <= 0 || result > 10000) return false;
    *thousandths = result;
    return true;
}

static int style_math_parse_function(StyleMathParser *parser,
                                     const char *name, size_t name_length)
{
    if (++parser->nesting > STYLE_MATH_NESTING_LIMIT) {
        parser->failed = true;
        return -1;
    }
    if (style_math_identifier_equal(name, name_length, "env")) {
        style_math_skip_space(parser);
        size_t environment_start = parser->at;
        while (parser->at < parser->length
               && (isalnum((unsigned char) parser->text[parser->at])
                   || parser->text[parser->at] == '-')) {
            parser->at++;
        }
        size_t environment_length = parser->at - environment_start;
        bool safe_area =
            style_math_identifier_equal(
                parser->text + environment_start, environment_length,
                "safe-area-inset-top")
            || style_math_identifier_equal(
                parser->text + environment_start, environment_length,
                "safe-area-inset-right")
            || style_math_identifier_equal(
                parser->text + environment_start, environment_length,
                "safe-area-inset-bottom")
            || style_math_identifier_equal(
                parser->text + environment_start, environment_length,
                "safe-area-inset-left");
        style_math_skip_space(parser);
        int fallback = -1;
        if (parser->at < parser->length && parser->text[parser->at] == ',') {
            parser->at++;
            fallback = style_math_parse_sum(parser);
            style_math_skip_space(parser);
        }
        if (environment_length == 0 || parser->at >= parser->length
            || parser->text[parser->at] != ')'
            || (!safe_area && fallback < 0)) {
            parser->failed = true;
            parser->nesting--;
            return -1;
        }
        parser->at++;
        parser->nesting--;
        /* The PSP panel has no display cutout. Preserve the standard
           fallback behavior for unknown environment variables, while the
           four safe-area insets resolve exactly to zero. */
        return safe_area ? style_math_length_node(parser, 0, 0) : fallback;
    }
    if (style_math_identifier_equal(name, name_length, "calc")) {
        int value = style_math_parse_sum(parser);
        style_math_skip_space(parser);
        if (parser->at >= parser->length || parser->text[parser->at] != ')') {
            parser->failed = true;
            parser->nesting--;
            return -1;
        }
        parser->at++;
        parser->nesting--;
        return value;
    }
    bool minimum = style_math_identifier_equal(name, name_length, "min");
    bool maximum = style_math_identifier_equal(name, name_length, "max");
    bool clamp = style_math_identifier_equal(name, name_length, "clamp");
    if (!minimum && !maximum && !clamp) {
        parser->failed = true;
        parser->nesting--;
        return -1;
    }
    int values[STYLE_MATH_ARGUMENT_LIMIT];
    size_t count = 0;
    for (;;) {
        if (count >= STYLE_MATH_ARGUMENT_LIMIT) {
            parser->failed = true;
            break;
        }
        values[count++] = style_math_parse_sum(parser);
        style_math_skip_space(parser);
        if (parser->at >= parser->length) {
            parser->failed = true;
            break;
        }
        if (parser->text[parser->at] == ')') {
            parser->at++;
            break;
        }
        if (parser->text[parser->at] != ',') {
            parser->failed = true;
            break;
        }
        parser->at++;
    }
    parser->nesting--;
    if (parser->failed || count == 0 || (clamp && count != 3)) return -1;
    if (clamp) {
        StyleMathNode *low = &parser->nodes[values[0]];
        StyleMathNode *preferred = &parser->nodes[values[1]];
        StyleMathNode *high = &parser->nodes[values[2]];
        if (low->type != preferred->type || low->type != high->type) {
            parser->failed = true;
            return -1;
        }
        if (low->folded && preferred->folded && high->folded
            && (low->type == STYLE_MATH_TYPE_NUMBER
                || (low->b == preferred->b && low->b == high->b))) {
            int64_t value = preferred->a > high->a
                            ? high->a : preferred->a;
            if (value < low->a) value = low->a;
            return low->type == STYLE_MATH_TYPE_NUMBER
                ? style_math_number_node(parser, value)
                : style_math_length_node(parser, value, low->b);
        }
        return style_math_add_node(parser, (StyleMathNode) {
            .op = STYLE_MATH_NODE_CLAMP, .type = low->type,
            .left = (uint8_t) values[0], .right = (uint8_t) values[1],
            .third = (uint8_t) values[2]
        });
    }
    int result = values[0];
    for (size_t i = 1; i < count; i++) {
        result = style_math_binary(
            parser, minimum ? STYLE_MATH_NODE_MINIMUM
                            : STYLE_MATH_NODE_MAXIMUM,
            result, values[i]);
    }
    return result;
}

static int style_math_parse_primary(StyleMathParser *parser)
{
    style_math_skip_space(parser);
    if (parser->at >= parser->length) {
        parser->failed = true;
        return -1;
    }
    if (parser->text[parser->at] == '(') {
        parser->at++;
        if (++parser->nesting > STYLE_MATH_NESTING_LIMIT) {
            parser->failed = true;
            return -1;
        }
        int value = style_math_parse_sum(parser);
        style_math_skip_space(parser);
        if (parser->at >= parser->length || parser->text[parser->at] != ')') {
            parser->failed = true;
            parser->nesting--;
            return -1;
        }
        parser->at++;
        parser->nesting--;
        return value;
    }
    if (isalpha((unsigned char) parser->text[parser->at])) {
        size_t start = parser->at++;
        while (parser->at < parser->length
               && (isalnum((unsigned char) parser->text[parser->at])
                   || parser->text[parser->at] == '-')) parser->at++;
        size_t name_length = parser->at - start;
        style_math_skip_space(parser);
        if (parser->at >= parser->length || parser->text[parser->at] != '(') {
            parser->failed = true;
            return -1;
        }
        parser->at++;
        return style_math_parse_function(parser, parser->text + start,
                                         name_length);
    }
    char *end = NULL;
    double number = strtod(parser->text + parser->at, &end);
    if (end == parser->text + parser->at || !isfinite(number)) {
        parser->failed = true;
        return -1;
    }
    parser->at = (size_t) (end - parser->text);
    size_t unit_start = parser->at;
    if (parser->at < parser->length && parser->text[parser->at] == '%') {
        parser->at++;
    } else {
        while (parser->at < parser->length
               && isalpha((unsigned char) parser->text[parser->at])) {
            parser->at++;
        }
    }
    size_t unit_length = parser->at - unit_start;
    int64_t first = 0, second = 0;
    if (unit_length == 0) {
        if (!style_math_q16(number, &first)) parser->failed = true;
        return parser->failed ? -1 : style_math_number_node(parser, first);
    }
    const char *unit = parser->text + unit_start;
    if (unit_length == 1 && unit[0] == '%') {
        if (!style_math_q16(number, &second)) parser->failed = true;
        return parser->failed ? -1 : style_math_length_node(parser, 0, second);
    }
    double pixels = number;
    if (style_math_identifier_equal(unit, unit_length, "px")) {
        /* already CSS pixels */
    } else if (style_math_identifier_equal(unit, unit_length, "em")) {
        pixels *= parser->em_basis;
    } else if (style_math_identifier_equal(unit, unit_length, "rem")) {
        pixels *= parser->rem_basis;
    } else if (style_math_identifier_equal(unit, unit_length, "ch")) {
        pixels *= parser->ch_basis;
        if (parser->sheet != NULL
            && parser->sheet->resolve_scratch != NULL
            && !parser->sheet->resolve_scratch->font_ch_basis_active) {
            parser->sheet->resolve_scratch->font_ch_pending = true;
        }
    } else if (style_math_identifier_equal(unit, unit_length, "pc")) {
        pixels *= 16.0;
    } else if (style_math_identifier_equal(unit, unit_length, "pt")) {
        pixels *= 4.0 / 3.0;
    } else if (style_math_identifier_equal(unit, unit_length, "in")) {
        pixels *= 96.0;
    } else if (style_math_identifier_equal(unit, unit_length, "cm")) {
        pixels *= 96.0 / 2.54;
    } else if (style_math_identifier_equal(unit, unit_length, "mm")) {
        pixels *= 96.0 / 25.4;
    } else if (style_math_identifier_equal(unit, unit_length, "q")) {
        pixels *= 96.0 / 101.6;
    } else if (parser->sheet != NULL
               && (style_math_identifier_equal(unit, unit_length, "vw")
                   || style_math_identifier_equal(unit, unit_length, "dvw")
                   || style_math_identifier_equal(unit, unit_length, "svw")
                   || style_math_identifier_equal(unit, unit_length, "lvw")
                   || style_math_identifier_equal(unit, unit_length, "vi")
                   || style_math_identifier_equal(unit, unit_length, "dvi")
                   || style_math_identifier_equal(unit, unit_length, "svi")
                   || style_math_identifier_equal(unit, unit_length, "lvi"))) {
        pixels *= parser->sheet->viewport_width / 100.0;
    } else if (parser->sheet != NULL
               && (style_math_identifier_equal(unit, unit_length, "vh")
                   || style_math_identifier_equal(unit, unit_length, "vb")
                   || style_math_identifier_equal(unit, unit_length, "dvh")
                   || style_math_identifier_equal(unit, unit_length, "dvb")
                   || style_math_identifier_equal(unit, unit_length, "svh")
                   || style_math_identifier_equal(unit, unit_length, "svb")
                   || style_math_identifier_equal(unit, unit_length, "lvh")
                   || style_math_identifier_equal(unit, unit_length, "lvb"))) {
        pixels *= parser->sheet->viewport_height / 100.0;
    } else if (parser->sheet != NULL
               && (style_math_identifier_equal(unit, unit_length, "vmin")
                   || style_math_identifier_equal(unit, unit_length, "vmax")
                   || style_math_identifier_equal(unit, unit_length, "dvmin")
                   || style_math_identifier_equal(unit, unit_length, "dvmax")
                   || style_math_identifier_equal(unit, unit_length, "svmin")
                   || style_math_identifier_equal(unit, unit_length, "svmax")
                   || style_math_identifier_equal(unit, unit_length, "lvmin")
                   || style_math_identifier_equal(unit, unit_length,
                                                  "lvmax"))) {
        int basis = parser->sheet->viewport_width;
        bool minimum = style_math_identifier_equal(unit, unit_length, "vmin")
            || style_math_identifier_equal(unit, unit_length, "dvmin")
            || style_math_identifier_equal(unit, unit_length, "svmin")
            || style_math_identifier_equal(unit, unit_length, "lvmin");
        if (minimum) {
            if (parser->sheet->viewport_height < basis)
                basis = parser->sheet->viewport_height;
        } else if (parser->sheet->viewport_height > basis) {
            basis = parser->sheet->viewport_height;
        }
        pixels *= basis / 100.0;
    } else if (parser->sheet != NULL
               && (style_math_identifier_equal(unit, unit_length, "cqw")
                   || style_math_identifier_equal(unit, unit_length, "cqi")
                   || style_math_identifier_equal(unit, unit_length, "cqh")
                   || style_math_identifier_equal(unit, unit_length, "cqb")
                   || style_math_identifier_equal(unit, unit_length, "cqmin")
                   || style_math_identifier_equal(unit, unit_length,
                                                  "cqmax"))) {
        const StyleResolveScratch *scratch =
            parser->sheet->resolve_scratch;
        int inline_basis = scratch != NULL && scratch->container_basis_active
            ? scratch->container_inline_basis
            : parser->sheet->viewport_width;
        int block_basis = scratch != NULL && scratch->container_basis_active
            ? scratch->container_block_basis
            : parser->sheet->viewport_height;
        int basis = inline_basis;
        if (style_math_identifier_equal(unit, unit_length, "cqh")
            || style_math_identifier_equal(unit, unit_length, "cqb")) {
            basis = block_basis;
        } else if (style_math_identifier_equal(
                       unit, unit_length, "cqmin")) {
            basis = inline_basis < block_basis
                ? inline_basis : block_basis;
        } else if (style_math_identifier_equal(
                       unit, unit_length, "cqmax")) {
            basis = inline_basis > block_basis
                ? inline_basis : block_basis;
        }
        pixels *= basis / 100.0;
    } else {
        parser->failed = true;
        return -1;
    }
    if (!style_math_px(pixels, &first)) parser->failed = true;
    return parser->failed ? -1 : style_math_length_node(parser, first, 0);
}

static int style_math_parse_unary(StyleMathParser *parser)
{
    style_math_skip_space(parser);
    int sign = 1;
    while (parser->at < parser->length
           && (parser->text[parser->at] == '+'
               || parser->text[parser->at] == '-')) {
        if (parser->text[parser->at++] == '-') sign = -sign;
        style_math_skip_space(parser);
    }
    int value = style_math_parse_primary(parser);
    if (sign > 0 || value < 0) return value;
    int minus_one = style_math_number_node(parser, -STYLE_MATH_Q16);
    return style_math_scale(parser, value, minus_one);
}

static int style_math_parse_product(StyleMathParser *parser)
{
    int result = style_math_parse_unary(parser);
    for (;;) {
        style_math_skip_space(parser);
        if (parser->at >= parser->length
            || (parser->text[parser->at] != '*'
                && parser->text[parser->at] != '/')) return result;
        char operation = parser->text[parser->at++];
        int right = style_math_parse_unary(parser);
        if (result < 0 || right < 0) return -1;
        StyleMathNode *left_node = &parser->nodes[result];
        StyleMathNode *right_node = &parser->nodes[right];
        if (operation == '*') {
            if (left_node->type == STYLE_MATH_TYPE_NUMBER) {
                result = style_math_scale(parser, right, result);
            } else if (right_node->type == STYLE_MATH_TYPE_NUMBER) {
                result = style_math_scale(parser, result, right);
            } else {
                parser->failed = true;
                return -1;
            }
        } else {
            if (right_node->type != STYLE_MATH_TYPE_NUMBER
                || !right_node->folded || right_node->a == 0) {
                parser->failed = true;
                return -1;
            }
            result = style_math_divide(parser, result, right);
        }
    }
}

static int style_math_parse_sum(StyleMathParser *parser)
{
    int result = style_math_parse_product(parser);
    for (;;) {
        style_math_skip_space(parser);
        if (parser->at >= parser->length
            || (parser->text[parser->at] != '+'
                && parser->text[parser->at] != '-')) return result;
        char operation = parser->text[parser->at++];
        int right = style_math_parse_product(parser);
        result = style_math_binary(
            parser, operation == '+' ? STYLE_MATH_NODE_ADD
                                     : STYLE_MATH_NODE_SUBTRACT,
            result, right);
    }
}

static bool style_math_emit_node(const StyleMathParser *parser, int index,
                                 StyleMathCandidate *candidate)
{
    if (index < 0 || (size_t) index >= parser->node_count
        || candidate->count >= STYLE_MATH_NODE_LIMIT) return false;
    const StyleMathNode *node = &parser->nodes[index];
    if (node->folded) {
        if (node->type != STYLE_MATH_TYPE_LENGTH
            || node->a > INT32_MAX || node->a < INT32_MIN
            || node->b > INT32_MAX || node->b < INT32_MIN) return false;
        candidate->instructions[candidate->count++] = (StyleMathInstruction) {
            .a = (int32_t) node->a, .b = (int32_t) node->b,
            .op = STYLE_MATH_PUSH_AFFINE
        };
        if (node->b != 0) candidate->has_percent = true;
        return true;
    }
    if (!style_math_emit_node(parser, node->left, candidate)) return false;
    if (node->op != STYLE_MATH_NODE_SCALE
        && node->op != STYLE_MATH_NODE_DIVIDE
        && !style_math_emit_node(parser, node->right, candidate)) return false;
    if (node->op == STYLE_MATH_NODE_CLAMP
        && !style_math_emit_node(parser, node->third, candidate)) return false;
    if (candidate->count >= STYLE_MATH_NODE_LIMIT) return false;
    uint8_t operation = STYLE_MATH_ADD;
    if (node->op == STYLE_MATH_NODE_SUBTRACT) {
        operation = STYLE_MATH_SUBTRACT;
    } else if (node->op == STYLE_MATH_NODE_SCALE) {
        operation = STYLE_MATH_SCALE;
    } else if (node->op == STYLE_MATH_NODE_DIVIDE) {
        operation = STYLE_MATH_DIVIDE;
    } else if (node->op == STYLE_MATH_NODE_MINIMUM) {
        operation = STYLE_MATH_MINIMUM;
    } else if (node->op == STYLE_MATH_NODE_MAXIMUM) {
        operation = STYLE_MATH_MAXIMUM;
    } else if (node->op == STYLE_MATH_NODE_CLAMP) {
        operation = STYLE_MATH_CLAMP;
    }
    candidate->instructions[candidate->count++] = (StyleMathInstruction) {
        .a = (node->op == STYLE_MATH_NODE_SCALE
              || node->op == STYLE_MATH_NODE_DIVIDE)
             ? (int32_t) node->a : 0,
        .op = operation
    };
    return true;
}

bool style_math_candidate(const Stylesheet *sheet, const char *text,
                                 size_t length, StyleMathParser *parser,
                                 StyleMathCandidate *candidate,
                                 int *root_output, int em_basis,
                                 int rem_basis, int ch_basis)
{
    if (parser == NULL || candidate == NULL || root_output == NULL
        || text == NULL || length == 0
        || length >= STYLE_MATH_SOURCE_CAPACITY) return false;
    *parser = (StyleMathParser) {
        .sheet = sheet, .text = text, .length = length,
        .em_basis = em_basis > 0 ? em_basis : 16,
        .rem_basis = rem_basis > 0 ? rem_basis : 16,
        .ch_basis = ch_basis > 0 ? ch_basis
                  : (em_basis > 0 ? (em_basis + 1) / 2 : 8)
    };
    *candidate = (StyleMathCandidate) {0};
    int root = style_math_parse_sum(parser);
    style_math_skip_space(parser);
    if (parser->failed || root < 0 || parser->at != parser->length) return false;
    StyleMathNode *node = &parser->nodes[root];
    if (node->type == STYLE_MATH_TYPE_NUMBER) {
        if (!node->folded || node->a != 0) return false;
        root = style_math_length_node(parser, 0, 0);
        if (root < 0) return false;
    }
    if (!style_math_emit_node(parser, root, candidate)
        || candidate->count == 0) return false;
    size_t depth = 0, maximum_depth = 0;
    for (size_t i = 0; i < candidate->count; i++) {
        uint8_t operation = candidate->instructions[i].op;
        if (operation == STYLE_MATH_PUSH_AFFINE) {
            if (++depth > STYLE_MATH_STACK_LIMIT) return false;
            if (depth > maximum_depth) maximum_depth = depth;
        } else if (operation == STYLE_MATH_SCALE
                   || operation == STYLE_MATH_DIVIDE) {
            if (depth < 1) return false;
        } else if (operation == STYLE_MATH_CLAMP) {
            if (depth < 3) return false;
            depth -= 2;
        } else {
            if (depth < 2) return false;
            depth--;
        }
    }
    if (depth != 1 || maximum_depth == 0) return false;
    candidate->stack_depth = (uint8_t) maximum_depth;
    *root_output = root;
    return true;
}

static uint32_t style_math_hash(const StyleMathInstruction *instructions,
                                size_t count)
{
    uint32_t hash = UINT32_C(2166136261);
    const unsigned char *bytes = (const unsigned char *) instructions;
    size_t length = count * sizeof(*instructions);
    for (size_t i = 0; i < length; i++) {
        hash ^= bytes[i];
        hash *= UINT32_C(16777619);
    }
    return hash;
}

static bool style_math_intern(Stylesheet *sheet,
                              const StyleMathCandidate *candidate,
                              StyleLength *value)
{
    if (sheet == NULL || candidate == NULL || value == NULL
        || candidate->count == 0 || candidate->count > UINT8_MAX) return false;
    uint32_t hash = style_math_hash(candidate->instructions,
                                    candidate->count);
    for (size_t i = 0; i < sheet->math_program_count; i++) {
        const StyleMathProgram *program = &sheet->math_programs[i];
        if (program->hash == hash && program->count == candidate->count
            && memcmp(sheet->math_instructions + program->first,
                      candidate->instructions,
                      candidate->count * sizeof(*candidate->instructions))
               == 0) {
            *value = STYLE_LENGTH_PROGRAM_TAG + (StyleLength) i;
            return true;
        }
    }
    if (sheet->math_program_count >= STYLE_MATH_PROGRAM_LIMIT
        || candidate->count > STYLE_MATH_INSTRUCTION_LIMIT
            - sheet->math_instruction_count) return false;
    size_t required_instructions = sheet->math_instruction_count
                                   + candidate->count;
    if (required_instructions > sheet->math_instruction_capacity) {
        size_t capacity = sheet->math_instruction_capacity == 0
                          ? 32 : sheet->math_instruction_capacity;
        while (capacity < required_instructions) capacity *= 2;
        if (capacity > STYLE_MATH_INSTRUCTION_LIMIT) {
            capacity = STYLE_MATH_INSTRUCTION_LIMIT;
        }
        StyleMathInstruction *instructions = budget_realloc(
            sheet->budget, sheet->math_instructions,
            capacity * sizeof(*instructions));
        if (instructions == NULL) return false;
        sheet->math_instructions = instructions;
        sheet->math_instruction_capacity = capacity;
    }
    if (sheet->math_program_count == sheet->math_program_capacity) {
        size_t capacity = sheet->math_program_capacity == 0
                          ? 16 : sheet->math_program_capacity * 2;
        if (capacity > STYLE_MATH_PROGRAM_LIMIT) {
            capacity = STYLE_MATH_PROGRAM_LIMIT;
        }
        StyleMathProgram *programs = budget_realloc(
            sheet->budget, sheet->math_programs,
            capacity * sizeof(*programs));
        if (programs == NULL) return false;
        sheet->math_programs = programs;
        sheet->math_program_capacity = capacity;
    }
    size_t first = sheet->math_instruction_count;
    memcpy(sheet->math_instructions + first, candidate->instructions,
           candidate->count * sizeof(*candidate->instructions));
    sheet->math_instruction_count += candidate->count;
    size_t index = sheet->math_program_count++;
    sheet->math_programs[index] = (StyleMathProgram) {
        .first = (uint16_t) first,
        .count = (uint8_t) candidate->count,
        .stack_depth = candidate->stack_depth,
        .hash = hash
    };
    sheet->math_pool_bytes =
        sheet->math_instruction_capacity * sizeof(*sheet->math_instructions)
        + sheet->math_program_capacity * sizeof(*sheet->math_programs);
    *value = STYLE_LENGTH_PROGRAM_TAG + (StyleLength) index;
    return true;
}

bool parse_style_length(Stylesheet *sheet, const char *text,
                               size_t length, StyleLength *value,
                               bool *has_percent)
{
    if (value == NULL) return false;
    char resolved[STYLE_MATH_SOURCE_CAPACITY];
    if (!style_resolve_value(sheet, text, length, resolved, sizeof(resolved), 0)) {
        return false;
    }
    const char *start = resolved;
    size_t resolved_length = strlen(resolved);
    trim(&start, &resolved_length);
    StyleMathParser parser;
    StyleMathCandidate candidate;
    int root = -1;
    const StyleResolveScratch *scratch =
        sheet == NULL ? NULL : sheet->resolve_scratch;
    int em_basis = scratch != NULL && scratch->font_resolution_active
        ? scratch->font_resolution_inherited : STYLE_DEFAULT_FONT_PX;
    int rem_basis = scratch != NULL && scratch->font_resolution_active
        ? scratch->font_resolution_root : STYLE_DEFAULT_FONT_PX;
    int ch_basis = scratch != NULL && scratch->font_ch_basis_active
        ? scratch->font_ch_basis : (em_basis + 1) / 2;
    if (!style_math_candidate(sheet, start, resolved_length, &parser,
                              &candidate, &root, em_basis, rem_basis,
                              ch_basis)) {
        return false;
    }
    const StyleMathNode *node = &parser.nodes[root];
    if (has_percent != NULL) *has_percent = candidate.has_percent;
    if (node->folded && node->type == STYLE_MATH_TYPE_LENGTH) {
        if (node->b == 0) {
            int64_t pixels = node->a < 0 ? (node->a - 128) / 256
                                         : (node->a + 128) / 256;
            if (pixels > STYLE_LENGTH_DIRECT_LIMIT
                || pixels < -STYLE_LENGTH_DIRECT_LIMIT) return false;
            *value = (StyleLength) pixels;
            return true;
        }
        if (node->a == 0
            && node->b >= -STYLE_LENGTH_PERCENT_BIAS
            && node->b < STYLE_LENGTH_PERCENT_BIAS) {
            *value = STYLE_LENGTH_PERCENT_TAG
                     + (StyleLength) (node->b + STYLE_LENGTH_PERCENT_BIAS);
            return true;
        }
    }
    return style_math_intern(sheet, &candidate, value);
}

/* Existing ComputedStyle percentage flags are useful both for percentage
   definiteness and for retaining the compact integer representation used by
   older callers.  Preserve that representation for an exact integral
   percentage; mixed expressions remain tagged program references. */
bool parse_dimension_length(Stylesheet *sheet, const char *text,
                                   size_t length, StyleLength *value,
                                   bool *has_percent)
{
    bool percent = false;
    StyleLength parsed = 0;
    if (!parse_style_length(sheet, text, length, &parsed, &percent)) {
        return false;
    }
    if (percent && parsed >= STYLE_LENGTH_PERCENT_TAG
        && parsed <= STYLE_LENGTH_PERCENT_END) {
        int64_t q16 = (int64_t) parsed - STYLE_LENGTH_PERCENT_TAG
                      - STYLE_LENGTH_PERCENT_BIAS;
        if (q16 % STYLE_MATH_Q16 == 0) {
            parsed = (StyleLength) (q16 / STYLE_MATH_Q16);
        }
    }
    *value = parsed;
    if (has_percent != NULL) *has_percent = percent;
    return true;
}

void style_math_restore(Stylesheet *sheet, size_t program_count,
                               size_t instruction_count)
{
    if (sheet == NULL) return;
    sheet->math_program_count = program_count;
    sheet->math_instruction_count = instruction_count;
}

bool style_keyword(const Stylesheet *sheet, const char *text,
                          size_t length, const char *keyword)
{
    char resolved[64];
    return style_resolve_value(sheet, text, length, resolved, sizeof(resolved), 0)
           && strcmp(resolved, keyword) == 0;
}

static int64_t style_math_clamp_used(int64_t value)
{
    const int64_t limit = (int64_t) STYLE_LENGTH_DIRECT_LIMIT * 256;
    if (value > limit) return limit;
    if (value < -limit) return -limit;
    return value;
}

static int style_math_round_used(int64_t value)
{
    value = style_math_clamp_used(value);
    return (int) (value < 0 ? (value - 128) / 256
                            : (value + 128) / 256);
}

bool style_math_resolve_instructions(
    const StyleMathInstruction *instructions, size_t count,
    uint8_t stack_depth, int reference, int *pixels)
{
    if (instructions == NULL || pixels == NULL || count == 0
        || stack_depth == 0 || stack_depth > STYLE_MATH_STACK_LIMIT) {
        return false;
    }
    int64_t basis = reference;
    if (basis > STYLE_LENGTH_DIRECT_LIMIT) basis = STYLE_LENGTH_DIRECT_LIMIT;
    if (basis < -STYLE_LENGTH_DIRECT_LIMIT) {
        basis = -STYLE_LENGTH_DIRECT_LIMIT;
    }
    int64_t stack[STYLE_MATH_STACK_LIMIT];
    size_t depth = 0;
    for (size_t i = 0; i < count; i++) {
        const StyleMathInstruction *instruction = &instructions[i];
        if (instruction->op == STYLE_MATH_PUSH_AFFINE) {
            if (depth >= STYLE_MATH_STACK_LIMIT) return false;
            stack[depth++] = style_math_clamp_used(
                (int64_t) instruction->a
                + basis * instruction->b * 256
                  / (100 * STYLE_MATH_Q16));
            continue;
        }
        if (instruction->op == STYLE_MATH_SCALE) {
            if (depth < 1) return false;
            int64_t scaled = 0;
            if (!style_math_scaled(stack[depth - 1], instruction->a,
                                   &scaled)) return false;
            stack[depth - 1] = style_math_clamp_used(scaled);
            continue;
        }
        if (instruction->op == STYLE_MATH_DIVIDE) {
            if (depth < 1) return false;
            int64_t divided = 0;
            if (!style_math_divided(stack[depth - 1], instruction->a,
                                    &divided)) return false;
            stack[depth - 1] = style_math_clamp_used(divided);
            continue;
        }
        if (instruction->op == STYLE_MATH_CLAMP) {
            if (depth < 3) return false;
            int64_t high = stack[--depth];
            int64_t preferred = stack[--depth];
            int64_t low = stack[depth - 1];
            int64_t result = preferred > high ? high : preferred;
            stack[depth - 1] = result < low ? low : result;
            continue;
        }
        if (depth < 2) return false;
        int64_t right = stack[--depth];
        int64_t left = stack[depth - 1];
        if (instruction->op == STYLE_MATH_ADD) {
            stack[depth - 1] = style_math_clamp_used(left + right);
        } else if (instruction->op == STYLE_MATH_SUBTRACT) {
            stack[depth - 1] = style_math_clamp_used(left - right);
        } else if (instruction->op == STYLE_MATH_MINIMUM) {
            stack[depth - 1] = left < right ? left : right;
        } else if (instruction->op == STYLE_MATH_MAXIMUM) {
            stack[depth - 1] = left > right ? left : right;
        } else {
            return false;
        }
    }
    if (depth != 1) return false;
    *pixels = style_math_round_used(stack[0]);
    return true;
}

bool style_length_resolve(const Stylesheet *sheet, StyleLength value,
                          int reference, int *pixels)
{
    if (pixels == NULL || value == STYLE_LENGTH_NONE) return false;
    int64_t basis = reference;
    if (basis > STYLE_LENGTH_DIRECT_LIMIT) basis = STYLE_LENGTH_DIRECT_LIMIT;
    if (basis < -STYLE_LENGTH_DIRECT_LIMIT) {
        basis = -STYLE_LENGTH_DIRECT_LIMIT;
    }
    if (value < STYLE_LENGTH_PERCENT_TAG) {
        *pixels = value;
        return true;
    }
    if (value <= STYLE_LENGTH_PERCENT_END) {
        int64_t percent_q16 = (int64_t) value
                              - STYLE_LENGTH_PERCENT_TAG
                              - STYLE_LENGTH_PERCENT_BIAS;
        int64_t used_q8 = basis * percent_q16 * 256
                          / (100 * STYLE_MATH_Q16);
        *pixels = style_math_round_used(used_q8);
        return true;
    }
    if (value < STYLE_LENGTH_PROGRAM_TAG || value > STYLE_LENGTH_PROGRAM_END
        || sheet == NULL) return false;
    size_t index = (size_t) (value - STYLE_LENGTH_PROGRAM_TAG);
    if (index >= sheet->math_program_count) return false;
    const StyleMathProgram *program = &sheet->math_programs[index];
    if ((size_t) program->first + program->count
        > sheet->math_instruction_count || program->stack_depth == 0
        || program->stack_depth > STYLE_MATH_STACK_LIMIT) return false;
    return style_math_resolve_instructions(
        sheet->math_instructions + program->first, program->count,
        program->stack_depth, reference, pixels);
}

static int parse_length_depth(const Stylesheet *sheet, const char *text,
                              size_t length, int fallback, bool *percent,
                              unsigned depth);

static bool parse_calc_scalar(const char *text, size_t length, double *result)
{
    if (text == NULL || result == NULL) return false;
    trim(&text, &length);
    if (length == 0 || length >= 64) return false;
    char value[64];
    memcpy(value, text, length);
    value[length] = '\0';
    char *end = NULL;
    double parsed = strtod(value, &end);
    if (end == value) return false;
    while (isspace((unsigned char) *end)) end++;
    if (*end != '\0' || !isfinite(parsed)) return false;
    *result = parsed;
    return true;
}

/* CSS calc() permits scalar products such as `16px * 2` and division by a
   number.  Keep this evaluator deliberately typed and bounded: one length
   factor may be combined with unitless scalars, while multiplying lengths or
   dividing by a length is rejected.  Addition remains in parse_length_depth
   so multiplication has the required precedence without a general-purpose
   expression tree. */
static int parse_calc_product(const Stylesheet *sheet, const char *text,
                              size_t length, int fallback, bool *percent,
                              unsigned depth)
{
    double result = 0.0;
    bool started = false;
    bool have_length = false;
    bool result_percent = false;
    char operation = '*';
    size_t start = 0;
    int nesting = 0;
    size_t factors = 0;
    for (size_t i = 0; i <= length; i++) {
        if (i < length && text[i] == '(') nesting++;
        else if (i < length && text[i] == ')' && nesting > 0) nesting--;
        bool split = i == length
                     || (nesting == 0 && (text[i] == '*' || text[i] == '/'));
        if (!split) continue;
        if (++factors > 8) return fallback;
        const char *factor = text + start;
        size_t factor_length = i - start;
        trim(&factor, &factor_length);
        if (factor_length == 0) return fallback;
        double value = 0.0;
        bool factor_is_length = !parse_calc_scalar(
            factor, factor_length, &value);
        bool factor_percent = false;
        if (factor_is_length) {
            int parsed = parse_length_depth(
                sheet, factor, factor_length, INT_MIN, &factor_percent,
                depth + 1);
            if (parsed == INT_MIN || operation == '/' || have_length) {
                return fallback;
            }
            value = parsed;
            have_length = true;
            result_percent = factor_percent;
        }
        if (!started) {
            result = value;
            started = true;
        } else if (operation == '*') {
            result *= value;
        } else {
            if (value == 0.0) return fallback;
            result /= value;
        }
        if (!isfinite(result) || result > 32767.0 || result < -32767.0) {
            return fallback;
        }
        if (i < length) operation = text[i];
        start = i + 1;
    }
    if (!started) return fallback;
    if (percent != NULL) *percent = result_percent;
    return (int) (result < 0.0 ? result - 0.5 : result + 0.5);
}

static int parse_length_depth(const Stylesheet *sheet, const char *text,
                              size_t length, int fallback, bool *percent,
                              unsigned depth)
{
    if (percent != NULL) *percent = false;
    char value[128];
    if (!style_resolve_value(sheet, text, length, value, sizeof(value), 0)) return fallback;
    size_t value_length = strlen(value);
    if (depth < 4 && value_length > 5 && value[value_length - 1] == ')') {
        enum { LENGTH_NONE, LENGTH_MIN, LENGTH_MAX, LENGTH_CLAMP,
               LENGTH_CALC } function = LENGTH_NONE;
        size_t prefix = 0;
        if (strncmp(value, "min(", 4) == 0) {
            function = LENGTH_MIN; prefix = 4;
        } else if (strncmp(value, "max(", 4) == 0) {
            function = LENGTH_MAX; prefix = 4;
        } else if (strncmp(value, "clamp(", 6) == 0) {
            function = LENGTH_CLAMP; prefix = 6;
        } else if (strncmp(value, "calc(", 5) == 0) {
            function = LENGTH_CALC; prefix = 5;
        }
        if (function != LENGTH_NONE) {
            const char *inside = value + prefix;
            size_t inside_length = value_length - prefix - 1;
            int values[8] = {0};
            bool percentages[8] = {false};
            size_t count = 0, start = 0;
            int nesting = 0;
            int calc_sign = 1;
            for (size_t i = 0; i <= inside_length && count < 8; i++) {
                if (i < inside_length && inside[i] == '(') nesting++;
                else if (i < inside_length && inside[i] == ')'
                         && nesting > 0) nesting--;
                bool split = i == inside_length
                    || (function != LENGTH_CALC && nesting == 0
                        && inside[i] == ',')
                    || (function == LENGTH_CALC && nesting == 0 && i > start
                        && (inside[i] == '+' || inside[i] == '-'));
                if (!split) continue;
                size_t part_start = start, part_length = i - start;
                const char *part = value + prefix + part_start;
                trim(&part, &part_length);
                if (part_length == 0) return fallback;
                values[count] = function == LENGTH_CALC
                    ? parse_calc_product(sheet, part, part_length, fallback,
                                         &percentages[count], depth + 1)
                    : parse_length_depth(sheet, part, part_length, fallback,
                                         &percentages[count], depth + 1);
                values[count] *= calc_sign;
                count++;
                if (function == LENGTH_CALC && i < inside_length) {
                    calc_sign = inside[i] == '-' ? -1 : 1;
                    start = i + 1;
                } else {
                    start = i + 1;
                }
            }
            if (count == 0) return fallback;
            for (size_t i = 1; i < count; i++) {
                if (percentages[i] != percentages[0]) return fallback;
            }
            int result = values[0];
            if (function == LENGTH_MIN || function == LENGTH_MAX) {
                for (size_t i = 1; i < count; i++) {
                    if ((function == LENGTH_MIN && values[i] < result)
                        || (function == LENGTH_MAX && values[i] > result)) {
                        result = values[i];
                    }
                }
            } else if (function == LENGTH_CLAMP) {
                if (count != 3) return fallback;
                result = values[1] < values[0] ? values[0] : values[1];
                if (result > values[2]) result = values[2];
            } else {
                result = 0;
                for (size_t i = 0; i < count; i++) result += values[i];
            }
            if (percent != NULL) *percent = percentages[0];
            return result;
        }
    }
    char *end = NULL;
    double number = strtod(value, &end);
    if (end == value) return fallback;
    while (isspace((unsigned char) *end)) end++;
    if (percent != NULL) *percent = *end == '%';
    if (strncmp(end, "rem", 3) == 0) {
        const StyleResolveScratch *scratch =
            sheet == NULL ? NULL : sheet->resolve_scratch;
        number *= scratch != NULL && scratch->font_resolution_active
            ? scratch->font_resolution_root : STYLE_DEFAULT_FONT_PX;
    } else if (strncmp(end, "em", 2) == 0) {
        const StyleResolveScratch *scratch =
            sheet == NULL ? NULL : sheet->resolve_scratch;
        number *= scratch != NULL && scratch->font_resolution_active
            ? scratch->font_resolution_inherited : STYLE_DEFAULT_FONT_PX;
    } else if (strncmp(end, "ch", 2) == 0) {
        const StyleResolveScratch *scratch =
            sheet == NULL ? NULL : sheet->resolve_scratch;
        int em_basis = scratch != NULL && scratch->font_resolution_active
            ? scratch->font_resolution_inherited : STYLE_DEFAULT_FONT_PX;
        number *= scratch != NULL && scratch->font_ch_basis_active
            ? scratch->font_ch_basis : (em_basis + 1) / 2;
    }
    else if (strcmp(end, "pt") == 0) number *= 4.0 / 3.0;
    else if (strcmp(end, "pc") == 0) number *= 16.0;
    else if (strcmp(end, "in") == 0) number *= 96.0;
    else if (strcmp(end, "cm") == 0) number *= 96.0 / 2.54;
    else if (strcmp(end, "mm") == 0) number *= 96.0 / 25.4;
    else if (strcmp(end, "q") == 0) number *= 96.0 / 101.6;
    else if (sheet != NULL
             && (strcmp(end, "vw") == 0 || strcmp(end, "vi") == 0
                 || strcmp(end, "dvw") == 0 || strcmp(end, "svw") == 0
                 || strcmp(end, "lvw") == 0
                 || strcmp(end, "dvi") == 0 || strcmp(end, "svi") == 0
                 || strcmp(end, "lvi") == 0)) {
        number *= sheet->viewport_width / 100.0;
    } else if (sheet != NULL
               && (strcmp(end, "vh") == 0 || strcmp(end, "vb") == 0
               || strcmp(end, "dvh") == 0 || strcmp(end, "dvb") == 0
               || strcmp(end, "svh") == 0 || strcmp(end, "svb") == 0
               || strcmp(end, "lvh") == 0 || strcmp(end, "lvb") == 0)) {
        number *= sheet->viewport_height / 100.0;
    } else if (sheet != NULL
               && (strcmp(end, "vmin") == 0 || strcmp(end, "vmax") == 0
                   || strcmp(end, "dvmin") == 0 || strcmp(end, "dvmax") == 0
                   || strcmp(end, "svmin") == 0 || strcmp(end, "svmax") == 0
                   || strcmp(end, "lvmin") == 0 || strcmp(end, "lvmax") == 0)) {
        int basis = sheet->viewport_width;
        bool minimum = strcmp(end, "vmin") == 0
            || strcmp(end, "dvmin") == 0 || strcmp(end, "svmin") == 0
            || strcmp(end, "lvmin") == 0;
        if (minimum) {
            if (sheet->viewport_height < basis) basis = sheet->viewport_height;
        } else if (sheet->viewport_height > basis) {
            basis = sheet->viewport_height;
        }
        number *= basis / 100.0;
    } else if (sheet != NULL
               && (strcmp(end, "cqw") == 0 || strcmp(end, "cqi") == 0
                   || strcmp(end, "cqh") == 0 || strcmp(end, "cqb") == 0
                   || strcmp(end, "cqmin") == 0
                   || strcmp(end, "cqmax") == 0)) {
        const StyleResolveScratch *scratch = sheet->resolve_scratch;
        int inline_basis = scratch != NULL && scratch->container_basis_active
            ? scratch->container_inline_basis : sheet->viewport_width;
        int block_basis = scratch != NULL && scratch->container_basis_active
            ? scratch->container_block_basis : sheet->viewport_height;
        int basis = inline_basis;
        if (strcmp(end, "cqh") == 0 || strcmp(end, "cqb") == 0) {
            basis = block_basis;
        } else if (strcmp(end, "cqmin") == 0) {
            basis = inline_basis < block_basis
                ? inline_basis : block_basis;
        } else if (strcmp(end, "cqmax") == 0) {
            basis = inline_basis > block_basis
                ? inline_basis : block_basis;
        }
        number *= basis / 100.0;
    }
    if (number > 32767.0) number = 32767.0;
    if (number < -32767.0) number = -32767.0;
    return (int) (number < 0.0 ? number - 0.5 : number + 0.5);
}

int style_parse_length(const Stylesheet *sheet, const char *text,
                        size_t length, int fallback, bool *percent)
{
    return parse_length_depth(sheet, text, length, fallback, percent, 0);
}
