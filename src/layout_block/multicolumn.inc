#define LAYOUT_MULTICOLUMN_LIMIT 8u
#define LAYOUT_MULTICOLUMN_ITEM_LIMIT 2048u

typedef struct {
    size_t item_count;
    int column_width;
    int gap;
    uint8_t count;
    bool active;
} MulticolumnLayout;

static void multicolumn_trim(const char **text, size_t *length)
{
    while (*length != 0 && isspace((unsigned char) (*text)[0])) {
        (*text)++;
        (*length)--;
    }
    while (*length != 0
           && isspace((unsigned char) (*text)[*length - 1])) {
        (*length)--;
    }
}

static bool multicolumn_case_equal(const char *text, size_t length,
                                   const char *wanted)
{
    return strlen(wanted) == length
        && strncasecmp(text, wanted, length) == 0;
}

static bool multicolumn_parse_count(const char *text, size_t length,
                                    int *count)
{
    multicolumn_trim(&text, &length);
    if (length == 0 || length >= 16
        || multicolumn_case_equal(text, length, "auto")) return false;
    char copy[16];
    memcpy(copy, text, length);
    copy[length] = '\0';
    char *end = NULL;
    long parsed = strtol(copy, &end, 10);
    if (end == copy || *end != '\0' || parsed < 1
        || parsed > (long) LAYOUT_MULTICOLUMN_LIMIT) return false;
    *count = (int) parsed;
    return true;
}

static bool multicolumn_parse_length(const ComputedStyle *style,
                                     const char *text, size_t length,
                                     int *pixels)
{
    multicolumn_trim(&text, &length);
    if (length == 0 || length >= 48
        || multicolumn_case_equal(text, length, "auto")
        || multicolumn_case_equal(text, length, "normal")) return false;
    char copy[48];
    memcpy(copy, text, length);
    copy[length] = '\0';
    char *end = NULL;
    double value = strtod(copy, &end);
    if (end == copy || value < 0.0) return false;
    double scale = 1.0;
    if (*end == '\0' || strcasecmp(end, "px") == 0) {
        scale = 1.0;
    } else if (strcasecmp(end, "em") == 0) {
        scale = (double) computed_style_font_size_fixed(style) / 64.0;
    } else if (strcasecmp(end, "rem") == 0) {
        scale = (double) computed_style_root_font_size_fixed(style) / 64.0;
    } else if (strcasecmp(end, "ch") == 0) {
        scale = (double) computed_style_font_size_fixed(style) / 128.0;
    } else {
        return false;
    }
    double used = value * scale;
    if (used > INT_MAX) return false;
    *pixels = (int) (used + 0.5);
    return true;
}

static bool multicolumn_inline_candidate(const Stylesheet *sheet,
                                         lxb_dom_node_t *node)
{
    if (sheet != NULL && sheet->block_inline_style_attributes
        && !document_style_attribute_cssom_authorized(node)) return false;
    size_t length = 0;
    const char *style = document_attribute(node, "style", &length);
    if (style == NULL) return false;
    static const char wanted[] = "column";
    for (size_t i = 0; i + sizeof(wanted) - 1u <= length; i++) {
        size_t j = 0;
        while (j < sizeof(wanted) - 1u
               && tolower((unsigned char) style[i + j])
                  == wanted[j]) j++;
        if (j == sizeof(wanted) - 1u) return true;
    }
    return false;
}

static void multicolumn_parse_shorthand(const ComputedStyle *style,
                                        const char *value,
                                        int *count, int *width)
{
    size_t length = strlen(value);
    for (size_t at = 0; at < length;) {
        while (at < length && isspace((unsigned char) value[at])) at++;
        size_t start = at;
        unsigned nesting = 0;
        while (at < length) {
            if (value[at] == '(') nesting++;
            else if (value[at] == ')' && nesting != 0) nesting--;
            else if (nesting == 0
                     && isspace((unsigned char) value[at])) break;
            at++;
        }
        if (at == start) break;
        int parsed = 0;
        if (multicolumn_parse_count(value + start, at - start, &parsed)) {
            *count = parsed;
        } else if (multicolumn_parse_length(
                       style, value + start, at - start, &parsed)) {
            *width = parsed;
        }
    }
}

static MulticolumnLayout multicolumn_prepare(
    LayoutContext *context, lxb_dom_node_t *node,
    const ComputedStyle *style, int content_width)
{
    MulticolumnLayout result = {0};
    if (context == NULL || context->sheet == NULL || node == NULL
        || style == NULL || content_width <= 0
        || (style->display != DISPLAY_BLOCK
            && style->display != DISPLAY_FLOW_ROOT
            && style->display != DISPLAY_INLINE_BLOCK)
        || (!context->sheet->has_multicolumn_rules
            && !multicolumn_inline_candidate(context->sheet, node))) {
        return result;
    }

    char shorthand[96] = {0};
    char count_value[96] = {0};
    char width_value[96] = {0};
    char gap_value[96] = {0};
    int requested_count = 0;
    int requested_width = 0;
    if (style_retained_presentation_value(
            context->sheet, node, "columns", 7,
            shorthand, sizeof(shorthand))) {
        multicolumn_parse_shorthand(
            style, shorthand, &requested_count, &requested_width);
    }
    if (style_retained_presentation_value(
            context->sheet, node, "column-count", 12,
            count_value, sizeof(count_value))) {
        int parsed = 0;
        requested_count = multicolumn_parse_count(
            count_value, strlen(count_value), &parsed) ? parsed : 0;
    }
    if (style_retained_presentation_value(
            context->sheet, node, "column-width", 12,
            width_value, sizeof(width_value))) {
        int parsed = 0;
        requested_width = multicolumn_parse_length(
            style, width_value, strlen(width_value), &parsed) ? parsed : 0;
    }
    if (requested_count == 0 && requested_width == 0) return result;

    int gap = (computed_style_font_size_fixed(style) + 32) / 64;
    if (style_retained_presentation_value(
            context->sheet, node, "column-gap", 10,
            gap_value, sizeof(gap_value))) {
        int parsed = 0;
        if (multicolumn_parse_length(
                style, gap_value, strlen(gap_value), &parsed)) gap = parsed;
    }
    if (gap < 0) gap = 0;
    int fit = requested_width > 0
        ? (content_width + gap) / (requested_width + gap) : requested_count;
    if (fit < 1) fit = 1;
    if (fit > (int) LAYOUT_MULTICOLUMN_LIMIT) {
        fit = LAYOUT_MULTICOLUMN_LIMIT;
    }
    int count = requested_count > 0
        ? requested_count : fit;
    if (requested_width > 0 && count > fit) count = fit;
    if (count < 2) return result;

    FlexItemIterator iterator;
    FlatItem item;
    flex_iterator_init(&iterator, context, node, style, NULL);
    iterator.source.include_whitespace = false;
    while (flex_iterator_next(&iterator, &item)) {
        if (item.style.out_of_flow || item.style.fixed_position) continue;
        if (item.anonymous_text || item.style.float_mode != FLOAT_NONE
            || !is_block_display(item.style.display)) {
            return (MulticolumnLayout) {0};
        }
        if (++result.item_count > LAYOUT_MULTICOLUMN_ITEM_LIMIT) {
            return (MulticolumnLayout) {0};
        }
        if ((result.item_count & 31u) == 0
            && !layout_cooperate(context, item.node)) {
            return (MulticolumnLayout) {0};
        }
    }
    if (result.item_count < 2) return (MulticolumnLayout) {0};
    if ((size_t) count > result.item_count) count = (int) result.item_count;
    int available = content_width - gap * (count - 1);
    if (available < count * 8) return (MulticolumnLayout) {0};
    result.column_width = available / count;
    result.gap = gap;
    result.count = (uint8_t) count;
    result.active = true;
    return result;
}

static bool multicolumn_redistribute(
    LayoutContext *context, lxb_dom_node_t *node,
    const ComputedStyle *style, const MulticolumnLayout *multi,
    int content_x, int content_top, int definite_height, int *flow_bottom)
{
    if (multi == NULL || !multi->active || flow_bottom == NULL) return true;
    int total = 0;
    FlexItemIterator iterator;
    FlatItem item;
    flex_iterator_init(&iterator, context, node, style, NULL);
    iterator.source.include_whitespace = false;
    while (flex_iterator_next(&iterator, &item)) {
        if (item.style.out_of_flow || item.style.fixed_position) continue;
        LayoutNodeBox *box = layout_box_for_node_mutable(
            context->layout, item.node);
        if (box == NULL) continue;
        resolve_margin(context->sheet, &item.style, multi->column_width);
        int outer = item.style.margin.top + box->height
                    + item.style.margin.bottom;
        if (outer < 0) outer = 0;
        total = layout_add_coordinate(total, outer);
    }
    int target = definite_height > 0 ? definite_height
        : (total + multi->count - 1) / multi->count;
    if (target < 1) target = 1;

    size_t item_index = 0;
    unsigned column = 0;
    int used = 0;
    int maximum = 0;
    flex_iterator_init(&iterator, context, node, style, NULL);
    iterator.source.include_whitespace = false;
    while (flex_iterator_next(&iterator, &item)) {
        if (item.style.out_of_flow || item.style.fixed_position) continue;
        LayoutNodeBox *box = layout_box_for_node_mutable(
            context->layout, item.node);
        if (box == NULL) continue;
        resolve_margin(context->sheet, &item.style, multi->column_width);
        int outer = item.style.margin.top + box->height
                    + item.style.margin.bottom;
        if (outer < 0) outer = 0;
        size_t remaining_items = multi->item_count - item_index;
        unsigned remaining_columns = multi->count - column - 1u;
        if (column + 1u < multi->count && used != 0
            && (used + outer > target
                || remaining_items <= remaining_columns)) {
            if (used > maximum) maximum = used;
            column++;
            used = 0;
        }
        unsigned physical_column = computed_style_direction_rtl(style)
            ? multi->count - column - 1u : column;
        int target_x = content_x
            + (int) physical_column * (multi->column_width + multi->gap)
            + item.style.margin.left;
        int target_y = content_top + used + item.style.margin.top;
        int dx = target_x - box->x;
        int dy = target_y - box->y;
        if (dx != 0 || dy != 0) {
            translate_node_subtree(context->layout, item.node, dx, dy);
        }
        used = layout_add_coordinate(used, outer);
        item_index++;
        if ((item_index & 31u) == 0
            && !layout_cooperate(context, item.node)) return false;
    }
    if (used > maximum) maximum = used;
    *flow_bottom = layout_add_coordinate(content_top, maximum);
    return true;
}
