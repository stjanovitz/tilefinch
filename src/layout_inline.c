/* Inline formatting: line boxes, floats, text measurement and flow,
   generated-content pseudo elements, and the inline fallback path.
   Split out of layout.c. */

#include "layout_internal.h"
#include "tilefinch/integer_math.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int line_y_fixed(LineState *line)
{
    if (!line->y_fixed_valid
        || layout_fixed_floor(line->y_fixed) != line->y) {
        line->y_fixed = layout_fixed_from_integer(line->y);
        line->y_fixed_valid = true;
    }
    return line->y_fixed;
}

static void line_height_include_fixed(LineState *line, int height_fixed)
{
    if (height_fixed <= 0) return;
    if (height_fixed > line->line_height_fixed) {
        line->line_height_fixed = height_fixed;
    }
    int height = layout_fixed_ceil(height_fixed);
    if (height > line->line_height) line->line_height = height;
}

void line_finish_vertical(LineState *line)
{
    if (line == NULL || !line->y_fixed_valid) return;
    line->y = layout_fixed_ceil(line->y_fixed);
    line->y_fixed = layout_fixed_from_integer(line->y);
}

int layout_fixed_scale_floor(int value, int numerator,
                                    int denominator)
{
    if (denominator <= 0) return 0;
    return tilefinch_mul_div_floor_int(value, numerator, denominator);
}

void line_cursor_set(LineState *line, int x)
{
    line->x = x;
    line->x_fixed = layout_fixed_from_integer(x);
    line->x_fixed_valid = true;
}

static void line_cursor_set_fixed(LineState *line, int x_fixed)
{
    line->x_fixed = x_fixed;
    /* Atomic integer boxes placed after text must not overlap its fractional
       advance. Text-to-text placement continues to use x_fixed exactly. */
    line->x = layout_fixed_ceil(x_fixed);
    line->x_fixed_valid = true;
}

static int line_cursor_fixed(LineState *line)
{
    if (!line->x_fixed_valid) line_cursor_set(line, line->x);
    return line->x_fixed;
}


#define GENERATED_COUNTER_STACK_LIMIT 32
#define GENERATED_COUNTER_NODE_LIMIT 4096

typedef struct {
    const char *name;
    int value;
} GeneratedCounter;

typedef struct {
    const Stylesheet *sheet;
    lxb_dom_node_t *target;
    PseudoElement pseudo;
    const char *name;
    GeneratedCounter stack[GENERATED_COUNTER_STACK_LIMIT];
    size_t count;
    size_t visits;
    int values[GENERATED_COUNTER_STACK_LIMIT];
    size_t value_count;
    bool found;
} GeneratedCounterWalk;

static bool counter_name_equal(const char *left, const char *right)
{
    return left != NULL && right != NULL && strcmp(left, right) == 0;
}

static void generated_counter_apply(
    GeneratedCounterWalk *walk, uint8_t reset_id, uint8_t set_id,
    uint8_t increment_id)
{
    const StyleCounterOperations *reset =
        style_counter_operations(walk->sheet, reset_id);
    if (reset != NULL) {
        for (size_t i = 0; i < reset->count; i++) {
            const StyleCounterOperation *operation = &reset->operations[i];
            if (!counter_name_equal(operation->name, walk->name)
                || walk->count >= GENERATED_COUNTER_STACK_LIMIT) continue;
            walk->stack[walk->count++] = (GeneratedCounter) {
                .name = operation->name,
                .value = operation->value
            };
        }
    }
    const StyleCounterOperations *set =
        style_counter_operations(walk->sheet, set_id);
    if (set != NULL) {
        for (size_t i = 0; i < set->count; i++) {
            const StyleCounterOperation *operation = &set->operations[i];
            if (!counter_name_equal(operation->name, walk->name)) continue;
            for (size_t at = walk->count; at != 0; at--) {
                if (counter_name_equal(
                        walk->stack[at - 1].name, operation->name)) {
                    walk->stack[at - 1].value = operation->value;
                    break;
                }
            }
        }
    }
    const StyleCounterOperations *increment =
        style_counter_operations(walk->sheet, increment_id);
    if (increment != NULL) {
        for (size_t i = 0; i < increment->count; i++) {
            const StyleCounterOperation *operation =
                &increment->operations[i];
            if (!counter_name_equal(operation->name, walk->name)) continue;
            for (size_t at = walk->count; at != 0; at--) {
                if (counter_name_equal(
                        walk->stack[at - 1].name, operation->name)) {
                    int64_t value = (int64_t) walk->stack[at - 1].value
                                    + operation->value;
                    if (value > INT_MAX) value = INT_MAX;
                    if (value < INT_MIN) value = INT_MIN;
                    walk->stack[at - 1].value = (int) value;
                    break;
                }
            }
        }
    }
}

static bool generated_counter_visit(
    GeneratedCounterWalk *walk, lxb_dom_node_t *node,
    const ComputedStyle *parent, unsigned depth)
{
    if (walk->found || node == NULL || depth >= 64
        || ++walk->visits > GENERATED_COUNTER_NODE_LIMIT) return walk->found;
    ComputedStyle style = style_for_node(walk->sheet, node, parent);
    size_t entry_count = walk->count;
    if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
        generated_counter_apply(
            walk, style.counter_reset_id, style.counter_set_id,
            style.counter_increment_id);
        if (node == walk->target) {
            ComputedStyle pseudo = style_for_pseudo(
                walk->sheet, node, walk->pseudo, &style);
            generated_counter_apply(
                walk, pseudo.counter_reset_id, pseudo.counter_set_id,
                pseudo.counter_increment_id);
            walk->value_count = 0;
            for (size_t i = 0; i < walk->count; i++) {
                if (counter_name_equal(walk->stack[i].name, walk->name)) {
                    walk->values[walk->value_count++] =
                        walk->stack[i].value;
                }
            }
            walk->found = true;
            walk->count = entry_count;
            return true;
        }
    }
    for (lxb_dom_node_t *child = node->first_child;
         child != NULL && !walk->found; child = child->next) {
        if (child->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            (void) generated_counter_visit(walk, child, &style, depth + 1);
        }
    }
    walk->count = entry_count;
    return walk->found;
}

static size_t generated_counter_values(
    const Stylesheet *sheet, lxb_dom_node_t *node, PseudoElement pseudo,
    const char *name, int *values, size_t capacity)
{
    if (sheet == NULL || node == NULL || name == NULL || name[0] == '\0'
        || values == NULL || capacity == 0) return 0;
    lxb_dom_node_t *root = node;
    while (root->parent != NULL) root = root->parent;
    GeneratedCounterWalk walk = {
        .sheet = sheet,
        .target = node,
        .pseudo = pseudo,
        .name = name,
        .stack = {{.name = name, .value = 0}},
        .count = 1
    };
    (void) generated_counter_visit(&walk, root, NULL, 0);
    size_t count = walk.found ? walk.value_count : 1;
    if (count > capacity) count = capacity;
    if (walk.found) memcpy(values, walk.values, count * sizeof(*values));
    else values[0] = 0;
    return count;
}

static size_t format_roman(unsigned value, bool upper,
                           char *output, size_t capacity)
{
    static const struct {
        unsigned value;
        const char *digits;
    } roman[] = {
        {1000, "m"}, {900, "cm"}, {500, "d"}, {400, "cd"},
        {100, "c"}, {90, "xc"}, {50, "l"}, {40, "xl"},
        {10, "x"}, {9, "ix"}, {5, "v"}, {4, "iv"}, {1, "i"}
    };
    if (value == 0 || value > 3999 || capacity == 0) return 0;
    size_t written = 0;
    for (size_t i = 0; i < sizeof(roman) / sizeof(roman[0]); i++) {
        while (value >= roman[i].value) {
            for (const char *at = roman[i].digits; *at != '\0'; at++) {
                if (written + 1 >= capacity) return written;
                output[written++] = upper
                    ? (char) toupper((unsigned char) *at) : *at;
            }
            value -= roman[i].value;
        }
    }
    output[written] = '\0';
    return written;
}

static size_t format_counter_value(int value, ListStyleType type,
                                   char *output, size_t capacity)
{
    if (capacity == 0) return 0;
    if ((type == LIST_STYLE_LOWER_ALPHA || type == LIST_STYLE_UPPER_ALPHA)
        && value > 0) {
        char reverse[16];
        size_t count = 0;
        unsigned remaining = (unsigned) value;
        while (remaining != 0 && count < sizeof(reverse)) {
            remaining--;
            reverse[count++] = (char) ('a' + remaining % 26u);
            remaining /= 26u;
        }
        size_t written = 0;
        while (count != 0 && written + 1 < capacity) {
            char character = reverse[--count];
            output[written++] = type == LIST_STYLE_UPPER_ALPHA
                ? (char) toupper((unsigned char) character) : character;
        }
        output[written] = '\0';
        return written;
    }
    if ((type == LIST_STYLE_LOWER_ROMAN || type == LIST_STYLE_UPPER_ROMAN)
        && value > 0) {
        return format_roman(
            (unsigned) value, type == LIST_STYLE_UPPER_ROMAN,
            output, capacity);
    }
    int written = type == LIST_STYLE_DECIMAL_LEADING_ZERO
        && value >= 0 && value < 10
        ? snprintf(output, capacity, "0%d", value)
        : snprintf(output, capacity, "%d", value);
    if (written < 0) return 0;
    return (size_t) written < capacity ? (size_t) written : capacity - 1;
}

size_t list_marker_text(ListStyleType type, int position,
                        char *output, size_t capacity)
{
    if (output == NULL || capacity == 0 || type == LIST_STYLE_NONE) return 0;
    const char *symbol = NULL;
    if (type == LIST_STYLE_DISC || type == LIST_STYLE_AUTO) {
        symbol = "\xe2\x80\xa2";
    } else if (type == LIST_STYLE_CIRCLE) {
        symbol = "\xe2\x97\xa6";
    } else if (type == LIST_STYLE_SQUARE) {
        symbol = "\xe2\x96\xaa";
    }
    if (symbol != NULL) {
        size_t length = strlen(symbol);
        if (length >= capacity) length = capacity - 1;
        memcpy(output, symbol, length);
        output[length] = '\0';
        return length;
    }
    size_t written = format_counter_value(
        position, type, output, capacity);
    if (written + 1 < capacity) {
        output[written++] = '.';
        output[written] = '\0';
    }
    return written;
}

static bool generated_append(char *output, size_t capacity, size_t *written,
                             const char *text, size_t length)
{
    if (length > capacity - *written - 1) return false;
    memcpy(output + *written, text, length);
    *written += length;
    output[*written] = '\0';
    return true;
}

static const char *generated_function_end(const char *at)
{
    unsigned depth = 0;
    char quote = 0;
    for (; *at != '\0'; at++) {
        if (quote != 0) {
            if (*at == '\\' && at[1] != '\0') at++;
            else if (*at == quote) quote = 0;
        } else if (*at == '\'' || *at == '"') {
            quote = *at;
        } else if (*at == '(') {
            depth++;
        } else if (*at == ')' && --depth == 0) {
            return at + 1;
        }
    }
    return at;
}

static ListStyleType generated_counter_style(
    const char *text, size_t length)
{
    while (length != 0 && isspace((unsigned char) *text)) {
        text++;
        length--;
    }
    while (length != 0
           && isspace((unsigned char) text[length - 1])) length--;
#define COUNTER_STYLE(name, value) \
    if (length == sizeof(name) - 1 \
        && memcmp(text, name, sizeof(name) - 1) == 0) return value
    COUNTER_STYLE("decimal-leading-zero", LIST_STYLE_DECIMAL_LEADING_ZERO);
    COUNTER_STYLE("lower-alpha", LIST_STYLE_LOWER_ALPHA);
    COUNTER_STYLE("lower-latin", LIST_STYLE_LOWER_ALPHA);
    COUNTER_STYLE("upper-alpha", LIST_STYLE_UPPER_ALPHA);
    COUNTER_STYLE("upper-latin", LIST_STYLE_UPPER_ALPHA);
    COUNTER_STYLE("lower-roman", LIST_STYLE_LOWER_ROMAN);
    COUNTER_STYLE("upper-roman", LIST_STYLE_UPPER_ROMAN);
#undef COUNTER_STYLE
    return LIST_STYLE_DECIMAL;
}

static void resolve_generated_expression(
    LayoutContext *context, lxb_dom_node_t *node, PseudoElement pseudo,
    ComputedStyle *style, char *output, size_t capacity)
{
    if (context == NULL || style == NULL || !style->generated_expression
        || style->generated_text == NULL || capacity == 0) return;
    const Stylesheet *sheet = context->sheet;
    const char *at = style->generated_text;
    size_t written = 0;
    output[0] = '\0';
    unsigned quote_depth = 0;
    while (*at != '\0') {
        while (isspace((unsigned char) *at)) at++;
        if (*at == '\0') break;
        if (*at == '\'' || *at == '"') {
            char decoded[STYLE_GENERATED_TEXT_CAPACITY];
            size_t length = style_decode_generated_text(
                at, decoded, sizeof(decoded));
            if (!generated_append(
                    output, capacity, &written, decoded, length)) break;
            char quote = *at++;
            while (*at != '\0') {
                if (*at == '\\' && at[1] != '\0') at += 2;
                else if (*at++ == quote) break;
            }
            continue;
        }
        const char *token = at;
        while (isalnum((unsigned char) *at) || *at == '-'
               || *at == '_') at++;
        size_t token_length = (size_t) (at - token);
        if (*at == '(' && token_length != 0) {
            const char *end = generated_function_end(at);
            const char *inside = at + 1;
            size_t inside_length =
                end > inside && end[-1] == ')'
                ? (size_t) (end - inside - 1) : strlen(inside);
            const char *comma = memchr(inside, ',', inside_length);
            if (token_length == 4 && memcmp(token, "attr", 4) == 0) {
                size_t name_length = comma == NULL ? inside_length
                                                   : (size_t) (comma - inside);
                while (name_length != 0
                       && isspace((unsigned char) inside[name_length - 1])) {
                    name_length--;
                }
                while (name_length != 0
                       && isspace((unsigned char) *inside)) {
                    inside++;
                    name_length--;
                }
                char name[32];
                if (name_length != 0 && name_length < sizeof(name)) {
                    memcpy(name, inside, name_length);
                    name[name_length] = '\0';
                    size_t attribute_length = 0;
                    const char *attribute = document_attribute(
                        node, name, &attribute_length);
                    if (attribute != NULL
                        && !generated_append(
                            output, capacity, &written,
                            attribute, attribute_length)) break;
                }
            } else if ((token_length == 7
                        && memcmp(token, "counter", 7) == 0)
                       || (token_length == 8
                           && memcmp(token, "counters", 8) == 0)) {
                bool plural = token_length == 8;
                const char *name_end = comma == NULL
                    ? inside + inside_length : comma;
                while (name_end > inside
                       && isspace((unsigned char) name_end[-1])) name_end--;
                while (inside < name_end
                       && isspace((unsigned char) *inside)) inside++;
                char name[32];
                size_t name_length = (size_t) (name_end - inside);
                if (name_length != 0 && name_length < sizeof(name)) {
                    memcpy(name, inside, name_length);
                    name[name_length] = '\0';
                    int values[GENERATED_COUNTER_STACK_LIMIT];
                    size_t count = generated_counter_values(
                        sheet, node, pseudo, name, values,
                        GENERATED_COUNTER_STACK_LIMIT);
                    const char *style_text = NULL;
                    size_t style_length = 0;
                    char separator[STYLE_GENERATED_TEXT_CAPACITY] = "";
                    size_t separator_length = 0;
                    if (comma != NULL) {
                        const char *argument = comma + 1;
                        while (argument < end
                               && isspace((unsigned char) *argument)) {
                            argument++;
                        }
                        if (plural && (*argument == '\''
                                       || *argument == '"')) {
                            separator_length = style_decode_generated_text(
                                argument, separator, sizeof(separator));
                            const char *separator_end =
                                generated_function_end(argument);
                            char quote = *argument++;
                            while (argument < end) {
                                if (*argument == '\\'
                                    && argument[1] != '\0') argument += 2;
                                else if (*argument++ == quote) break;
                            }
                            while (argument < end
                                   && isspace((unsigned char) *argument)) {
                                argument++;
                            }
                            if (*argument == ',') {
                                style_text = argument + 1;
                                style_length = (size_t) (
                                    end - 1 - style_text);
                            }
                            (void) separator_end;
                        } else {
                            style_text = argument;
                            style_length = (size_t) (end - 1 - argument);
                        }
                    }
                    ListStyleType counter_style = generated_counter_style(
                        style_text == NULL ? "" : style_text,
                        style_length);
                    size_t first = plural ? 0 : count - 1;
                    for (size_t i = first; i < count; i++) {
                        if (i != first
                            && !generated_append(
                                output, capacity, &written,
                                separator, separator_length)) break;
                        char formatted[32];
                        size_t length = format_counter_value(
                            values[i], counter_style,
                            formatted, sizeof(formatted));
                        if (!generated_append(
                                output, capacity, &written,
                                formatted, length)) break;
                    }
                }
            }
            at = end;
            continue;
        }
        if (token_length == 10
            && memcmp(token, "open-quote", 10) == 0) {
            static const char *const quotes[] = {"\xe2\x80\x9c", "\xe2\x80\x98"};
            const char *quote = quotes[quote_depth > 0 ? 1 : 0];
            (void) generated_append(
                output, capacity, &written, quote, strlen(quote));
            quote_depth++;
        } else if (token_length == 11
                   && memcmp(token, "close-quote", 11) == 0) {
            static const char *const quotes[] = {"\xe2\x80\x9d", "\xe2\x80\x99"};
            if (quote_depth != 0) quote_depth--;
            const char *quote = quotes[quote_depth > 0 ? 1 : 0];
            (void) generated_append(
                output, capacity, &written, quote, strlen(quote));
        }
        if (token_length == 0) at++;
    }
    style->generated_text = written == 0 ? NULL
        : layout_retain_generated_text(
            context->layout, output, written);
    if (style->generated_text == NULL) written = 0;
    style->generated_text_length = (uint8_t) written;
}


bool generated_pseudo_is_flow_block(const ComputedStyle *style)
{
    if (style == NULL || !style->generated_content || style->hidden
        || style->display == DISPLAY_NONE
        || style->display == DISPLAY_TABLE_COLUMN || style->out_of_flow
        || style->fixed_position || style->float_mode != FLOAT_NONE) {
        return false;
    }
    return style->display == DISPLAY_BLOCK
           || style->display == DISPLAY_FLOW_ROOT
           || style->display == DISPLAY_FLEX
           || style->display == DISPLAY_GRID
           || style->display == DISPLAY_TABLE;
}

GeneratedPseudoFlow generated_pseudo_flow(
    LayoutContext *context, lxb_dom_node_t *node,
    const ComputedStyle *parent, PseudoElement pseudo,
    int width, int containing_height)
{
    ComputedStyle style = style_for_pseudo(context->sheet, node, pseudo,
                                           parent);
    char generated_text[STYLE_GENERATED_TEXT_CAPACITY];
    resolve_generated_expression(
        context, node, pseudo, &style,
        generated_text, sizeof(generated_text));
    resolve_padding(context->sheet, &style, width);
    const char *trace_class = context->layout->trace_pseudo_class;
    if (trace_class != NULL && trace_class[0] != '\0') {
        size_t class_length = 0;
        const char *class_name = document_attribute(
            node, "class", &class_length);
        if (class_name != NULL && strstr(class_name, trace_class) != NULL) {
            fprintf(stderr,
                    "layout-pseudo class=%.*s pseudo=%d generated=%d "
                    "display=%d hidden=%d out=%d float=%d padding=%d/%d "
                    "height=%d/%d background=%d image=%s mask=%s\n",
                    (int) class_length, class_name, pseudo,
                    style.generated_content, style.display, style.hidden,
                    style.out_of_flow, style.float_mode,
                    style.padding.top, style.padding.bottom,
                    style.has_height, style.height, style.has_background,
                    style.background_image == NULL
                        ? "" : style.background_image,
                    style.mask_image == NULL ? "" : style.mask_image);
        }
    }
    if (!generated_pseudo_is_flow_block(&style)) {
        return (GeneratedPseudoFlow) {0};
    }
    int content_height = style_content_height(context->sheet, &style, width,
                                              containing_height);
    if (content_height <= 0 && style.generated_text != NULL
        && style.generated_text_length != 0) {
        const FontFace *face = font_context_face_variant(
            context->fonts, context->web_fonts, style.font_family,
            style.font_italic, style_uses_bold_face(&style));
        content_height = style.line_height > 0
            ? style.line_height
            : font_line_height_at_size(
                  face, computed_style_font_size_fixed(&style));
        if (content_height < 0) content_height = 7 * style.font_scale;
    }
    int64_t border_height = (int64_t) content_height + style.padding.top
                            + style.padding.bottom + style.border.top
                            + style.border.bottom;
    if (border_height > INT_MAX) border_height = INT_MAX;
    if (border_height < 0) border_height = 0;
    return (GeneratedPseudoFlow) {
        .active = true,
        .margin_top = style.margin.top,
        .margin_right = style.margin.right,
        .margin_bottom = style.margin.bottom,
        .margin_left = style.margin.left,
        .border_height = (int) border_height
    };
}

/* Reads an HTML dimension attribute (`width`/`height`) as a non-negative
   pixel count.  Returns -1 when the attribute is absent or is not a plain
   run of digits, which keeps an explicit `width="0"` distinguishable from an
   unspecified dimension. */
static int html_dimension_attribute(lxb_dom_node_t *node, const char *name)
{
    size_t length = 0;
    const char *attribute = document_attribute(node, name, &length);
    if (attribute == NULL || length == 0 || length >= 32) return -1;
    char value[32];
    memcpy(value, attribute, length);
    value[length] = '\0';
    size_t at = 0;
    while (value[at] == ' ' || value[at] == '\t' || value[at] == '\n'
           || value[at] == '\f' || value[at] == '\r') at++;
    if (value[at] < '0' || value[at] > '9') return -1;
    long parsed = 0;
    while (value[at] >= '0' && value[at] <= '9') {
        parsed = parsed * 10 + (value[at] - '0');
        if (parsed > 1 << 20) return 1 << 20;
        at++;
    }
    return (int) parsed;
}

static void size_pseudo_background_command(
    DrawCommand *command, const ComputedStyle *style,
    const ImageResource *image, int area_width, int area_height)
{
    if (command == NULL || style == NULL || image == NULL
        || (style->background_size_flags
            & STYLE_BACKGROUND_SIZE_EXPLICIT) == 0) return;
    bool width_auto = (style->background_size_flags
                       & STYLE_BACKGROUND_WIDTH_AUTO) != 0;
    bool height_auto = (style->background_size_flags
                        & STYLE_BACKGROUND_HEIGHT_AUTO) != 0;
    int image_width = width_auto ? 0
        : ((style->background_size_flags
            & STYLE_BACKGROUND_WIDTH_PERCENT) != 0
           ? layout_scale_dimension(area_width, style->background_width, 100)
           : style->background_width);
    int image_height = height_auto ? 0
        : ((style->background_size_flags
            & STYLE_BACKGROUND_HEIGHT_PERCENT) != 0
           ? layout_scale_dimension(area_height, style->background_height,
                                    100)
           : style->background_height);
    int natural_width = image_resource_intrinsic_width(image);
    int natural_height = image_resource_intrinsic_height(image);
    if (width_auto && height_auto) {
        image_width = natural_width;
        image_height = natural_height;
    } else if (width_auto && image_height > 0 && natural_height > 0) {
        image_width = layout_scale_dimension(
            image_height, natural_width, natural_height);
    } else if (height_auto && image_width > 0 && natural_width > 0) {
        image_height = layout_scale_dimension(
            image_width, natural_height, natural_width);
    }
    if (image_width < 1) image_width = 1;
    if (image_height < 1) image_height = 1;
    bool pixels = (style->background_size_flags
                   & STYLE_BACKGROUND_POSITION_PIXELS) != 0;
    int offset_x = pixels ? style->background_position_x
        : (area_width - image_width) * style->background_position_x / 100;
    int offset_y = pixels ? style->background_position_y
        : (area_height - image_height) * style->background_position_y / 100;
    bool tile_x = (style->background_size_flags
                   & STYLE_BACKGROUND_NO_REPEAT_X) == 0;
    bool tile_y = (style->background_size_flags
                   & STYLE_BACKGROUND_NO_REPEAT_Y) == 0;
    command->image_fit =
        tile_x && tile_y ? LAYOUT_IMAGE_FIT_SPRITE_TILE_XY
        : (tile_x ? LAYOUT_IMAGE_FIT_SPRITE_TILE_X
           : (tile_y ? LAYOUT_IMAGE_FIT_SPRITE_TILE_Y
                     : LAYOUT_IMAGE_FIT_SPRITE));
    draw_command_set_image_offset(command, offset_x, offset_y);
    draw_command_set_image_sprite_size(command, image_width, image_height);
}

int generated_inline_pseudo_width(LayoutContext *context,
                                  lxb_dom_node_t *node,
                                  const ComputedStyle *parent,
                                  PseudoElement pseudo)
{
    ComputedStyle style = style_for_pseudo(context->sheet, node, pseudo,
                                           parent);
    char generated_text[STYLE_GENERATED_TEXT_CAPACITY];
    resolve_generated_expression(
        context, node, pseudo, &style,
        generated_text, sizeof(generated_text));
    if (!style.generated_content || style.display == DISPLAY_NONE
        || style.hidden || style.out_of_flow || style.fixed_position
        || style.has_width
        || (style.mask_image != NULL && style.mask_image[0] != '\0')
        || (style.background_image != NULL
            && style.background_image[0] != '\0')
        || style.has_background
        || style.border.left > 0 || style.border.right > 0
        || style.border.top > 0 || style.border.bottom > 0
        || generated_pseudo_is_flow_block(&style)
        || style.generated_text == NULL
        || style.generated_text_length == 0) {
        return 0;
    }
    const FontFace *face = font_context_face_variant(
        context->fonts, context->web_fonts, style.font_family,
        style.font_italic, style_uses_bold_face(&style));
    FontFamily metric_family = font_context_metric_family(
        context->web_fonts, style.font_family, face);
    int width_fixed = font_text_width_for_family_at_size_fixed(
        face, metric_family, style.generated_text,
        style.generated_text_length,
        computed_style_font_size_fixed(&style),
        style_uses_synthetic_weight(context->fonts, context->web_fonts,
                                    &style, face),
        style_uses_bold_face(&style));
    int width = width_fixed < 0
        ? (int) utf8_codepoints(style.generated_text,
                                style.generated_text_length)
              * 6 * style.font_scale
        : layout_fixed_ceil(width_fixed);
    return width + style.margin.left + style.padding.left
           + style.padding.right + style.margin.right;
}

/* Text-only generated content participates in the inline flow of its
   originating box (hlist separators, quote marks). Relatively positioned
   text keeps its flow advance but shifts only the generated fragment;
   out-of-flow and box-bearing pseudos keep the rectangle painter. */
bool flow_generated_inline_pseudo(LayoutContext *context,
                                  lxb_dom_node_t *node,
                                  const ComputedStyle *parent,
                                  PseudoElement pseudo, LineState *line,
                                  const char *link_url,
                                  size_t link_url_length,
                                  lxb_dom_node_t *link_node, bool *flowed)
{
    if (flowed != NULL) *flowed = false;
    ComputedStyle style = style_for_pseudo(context->sheet, node, pseudo,
                                           parent);
    char generated_text[STYLE_GENERATED_TEXT_CAPACITY];
    resolve_generated_expression(
        context, node, pseudo, &style,
        generated_text, sizeof(generated_text));
    if (!style.generated_content || style.display == DISPLAY_NONE
        || style.hidden) return true;
    bool out_of_flow = style.out_of_flow || style.fixed_position;
    bool own_box = style.has_width
        || (style.mask_image != NULL && style.mask_image[0] != '\0')
        || (style.background_image != NULL
            && style.background_image[0] != '\0')
        || style.has_background
        || style.border.left > 0 || style.border.right > 0
        || style.border.top > 0 || style.border.bottom > 0;
    if (out_of_flow || own_box || generated_pseudo_is_flow_block(&style)
        || style.generated_text == NULL
        || style.generated_text_length == 0) {
        return true;
    }
    if (style.margin.left + style.padding.left > 0) {
        line_cursor_set(line, line->x + style.margin.left
                              + style.padding.left);
        line->pending_space = false;
    }
    size_t command_start = context->layout->count;
    size_t link_start = context->layout->link_count;
    size_t control_start = context->layout->control_count;
    size_t node_box_start = context->layout->node_box_count;
    if (!flow_text(context, line, style.generated_text,
                   style.generated_text_length, &style, link_url,
                   link_url_length, link_node)) return false;
    if (style.relative_position) {
        int reference_width = line->right - line->start_x;
        int reference_height = line->line_height > 0 ? line->line_height
                                                      : style.line_height;
        if (reference_height < 1) reference_height = 1;
        int dx = 0, dy = 0, inset = 0;
        if (style.has_left
            && resolve_computed_length(
                context->sheet, style.left,
                (style.inset_percent_mask & STYLE_INSET_LEFT_PERCENT) != 0,
                reference_width, &inset)) {
            dx = inset;
        } else if (style.has_right
                   && resolve_computed_length(
                       context->sheet, style.right,
                       (style.inset_percent_mask
                        & STYLE_INSET_RIGHT_PERCENT) != 0,
                       reference_width, &inset)) {
            dx = -inset;
        }
        if (style.has_top
            && resolve_computed_length(
                context->sheet, style.top,
                (style.inset_percent_mask & STYLE_INSET_TOP_PERCENT) != 0,
                reference_height, &inset)) {
            dy = inset;
        } else if (style.has_bottom
                   && resolve_computed_length(
                       context->sheet, style.bottom,
                       (style.inset_percent_mask
                        & STYLE_INSET_BOTTOM_PERCENT) != 0,
                       reference_height, &inset)) {
            dy = -inset;
        }
        if (dx != 0 || dy != 0) {
            layout_translate_range(
                context->layout, command_start, link_start, control_start,
                node_box_start, dx, dy, "generated-relative", node);
        }
    }
    if (style.padding.right + style.margin.right > 0) {
        line_cursor_set(line, line->x + style.padding.right
                              + style.margin.right);
    }
    line->pending_space = false;
    if (flowed != NULL) *flowed = true;
    return true;
}

bool paint_pseudo(LayoutContext *context, lxb_dom_node_t *node,
                         const ComputedStyle *parent, PseudoElement pseudo,
                         int x, int y, int width, int height,
                         size_t insertion_index, int forced_border_height)
{
    ComputedStyle style = style_for_pseudo(context->sheet, node, pseudo,
                                           parent);
    int border_radius_code = stylesheet_border_radius_code(
        context->sheet, &style);
    char generated_text[STYLE_GENERATED_TEXT_CAPACITY];
    resolve_generated_expression(
        context, node, pseudo, &style,
        generated_text, sizeof(generated_text));
    resolve_padding(context->sheet, &style, width);
    if (!style.generated_content || style.display == DISPLAY_NONE
        || style.hidden) return true;
    const ImageResource *pseudo_mask = style.mask_image != NULL
                                       && style.mask_image[0] != '\0'
                                       ? images_find_mask_source(
                                           context->images, node,
                                           style.mask_image, pseudo)
                                       : NULL;
    const ImageResource *pseudo_background =
        style.background_image != NULL && style.background_image[0] != '\0'
        ? images_find_background_source(
            context->images, node, style.background_image, pseudo)
        : NULL;
    if (style.mask_image != NULL && style.mask_image[0] != '\0'
        && !image_resource_available(pseudo_mask)) {
        layout_note_unresolved_external_visual(
            context, node, style.mask_image, IMAGE_PRIORITY_KIND_MASK,
            pseudo);
    }
    if (style.background_image_kind == STYLE_BACKGROUND_IMAGE_URL
        && !image_resource_available(pseudo_background)) {
        layout_note_unresolved_external_visual(
            context, node, style.background_image,
            IMAGE_PRIORITY_KIND_BACKGROUND, pseudo);
    }
    /* A masked pseudo needs its pixels, not an unmasked solid fallback. */
    if (style.mask_image != NULL && style.mask_image[0] != '\0'
        && pseudo_mask == NULL) return true;
    const FontFace *face = font_context_face_variant(
        context->fonts, context->web_fonts, style.font_family,
        style.font_italic, style_uses_bold_face(&style));
    bool metric_bold = style_uses_bold_face(&style);
    FontFamily metric_family = font_context_metric_family(
        context->web_fonts, style.font_family, face);
    bool synthetic_bold = style_uses_synthetic_weight(
        context->fonts, context->web_fonts, &style, face);
    int text_width = 0;
    int text_height = 0;
    if (style.generated_text != NULL && style.generated_text_length != 0) {
        int text_width_fixed = font_text_width_for_family_at_size_fixed(
            face, metric_family, style.generated_text,
            style.generated_text_length,
            computed_style_font_size_fixed(&style), synthetic_bold,
            metric_bold);
        text_width = text_width_fixed < 0 ? text_width_fixed
                     : (text_width_fixed + 32) / 64;
        if (text_width < 0) {
            text_width = (int) utf8_codepoints(style.generated_text,
                                               style.generated_text_length)
                         * 6 * style.font_scale;
        }
        size_t characters = utf8_codepoints(style.generated_text,
                                            style.generated_text_length);
        int minimum_text_width = characters == 0
                                 ? 0 : text_width / (int) characters;
        if (characters > 1) {
            text_width += (int) (characters - 1) * style.letter_spacing;
        }
        if (text_width < minimum_text_width) {
            text_width = minimum_text_width;
        }
        text_height = font_line_height_at_size(
            face, computed_style_font_size_fixed(&style));
        if (text_height < 0) text_height = 7 * style.font_scale;
    }
    bool flow_block = generated_pseudo_is_flow_block(&style);
    int pseudo_width = style.has_width
                       ? resolve_declared_length(
                           context->sheet, style.width,
                           style.width_percent, width)
                       : (flow_block ? width
                          : (text_width > 0
                          ? text_width + style.padding.left
                            + style.padding.right : width));
    if (!style.has_width && style.has_left && style.has_right) {
        pseudo_width = width - style.left - style.right;
    }
    int pseudo_content_height = style_content_height(
        context->sheet, &style, width, height);
    if (pseudo_content_height <= 0 && text_height > 0) {
        pseudo_content_height = text_height;
    }
    int64_t used_height = (int64_t) pseudo_content_height
                          + style.padding.top + style.padding.bottom
                          + style.border.top + style.border.bottom;
    if (used_height > INT_MAX) used_height = INT_MAX;
    int pseudo_height = forced_border_height > 0
                        ? forced_border_height : (int) used_height;
    if (pseudo_width <= 0 || pseudo_height <= 0) return true;
    int pseudo_x = style.has_left ? x + style.left
                   : (style.has_right ? x + width - style.right - pseudo_width
                                      : x);
    int pseudo_y = style.has_top ? y + style.top
                   : (style.has_bottom
                      ? y + height - style.bottom - pseudo_height
                      : (pseudo == PSEUDO_AFTER ? y + height - pseudo_height
                                                : y));
    size_t command_count_before = context->layout->count;
    size_t pseudo_command_start = insertion_index <= command_count_before
                                  ? insertion_index : command_count_before;
    size_t next_insertion = insertion_index;
    /* A mask remains one colored image command: painting its source color as
       a separate fill would erase the masked transparency.  A background
       image, however, is an independent CSS layer and must be painted over
       the background color so transparent pixels reveal that color. */
    if (pseudo_mask != NULL) {
        DrawCommand mask = {
            .type = DRAW_IMAGE, .x = pseudo_x, .y = pseudo_y,
            .width = pseudo_width, .height = pseudo_height,
            .color = style.has_background ? style.background : style.color,
            .image = pseudo_mask, .image_fit = style.background_fit,
            .scale = border_radius_code,
            .opacity_scale = alpha_opacity_scale(
                style.has_background ? style.background_alpha
                                     : style.color_alpha)
        };
        if (next_insertion <= context->layout->count) {
            if (!layout_insert_command(context, next_insertion, mask)) {
                return false;
            }
            next_insertion++;
        } else if (layout_add_command(context->layout, mask) == NULL) {
            return false;
        }
    } else {
        if (style.has_background) {
            DrawCommand fill = {
                .type = DRAW_FILL_RECT, .x = pseudo_x, .y = pseudo_y,
                .width = pseudo_width, .height = pseudo_height,
                .color = style.background, .radius = border_radius_code,
                .opacity_scale = alpha_opacity_scale(style.background_alpha)
            };
            if (next_insertion <= context->layout->count) {
                if (!layout_insert_command(context, next_insertion, fill)) {
                    return false;
                }
                next_insertion++;
            } else if (layout_add_command(context->layout, fill) == NULL) {
                return false;
            }
        }
        if (image_resource_available(pseudo_background)) {
            DrawCommand image = {
                .type = DRAW_IMAGE, .x = pseudo_x, .y = pseudo_y,
                .width = pseudo_width, .height = pseudo_height,
                .image = pseudo_background, .image_fit = style.background_fit,
                .scale = border_radius_code,
                .opacity_scale = alpha_opacity_scale(255)
            };
            size_pseudo_background_command(
                &image, &style, pseudo_background,
                pseudo_width, pseudo_height);
            if (next_insertion <= context->layout->count) {
                if (!layout_insert_command(context, next_insertion, image)) {
                    return false;
                }
                next_insertion++;
            } else if (layout_add_command(context->layout, image) == NULL) {
                return false;
            }
        }
    }
    if (style.generated_text != NULL && style.generated_text_length != 0) {
        DrawCommand text = {
            .type = DRAW_TEXT,
            .x = pseudo_x + style.padding.left,
            .y = pseudo_y + style.padding.top,
            .width = text_width,
            .height = text_height,
            .color = style.color,
            .text = style.generated_text,
            .text_length = style.generated_text_length,
            .scale = style.font_scale,
            .font_size = style.font_size,
            .font_family = style.font_family,
            .font_weight = draw_font_weight(&style),
            .font_italic = style.font_italic,
            .letter_spacing = style.letter_spacing,
            .radius = (int) style.font_size_fraction
                      << LAYOUT_TEXT_FONT_SIZE_FRACTION_SHIFT
                      | (computed_style_kerning_none(&style)
                         ? LAYOUT_TEXT_KERNING_NONE : 0),
            .image_fit = text_decoration_bits(&style),
            .opacity_scale = alpha_opacity_scale(style.color_alpha)
        };
        if (next_insertion <= context->layout->count) {
            size_t shadow_count = 0;
            if (!layout_insert_text_shadow_commands(
                    context, &style, &text, next_insertion,
                    &shadow_count)) {
                return false;
            }
            next_insertion += shadow_count;
            if (!layout_insert_command(context, next_insertion, text)) {
                return false;
            }
        } else {
            if (!layout_add_text_shadow_commands(context, &style, &text)
                || layout_add_command(context->layout, text) == NULL) {
                return false;
            }
        }
    }
    if (style.has_transform) {
        size_t inserted = context->layout->count - command_count_before;
        int origin_x_twice = 0, origin_y_twice = 0;
        layout_transform_origin_twice(
            context->sheet, &style, pseudo_x, pseudo_y,
            pseudo_width, pseudo_height,
            &origin_x_twice, &origin_y_twice);
        int dx = style.transform_x_percent
                 ? pseudo_width * style.transform_x / 100
                 : style.transform_x;
        int dy = style.transform_y_percent
                 ? pseudo_height * style.transform_y / 100
                 : style.transform_y;
        layout_transform_command_span(
            context->layout, pseudo_command_start,
            pseudo_command_start + inserted,
            origin_x_twice, origin_y_twice, style.transform_scale_q6,
            style.individual_rotate_quadrants, dx, dy);
    }
    return true;
}

static bool flow_generated_block_pseudo(
    LayoutContext *context, lxb_dom_node_t *node,
    const ComputedStyle *parent, PseudoElement pseudo,
    LineState *line, const GeneratedPseudoFlow *flow)
{
    if (flow == NULL || !flow->active) return true;
    /* A block-level generated child splits an inline formatting box.  Flush
       any preceding inline content, place the generated margin/border box,
       and start a fresh line so later text alignment cannot translate the
       pseudo fragment with the following text. */
    layout_flush_line(line);
    int width = line->right - line->start_x
                - flow->margin_left - flow->margin_right;
    if (width < 0) width = 0;
    int y = layout_add_coordinate(line->y, flow->margin_top);
    if (!paint_pseudo(context, node, parent, pseudo,
                      line->start_x + flow->margin_left, y,
                      width, flow->border_height, SIZE_MAX,
                      flow->border_height)) {
        return false;
    }
    line->y = layout_add_coordinate(
        layout_add_coordinate(y, flow->border_height), flow->margin_bottom);
    layout_flush_line(line);
    return true;
}

bool layout_add_link(LayoutDocument *layout, const DrawCommand *command,
                     size_t command_index, const char *url, size_t url_length,
                     lxb_dom_node_t *node)
{
    if (url == NULL || url_length == 0) return true;
    if (url_length > LAYOUT_LINK_URL_LENGTH_MASK) return false;
    if (layout->link_count == layout->link_capacity) {
        size_t capacity = layout->link_capacity == 0 ? 64
                          : (layout->link_capacity < 1024
                             ? layout->link_capacity * 2
                             : layout->link_capacity + 1024);
        LinkRegion *links = budget_realloc(layout->budget, layout->links,
                                           capacity * sizeof(*links));
        if (links == NULL) return false;
        layout->links = links;
        layout->link_capacity = capacity;
    }
    int transient_command = command->z_index;
    uint32_t retained_url_length = (uint32_t) url_length;
    if (command->type == DRAW_TEXT && command_index < (size_t) INT_MAX) {
        /*
         * Text links are appended with their draw command. Retain that
         * one-based relationship as a negative value only until the line
         * baseline pass; authored z-index is applied to both objects later.
         */
        transient_command = -(int) (command_index + 1u);
        retained_url_length |= LAYOUT_LINK_TRANSIENT_COMMAND;
    }
    layout->links[layout->link_count++] = (LinkRegion) {
        .url = url, .node = node, .x = command->x, .y = command->y,
        .width = command->width, .height = command->height,
        .z_index = transient_command, .url_length = retained_url_length
    };
    return true;
}

bool layout_add_control(LayoutDocument *layout, int x, int y,
                        int width, int height, ControlType type,
                        lxb_dom_node_t *node)
{
    if (!layout_node_name_is(node, "label") && node_effectively_disabled(node)) {
        return true;
    }
    if (layout->control_count == layout->control_capacity) {
        size_t capacity = layout->control_capacity == 0
                          ? 16 : layout->control_capacity * 2;
        ControlRegion *controls = budget_realloc(
            layout->budget, layout->controls,
            capacity * sizeof(*controls));
        if (controls == NULL) return false;
        layout->controls = controls;
        layout->control_capacity = capacity;
    }
    layout->controls[layout->control_count++] = (ControlRegion) {
        x, y, width, height, type, node, 0
    };
    return true;
}

static int line_text_baseline(const LayoutDocument *layout,
                              const DrawCommand *command)
{
    if (layout == NULL || command == NULL || command->type != DRAW_TEXT) {
        return -1;
    }
    const FontFace *face = font_context_face_variant(
        layout->fonts, layout->web_fonts, draw_command_font_family(command),
        draw_command_font_italic(command), draw_uses_bold_face(command));
    int baseline = font_line_baseline_at_size(
        face, draw_command_text_font_size_fixed(command), command->height);
    if (baseline < 0) {
        baseline = command->height > 0
            ? command->height * 3 / 4 : 0;
    }
    return baseline;
}

static void align_line_text_baselines(LineState *line)
{
    if (line == NULL || line->layout == NULL
        || line->command_start >= line->layout->count) return;
    int target = 0;
    for (size_t i = line->command_start; i < line->layout->count; i++) {
        DrawCommand *command = &line->layout->commands[i];
        if (draw_command_text_baseline_aligned(command)) continue;
        int baseline = line_text_baseline(line->layout, command);
        if (baseline > target) target = baseline;
        command->z_index = baseline < INT_MAX ? baseline + 1 : INT_MAX;
    }
    for (size_t i = line->link_start;
         i < line->layout->link_count; i++) {
        LinkRegion *link = &line->layout->links[i];
        if ((link->url_length & LAYOUT_LINK_TRANSIENT_COMMAND) == 0) {
            continue;
        }
        size_t command_index =
            (size_t) (-(int64_t) link->z_index - 1);
        if (command_index >= line->command_start
            && command_index < line->layout->count) {
            const DrawCommand *command =
                &line->layout->commands[command_index];
            if (!draw_command_text_baseline_aligned(command)
                && command->z_index > 0) {
                int baseline = command->z_index - 1;
                link->y = layout_add_coordinate(
                    link->y, target - baseline);
            }
        }
        link->url_length &= LAYOUT_LINK_URL_LENGTH_MASK;
        link->z_index = 0;
    }
    int used_height = line->line_height;
    for (size_t i = line->command_start; i < line->layout->count; i++) {
        DrawCommand *command = &line->layout->commands[i];
        if (draw_command_text_baseline_aligned(command)) continue;
        int baseline = command->z_index > 0
            ? command->z_index - 1 : -1;
        command->z_index = 0;
        if (baseline < 0) continue;
        int dy = target - baseline;
        command->y = layout_add_coordinate(command->y, dy);
        int bottom = command->y - line->y + command->height;
        if (bottom > used_height) used_height = bottom;
        command->radius |= LAYOUT_TEXT_BASELINE_ALIGNED;
    }
    if (used_height > line->line_height) line->line_height = used_height;
}

static int measured_flow_text_width_fixed(
    const FontFace *face, FontFamily metric_family,
    const char *text, size_t length, int font_size_fixed,
    bool synthetic_bold, bool metric_bold, int scale, int letter_spacing,
    TextTransformMode transform, bool kerning);

static size_t fitting_text_prefix(const FontFace *face,
                                  FontFamily metric_family,
                                  const char *text, size_t length,
                                  int font_size_fixed,
                                  bool synthetic_bold, bool metric_bold,
                                  int scale, int letter_spacing,
                                  int available_fixed,
                                  TextTransformMode transform,
                                  bool kerning);

static int line_clamp_measure_text(const LayoutDocument *layout,
                                   const DrawCommand *command,
                                   const char *text, size_t length,
                                   TextTransformMode transform)
{
    if (layout == NULL || command == NULL || text == NULL || length == 0) {
        return 0;
    }
    bool bold_face = draw_uses_bold_face(command);
    const FontFace *face = font_context_face_variant(
        layout->fonts, layout->web_fonts, draw_command_font_family(command),
        draw_command_font_italic(command), bold_face);
    FontFamily family = font_context_metric_family(
        layout->web_fonts, draw_command_font_family(command), face);
    unsigned weight = (unsigned) draw_command_font_weight_code(command) * 10u;
    bool synthetic_bold = weight >= 550u
        && (weight < 650u
            || !font_context_face_is_bold(
                   layout->fonts, layout->web_fonts, face));
    return measured_flow_text_width_fixed(
        face, family, text, length,
        draw_command_text_font_size_fixed(command),
        synthetic_bold, bold_face, command->scale,
        command->letter_spacing, transform,
        !draw_command_text_kerning_none(command));
}

bool layout_line_clamp_overflow(LineState *line)
{
    if (line == NULL || !line->clamp_pending || line->text_overflow_ended) {
        return line != NULL;
    }
    line->clamp_pending = false;
    line->text_overflow_ended = true;
    if (line->layout == NULL
        || line->clamp_command_start >= line->clamp_command_end
        || line->clamp_command_end > line->layout->count) return true;
    DrawCommand *last = NULL;
    for (size_t i = line->clamp_command_end;
         i-- > line->clamp_command_start;) {
        DrawCommand *candidate = &line->layout->commands[i];
        if (candidate->type == DRAW_TEXT
            && !draw_command_is_text_shadow(candidate)) {
            last = candidate;
            break;
        }
    }
    if (last == NULL) return true;
    static const char marker_text[] = "\xe2\x80\xa6";
    int marker_fixed = line_clamp_measure_text(
        line->layout, last, marker_text, sizeof(marker_text) - 1,
        TEXT_TRANSFORM_NONE);
    int marker_width = layout_fixed_ceil(marker_fixed);
    if (marker_width < 1) marker_width = 1;
    int available = line->clamp_right - last->x - marker_width;
    if (available < 0) available = 0;
    bool bold_face = draw_uses_bold_face(last);
    const FontFace *face = font_context_face_variant(
        line->layout->fonts, line->layout->web_fonts,
        draw_command_font_family(last), draw_command_font_italic(last),
        bold_face);
    FontFamily family = font_context_metric_family(
        line->layout->web_fonts, draw_command_font_family(last), face);
    unsigned weight = (unsigned) draw_command_font_weight_code(last) * 10u;
    bool synthetic_bold = weight >= 550u
        && (weight < 650u
            || !font_context_face_is_bold(
                   line->layout->fonts, line->layout->web_fonts, face));
    size_t prefix = available <= 0 ? 0 : fitting_text_prefix(
        face, family, last->text, last->text_length,
        draw_command_text_font_size_fixed(last), synthetic_bold, bold_face,
        last->scale, last->letter_spacing,
        layout_fixed_from_integer(available),
        draw_command_text_transform(last),
        !draw_command_text_kerning_none(last));
    if (prefix > last->text_length) prefix = last->text_length;
    int prefix_fixed = prefix == 0 ? 0 : line_clamp_measure_text(
        line->layout, last, last->text, prefix,
        draw_command_text_transform(last));
    if (prefix_fixed > layout_fixed_from_integer(available)) {
        prefix = 0;
        prefix_fixed = 0;
    }
    last->text_length = (uint32_t) prefix;
    last->width = layout_fixed_ceil(prefix_fixed);
    /* Preserve the prefix's 26.6 advance.  Rounding it to an integer before
       positioning the marker shifts an injected ellipsis by one pixel for
       common fractional glyph advances, even though an authored ellipsis on
       the same line retains the fraction through the normal inline path. */
    int64_t marker_x_wide = (int64_t) draw_command_text_x_fixed(last)
                            + prefix_fixed;
    int marker_x_fixed = marker_x_wide > INT_MAX ? INT_MAX
        : (marker_x_wide < INT_MIN ? INT_MIN : (int) marker_x_wide);
    for (size_t i = line->clamp_link_start;
         i < line->clamp_link_end && i < line->layout->link_count; i++) {
        LinkRegion *link = &line->layout->links[i];
        if (link->x == last->x && link->y == last->y
            && link->width > last->width) link->width = last->width;
    }
    DrawCommand marker = *last;
    draw_command_set_text_x_fixed(&marker, marker_x_fixed);
    marker.width = marker_width;
    marker.text = marker_text;
    marker.text_length = sizeof(marker_text) - 1;
    marker.image_fit &= (uint8_t) ~LAYOUT_TEXT_DECORATION_UNDERLINE;
    marker.radius &= ~(LAYOUT_TEXT_FIND_SPACE_BEFORE
                       | LAYOUT_TEXT_FIND_BLOCK_START);
    marker.z_index = 0;
    return layout_add_command(line->layout, marker) != NULL;
}

void layout_flush_line(LineState *line)
{
    if (line->line_height != 0 || line->line_height_fixed != 0) {
        int fragment_height = line->line_height_fixed != 0
            ? layout_fixed_ceil(line->line_height_fixed)
            : line->line_height;
        for (size_t i = line->command_start;
             i < line->layout->count; i++) {
            DrawCommand *command = &line->layout->commands[i];
            if (command->type == DRAW_STROKE_RECT
                && command->font_size == -1) {
                command->height = fragment_height;
                command->font_size = 0;
            }
        }
        if (line->direction_rtl) {
            int start_fixed = layout_fixed_from_integer(line->start_x);
            int content_right_fixed = line_cursor_fixed(line);
            int content_right = layout_fixed_ceil(content_right_fixed);
            for (size_t i = line->command_start;
                 i < line->layout->count; i++) {
                DrawCommand *command = &line->layout->commands[i];
                if (command->type == DRAW_TEXT) {
                    int mirrored = layout_fixed_add(
                        start_fixed,
                        layout_fixed_subtract(
                            layout_fixed_subtract(
                                content_right_fixed,
                                draw_command_text_x_fixed(command)),
                            layout_fixed_from_integer(command->width)));
                    draw_command_set_text_x_fixed(command, mirrored);
                } else {
                    command->x = layout_add_coordinate(
                        line->start_x,
                        content_right - command->x - command->width);
                }
            }
            for (size_t i = line->link_start;
                 i < line->layout->link_count; i++) {
                LinkRegion *link = &line->layout->links[i];
                link->x = layout_add_coordinate(
                    line->start_x,
                    content_right - link->x - link->width);
            }
            for (size_t i = line->control_start;
                 i < line->layout->control_count; i++) {
                ControlRegion *control = &line->layout->controls[i];
                control->x = layout_add_coordinate(
                    line->start_x,
                    content_right - control->x - control->width);
            }
            for (size_t i = line->node_box_start;
                 i < line->layout->node_box_count; i++) {
                LayoutNodeBox *box = &line->layout->node_boxes[i];
                box->x = layout_add_coordinate(
                    line->start_x,
                    content_right - box->x - box->width);
            }
        }
        align_line_text_baselines(line);
        int64_t right_fixed = (int64_t) line->right * 64;
        int64_t cursor_fixed = line_cursor_fixed(line);
        int64_t remaining_fixed = right_fixed - cursor_fixed;
        int64_t offset_fixed = line->text_align == TEXT_ALIGN_CENTER
            ? remaining_fixed / 2
            : (line->text_align == TEXT_ALIGN_RIGHT ? remaining_fixed : 0);
        int bounded_offset_fixed = offset_fixed > INT_MAX ? INT_MAX
            : (offset_fixed < INT_MIN ? INT_MIN : (int) offset_fixed);
        int offset = layout_fixed_floor(bounded_offset_fixed);
        if (offset > 0) {
            layout_translate_range(line->layout, line->command_start,
                            line->link_start, line->control_start,
                            line->node_box_start, offset, 0,
                            "text-align", NULL);
        }
        int height_fixed = line->line_height_fixed;
        if (height_fixed == 0
            || line->line_height > layout_fixed_ceil(height_fixed)) {
            height_fixed = layout_fixed_from_integer(line->line_height);
        }
        if (line->line_gap != 0) {
            height_fixed = layout_fixed_add(
                height_fixed, layout_fixed_from_integer(line->line_gap));
        }
        if (line->clamp_limit != 0
            && line->clamp_lines < line->clamp_limit) {
            line->clamp_lines++;
            if (line->clamp_lines == line->clamp_limit) {
                line->clamp_pending = true;
                line->clamp_command_start = line->command_start;
                line->clamp_command_end = line->layout->count;
                line->clamp_link_start = line->link_start;
                line->clamp_link_end = line->layout->link_count;
                line->clamp_right = line->right;
            }
        }
        line->y_fixed = layout_fixed_add(line_y_fixed(line), height_fixed);
        line->y = layout_fixed_floor(line->y_fixed);
        line->y_fixed_valid = true;
    }
    line_cursor_set(line, line->start_x);
    line->line_height = 0;
    line->line_height_fixed = 0;
    /* font_line_height already includes the face's normal line gap. */
    line->line_gap = 0;
    line->first_line_indent = 0;
    line->pending_space = false;
    line->has_text_character = false;
    line->previous_atomic = false;
    line->letter_boundary_spacing = 0;
    line->pending_space_letter_spacing = 0;
    line->command_start = line->layout->count;
    line->link_start = line->layout->link_count;
    line->control_start = line->layout->control_count;
    line->node_box_start = line->layout->node_box_count;
    update_float_bounds(line);
}

static int inline_style_line_height_fixed(
    LayoutContext *context, const ComputedStyle *style)
{
    const FontFace *face = font_context_face_variant(
        context->fonts, context->web_fonts, style->font_family,
        style->font_italic, style_uses_bold_face(style));
    int height_fixed = style->line_height > 0
        ? layout_fixed_from_integer(style->line_height)
        : font_line_height_fixed_at_size(
            face, computed_style_font_size_fixed(style));
    return height_fixed >= 0
        ? height_fixed
        : layout_fixed_from_integer(7 * style->font_scale);
}

static void force_line_break(LayoutContext *context, LineState *line,
                             const ComputedStyle *style)
{
    /* A BR creates a line box even when the current line has no painted
       content.  In particular, consecutive BR elements must retain their
       intervening blank line instead of collapsing into one break. */
    int height_fixed = inline_style_line_height_fixed(context, style);
    line_height_include_fixed(line, height_fixed);
    if (style->line_height > 0) line->line_gap = 0;
    layout_flush_line(line);
}

static bool paint_inline_strokes(
    LayoutContext *context, const ComputedStyle *style,
    int x, int y, int width, size_t background_insertion)
{
    if (context == NULL || style == NULL || width < 0) {
        return false;
    }
    int border_radius_code = stylesheet_border_radius_code(
        context->sheet, style);
    unsigned border_lines =
        style->paint_stack_state >> STYLE_BORDER_LINES_SHIFT;
    unsigned border_line = border_lines & 3u;
    bool uniform_border =
        style->border.top > 0
        && style->border.top == style->border.right
        && style->border.top == style->border.bottom
        && style->border.top == style->border.left
        && border_line != STYLE_BORDER_NONE
        && border_lines == border_line * 0x55u;
    if (uniform_border) {
        uint8_t alpha = 255;
        uint32_t color = stylesheet_border_color(
            context->sheet, style, STYLE_BORDER_TOP, &alpha);
        DrawCommand border = {
            .type = DRAW_STROKE_RECT,
            .x = x, .y = y, .width = width > 0 ? width : 1,
            .height = 0, .color = color, .scale = style->border.top,
            .font_size = -1,
            .radius = border_radius_code,
            .opacity_scale = alpha_opacity_scale(alpha),
            .image_fit = LAYOUT_STROKE_SOLID
        };
        if (!layout_insert_command(
                context, background_insertion, border)) {
            return false;
        }
    }
    unsigned outline_width = computed_style_outline_width(style);
    unsigned outline_style = computed_style_outline_style(style);
    if (outline_width != 0 && outline_style != STYLE_OUTLINE_NONE) {
        DrawCommand outline = {
            .type = DRAW_STROKE_RECT,
            .x = x, .y = y, .width = width > 0 ? width : 1,
            .height = 0,
            .color = (style->outline_state & STYLE_OUTLINE_CURRENT_COLOR)
                     != 0 ? style->color : style->outline_color,
            .scale = (int) outline_width,
            .font_size = -1,
            .opacity_scale = alpha_opacity_scale(
                (style->outline_state & STYLE_OUTLINE_CURRENT_COLOR) != 0
                ? style->color_alpha : style->outline_alpha),
            .image_fit = outline_style == STYLE_OUTLINE_DASHED
                ? LAYOUT_STROKE_DASHED
                : (outline_style == STYLE_OUTLINE_DOTTED
                   ? LAYOUT_STROKE_DOTTED : LAYOUT_STROKE_SOLID)
        };
        if (!layout_insert_command(
                context, background_insertion, outline)) {
            return false;
        }
    }
    return true;
}

static bool coalesce_rtl_text_fragment(
    LayoutContext *context, size_t command_start, size_t link_start)
{
    if (context == NULL || context->layout == NULL
        || context->layout->count <= command_start + 1
        || context->layout->link_count != link_start) return true;
    DrawCommand *first = &context->layout->commands[command_start];
    if (first->type != DRAW_TEXT) return true;
    char text[256];
    size_t used = 0;
    int right = first->x;
    for (size_t i = command_start; i < context->layout->count; i++) {
        const DrawCommand *command = &context->layout->commands[i];
        if (command->type != DRAW_TEXT || command->text == NULL
            || command->text_length > sizeof(text) - used
            || command->y != first->y || command->height != first->height
            || command->color != first->color
            || command->scale != first->scale
            || command->font_size != first->font_size
            || command->font_family != first->font_family
            || command->font_weight != first->font_weight
            || command->font_italic != first->font_italic
            || command->letter_spacing != first->letter_spacing
            || command->image_fit != first->image_fit
            || command->opacity_scale != first->opacity_scale
            || (i != command_start && command->x > right + 1)) {
            return true;
        }
        memcpy(text + used, command->text, command->text_length);
        used += command->text_length;
        right = command->x + command->width;
    }
    const char *retained = layout_retain_generated_text(
        context->layout, text, used);
    if (retained == NULL) return true;
    first = &context->layout->commands[command_start];
    first->text = retained;
    first->text_length = (uint32_t) used;
    first->width = right - first->x;
    context->layout->count = command_start + 1;
    return true;
}


static bool is_atomic_inline(DisplayMode display)
{
    return display == DISPLAY_INLINE_BLOCK
           || display == DISPLAY_INLINE_FLEX
           || display == DISPLAY_INLINE_GRID;
}

/*
 * Atomic inlines form one typographic run: letter spacing belongs between
 * that run and adjacent text, but not between consecutive atomic boxes.
 * Keep this state in the shared line cursor so images, controls, and CSS
 * atomic boxes cannot drift into subtly different boundary behavior.
 */
static int atomic_inline_spacing_fixed(
    LayoutContext *context, const ComputedStyle *parent,
    const LineState *line)
{
    if (context == NULL || parent == NULL || line == NULL
        || line->x == line->start_x || !line->has_text_character) {
        return 0;
    }
    int spacing = 0;
    if (!line->previous_atomic || line->pending_space) {
        spacing = layout_fixed_from_integer(
            line->letter_boundary_spacing);
    }
    if (line->pending_space) {
        const FontFace *face = font_context_face_variant(
            context->fonts, context->web_fonts, parent->font_family,
            parent->font_italic, style_uses_bold_face(parent));
        int space = font_text_width_for_family_at_size_fixed(
            face,
            font_context_metric_family(
                context->web_fonts, parent->font_family, face),
            " ", 1, computed_style_font_size_fixed(parent),
            style_uses_synthetic_weight(
                context->fonts, context->web_fonts, parent, face),
            style_uses_bold_face(parent));
        if (space < 0) {
            space = 6 * parent->font_scale * 64;
        }
        spacing = layout_fixed_add(
            spacing,
            layout_fixed_add(
                space,
                layout_fixed_from_integer(
                    parent->word_spacing
                    + line->pending_space_letter_spacing)));
    }
    return spacing;
}

static void line_note_atomic(LineState *line,
                             const ComputedStyle *parent)
{
    line->pending_space = false;
    line->has_text_character = true;
    line->previous_atomic = true;
    line->letter_boundary_spacing = parent->letter_spacing;
    line->text_generation++;
}

static int measured_flow_text_width_fixed(
    const FontFace *face, FontFamily metric_family,
    const char *text, size_t length, int font_size_fixed,
    bool synthetic_bold, bool metric_bold, int scale, int letter_spacing,
    TextTransformMode transform, bool kerning);

int layout_single_text_advance_fixed(
    LayoutContext *context, lxb_dom_node_t *node,
    const ComputedStyle *style, int used_width)
{
    if (context == NULL || node == NULL || style == NULL
        || style->has_width || node->first_child == NULL
        || node->first_child != node->last_child
        || node->first_child->type != LXB_DOM_NODE_TYPE_TEXT) {
        return layout_fixed_from_integer(used_width);
    }
    size_t length = 0;
    const char *text = document_text_data(node->first_child, &length);
    if (text == NULL) return layout_fixed_from_integer(used_width);
    const FontFace *face = font_context_face_variant(
        context->fonts, context->web_fonts, style->font_family,
        style->font_italic, style_uses_bold_face(style));
    FontFamily metric_family = font_context_metric_family(
        context->web_fonts, style->font_family, face);
    int content = measured_flow_text_width_fixed(
        face, metric_family, text, length,
        computed_style_font_size_fixed(style),
        style_uses_synthetic_weight(
            context->fonts, context->web_fonts, style, face),
        style_uses_bold_face(style), style->font_scale,
        style->letter_spacing, style->text_transform,
        !computed_style_kerning_none(style));
    int edges = style->margin.left + style->margin.right
                + style->border.left + style->border.right
                + style->padding.left + style->padding.right;
    int measured = layout_fixed_add(
        content, layout_fixed_from_integer(edges));
    /* Keep every min/max/shrink-to-fit clamp authoritative. Only retain the
       fractional advance when it rounds to the already-selected used box
       width, so visual geometry and the inline cursor cannot diverge. */
    return layout_fixed_ceil(measured) == used_width
           ? measured : layout_fixed_from_integer(used_width);
}

size_t utf8_codepoints(const char *text, size_t length)
{
    size_t count = 0;
    for (size_t at = 0; at < length;) {
        unsigned codepoint = 0;
        size_t used = font_utf8_next(
            text + at, length - at, &codepoint);
        if (used == 0) break;
        if (!font_codepoint_default_ignorable(codepoint)) count++;
        at += used;
    }
    return count;
}

size_t utf8_character_length(const char *text, size_t available)
{
    if (available == 0) return 0;
    unsigned char lead = (unsigned char) text[0];
    size_t length = lead < 0x80u ? 1
                    : ((lead & 0xe0u) == 0xc0u ? 2
                       : ((lead & 0xf0u) == 0xe0u ? 3
                          : ((lead & 0xf8u) == 0xf0u ? 4 : 1)));
    if (length > available) return 1;
    for (size_t i = 1; i < length; i++) {
        if (((unsigned char) text[i] & 0xc0u) != 0x80u) return 1;
    }
    return length;
}

static bool unicode_line_break_ideograph(unsigned codepoint)
{
    return (codepoint >= 0x2e80u && codepoint <= 0x9fffu)
        || (codepoint >= 0xac00u && codepoint <= 0xd7a3u)
        || (codepoint >= 0xf900u && codepoint <= 0xfaffu)
        || (codepoint >= 0x3040u && codepoint <= 0x30ffu)
        || (codepoint >= 0x31f0u && codepoint <= 0x31ffu)
        || (codepoint >= 0x20000u && codepoint <= 0x3ffffu);
}

static bool unicode_line_break_extender(unsigned codepoint)
{
    return (codepoint >= 0x0300u && codepoint <= 0x036fu)
        || (codepoint >= 0x0591u && codepoint <= 0x05c7u)
        || (codepoint >= 0x0610u && codepoint <= 0x061au)
        || (codepoint >= 0x064bu && codepoint <= 0x065fu)
        || (codepoint >= 0x06d6u && codepoint <= 0x06edu)
        || (codepoint >= 0x08d3u && codepoint <= 0x0903u)
        || (codepoint >= 0x093au && codepoint <= 0x094fu)
        || (codepoint >= 0x0951u && codepoint <= 0x0957u)
        || (codepoint >= 0x0962u && codepoint <= 0x0963u)
        || (codepoint >= 0x1ab0u && codepoint <= 0x1affu)
        || (codepoint >= 0x1dc0u && codepoint <= 0x1dffu)
        || (codepoint >= 0xfe00u && codepoint <= 0xfe0fu)
        || (codepoint >= 0xfe20u && codepoint <= 0xfe2fu)
        || (codepoint >= 0xe0100u && codepoint <= 0xe01efu)
        || codepoint == 0x200du;
}

static size_t utf8_grapheme_length(const char *text, size_t available)
{
    size_t used = utf8_character_length(text, available);
    if (used == 0) return 0;
    bool join_next = false;
    while (used < available) {
        unsigned codepoint = 0;
        size_t next = font_utf8_next(
            text + used, available - used, &codepoint);
        if (next == 0) break;
        if (unicode_line_break_extender(codepoint) || join_next) {
            used += next;
            join_next = codepoint == 0x200du;
            continue;
        }
        break;
    }
    return used;
}

static bool unicode_line_break_closing(unsigned codepoint)
{
    switch (codepoint) {
        case 0x3001u: case 0x3002u: case 0xff01u: case 0xff0cu:
        case 0xff0eu: case 0xff1au: case 0xff1bu: case 0xff1fu:
        case 0x3009u: case 0x300bu: case 0x300du: case 0x300fu:
        case 0x3011u: case 0x3015u: case 0x3017u: case 0x3019u:
        case 0x301bu: case 0xff09u: case 0xff3du: case 0xff5du:
            return true;
        default:
            return false;
    }
}

static bool unicode_line_break_opening(unsigned codepoint)
{
    switch (codepoint) {
        case 0x3008u: case 0x300au: case 0x300cu: case 0x300eu:
        case 0x3010u: case 0x3014u: case 0x3016u: case 0x3018u:
        case 0x301au: case 0xff08u: case 0xff3bu: case 0xff5bu:
            return true;
        default:
            return false;
    }
}

size_t utf8_line_segment_length(const char *text, size_t available,
                                bool keep_cjk_together, bool hyphens_none,
                                bool *discard)
{
    if (discard != NULL) *discard = false;
    if (text == NULL || available == 0) return 0;
    unsigned first = 0;
    size_t first_length = font_utf8_next(text, available, &first);
    if (first_length == 0) return 0;
    /* U+200B creates an ordinary opportunity without ink. Soft hyphen is
       likewise invisible in the unbroken case; visible discretionary-hyphen
       insertion remains outside this bounded breaker. */
    if (first == 0x200bu || (first == 0x00adu && !hyphens_none)) {
        if (discard != NULL) *discard = true;
        return first_length;
    }
    bool ideographic = unicode_line_break_ideograph(first);
    size_t used = first_length;
    bool keep_next = ideographic && unicode_line_break_opening(first);
    while (used < available) {
        unsigned next = 0;
        size_t next_length = font_utf8_next(
            text + used, available - used, &next);
        if (next_length == 0
            || (next < 0x80u && isspace((unsigned char) next))
            || next == 0x200bu
            || (next == 0x00adu && !hyphens_none)) break;
        if (unicode_line_break_extender(next)
            || (ideographic && unicode_line_break_closing(next))) {
            used += next_length;
            continue;
        }
        if (ideographic && !keep_cjk_together) {
            if (!keep_next) break;
            used += next_length;
            keep_next = unicode_line_break_opening(next);
            continue;
        }
        if (!keep_cjk_together
            && unicode_line_break_ideograph(next)) break;
        used += next_length;
    }
    return used;
}

static bool text_span_has_strong_rtl(const char *text, size_t length)
{
    for (size_t at = 0; at < length;) {
        unsigned codepoint = 0;
        size_t used = font_utf8_next(text + at, length - at, &codepoint);
        if (used == 0) break;
        if ((codepoint >= 0x0590u && codepoint <= 0x08ffu)
            || (codepoint >= 0xfb1du && codepoint <= 0xfdffu)
            || (codepoint >= 0xfe70u && codepoint <= 0xfeffu)) return true;
        at += used;
    }
    return false;
}

static size_t text_spacing_units(const char *text, size_t length)
{
    size_t units = 0;
    for (size_t at = 0; at < length;) {
        unsigned codepoint = 0;
        size_t used = utf8_grapheme_length(text + at, length - at);
        if (used == 0) break;
        (void) font_utf8_next(text + at, used, &codepoint);
        if (!font_codepoint_default_ignorable(codepoint)) units++;
        at += used;
    }
    return units;
}

int measured_text_width_fixed_mode(const FontFace *face,
                                     FontFamily metric_family,
                                     const char *text, size_t length,
                                     int font_size_fixed,
                                     bool synthetic_bold, bool metric_bold,
                                     int scale, int letter_spacing,
                                     bool kerning)
{
    int width = font_text_width_for_family_at_size_fixed_mode(
        face, metric_family, text, length, font_size_fixed,
        synthetic_bold, metric_bold, kerning);
    /* CSS letter-spacing is inserted between typographic character units,
       not around default-ignorable formatting controls such as bidi marks,
       joiners, or a zero-width break opportunity. The font path already
       gives these controls zero advance; keep spacing and fallback metrics
       on the same set of visible units. */
    size_t characters = text_spacing_units(text, length);
    if (width < 0) {
        int64_t fallback = (int64_t) characters * 6 * scale * 64;
        width = fallback > INT_MAX ? INT_MAX : (int) fallback;
    }
    int minimum = characters == 0 ? 0 : width / (int) characters;
    if (characters > 1) {
        int64_t spaced = (int64_t) width
            + (int64_t) (characters - 1) * letter_spacing * 64;
        width = spaced > INT_MAX ? INT_MAX
                : (spaced < INT_MIN ? INT_MIN : (int) spaced);
    }
    if (width < minimum) width = minimum;
    return width;
}

int measured_text_width_fixed(const FontFace *face,
                              FontFamily metric_family,
                              const char *text, size_t length,
                              int font_size_fixed,
                              bool synthetic_bold, bool metric_bold,
                              int scale, int letter_spacing)
{
    return measured_text_width_fixed_mode(
        face, metric_family, text, length, font_size_fixed,
        synthetic_bold, metric_bold, scale, letter_spacing, true);
}

int measured_text_width(const FontFace *face,
                               FontFamily metric_family,
                               const char *text, size_t length,
                               int font_size_fixed,
                               bool synthetic_bold, bool metric_bold,
                               int scale, int letter_spacing)
{
    int fixed = measured_text_width_fixed(
        face, metric_family, text, length, font_size_fixed,
        synthetic_bold, metric_bold, scale, letter_spacing);
    return fixed > INT_MAX - 32 ? INT_MAX / 64 : (fixed + 32) / 64;
}

static int measured_flow_text_width_fixed(
    const FontFace *face, FontFamily metric_family,
    const char *text, size_t length, int font_size_fixed,
    bool synthetic_bold, bool metric_bold, int scale, int letter_spacing,
    TextTransformMode transform, bool kerning)
{
    if (transform == TEXT_TRANSFORM_NONE || length > 511) {
        return measured_text_width_fixed_mode(
            face, metric_family, text, length, font_size_fixed,
            synthetic_bold, metric_bold, scale, letter_spacing, kerning);
    }
    char transformed[512];
    memcpy(transformed, text, length);
    for (size_t i = 0; i < length; i++) {
        unsigned char value = (unsigned char) transformed[i];
        bool upper = transform == TEXT_TRANSFORM_UPPERCASE
                     || (transform == TEXT_TRANSFORM_CAPITALIZE && i == 0);
        if (upper && value >= 'a' && value <= 'z') {
            transformed[i] = (char) (value - 'a' + 'A');
        } else if (transform == TEXT_TRANSFORM_LOWERCASE
                   && value >= 'A' && value <= 'Z') {
            transformed[i] = (char) (value - 'A' + 'a');
        }
    }
    return measured_text_width_fixed_mode(
        face, metric_family, transformed, length, font_size_fixed,
        synthetic_bold, metric_bold, scale, letter_spacing, kerning);
}

static size_t fitting_text_prefix(const FontFace *face,
                                  FontFamily metric_family,
                                  const char *text, size_t length,
                                  int font_size_fixed,
                                  bool synthetic_bold, bool metric_bold,
                                  int scale, int letter_spacing,
                                  int available_fixed,
                                  TextTransformMode transform,
                                  bool kerning)
{
    size_t used = 0;
    int width = 0;
    while (used < length) {
        size_t character = utf8_grapheme_length(
            text + used, length - used);
        if (character == 0) break;
        int character_width = measured_flow_text_width_fixed(
            face, metric_family, text + used, character, font_size_fixed,
            synthetic_bold, metric_bold, scale, 0,
            transform == TEXT_TRANSFORM_CAPITALIZE && used != 0
                ? TEXT_TRANSFORM_NONE : transform,
            kerning);
        if (used != 0) {
            int64_t spaced = (int64_t) character_width
                             + (int64_t) letter_spacing * 64;
            character_width = spaced > INT_MAX ? INT_MAX
                : (spaced < INT_MIN ? INT_MIN : (int) spaced);
        }
        if (used != 0
            && (int64_t) width + character_width > available_fixed) break;
        int64_t next_width = (int64_t) width + character_width;
        width = next_width > INT_MAX ? INT_MAX
                : (next_width < INT_MIN ? INT_MIN : (int) next_width);
        used += character;
        if (width > available_fixed) break;
    }
    return used == 0 ? 1 : used;
}

bool layout_add_replaced_alt_text(
    LayoutContext *context, lxb_dom_node_t *node,
    const ComputedStyle *style, int x, int y, int width, int height)
{
    if (context == NULL || node == NULL || style == NULL
        || !layout_node_name_is(node, "img") || width < 3 || height < 3) {
        return true;
    }
    size_t length = 0;
    const char *alt = document_attribute(node, "alt", &length);
    if (alt == NULL || length == 0) return true;
    int inset = width >= 8 ? 2 : 1;
    int available = width - inset * 2;
    const FontFace *face = font_context_face_variant(
        context->fonts, context->web_fonts, style->font_family,
        style->font_italic, style_uses_bold_face(style));
    FontFamily metric_family = font_context_metric_family(
        context->web_fonts, style->font_family, face);
    bool metric_bold = style_uses_bold_face(style);
    bool synthetic_bold = style_uses_synthetic_weight(
        context->fonts, context->web_fonts, style, face);
    int font_size_fixed = computed_style_font_size_fixed(style);
    size_t prefix = fitting_text_prefix(
        face, metric_family, alt, length, font_size_fixed,
        synthetic_bold, metric_bold, style->font_scale,
        style->letter_spacing, layout_fixed_from_integer(available),
        TEXT_TRANSFORM_NONE, !computed_style_kerning_none(style));
    if (prefix > length) prefix = length;
    int line_height = style->line_height > 0 ? style->line_height
        : font_line_height_at_size(face, font_size_fixed);
    if (line_height < 1) line_height = 7 * style->font_scale;
    if (line_height > height) line_height = height;
    DrawCommand fallback = {
        .type = DRAW_TEXT,
        .x = x + inset,
        .y = y + (height - line_height) / 2,
        .width = available,
        .height = line_height,
        .color = style->color,
        .text = alt,
        .text_length = prefix > UINT32_MAX ? UINT32_MAX : (uint32_t) prefix,
        .scale = style->font_scale,
        .font_size = style->font_size,
        .font_family = style->font_family,
        .font_weight = draw_font_weight(style),
        .font_italic = style->font_italic,
        .letter_spacing = style->letter_spacing,
        .radius = (int) style->font_size_fraction
                  << LAYOUT_TEXT_FONT_SIZE_FRACTION_SHIFT
                  | (computed_style_kerning_none(style)
                     ? LAYOUT_TEXT_KERNING_NONE : 0),
        .opacity_scale = alpha_opacity_scale(style->color_alpha)
    };
    return layout_add_command(context->layout, fallback) != NULL;
}

bool flow_text(LayoutContext *context, LineState *line,
                      const char *text, size_t length,
                      const ComputedStyle *style,
                      const char *link_url, size_t link_url_length,
                      lxb_dom_node_t *link_node)
{
    if (line->text_overflow_ended) return true;
    if (line->clamp_pending) {
        bool visible = false;
        for (size_t i = 0; i < length; i++) {
            if (!isspace((unsigned char) text[i])) {
                visible = true;
                break;
            }
        }
        if (visible) return layout_line_clamp_overflow(line);
    }
    if (style->color_alpha < 255 && LAYOUT_TRACE(context->layout, PAINT)
        && context->trace_paint_lines++ < 64) {
        fprintf(stderr, "layout-text-alpha value=%u text=%.*s\n",
                style->color_alpha, (int) (length < 48 ? length : 48), text);
    }
    size_t at = 0;
    size_t previous_decorated_command = SIZE_MAX;
    size_t previous_decorated_link = SIZE_MAX;
    int scale = style->font_scale;
    int font_size_fixed = computed_style_font_size_fixed(style);
    const FontFace *face = font_context_face_variant(
        context->fonts, context->web_fonts, style->font_family,
        style->font_italic, style_uses_bold_face(style));
    bool metric_bold = style_uses_bold_face(style);
    FontFamily metric_family = font_context_metric_family(
        context->web_fonts, style->font_family, face);
    bool synthetic_bold = style_uses_synthetic_weight(
        context->fonts, context->web_fonts, style, face);
    int space_width_fixed = font_text_width_for_family_at_size_fixed(
        face, metric_family, " ", 1, font_size_fixed,
        synthetic_bold, metric_bold);
    if (space_width_fixed < 0) space_width_fixed = 6 * scale * 64;
    space_width_fixed = layout_fixed_add(
        space_width_fixed,
        layout_fixed_from_integer(style->word_spacing));
    if (space_width_fixed < 0) space_width_fixed = 0;
    int line_height_fixed = style->line_height > 0
        ? layout_fixed_from_integer(style->line_height)
        : font_line_height_fixed_at_size(face, font_size_fixed);
    if (line_height_fixed < 0) {
        line_height_fixed = layout_fixed_from_integer(7 * scale);
    }
    int wrap_right = line->right;
    StyleTextWrap text_wrap = computed_style_text_wrap(style);
    int available_pixels = line->right - line->start_x;
    if (text_wrap != STYLE_TEXT_WRAP_NORMAL
        && line_cursor_fixed(line)
               == layout_fixed_from_integer(line->start_x)
        && available_pixels > 0 && length <= 512u) {
        int total_fixed = measured_flow_text_width_fixed(
            face, metric_family, text, length, font_size_fixed,
            synthetic_bold, metric_bold, scale, style->letter_spacing,
            style->text_transform, !computed_style_kerning_none(style));
        int available_fixed = layout_fixed_from_integer(available_pixels);
        int lines = total_fixed <= 0 ? 1
            : (total_fixed + available_fixed - 1) / available_fixed;
        if (lines >= 2 && lines <= 6) {
            int remainder = total_fixed % available_fixed;
            bool improve = text_wrap == STYLE_TEXT_WRAP_BALANCE
                || remainder < available_fixed / 3;
            if (improve) {
                int target_fixed = (total_fixed + lines - 1) / lines;
                /* Two collapsed spaces of slack keep the bounded target
                   from adding a line merely because a word boundary lies
                   just beyond the ideal mathematical division. */
                target_fixed = layout_fixed_add(
                    target_fixed, layout_fixed_add(
                        space_width_fixed, space_width_fixed));
                int minimum_target = available_fixed * 3 / 4;
                if (target_fixed < minimum_target) {
                    target_fixed = minimum_target;
                }
                int target = layout_fixed_ceil(target_fixed);
                if (target >= available_pixels / 2
                    && target < available_pixels) {
                    wrap_right = line->start_x + target;
                }
            }
        }
    }
    bool preserve_spaces = style->white_space_mode == WHITE_SPACE_PRE
        || style->white_space_mode == WHITE_SPACE_PRE_WRAP
        || style->white_space_mode == WHITE_SPACE_BREAK_SPACES;
    bool preserve_newlines = preserve_spaces
        || style->white_space_mode == WHITE_SPACE_PRE_LINE;
    bool wrap_preserved_spaces =
        style->white_space_mode == WHITE_SPACE_BREAK_SPACES;
    struct {
        bool valid;
        size_t at, command_count, link_count;
        int cursor_fixed, line_height, line_height_fixed, line_gap;
        bool pending_space, has_text_character, previous_atomic;
        int8_t letter_boundary_spacing, pending_space_letter_spacing;
        uint32_t text_generation;
    } rollback = {0};
    /*
     * Every pass of either loop below consumes at least one byte of `text`,
     * and the one rewind -- the break-spaces rollback -- disarms itself, so
     * twice the run's length is already unreachable.  It is a ceiling rather
     * than an assertion because the alternative to a wrong line break is a
     * navigation pump that owns the CPU forever: neither loop reaches a
     * cooperate checkpoint, so a line that stopped advancing would take
     * cancel, the HOME exit, and the supervisor's frames down with it.
     */
    size_t flow_guard = length <= (SIZE_MAX - 64u) / 2u
        ? length * 2u + 64u : SIZE_MAX;
    while (at < length) {
        if (line->clamp_pending) return layout_line_clamp_overflow(line);
        if (layout_preview_limit_reached(context, line->y)) break;
        if (flow_guard == 0) {
            fprintf(stderr, "layout-work-limit phase=%s bytes=%zu\n",
                    "inline-text", length);
            break;
        }
        flow_guard--;
        bool separated = false;
        size_t preserved_space_count = 0;
        while (at < length && isspace((unsigned char) text[at])) {
            bool newline = text[at] == '\n' || text[at] == '\r'
                           || text[at] == '\f';
            if (newline && preserve_newlines) {
                while (preserved_space_count != 0) {
                    int cursor_fixed = line_cursor_fixed(line);
                    if (wrap_preserved_spaces
                        && cursor_fixed
                               >= layout_fixed_from_integer(wrap_right)
                        && cursor_fixed
                               != layout_fixed_from_integer(line->start_x)) {
                        layout_flush_line(line);
                        if (line->clamp_pending) {
                            return layout_line_clamp_overflow(line);
                        }
                    }
                    int advance_fixed = space_width_fixed;
                    if (line->has_text_character) {
                        advance_fixed = layout_fixed_add(
                            advance_fixed,
                            layout_fixed_from_integer(
                                line->letter_boundary_spacing));
                    }
                    line_cursor_set_fixed(
                        line, layout_fixed_add(
                                  line_cursor_fixed(line),
                                  advance_fixed));
                    line->has_text_character = true;
                    line->previous_atomic = false;
                    line->letter_boundary_spacing = style->letter_spacing;
                    line->text_generation++;
                    line_height_include_fixed(line, line_height_fixed);
                    preserved_space_count--;
                }
                force_line_break(context, line, style);
                if (line->clamp_pending) {
                    return layout_line_clamp_overflow(line);
                }
                previous_decorated_command = SIZE_MAX;
                previous_decorated_link = SIZE_MAX;
                separated = false;
                if (text[at] == '\r' && at + 1 < length
                    && text[at + 1] == '\n') {
                    at++;
                }
            } else if (preserve_spaces) {
                /* CSS's initial tab-size is eight. This compact renderer
                   preserves that advance without retaining a second text
                   buffer merely to paint blank glyphs. */
                preserved_space_count += text[at] == '\t'
                    ? computed_style_tab_size(style) : 1u;
            } else {
                separated = true;
            }
            at++;
        }
        bool had_preserved_spaces = preserved_space_count != 0;
        bool previous_preserved_space = false;
        while (preserved_space_count != 0) {
            int cursor_fixed = line_cursor_fixed(line);
            if (wrap_preserved_spaces
                && (cursor_fixed
                        > layout_fixed_from_integer(wrap_right)
                    || (previous_preserved_space
                        && cursor_fixed
                           >= layout_fixed_from_integer(wrap_right)))
                && cursor_fixed
                       != layout_fixed_from_integer(line->start_x)) {
                layout_flush_line(line);
                if (line->clamp_pending) {
                    return layout_line_clamp_overflow(line);
                }
            }
            int advance_fixed = space_width_fixed;
            if (line->has_text_character) {
                advance_fixed = layout_fixed_add(
                    advance_fixed,
                    layout_fixed_from_integer(
                        line->letter_boundary_spacing));
            }
            line_cursor_set_fixed(
                line, layout_fixed_add(
                          line_cursor_fixed(line), advance_fixed));
            line->has_text_character = true;
            line->previous_atomic = false;
            line->letter_boundary_spacing = style->letter_spacing;
            line->text_generation++;
            line_height_include_fixed(line, line_height_fixed);
            preserved_space_count--;
            previous_preserved_space = true;
        }
        /*
         * break-spaces creates an opportunity after each preserved run. Keep
         * the latest opportunity that still fits the line so an emergency
         * break can move already-emitted text to the next line instead of
         * splitting at a later, overflowing space. The checkpoint owns no
         * allocation and can only roll back commands emitted by this call.
         */
        if (wrap_preserved_spaces && had_preserved_spaces && at < length
            && line_cursor_fixed(line)
                   <= layout_fixed_from_integer(wrap_right)) {
            rollback.valid = true;
            rollback.at = at;
            rollback.command_count = context->layout->count;
            rollback.link_count = context->layout->link_count;
            rollback.cursor_fixed = line_cursor_fixed(line);
            rollback.line_height = line->line_height;
            rollback.line_height_fixed = line->line_height_fixed;
            rollback.line_gap = line->line_gap;
            rollback.pending_space = line->pending_space;
            rollback.has_text_character = line->has_text_character;
            rollback.previous_atomic = line->previous_atomic;
            rollback.letter_boundary_spacing =
                line->letter_boundary_spacing;
            rollback.pending_space_letter_spacing =
                line->pending_space_letter_spacing;
            rollback.text_generation = line->text_generation;
        }
        if (preserve_spaces) {
            line->pending_space = false;
        } else if (separated) {
            line->pending_space = true;
            line->pending_space_letter_spacing = style->letter_spacing;
        }
        bool discard = false;
        size_t segment = utf8_line_segment_length(
            text + at, length - at,
            computed_style_word_break(style) == WORD_BREAK_KEEP_ALL,
            computed_style_hyphens_none(style), &discard);
        if (segment == 0) break;
        if (discard) {
            at += segment;
            continue;
        }
        size_t end = at + segment;
        if (utf8_codepoints(text + at, segment) == 0) {
            at = end;
            continue;
        }
        int word_width_fixed = measured_flow_text_width_fixed(
            face, metric_family, text + at, end - at, font_size_fixed,
            synthetic_bold, metric_bold, scale, style->letter_spacing,
            style->text_transform, !computed_style_kerning_none(style));
        int boundary_fixed = line->has_text_character
            ? layout_fixed_from_integer(line->letter_boundary_spacing) : 0;
        if (wrap_preserved_spaces && rollback.valid
            && line_cursor_fixed(line)
                   != layout_fixed_from_integer(line->start_x)
            && (int64_t) line_cursor_fixed(line) + boundary_fixed
                       + word_width_fixed
                   > layout_fixed_from_integer(wrap_right)) {
            context->layout->count = rollback.command_count;
            context->layout->link_count = rollback.link_count;
            line_cursor_set_fixed(line, rollback.cursor_fixed);
            line->line_height = rollback.line_height;
            line->line_height_fixed = rollback.line_height_fixed;
            line->line_gap = rollback.line_gap;
            line->pending_space = rollback.pending_space;
            line->has_text_character = rollback.has_text_character;
            line->previous_atomic = rollback.previous_atomic;
            line->letter_boundary_spacing =
                rollback.letter_boundary_spacing;
            line->pending_space_letter_spacing =
                rollback.pending_space_letter_spacing;
            line->text_generation = rollback.text_generation;
            at = rollback.at;
            rollback.valid = false;
            previous_decorated_command = SIZE_MAX;
            previous_decorated_link = SIZE_MAX;
            layout_flush_line(line);
            if (line->clamp_pending) {
                return layout_line_clamp_overflow(line);
            }
            continue;
        }
        int empty_line_width_fixed = layout_fixed_subtract(
            layout_fixed_from_integer(wrap_right),
            layout_fixed_from_integer(line->start_x));
        bool no_wrap = style->white_space_mode == WHITE_SPACE_NOWRAP
            || style->white_space_mode == WHITE_SPACE_PRE;
        bool ellipsis = no_wrap
            && computed_style_text_overflow_ellipsis(style)
            && (computed_style_overflow_x_hidden(style)
                || style->overflow_x_clip_only);
        bool emergency_break = !no_wrap
            && (computed_style_overflow_wrap(style) != OVERFLOW_WRAP_NORMAL
                || computed_style_word_break(style) == WORD_BREAK_LEGACY)
            && word_width_fixed > empty_line_width_fixed;

        /* overflow-wrap is an emergency opportunity, not break-all: first
           take the ordinary whitespace opportunity before the word.  Only a
           word that still cannot fit an empty line may be split. */
        if (emergency_break
            && computed_style_word_break(style) != WORD_BREAK_ALL
            && line_cursor_fixed(line)
                   != layout_fixed_from_integer(line->start_x)) {
            layout_flush_line(line);
            if (line->clamp_pending) {
                return layout_line_clamp_overflow(line);
            }
        }
        bool break_inside = !no_wrap
                            && (computed_style_word_break(style)
                                    == WORD_BREAK_ALL
                                || emergency_break);
        size_t piece_at = at;
        bool first_piece = true;
        bool authored_space_before = line->pending_space;
        while (piece_at < end) {
            if (line->clamp_pending) return layout_line_clamp_overflow(line);
            if (layout_preview_limit_reached(context, line->y)) break;
            if (flow_guard == 0) {
                fprintf(stderr, "layout-work-limit phase=%s bytes=%zu\n",
                        "inline-text-piece", length);
                break;
            }
            flow_guard--;
            int cursor_fixed = line_cursor_fixed(line);
            int start_fixed = layout_fixed_from_integer(line->start_x);
            int space_fixed = 0;
            if (first_piece && cursor_fixed != start_fixed
                && line->has_text_character) {
                space_fixed = layout_fixed_from_integer(
                    line->letter_boundary_spacing);
                if (line->pending_space) {
                    space_fixed = layout_fixed_add(
                        space_fixed,
                        layout_fixed_add(
                            space_width_fixed,
                            layout_fixed_from_integer(
                                line->pending_space_letter_spacing)));
                }
            }
            int available_fixed = layout_fixed_subtract(
                layout_fixed_subtract(
                    layout_fixed_from_integer(wrap_right), cursor_fixed),
                space_fixed);
            size_t piece_end = end;
            int width_fixed;
            bool truncated_for_ellipsis = false;
            if (!no_wrap && break_inside) {
                size_t prefix = fitting_text_prefix(
                    face, metric_family, text + piece_at, end - piece_at,
                    font_size_fixed, synthetic_bold, metric_bold, scale,
                    style->letter_spacing, available_fixed,
                    style->text_transform,
                    !computed_style_kerning_none(style));
                piece_end = piece_at + prefix;
                if (piece_end > end) piece_end = end;
                width_fixed = measured_flow_text_width_fixed(
                    face, metric_family, text + piece_at,
                    piece_end - piece_at, font_size_fixed,
                    synthetic_bold, metric_bold, scale,
                    style->letter_spacing, style->text_transform,
                    !computed_style_kerning_none(style));
                if (width_fixed > available_fixed
                    && cursor_fixed != start_fixed) {
                    layout_flush_line(line);
                    if (line->clamp_pending) {
                        return layout_line_clamp_overflow(line);
                    }
                    cursor_fixed = line_cursor_fixed(line);
                    space_fixed = 0;
                    available_fixed = layout_fixed_subtract(
                        layout_fixed_from_integer(wrap_right),
                        cursor_fixed);
                    prefix = fitting_text_prefix(
                        face, metric_family, text + piece_at,
                        end - piece_at, font_size_fixed,
                        synthetic_bold, metric_bold, scale,
                        style->letter_spacing, available_fixed,
                        style->text_transform,
                        !computed_style_kerning_none(style));
                    piece_end = piece_at + prefix;
                    if (piece_end > end) piece_end = end;
                    width_fixed = measured_flow_text_width_fixed(
                        face, metric_family, text + piece_at,
                        piece_end - piece_at, font_size_fixed,
                        synthetic_bold, metric_bold, scale,
                        style->letter_spacing, style->text_transform,
                        !computed_style_kerning_none(style));
                }
            } else {
                width_fixed = word_width_fixed;
                if (ellipsis
                    && (int64_t) cursor_fixed + space_fixed + width_fixed
                           > layout_fixed_from_integer(wrap_right)) {
                    static const char marker[] = "\xe2\x80\xa6";
                    int marker_width = measured_flow_text_width_fixed(
                        face, metric_family, marker, sizeof(marker) - 1,
                        font_size_fixed, synthetic_bold, metric_bold,
                        scale, style->letter_spacing,
                        TEXT_TRANSFORM_NONE,
                        !computed_style_kerning_none(style));
                    int available = layout_fixed_subtract(
                        layout_fixed_subtract(
                            layout_fixed_subtract(
                                layout_fixed_from_integer(wrap_right),
                                cursor_fixed),
                            space_fixed),
                        marker_width);
                    size_t prefix = available <= 0 ? 0
                        : fitting_text_prefix(
                              face, metric_family, text + piece_at,
                              end - piece_at, font_size_fixed,
                              synthetic_bold, metric_bold, scale,
                              style->letter_spacing, available,
                              style->text_transform,
                              !computed_style_kerning_none(style));
                    if (prefix > end - piece_at) prefix = end - piece_at;
                    piece_end = piece_at + prefix;
                    width_fixed = prefix == 0 ? 0
                        : measured_flow_text_width_fixed(
                              face, metric_family, text + piece_at,
                              prefix, font_size_fixed, synthetic_bold,
                              metric_bold, scale, style->letter_spacing,
                              style->text_transform,
                              !computed_style_kerning_none(style));
                    truncated_for_ellipsis = true;
                }
                if (!no_wrap
                    && (int64_t) cursor_fixed + space_fixed + width_fixed
                           > layout_fixed_from_integer(wrap_right)
                    && cursor_fixed != start_fixed) {
                    layout_flush_line(line);
                    if (line->clamp_pending) {
                        return layout_line_clamp_overflow(line);
                    }
                    cursor_fixed = line_cursor_fixed(line);
                    space_fixed = 0;
                }
            }
            if (piece_end - piece_at > UINT32_MAX) return false;
            int height_fixed = line_height_fixed;
            int height = layout_fixed_ceil(height_fixed);
            (void) line_y_fixed(line);
            int command_x_fixed = layout_fixed_add(
                cursor_fixed, space_fixed);
            int command_x = layout_fixed_floor(command_x_fixed);
            int command_right = layout_fixed_ceil(
                layout_fixed_add(command_x_fixed, width_fixed));
            int64_t command_width_wide = (int64_t) command_right - command_x;
            int command_width = command_width_wide <= 0 ? 0
                : (command_width_wide > INT_MAX
                   ? INT_MAX : (int) command_width_wide);
            DrawCommand command = {
                .type = DRAW_TEXT,
                .x = command_x,
                .y = line->y + (style->vertical_align == VERTICAL_SUPER
                                ? -style->font_size / 3
                                : (style->vertical_align == VERTICAL_SUB
                                   ? style->font_size / 4
                                   : (style->vertical_align == VERTICAL_MIDDLE
                                      ? -style->font_size / 8 : 0))),
                .width = command_width,
                .height = height,
                .color = style->color,
                .text = text + piece_at,
                .text_length = (uint32_t) (piece_end - piece_at),
                .scale = scale,
                .font_size = style->font_size,
                .font_family = style->font_family,
                .font_weight = draw_font_weight(style),
                .font_italic = style->font_italic,
                .letter_spacing = style->letter_spacing,
                .radius = layout_fixed_fraction(command_x_fixed)
                    | ((int) style->font_size_fraction
                       << LAYOUT_TEXT_FONT_SIZE_FRACTION_SHIFT)
                    | ((int) style->text_transform
                       << LAYOUT_TEXT_TRANSFORM_SHIFT)
                    | ((computed_style_direction_rtl(style)
                        || (!style->unicode_bidi_override
                            && text_span_has_strong_rtl(
                                text + piece_at,
                                piece_end - piece_at)))
                       ? LAYOUT_TEXT_RTL : 0),
                .image_fit = text_decoration_bits(style),
                .opacity_scale = alpha_opacity_scale(style->color_alpha)
            };
            if (computed_style_kerning_none(style)) {
                command.radius |= LAYOUT_TEXT_KERNING_NONE;
            }
            if (first_piece && authored_space_before)
                command.radius |= LAYOUT_TEXT_FIND_SPACE_BEFORE;
            if (line->find_block_start)
                command.radius |= LAYOUT_TEXT_FIND_BLOCK_START;
            bool underlined = draw_command_text_has_underline(&command);
            if (underlined && space_fixed > 0
                && previous_decorated_command < context->layout->count) {
                DrawCommand *previous = &context->layout->commands[
                    previous_decorated_command];
                int desired_right = layout_fixed_ceil(command_x_fixed);
                int64_t previous_right = (int64_t) previous->x
                                         + previous->width;
                int extension = desired_right > previous_right
                    && desired_right - previous_right <= INT_MAX
                    ? (int) (desired_right - previous_right) : 0;
                if (previous->type == DRAW_TEXT
                    && draw_command_text_has_underline(previous)
                    && previous->y == command.y
                    && extension > 0
                    && previous->width <= INT_MAX - extension) {
                    previous->width += extension;
                    if (previous_decorated_link
                        < context->layout->link_count) {
                        LinkRegion *previous_link = &context->layout->links[
                            previous_decorated_link];
                        size_t previous_url_length =
                            previous_link->url_length
                            & LAYOUT_LINK_URL_LENGTH_MASK;
                        bool same_url = previous_url_length
                                        == link_url_length
                            && (link_url_length == 0
                                || (previous_link->url != NULL
                                    && link_url != NULL
                                    && memcmp(previous_link->url, link_url,
                                              link_url_length) == 0));
                        if (previous_link->node == link_node && same_url
                            && previous_link->width
                                   <= INT_MAX - extension) {
                            previous_link->width += extension;
                        }
                    }
                }
            }
            size_t link_index = context->layout->link_count;
            DrawCommand *stored = NULL;
            if (piece_end != piece_at) {
                if (!layout_add_text_shadow_commands(
                        context, style, &command)) {
                    return false;
                }
                size_t command_index = context->layout->count;
                stored = layout_add_command(context->layout, command);
                if (stored == NULL
                    || !layout_add_link(
                           context->layout, stored, command_index,
                           link_url, link_url_length, link_node)) {
                    return false;
                }
                line->find_block_start = false;
                if (underlined) {
                    previous_decorated_command = command_index;
                }
            }
            if (underlined && stored != NULL) {
                previous_decorated_link = context->layout->link_count
                                          > link_index
                                          ? link_index : SIZE_MAX;
            } else {
                previous_decorated_command = SIZE_MAX;
                previous_decorated_link = SIZE_MAX;
            }
            line_cursor_set_fixed(
                line, layout_fixed_add(command_x_fixed, width_fixed));
            line->has_text_character = true;
            line->previous_atomic = false;
            line->letter_boundary_spacing = style->letter_spacing;
            line->text_generation++;
            line_height_include_fixed(line, height_fixed);
            if (style->line_height > 0) line->line_gap = 0;
            line->pending_space = false;
            if (truncated_for_ellipsis) {
                static const char marker[] = "\xe2\x80\xa6";
                ComputedStyle marker_style = *style;
                marker_style.overflow_wrap = (OverflowWrap) (
                    marker_style.overflow_wrap
                    & ~STYLE_TEXT_OVERFLOW_ELLIPSIS);
                if (!flow_text(context, line, marker, sizeof(marker) - 1,
                               &marker_style, link_url, link_url_length,
                               link_node)) {
                    return false;
                }
                line->text_overflow_ended = true;
                return true;
            }
            piece_at = piece_end;
            first_piece = false;
            if (piece_at < end) layout_flush_line(line);
        }
        at = end;
    }
    return true;
}

static lxb_dom_node_t *layout_fallback_next(lxb_dom_node_t *node,
                                             lxb_dom_node_t *root,
                                             bool descend)
{
    if (node == NULL) return NULL;
    if (descend && node->first_child != NULL) return node->first_child;
    while (node != NULL && node != root) {
        if (node->next != NULL) return node->next;
        node = node->parent;
    }
    return NULL;
}

static bool layout_fallback_consume_work(LayoutContext *context,
                                         lxb_dom_node_t *node)
{
    if (context == NULL || context->cancelled) return false;
    if (context->fallback_visits >= LAYOUT_FALLBACK_VISIT_LIMIT) {
        fprintf(stderr,
                "layout-work-limit phase=depth-fallback limit=%u\n",
                (unsigned) LAYOUT_FALLBACK_VISIT_LIMIT);
        context->cancelled = true;
        return false;
    }
    context->fallback_visits++;
    return layout_cooperate(context, node);
}

/* Preserve readable content once the full CSS state machine reaches its
   native-call ceiling.  This is intentionally generic: descendants use the
   last safely computed inherited style, anchors remain interactive, and the
   traversal is constant-stack.  Geometry beyond the boundary is approximate
   but content is neither silently dropped nor allowed to exhaust the PSP
   stack. */
__attribute__((noinline))
bool flow_subtree_fallback(LayoutContext *context,
                                  lxb_dom_node_t *root,
                                  const ComputedStyle *style,
                                  LineState *line,
                                  const char *inherited_link_url,
                                  size_t inherited_link_length,
                                  lxb_dom_node_t *inherited_link_node)
{
    lxb_dom_node_t *node = root;
    while (node != NULL) {
        if (layout_preview_limit_reached(context, line->y)) break;
        if (!layout_fallback_consume_work(context, node)) return false;
        ComputedStyle node_style;
        bool style_hides_subtree = false;
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            layout_style_for_node_into(
                context, node, style, &node_style);
            style_hides_subtree = node_style.display == DISPLAY_NONE
                || node_style.hidden;
        }
        bool skip_subtree = node->type == LXB_DOM_NODE_TYPE_ELEMENT
            && (layout_node_name_is(node, "script") || layout_node_name_is(node, "style")
                || layout_node_name_is(node, "template")
                || layout_node_name_is(node, "head")
                || style_hides_subtree
                || lxb_dom_element_has_attribute(
                       lxb_dom_interface_element(node),
                       (const lxb_char_t *) "hidden", 6));
        if (skip_subtree) {
            node = layout_fallback_next(node, root, false);
            continue;
        }
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT
            && layout_node_name_is(node, "br")) {
            force_line_break(context, line, style);
            node = layout_fallback_next(node, root, false);
            continue;
        }
        if (node->type != LXB_DOM_NODE_TYPE_TEXT) {
            node = layout_fallback_next(node, root, true);
            continue;
        }
        size_t length = 0;
        const char *text = document_text_data(node, &length);
        if (text == NULL || length == 0) {
            node = layout_fallback_next(node, root, false);
            continue;
        }
        const char *link_url = inherited_link_url;
        size_t link_length = inherited_link_length;
        lxb_dom_node_t *link_node = inherited_link_node;
        for (lxb_dom_node_t *at = node->parent; at != NULL;
             at = at->parent) {
            /* A comb-shaped deep tree can otherwise turn one bounded node
               walk into quadratic unaccounted ancestry work. */
            if (!layout_fallback_consume_work(context, at)) return false;
            if (layout_node_name_is(at, "a")) {
                size_t href_length = 0;
                const char *href = document_attribute(
                    at, "href", &href_length);
                if (href != NULL && href_length != 0) {
                    link_url = href;
                    link_length = href_length;
                    link_node = at;
                }
                break;
            }
            if (at == root) break;
        }
        if (!flow_text(context, line, text, length, style, link_url,
                       link_length, link_node)) return false;
        node = layout_fallback_next(node, root, false);
    }
    return true;
}

__attribute__((noinline))
static bool flow_inline_fallback(LayoutContext *context,
                                 lxb_dom_node_t *node,
                                 const ComputedStyle *parent,
                                 LineState *line,
                                 const char *link_url,
                                 size_t link_url_length,
                                 lxb_dom_node_t *link_node)
{
    return flow_subtree_fallback(context, node, parent, line, link_url,
                                 link_url_length, link_node);
}

static bool flow_inline_impl(LayoutContext *context, lxb_dom_node_t *node,
                             const ComputedStyle *parent, LineState *line,
                             const char *link_url, size_t link_url_length,
                             lxb_dom_node_t *link_node,
                             bool *resolved_hidden);

bool flow_inline(LayoutContext *context, lxb_dom_node_t *node,
                        const ComputedStyle *parent, LineState *line,
                        const char *link_url, size_t link_url_length,
                        lxb_dom_node_t *link_node)
{
    if (line != NULL && line->text_overflow_ended) return true;
    size_t command_start = context == NULL || context->layout == NULL
        ? 0 : context->layout->count;
    size_t link_start = context == NULL || context->layout == NULL
        ? 0 : context->layout->link_count;
    size_t control_start = context == NULL || context->layout == NULL
        ? 0 : context->layout->control_count;
    if (!layout_tree_enter(context, node, "inline")) {
        bool success = context != NULL && !context->cancelled
            && flow_inline_fallback(context, node, parent, line, link_url,
                                    link_url_length, link_node);
        if (success && parent != NULL && parent->visibility_hidden
            && !layout_record_visibility_range(
                context, command_start, link_start, control_start, true)) {
            success = false;
        }
        return success;
    }
    bool resolved_hidden = parent != NULL && parent->visibility_hidden;
    bool success = flow_inline_impl(context, node, parent, line, link_url,
                                    link_url_length, link_node,
                                    &resolved_hidden);
    if (success && parent != NULL
        && resolved_hidden != parent->visibility_hidden
        && !layout_record_visibility_range(
            context, command_start, link_start, control_start,
            resolved_hidden)) {
        success = false;
    }
    layout_tree_leave(context);
    return success && !context->cancelled;
}

static bool flow_inline_impl(LayoutContext *context, lxb_dom_node_t *node,
                             const ComputedStyle *parent, LineState *line,
                             const char *link_url, size_t link_url_length,
                             lxb_dom_node_t *link_node,
                             bool *resolved_hidden)
{
    if (layout_preview_limit_reached(context, line->y)) return true;
    if (node->type == LXB_DOM_NODE_TYPE_TEXT) {
        size_t length = 0;
        const char *text = document_text_data(node, &length);
        for (size_t i = 0; text != NULL && i < length; i++) {
            if (!isspace((unsigned char) text[i])) {
                line->first_inline_block_collapses_top = false;
                break;
            }
        }
        return text == NULL || flow_text(context, line, text, length, parent,
                                         link_url, link_url_length, link_node);
    }
    if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) return true;
    ComputedStyle style = layout_style_for_node(context, node, parent);
    if (resolved_hidden != NULL) {
        *resolved_hidden = style.visibility_hidden;
    }
    resolve_padding(context->sheet, &style,
                    line->base_right - line->base_left);
    resolve_margin(context->sheet, &style,
                   line->base_right - line->base_left);
    if (style.color_alpha < 255 && LAYOUT_TRACE(context->layout, PAINT)
        && context->trace_paint_lines++ < 64) {
        size_t id_length = 0, class_length = 0;
        const char *id = document_attribute(node, "id", &id_length);
        const char *class_name = document_attribute(node, "class",
                                                     &class_length);
        fprintf(stderr, "layout-inline-alpha value=%u id=%.*s class=%.*s\n",
                style.color_alpha,
                (int) id_length, id == NULL ? "" : id,
                (int) class_length, class_name == NULL ? "" : class_name);
    }
    if (style.opacity < 255 && LAYOUT_TRACE(context->layout, PAINT)
        && context->trace_paint_lines++ < 64) {
        size_t id_length = 0, class_length = 0;
        const char *id = document_attribute(node, "id", &id_length);
        const char *class_name = document_attribute(node, "class",
                                                     &class_length);
        fprintf(stderr, "layout-opacity value=%u id=%.*s class=%.*s\n",
                style.opacity,
                (int) id_length, id == NULL ? "" : id,
                (int) class_length, class_name == NULL ? "" : class_name);
    }
    if (LAYOUT_TRACE(context->layout, LAYOUT)) {
        size_t class_length = 0;
        const char *class_name = document_attribute(node, "class",
                                                     &class_length);
        if (class_name != NULL && strstr(class_name, "wm-") != NULL) {
            fprintf(stderr, "layout-node class=%.*s display=%d hidden=%d out=%d fixed=%d x=%d y=%d width=%d\n",
                    (int) class_length, class_name, style.display,
                    style.hidden, style.out_of_flow, style.fixed_position,
                    line->x, line->y, line->right - line->start_x);
        }
    }
    if (style.display == DISPLAY_NONE
        || style.display == DISPLAY_TABLE_COLUMN || style.hidden) return true;
    if (style.out_of_flow || style.fixed_position) {
        bool fixed_captured = style.fixed_position
            && line->positioned_box.fixed_node != NULL;
        int containing_width = style.fixed_position
            ? (fixed_captured ? line->positioned_box.fixed_width
                              : context->layout->width)
            : line->positioned_box.width;
        int containing_height = style.fixed_position
            ? (fixed_captured && line->positioned_box.fixed_height > 0
                   ? line->positioned_box.fixed_height
                   : (context->layout->viewport.css_height > 0
                      ? context->layout->viewport.css_height : 272))
            : line->positioned_box.height;
        if (containing_width <= 0) {
            containing_width = line->base_right - line->base_left;
        }
        int ignored_bottom = line->y;
        /* Absolutely positioned descendants do not advance the inline
           formatting cursor, but they still generate their box.  Keep the
           static-position origin at the current cursor while resolving
           percentages against the nearest retained positioned padding box. */
        return layout_block(context, node, parent, line->x, line->y,
                            containing_width, containing_height, false,
                            &line->positioned_box, &ignored_bottom);
    }
    if (layout_node_name_is(node, "br")) {
        /*
         * `clear` applies to a BR's forced break.  Merely flushing a normal
         * line leaves the cursor in a float exclusion band, so the next
         * row can continue beside the floats instead of below them.  A
         * clearing BR has no independently visible line box: finish any
         * preceding inline content, then advance directly to the matching
         * float bottom.
         */
        if (style.clear_mode != CLEAR_NONE) {
            if (line->line_height != 0 || line->line_height_fixed != 0
                || line->x != line->start_x) {
                layout_flush_line(line);
            }
            clear_line_floats(line, style.clear_mode);
            return true;
        }
        force_line_break(context, line, parent);
        return true;
    }
    if (layout_node_name_is(node, "a")) {
        size_t href_length = 0;
        const char *href = document_attribute(node, "href", &href_length);
        if (href != NULL && href_length != 0) {
            link_url = href;
            link_url_length = href_length;
            link_node = node;
        }
    }
    bool input_control = layout_node_name_is(node, "input");
    bool textarea_control = layout_node_name_is(node, "textarea");
    bool button_control = layout_node_name_is(node, "button");
    bool select_control = layout_node_name_is(node, "select");
    bool label_control = layout_node_name_is(node, "label");
    size_t editable_length = 0;
    const char *editable_value = document_attribute(
        node, "contenteditable", &editable_length);
    bool editable_control = lxb_dom_element_has_attribute(
        lxb_dom_interface_element(node),
        (const lxb_char_t *) "contenteditable", 15)
        && !(editable_value != NULL && editable_length == 5
             && strncasecmp(editable_value, "false", 5) == 0);
    if (is_block_display(style.display)
        && style.float_mode == FLOAT_NONE) {
        /*
         * A block descendant splits its inline ancestor into anonymous
         * block fragments.  The inline box itself contributes no atomic
         * dimensions; place the descendant in the containing block's full
         * inline band and resume a fresh anonymous line after it.
         */
        bool collapse_top =
            line->first_inline_block_collapses_top
            && line->x == line->start_x
            && line->line_height == 0
            && line->line_height_fixed == 0;
        line->first_inline_block_collapses_top = false;
        layout_flush_line(line);
        line_finish_vertical(line);
        int block_y = line->y;
        if (collapse_top) {
            block_y = layout_subtract_coordinate(
                block_y,
                layout_collapsed_block_top_margin(
                    context, node, &style));
        }
        int block_bottom = block_y;
        if (!layout_block(
                context, node, parent, line->base_left, block_y,
                line->base_right - line->base_left, 0, false,
                &line->positioned_box, &block_bottom)) {
            return false;
        }
        line->y = block_bottom;
        line->y_fixed_valid = false;
        line->line_height = 0;
        line->line_height_fixed = 0;
        update_float_bounds(line);
        line_cursor_set(line, line->start_x);
        line->command_start = context->layout->count;
        line->link_start = context->layout->link_count;
        line->control_start = context->layout->control_count;
        line->node_box_start = context->layout->node_box_count;
        return true;
    }
    if (style.float_mode != FLOAT_NONE) {
        return layout_place_float(
            context, node, parent, &style, line,
            line->positioned_box.height);
    }
    bool replaced_element = layout_node_name_is(node, "img")
        || layout_node_name_is(node, "svg")
        || layout_node_name_is(node, "video")
        || layout_node_name_is(node, "audio")
        || layout_node_name_is(node, "iframe");
    if (is_atomic_inline(style.display) && !input_control
        && !textarea_control && !editable_control && !replaced_element) {
        int atomic_span_height = style_pixel_height(
            context->sheet, &style,
            line->base_right - line->base_left);
        if (atomic_span_height > 0) {
            if (!style.box_sizing_border_box) {
                atomic_span_height += style.padding.top
                    + style.padding.bottom + style.border.top
                    + style.border.bottom;
            }
            atomic_span_height += style.margin.top + style.margin.bottom;
        }
        if (atomic_span_height > 0
            && line->x == line->start_x && line->line_height == 0
            && line->line_height_fixed == 0) {
            int first_line_indent = line->first_line_indent;
            update_float_bounds_for_span(line, atomic_span_height);
            if (first_line_indent != 0) {
                line->start_x = layout_add_coordinate(
                    line->start_x, first_line_indent);
                line_cursor_set(line, line->start_x);
            }
        }
        int available = line->right - line->start_x;
        int margins = style.margin.left + style.margin.right;
        int width;
        bool explicit_width = style.has_width;
        if (style.width_max_content) {
            if (computed_style_width_min_content(&style)) {
                width = intrinsic_min_text_width_ignoring_own_width(
                    context, node, parent, available);
            } else {
                int limit = computed_style_width_max_content(&style)
                    ? LAYOUT_COORDINATE_LIMIT : available;
                width = intrinsic_text_width(context, node, parent, limit);
            }
            if (width < margins) width = margins;
        } else if (style.has_width) {
            width = resolve_declared_length(
                context->sheet, style.width, style.width_percent, available);
            int edges = style.padding.left + style.padding.right
                        + style.border.left + style.border.right;
            if (!style.box_sizing_border_box) width += edges;
            width = constrain_border_box_width(
                context, node, parent, &style, available, width, NULL);
            width += margins;
        } else {
            width = intrinsic_text_width(context, node, parent, available);
            if (width < margins) width = margins;
            width = constrain_border_box_width(
                context, node, parent, &style, available,
                width - margins, NULL) + margins;
        }
        if (width < 8) width = 8;
        /* A specified inline size may overflow its containing line. Auto
           shrink-to-fit remains bounded by the available inline size. */
        if (!explicit_width && width > available) width = available;
        int spacing_fixed = atomic_inline_spacing_fixed(
            context, parent, line);
        if (parent->white_space_mode != WHITE_SPACE_NOWRAP
            && parent->white_space_mode != WHITE_SPACE_PRE
            && line->x != line->start_x
            && (int64_t) line_cursor_fixed(line) + spacing_fixed
                       + (int64_t) width * 64
                   > (int64_t) line->right * 64) {
            layout_flush_line(line);
            if (atomic_span_height > 0) {
                update_float_bounds_for_span(line, atomic_span_height);
            }
            spacing_fixed = 0;
        }
        int atomic_x = layout_fixed_ceil(
            layout_fixed_add(line_cursor_fixed(line), spacing_fixed));
        /* A wrapped atomic inline begins at the exact accumulated line
           boundary. Its retained integer box must round that boundary up,
           just as a block boundary does, rather than painting one pixel
           above the line when prior normal line heights were fractional. */
        int atomic_y = layout_fixed_ceil(line_y_fixed(line));
        int atomic_bottom = atomic_y;
        if (!layout_block(context, node, parent, atomic_x, atomic_y, width,
                          0, true, &line->positioned_box,
                          &atomic_bottom)) return false;
        const LayoutNodeBox *box = layout_box_for_node(context->layout, node);
        int atomic_height = atomic_bottom - line->y;
        if (atomic_height < 1) atomic_height = 1;
        if (link_node == node && link_url != NULL && link_url_length != 0) {
            DrawCommand region = {.x = atomic_x, .y = atomic_y,
                                  .width = width, .height = atomic_height};
            if (!layout_add_link(
                    context->layout, &region, SIZE_MAX,
                    link_url, link_url_length, link_node)) return false;
        }
        if (button_control || select_control) {
            int control_x = box != NULL ? box->x : atomic_x;
            int control_y = box != NULL ? box->y : atomic_y;
            int control_width = box != NULL ? box->width : width;
            int control_height = box != NULL ? box->height : atomic_height;
            bool already_registered = false;
            for (size_t i = 0; i < context->layout->control_count; i++) {
                if (context->layout->controls[i].node == node) {
                    already_registered = true;
                    break;
                }
            }
            if (!already_registered
                && !layout_add_control(
                    context->layout, control_x, control_y,
                    control_width, control_height,
                    button_control ? CONTROL_BUTTON : CONTROL_SELECT,
                    node)) return false;
        }
        int atomic_advance_fixed = layout_single_text_advance_fixed(
            context, node, &style, width);
        line_cursor_set_fixed(
            line, layout_fixed_add(
                layout_fixed_from_integer(atomic_x),
                atomic_advance_fixed));
        line_height_include_fixed(
            line, layout_fixed_from_integer(atomic_height));
        line_note_atomic(line, parent);
        return true;
    }
    if (input_control || textarea_control || editable_control) {
        size_t inline_control_command_start = context->layout->count;
        size_t inline_control_link_start = context->layout->link_count;
        size_t inline_control_region_start = context->layout->control_count;
        size_t type_length = 0;
        const char *input_type = input_control
            ? document_attribute(node, "type", &type_length) : NULL;
        ControlType input_control_type = input_control
            ? layout_input_control_type(node) : CONTROL_INPUT;
        if (layout_node_is_hidden_input(node)) return true;
        int available = line->right - line->start_x;
        int horizontal_edges = style.padding.left + style.padding.right
                               + style.border.left + style.border.right;
        int vertical_edges = style.padding.top + style.padding.bottom
                             + style.border.top + style.border.bottom;
        bool definite_width = style.has_width && !style.width_max_content;
        int default_width = layout_control_default_width(node);
        if (default_width <= 0) default_width = 220;
        int specified_width = definite_width
            ? resolve_declared_length(context->sheet, style.width,
                                      style.width_percent, available)
            : default_width;
        int outer_width = definite_width && !style.box_sizing_border_box
                          ? specified_width + horizontal_edges
                          : specified_width;
        int resized_width = 0, resized_height = 0;
        bool has_resized_control = textarea_control
            && style.resize_mode != STYLE_RESIZE_NONE
            && document_control_resize(
                node, &resized_width, &resized_height);
        if (has_resized_control
            && (style.resize_mode == STYLE_RESIZE_BOTH
                || style.resize_mode == STYLE_RESIZE_HORIZONTAL)) {
            outer_width = resized_width;
            definite_width = true;
        }
        int maximum_outer_width = available - style.margin.left
                                  - style.margin.right;
        int minimum_outer_width = input_control_type == CONTROL_TOGGLE
            ? horizontal_edges + 18 : 32;
        if (maximum_outer_width < minimum_outer_width) {
            maximum_outer_width = minimum_outer_width;
        }
        if (outer_width > maximum_outer_width) {
            outer_width = maximum_outer_width;
        }
        if (outer_width < minimum_outer_width) {
            outer_width = minimum_outer_width;
        }
        int content_width = outer_width - horizontal_edges;
        if (content_width < 1) content_width = 1;
        int specified_height = style_pixel_height(context->sheet, &style,
                                                  available);
        int outer_height = 0;
        if (specified_height > 0) {
            outer_height = style.box_sizing_border_box
                           ? specified_height
                           : specified_height + vertical_edges;
        } else if (textarea_control || editable_control) {
            outer_height = 64;
        } else if (input_control_type == CONTROL_TOGGLE) {
            outer_height = 18 + vertical_edges;
        } else if (input_control_type == CONTROL_RANGE) {
            outer_height = 20 + vertical_edges;
        } else {
            /* Native single-line text controls size their content box from
               the used font size, not from a downloadable face's internal
               line gap. This keeps their UA geometry stable across fallback
               faces and matches the 22px Chromium text input at 16px. */
            int text_height = layout_fixed_ceil(
                computed_style_font_size_fixed(&style));
            if (text_height < 1) text_height = 7 * style.font_scale;
            outer_height = text_height + vertical_edges;
        }
        if (has_resized_control
            && (style.resize_mode == STYLE_RESIZE_BOTH
                || style.resize_mode == STYLE_RESIZE_VERTICAL)) {
            outer_height = resized_height;
        }
        if (outer_height < vertical_edges + 1) {
            outer_height = vertical_edges + 1;
        }
        int content_height = outer_height - vertical_edges;
        int spacing_fixed = atomic_inline_spacing_fixed(
            context, parent, line);
        int advance = style.margin.left + outer_width + style.margin.right;
        if (line->x != line->start_x
            && (int64_t) line_cursor_fixed(line) + spacing_fixed
                       + (int64_t) advance * 64
                   > (int64_t) line->right * 64) {
            layout_flush_line(line);
            spacing_fixed = 0;
        }
        int control_start_x = layout_fixed_ceil(
            layout_fixed_add(line_cursor_fixed(line), spacing_fixed));
        int outer_x = control_start_x + style.margin.left;
        int outer_y = line->y + style.margin.top;
        int content_x = outer_x + style.border.left + style.padding.left;
        int content_y = outer_y + style.border.top + style.padding.top;
        bool special_input = false;
        if (input_control
            && !layout_paint_special_input(
                context, node, &style, outer_x, outer_y,
                outer_width, outer_height, &special_input)) {
            return false;
        }
        if (!special_input) {
        int border_radius_code = stylesheet_border_radius_code(
            context->sheet, &style);
        DrawCommand background = {
            .type = DRAW_FILL_RECT,
            .x = outer_x + style.border.left,
            .y = outer_y + style.border.top,
            .width = outer_width - style.border.left - style.border.right,
            .height = outer_height - style.border.top - style.border.bottom,
            .color = style.background,
            .radius = style_border_radius_maximum(border_radius_code) > 0
                ? border_radius_code : 5,
            .opacity_scale = alpha_opacity_scale(style.background_alpha)
        };
        /* The UA cascade already supplies the native control surface.
           `has_background == false` therefore means an author explicitly
           selected transparent (or appearance:none), not that layout should
           paint a second white fallback. That fallback used to cover sprite-
           backed submit buttons whose transparent hit target sits above the
           authored parent surface. */
        if (style.has_background
            && layout_add_command(context->layout, background) == NULL) {
            return false;
        }
        size_t value_length = 0;
        const char *value = document_control_value(node, &value_length);
        bool placeholder_value = false;
        if (value == NULL) {
            value = input_control
                ? document_attribute(node, "value", &value_length)
                : first_text_data(node, &value_length);
        }
        if (input_control_type == CONTROL_TOGGLE
            || input_control_type == CONTROL_RANGE) {
            value = NULL;
            value_length = 0;
        } else if (value == NULL || value_length == 0) {
            value = document_attribute(node, "placeholder", &value_length);
            placeholder_value = value != NULL && value_length != 0;
        }
        if ((value == NULL || value_length == 0)
            && input_control_type == CONTROL_BUTTON
            && input_type != NULL) {
            if (type_length == 4
                && strncasecmp(input_type, "file", 4) == 0) {
                value = "CHOOSE FILE";
                value_length = 11;
            } else if ((type_length == 6
                        && strncasecmp(input_type, "submit", 6) == 0)
                       || (type_length == 5
                           && strncasecmp(input_type, "image", 5) == 0)) {
                value = "SUBMIT";
                value_length = 6;
            } else if (type_length == 5
                       && strncasecmp(input_type, "reset", 5) == 0) {
                value = "RESET";
                value_length = 5;
            }
        }
        static const char password_mask[] = "********************************";
        if (input_type != NULL && type_length == 8
            && memcmp(input_type, "password", 8) == 0
            && value != NULL && value_length != 0) {
            if (value_length > sizeof(password_mask) - 1) {
                value_length = sizeof(password_mask) - 1;
            }
            value = password_mask;
        }
        if (value != NULL && value_length != 0) {
            if (value_length > UINT32_MAX) return false;
            int text_indent = 0;
            if (!style_length_resolve(
                    context->sheet, style.text_indent,
                    content_width, &text_indent)) {
                text_indent = 0;
            }
            DrawCommand text = {
                .type = DRAW_TEXT,
                .x = layout_add_coordinate(content_x, text_indent),
                .y = content_y
                     + (content_height - 7 * style.font_scale) / 2,
                .width = content_width,
                .height = 7 * style.font_scale,
                .color = placeholder_value ? 0x70757a : style.color,
                .text = value,
                .text_length = (uint32_t) value_length,
                .scale = style.font_scale,
                .font_size = style.font_size,
                .font_family = style.font_family,
                .font_weight = draw_font_weight(&style),
                .font_italic = style.font_italic,
                .letter_spacing = style.letter_spacing,
                .radius = (int) style.font_size_fraction
                          << LAYOUT_TEXT_FONT_SIZE_FRACTION_SHIFT
                          | (computed_style_kerning_none(&style)
                             ? LAYOUT_TEXT_KERNING_NONE : 0),
                .image_fit = text_decoration_bits(&style),
                .opacity_scale = alpha_opacity_scale(style.color_alpha)
            };
            if (layout_add_command(context->layout, text) == NULL) return false;
        }
        if (style.border.top > 0) {
            DrawCommand border = {
                .type = DRAW_FILL_RECT, .x = outer_x, .y = outer_y,
                .width = outer_width, .height = style.border.top,
                .color = style.border_color,
                .opacity_scale = alpha_opacity_scale(style.border_alpha)
            };
            if (layout_add_command(context->layout, border) == NULL) return false;
        }
        if (style.border.bottom > 0) {
            DrawCommand border = {
                .type = DRAW_FILL_RECT, .x = outer_x,
                .y = outer_y + outer_height - style.border.bottom,
                .width = outer_width, .height = style.border.bottom,
                .color = style.border_color,
                .opacity_scale = alpha_opacity_scale(style.border_alpha)
            };
            if (layout_add_command(context->layout, border) == NULL) return false;
        }
        if (style.border.left > 0) {
            DrawCommand border = {
                .type = DRAW_FILL_RECT, .x = outer_x, .y = outer_y,
                .width = style.border.left, .height = outer_height,
                .color = style.border_color,
                .opacity_scale = alpha_opacity_scale(style.border_alpha)
            };
            if (layout_add_command(context->layout, border) == NULL) return false;
        }
        if (style.border.right > 0) {
            DrawCommand border = {
                .type = DRAW_FILL_RECT,
                .x = outer_x + outer_width - style.border.right,
                .y = outer_y, .width = style.border.right,
                .height = outer_height, .color = style.border_color,
                .opacity_scale = alpha_opacity_scale(style.border_alpha)
            };
            if (layout_add_command(context->layout, border) == NULL) return false;
        }
        }
        ControlType control_type = input_control ? input_control_type
                                   : (textarea_control ? CONTROL_TEXTAREA
                                                       : CONTROL_EDITABLE);
        if (!layout_add_control(context->layout, outer_x, outer_y,
                         outer_width, outer_height,
                         control_type, node)) return false;
        if (textarea_control && style.resize_mode != STYLE_RESIZE_NONE) {
            int handle = 12;
            if (!layout_add_control(
                    context->layout,
                    outer_x + (outer_width > handle
                               ? outer_width - handle : 0),
                    outer_y + (outer_height > handle
                               ? outer_height - handle : 0),
                    outer_width < handle ? outer_width : handle,
                    outer_height < handle ? outer_height : handle,
                    CONTROL_RESIZE, node)) return false;
        }
        if (!apply_visual_range(
                context, node, inline_control_command_start,
                inline_control_link_start, inline_control_region_start,
                &style, false)) {
            return false;
        }
        line_cursor_set(line, control_start_x + advance);
        int line_height = style.margin.top + outer_height
                          + style.margin.bottom;
        line_height_include_fixed(
            line, layout_fixed_from_integer(line_height));
        line_note_atomic(line, parent);
        return true;
    }
    if (layout_node_name_is(node, "img") || layout_node_name_is(node, "svg")
        || layout_node_name_is(node, "video")
        || layout_node_name_is(node, "audio")
        || layout_node_name_is(node, "iframe")) {
        bool audio_element = layout_node_name_is(node, "audio");
        bool audio_controls = audio_element
            && lxb_dom_element_has_attribute(
                lxb_dom_interface_element(node),
                (const lxb_char_t *) "controls", 8);
        size_t replaced_link_start = context->layout->link_count;
        size_t replaced_control_start = context->layout->control_count;
        const ImageResource *image = images_find_node(context->images, node);
        bool image_available = image_resource_available(image)
                               && image->width > 0 && image->height > 0;
        if (layout_node_name_is(node, "img") && !image_available) {
            size_t source_length = 0;
            const char *source = document_attribute(
                node, "src", &source_length);
            if (source == NULL || source_length == 0) {
                source = document_attribute(
                    node, "srcset", &source_length);
            }
            if (source != NULL && source_length != 0) {
                layout_note_unresolved_external_visual(
                    context, node, NULL, IMAGE_PRIORITY_KIND_DOCUMENT,
                    PSEUDO_NONE);
            }
        }
        int width = image_available
            ? image_resource_intrinsic_width(image) : 0;
        int height = image_available
            ? image_resource_intrinsic_height(image) : 0;
        if ((layout_node_name_is(node, "video")
             || audio_controls
             || layout_node_name_is(node, "iframe"))
            && (width <= 0 || height <= 0)) {
            width = 300;
            height = audio_controls ? 54 : 150;
        }
        int horizontal_edges = style.padding.left + style.padding.right
                               + style.border.left + style.border.right;
        int vertical_edges = style.padding.top + style.padding.bottom
                             + style.border.top + style.border.bottom;
        int declared_width = html_dimension_attribute(node, "width");
        int declared_height = html_dimension_attribute(node, "height");
        int intrinsic_height = height;
        /* A zero dimension attribute is an explicit request for a degenerate
           replaced box, not an unknown size.  Spacer images such as
           <img src="s.gif" height="10" width="0"> paint nothing but still
           reserve their height on the line, and a
           zero-wide box could never show alt text, so both fallbacks below
           would silently swallow the spacing the author asked for. */
        bool zero_width_spacer = declared_width == 0
                                 && (style.has_height
                                     || declared_height > 0);
        /* Positive HTML dimensions already entered ComputedStyle as
           presentational hints before the author cascade. Reapplying them
           here made a later `img { height:auto }` ineffective and distorted
           responsive logos. The separate attribute read is retained only
           for the explicit zero-width spacer contract above. */
        bool declared_box = style.has_width && style.has_height;
        if (!image_available && !declared_box && !zero_width_spacer
            && !layout_node_name_is(node, "svg")
            && !layout_node_name_is(node, "video")
            && !layout_node_name_is(node, "audio")
            && !layout_node_name_is(node, "iframe")) {
            size_t alt_length = 0;
            const char *alt = layout_node_name_is(node, "img")
                ? document_attribute(node, "alt", &alt_length) : NULL;
            return alt == NULL || alt_length == 0
                   || flow_text(context, line, alt, alt_length, &style,
                                link_url, link_url_length, link_node);
        }
        bool styled_width = style.has_width;
        if (styled_width) {
            int styled_width = resolve_declared_length(
                context->sheet, style.width, style.width_percent,
                line->right - line->start_x);
            if (styled_width > 0) {
                int content_width = style.box_sizing_border_box
                                    ? styled_width - horizontal_edges
                                    : styled_width;
                if (content_width < 1) content_width = 1;
                if (width > 0) {
                    height = style.aspect_width > 0
                             && style.aspect_height > 0
                        ? layout_scale_dimension(
                            style.aspect_height, content_width,
                            style.aspect_width)
                        : layout_scale_dimension(
                            height, content_width, width);
                }
                width = content_width;
            }
        }
        int styled_height = style_pixel_height(context->sheet, &style, width);
        if (styled_height > 0) {
            int content_height = style.box_sizing_border_box
                                 ? styled_height - vertical_edges
                                 : styled_height;
            if (content_height < 1) content_height = 1;
            if (!styled_width && height > 0) {
                width = style.aspect_width > 0
                        && style.aspect_height > 0
                    ? layout_scale_dimension(
                        style.aspect_width, content_height,
                        style.aspect_height)
                    : layout_scale_dimension(
                        width, content_height, height);
            }
            height = content_height;
        } else if (!style.has_height && style.aspect_width > 0
                   && style.aspect_height > 0 && width > 0) {
            height = layout_scale_dimension(
                style.aspect_height, width, style.aspect_width);
        }
        if (style.max_height == STYLE_LENGTH_MIN_CONTENT
            && intrinsic_height > 0 && height > intrinsic_height) {
            height = intrinsic_height;
        }
        constrain_replaced_content_size(
            context, node, parent, &style,
            line->right - line->start_x, 0,
            style.has_width, style.has_height,
            &width, &height);
        if ((width <= 0 && !zero_width_spacer) || height <= 0) {
            size_t alt_length = 0;
            const char *alt = layout_node_name_is(node, "img")
                ? document_attribute(node, "alt", &alt_length) : NULL;
            return alt == NULL || alt_length == 0
                   || flow_text(context, line, alt, alt_length, &style,
                                link_url, link_url_length, link_node);
        }
        int available = line->right - line->start_x;
        int fixed_width = horizontal_edges + style.margin.left
                          + style.margin.right;
        int maximum_content_width = available - fixed_width;
        if (maximum_content_width < 1) maximum_content_width = 1;
        if (width > maximum_content_width) {
            height = layout_scale_dimension(
                height, maximum_content_width, width);
            width = maximum_content_width;
        }
        int outer_width = width + horizontal_edges;
        int outer_height = height + vertical_edges;
        int advance = style.margin.left + outer_width + style.margin.right;
        int spacing_fixed = atomic_inline_spacing_fixed(
            context, parent, line);
        if (line->x != line->start_x
            && (int64_t) line_cursor_fixed(line) + spacing_fixed
                       + (int64_t) advance * 64
                   > (int64_t) line->right * 64) {
            layout_flush_line(line);
            spacing_fixed = 0;
        }
        if (height < 1) height = 1;
        int replaced_start_x = layout_fixed_ceil(
            layout_fixed_add(line_cursor_fixed(line), spacing_fixed));
        int outer_x = replaced_start_x + style.margin.left;
        int outer_y = line->y + style.margin.top;
        int content_x = outer_x + style.border.left + style.padding.left;
        int content_y = outer_y + style.border.top + style.padding.top;
        if (style.has_background) {
            int border_radius_code = stylesheet_border_radius_code(
                context->sheet, &style);
            DrawCommand background = {
                .type = DRAW_FILL_RECT,
                .x = outer_x + style.border.left,
                .y = outer_y + style.border.top,
                .width = outer_width - style.border.left
                         - style.border.right,
                .height = outer_height - style.border.top
                          - style.border.bottom,
                .color = style.background,
                .radius = border_radius_code,
                .opacity_scale = alpha_opacity_scale(style.background_alpha)
            };
            if (layout_add_command(context->layout, background) == NULL) return false;
        }
        size_t image_command = context->layout->count;
        if (image_available) {
            int border_radius_code = stylesheet_border_radius_code(
                context->sheet, &style);
            DrawCommand command = {
                .type = DRAW_IMAGE, .x = content_x, .y = content_y,
                .width = width, .height = height, .image = image,
                .image_fit = style.object_fit,
                .scale = border_radius_code
            };
            draw_command_set_object_position(
                &command, style.object_position_x, style.object_position_y);
            DrawCommand *stored = layout_add_command(
                context->layout, command);
            if (stored == NULL
                || !layout_add_link(
                       context->layout, stored, image_command,
                       link_url, link_url_length, link_node)) {
                return false;
            }
            image_command = (size_t) (
                stored - context->layout->commands);
        } else if (!layout_add_replaced_alt_text(
                       context, node, &style, content_x, content_y,
                       width, height)) {
            return false;
        }
        if (audio_controls
            && !layout_paint_audio_control(
                context, &style, outer_x, outer_y,
                outer_width, outer_height)) return false;
        /* Page media is a single native-player activation target. It uses
           the existing retained control map so d-pad focus and pointer taps
           share the same DOM activation/cancellation path as buttons. */
        if ((layout_node_name_is(node, "video") || audio_controls)
            && !layout_add_control(
                   context->layout, outer_x, outer_y,
                   outer_width, outer_height, CONTROL_BUTTON, node)) {
            return false;
        }
        /* Inline replaced elements need a queryable box: page scripts
           (lazy-image loaders, IntersectionObserver clients) read their
           geometry, which only block layout used to register. */
        {
            if (!add_node_box(context->layout, node, outer_x, outer_y,
                              outer_width, outer_height, width, height,
                              width, height,
                              style.padding.left + style.padding.right,
                              style.padding.top + style.padding.bottom,
                              false, false, 0, 0,
                              style.border.left, style.border.top, false,
                              false, false, true,
                              image_command,
                              image_command + (image_available ? 1u : 0u),
                              image_command,
                              image_command,
                              replaced_link_start,
                              context->layout->link_count,
                              replaced_control_start,
                              context->layout->control_count)) {
                return false;
            }
        }
        if (style.border.top > 0) {
            DrawCommand border = {
                .type = DRAW_FILL_RECT, .x = outer_x, .y = outer_y,
                .width = outer_width, .height = style.border.top,
                .color = style.border_color,
                .opacity_scale = alpha_opacity_scale(style.border_alpha)
            };
            if (layout_add_command(context->layout, border) == NULL) return false;
        }
        if (style.border.bottom > 0) {
            DrawCommand border = {
                .type = DRAW_FILL_RECT, .x = outer_x,
                .y = outer_y + outer_height - style.border.bottom,
                .width = outer_width, .height = style.border.bottom,
                .color = style.border_color,
                .opacity_scale = alpha_opacity_scale(style.border_alpha)
            };
            if (layout_add_command(context->layout, border) == NULL) return false;
        }
        if (style.border.left > 0) {
            DrawCommand border = {
                .type = DRAW_FILL_RECT, .x = outer_x, .y = outer_y,
                .width = style.border.left, .height = outer_height,
                .color = style.border_color,
                .opacity_scale = alpha_opacity_scale(style.border_alpha)
            };
            if (layout_add_command(context->layout, border) == NULL) return false;
        }
        if (style.border.right > 0) {
            DrawCommand border = {
                .type = DRAW_FILL_RECT,
                .x = outer_x + outer_width - style.border.right,
                .y = outer_y, .width = style.border.right,
                .height = outer_height, .color = style.border_color,
                .opacity_scale = alpha_opacity_scale(style.border_alpha)
            };
            if (layout_add_command(context->layout, border) == NULL) return false;
        }
        line_cursor_set(line, replaced_start_x + advance);
        int line_height = style.margin.top + outer_height + style.margin.bottom;
        line_height_include_fixed(
            line, layout_fixed_from_integer(line_height));
        line_note_atomic(line, parent);
        return true;
    }
    const ImageResource *mask = style.mask_image != NULL
        ? images_find_mask_source(
            context->images, node, style.mask_image, PSEUDO_NONE)
        : images_find_mask_node(context->images, node);
    if (style.mask_image != NULL && style.mask_image[0] != '\0'
        && !image_resource_available(mask)) {
        layout_note_unresolved_external_visual(
            context, node, style.mask_image, IMAGE_PRIORITY_KIND_MASK,
            PSEUDO_NONE);
    }
    if (image_resource_available(mask)) {
        int width = style.has_width && !style.width_max_content
                    ? resolve_declared_length(
                        context->sheet, style.width, style.width_percent,
                        line->right - line->start_x)
                    : mask->width;
        int height = style_pixel_height(context->sheet, &style,
                                        line->right - line->start_x);
        if (height <= 0) height = mask->height;
        int minimum_width = style_minimum_width(
            context->sheet, &style, line->right - line->start_x);
        if (width < minimum_width) width = minimum_width;
        int minimum_height = 0;
        if (!style.min_height_percent
            && resolve_computed_length(
                context->sheet, style.min_height, false,
                line->right - line->start_x, &minimum_height)
            && height < minimum_height) {
            height = minimum_height;
        }
        if (width <= 0 || height <= 0) return true;
        int spacing_fixed = atomic_inline_spacing_fixed(
            context, parent, line);
        if (line->x != line->start_x
            && (int64_t) line_cursor_fixed(line) + spacing_fixed
                       + (int64_t) width * 64
                   > (int64_t) line->right * 64) {
            layout_flush_line(line);
            spacing_fixed = 0;
        }
        int mask_x = layout_fixed_ceil(
            layout_fixed_add(line_cursor_fixed(line), spacing_fixed));
        size_t pseudo_insertion_index = context->layout->count;
        int border_radius_code = stylesheet_border_radius_code(
            context->sheet, &style);
        DrawCommand command = {.type = DRAW_IMAGE, .x = mask_x,
                               .y = line->y, .width = width, .height = height,
                               .color = style.has_background
                                        ? style.background : style.color,
                               .image = mask,
                               .scale = border_radius_code,
                               .opacity_scale = alpha_opacity_scale(
                                   style.has_background
                                   ? style.background_alpha
                                   : style.color_alpha)};
        DrawCommand *stored = layout_add_command(context->layout, command);
        if (stored == NULL
            || !layout_add_link(
                   context->layout, stored, pseudo_insertion_index,
                   link_url, link_url_length, link_node)) return false;
        if (!paint_pseudo(context, node, &style, PSEUDO_BEFORE,
                          command.x, command.y, command.width, command.height,
                          pseudo_insertion_index, 0)
            || !paint_pseudo(context, node, &style, PSEUDO_AFTER,
                             command.x, command.y, command.width,
                             command.height, SIZE_MAX, 0)) return false;
        line_cursor_set(line, mask_x + width);
        line_height_include_fixed(
            line, layout_fixed_from_integer(height));
        line_note_atomic(line, parent);
        return true;
    }
    int pseudo_reference_width = line->right - line->start_x;
    GeneratedPseudoFlow inline_before = generated_pseudo_flow(
        context, node, &style, PSEUDO_BEFORE,
        pseudo_reference_width, 0);
    GeneratedPseudoFlow inline_after = generated_pseudo_flow(
        context, node, &style, PSEUDO_AFTER,
        pseudo_reference_width, 0);
    if (!flow_generated_block_pseudo(context, node, &style, PSEUDO_BEFORE,
                                     line, &inline_before)) {
        return false;
    }
    if (!inline_before.active
        && !flow_generated_inline_pseudo(
            context, node, &style, PSEUDO_BEFORE, line, link_url,
            link_url_length, link_node, NULL)) {
        return false;
    }
    int control_x = line->x;
    int control_y = line->y;
    size_t inline_command_start = context->layout->count;
    size_t inline_link_start = context->layout->link_count;
    size_t inline_control_start = context->layout->control_count;
    /* Inline boxes contribute their horizontal margin/border/padding around
       their content (nav pills and tab rows render unseparated otherwise).
       Vertical edges stay out of inline
       line-height per CSS. */
    int inline_left_edges = style.margin.left + style.border.left
                            + style.padding.left;
    int inline_right_edges = style.padding.right + style.border.right
                             + style.margin.right;
    /* An inline nowrap box establishes one unbreakable group.  Measuring
       only each descendant text node lets the first word enter the current
       line and strands the rest beyond its right edge.  Preflight the
       bounded intrinsic width while the group can still move as a unit. */
    if ((style.white_space_mode == WHITE_SPACE_NOWRAP
         || style.white_space_mode == WHITE_SPACE_PRE)
        && parent->white_space_mode != WHITE_SPACE_NOWRAP
        && parent->white_space_mode != WHITE_SPACE_PRE
        && line_cursor_fixed(line)
               != layout_fixed_from_integer(line->start_x)) {
        int line_width = line->right - line->start_x;
        int group_width = intrinsic_text_width(
            context, node, parent, line_width);
        int spacing_fixed = atomic_inline_spacing_fixed(
            context, parent, line);
        if (group_width > 0
            && (int64_t) line_cursor_fixed(line) + spacing_fixed
                       + (int64_t) group_width * 64
                   > (int64_t) line->right * 64) {
            layout_flush_line(line);
        }
    }
    if (inline_left_edges > 0) {
        line_cursor_set(line, line->x + inline_left_edges);
    }
    if (select_control) {
        lxb_dom_node_t *option = select_display_option(node);
        if (option != NULL
            && !flow_inline(context, option, &style, line,
                            link_url, link_url_length, link_node)) return false;
    } else {
        bool has_flowed_text_child = false;
        for (lxb_dom_node_t *child = node->first_child; child != NULL;
             child = child->next) {
            if (has_flowed_text_child && line->has_text_character) {
                line->letter_boundary_spacing = style.letter_spacing;
            }
            uint32_t text_generation = line->text_generation;
            if (!flow_inline(context, child, &style, line,
                             link_url, link_url_length, link_node)) return false;
            if (line->text_generation != text_generation) {
                has_flowed_text_child = true;
            }
        }
    }
    if (!inline_after.active
        && !flow_generated_inline_pseudo(
            context, node, &style, PSEUDO_AFTER, line, link_url,
            link_url_length, link_node, NULL)) {
        return false;
    }
    if (!flow_generated_block_pseudo(context, node, &style, PSEUDO_AFTER,
                                     line, &inline_after)) {
        return false;
    }
    if (inline_right_edges > 0) {
        line_cursor_set(line, line->x + inline_right_edges);
        line->pending_space = false;
    }
    bool inline_box_visual =
        style.border.top > 0 || style.border.right > 0
        || style.border.bottom > 0 || style.border.left > 0
        || (computed_style_outline_width(&style) != 0
            && computed_style_outline_style(&style)
                   != STYLE_OUTLINE_NONE);
    bool rtl_fragment = computed_style_direction_rtl(&style)
                        && !computed_style_direction_rtl(parent)
                        && line->y == control_y;
    if (rtl_fragment
        && !coalesce_rtl_text_fragment(
            context, inline_command_start, inline_link_start)) {
        return false;
    }
    if (inline_box_visual && line->y == control_y) {
        if (style.border.top > 0 || style.border.right > 0
            || style.border.bottom > 0 || style.border.left > 0) {
            line_height_include_fixed(
                line, inline_style_line_height_fixed(context, &style));
        }
        int fragment_x = control_x + style.margin.left;
        int fragment_width = line->x - control_x
                             - style.margin.left - style.margin.right;
        if (!paint_inline_strokes(
                context, &style, fragment_x, control_y,
                fragment_width, inline_command_start)) {
            return false;
        }
    }
    if (rtl_fragment) {
        int left_fixed = layout_fixed_from_integer(control_x);
        int right_fixed = line_cursor_fixed(line);
        for (size_t i = inline_command_start;
             i < context->layout->count; i++) {
            DrawCommand *command = &context->layout->commands[i];
            if (command->type == DRAW_TEXT) {
                draw_command_set_text_x_fixed(
                    command,
                    layout_fixed_add(
                        left_fixed,
                        layout_fixed_subtract(
                            layout_fixed_subtract(
                                right_fixed,
                                draw_command_text_x_fixed(command)),
                            layout_fixed_from_integer(command->width))));
            } else {
                command->x = layout_add_coordinate(
                    control_x,
                    layout_fixed_ceil(right_fixed)
                        - command->x - command->width);
            }
        }
        int right = layout_fixed_ceil(right_fixed);
        for (size_t i = inline_link_start;
             i < context->layout->link_count; i++) {
            LinkRegion *link = &context->layout->links[i];
            link->x = layout_add_coordinate(
                control_x, right - link->x - link->width);
        }
    }
    if (button_control || select_control || label_control) {
        int width = line->x - control_x;
        int height = line->line_height;
        if (width < 12) width = 12;
        if (height < 12) height = 12;
        if (!layout_add_control(context->layout, control_x, control_y, width, height,
                         select_control ? CONTROL_SELECT : CONTROL_BUTTON,
                         node)) return false;
    }
    size_t id_length = 0;
    const char *id = document_attribute(node, "id", &id_length);
    if (id != NULL && id_length != 0 && line->y == control_y) {
        int box_x = control_x + style.margin.left;
        int box_width = line->x - control_x
                        - style.margin.left - style.margin.right;
        if (box_width < 0) box_width = 0;
        if (!add_node_box(
                context->layout, node, box_x, control_y,
                box_width, line->line_height,
                box_width, line->line_height,
                box_width, line->line_height,
                style.padding.left + style.padding.right,
                style.padding.top + style.padding.bottom,
                false, false, 0, 0, 0, 0, false, false, 0, true,
                inline_command_start, context->layout->count,
                inline_command_start, context->layout->count,
                inline_link_start, context->layout->link_count,
                inline_control_start,
                context->layout->control_count)) {
            return false;
        }
    }
    return true;
}
