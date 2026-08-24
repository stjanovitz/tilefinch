/* Bounded paragraph bidi integration. Ordinary LTR blocks never enter this
   file beyond the cheap subtree scan performed by layout_bidi_flow_create. */

#include "layout_internal.h"
#include "tilefinch/platform.h"
#include "tilefinch/text_bidi.h"

#include <stdint.h>
#include <string.h>

#define LAYOUT_BIDI_SPAN_LIMIT 512u
#define LAYOUT_BIDI_FLOW_COMMAND_LIMIT 1024u
#define LAYOUT_BIDI_ATOMIC_LIMIT 256u
#define LAYOUT_BIDI_SCAN_NODE_LIMIT 4096u

_Static_assert(TEXT_BIDI_PARAGRAPH_BYTE_LIMIT <= UINT16_MAX,
               "bidi glyph logical offsets must cover an admitted paragraph");

typedef struct {
    const char *source;
    uint32_t source_length;
    uint32_t paragraph_byte_start;
} LayoutBidiSourceSpan;

typedef struct {
    uint32_t command_index;
    int32_t logical_advance_fixed;
    int32_t space_advance_fixed;
    uint16_t unit_start;
    uint16_t unit_count;
} LayoutBidiFlowCommand;

typedef struct {
    lxb_dom_node_t *node;
    uint32_t paragraph_byte_start;
    uint32_t command_start, command_end;
    uint32_t link_start, link_end;
    uint32_t control_start, control_end;
    uint32_t node_box_start, node_box_end;
    int32_t logical_start_fixed;
    int32_t logical_end_fixed;
    int32_t pending_dx;
    uint16_t unit;
    uint8_t laid_out;
    uint8_t pending_translate;
} LayoutBidiAtomic;

typedef struct {
    uint32_t codepoint;
    int32_t x_fixed;
    uint16_t logical_byte_offset;
    uint32_t command_index;
    uint16_t advance_fixed;
    uint8_t logical_byte_length;
    uint8_t level;
} LayoutBidiVisualGlyph;

struct LayoutBidiFlow {
    Budget *budget;
    LayoutContext *context;
    TextBidiParagraph *paragraph;
    size_t byte_count;
    size_t span_count;
    size_t command_count;
    size_t atomic_count;
    size_t scan_nodes;
    bool truncated;
    char text[TEXT_BIDI_PARAGRAPH_BYTE_LIMIT + 1u];
    LayoutBidiSourceSpan spans[LAYOUT_BIDI_SPAN_LIMIT];
    LayoutBidiFlowCommand commands[LAYOUT_BIDI_FLOW_COMMAND_LIMIT];
    LayoutBidiAtomic atomics[LAYOUT_BIDI_ATOMIC_LIMIT];
    uint16_t visual_order[TEXT_BIDI_CODEPOINT_LIMIT];
    LayoutBidiVisualGlyph visual_glyphs[TEXT_BIDI_CODEPOINT_LIMIT];
};

static size_t bidi_unit_at_byte(const LayoutBidiFlow *flow, size_t byte);

static bool bidi_atomic_inline_node(lxb_dom_node_t *node,
                                    const ComputedStyle *style)
{
    if (node == NULL || style == NULL || node->type != LXB_DOM_NODE_TYPE_ELEMENT)
        return false;
    if (style->display == DISPLAY_INLINE_BLOCK
        || style->display == DISPLAY_INLINE_FLEX
        || style->display == DISPLAY_INLINE_GRID) return true;
    return layout_node_name_is(node, "img")
        || layout_node_name_is(node, "svg")
        || layout_node_name_is(node, "video")
        || layout_node_name_is(node, "audio")
        || layout_node_name_is(node, "iframe")
        || layout_node_name_is(node, "canvas")
        || layout_node_name_is(node, "input")
        || layout_node_name_is(node, "textarea")
        || layout_node_name_is(node, "select")
        || layout_node_name_is(node, "button");
}

static bool bidi_scan_inline_subtree(LayoutContext *context,
                                     lxb_dom_node_t *node,
                                     const ComputedStyle *parent,
                                     bool root, unsigned depth,
                                     size_t *nodes, size_t *bytes,
                                     bool *limited)
{
    if (context == NULL || node == NULL || parent == NULL || nodes == NULL
        || bytes == NULL || limited == NULL || depth >= 64u
        || ++*nodes > LAYOUT_BIDI_SCAN_NODE_LIMIT) {
        if (limited != NULL) *limited = true;
        return false;
    }
    if (node->type == LXB_DOM_NODE_TYPE_TEXT) {
        size_t length = 0;
        const char *text = document_text_data(node, &length);
        if (text == NULL) return false;
        if (length > TEXT_BIDI_PARAGRAPH_BYTE_LIMIT - *bytes) {
            *limited = true;
            return false;
        }
        *bytes += length;
        return text_bidi_maybe_needed(text, length);
    }
    if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) return false;
    ComputedStyle style = root ? *parent
        : layout_style_for_node(context, node, parent);
    if (style.display == DISPLAY_NONE || style.visibility_hidden) return false;
    if (!root && is_block_display(style.display)) return false;
    bool direction_boundary = !root
        && computed_style_direction_rtl(&style)
               != computed_style_direction_rtl(parent);
    /* A same-direction `dir=ltr`/isolate is visually inert when this entire
       formatting context contains no RTL text or controls. Keep scanning so
       a later opposing run still admits the complete paragraph, but do not
       send common server-authored LTR wrappers through the shaped renderer.
       A real direction change remains significant even for ASCII content. */
    if (direction_boundary) return true;
    for (lxb_dom_node_t *child = node->first_child; child != NULL;
         child = child->next) {
        if (bidi_scan_inline_subtree(
                context, child, &style, false, depth + 1u,
                nodes, bytes, limited)) return true;
        if (*limited) return false;
    }
    return false;
}

static size_t bidi_encode(uint32_t codepoint, char output[4])
{
    if (codepoint <= 0x7fu) {
        output[0] = (char) codepoint;
        return 1;
    }
    if (codepoint <= 0x7ffu) {
        output[0] = (char) (0xc0u | (codepoint >> 6));
        output[1] = (char) (0x80u | (codepoint & 0x3fu));
        return 2;
    }
    if (codepoint <= 0xffffu) {
        output[0] = (char) (0xe0u | (codepoint >> 12));
        output[1] = (char) (0x80u | ((codepoint >> 6) & 0x3fu));
        output[2] = (char) (0x80u | (codepoint & 0x3fu));
        return 3;
    }
    if (codepoint <= 0x10ffffu) {
        output[0] = (char) (0xf0u | (codepoint >> 18));
        output[1] = (char) (0x80u | ((codepoint >> 12) & 0x3fu));
        output[2] = (char) (0x80u | ((codepoint >> 6) & 0x3fu));
        output[3] = (char) (0x80u | (codepoint & 0x3fu));
        return 4;
    }
    return 0;
}

static bool bidi_append_bytes(LayoutBidiFlow *flow, const char *text,
                              size_t length, bool source)
{
    if (flow == NULL || text == NULL
        || length > TEXT_BIDI_PARAGRAPH_BYTE_LIMIT - flow->byte_count) {
        if (flow != NULL) flow->truncated = true;
        return false;
    }
    if (source && length != 0) {
        if (flow->span_count == LAYOUT_BIDI_SPAN_LIMIT
            || length > UINT32_MAX || flow->byte_count > UINT32_MAX) {
            flow->truncated = true;
            return false;
        }
        flow->spans[flow->span_count++] = (LayoutBidiSourceSpan) {
            .source = text,
            .source_length = (uint32_t) length,
            .paragraph_byte_start = (uint32_t) flow->byte_count
        };
    }
    memcpy(flow->text + flow->byte_count, text, length);
    flow->byte_count += length;
    flow->text[flow->byte_count] = '\0';
    return true;
}

static bool bidi_append_control(LayoutBidiFlow *flow, uint32_t codepoint)
{
    char encoded[4];
    size_t length = bidi_encode(codepoint, encoded);
    return length != 0 && bidi_append_bytes(flow, encoded, length, false);
}

static bool bidi_flatten(LayoutBidiFlow *flow, lxb_dom_node_t *node,
                         const ComputedStyle *parent, bool root,
                         unsigned depth)
{
    if (flow == NULL || node == NULL || parent == NULL || depth >= 64u
        || ++flow->scan_nodes > LAYOUT_BIDI_SCAN_NODE_LIMIT) {
        if (flow != NULL) flow->truncated = true;
        return false;
    }
    if (node->type == LXB_DOM_NODE_TYPE_TEXT) {
        size_t length = 0;
        const char *text = document_text_data(node, &length);
        return text == NULL || bidi_append_bytes(flow, text, length, true);
    }
    if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) return true;
    ComputedStyle style = root ? *parent
        : layout_style_for_node(flow->context, node, parent);
    if (style.display == DISPLAY_NONE || style.visibility_hidden) return true;
    if (!root && is_block_display(style.display)) return true;

    size_t dir_length = 0;
    const char *dir = document_attribute(node, "dir", &dir_length);
    bool explicit_dir = dir != NULL && dir_length != 0;
    StyleUnicodeBidi mode = style.unicode_bidi;
    if (explicit_dir && mode == STYLE_UNICODE_BIDI_NORMAL)
        mode = STYLE_UNICODE_BIDI_ISOLATE;
    bool rtl = computed_style_direction_rtl(&style);
    uint32_t first = 0, second = 0, last = 0, second_last = 0;
    switch (mode) {
    case STYLE_UNICODE_BIDI_EMBED:
        first = rtl ? 0x202bu : 0x202au; /* RLE/LRE */
        last = 0x202cu;                  /* PDF */
        break;
    case STYLE_UNICODE_BIDI_ISOLATE:
        first = rtl ? 0x2067u : 0x2066u; /* RLI/LRI */
        if (explicit_dir && dir_length == 4
            && memcmp(dir, "auto", 4) == 0) first = 0x2068u; /* FSI */
        last = 0x2069u;                  /* PDI */
        break;
    case STYLE_UNICODE_BIDI_OVERRIDE:
        first = rtl ? 0x202eu : 0x202du; /* RLO/LRO */
        last = 0x202cu;
        break;
    case STYLE_UNICODE_BIDI_ISOLATE_OVERRIDE:
        first = rtl ? 0x2067u : 0x2066u;
        second = rtl ? 0x202eu : 0x202du;
        second_last = 0x202cu;
        last = 0x2069u;
        break;
    case STYLE_UNICODE_BIDI_PLAINTEXT:
        first = 0x2068u;                 /* bounded paragraph-like isolate */
        last = 0x2069u;
        break;
    default:
        break;
    }
    if ((first != 0 && !bidi_append_control(flow, first))
        || (second != 0 && !bidi_append_control(flow, second))) return false;
    if (!root && bidi_atomic_inline_node(node, &style)) {
        if (flow->atomic_count == LAYOUT_BIDI_ATOMIC_LIMIT
            || flow->byte_count > UINT32_MAX) {
            flow->truncated = true;
            return false;
        }
        LayoutBidiAtomic *atomic = &flow->atomics[flow->atomic_count++];
        atomic->node = node;
        atomic->paragraph_byte_start = (uint32_t) flow->byte_count;
        if (!bidi_append_control(flow, 0xfffcu)) return false;
    } else {
        for (lxb_dom_node_t *child = node->first_child; child != NULL;
             child = child->next) {
            if (!bidi_flatten(flow, child, &style, false, depth + 1u))
                return false;
        }
    }
    return (second_last == 0 || bidi_append_control(flow, second_last))
        && (last == 0 || bidi_append_control(flow, last));
}

LayoutBidiFlow *layout_bidi_flow_create(LayoutContext *context,
                                         lxb_dom_node_t *node,
                                         const ComputedStyle *style,
                                         bool *attempted)
{
    if (attempted != NULL) *attempted = false;
    if (context == NULL || context->layout == NULL || node == NULL
        || style == NULL) return NULL;
    bool authored_bidi = style->unicode_bidi != STYLE_UNICODE_BIDI_NORMAL
                         || computed_style_direction_rtl(style);
    bool possible_bidi = context->document_bidi_text_present
        || context->document_bidi_markup_present
        || (context->sheet != NULL
            && context->sheet->has_bidi_declarations);
    if (!authored_bidi && !possible_bidi)
        return NULL;
    uint64_t started_us = tilefinch_platform_monotonic_time_us();
    size_t budget_before = context->layout->budget->current;
    size_t nodes = 0, bytes = 0;
    bool limited = false;
    if (!authored_bidi
        && !bidi_scan_inline_subtree(
               context, node, style, true, 0, &nodes, &bytes, &limited)) {
        if (limited) {
            if (attempted != NULL) *attempted = true;
            context->layout->performance.bidi_limit_degradations++;
        }
        context->layout->performance.bidi_us +=
            tilefinch_platform_monotonic_time_us() - started_us;
        return NULL;
    }
    if (attempted != NULL) *attempted = true;
    LayoutBidiFlow *flow = budget_calloc(
        context->layout->budget, 1, sizeof(*flow));
    if (flow == NULL) {
        context->layout->performance.bidi_line_fallbacks++;
        context->layout->performance.bidi_us +=
            tilefinch_platform_monotonic_time_us() - started_us;
        return NULL;
    }
    flow->budget = context->layout->budget;
    flow->context = context;
    if (!bidi_flatten(flow, node, style, true, 0) || flow->truncated
        || flow->byte_count == 0) {
        if (flow->truncated)
            context->layout->performance.bidi_limit_degradations++;
        budget_free(flow->budget, flow);
        context->layout->performance.bidi_us +=
            tilefinch_platform_monotonic_time_us() - started_us;
        return NULL;
    }
    TextBidiStatus status = TEXT_BIDI_MALFORMED;
    TextBidiBase base = computed_style_direction_rtl(style)
        ? TEXT_BIDI_BASE_RTL : TEXT_BIDI_BASE_LTR;
    size_t root_dir_length = 0;
    const char *root_dir = document_attribute(
        node, "dir", &root_dir_length);
    bool root_dir_auto = root_dir != NULL && root_dir_length == 4u
        && memcmp(root_dir, "auto", 4u) == 0;
    if (root_dir_auto
        || style->unicode_bidi == STYLE_UNICODE_BIDI_PLAINTEXT)
        base = computed_style_direction_rtl(style)
            ? TEXT_BIDI_BASE_AUTO_RTL : TEXT_BIDI_BASE_AUTO_LTR;
    flow->paragraph = text_bidi_paragraph_create(
        flow->budget, flow->text, flow->byte_count, base, &status);
    if (flow->paragraph == NULL) {
        budget_free(flow->budget, flow);
        if (status == TEXT_BIDI_LIMIT_EXCEEDED)
            context->layout->performance.bidi_limit_degradations++;
        else
            context->layout->performance.bidi_line_fallbacks++;
        context->layout->performance.bidi_us +=
            tilefinch_platform_monotonic_time_us() - started_us;
        return NULL;
    }
    for (size_t i = 0; i < flow->atomic_count; i++) {
        size_t unit = bidi_unit_at_byte(
            flow, flow->atomics[i].paragraph_byte_start);
        if (unit >= text_bidi_paragraph_count(flow->paragraph)
            || unit > UINT16_MAX) {
            text_bidi_paragraph_destroy(flow->paragraph);
            budget_free(flow->budget, flow);
            context->layout->performance.bidi_line_fallbacks++;
            context->layout->performance.bidi_us +=
                tilefinch_platform_monotonic_time_us() - started_us;
            return NULL;
        }
        flow->atomics[i].unit = (uint16_t) unit;
    }
    context->layout->performance.bidi_paragraphs++;
    size_t transient = context->layout->budget->current - budget_before;
    if (transient
        > context->layout->performance.bidi_peak_transient_bytes) {
        context->layout->performance.bidi_peak_transient_bytes = transient;
    }
    context->layout->performance.bidi_us +=
        tilefinch_platform_monotonic_time_us() - started_us;
    return flow;
}

void layout_bidi_flow_destroy(LayoutBidiFlow *flow)
{
    if (flow == NULL) return;
    text_bidi_paragraph_destroy(flow->paragraph);
    budget_free(flow->budget, flow);
}

static size_t bidi_unit_at_byte(const LayoutBidiFlow *flow, size_t byte)
{
    const TextBidiUnit *units = text_bidi_paragraph_units(flow->paragraph);
    size_t low = 0, high = text_bidi_paragraph_count(flow->paragraph);
    while (low < high) {
        size_t middle = low + (high - low) / 2u;
        if (units[middle].byte_offset < byte) low = middle + 1u;
        else high = middle;
    }
    return low;
}

static bool bidi_source_unit_range(
    const LayoutBidiFlow *flow, const char *text, size_t length,
    size_t *unit_start, size_t *unit_end, size_t *paragraph_byte_start)
{
    if (flow == NULL || flow->paragraph == NULL || text == NULL
        || unit_start == NULL || unit_end == NULL
        || paragraph_byte_start == NULL) return false;
    uintptr_t wanted = (uintptr_t) text;
    if (wanted > UINTPTR_MAX - length) return false;
    for (size_t i = 0; i < flow->span_count; i++) {
        const LayoutBidiSourceSpan *span = &flow->spans[i];
        uintptr_t start = (uintptr_t) span->source;
        if (start > UINTPTR_MAX - span->source_length || wanted < start
            || wanted + length > start + span->source_length) continue;
        size_t paragraph_byte = span->paragraph_byte_start + wanted - start;
        size_t first = bidi_unit_at_byte(flow, paragraph_byte);
        size_t last = bidi_unit_at_byte(flow, paragraph_byte + length);
        if (last < first
            || last > text_bidi_paragraph_count(flow->paragraph)) return false;
        *unit_start = first;
        *unit_end = last;
        *paragraph_byte_start = paragraph_byte;
        return true;
    }
    return false;
}

static uint32_t bidi_transform_codepoint(
    uint32_t codepoint, TextTransformMode transform, bool first)
{
    bool upper = transform == TEXT_TRANSFORM_UPPERCASE
        || (transform == TEXT_TRANSFORM_CAPITALIZE && first);
    if (upper && codepoint >= 'a' && codepoint <= 'z')
        return codepoint - 'a' + 'A';
    if (transform == TEXT_TRANSFORM_LOWERCASE
        && codepoint >= 'A' && codepoint <= 'Z')
        return codepoint - 'A' + 'a';
    return codepoint;
}

static int bidi_shaped_unit_advance(
    const FontFace *face, FontFamily metric_family,
    const TextBidiUnit *unit, int font_size_fixed,
    bool synthetic_bold, bool metric_bold, TextTransformMode transform,
    bool first, bool kerning)
{
    uint32_t codepoint = bidi_transform_codepoint(
        unit->shaped_codepoint, transform, first);
    if (font_codepoint_default_ignorable(codepoint)) return 0;
    char encoded[4];
    size_t length = bidi_encode(codepoint, encoded);
    if (length == 0) return 0;
    int advance = font_text_width_for_family_at_size_fixed_mode(
        face, metric_family, encoded, length, font_size_fixed,
        synthetic_bold, metric_bold, kerning);
    if (advance <= 0 && codepoint != unit->codepoint) {
        length = bidi_encode(unit->codepoint, encoded);
        advance = length == 0 ? 0
            : font_text_width_for_family_at_size_fixed_mode(
                  face, metric_family, encoded, length, font_size_fixed,
                  synthetic_bold, metric_bold, kerning);
    }
    return advance < 0 ? 0 : advance;
}

bool layout_bidi_measure_shaped_text(
    LineState *line, const FontFace *face, FontFamily metric_family,
    const char *text, size_t length, int font_size_fixed,
    bool synthetic_bold, bool metric_bold, int letter_spacing,
    TextTransformMode transform, bool kerning, int *width_fixed)
{
    if (line == NULL || line->bidi_flow == NULL || width_fixed == NULL)
        return false;
    LayoutBidiFlow *flow = line->bidi_flow;
    size_t first = 0, last = 0, paragraph_byte = 0;
    if (!bidi_source_unit_range(
            flow, text, length, &first, &last, &paragraph_byte)) return false;
    const TextBidiUnit *units = text_bidi_paragraph_units(flow->paragraph);
    bool shaped = false;
    for (size_t i = first; i < last; i++) {
        if (units[i].shaped_codepoint != units[i].codepoint) {
            shaped = true;
            break;
        }
    }
    /* Preserve the existing whole-run kerning and optional-sequence path for
       Hebrew, Latin, and unchanged Arabic text. Only contextual substitutions
       need the per-shaped-glyph measurement below. */
    if (!shaped) return false;
    int width = 0;
    bool visible = false;
    for (size_t i = first; i < last; i++) {
        uint32_t codepoint = units[i].shaped_codepoint;
        bool ignored = font_codepoint_default_ignorable(codepoint);
        int advance = bidi_shaped_unit_advance(
            face, metric_family, &units[i], font_size_fixed,
            synthetic_bold, metric_bold, transform,
            units[i].byte_offset == paragraph_byte, kerning);
        if (visible && !ignored) {
            advance = layout_fixed_add(
                advance, layout_fixed_from_integer(letter_spacing));
        }
        width = layout_fixed_add(width, advance);
        if (!ignored) visible = true;
    }
    /* An unattached optional Arabic pack can leave every contextual form
       without a metric. Let the ordinary logical-codepoint fallback retain
       its established .notdef advances until that pack is available. */
    if (width <= 0) return false;
    *width_fixed = width;
    return true;
}

bool layout_bidi_fitting_shaped_prefix(
    LineState *line, const FontFace *face, FontFamily metric_family,
    const char *text, size_t length, int font_size_fixed,
    bool synthetic_bold, bool metric_bold, int letter_spacing,
    TextTransformMode transform, bool kerning, int available_fixed,
    size_t *prefix)
{
    if (line == NULL || line->bidi_flow == NULL || prefix == NULL)
        return false;
    LayoutBidiFlow *flow = line->bidi_flow;
    size_t first = 0, last = 0, paragraph_byte = 0;
    if (!bidi_source_unit_range(
            flow, text, length, &first, &last, &paragraph_byte)) return false;
    const TextBidiUnit *units = text_bidi_paragraph_units(flow->paragraph);
    bool shaped = false;
    for (size_t i = first; i < last; i++) {
        if (units[i].shaped_codepoint != units[i].codepoint) {
            shaped = true;
            break;
        }
    }
    if (!shaped) return false;
    int width = 0;
    bool visible = false;
    size_t used = 0;
    for (size_t i = first; i < last; i++) {
        uint32_t codepoint = units[i].shaped_codepoint;
        bool ignored = font_codepoint_default_ignorable(codepoint);
        int advance = bidi_shaped_unit_advance(
            face, metric_family, &units[i], font_size_fixed,
            synthetic_bold, metric_bold, transform,
            units[i].byte_offset == paragraph_byte, kerning);
        if (visible && !ignored) {
            advance = layout_fixed_add(
                advance, layout_fixed_from_integer(letter_spacing));
        }
        int next = layout_fixed_add(width, advance);
        size_t relative_end = (size_t) units[i].byte_offset
            + units[i].byte_length - paragraph_byte;
        if (used != 0 && next > available_fixed) break;
        width = next;
        used = relative_end > length ? length : relative_end;
        if (!ignored) visible = true;
        if (width > available_fixed) break;
    }
    if (width <= 0) return false;
    *prefix = used == 0 ? (length == 0 ? 0 : 1) : used;
    return true;
}

void layout_bidi_note_text_command(LineState *line, size_t command_index,
                                   const char *text, size_t length,
                                   int logical_advance_fixed,
                                   int space_advance_fixed)
{
    if (line == NULL || line->bidi_flow == NULL || text == NULL
        || length == 0 || command_index > UINT32_MAX) return;
    LayoutBidiFlow *flow = line->bidi_flow;
    if (flow->command_count == LAYOUT_BIDI_FLOW_COMMAND_LIMIT) return;
    uintptr_t wanted = (uintptr_t) text;
    if (wanted > UINTPTR_MAX - length) return;
    for (size_t i = 0; i < flow->span_count; i++) {
        const LayoutBidiSourceSpan *span = &flow->spans[i];
        uintptr_t start = (uintptr_t) span->source;
        if (start > UINTPTR_MAX - span->source_length
            || wanted < start || wanted + length > start + span->source_length)
            continue;
        size_t paragraph_byte = span->paragraph_byte_start + wanted - start;
        size_t unit_start = bidi_unit_at_byte(flow, paragraph_byte);
        size_t unit_end = bidi_unit_at_byte(flow, paragraph_byte + length);
        if (unit_start > UINT16_MAX || unit_end < unit_start
            || unit_end - unit_start > UINT16_MAX) return;
        flow->commands[flow->command_count++] = (LayoutBidiFlowCommand) {
            .command_index = (uint32_t) command_index,
            .logical_advance_fixed = logical_advance_fixed,
            .space_advance_fixed = space_advance_fixed,
            .unit_start = (uint16_t) unit_start,
            .unit_count = (uint16_t) (unit_end - unit_start)
        };
        return;
    }
}

void layout_bidi_note_atomic(LineState *line, lxb_dom_node_t *node,
                             size_t command_start, size_t link_start,
                             size_t control_start, size_t node_box_start,
                             size_t previous_line_command_start,
                             int previous_cursor_fixed)
{
    if (line == NULL || line->bidi_flow == NULL || line->layout == NULL
        || node == NULL) return;
    LayoutBidiFlow *flow = line->bidi_flow;
    for (size_t i = 0; i < flow->atomic_count; i++) {
        LayoutBidiAtomic *atomic = &flow->atomics[i];
        if (atomic->node != node || atomic->laid_out) continue;
        if (command_start > UINT32_MAX || line->layout->count > UINT32_MAX
            || link_start > UINT32_MAX
            || line->layout->link_count > UINT32_MAX
            || control_start > UINT32_MAX
            || line->layout->control_count > UINT32_MAX
            || node_box_start > UINT32_MAX
            || line->layout->node_box_count > UINT32_MAX) return;
        atomic->command_start = (uint32_t) command_start;
        atomic->command_end = (uint32_t) line->layout->count;
        atomic->link_start = (uint32_t) link_start;
        atomic->link_end = (uint32_t) line->layout->link_count;
        atomic->control_start = (uint32_t) control_start;
        atomic->control_end = (uint32_t) line->layout->control_count;
        atomic->node_box_start = (uint32_t) node_box_start;
        atomic->node_box_end = (uint32_t) line->layout->node_box_count;
        atomic->logical_start_fixed = previous_line_command_start
                == line->command_start
            ? previous_cursor_fixed
            : layout_fixed_from_integer(line->start_x);
        atomic->logical_end_fixed = line->x_fixed_valid
            ? line->x_fixed : layout_fixed_from_integer(line->x);
        atomic->laid_out = 1;
        return;
    }
}

static LayoutBidiAtomic *bidi_atomic_for_unit(
    LayoutBidiFlow *flow, size_t line_command_start,
    size_t line_command_end, size_t unit)
{
    for (size_t i = 0; i < flow->atomic_count; i++) {
        LayoutBidiAtomic *atomic = &flow->atomics[i];
        if (!atomic->laid_out || atomic->unit != unit) continue;
        if ((atomic->command_start < line_command_end
             && atomic->command_end > line_command_start)
            || (atomic->command_start == atomic->command_end
                && atomic->command_start >= line_command_start
                && atomic->command_start <= line_command_end)) return atomic;
    }
    return NULL;
}

static bool bidi_atomic_bounds(const LayoutDocument *layout,
                               const LayoutBidiAtomic *atomic,
                               int *left, int *right)
{
    int minimum = INT_MAX, maximum = INT_MIN;
#define BIDI_INCLUDE_BOX(x_, width_) do { \
    int bx_ = (x_); \
    int64_t br_ = (int64_t) bx_ + (width_); \
    if (bx_ < minimum) minimum = bx_; \
    if (br_ > maximum && br_ <= INT_MAX) maximum = (int) br_; \
} while (0)
    for (size_t i = atomic->command_start;
         i < atomic->command_end && i < layout->count; i++)
        BIDI_INCLUDE_BOX(layout->commands[i].x, layout->commands[i].width);
    for (size_t i = atomic->link_start;
         i < atomic->link_end && i < layout->link_count; i++)
        BIDI_INCLUDE_BOX(layout->links[i].x, layout->links[i].width);
    for (size_t i = atomic->control_start;
         i < atomic->control_end && i < layout->control_count; i++)
        BIDI_INCLUDE_BOX(layout->controls[i].x, layout->controls[i].width);
    for (size_t i = atomic->node_box_start;
         i < atomic->node_box_end && i < layout->node_box_count; i++)
        BIDI_INCLUDE_BOX(layout->node_boxes[i].x, layout->node_boxes[i].width);
#undef BIDI_INCLUDE_BOX
    if (minimum == INT_MAX || maximum <= minimum) return false;
    *left = minimum;
    *right = maximum;
    return true;
}

static void bidi_translate_atomic(LayoutDocument *layout,
                                  const LayoutBidiAtomic *atomic, int dx)
{
    if (dx == 0) return;
    int delta_fixed = layout_fixed_from_integer(dx);
    for (size_t i = 0; i < layout->bidi_command_count; i++) {
        const LayoutBidiCommand *mapped = &layout->bidi_commands[i];
        if (mapped->command_index < atomic->command_start
            || mapped->command_index >= atomic->command_end) continue;
        for (size_t glyph = 0; glyph < mapped->glyph_count; glyph++) {
            LayoutBidiGlyph *item = &layout->bidi_glyphs[
                mapped->glyph_start + glyph];
            item->x_fixed = layout_fixed_add(item->x_fixed, delta_fixed);
        }
    }
    for (size_t i = atomic->command_start;
         i < atomic->command_end && i < layout->count; i++) {
        DrawCommand *command = &layout->commands[i];
        if (command->type == DRAW_TEXT) {
            draw_command_set_text_x_fixed(command, layout_fixed_add(
                draw_command_text_x_fixed(command),
                delta_fixed));
        } else {
            command->x = layout_add_coordinate(command->x, dx);
        }
    }
    for (size_t i = atomic->link_start;
         i < atomic->link_end && i < layout->link_count; i++)
        layout->links[i].x = layout_add_coordinate(layout->links[i].x, dx);
    for (size_t i = atomic->control_start;
         i < atomic->control_end && i < layout->control_count; i++)
        layout->controls[i].x = layout_add_coordinate(
            layout->controls[i].x, dx);
    for (size_t i = atomic->node_box_start;
         i < atomic->node_box_end && i < layout->node_box_count; i++)
        layout->node_boxes[i].x = layout_add_coordinate(
            layout->node_boxes[i].x, dx);
}

static const LayoutBidiFlowCommand *bidi_command_for_unit(
    const LayoutBidiFlow *flow, size_t line_command_start,
    size_t line_command_end, size_t unit)
{
    for (size_t i = 0; i < flow->command_count; i++) {
        const LayoutBidiFlowCommand *command = &flow->commands[i];
        if (command->command_index < line_command_start
            || command->command_index >= line_command_end) continue;
        if (unit >= command->unit_start
            && unit - command->unit_start < command->unit_count)
            return command;
    }
    return NULL;
}

static const LayoutBidiFlowCommand *bidi_nearest_command_for_unit(
    const LayoutBidiFlow *flow, size_t line_command_start,
    size_t line_command_end, size_t line_unit_start, size_t line_unit_end,
    size_t unit)
{
    (void) line_unit_start;
    (void) line_unit_end;
    const LayoutBidiFlowCommand *best = NULL;
    size_t best_distance = SIZE_MAX;
    bool best_is_after = true;
    for (size_t i = 0; i < flow->command_count; i++) {
        const LayoutBidiFlowCommand *candidate = &flow->commands[i];
        if (candidate->command_index < line_command_start
            || candidate->command_index >= line_command_end
            || candidate->unit_count == 0u
            || candidate->unit_count > SIZE_MAX - candidate->unit_start)
            continue;
        size_t end = candidate->unit_start + candidate->unit_count;
        size_t distance = 0;
        bool is_after = false;
        if (unit < candidate->unit_start) {
            distance = candidate->unit_start - unit;
            is_after = true;
        } else if (unit >= end) {
            distance = unit - (end - 1u);
        } else {
            return candidate;
        }
        /* Preserve the old expanding search's tie rule: the nearest command
           before the gap wins over an equally distant command after it. */
        if (distance < best_distance
            || (distance == best_distance && best_is_after && !is_after)) {
            best = candidate;
            best_distance = distance;
            best_is_after = is_after;
        }
    }
    return best;
}

static int bidi_glyph_advance(const LayoutDocument *layout,
                              const DrawCommand *command,
                              uint32_t codepoint, uint32_t previous,
                              const char *sequence, size_t sequence_length)
{
    bool bold_face = draw_uses_bold_face(command);
    const FontFace *face = font_context_face_variant(
        layout->fonts, layout->web_fonts, draw_command_font_family(command),
        draw_command_font_italic(command), bold_face);
    FontFamily family = font_context_metric_family(
        layout->web_fonts, draw_command_font_family(command), face);
    unsigned weight = (unsigned) draw_command_font_weight_code(command) * 10u;
    bool synthetic_bold = weight >= LAYOUT_SYNTHETIC_WEIGHT
        && (weight < LAYOUT_BOLD_FACE_WEIGHT
            || !font_context_face_is_bold(
                   layout->fonts, layout->web_fonts, face));
    char encoded[4];
    size_t length = sequence == NULL
        ? bidi_encode(codepoint, encoded) : sequence_length;
    const char *source = sequence == NULL ? encoded : sequence;
    int advance = length == 0 ? 0
        : font_text_width_for_family_at_size_fixed_mode(
              face, family, source, length,
              draw_command_text_font_size_fixed(command),
              synthetic_bold, bold_face,
              !draw_command_text_kerning_none(command));
    if (advance < 0) advance = 0;
    if (previous != 0) {
        if (!draw_command_text_kerning_none(command)) {
            advance = layout_fixed_add(
                advance, font_kerning_at_size_fixed(
                    face, previous, codepoint,
                    draw_command_text_font_size_fixed(command)));
        }
        advance = layout_fixed_add(
            advance, layout_fixed_from_integer(command->letter_spacing));
    }
    return advance;
}

static bool bidi_reserve(LayoutDocument *layout, size_t commands,
                         size_t glyphs)
{
    if (commands > LAYOUT_BIDI_COMMAND_LIMIT - layout->bidi_command_count
        || glyphs > LAYOUT_BIDI_GLYPH_LIMIT - layout->bidi_glyph_count)
        return false;
    size_t needed_commands = layout->bidi_command_count + commands;
    if (needed_commands > layout->bidi_command_capacity) {
        size_t capacity = layout->bidi_command_capacity == 0
            ? 16u : layout->bidi_command_capacity;
        while (capacity < needed_commands && capacity < LAYOUT_BIDI_COMMAND_LIMIT)
            capacity *= 2u;
        if (capacity > LAYOUT_BIDI_COMMAND_LIMIT)
            capacity = LAYOUT_BIDI_COMMAND_LIMIT;
        LayoutBidiCommand *grown = budget_realloc(
            layout->budget, layout->bidi_commands,
            capacity * sizeof(*grown));
        if (grown == NULL) return false;
        layout->bidi_commands = grown;
        layout->bidi_command_capacity = capacity;
    }
    size_t needed_glyphs = layout->bidi_glyph_count + glyphs;
    if (needed_glyphs > layout->bidi_glyph_capacity) {
        size_t capacity = layout->bidi_glyph_capacity == 0
            ? 64u : layout->bidi_glyph_capacity;
        while (capacity < needed_glyphs && capacity < LAYOUT_BIDI_GLYPH_LIMIT)
            capacity *= 2u;
        if (capacity > LAYOUT_BIDI_GLYPH_LIMIT)
            capacity = LAYOUT_BIDI_GLYPH_LIMIT;
        LayoutBidiGlyph *grown = budget_realloc(
            layout->budget, layout->bidi_glyphs,
            capacity * sizeof(*grown));
        if (grown == NULL) return false;
        layout->bidi_glyphs = grown;
        layout->bidi_glyph_capacity = capacity;
    }
    return true;
}

bool layout_bidi_resolve_line(LineState *line)
{
    if (line == NULL || line->bidi_flow == NULL || line->layout == NULL
        || line->command_start >= line->layout->count) return false;
    LayoutBidiFlow *flow = line->bidi_flow;
    size_t line_end = line->layout->count;
    size_t unit_start = SIZE_MAX, unit_end = 0, mapped_commands = 0;
    for (size_t i = 0; i < flow->command_count; i++) {
        const LayoutBidiFlowCommand *map = &flow->commands[i];
        if (map->command_index < line->command_start
            || map->command_index >= line_end || map->unit_count == 0) continue;
        if (map->unit_start < unit_start) unit_start = map->unit_start;
        size_t end = (size_t) map->unit_start + map->unit_count;
        if (end > unit_end) unit_end = end;
        mapped_commands++;
    }
    for (size_t i = 0; i < flow->atomic_count; i++) {
        const LayoutBidiAtomic *atomic = &flow->atomics[i];
        if (!atomic->laid_out) continue;
        bool on_line = (atomic->command_start < line_end
                        && atomic->command_end > line->command_start)
            || (atomic->command_start == atomic->command_end
                && atomic->command_start >= line->command_start
                && atomic->command_start <= line_end);
        if (!on_line) continue;
        if (atomic->unit < unit_start) unit_start = atomic->unit;
        if ((size_t) atomic->unit + 1u > unit_end)
            unit_end = (size_t) atomic->unit + 1u;
    }
    if (unit_start == SIZE_MAX || unit_end <= unit_start
        || unit_end - unit_start > TEXT_BIDI_CODEPOINT_LIMIT) return false;
    size_t run_count = 0;
    size_t unit_count = unit_end - unit_start;
    uint64_t started_us = tilefinch_platform_monotonic_time_us();
    if (!text_bidi_line_visual_order(
            flow->paragraph, unit_start, unit_count, flow->visual_order,
            TEXT_BIDI_CODEPOINT_LIMIT, &run_count)) {
        line->layout->performance.bidi_line_fallbacks++;
        line->layout->performance.bidi_us +=
            tilefinch_platform_monotonic_time_us() - started_us;
        return false;
    }
    (void) run_count;
    const TextBidiUnit *units = text_bidi_paragraph_units(flow->paragraph);
    int pen_fixed = layout_fixed_from_integer(line->start_x);
    int old_end = line->x_fixed_valid ? line->x_fixed
        : layout_fixed_from_integer(line->x);
    int64_t mapped_width_fixed = 0;
    for (size_t i = 0; i < flow->command_count; i++) {
        const LayoutBidiFlowCommand *map = &flow->commands[i];
        if (map->command_index >= line->command_start
            && map->command_index < line_end) {
            mapped_width_fixed += map->logical_advance_fixed;
        }
    }
    for (size_t i = 0; i < flow->atomic_count; i++) {
        LayoutBidiAtomic *atomic = &flow->atomics[i];
        atomic->pending_translate = 0;
        int left = 0, right = 0;
        if (bidi_atomic_for_unit(
                flow, line->command_start, line_end, atomic->unit) == atomic
            && bidi_atomic_bounds(line->layout, atomic, &left, &right)) {
            int advance = atomic->logical_end_fixed
                          - atomic->logical_start_fixed;
            mapped_width_fixed += advance > 0
                ? advance : (int64_t) (right - left) * 64;
        }
    }
    int64_t original_spacing_fixed = (int64_t) old_end - pen_fixed
                                     - mapped_width_fixed;
    if (original_spacing_fixed < 0) original_spacing_fixed = 0;
    int64_t spacing_weight_total = 0;
    size_t spacing_count = 0;
    for (size_t i = 0; i < unit_count; i++) {
        size_t logical = flow->visual_order[i];
        uint32_t codepoint = units[logical].shaped_codepoint;
        if (codepoint != ' ' && codepoint != 0x00a0u) continue;
        const LayoutBidiFlowCommand *spacing_map =
            bidi_nearest_command_for_unit(
                flow, line->command_start, line_end, unit_start,
                unit_end, logical);
        if (spacing_map == NULL) continue;
        int weight = spacing_map->space_advance_fixed;
        spacing_weight_total += weight > 0 ? weight : 1;
        spacing_count++;
    }
    int64_t spacing_assigned = 0;
    size_t spacing_seen = 0;
    size_t visual_count = 0;
    const LayoutBidiFlowCommand *last_map = NULL;
    uint32_t previous = 0;
    for (size_t i = 0; i < unit_count; i++) {
        size_t logical = flow->visual_order[i];
        LayoutBidiAtomic *atomic = bidi_atomic_for_unit(
            flow, line->command_start, line_end, logical);
        if (atomic != NULL) {
            int left = 0, right = 0;
            if (!bidi_atomic_bounds(line->layout, atomic, &left, &right))
                continue;
            int advance = atomic->logical_end_fixed
                          - atomic->logical_start_fixed;
            int leading = layout_fixed_from_integer(left)
                          - atomic->logical_start_fixed;
            if (advance <= 0) {
                leading = 0;
                advance = layout_fixed_from_integer(right - left);
            }
            int target = layout_fixed_ceil(
                layout_fixed_add(pen_fixed, leading));
            atomic->pending_dx = target - left;
            atomic->pending_translate = 1;
            pen_fixed = layout_fixed_add(pen_fixed, advance);
            previous = 0;
            last_map = NULL;
            continue;
        }
        const LayoutBidiFlowCommand *map = bidi_command_for_unit(
            flow, line->command_start, line_end, logical);
        uint32_t codepoint = units[logical].shaped_codepoint;
        if (font_codepoint_default_ignorable(codepoint)) continue;
        if (map == NULL) {
            if (codepoint == ' ' || codepoint == 0x00a0u) {
                const LayoutBidiFlowCommand *spacing_map =
                    bidi_nearest_command_for_unit(
                        flow, line->command_start, line_end, unit_start,
                        unit_end, logical);
                if (spacing_map != NULL) {
                    int weight = spacing_map->space_advance_fixed;
                    int64_t advance = ++spacing_seen == spacing_count
                        ? original_spacing_fixed - spacing_assigned
                        : (spacing_weight_total == 0 ? 0
                           : original_spacing_fixed
                             * (weight > 0 ? weight : 1)
                             / spacing_weight_total);
                    if (advance < 0) advance = 0;
                    if (advance > INT_MAX) advance = INT_MAX;
                    pen_fixed = layout_fixed_add(
                        pen_fixed, (int) advance);
                    spacing_assigned += advance;
                }
            }
            previous = 0;
            continue;
        }
        const DrawCommand *style = &line->layout->commands[map->command_index];
        const char *sequence = NULL;
        size_t sequence_length = 0;
        size_t sequence_units = 1;
        if (logical >= map->unit_start) {
            size_t local_byte = units[logical].byte_offset
                - units[map->unit_start].byte_offset;
            size_t used = 0;
            unsigned key = 0;
            if (local_byte < style->text_length
                && font_optional_glyph_match_sequence(
                       style->text + local_byte,
                       style->text_length - local_byte, &used, &key)) {
                size_t covered = units[logical].byte_length;
                while (covered < used && i + sequence_units < unit_count) {
                    size_t next = flow->visual_order[i + sequence_units];
                    const LayoutBidiFlowCommand *next_map =
                        bidi_command_for_unit(
                            flow, line->command_start, line_end, next);
                    if (next_map != map || next != logical + sequence_units)
                        break;
                    covered += units[next].byte_length;
                    sequence_units++;
                }
                if (covered == used && used <= UINT16_MAX) {
                    sequence = style->text + local_byte;
                    sequence_length = used;
                    codepoint = key;
                } else {
                    sequence_units = 1;
                }
            }
        }
        if (sequence == NULL) {
            codepoint = draw_command_transform_codepoint(
                style, codepoint, logical == map->unit_start);
        }
        if (last_map != map) previous = 0;
        if (visual_count == TEXT_BIDI_CODEPOINT_LIMIT) return false;
        int advance = bidi_glyph_advance(
            line->layout, style, codepoint, previous,
            sequence, sequence_length);
        if (advance <= 0 && sequence == NULL
            && codepoint != units[logical].codepoint) {
            advance = bidi_glyph_advance(
                line->layout, style, units[logical].codepoint,
                previous, NULL, 0);
        }
        if (advance <= 0 && map->unit_count != 0) {
            if (units[logical].cluster_extender) {
                advance = 0;
            } else {
                size_t paintable = 0, paintable_before = 0;
                for (size_t item = map->unit_start;
                     item < (size_t) map->unit_start + map->unit_count;
                     item++) {
                    uint32_t original = units[item].codepoint;
                    if (font_codepoint_default_ignorable(original)
                        || units[item].cluster_extender) continue;
                    if (item < logical) paintable_before++;
                    paintable++;
                }
                if (paintable == 0) paintable = 1;
                int quotient = map->logical_advance_fixed / (int) paintable;
                int remainder = map->logical_advance_fixed
                                - quotient * (int) paintable;
                advance = quotient + (remainder > 0
                    && paintable_before < (size_t) remainder ? 1 : 0);
            }
        }
        size_t logical_offset = units[logical].byte_offset
            - units[map->unit_start].byte_offset;
        size_t logical_length = sequence != NULL ? sequence_length
            : units[logical].byte_length;
        if (logical_offset > UINT16_MAX || logical_length > UINT8_MAX)
            return false;
        flow->visual_glyphs[visual_count++] = (LayoutBidiVisualGlyph) {
            .codepoint = codepoint,
            .x_fixed = pen_fixed,
            .logical_byte_offset = (uint16_t) logical_offset,
            .command_index = map->command_index,
            .advance_fixed = (uint16_t) (
                advance > UINT16_MAX ? UINT16_MAX : advance),
            .logical_byte_length = (uint8_t) logical_length,
            .level = units[logical].level
        };
        pen_fixed = layout_fixed_add(pen_fixed, advance);
        previous = sequence == NULL ? codepoint : 0;
        last_map = map;
        i += sequence_units - 1u;
    }
    int difference = pen_fixed - old_end;
    if (difference > 2 * 64 || difference < -2 * 64) {
        if (LAYOUT_TRACE(line->layout, LAYOUT_PROFILE)) {
            fprintf(stderr,
                    "layout-bidi-fallback units=%zu difference=%d "
                    "old-end=%d shaped-end=%d mapped=%zu glyphs=%zu\n",
                    unit_count, difference, old_end, pen_fixed,
                    mapped_commands, visual_count);
        }
        /* The line was wrapped with materially different metrics. Preserve
           the readable legacy layout instead of overflowing its chosen box. */
        line->layout->performance.bidi_line_fallbacks++;
        line->layout->performance.bidi_us +=
            tilefinch_platform_monotonic_time_us() - started_us;
        return false;
    }
    if (!bidi_reserve(line->layout, mapped_commands, visual_count)) {
        line->layout->performance.bidi_limit_degradations++;
        line->layout->performance.bidi_us +=
            tilefinch_platform_monotonic_time_us() - started_us;
        return false;
    }
    for (size_t i = 0; i < flow->atomic_count; i++) {
        LayoutBidiAtomic *atomic = &flow->atomics[i];
        if (atomic->pending_translate) {
            bidi_translate_atomic(line->layout, atomic, atomic->pending_dx);
            atomic->pending_translate = 0;
        }
    }
    for (size_t command_index = line->command_start;
         command_index < line_end; command_index++) {
        size_t count = 0;
        for (size_t i = 0; i < visual_count; i++)
            if (flow->visual_glyphs[i].command_index == command_index) count++;
        if (count == 0) continue;
        size_t glyph_start = line->layout->bidi_glyph_count;
        int minimum = INT_MAX, maximum = INT_MIN;
        uint8_t level = 0;
        for (size_t map_at = 0; map_at < flow->command_count; map_at++) {
            const LayoutBidiFlowCommand *map = &flow->commands[map_at];
            if (map->command_index == command_index) {
                level = units[map->unit_start].level;
                break;
            }
        }
        for (size_t i = 0; i < visual_count; i++) {
            const LayoutBidiVisualGlyph *source = &flow->visual_glyphs[i];
            if (source->command_index != command_index) continue;
            line->layout->bidi_glyphs[line->layout->bidi_glyph_count++] =
                (LayoutBidiGlyph) {
                    .codepoint = source->codepoint,
                    .x_fixed = source->x_fixed,
                    .logical_byte_offset = source->logical_byte_offset,
                    .advance_fixed = source->advance_fixed,
                    .logical_byte_length = source->logical_byte_length,
                    .level = source->level
                };
            if (source->x_fixed < minimum) minimum = source->x_fixed;
            int right = layout_fixed_add(
                source->x_fixed, source->advance_fixed);
            if (right > maximum) maximum = right;
        }
        DrawCommand *command = &line->layout->commands[command_index];
        draw_command_set_text_x_fixed(command, minimum);
        command->width = layout_fixed_ceil(maximum)
                         - layout_fixed_floor(minimum);
        command->radius &= ~LAYOUT_TEXT_RTL;
        line->layout->bidi_commands[line->layout->bidi_command_count++] =
            (LayoutBidiCommand) {
                .command_index = (uint32_t) command_index,
                .glyph_start = (uint32_t) glyph_start,
                .glyph_count = (uint16_t) count,
                .level = level
            };
        for (size_t link = line->link_start;
             link < line->layout->link_count; link++) {
            LinkRegion *region = &line->layout->links[link];
            size_t linked_command = region->z_index < 0
                ? (size_t) (-(int64_t) region->z_index - 1) : SIZE_MAX;
            if (linked_command == command_index) {
                region->x = command->x;
                region->width = command->width;
            }
        }
    }
    line->x_fixed = pen_fixed;
    line->x = layout_fixed_ceil(pen_fixed);
    line->x_fixed_valid = true;
    line->layout->performance.bidi_us +=
        tilefinch_platform_monotonic_time_us() - started_us;
    return true;
}

const LayoutBidiCommand *layout_bidi_command_for_index(
    const LayoutDocument *layout, size_t command_index)
{
    if (layout == NULL) return NULL;
    size_t low = 0, high = layout->bidi_command_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2u;
        if (layout->bidi_commands[middle].command_index < command_index)
            low = middle + 1u;
        else high = middle;
    }
    return low < layout->bidi_command_count
               && layout->bidi_commands[low].command_index == command_index
        ? &layout->bidi_commands[low] : NULL;
}

static void bidi_glyph_caret_edges(const LayoutBidiGlyph *glyph,
                                   size_t *left_logical,
                                   size_t *right_logical)
{
    size_t start = glyph->logical_byte_offset;
    size_t end = start + glyph->logical_byte_length;
    if ((glyph->level & 1u) != 0u) {
        *left_logical = end;
        *right_logical = start;
    } else {
        *left_logical = start;
        *right_logical = end;
    }
}

static bool bidi_nearest_caret(const LayoutDocument *layout,
                               const LayoutBidiCommand *command,
                               int wanted_x, int direction,
                               size_t *logical_byte_offset,
                               int *caret_x_fixed)
{
    if (layout == NULL || command == NULL || logical_byte_offset == NULL
        || caret_x_fixed == NULL || command->glyph_count == 0)
        return false;
    bool found = false;
    uint64_t best_distance = UINT64_MAX;
    int best_x = 0;
    size_t best_logical = 0;
    for (size_t at = 0; at < command->glyph_count; at++) {
        const LayoutBidiGlyph *glyph = &layout->bidi_glyphs[
            command->glyph_start + at];
        int left_x = glyph->x_fixed;
        int right_x = layout_fixed_add(left_x, glyph->advance_fixed);
        size_t left_logical = 0, right_logical = 0;
        bidi_glyph_caret_edges(glyph, &left_logical, &right_logical);
        const int positions[2] = {left_x, right_x};
        const size_t logicals[2] = {left_logical, right_logical};
        for (size_t edge = 0; edge < 2; edge++) {
            int candidate = positions[edge];
            if ((direction < 0 && candidate >= wanted_x)
                || (direction > 0 && candidate <= wanted_x)) continue;
            int64_t delta = (int64_t) candidate - wanted_x;
            uint64_t distance = (uint64_t) (delta < 0 ? -delta : delta);
            if (!found || distance < best_distance
                || (distance == best_distance && candidate < best_x)) {
                found = true;
                best_distance = distance;
                best_x = candidate;
                best_logical = logicals[edge];
            }
        }
    }
    if (!found) return false;
    *logical_byte_offset = best_logical;
    *caret_x_fixed = best_x;
    return true;
}

bool layout_bidi_visual_hit_test(const LayoutDocument *layout,
                                 size_t command_index, int x_fixed,
                                 size_t *logical_byte_offset,
                                 int *caret_x_fixed)
{
    const LayoutBidiCommand *command = layout_bidi_command_for_index(
        layout, command_index);
    return bidi_nearest_caret(
        layout, command, x_fixed, 0, logical_byte_offset, caret_x_fixed);
}

bool layout_bidi_visual_step(const LayoutDocument *layout,
                             size_t command_index, int current_x_fixed,
                             int visual_direction,
                             size_t *logical_byte_offset,
                             int *caret_x_fixed)
{
    if (visual_direction == 0) return false;
    const LayoutBidiCommand *command = layout_bidi_command_for_index(
        layout, command_index);
    return bidi_nearest_caret(
        layout, command, current_x_fixed,
        visual_direction < 0 ? -1 : 1,
        logical_byte_offset, caret_x_fixed);
}

void layout_bidi_translate_commands(LayoutDocument *layout,
                                    size_t command_start, int dx)
{
    if (layout == NULL || dx == 0) return;
    int delta = layout_fixed_from_integer(dx);
    for (size_t i = 0; i < layout->bidi_command_count; i++) {
        const LayoutBidiCommand *command = &layout->bidi_commands[i];
        if (command->command_index < command_start) continue;
        for (size_t glyph = 0; glyph < command->glyph_count; glyph++) {
            LayoutBidiGlyph *item = &layout->bidi_glyphs[
                command->glyph_start + glyph];
            item->x_fixed = layout_fixed_add(item->x_fixed, delta);
        }
    }
}

static int bidi_scale_fixed(int value, int origin_x_twice, uint8_t scale_q6)
{
    int64_t origin_fixed = (int64_t) origin_x_twice * 32;
    int64_t scaled = origin_fixed
        + ((int64_t) value - origin_fixed) * scale_q6 / 64;
    return scaled > INT_MAX ? INT_MAX : (scaled < INT_MIN ? INT_MIN
                                                          : (int) scaled);
}

void layout_bidi_scale_commands(LayoutDocument *layout,
                                size_t command_start,
                                int origin_x_twice, uint8_t scale_q6)
{
    if (layout == NULL || scale_q6 == 64u) return;
    for (size_t i = 0; i < layout->bidi_command_count; i++) {
        const LayoutBidiCommand *command = &layout->bidi_commands[i];
        if (command->command_index < command_start) continue;
        for (size_t glyph = 0; glyph < command->glyph_count; glyph++) {
            LayoutBidiGlyph *item = &layout->bidi_glyphs[
                command->glyph_start + glyph];
            item->x_fixed = bidi_scale_fixed(
                item->x_fixed, origin_x_twice, scale_q6);
            uint32_t advance = ((uint32_t) item->advance_fixed * scale_q6
                                + 32u) / 64u;
            item->advance_fixed = (uint16_t) (
                advance > UINT16_MAX ? UINT16_MAX : advance);
        }
    }
}

void layout_bidi_rebase_command(LayoutDocument *layout,
                                size_t command_index, int old_x_fixed)
{
    if (layout == NULL || command_index >= layout->count) return;
    const LayoutBidiCommand *command = layout_bidi_command_for_index(
        layout, command_index);
    if (command == NULL) return;
    int delta = layout_fixed_subtract(
        draw_command_text_x_fixed(&layout->commands[command_index]),
        old_x_fixed);
    for (size_t glyph = 0; glyph < command->glyph_count; glyph++) {
        LayoutBidiGlyph *item = &layout->bidi_glyphs[
            command->glyph_start + glyph];
        item->x_fixed = layout_fixed_add(item->x_fixed, delta);
    }
}
