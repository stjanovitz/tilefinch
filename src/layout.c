#include "tilefinch/layout.h"
#include "tilefinch/platform.h"
#include "tilefinch/work_vector.h"

#include "layout_internal.h"
#include "style_cache_internal.h"
#include "tilefinch_test_faults.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static uint64_t layout_performance_now_us(void)
{
    return tilefinch_platform_monotonic_time_us();
}

#if !defined(TILEFINCH_NO_TRACE) || defined(TILEFINCH_PROFILE_LAYOUT_FLOW)
static void layout_flow_profile_switch(LayoutContext *context,
                                       LayoutFlowPhase phase)
{
    uint64_t now = layout_performance_now_us();
    context->layout->performance.flow_phase_us[context->flow_profile_phase]
        += now >= context->flow_profile_last_us
            ? now - context->flow_profile_last_us : 0;
    context->flow_profile_last_us = now;
    context->flow_profile_phase = phase;
    context->layout->performance.flow_phase_transitions++;
}

void layout_flow_profile_begin(LayoutContext *context)
{
    if (context == NULL) return;
#ifndef TILEFINCH_PSP_VALIDATION_LOG
    if (!LAYOUT_TRACE(context->layout, LAYOUT_PROFILE)) return;
#endif
    memset(context->layout->performance.flow_phase_us, 0,
           sizeof(context->layout->performance.flow_phase_us));
    context->layout->performance.flow_phase_transitions = 0;
    context->flow_profile_phase = LAYOUT_FLOW_OTHER;
    context->flow_profile_last_us = layout_performance_now_us();
    context->flow_profile_active = true;
}

void layout_flow_profile_end(LayoutContext *context)
{
    if (context == NULL || !context->flow_profile_active) return;
    layout_flow_profile_switch(context, LAYOUT_FLOW_OTHER);
    context->flow_profile_active = false;
}

void layout_flow_profile_report(LayoutContext *context)
{
    if (context == NULL
        || context->layout->performance.flow_phase_transitions == 0) return;
    const LayoutPerformance *p = &context->layout->performance;
#ifndef __PSP__
    printf("tilefinch-layout-flow: other=%lluus style=%lluus pseudo=%lluus "
            "intrinsic=%lluus margin=%lluus inline=%lluus cooperate=%lluus "
            "switches=%u\n",
            (unsigned long long) p->flow_phase_us[LAYOUT_FLOW_OTHER],
            (unsigned long long) p->flow_phase_us[LAYOUT_FLOW_STYLE],
            (unsigned long long) p->flow_phase_us[LAYOUT_FLOW_PSEUDO],
            (unsigned long long) p->flow_phase_us[LAYOUT_FLOW_INTRINSIC],
            (unsigned long long) p->flow_phase_us[LAYOUT_FLOW_MARGIN],
            (unsigned long long) p->flow_phase_us[LAYOUT_FLOW_INLINE],
            (unsigned long long) p->flow_phase_us[LAYOUT_FLOW_COOPERATE],
            (unsigned) p->flow_phase_transitions);
    printf("tilefinch-layout-pseudos: resolutions=%zu generated=%zu absence-hits=%zu\n",
           context->pseudo_resolutions, context->pseudo_generated,
           context->pseudo_absence_hits);
#else
    (void) p; /* The activation receiver reports these retained counters. */
#endif
}

LayoutFlowScope layout_flow_scope_enter(LayoutContext *context,
                                        LayoutFlowPhase phase)
{
    LayoutFlowScope scope = {.context = context};
    if (context != NULL && context->flow_profile_active
        && context->flow_profile_phase != phase) {
        scope.previous = context->flow_profile_phase;
        scope.changed = true;
        layout_flow_profile_switch(context, phase);
    }
    return scope;
}

void layout_flow_scope_leave(LayoutFlowScope *scope)
{
    if (scope->changed)
        layout_flow_profile_switch(scope->context, scope->previous);
}
#endif

static void layout_read_trace_environment(LayoutDocument *layout)
{
#ifndef TILEFINCH_NO_TRACE
    layout->trace_flags =
        (getenv("TILEFINCH_TRACE_LAYOUT") != NULL ? LAYOUT_TRACE_LAYOUT : 0u)
        | (getenv("TILEFINCH_TRACE_PAINT") != NULL ? LAYOUT_TRACE_PAINT : 0u)
        | (getenv("TILEFINCH_TRACE_CLIP") != NULL ? LAYOUT_TRACE_CLIP : 0u)
        | (getenv("TILEFINCH_TRACE_FLEX_TRANSLATE") != NULL
           ? LAYOUT_TRACE_FLEX_TRANSLATE : 0u)
        | (getenv("TILEFINCH_TRACE_LAYOUT_SLICES") != NULL
           ? LAYOUT_TRACE_LAYOUT_SLICES : 0u)
        | (getenv("TILEFINCH_TRACE_LAYOUT_PROFILE") != NULL
           ? LAYOUT_TRACE_LAYOUT_PROFILE : 0u)
        | (getenv("TILEFINCH_TRACE_SCROLL_WIDTH") != NULL
           ? LAYOUT_TRACE_SCROLL_WIDTH : 0u);
    layout->trace_flex_class = getenv("TILEFINCH_TRACE_FLEX_CLASS");
    layout->trace_layout_class = getenv("TILEFINCH_TRACE_LAYOUT_CLASS");
    layout->trace_pseudo_class = getenv("TILEFINCH_TRACE_PSEUDO_CLASS");
    layout->trace_range_class = getenv("TILEFINCH_TRACE_RANGE_CLASS");
#else
    (void) layout;
#endif
}

static void layout_free_block_scratch(LayoutContext *context)
{
    if (context == NULL || context->layout == NULL) return;
    for (size_t i = 0; i < LAYOUT_BLOCK_SCRATCH_PAGE_COUNT; i++) {
        budget_free(context->layout->budget,
                    context->block_scratch_pages[i]);
        context->block_scratch_pages[i] = NULL;
    }
    context->block_scratch_page_count = 0;
}

static void layout_release_context(LayoutContext *context, Budget *budget)
{
    if (context == NULL) return;
    /* Every pass, finished or cancelled, releases its context here. */
    TILEFINCH_WORK_ADD(style_resolutions, context->style_resolutions);
    TILEFINCH_WORK_ADD(style_cache_hits, context->style_cache_hits);
    TILEFINCH_WORK_ADD(style_cache_misses, context->style_cache_misses);
    if (context->style_selector_cooperation_owned
        && context->sheet != NULL) {
        style_selector_cooperation_end((Stylesheet *) context->sheet);
    }
    tilefinch_platform_trace_step("release-variable-cache");
    if (context->style_variable_cache_owned && context->sheet != NULL) {
        style_variable_cache_end((Stylesheet *) context->sheet);
    }
    tilefinch_platform_trace_step("release-block-scratch");
    layout_free_block_scratch(context);
    tilefinch_platform_trace_step("release-rest");
    layout_bidi_release(context, budget);
    for (size_t i = 0; i < context->table_track_count; i++) {
        budget_free(budget, context->table_tracks[i].placements);
    }
    budget_free(budget, context->stacking_contexts);
    budget_free(budget, context->visibility_ranges);
    LayoutReuseCache *reuse = context->reuse;
    if (reuse != NULL && context->counter_entries != NULL
        && !context->cancelled && !context->counter_entries_allocation_failed
        && context->sheet != NULL
        && reuse->sheet == context->sheet
        && reuse->sheet_generation == context->sheet->build_generation
        && !stylesheet_has_container_queries(context->sheet)) {
        layout_reuse_clear_counters(reuse);
        reuse->counter_entries = context->counter_entries;
        reuse->counter_count = context->counter_entry_count;
        reuse->counter_capacity = context->counter_entry_capacity;
        reuse->counter_bounded_out = context->counter_entries_bounded_out;
        reuse->counter_root = lxb_dom_interface_node(context->document->html);
        reuse->stats.retained_bytes += reuse->counter_capacity
            * sizeof(*reuse->counter_entries);
        context->counter_entries = NULL;
    }
    budget_free(budget, context->counter_entries);
    budget_free(budget, context->counter_cursor);
    budget_free(budget, context);
}

bool layout_tree_enter(LayoutContext *context,
                              lxb_dom_node_t *node,
                              const char *phase)
{
    if (context == NULL || context->cancelled) return false;
    if (context->tree_call_depth >= LAYOUT_TREE_CALL_DEPTH_LIMIT) {
        if (!context->depth_limit_reported) {
            size_t name_length = 0;
            const char *name = document_element_name(node, &name_length);
            if (name == NULL && node != NULL
                && node->type == LXB_DOM_NODE_TYPE_TEXT) {
                name = "#text";
                name_length = 5;
            }
            fprintf(stderr,
                    "layout-depth-limit phase=%s limit=%u node=%.*s\n",
                    phase == NULL ? "unknown" : phase,
                    (unsigned) LAYOUT_TREE_CALL_DEPTH_LIMIT,
                    (int) name_length, name == NULL ? "" : name);
            context->depth_limit_reported = true;
        }
        context->depth_fallback_count++;
        return false;
    }
    context->tree_call_depth++;
    if (context->tree_call_depth > context->max_tree_call_depth) {
        context->max_tree_call_depth = context->tree_call_depth;
    }
    return true;
}

void layout_tree_leave(LayoutContext *context)
{
    if (context != NULL && context->tree_call_depth != 0) {
        context->tree_call_depth--;
    }
}

void layout_tree_note_fallback(LayoutContext *context,
                                      lxb_dom_node_t *node,
                                      const char *phase)
{
    if (context == NULL) return;
    context->depth_fallback_count++;
    if (context->depth_limit_reported) return;
    size_t name_length = 0;
    const char *name = document_element_name(node, &name_length);
    if (name == NULL && node != NULL
        && node->type == LXB_DOM_NODE_TYPE_TEXT) {
        name = "#text";
        name_length = 5;
    }
    fprintf(stderr, "layout-depth-limit phase=%s limit=%u node=%.*s\n",
            phase == NULL ? "unknown" : phase,
            (unsigned) LAYOUT_TREE_CALL_DEPTH_LIMIT,
            (int) name_length, name == NULL ? "" : name);
    context->depth_limit_reported = true;
}

static unsigned style_font_weight(const ComputedStyle *style)
{
    return computed_style_font_weight(style);
}

bool style_uses_bold_face(const ComputedStyle *style)
{
    return style_font_weight(style) >= LAYOUT_BOLD_FACE_WEIGHT;
}

bool style_uses_synthetic_weight(const FontSet *fonts,
                                        const WebFontSet *web_fonts,
                                        const ComputedStyle *style,
                                        const FontFace *face)
{
    unsigned weight = style_font_weight(style);
    return font_face_needs_synthetic_bold(fonts, web_fonts, face, weight);
}

/* DrawCommand is deliberately compact on PSP. Store CSS weights in ten-unit
   increments (600 becomes 60), which preserves the face-selection boundary
   without growing the display-list record; draw_command_font_weight()
   decodes it. */
uint8_t draw_font_weight_code_for_style(const ComputedStyle *style)
{
    unsigned encoded = (style_font_weight(style) + 5u) / 10u;
    return (uint8_t) (encoded > 100u ? 100u : encoded);
}

uint8_t text_decoration_bits(const ComputedStyle *style)
{
    if (!computed_style_has_text_underline(style)
        && !computed_style_has_ancestor_text_underline(style)) return 0;
    uint8_t bits = LAYOUT_TEXT_DECORATION_UNDERLINE;
    int pixels = 0;
    if (computed_style_effective_text_underline_offset(style, &pixels)) {
        bits |= (uint8_t) ((pixels + 16)
                          << LAYOUT_TEXT_UNDERLINE_OFFSET_SHIFT);
    }
    return bits;
}

const FontFace *draw_command_face(const LayoutDocument *layout,
                                  const DrawCommand *command)
{
    return font_context_face_at_weight(
        layout->fonts, layout->web_fonts, draw_command_font_family(command),
        draw_command_font_italic(command), draw_uses_bold_face(command),
        draw_command_font_weight(command));
}

DrawTextFace draw_command_text_face(const LayoutDocument *layout,
                                    const DrawCommand *command)
{
    DrawTextFace result;
    result.face = draw_command_face(layout, command);
    result.metric_family = font_context_metric_family(
        layout->web_fonts, draw_command_font_family(command), result.face);
    result.weight = draw_command_font_weight(command);
    result.bold_face = draw_uses_bold_face(command);
    result.synthetic_bold = font_face_needs_synthetic_bold(
        layout->fonts, layout->web_fonts, result.face, result.weight);
    return result;
}

void layout_finish_work_slice(LayoutContext *context)
{
    if (context == NULL || context->slice_units == 0) return;
    uint64_t finished_ns = tilefinch_platform_monotonic_time_ns();
    uint64_t elapsed_us = finished_ns >= context->slice_started_ns
        ? (finished_ns - context->slice_started_ns) / UINT64_C(1000) : 0;
    if (elapsed_us > context->layout->max_work_slice_us) {
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        if (elapsed_us > UINT64_C(33000)) {
            size_t name_length = 0, id_length = 0;
            const char *name = document_element_name(context->slice_last_node,
                                                      &name_length);
            const char *id = document_attribute(context->slice_last_node,
                                                "id", &id_length);
            char message[192];
            snprintf(message, sizeof(message),
                "tilefinch-layout-slow-slice: elapsed=%lluus units=%zu total=%zu node=%.*s id=%.*s",
                (unsigned long long) elapsed_us, context->slice_units,
                context->layout->layout_work_units,
                (int) (name_length > 24u ? 24u : name_length), name == NULL ? "" : name,
                (int) (id_length > 40u ? 40u : id_length), id == NULL ? "" : id);
            tilefinch_platform_log_message(message);
        }
#endif
        context->layout->max_work_slice_us = elapsed_us;
        context->layout->max_work_slice_units = context->slice_units;
        context->layout->max_work_slice_node = context->slice_last_node;
    }
    if (elapsed_us > 16000
        && LAYOUT_TRACE(context->layout, LAYOUT_SLICES)) {
        size_t name_length = 0, id_length = 0, class_length = 0;
        const char *name = document_element_name(
            context->slice_last_node, &name_length);
        const char *id = document_attribute(
            context->slice_last_node, "id", &id_length);
        const char *class_name = document_attribute(
            context->slice_last_node, "class", &class_length);
        fprintf(stderr,
                "layout-slice elapsed-us=%llu work=%zu total-work=%zu "
                "last=%.*s id=%.*s class=%.*s\n",
                (unsigned long long) elapsed_us, context->slice_units,
                context->layout->layout_work_units,
                (int) name_length, name == NULL ? "" : name,
                (int) id_length, id == NULL ? "" : id,
                (int) class_length, class_name == NULL ? "" : class_name);
    }
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    /* Stall attribution: the nodes a long slice spans. */
    if (elapsed_us >= UINT64_C(60000)) {
        size_t first_length = 0, last_length = 0, first_class_length = 0;
        const char *first = document_element_name(
            context->slice_first_node, &first_length);
        const char *last = document_element_name(
            context->slice_last_node, &last_length);
        const char *first_class = document_attribute(
            context->slice_first_node, "class", &first_class_length);
        char message[192];
        snprintf(message, sizeof(message),
            "tilefinch-layout-stall: elapsed=%lluus units=%zu "
            "from=%.*s.%.*s to=%.*s",
            (unsigned long long) elapsed_us, context->slice_units,
            (int) (first_length > 16u ? 16u : first_length),
            first == NULL ? "" : first,
            (int) (first_class_length > 32u ? 32u : first_class_length),
            first_class == NULL ? "" : first_class,
            (int) (last_length > 16u ? 16u : last_length),
            last == NULL ? "" : last);
        tilefinch_platform_log_message(message);
    }
    context->slice_first_node = context->slice_last_node;
#endif
    context->slice_units = 0;
    context->slice_started_ns = finished_ns;
}

/* Every cooperative refusal funnels through here so the owner can tell a
   cancelled build from a refused one after the context is gone. */
static void layout_context_cancel(LayoutContext *context)
{
    context->cancelled = true;
    if (context->layout != NULL) context->layout->performance.cancelled = true;
}

bool layout_cooperate(LayoutContext *context, lxb_dom_node_t *node)
{
    context->layout->layout_work_units++;
    context->slice_units++;
    context->slice_last_node = node;
#ifndef __PSP__
    if (tilefinch_test_faults()->cancel_next_layout_cooperate) {
        tilefinch_test_faults()->cancel_next_layout_cooperate = false;
        layout_context_cancel(context);
        return false;
    }
#endif
    if (context->slice_units < LAYOUT_WORK_QUOTA) {
        /* A node is not a time bound: native style/intrinsic work can make
           one node expensive on Allegrex. Keep the four-node amortization
           for cheap work, but service input at the next safe boundary after
           eight milliseconds, just like the script watchdog. */
        uint64_t now = tilefinch_platform_monotonic_time_ns();
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        context->safe_point_clock_checks++;
#endif
        if (now < context->slice_started_ns
            || now - context->slice_started_ns < UINT64_C(8000000)) return true;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        context->safe_point_time_yields++;
#endif
    }
    size_t completed = context->layout->layout_work_units;
    layout_finish_work_slice(context);
    context->layout->cooperative_yields++;
    LAYOUT_FLOW_SCOPE(context, LAYOUT_FLOW_COOPERATE);
    bool keep_going = tilefinch_platform_cooperate("layout", completed);
    if (!keep_going) layout_context_cancel(context);
    return keep_going;
}

bool layout_cooperate_timed(LayoutContext *context)
{
    if (context == NULL || context->cancelled) return false;
    uint64_t now = tilefinch_platform_monotonic_time_ns();
    if (now >= context->slice_started_ns
        && now - context->slice_started_ns < UINT64_C(8000000)) return true;
    context->layout->cooperative_yields++;
    bool keep_going = tilefinch_platform_cooperate(
        "layout-prepare", context->layout->layout_work_units);
    context->slice_started_ns = tilefinch_platform_monotonic_time_ns();
    if (!keep_going) layout_context_cancel(context);
    return keep_going;
}

static bool layout_selector_cooperate(
    void *opaque, lxb_dom_node_t *node, size_t completed_visits)
{
    LayoutContext *context = opaque;
    if (context == NULL || context->cancelled) return false;
    /* A root style can perform substantial selector work before the first
       ordinary layout unit. Give that interval a visible slice as well. */
    if (context->slice_units == 0) context->slice_units = 1;
    context->slice_last_node = node;
    layout_finish_work_slice(context);
    context->layout->cooperative_yields++;
    LAYOUT_FLOW_SCOPE(context, LAYOUT_FLOW_COOPERATE);
    bool keep_going = tilefinch_platform_cooperate(
        "layout-style", completed_visits);
    if (!keep_going) layout_context_cancel(context);
    return keep_going;
}

ComputedStyle layout_initial_root_style(void)
{
    return (ComputedStyle) {
        .display = DISPLAY_BLOCK, .color = 0x000000,
        .color_alpha = 255,
        .background = 0xffffff, .has_background = true,
        .background_alpha = 255,
        .font_scale = 2, .font_size = 16,
        .root_font_size = 16,
        .font_family = FONT_SANS,
        .border_color = UINT32_MAX, .border_alpha = 255,
        .opacity = 255,
        .max_width = STYLE_LENGTH_NONE,
        .max_height = STYLE_LENGTH_NONE,
        .flex_direction = FLEX_ROW, .flex_shrink = 512,
        .align_items = ALIGN_STRETCH,
        .justify_items = ALIGN_STRETCH,
        .align_content = JUSTIFY_STRETCH
    };
}

__attribute__((noinline))
void layout_style_for_node_into(LayoutContext *context,
                                       lxb_dom_node_t *node,
                                       const ComputedStyle *parent,
                                       ComputedStyle *result)
{
    context->style_resolutions++;
    /* Every layout path resolves each element's style here, including
       inline and list content that never reaches a block checkpoint (the
       article's 595 KB reference list). A declined check leaves the build
       cancelled for the next block checkpoint to return. */
    if ((context->style_resolutions & 7u) == 0)
        (void) layout_cooperate_timed(context);
    /* Open addressing over the fixed bounded array: hash the node pointer
       to a home slot and probe a short window, evicting the oldest stamp in
       the window on a miss.  This is the single owner of the cache keying
       decision (node pointer only; styles cannot change within one build). */
    const size_t mask = LAYOUT_STYLE_CACHE_CAPACITY - 1;
    size_t home = layout_pointer_hash(node) & mask;
    size_t replacement = home;
    uint64_t oldest = UINT64_MAX;
    for (size_t probe = 0; probe < LAYOUT_STYLE_CACHE_PROBE_LIMIT; probe++) {
        size_t slot = (home + probe) & mask;
        LayoutStyleCacheEntry *entry = &context->style_cache[slot];
        if (entry->node == node) {
            entry->stamp = ++context->style_cache_clock;
            context->style_cache_hits++;
            *result = entry->style;
            return;
        }
        if (entry->node == NULL) {
            replacement = slot;
            break;
        }
        if (entry->stamp < oldest) {
            oldest = entry->stamp;
            replacement = slot;
        }
    }
    LAYOUT_FLOW_SCOPE(context, LAYOUT_FLOW_STYLE);
#ifndef TILEFINCH_NO_TRACE
    bool reused = false;
    uint64_t style_started_us = layout_performance_now_us();
    reused = layout_reuse_cache_resolve_style(
        context->reuse, context->sheet, context->fonts,
        node, parent, result);
    if (!reused) {
        context->style_resolve_us +=
            layout_performance_now_us() - style_started_us;
    }
#else
    (void) layout_reuse_cache_resolve_style(
        context->reuse, context->sheet, context->fonts,
        node, parent, result);
#endif
    LayoutStyleCacheEntry *entry = &context->style_cache[replacement];
    if (style_selector_cooperation_cancelled(context->sheet)) {
        layout_context_cancel(context);
        return;
    }
    entry->node = node;
    entry->style = *result;
    entry->stamp = ++context->style_cache_clock;
    context->style_cache_misses++;
}

ComputedStyle layout_style_for_node(LayoutContext *context,
                                           lxb_dom_node_t *node,
                                           const ComputedStyle *parent)
{
    ComputedStyle style;
    layout_style_for_node_into(context, node, parent, &style);
    return style;
}


static lxb_dom_node_t *selected_option_node(lxb_dom_node_t *node,
                                            lxb_dom_node_t **first)
{
    for (lxb_dom_node_t *child = node == NULL ? NULL : node->first_child;
         child != NULL; child = child->next) {
        if (document_node_is_tag(child, LXB_TAG_OPTION)) {
            if (*first == NULL) *first = child;
            size_t state_length = 0;
            const char *state = document_attribute(
                child, "data-tilefinch-option-selected", &state_length);
            bool selected = state != NULL
                ? state_length == 4 && memcmp(state, "true", 4) == 0
                : lxb_dom_element_has_attribute(
                      lxb_dom_interface_element(child),
                      (const lxb_char_t *) "selected", 8);
            if (selected) {
                return child;
            }
        }
        lxb_dom_node_t *selected = selected_option_node(child, first);
        if (selected != NULL) return selected;
    }
    return NULL;
}

lxb_dom_node_t *select_display_option(lxb_dom_node_t *select)
{
    lxb_dom_node_t *first = NULL;
    lxb_dom_node_t *selected = selected_option_node(select, &first);
    return selected == NULL ? first : selected;
}

bool attribute_is(lxb_dom_node_t *node, const char *name,
                         const char *wanted)
{
    size_t length = 0;
    const char *value = document_attribute(node, name, &length);
    return value != NULL && length == strlen(wanted)
           && strncasecmp(value, wanted, length) == 0;
}

bool layout_node_is_hidden_input(lxb_dom_node_t *node)
{
    return document_node_is_tag(node, LXB_TAG_INPUT)
        && attribute_is(node, "type", "hidden");
}

static bool node_descends_from(lxb_dom_node_t *node, lxb_dom_node_t *ancestor)
{
    for (lxb_dom_node_t *at = node; at != NULL; at = at->parent) {
        if (at == ancestor) return true;
    }
    return false;
}

bool node_effectively_disabled(lxb_dom_node_t *node)
{
    if (node == NULL || node->type != LXB_DOM_NODE_TYPE_ELEMENT) return false;
    if (lxb_dom_element_has_attribute(
            lxb_dom_interface_element(node),
            (const lxb_char_t *) "disabled", 8)) return true;
    for (lxb_dom_node_t *at = node->parent; at != NULL; at = at->parent) {
        if (!document_node_is_tag(at, LXB_TAG_FIELDSET)
            || !lxb_dom_element_has_attribute(
                lxb_dom_interface_element(at),
                (const lxb_char_t *) "disabled", 8)) continue;
        lxb_dom_node_t *legend = NULL;
        for (lxb_dom_node_t *child = at->first_child; child != NULL;
             child = child->next) {
            if (document_node_is_tag(child, LXB_TAG_LEGEND)) { legend = child; break; }
        }
        if (legend != NULL && node_descends_from(node, legend)) continue;
        return true;
    }
    return false;
}

const char *layout_text_data(lxb_dom_node_t *node, size_t *length)
{
    const char *text = document_text_data(node, length);
    if (text != NULL && length != NULL && *length != 0
        && !document_shadow_text_rendered(node)) {
        *length = 0;
        return "";
    }
    return text;
}

const char *first_text_data(lxb_dom_node_t *node, size_t *length)
{
    if (length == NULL) return NULL;
    lxb_dom_node_t *boundary = node;
    lxb_dom_node_t *at = node == NULL ? NULL : node->first_child;
    size_t visited = 0;
    while (at != NULL && visited++ < LAYOUT_FALLBACK_VISIT_LIMIT) {
        const char *text = layout_text_data(at, length);
        if (text != NULL && *length != 0) return text;
        if (at->first_child != NULL) {
            at = at->first_child;
            continue;
        }
        while (at != boundary && at->next == NULL) at = at->parent;
        if (at == boundary) break;
        at = at->next;
    }
    *length = 0;
    return NULL;
}

static bool layout_node_index_rebuild(LayoutDocument *layout,
                                      size_t capacity)
{
    if (layout == NULL || capacity < 16
        || (capacity & (capacity - 1u)) != 0
        || capacity > SIZE_MAX / sizeof(*layout->node_index)) return false;
    LayoutNodeIndexEntry *entries = budget_calloc(
        layout->budget, capacity, sizeof(*entries));
    if (entries == NULL) return false;
    size_t mask = capacity - 1u;
    for (size_t i = 0; i < layout->node_index_capacity; i++) {
        LayoutNodeIndexEntry entry = layout->node_index[i];
        if (entry.node == NULL) continue;
        size_t slot = layout_pointer_hash(entry.node) & mask;
        while (entries[slot].node != NULL) slot = (slot + 1u) & mask;
        entries[slot] = entry;
    }
    budget_free(layout->budget, layout->node_index);
    layout->node_index = entries;
    layout->node_index_capacity = capacity;
    return true;
}

static bool layout_node_index_lookup(const LayoutDocument *layout,
                                     const lxb_dom_node_t *node,
                                     size_t *index)
{
    if (layout == NULL || node == NULL || layout->node_index == NULL
        || layout->node_index_capacity == 0) return false;
    size_t mask = layout->node_index_capacity - 1u;
    size_t slot = layout_pointer_hash(node) & mask;
    for (size_t probes = 0; probes < layout->node_index_capacity;
         probes++, slot = (slot + 1u) & mask) {
        const LayoutNodeIndexEntry *entry = &layout->node_index[slot];
        if (entry->node == NULL) return false;
        if (entry->node == node) {
            if (entry->index >= layout->node_box_count) return false;
            if (index != NULL) *index = entry->index;
            return true;
        }
    }
    return false;
}

static void layout_node_index_insert(LayoutDocument *layout,
                                     lxb_dom_node_t *node, size_t index)
{
    if (layout == NULL || node == NULL || index > UINT32_MAX
        || layout->node_index_growth_disabled) return;
    if (layout->node_index_capacity == 0
        && !layout_node_index_rebuild(layout, 128)) {
        layout->node_index_growth_disabled = true;
        return;
    }
    if (layout->node_index_count + 1u
        >= layout->node_index_capacity
             - layout->node_index_capacity / 4u) {
        if (layout->node_index_capacity > SIZE_MAX / 2u
            || !layout_node_index_rebuild(
                layout, layout->node_index_capacity * 2u)) {
            layout->node_index_growth_disabled = true;
            return;
        }
    }
    size_t mask = layout->node_index_capacity - 1u;
    size_t slot = layout_pointer_hash(node) & mask;
    while (layout->node_index[slot].node != NULL) {
        if (layout->node_index[slot].node == node) {
            layout->node_index[slot].index = (uint32_t) index;
            return;
        }
        slot = (slot + 1u) & mask;
    }
    layout->node_index[slot] = (LayoutNodeIndexEntry) {
        .node = node, .index = (uint32_t) index
    };
    layout->node_index_count++;
}

static size_t layout_node_box_index(const LayoutDocument *layout,
                                    const lxb_dom_node_t *node)
{
    size_t index = 0;
    if (layout_node_index_lookup(layout, node, &index)) return index;
    /* A complete index proves absence too. Partial indexes (allocation
       refusal, or a visual clone still being populated) retain the scan. */
    if (layout->node_index != NULL
        && layout->node_index_count == layout->node_box_count) return SIZE_MAX;
#ifndef __PSP__
    tilefinch_test_faults()->layout_node_box_scans++;
#endif
    for (size_t i = 0; i < layout->node_box_count; i++) {
        if (layout->node_boxes[i].node == node) return i;
    }
    return SIZE_MAX;
}

static bool layout_set_node_clip_radius_code(
    LayoutDocument *layout, size_t index, int code)
{
    if (layout == NULL || index >= layout->node_box_capacity) return false;
    if (style_border_radius_is_packed(code)
        && layout->node_clip_radius_codes == NULL) {
        layout->node_clip_radius_codes = budget_calloc(
            layout->budget, layout->node_box_capacity,
            sizeof(*layout->node_clip_radius_codes));
        if (layout->node_clip_radius_codes == NULL) return false;
    }
    if (layout->node_clip_radius_codes != NULL) {
        layout->node_clip_radius_codes[index] =
            style_border_radius_is_packed(code) ? code : 0;
    }
    return true;
}

int layout_node_box_clip_radius_code(
    const LayoutDocument *layout, const LayoutNodeBox *box)
{
    if (layout == NULL || box == NULL || layout->node_boxes == NULL
        || box < layout->node_boxes
        || box >= layout->node_boxes + layout->node_box_count) return 0;
    size_t index = (size_t) (box - layout->node_boxes);
    int code = layout->node_clip_radius_codes == NULL
        ? 0 : layout->node_clip_radius_codes[index];
    return code != 0 ? code : (int) layout_node_box_clip_radius(box);
}

int layout_node_box_effective_clip_radius_code(
    const LayoutDocument *layout, const LayoutNodeBox *box)
{
    if (layout == NULL || box == NULL) return 0;
    int code = layout_node_box_clip_radius_code(layout, box);
    StyleOverflowClipBox clip_box = layout_node_box_clip_box(box);
    int border_inset_x = (box->width - box->client_width) / 2;
    int border_inset_y = (box->height - box->client_height) / 2;
    if (border_inset_x < 0) border_inset_x = 0;
    if (border_inset_y < 0) border_inset_y = 0;
    int border_inset = border_inset_x > border_inset_y
        ? border_inset_x : border_inset_y;
    /* CSS defines every overflow-clip edge as one cumulative offset from
       the padding edge. Do not clamp at the selected content/border edge and
       then expand: that loses curvature when a positive clip margin crosses
       an intermediate sharp edge. */
    code = style_border_radius_adjust(code, -border_inset);
    int outset = (int) layout_node_box_clip_margin(box);
    if (clip_box == STYLE_OVERFLOW_CLIP_BORDER_BOX) {
        outset += border_inset;
    } else if (clip_box == STYLE_OVERFLOW_CLIP_CONTENT_BOX) {
        int selected_inset = box->clip_inset_left > box->clip_inset_top
            ? box->clip_inset_left : box->clip_inset_top;
        int padding_inset = selected_inset - border_inset;
        if (padding_inset > 0) outset -= padding_inset;
    }
    return style_border_radius_outset(
        code, outset, box->client_width, box->client_height);
}

static void layout_build_focus_index(LayoutDocument *layout)
{
    if (layout == NULL || layout->focus_index != NULL) return;
    if (layout->link_count > SIZE_MAX - layout->control_count) return;
    size_t regions = layout->link_count + layout->control_count;
    if (regions == 0 || regions > UINT32_MAX || regions > SIZE_MAX / 2u
        || regions * 2u
               > SIZE_MAX / sizeof(LayoutFocusIndexEntry)) {
        return;
    }
    size_t capacity = 16;
    while (capacity < regions * 2u) {
        if (capacity > SIZE_MAX / 2u) return;
        capacity *= 2u;
    }
    LayoutFocusIndexEntry *entries = budget_calloc(
        layout->budget, capacity, sizeof(*entries));
    if (entries == NULL) return;
    size_t mask = capacity - 1u;
    for (size_t kind = 0; kind < 2; kind++) {
        size_t count = kind == 0 ? layout->link_count : layout->control_count;
        for (size_t i = 0; i < count; i++) {
            lxb_dom_node_t *node = kind == 0 ? layout->links[i].node
                                             : layout->controls[i].node;
            if (node == NULL) continue;
            size_t slot = layout_pointer_hash(node) & mask;
            while (entries[slot].node != NULL
                   && entries[slot].node != node) {
                slot = (slot + 1u) & mask;
            }
            if (entries[slot].node == NULL) entries[slot].node = node;
            if (kind == 0 && entries[slot].link_plus_one == 0) {
                entries[slot].link_plus_one = (uint32_t) i + 1u;
            } else if (kind == 1 && entries[slot].control_plus_one == 0) {
                entries[slot].control_plus_one = (uint32_t) i + 1u;
            }
        }
    }
    layout->focus_index = entries;
    layout->focus_index_capacity = capacity;
}

static bool focus_pseudo_text_command(const DrawCommand *command)
{
    unsigned pseudo = command->image_fit & LAYOUT_TEXT_FOCUS_PSEUDO_MASK;
    return command->type == DRAW_TEXT && command->image != NULL
        && (pseudo == PSEUDO_BEFORE || pseudo == PSEUDO_AFTER);
}

/* Inline layout gives at most 64 generated boxes per build focus
   visibility identity (focus_pseudo_visibility_slots); a box can still emit
   several text commands, so count before allocating. */
static void layout_index_focus_pseudo_texts(LayoutDocument *layout,
                                            bool any)
{
    layout->focus_pseudo_texts_indexed = false;
    if (!any) {
        layout->focus_pseudo_text_count = 0;
        layout->focus_pseudo_texts_indexed = true;
        return;
    }
    if (layout->count > UINT32_MAX) return;
    size_t count = 0;
    for (size_t i = 0; i < layout->count; i++)
        if (focus_pseudo_text_command(&layout->commands[i])) count++;
    uint32_t *indices = count == 0 ? NULL
        : budget_malloc(layout->budget, count * sizeof(*indices));
    if (count != 0 && indices == NULL) return;
    size_t at = 0;
    for (size_t i = 0; i < layout->count && at < count; i++)
        if (focus_pseudo_text_command(&layout->commands[i]))
            indices[at++] = (uint32_t) i;
    layout->focus_pseudo_texts = indices;
    layout->focus_pseudo_text_count = (uint32_t) count;
    layout->focus_pseudo_texts_indexed = true;
}

bool add_node_box(LayoutDocument *layout, lxb_dom_node_t *node,
                         int x, int y, int width, int height,
                         int client_width, int client_height,
                         int content_width, int content_height,
                         int padding_horizontal, int padding_vertical,
                         bool clips_x, bool clips_y, int clip_radius,
                         uint8_t overflow_clip_margin,
                         int clip_inset_left, int clip_inset_top,
                         bool clip_only_x, bool clip_only_y,
                         uint8_t positioned_ancestor_distance,
                         bool cssom_geometry_authoritative,
                         size_t command_start, size_t command_end,
                         size_t scroll_command_start,
                         size_t scroll_command_end,
                         size_t link_start, size_t link_end,
                         size_t control_start, size_t control_end)
{
    if (node == NULL || width < 0 || height < 0) return false;
    if (command_start > UINT32_MAX || command_end > UINT32_MAX
        || scroll_command_start > UINT32_MAX
        || scroll_command_end > UINT32_MAX
        || link_start > UINT32_MAX || link_end > UINT32_MAX
        || control_start > UINT32_MAX || control_end > UINT32_MAX
        || link_start > link_end || control_start > control_end) return false;
    if ((clips_x || clips_y) && LAYOUT_TRACE(layout, CLIP)) {
        size_t id_length = 0, class_length = 0;
        const char *id = document_attribute(node, "id", &id_length);
        const char *class_name = document_attribute(node, "class",
                                                     &class_length);
        fprintf(stderr, "layout-clip id=%.*s class=%.*s box=%d,%d,%dx%d "
                "client=%dx%d content=%dx%d axes=%d/%d clip-only=%d/%d "
                "radius=%d commands=%zu..%zu\n",
                (int) id_length, id == NULL ? "" : id,
                (int) class_length, class_name == NULL ? "" : class_name,
                x, y, width, height, client_width, client_height,
                content_width, content_height, clips_x, clips_y,
                clip_only_x, clip_only_y, clip_radius,
                scroll_command_start, scroll_command_end);
    }
    size_t existing = layout_node_box_index(layout, node);
    if (existing != SIZE_MAX) {
        size_t i = existing;
        if (layout->retain_container_padding
            && layout->node_container_padding_sums != NULL
            && i < layout->node_container_padding_capacity) {
            unsigned horizontal = padding_horizontal < 0 ? 0u
                : (padding_horizontal > UINT16_MAX
                   ? UINT16_MAX : (unsigned) padding_horizontal);
            unsigned vertical = padding_vertical < 0 ? 0u
                : (padding_vertical > UINT16_MAX
                   ? UINT16_MAX : (unsigned) padding_vertical);
            layout->node_container_padding_sums[i] =
                horizontal | (vertical << 16);
        }
        uint32_t *interactions = layout->node_interaction_ranges
            + i * LAYOUT_NODE_INTERACTION_STRIDE;
        interactions[LAYOUT_NODE_LINK_START] = (uint32_t) link_start;
        interactions[LAYOUT_NODE_LINK_END] = (uint32_t) link_end;
        interactions[LAYOUT_NODE_CONTROL_START] = (uint32_t) control_start;
        interactions[LAYOUT_NODE_CONTROL_END] = (uint32_t) control_end;
        layout->node_boxes[i] = (LayoutNodeBox) {
            .node = node, .x = x, .y = y, .width = width, .height = height,
            .client_width = client_width, .client_height = client_height,
            .content_width = content_width, .content_height = content_height,
            .scroll_x = layout->node_boxes[i].scroll_x,
            .scroll_y = layout->node_boxes[i].scroll_y,
            .command_start = (uint32_t) command_start,
            .command_end = (uint32_t) command_end,
            .scroll_command_start = (uint32_t) scroll_command_start,
            .scroll_command_end = (uint32_t) scroll_command_end,
            .clip_radius = (uint16_t) (
                (style_border_radius_maximum(clip_radius)
                     > (int) LAYOUT_CLIP_RADIUS_MASK
                    ? LAYOUT_CLIP_RADIUS_MASK
                    : style_border_radius_maximum(clip_radius))
                | ((overflow_clip_margin
                    & STYLE_OVERFLOW_CLIP_MARGIN_MASK)
                   << LAYOUT_CLIP_MARGIN_SHIFT)),
            .clips_x = clips_x, .clips_y = clips_y,
            .clip_only_x = clip_only_x, .clip_only_y = clip_only_y,
            .clip_flags_reserved = (uint8_t) (
                (overflow_clip_margin & STYLE_OVERFLOW_CLIP_BOX_MASK)
                >> STYLE_OVERFLOW_CLIP_BOX_SHIFT),
            .positioned_ancestor_distance = positioned_ancestor_distance,
            .cssom_geometry_authoritative = cssom_geometry_authoritative,
            .clip_inset_left = (uint8_t) (
                clip_inset_left > UINT8_MAX ? UINT8_MAX
                : (clip_inset_left < 0 ? 0 : clip_inset_left)),
            .clip_inset_top = (uint8_t) (
                clip_inset_top > UINT8_MAX ? UINT8_MAX
                : (clip_inset_top < 0 ? 0 : clip_inset_top))
        };
        int retained_clip_radius = clips_x && clips_y
            ? clip_radius : style_border_radius_maximum(clip_radius);
        if (!layout_set_node_clip_radius_code(
                layout, i, retained_clip_radius)) {
            return false;
        }
        layout_node_index_insert(layout, node, i);
        return true;
    }
    if (layout->node_box_count == layout->node_box_capacity) {
        size_t capacity = layout->node_box_capacity == 0
                          ? 64 : layout->node_box_capacity * 2;
        if (capacity > SIZE_MAX
                       / (LAYOUT_NODE_INTERACTION_STRIDE
                          * sizeof(uint32_t))) return false;
        uint32_t *interactions = budget_realloc(
            layout->budget, layout->node_interaction_ranges,
            capacity * LAYOUT_NODE_INTERACTION_STRIDE
                * sizeof(*interactions));
        if (interactions == NULL) return false;
        layout->node_interaction_ranges = interactions;
        layout->node_interaction_capacity = capacity;
        if (layout->retain_container_padding) {
            uint32_t *padding_sums = budget_realloc(
                layout->budget, layout->node_container_padding_sums,
                capacity * sizeof(*padding_sums));
            if (padding_sums == NULL) return false;
            memset(padding_sums + layout->node_container_padding_capacity, 0,
                   (capacity - layout->node_container_padding_capacity)
                       * sizeof(*padding_sums));
            layout->node_container_padding_sums = padding_sums;
            int32_t *contained = budget_realloc(
                layout->budget, layout->node_container_contained_heights,
                capacity * sizeof(*contained));
            if (contained == NULL) return false;
            layout->node_container_contained_heights = contained;
            layout->node_container_padding_capacity = capacity;
        }
        LayoutNodeBox *boxes = budget_realloc(
            layout->budget, layout->node_boxes, capacity * sizeof(*boxes));
        if (boxes == NULL) return false;
        layout->node_boxes = boxes;
        if (layout->node_clip_radius_codes != NULL) {
            int32_t *codes = budget_realloc(
                layout->budget, layout->node_clip_radius_codes,
                capacity * sizeof(*codes));
            if (codes == NULL) return false;
            memset(codes + layout->node_box_capacity, 0,
                   (capacity - layout->node_box_capacity) * sizeof(*codes));
            layout->node_clip_radius_codes = codes;
        }
        layout->node_box_capacity = capacity;
    }
    size_t index = layout->node_box_count++;
    if (layout->retain_container_padding) {
        layout->node_container_contained_heights[index] = -1;
        unsigned horizontal = padding_horizontal < 0 ? 0u
            : (padding_horizontal > UINT16_MAX
               ? UINT16_MAX : (unsigned) padding_horizontal);
        unsigned vertical = padding_vertical < 0 ? 0u
            : (padding_vertical > UINT16_MAX
               ? UINT16_MAX : (unsigned) padding_vertical);
        layout->node_container_padding_sums[index] =
            horizontal | (vertical << 16);
    }
    uint32_t *interactions =
        layout->node_interaction_ranges
        + index * LAYOUT_NODE_INTERACTION_STRIDE;
    interactions[LAYOUT_NODE_LINK_START] = (uint32_t) link_start;
    interactions[LAYOUT_NODE_LINK_END] = (uint32_t) link_end;
    interactions[LAYOUT_NODE_CONTROL_START] = (uint32_t) control_start;
    interactions[LAYOUT_NODE_CONTROL_END] = (uint32_t) control_end;
    layout->node_boxes[index] = (LayoutNodeBox) {
        .node = node, .x = x, .y = y, .width = width, .height = height,
        .client_width = client_width, .client_height = client_height,
        .content_width = content_width, .content_height = content_height,
        .command_start = (uint32_t) command_start,
        .command_end = (uint32_t) command_end,
        .scroll_command_start = (uint32_t) scroll_command_start,
        .scroll_command_end = (uint32_t) scroll_command_end,
        .clip_radius = (uint16_t) (
            (style_border_radius_maximum(clip_radius)
                 > (int) LAYOUT_CLIP_RADIUS_MASK
                ? LAYOUT_CLIP_RADIUS_MASK
                : style_border_radius_maximum(clip_radius))
            | ((overflow_clip_margin
                & STYLE_OVERFLOW_CLIP_MARGIN_MASK)
               << LAYOUT_CLIP_MARGIN_SHIFT)),
        .clips_x = clips_x, .clips_y = clips_y,
        .clip_only_x = clip_only_x, .clip_only_y = clip_only_y,
        .clip_flags_reserved = (uint8_t) (
            (overflow_clip_margin & STYLE_OVERFLOW_CLIP_BOX_MASK)
            >> STYLE_OVERFLOW_CLIP_BOX_SHIFT),
        .positioned_ancestor_distance = positioned_ancestor_distance,
        .cssom_geometry_authoritative = cssom_geometry_authoritative,
        .clip_inset_left = (uint8_t) (
            clip_inset_left > UINT8_MAX ? UINT8_MAX
            : (clip_inset_left < 0 ? 0 : clip_inset_left)),
        .clip_inset_top = (uint8_t) (
            clip_inset_top > UINT8_MAX ? UINT8_MAX
            : (clip_inset_top < 0 ? 0 : clip_inset_top))
    };
    int retained_clip_radius = clips_x && clips_y
        ? clip_radius : style_border_radius_maximum(clip_radius);
    if (!layout_set_node_clip_radius_code(
            layout, index, retained_clip_radius)) {
        return false;
    }
    layout_node_index_insert(layout, node, index);
    return true;
}

static void compact_layout_storage(LayoutDocument *layout)
{
    /* Interaction ranges are needed only while flex/grid/table placement can
       still translate item subtrees. Do not retain this parallel sidecar
       after geometry is final. */
    budget_free(layout->budget, layout->node_interaction_ranges);
    layout->node_interaction_ranges = NULL;
    layout->node_interaction_capacity = 0;
    if (layout->count != 0 && layout->count < layout->capacity) {
        DrawCommand *commands = budget_realloc(
            layout->budget, layout->commands,
            layout->count * sizeof(*commands));
        if (commands != NULL) {
            layout->commands = commands;
            layout->capacity = layout->count;
        }
    }
    if (layout->bidi_glyph_count != 0
        && layout->bidi_glyph_count < layout->bidi_glyph_capacity) {
        LayoutBidiGlyph *glyphs = budget_realloc(
            layout->budget, layout->bidi_glyphs,
            layout->bidi_glyph_count * sizeof(*glyphs));
        if (glyphs != NULL) {
            layout->bidi_glyphs = glyphs;
            layout->bidi_glyph_capacity = layout->bidi_glyph_count;
        }
    }
    if (layout->bidi_command_count != 0
        && layout->bidi_command_count < layout->bidi_command_capacity) {
        LayoutBidiCommand *commands = budget_realloc(
            layout->budget, layout->bidi_commands,
            layout->bidi_command_count * sizeof(*commands));
        if (commands != NULL) {
            layout->bidi_commands = commands;
            layout->bidi_command_capacity = layout->bidi_command_count;
        }
    }
    if (layout->link_count != 0
        && layout->link_count < layout->link_capacity) {
        LinkRegion *links = budget_realloc(
            layout->budget, layout->links,
            layout->link_count * sizeof(*links));
        if (links != NULL) {
            layout->links = links;
            layout->link_capacity = layout->link_count;
        }
    }
    if (layout->control_count != 0
        && layout->control_count < layout->control_capacity) {
        ControlRegion *controls = budget_realloc(
            layout->budget, layout->controls,
            layout->control_count * sizeof(*controls));
        if (controls != NULL) {
            layout->controls = controls;
            layout->control_capacity = layout->control_count;
        }
    }
    if (layout->sticky_count != 0
        && layout->sticky_count < layout->sticky_capacity) {
        StickyRange *ranges = budget_realloc(
            layout->budget, layout->sticky_ranges,
            layout->sticky_count * sizeof(*ranges));
        if (ranges != NULL) {
            layout->sticky_ranges = ranges;
            layout->sticky_capacity = layout->sticky_count;
        }
    }
    if (layout->fixed_count != 0
        && layout->fixed_count < layout->fixed_capacity) {
        FixedRange *ranges = budget_realloc(
            layout->budget, layout->fixed_ranges,
            layout->fixed_count * sizeof(*ranges));
        if (ranges != NULL) {
            layout->fixed_ranges = ranges;
            layout->fixed_capacity = layout->fixed_count;
        }
    }
    if (layout->node_box_count == 0) {
        budget_free(layout->budget, layout->node_boxes);
        budget_free(layout->budget, layout->node_clip_radius_codes);
        layout->node_boxes = NULL;
        layout->node_clip_radius_codes = NULL;
        layout->node_box_capacity = 0;
    } else if (layout->node_box_count < layout->node_box_capacity) {
        LayoutNodeBox *boxes = budget_realloc(
            layout->budget, layout->node_boxes,
            layout->node_box_count * sizeof(*boxes));
        if (boxes != NULL) {
            layout->node_boxes = boxes;
            layout->node_box_capacity = layout->node_box_count;
        }
    }
}

void trace_flex_translation(LayoutContext *context,
                                   const char *phase,
                                   lxb_dom_node_t *container,
                                   lxb_dom_node_t *item,
                                   int alignment,
                                   int content_x,
                                   int content_width,
                                   int box_x,
                                   int box_width,
                                   int target_x,
                                   int dx)
{
    if (!LAYOUT_TRACE(context->layout, FLEX_TRANSLATE)
        || context->trace_flex_translate_lines++ >= 64) return;
    size_t container_class_length = 0;
    size_t item_class_length = 0;
    const char *container_class = document_attribute(
        container, "class", &container_class_length);
    const char *item_class = document_attribute(
        item, "class", &item_class_length);
    fprintf(stderr,
            "layout-flex-translate phase=%s container=%.*s item=%.*s "
            "alignment=%d content=%d/%d box=%d/%d target=%d dx=%d\n",
            phase,
            (int) container_class_length,
            container_class == NULL ? "" : container_class,
            (int) item_class_length,
            item_class == NULL ? "" : item_class,
            alignment, content_x, content_width, box_x, box_width,
            target_x, dx);
}

static bool trace_flex_container_matches(const LayoutContext *context,
                                         lxb_dom_node_t *container)
{
    const char *wanted =
        TILEFINCH_TRACE_COMPILED_IN != 0 ? context->layout->trace_flex_class
                                      : NULL;
    if (wanted == NULL || wanted[0] == '\0' || container == NULL) {
        return false;
    }
    size_t class_length = 0;
    const char *class_name = document_attribute(container, "class",
                                                 &class_length);
    return class_name != NULL && strstr(class_name, wanted) != NULL;
}

void trace_flex_sizing(LayoutContext *context, const char *phase,
                              lxb_dom_node_t *container,
                              lxb_dom_node_t *item_node,
                              const ComputedStyle *item_style,
                              int content_width,
                              int total_basis, int remaining, int cursor_x,
                              int basis, int used_width)
{
    if (!trace_flex_container_matches(context, container)
        || context->trace_flex_sizing_lines++ >= 128) return;
    size_t container_class_length = 0, item_class_length = 0;
    const char *container_class = document_attribute(
        container, "class", &container_class_length);
    const char *item_class = item_node == NULL ? NULL
        : document_attribute(item_node, "class", &item_class_length);
    fprintf(stderr,
            "layout-flex-size phase=%s container=%.*s item=%.*s "
            "content=%d total-basis=%d remaining=%d cursor=%d basis=%d "
            "used=%d grow=%d shrink=%d margins=%d/%d min=%d max=%d\n",
            phase, (int) container_class_length,
            container_class == NULL ? "" : container_class,
            (int) item_class_length, item_class == NULL ? "" : item_class,
            content_width, total_basis, remaining, cursor_x, basis,
            used_width, item_style == NULL ? 0 : item_style->flex_grow,
            item_style == NULL ? 0 : item_style->flex_shrink,
            item_style == NULL ? 0 : item_style->margin.left,
            item_style == NULL ? 0 : item_style->margin.right,
            item_style == NULL ? 0 : item_style->min_width,
            item_style == NULL ? 0 : item_style->max_width);
}

static bool layout_work_cooperate(LayoutContext *context,
                                  size_t work_units, const char *phase,
                                  bool account_work)
{
    if (context == NULL || work_units == 0) return true;
    /* Index/ancestor revisits need safe points, but are not new page work
       for the existing optional-resource publication ceiling. */
    if (account_work) {
        if (work_units <= SIZE_MAX - context->layout->layout_work_units)
            context->layout->layout_work_units += work_units;
        else context->layout->layout_work_units = SIZE_MAX;
    }
    context->slice_units = work_units;
    context->slice_last_node = NULL;
    layout_finish_work_slice(context);
    context->layout->cooperative_yields++;
    bool keep_going = tilefinch_platform_cooperate(
        phase, context->layout->layout_work_units);
    if (!keep_going) layout_context_cancel(context);
    return keep_going;
}

bool layout_batch_cooperate(LayoutContext *context, size_t work_units)
{
    return layout_work_cooperate(context, work_units, "layout-index", true);
}

bool layout_index_poll(LayoutContext *context, size_t work_units)
{
    return layout_work_cooperate(context, work_units, "layout-index", false);
}

bool layout_text_cooperate(LayoutContext *context, size_t work_units)
{
    return layout_work_cooperate(context, work_units, "layout-text", true);
}

bool layout_batch_checkpoint(LayoutContext *context, size_t at,
                                    size_t count, size_t *checkpoint_at)
{
    if (context == NULL || at + 1 < *checkpoint_at) return true;
    size_t completed = at + 1;
    size_t units = completed > count ? count : completed;
    units -= *checkpoint_at - LAYOUT_BATCH_STRIDE;
    *checkpoint_at = completed + LAYOUT_BATCH_STRIDE;
    return layout_batch_cooperate(context, units);
}

static void compact_layout_storage(LayoutDocument *layout);
static void layout_build_focus_index(LayoutDocument *layout);
static bool layout_collect_container_state(Stylesheet *stylesheet,
                                           Budget *budget,
                                           const LayoutDocument *layout);
static bool layout_document_has_inline_container_units(
    const PocDocument *document, const Stylesheet *stylesheet);

typedef enum {
    LAYOUT_JOB_INIT = 0,
    LAYOUT_JOB_FLOW,
    LAYOUT_JOB_COMPACT,
    LAYOUT_JOB_VISIBILITY,
    LAYOUT_JOB_PAINT_ORDER,
    LAYOUT_JOB_SPATIAL_INDEX,
    LAYOUT_JOB_SCROLL_METADATA,
    LAYOUT_JOB_FINISH,
    LAYOUT_JOB_CONTAINER_STATE,
    LAYOUT_JOB_DONE,
    LAYOUT_JOB_ERROR
} LayoutJobPhase;
_Static_assert(LAYOUT_JOB_ERROR + 1 < LAYOUT_BUILD_FAILED_AT_STARTUP,
               "startup failures must not alias a one-based job phase");

struct LayoutBuildJob {
    Budget *budget;
    const PocDocument *document;
    const Stylesheet *stylesheet;
    const FontSet *fonts;
    const ImageResources *images;
    ViewportContext viewport;
    LayoutReuseCache *reuse;
    LayoutDocument layout;
    LayoutContext *context;
    lxb_dom_node_t *body;
    ComputedStyle html_style;
    ComputedStyle body_style;
    int viewport_width;
    int viewport_height;
    int html_height;
    int bottom;
    int preview_y_limit;
    size_t fingerprint_at;
    uint64_t text_fingerprint;
    uint64_t build_active_us;
    LayoutJobPhase phase;
    unsigned failed_phase;
    LayoutBuildStatus status;
    size_t resumable_phases;
    size_t resumable_passes;
    uint64_t maximum_resumable_phase_us;
    uint64_t container_previous_signature;
    /* One-pass container queries (style.h): eligible for this build, and
       the current pass owns the evaluation log. */
    uint64_t container_log_generation;
    bool container_log_eligible;
    bool container_log;
#ifndef TILEFINCH_NO_TRACE
    LayoutDocument container_check;
    bool container_check_pending;
#endif
    bool probe_pass;
    bool retain_container_padding;
    bool preview_truncated;
    bool taken;
};

#ifndef TILEFINCH_NO_TRACE
/* TILEFINCH_VERIFY_CONTAINER_QUERY_LOG: a verified one-pass layout must equal
   the measured rebuild exactly. Reports any difference on stderr. */
static void layout_job_check_container_log(LayoutBuildJob *job)
{
    if (!job->container_check_pending) return;
    job->container_check_pending = false;
    const LayoutDocument *a = &job->container_check, *b = &job->layout;
    size_t mismatch = SIZE_MAX;
    if (a->count != b->count || a->height != b->height
        || a->width != b->width || a->node_box_count != b->node_box_count) {
        mismatch = 0;
    }
    for (size_t i = 0; mismatch == SIZE_MAX && i < a->count; i++) {
        const DrawCommand *x = &a->commands[i], *y = &b->commands[i];
        if (x->type != y->type || x->x != y->x || x->y != y->y
            || x->width != y->width || x->height != y->height
            || x->color != y->color || x->image != y->image
            || x->text_length != y->text_length
            || (x->type == DRAW_TEXT && x->text_length != 0
                && x->text != NULL && y->text != NULL
                && memcmp(x->text, y->text, x->text_length) != 0))
            mismatch = i;
    }
    for (size_t i = 0; mismatch == SIZE_MAX && i < a->node_box_count; i++) {
        const LayoutNodeBox *x = &a->node_boxes[i], *y = &b->node_boxes[i];
        if (x->node != y->node || x->x != y->x || x->y != y->y
            || x->width != y->width || x->height != y->height)
            mismatch = i;
    }
    fprintf(stderr, "container-query-log-check %s commands=%zu/%zu "
            "height=%d/%d boxes=%zu/%zu at=%zu\n",
            mismatch == SIZE_MAX ? "same" : "MISMATCH", a->count, b->count,
            a->height, b->height, a->node_box_count, b->node_box_count,
            mismatch == SIZE_MAX ? 0 : mismatch);
    layout_destroy(&job->container_check);
}
#endif

static void layout_job_end_container_log(LayoutBuildJob *job)
{
    if (job == NULL || !job->container_log) return;
    style_container_log_end((Stylesheet *) job->stylesheet, job);
    job->container_log = false;
}

static void layout_job_fail(LayoutBuildJob *job,
                            LayoutBuildStatus status)
{
    if (job == NULL || job->status != LAYOUT_BUILD_PENDING) return;
    job->failed_phase = (unsigned) job->phase + 1u;
    layout_job_end_container_log(job);
    if (job->context != NULL) {
        layout_release_context(job->context, job->budget);
        job->context = NULL;
    }
    layout_destroy(&job->layout);
    job->phase = LAYOUT_JOB_ERROR;
    job->status = status;
}

static bool layout_job_init(LayoutBuildJob *job)
{
    LayoutDocument *layout = &job->layout;
#if !defined(__PSP__)
    if (tilefinch_test_faults()->refuse_next_layout_context) {
        tilefinch_test_faults()->refuse_next_layout_context = false;
        budget_inject_failure_after(job->budget, 0);
    }
#endif
    LayoutContext *context = budget_calloc(job->budget, 1, sizeof(*context));
    if (context == NULL) return false;
    job->context = context;
    /* After a complete layout measured the geometry, a complete relayout
       (a script mutation, an image batch) usually measures the same and
       keeps its single probe; the log would only add its lookups. */
    if (job->probe_pass && job->container_log_eligible
        && !job->container_log
        && !(job->preview_y_limit <= 0
             && job->stylesheet->container_state_complete)) {
        job->container_log = style_container_log_begin(
            (Stylesheet *) job->stylesheet, job->budget, job);
        job->container_log_generation = job->document->content_generation;
    }
    context->container_live = job->container_log;
    memset(layout, 0, sizeof(*layout));
    layout->retain_container_padding = job->retain_container_padding;
    layout->image_content_generation = job->document->content_generation;
    layout->image_style_generation = document_style_generation();
    layout->image_sheet_generation = job->stylesheet->build_generation;
    layout->image_resource_count = job->images == NULL ? 0 : job->images->count;
    layout_read_trace_environment(layout);
    layout->budget = job->budget;
    layout->width = job->viewport_width;
    layout->scroll_width = job->viewport_width;
    layout->viewport = job->viewport;
    layout->fonts = job->fonts;
    layout->web_fonts = stylesheet_web_font_set(job->stylesheet);
    layout->page_background = 0xffffff;
    job->body = document_body_node(job->document);
    if (job->body == NULL) return false;

    context->layout = layout;
    context->document = job->document;
    context->sheet = job->stylesheet;
    context->fonts = job->fonts;
    context->web_fonts = layout->web_fonts;
    context->images = job->images;
    context->reuse = job->reuse;
    context->preview_y_limit = job->preview_y_limit;
    context->record_unresolved_visuals =
        job->reuse != NULL && job->reuse->record_unresolved_visuals;
    context->document_bidi_text_present = job->document->bidi_text_present;
    context->document_bidi_markup_present =
        job->document->bidi_markup_present;
    context->slice_started_ns = tilefinch_platform_monotonic_time_ns();
    tilefinch_platform_trace_step("layout-bidi");
    layout_bidi_prepare(context, job->budget);
    tilefinch_platform_trace_step("layout-root-style");
    if (context->cancelled) return false;
    if (job->stylesheet != NULL) {
        context->style_variable_cache_owned = style_variable_cache_begin(
            (Stylesheet *) job->stylesheet, job->budget);
        context->style_variable_cache_bytes =
            style_variable_cache_bytes(job->stylesheet);
        context->style_rule_queries_at_start =
            job->stylesheet->rule_index_queries;
        context->style_rule_candidates_at_start =
            job->stylesheet->rule_index_candidates;
        context->style_variable_lookups_at_start =
            job->stylesheet->variable_lookup_calls;
        context->style_variable_rule_candidates_at_start =
            job->stylesheet->variable_rule_candidates;
        context->style_variable_cache_hits_at_start =
            job->stylesheet->variable_cache_hits;
        context->style_variable_cache_misses_at_start =
            job->stylesheet->variable_cache_misses;
        context->style_variable_cache_negative_hits_at_start =
            job->stylesheet->variable_cache_negative_hits;
        context->style_variable_cache_evictions_at_start =
            job->stylesheet->variable_cache_evictions;
        context->style_deferred_rule_applications_at_start =
            job->stylesheet->deferred_rule_applications;
        context->style_deferred_rule_us_at_start =
            job->stylesheet->deferred_rule_us;
    }
    if (job->preview_y_limit == 0)
        layout_reuse_cache_expect_elements(job->reuse,
                                           job->document->element_count);
    layout_reuse_cache_prepare(job->reuse, job->stylesheet, job->fonts,
                               job->images, job->viewport_width);
    /* Binding a new sheet and resolving the root style (hundreds of
       custom properties on Wikipedia) are each tens of ms on the PSP. */
    if (!layout_cooperate_timed(context)) return false;
    if (job->stylesheet != NULL) {
        context->style_selector_cooperation_owned =
            style_selector_cooperation_begin(
                (Stylesheet *) job->stylesheet,
                layout_selector_cooperate, context,
                &context->ancestor_bloom_cache);
    }

    ComputedStyle root = layout_initial_root_style();
    lxb_dom_node_t *html = job->body->parent;
    bool have_html = html != NULL
                     && html->type == LXB_DOM_NODE_TYPE_ELEMENT
                     && document_node_is_tag(html, LXB_TAG_HTML);
    job->html_style = have_html
        ? layout_style_for_node(context, html, &root) : root;
    resolve_padding(context->sheet, &job->html_style, job->viewport_width);
    job->html_height = style_content_height(
        context->sheet, &job->html_style, job->viewport_width,
        job->viewport_height);
    if (!(job->html_style.has_height
          && (!job->html_style.height_percent || job->html_height > 0))) {
        /* The initial containing block is definite, but an auto-height html
           element does not pass that definiteness through to normal-flow
           percentage heights on body and its descendants. */
        job->html_height = 0;
    }
    if (job->html_style.has_background) {
        layout->page_background = blend_color_over(
            job->html_style.background, job->html_style.background_alpha,
            layout->page_background);
    }
    if (!layout_cooperate_timed(context)) return false;
    job->body_style = layout_style_for_node(
        context, job->body, &job->html_style);
    if (job->body_style.has_background) {
        layout->page_background = blend_color_over(
            job->body_style.background, job->body_style.background_alpha,
            layout->page_background);
    }
    return !context->cancelled;
}

static void layout_trace_root_bounds(LayoutDocument *layout,
    const LayoutNodeBox *body_box, const ComputedStyle *html_style,
    const ComputedStyle *body_style, int bottom, int html_height)
{
    if (LAYOUT_TRACE(layout, LAYOUT)) {
        fprintf(stderr,
                "layout-root bottom=%d html-height=%d "
                "html-overflow=%d/%d html-clip-only=%d/%d "
                "body-overflow=%d/%d body-clip-only=%d/%d "
                "body-box=%d,%d,%dx%d client=%dx%d content=%dx%d "
                "clips=%d/%d\n",
                bottom, html_height,
                html_style->overflow_x_scroll, html_style->overflow_y_scroll,
                html_style->overflow_x_clip_only,
                html_style->overflow_y_clip_only,
                body_style->overflow_x_scroll, body_style->overflow_y_scroll,
                body_style->overflow_x_clip_only,
                body_style->overflow_y_clip_only,
                body_box == NULL ? 0 : body_box->x,
                body_box == NULL ? 0 : body_box->y,
                body_box == NULL ? 0 : body_box->width,
                body_box == NULL ? 0 : body_box->height,
                body_box == NULL ? 0 : body_box->client_width,
                body_box == NULL ? 0 : body_box->client_height,
                body_box == NULL ? 0 : body_box->content_width,
                body_box == NULL ? 0 : body_box->content_height,
                body_box != NULL && body_box->clips_x,
                body_box != NULL && body_box->clips_y);
    }
}

static bool layout_job_flow(LayoutBuildJob *job)
{
    LayoutDocument *layout = &job->layout;
    LayoutContext *context = job->context;
    PositionedBox initial_positioned_box = {
        .node = NULL, .x = 0, .y = 0,
        .width = job->viewport_width,
        .height = job->html_height > 0 ? job->html_height : job->viewport_height,
        .fixed_node = NULL, .fixed_x = 0, .fixed_y = 0,
        .fixed_width = job->viewport_width,
        .fixed_height = job->html_height > 0 ? job->html_height : job->viewport_height
    };
    if (!layout_block(context, job->body, &job->html_style,
                      0, 0, job->viewport_width, job->html_height, false,
                      &initial_positioned_box, &job->bottom)
        || context->cancelled) return false;

    int document_bottom = job->bottom;
    const LayoutNodeBox *body_box = layout_box_for_node(layout, job->body);
    layout_trace_root_bounds(layout, body_box, &job->html_style,
        &job->body_style, job->bottom, job->html_height);
    if (body_box != NULL && !body_box->clips_y) {
        int overflow_bottom = layout_add_coordinate(
            layout_add_coordinate(body_box->y, body_box->content_height),
            job->body_style.margin.bottom);
        if (overflow_bottom > document_bottom) {
            document_bottom = overflow_bottom;
        }
    }
    if (body_box != NULL && !body_box->clips_x) {
        int overflow_right = layout_add_coordinate(
            layout_add_coordinate(body_box->x, body_box->content_width),
            job->body_style.margin.right);
        if (overflow_right > layout->scroll_width) {
            layout->scroll_width = overflow_right;
        }
    }
    layout->height = document_bottom < job->viewport_height
        ? job->viewport_height : layout_clamp_coordinate(document_bottom);
    return true;
}

static void layout_job_fingerprint_commands(LayoutBuildJob *job)
{
    LayoutDocument *layout = &job->layout;
    for (; job->fingerprint_at < layout->count; job->fingerprint_at++) {
        size_t i = job->fingerprint_at;
        const DrawCommand *command = &layout->commands[i];
        if (command->type != DRAW_TEXT || command->text == NULL
            || draw_command_is_text_shadow(command)) continue;
        uint64_t structure[] = {
            i, command->text_length,
            (uint32_t) command->radius
                & (LAYOUT_TEXT_FIND_SPACE_BEFORE
                   | LAYOUT_TEXT_FIND_BLOCK_START)
        };
        for (size_t field = 0;
             field < sizeof(structure) / sizeof(structure[0]); field++) {
            for (size_t byte = 0; byte < sizeof(structure[field]); byte++) {
                job->text_fingerprint ^= (unsigned char) (
                    structure[field] >> (byte * 8u));
                job->text_fingerprint *= UINT64_C(1099511628211);
            }
        }
        for (size_t byte = 0; byte < command->text_length; byte++) {
            job->text_fingerprint ^= (unsigned char) command->text[byte];
            job->text_fingerprint *= UINT64_C(1099511628211);
        }
    }
    layout->text_fingerprint = job->text_fingerprint;
}

static void layout_job_trace_finished(LayoutBuildJob *job)
{
    LayoutDocument *layout = &job->layout;
    LayoutContext *context = job->context;
    if (LAYOUT_TRACE(layout, PAINT)) {
        size_t limit = layout->count < 24 ? layout->count : 24;
        for (size_t i = 0; i < limit; i++) {
            const DrawCommand *command = &layout->commands[i];
            fprintf(stderr, "layout-paint index=%zu type=%d box=%d,%d,%dx%d "
                    "opacity=%u z=%d flags=%u\n", i, command->type,
                    command->x, command->y, command->width, command->height,
                    command->opacity_scale, command->z_index,
                    layout->command_flags == NULL ? 0
                    : layout->command_flags[i]);
        }
    }
    if (LAYOUT_TRACE(layout, LAYOUT_PROFILE)) {
        fprintf(stderr,
                "layout-profile styles=%llu style-cache-hits=%llu "
                "style-cache-misses=%llu intrinsic-width-visits=%llu "
                "intrinsic-min-visits=%llu intrinsic-cache-hits=%llu "
                "intrinsic-cache-misses=%llu flex-basis=%llu flex-minimum=%llu "
                "tree-depth=%zu/%u depth-fallbacks=%zu fallback-visits=%zu "
                "block-scratch-pages=%zu block-scratch-bytes=%zu "
                "block-scratch-allocation-failures=%zu "
                "bidi-paragraphs=%llu bidi-fallbacks=%llu "
                "bidi-limit-degradations=%llu bidi-us=%llu "
                "bidi-peak-transient=%zu\n",
                (unsigned long long) context->style_resolutions,
                (unsigned long long) context->style_cache_hits,
                (unsigned long long) context->style_cache_misses,
                (unsigned long long) context->intrinsic_width_visits,
                (unsigned long long) context->intrinsic_min_visits,
                (unsigned long long) context->intrinsic_cache_hits,
                (unsigned long long) context->intrinsic_cache_misses,
                (unsigned long long) context->flex_basis_resolutions,
                (unsigned long long) context->flex_minimum_resolutions,
                context->max_tree_call_depth,
                (unsigned) LAYOUT_TREE_CALL_DEPTH_LIMIT,
                context->depth_fallback_count,
                context->fallback_visits,
                context->block_scratch_page_count,
                context->block_scratch_page_count
                    * LAYOUT_BLOCK_SCRATCH_PAGE_DEPTH
                    * sizeof(LayoutBlockScratch),
                context->block_scratch_allocation_failures,
                (unsigned long long) layout->performance.bidi_paragraphs,
                (unsigned long long) layout->performance.bidi_line_fallbacks,
                (unsigned long long)
                    layout->performance.bidi_limit_degradations,
                (unsigned long long) layout->performance.bidi_us,
                layout->performance.bidi_peak_transient_bytes);
    }
}

static void layout_job_capture_performance(LayoutBuildJob *job)
{
    LayoutDocument *layout = &job->layout;
    LayoutContext *context = job->context;
    const Stylesheet *stylesheet = job->stylesheet;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    char checkpoint_message[256];
    static bool clock_calibrated;
    if (!clock_calibrated) {
        clock_calibrated = true;
        uint64_t start = layout_performance_now_us();
        for (unsigned i = 0; i < 1024; i++)
            (void) tilefinch_platform_monotonic_time_ns();
        snprintf(checkpoint_message, sizeof(checkpoint_message),
            "tilefinch-layout-clock: calls=1024 elapsed=%lluus",
            (unsigned long long) (layout_performance_now_us() - start));
        tilefinch_platform_log_message(checkpoint_message);
    }
    snprintf(checkpoint_message, sizeof(checkpoint_message),
        "tilefinch-layout-checkpoints: work=%zu yields=%zu clock-checks=%zu "
        "time-yields=%zu profiled=%u cooperate=%lluus max-slice=%lluus",
        layout->layout_work_units, layout->cooperative_yields,
        context->safe_point_clock_checks, context->safe_point_time_yields,
        layout->performance.flow_phase_transitions != 0 ? 1u : 0u,
        (unsigned long long) layout->performance.flow_phase_us[LAYOUT_FLOW_COOPERATE],
        (unsigned long long) layout->max_work_slice_us);
    tilefinch_platform_log_message(checkpoint_message);
    printf("tilefinch-layout-cost: total=%lluus flow=%lluus "
        "styles=%llu/%llu counters=%zu commands=%zu\n",
        (unsigned long long) job->build_active_us,
        (unsigned long long) layout->performance.flow_us,
        (unsigned long long) context->style_cache_hits,
        (unsigned long long) context->style_cache_misses,
        context->counter_entry_count, layout->count);
#endif
    TILEFINCH_WORK_ADD(layout_passes, 1);
    TILEFINCH_WORK_ADD(layout_commands, layout->count);
    layout->performance.style_resolutions = context->style_resolutions;
    layout->performance.style_cache_hits = context->style_cache_hits;
    layout->performance.style_cache_misses = context->style_cache_misses;
    layout->performance.style_resolve_us = context->style_resolve_us;
    if (stylesheet != NULL) {
        layout->performance.style_rule_queries =
            stylesheet->rule_index_queries
            - context->style_rule_queries_at_start;
        layout->performance.style_rule_candidates =
            stylesheet->rule_index_candidates
            - context->style_rule_candidates_at_start;
        layout->performance.style_variable_lookups =
            stylesheet->variable_lookup_calls
            - context->style_variable_lookups_at_start;
        layout->performance.style_variable_rule_candidates =
            stylesheet->variable_rule_candidates
            - context->style_variable_rule_candidates_at_start;
        layout->performance.style_variable_cache_hits =
            stylesheet->variable_cache_hits
            - context->style_variable_cache_hits_at_start;
        layout->performance.style_variable_cache_misses =
            stylesheet->variable_cache_misses
            - context->style_variable_cache_misses_at_start;
        layout->performance.style_variable_cache_negative_hits =
            stylesheet->variable_cache_negative_hits
            - context->style_variable_cache_negative_hits_at_start;
        layout->performance.style_variable_cache_evictions =
            stylesheet->variable_cache_evictions
            - context->style_variable_cache_evictions_at_start;
        layout->performance.style_variable_cache_bytes =
            context->style_variable_cache_bytes;
        layout->performance.style_deferred_rule_applications =
            stylesheet->deferred_rule_applications
            - context->style_deferred_rule_applications_at_start;
        layout->performance.style_deferred_rule_us =
            stylesheet->deferred_rule_us
            - context->style_deferred_rule_us_at_start;
    }
    layout->performance.intrinsic_width_visits =
        context->intrinsic_width_visits;
    layout->performance.intrinsic_min_visits =
        context->intrinsic_min_visits;
    layout->performance.intrinsic_cache_hits =
        context->intrinsic_cache_hits;
    layout->performance.intrinsic_cache_misses =
        context->intrinsic_cache_misses;
    layout->performance.intrinsic_paired_text_measurements =
        context->intrinsic_paired_text_measurements;
    layout->performance.margin_collapse_visits =
        context->margin_collapse_visits;
    layout->performance.margin_cache_hits = context->margin_cache_hits;
    layout->performance.margin_cache_misses = context->margin_cache_misses;
    layout->performance.flat_iterator_passes = context->flat_iterator_passes;
    layout->performance.flat_iterator_yields = context->flat_iterator_yields;
    layout->performance.flex_iterator_passes = context->flex_iterator_passes;
    layout->performance.flex_iterator_yields = context->flex_iterator_yields;
    layout->performance.flex_basis_requests = context->flex_basis_resolutions;
    layout->performance.flex_minimum_requests =
        context->flex_minimum_resolutions;
    layout->performance.resumable_phases = job->resumable_phases;
    layout->performance.resumable_passes = job->resumable_passes;
    layout->performance.maximum_resumable_phase_us =
        job->maximum_resumable_phase_us;
    layout->performance.total_us = job->build_active_us;
}

static void layout_job_record_phase(LayoutBuildJob *job,
                                    uint64_t started_us)
{
    uint64_t finished_us = layout_performance_now_us();
    uint64_t elapsed_us = finished_us >= started_us
        ? finished_us - started_us : 0;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    if (elapsed_us >= UINT64_C(100000)) {
        char message[256];
        snprintf(message, sizeof(message),
            "tilefinch-layout-phase: next=%d elapsed=%lluus "
            "commands=%zu counters=%zu work=%zu yields=%zu styles=%llu bidi=%lluus",
            (int) job->phase,
            (unsigned long long) elapsed_us, job->layout.count,
            job->context == NULL ? 0 : job->context->counter_entry_count,
            job->layout.layout_work_units, job->layout.cooperative_yields,
            (unsigned long long) (job->context == NULL ? 0 : job->context->style_cache_misses),
            (unsigned long long) job->layout.performance.bidi_us);
        tilefinch_platform_log_message(message);
        if (job->layout.performance.flow_phase_transitions != 0) {
            const uint64_t *flow = job->layout.performance.flow_phase_us;
            snprintf(message, sizeof(message),
                "tilefinch-layout-phase-flow: other=%lluus style=%lluus pseudo=%lluus "
                "intrinsic=%lluus margin=%lluus inline=%lluus cooperate=%lluus",
                (unsigned long long) flow[LAYOUT_FLOW_OTHER],
                (unsigned long long) flow[LAYOUT_FLOW_STYLE],
                (unsigned long long) flow[LAYOUT_FLOW_PSEUDO],
                (unsigned long long) flow[LAYOUT_FLOW_INTRINSIC],
                (unsigned long long) flow[LAYOUT_FLOW_MARGIN],
                (unsigned long long) flow[LAYOUT_FLOW_INLINE],
                (unsigned long long) flow[LAYOUT_FLOW_COOPERATE]);
            tilefinch_platform_log_message(message);
        }
    }
#endif
    job->build_active_us += elapsed_us;
    job->resumable_phases++;
    if (elapsed_us > job->maximum_resumable_phase_us) {
        job->maximum_resumable_phase_us = elapsed_us;
    }
}

LayoutBuildJob *layout_build_job_begin(
    Budget *budget, const PocDocument *document,
    const Stylesheet *stylesheet, const FontSet *fonts,
    const ImageResources *images, const ViewportContext *viewport,
    LayoutReuseCache *reuse)
{
    if (budget == NULL || document == NULL || viewport == NULL
        || viewport->css_width <= 0 || viewport->css_height <= 0
        || viewport->device_width <= 0 || viewport->device_height <= 0) {
        return NULL;
    }
    LayoutBuildJob *job = budget_calloc_category(
        budget, BUDGET_CATEGORY_LAYOUT, 1, sizeof(*job));
    if (job == NULL) return NULL;
    job->budget = budget;
    job->document = document;
    job->stylesheet = stylesheet;
    job->fonts = fonts;
    job->images = images;
    job->viewport = *viewport;
    job->viewport_width = viewport->css_width;
    job->viewport_height = viewport->css_height;
    job->reuse = reuse;
    job->text_fingerprint = UINT64_C(1469598103934665603);
    job->phase = LAYOUT_JOB_INIT;
    job->status = LAYOUT_BUILD_PENDING;
    bool container_queries = stylesheet_has_container_queries(stylesheet)
        || layout_document_has_inline_container_units(document, stylesheet);
    job->probe_pass = container_queries;
    job->retain_container_padding = job->probe_pass;
    if (container_queries) {
        job->container_previous_signature =
            style_container_layout_state_signature(stylesheet);
        job->container_log_eligible =
            stylesheet_has_container_queries(stylesheet)
            && !stylesheet->has_container_relative_units
            && !layout_document_has_inline_container_units(
                   document, stylesheet);
#ifndef TILEFINCH_NO_TRACE
        /* Lab A/B: the measured probe and rebuild without the log. */
        if (getenv("TILEFINCH_DISABLE_CONTAINER_QUERY_LOG") != NULL)
            job->container_log_eligible = false;
#endif
    }
    return job;
}

LayoutBuildStatus layout_build_job_pump(LayoutBuildJob *job)
{
    if (job == NULL) return LAYOUT_BUILD_FAILED;
    if (job->status != LAYOUT_BUILD_PENDING) return job->status;
    uint64_t started_us = layout_performance_now_us();
    LayoutJobPhase active_phase = job->phase;
    bool okay = true;
    static const char *const phase_steps[] = {
        [LAYOUT_JOB_INIT] = "layout-job-init",
        [LAYOUT_JOB_FLOW] = "layout-job-flow",
        [LAYOUT_JOB_COMPACT] = "layout-job-compact",
        [LAYOUT_JOB_VISIBILITY] = "layout-job-visibility",
        [LAYOUT_JOB_PAINT_ORDER] = "layout-job-paint-order",
        [LAYOUT_JOB_SPATIAL_INDEX] = "layout-job-spatial",
        [LAYOUT_JOB_SCROLL_METADATA] = "layout-job-scroll",
        [LAYOUT_JOB_FINISH] = "layout-job-finish",
        [LAYOUT_JOB_CONTAINER_STATE] = "layout-job-container",
        [LAYOUT_JOB_DONE] = "layout-job-done"
    };
    if ((size_t) job->phase < sizeof(phase_steps) / sizeof(phase_steps[0])
        && phase_steps[job->phase] != NULL)
        tilefinch_platform_trace_step(phase_steps[job->phase]);
    switch (job->phase) {
    case LAYOUT_JOB_INIT:
        okay = layout_job_init(job);
        if (okay) {
            job->resumable_passes++;
            job->layout.performance.root_style_us =
                layout_performance_now_us() - started_us;
            job->phase = LAYOUT_JOB_FLOW;
        }
        break;
    case LAYOUT_JOB_FLOW:
        layout_flow_profile_begin(job->context);
        okay = layout_job_flow(job);
        layout_flow_profile_end(job->context);
        if (okay) {
            job->layout.performance.flow_us =
                layout_performance_now_us() - started_us;
            layout_flow_profile_report(job->context);
            job->phase = LAYOUT_JOB_COMPACT;
        }
        break;
    case LAYOUT_JOB_COMPACT:
        compact_layout_storage(&job->layout);
        layout_job_fingerprint_commands(job);
        job->layout.performance.compact_us =
            layout_performance_now_us() - started_us;
        job->phase = LAYOUT_JOB_VISIBILITY;
        break;
    case LAYOUT_JOB_VISIBILITY:
        okay = layout_resolve_visibility(job->context);
        if (okay) layout_build_focus_index(&job->layout);
        if (okay) layout_index_focus_pseudo_texts(
            &job->layout, job->context->focus_pseudo_visibility_slots != 0);
        if (okay) {
            job->layout.performance.focus_index_us =
                layout_performance_now_us() - started_us;
            job->phase = LAYOUT_JOB_PAINT_ORDER;
        }
        break;
    case LAYOUT_JOB_PAINT_ORDER:
        okay = build_paint_order(&job->layout, job->context);
        if (okay) {
            job->layout.performance.paint_order_us =
                layout_performance_now_us() - started_us;
            job->phase = LAYOUT_JOB_SPATIAL_INDEX;
        }
        break;
    case LAYOUT_JOB_SPATIAL_INDEX:
        okay = build_spatial_index(&job->layout, job->context);
        if (okay) {
            job->layout.performance.spatial_index_us =
                layout_performance_now_us() - started_us;
            job->phase = LAYOUT_JOB_SCROLL_METADATA;
        }
        break;
    case LAYOUT_JOB_SCROLL_METADATA:
        okay = layout_build_scroll_metadata(
            &job->layout, job->stylesheet);
        /* Its only refusal is a declined cooperate checkpoint. */
        if (!okay && job->context != NULL)
            layout_context_cancel(job->context);
        if (okay) job->phase = LAYOUT_JOB_FINISH;
        break;
    case LAYOUT_JOB_FINISH:
        layout_finish_work_slice(job->context);
        job->layout.scroll_width = root_scroll_width_after_clipping(
            &job->layout, job->viewport_width, job->context);
        if (job->context->cancelled) { okay = false; break; }
        job->layout.height = root_scroll_height_after_clipping(
            &job->layout, job->bottom < job->viewport_height
                ? job->viewport_height : job->bottom, job->context);
        if (job->context->cancelled) { okay = false; break; }
        job->preview_truncated = job->context->preview_truncated;
        tilefinch_platform_trace_step("finish-trace");
        layout_job_trace_finished(job);
        job->layout.performance.finalize_us =
            layout_performance_now_us() - started_us;
        layout_job_record_phase(job, started_us);
        tilefinch_platform_trace_step("finish-capture");
        layout_job_capture_performance(job);
        tilefinch_platform_trace_step("finish-release");
        layout_release_context(job->context, job->budget);
        tilefinch_platform_trace_step("finish-released");
        job->context = NULL;
        if (job->probe_pass) {
            job->phase = LAYOUT_JOB_CONTAINER_STATE;
            return job->status;
        }
#ifndef TILEFINCH_NO_TRACE
        layout_job_check_container_log(job);
#endif
        job->phase = LAYOUT_JOB_DONE;
        job->status = LAYOUT_BUILD_COMPLETE;
        return job->status;
    case LAYOUT_JOB_CONTAINER_STATE: {
        okay = layout_collect_container_state(
            (Stylesheet *) job->stylesheet, job->budget, &job->layout);
        if (!okay) break;
        ((Stylesheet *) job->stylesheet)->container_state_complete =
            job->preview_y_limit <= 0 && !job->preview_truncated;
        uint64_t measured_signature =
            style_container_layout_state_signature(job->stylesheet);
        /* Every query this pass evaluated answers the same against the
           geometry it measured: a rebuild would repeat it exactly. */
        bool verified = job->container_log
            && job->document->content_generation
                   == job->container_log_generation
            && style_container_log_verified(job->stylesheet);
        layout_job_end_container_log(job);
        job->probe_pass = false;
        job->retain_container_padding = false;
#ifndef TILEFINCH_NO_TRACE
        /* Lab self-check: rebuild anyway and compare (see below). */
        if (verified
            && getenv("TILEFINCH_VERIFY_CONTAINER_QUERY_LOG") != NULL) {
            layout_destroy(&job->container_check);
            job->container_check = job->layout;
            memset(&job->layout, 0, sizeof(job->layout));
            job->container_check_pending = true;
            verified = false;
            measured_signature = job->container_previous_signature + 1u;
        }
#endif
        if (measured_signature == job->container_previous_signature
            || verified) {
            layout_job_record_phase(job, started_us);
            job->layout.performance.total_us = job->build_active_us;
            job->layout.performance.resumable_phases =
                job->resumable_phases;
            job->layout.performance.resumable_passes =
                job->resumable_passes;
            job->layout.performance.maximum_resumable_phase_us =
                job->maximum_resumable_phase_us;
            job->phase = LAYOUT_JOB_DONE;
            job->status = LAYOUT_BUILD_COMPLETE;
            return job->status;
        }
        layout_destroy(&job->layout);
        /* The DOM and installed cascade are unchanged between the probe
           and final pass. Keep exact styles which never consulted container
           state; refresh dependent styles, pseudo discovery and all sizing. */
        layout_reuse_container_state_changed(job->reuse);
        job->body = NULL;
        job->fingerprint_at = 0;
        job->text_fingerprint = UINT64_C(1469598103934665603);
        job->phase = LAYOUT_JOB_INIT;
        break;
    }
    case LAYOUT_JOB_DONE:
        job->status = LAYOUT_BUILD_COMPLETE;
        return job->status;
    case LAYOUT_JOB_ERROR:
    default:
        return job->status;
    }
    /* A synchronous build pumps its phases back to back; each ending phase
       (compaction, visibility, ...) is a safe point for the clock check. */
    if (okay && job->context != NULL
        && !layout_cooperate_timed(job->context)) okay = false;
    layout_job_record_phase(job, started_us);
    if (!okay || (job->context != NULL && job->context->cancelled)) {
        LayoutBuildStatus failure =
            job->context != NULL && job->context->cancelled
                ? LAYOUT_BUILD_CANCELLED : LAYOUT_BUILD_FAILED;
        layout_job_fail(job, failure);
        job->failed_phase = (unsigned) active_phase + 1u;
        return job->status;
    }
    return job->status;
}

bool layout_build_job_take(LayoutBuildJob *job, LayoutDocument *layout)
{
    if (job == NULL || layout == NULL
        || job->status != LAYOUT_BUILD_COMPLETE || job->taken) return false;
    *layout = job->layout;
    memset(&job->layout, 0, sizeof(job->layout));
    job->taken = true;
    return true;
}

unsigned layout_build_job_failure_phase(const LayoutBuildJob *job)
{
    return job == NULL ? 0 : job->failed_phase;
}

void layout_build_job_cancel(LayoutBuildJob *job)
{
    if (job == NULL || job->status != LAYOUT_BUILD_PENDING) return;
    if (job->context != NULL) layout_context_cancel(job->context);
    layout_job_fail(job, LAYOUT_BUILD_CANCELLED);
}

void layout_build_job_destroy(LayoutBuildJob *job)
{
    if (job == NULL) return;
    layout_job_end_container_log(job);
#ifndef TILEFINCH_NO_TRACE
    layout_destroy(&job->container_check);
#endif
    if (job->context != NULL) {
        layout_release_context(job->context, job->budget);
        job->context = NULL;
    }
    if (!job->taken) layout_destroy(&job->layout);
    Budget *budget = job->budget;
    memset(job, 0, sizeof(*job));
    budget_free(budget, job);
}

static bool layout_build_context_resumable_sync(
    LayoutDocument *layout, Budget *budget, const PocDocument *document,
    const Stylesheet *stylesheet, const FontSet *fonts,
    const ImageResources *images, const ViewportContext *viewport,
    LayoutReuseCache *reuse, int preview_y_limit, bool *preview_truncated)
{
    if (layout == NULL) return false;
    LayoutBuildJob *job = layout_build_job_begin(
        budget, document, stylesheet, fonts, images, viewport, reuse);
    if (job == NULL) {
        layout->performance.failed_phase = LAYOUT_BUILD_FAILED_AT_STARTUP;
        return false;
    }
    job->preview_y_limit = preview_y_limit;
    LayoutBuildStatus status = LAYOUT_BUILD_PENDING;
    while (status == LAYOUT_BUILD_PENDING) {
        status = layout_build_job_pump(job);
    }
    bool okay = status == LAYOUT_BUILD_COMPLETE
                && layout_build_job_take(job, layout);
    /* The job's own document is gone by now; keep the one bit an owner
       needs to decide between retrying and giving up. */
    if (!okay) {
        layout->performance.cancelled = status == LAYOUT_BUILD_CANCELLED;
        layout->performance.failed_phase = job->failed_phase;
    }
    if (okay && preview_truncated != NULL)
        *preview_truncated = job->preview_truncated;
    layout_build_job_destroy(job);
    return okay;
}

void layout_note_contained_height(LayoutDocument *layout,
                                  const lxb_dom_node_t *node, int height)
{
    if (layout == NULL || !layout->retain_container_padding
        || layout->node_container_contained_heights == NULL) return;
    size_t index = layout_node_box_index(layout, node);
    if (index < layout->node_container_padding_capacity)
        layout->node_container_contained_heights[index] =
            height < 0 ? -1 : height;
}

static bool layout_collect_container_state(Stylesheet *stylesheet,
                                           Budget *budget,
                                           const LayoutDocument *layout)
{
    if (stylesheet == NULL || budget == NULL || layout == NULL
        || !style_container_layout_state_begin(
               stylesheet, budget, layout->node_box_count)) return false;
    for (size_t i = 0; i < layout->node_box_count; i++) {
        if ((i & 31u) == 0
            && !tilefinch_platform_cooperate(
                "layout-container-state", i)) {
            style_container_layout_state_clear(stylesheet);
            return false;
        }
        const LayoutNodeBox *box = &layout->node_boxes[i];
        bool sidecar = layout->node_container_padding_sums != NULL
            && i < layout->node_container_padding_capacity;
        uint32_t padding = sidecar ? layout->node_container_padding_sums[i]
                                   : 0;
        int contained = sidecar ? layout->node_container_contained_heights[i]
                                : -1;
        if (!style_container_layout_state_add_contained(
                stylesheet, box->node, box->client_width,
                box->client_height, (int) (padding & UINT16_MAX),
                (int) (padding >> 16), contained)) {
            style_container_layout_state_clear(stylesheet);
            return false;
        }
    }
    return true;
}

static bool layout_span_contains(const char *text, size_t length,
                                 const char *needle)
{
    size_t needle_length = strlen(needle);
    if (text == NULL || needle_length == 0 || needle_length > length) {
        return false;
    }
    for (size_t i = 0; i + needle_length <= length; i++) {
        size_t j = 0;
        while (j < needle_length
               && tolower((unsigned char) text[i + j])
                  == tolower((unsigned char) needle[j])) j++;
        if (j == needle_length) return true;
    }
    return false;
}

static bool layout_document_has_inline_container_units(
    const PocDocument *document, const Stylesheet *stylesheet)
{
    if (document == NULL) return false;
    if (stylesheet != NULL && stylesheet->block_inline_style_attributes) {
        return false;
    }
    if (document->inline_container_units_generation
        == document->content_generation) {
        return document->inline_container_units_present;
    }
    PocDocument *mutable_document = (PocDocument *) document;
    lxb_dom_node_t *node = document->html == NULL
        ? NULL : lxb_dom_interface_node(document->html);
    size_t visited = 0;
    while (node != NULL && visited++ < 16384) {
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            size_t length = 0;
            const char *style = stylesheet != NULL
                    && stylesheet->block_inline_style_attributes
                    && !document_style_attribute_cssom_authorized(node)
                ? NULL : document_attribute(node, "style", &length);
            if (style != NULL
                && (layout_span_contains(style, length, "cqw")
                    || layout_span_contains(style, length, "cqh")
                    || layout_span_contains(style, length, "cqi")
                    || layout_span_contains(style, length, "cqb")
                    || layout_span_contains(style, length, "cqmin")
                    || layout_span_contains(style, length, "cqmax"))) {
                mutable_document->inline_container_units_present = true;
                mutable_document->inline_container_units_generation =
                    document->content_generation;
                return true;
            }
        }
        if (node->first_child != NULL) {
            node = node->first_child;
            continue;
        }
        while (node != NULL && node->next == NULL) node = node->parent;
        if (node != NULL) node = node->next;
    }
    mutable_document->inline_container_units_present = false;
    mutable_document->inline_container_units_generation =
        document->content_generation;
    return false;
}

bool layout_build_context_reuse(LayoutDocument *layout, Budget *budget,
                                const PocDocument *document,
                                const Stylesheet *stylesheet,
                                const FontSet *fonts,
                                const ImageResources *images,
                                const ViewportContext *viewport,
                                LayoutReuseCache *reuse)
{
    return layout_build_context_resumable_sync(
        layout, budget, document, stylesheet, fonts, images, viewport, reuse,
        0, NULL);
}

bool layout_build_context_preview(LayoutDocument *layout, Budget *budget,
                                  const PocDocument *document,
                                  const Stylesheet *stylesheet,
                                  const FontSet *fonts,
                                  const ImageResources *images,
                                  const ViewportContext *viewport,
                                  LayoutReuseCache *reuse, int y_limit,
                                  bool *truncated)
{
    if (truncated == NULL || y_limit <= 0) return false;
    *truncated = false;
    return layout_build_context_resumable_sync(
        layout, budget, document, stylesheet, fonts, images, viewport, reuse,
        y_limit, truncated);
}

bool layout_build_context(LayoutDocument *layout, Budget *budget,
                          const PocDocument *document,
                          const Stylesheet *stylesheet,
                          const FontSet *fonts,
                          const ImageResources *images,
                          const ViewportContext *viewport)
{
    return layout_build_context_reuse(
        layout, budget, document, stylesheet, fonts, images, viewport, NULL);
}

bool layout_build_viewport(LayoutDocument *layout, Budget *budget,
                           const PocDocument *document,
                           const Stylesheet *stylesheet,
                           const FontSet *fonts,
                           const ImageResources *images,
                           int viewport_width, int viewport_height,
                           int device_width, int device_height)
{
    ViewportContext viewport;
    if (!viewport_context_init(&viewport, viewport_width, viewport_height,
                               device_width, device_height)) return false;
    return layout_build_context(layout, budget, document, stylesheet, fonts,
                                images, &viewport);
}

bool layout_build(LayoutDocument *layout, Budget *budget,
                  const PocDocument *document, const Stylesheet *stylesheet,
                  const FontSet *fonts, const ImageResources *images,
                  int viewport_width)
{
    return layout_build_viewport(layout, budget, document, stylesheet, fonts,
                                 images, viewport_width, 272,
                                 viewport_width, 272);
}

int layout_css_to_visual(const LayoutDocument *layout, int value)
{
    return layout == NULL ? value
                          : viewport_css_to_device(&layout->viewport, value);
}

int layout_visual_to_css(const LayoutDocument *layout, int value)
{
    return layout == NULL ? value
                          : viewport_device_to_css(&layout->viewport, value);
}

static void *clone_array(Budget *budget, const void *source, size_t count,
                         size_t item_size)
{
    if (count == 0) return NULL;
    if (count > SIZE_MAX / item_size) return NULL;
    void *copy = budget_malloc(budget, count * item_size);
    if (copy != NULL) memcpy(copy, source, count * item_size);
    return copy;
}

void layout_note_unresolved_external_visual(
    LayoutContext *context, lxb_dom_node_t *node, const char *source,
    uint8_t kind, PseudoElement pseudo)
{
    if (context == NULL || context->layout == NULL || node == NULL) return;
    LayoutDocument *layout = context->layout;
    layout->unresolved_external_visuals = true;
    if (context->preview_y_limit <= 0
        && !context->record_unresolved_visuals) return;
    for (size_t i = 0; i < layout->visual_priority_count; i++) {
        const ImagePriorityTarget *target =
            &layout->visual_priority_targets[i];
        if (target->node == node && target->source == source
            && target->kind == kind
            && target->pseudo == (uint8_t) pseudo) return;
    }
    if (layout->visual_priority_count == LAYOUT_VISUAL_PRIORITY_LIMIT) {
        layout->visual_priority_overflow = true;
        return;
    }
    if (layout->visual_priority_count == layout->visual_priority_capacity) {
        size_t capacity = layout->visual_priority_capacity == 0
            ? 8u : layout->visual_priority_capacity * 2u;
        if (capacity > LAYOUT_VISUAL_PRIORITY_LIMIT) {
            capacity = LAYOUT_VISUAL_PRIORITY_LIMIT;
        }
        ImagePriorityTarget *targets = budget_realloc(
            layout->budget, layout->visual_priority_targets,
            capacity * sizeof(*targets));
        if (targets == NULL) {
            layout->visual_priority_overflow = true;
            return;
        }
        layout->visual_priority_targets = targets;
        layout->visual_priority_capacity = capacity;
    }
    layout->visual_priority_targets[layout->visual_priority_count++] =
        (ImagePriorityTarget) {
            .node = node,
            .source = source,
            .kind = kind,
            .pseudo = (uint8_t) pseudo
        };
}

bool layout_clone_visual(LayoutDocument *visual,
                         const LayoutDocument *source)
{
    if (visual == NULL || source == NULL || source->budget == NULL
        || source->viewport.scale_numerator <= 0
        || source->viewport.scale_denominator <= 0) {
        return false;
    }
    memset(visual, 0, sizeof(*visual));
    visual->budget = source->budget;
    visual->fonts = source->fonts;
    visual->web_fonts = source->web_fonts;
    visual->page_background = source->page_background;
    visual->unresolved_external_visuals =
        source->unresolved_external_visuals;
    /* The gradient table is referenced by fill commands, so it must travel
       with the visual clone that the rasterizer consumes. */
    memcpy(visual->gradients, source->gradients, sizeof(visual->gradients));
    visual->gradient_count = source->gradient_count;
    visual->count = visual->capacity = source->count;
    visual->link_count = visual->link_capacity = source->link_count;
    visual->control_count = visual->control_capacity = source->control_count;
    visual->sticky_count = visual->sticky_capacity = source->sticky_count;
    visual->fixed_count = visual->fixed_capacity = source->fixed_count;
    visual->node_box_count = visual->node_box_capacity = source->node_box_count;
    visual->paint_order_count = source->paint_order_count;
    visual->bidi_glyph_count = visual->bidi_glyph_capacity =
        source->bidi_glyph_count;
    visual->bidi_command_count = visual->bidi_command_capacity =
        source->bidi_command_count;
    visual->commands = clone_array(source->budget, source->commands,
                                   source->count, sizeof(*source->commands));
    visual->bidi_glyphs = clone_array(
        source->budget, source->bidi_glyphs, source->bidi_glyph_count,
        sizeof(*source->bidi_glyphs));
    visual->bidi_commands = clone_array(
        source->budget, source->bidi_commands, source->bidi_command_count,
        sizeof(*source->bidi_commands));
    visual->links = clone_array(source->budget, source->links,
                                source->link_count, sizeof(*source->links));
    visual->controls = clone_array(source->budget, source->controls,
                                   source->control_count,
                                   sizeof(*source->controls));
    visual->sticky_ranges = clone_array(
        source->budget, source->sticky_ranges, source->sticky_count,
        sizeof(*source->sticky_ranges));
    visual->fixed_ranges = clone_array(
        source->budget, source->fixed_ranges, source->fixed_count,
        sizeof(*source->fixed_ranges));
    visual->node_boxes = clone_array(
        source->budget, source->node_boxes, source->node_box_count,
        sizeof(*source->node_boxes));
    if (source->node_clip_radius_codes != NULL) {
        visual->node_clip_radius_codes = clone_array(
            source->budget, source->node_clip_radius_codes,
            source->node_box_count, sizeof(*source->node_clip_radius_codes));
    }
    /* Viewport scaling changes geometry, never CSS stacking order.  Preserve
       the authoritative order built while transient stacking-context
       metadata was available instead of trying to reconstruct it from the
       flattened display list. */
    visual->paint_order = clone_array(
        source->budget, source->paint_order, source->paint_order_count,
        sizeof(*source->paint_order));
    if ((source->count != 0 && visual->commands == NULL)
        || (source->bidi_glyph_count != 0 && visual->bidi_glyphs == NULL)
        || (source->bidi_command_count != 0 && visual->bidi_commands == NULL)
        || (source->link_count != 0 && visual->links == NULL)
        || (source->control_count != 0 && visual->controls == NULL)
        || (source->sticky_count != 0 && visual->sticky_ranges == NULL)
        || (source->fixed_count != 0 && visual->fixed_ranges == NULL)
        || (source->node_box_count != 0 && visual->node_boxes == NULL)
        || (source->node_clip_radius_codes != NULL
            && visual->node_clip_radius_codes == NULL)
        || (source->paint_order_count != 0
            && visual->paint_order == NULL)) {
        layout_destroy(visual);
        return false;
    }
    int numerator = source->viewport.scale_numerator;
    int denominator = source->viewport.scale_denominator;
    visual->width = viewport_scale_ceil(source->width, numerator, denominator);
    visual->scroll_width = viewport_scale_ceil(source->scroll_width, numerator,
                                                denominator);
    visual->height = viewport_scale_ceil(source->height, numerator, denominator);
    /* The placeholder below a provisional layout's content starts at the
       same place on screen. */
    if (source->content_limit_y > 0) {
        visual->content_limit_y = viewport_scale_ceil(
            source->content_limit_y, numerator, denominator);
        if (visual->content_limit_y <= 0) visual->content_limit_y = 1;
    }
    if (!viewport_context_init(&visual->viewport,
                               source->viewport.device_width,
                               source->viewport.device_height,
                               source->viewport.device_width,
                               source->viewport.device_height)) {
        layout_destroy(visual);
        return false;
    }
    visual->viewport.declared = source->viewport.declared;
    visual->viewport.device_width_declared =
        source->viewport.device_width_declared;
    for (size_t i = 0; i < visual->bidi_glyph_count; i++) {
        LayoutBidiGlyph *glyph = &visual->bidi_glyphs[i];
        glyph->x_fixed = layout_fixed_scale_floor(
            glyph->x_fixed, numerator, denominator);
        uint64_t advance = (uint64_t) glyph->advance_fixed
                           * (unsigned) numerator;
        advance = (advance + (unsigned) denominator - 1u)
                  / (unsigned) denominator;
        glyph->advance_fixed = (uint16_t) (
            advance > UINT16_MAX ? UINT16_MAX : advance);
    }
    for (size_t i = 0; i < visual->count; i++) {
        DrawCommand *command = &visual->commands[i];
        int text_x_fixed = command->type == DRAW_TEXT
            ? draw_command_text_x_fixed(command) : 0;
        int text_font_size_fixed = command->type == DRAW_TEXT
            ? draw_command_text_font_size_fixed(command) : 0;
        viewport_css_box_to_device(&source->viewport, &command->x,
                                   &command->width);
        viewport_css_box_to_device(&source->viewport, &command->y,
                                   &command->height);
        if (command->type == DRAW_TEXT) {
            draw_command_set_text_letter_spacing_fixed(command,
                viewport_scale_floor(draw_command_text_letter_spacing_fixed(command),
                    numerator, denominator));
        }
        int underline_offset = 0;
        if (command->type == DRAW_TEXT
            && draw_command_text_underline_offset(
                   command, &underline_offset)) {
            draw_command_set_text_underline_offset(
                command, viewport_scale_floor(
                    underline_offset, numerator, denominator));
        }
        if (command->type == DRAW_TEXT) {
            text_x_fixed = layout_fixed_scale_floor(
                text_x_fixed, numerator, denominator);
            draw_command_set_text_x_fixed(command, text_x_fixed);
            if (text_font_size_fixed > 0) {
                int64_t scaled = (int64_t) text_font_size_fixed * numerator;
                scaled = (scaled + denominator - 1) / denominator;
                if (scaled < 1) scaled = 1;
                if (scaled > INT_MAX) scaled = INT_MAX;
                draw_command_set_text_font_size_fixed(command, (int) scaled);
            }
            if (draw_command_is_text_shadow(command)) {
                draw_command_set_text_shadow_blur(
                    command, viewport_scale_ceil(
                        draw_command_text_shadow_blur(command),
                        numerator, denominator));
            }
        } else if (command->type == DRAW_IMAGE
                   && command->image_fit >= LAYOUT_IMAGE_FIT_SPRITE
                   && command->image_fit <= LAYOUT_IMAGE_FIT_SPRITE_TILE_XY) {
            /* A sprite DRAW_IMAGE overloads radius with the background's
               source-pixel (CSS) offset, which the rasterizer subtracts
               after converting device deltas back to CSS.  Scaling it here
               would apply the viewport ratio twice, sliding a downscaled
               crop with a large negative offset off its art. */
        } else if (command->type == DRAW_SHEARED_FILL) {
            command->text_length = (uint32_t) viewport_scale_ceil(
                (int) command->text_length, numerator, denominator);
            command->font_size = viewport_scale_ceil(
                command->font_size, numerator, denominator);
            command->radius = viewport_scale_floor(
                command->radius, numerator, denominator);
        } else if (command->type == DRAW_ROTATED_FILL) {
            unsigned w = (unsigned) viewport_scale_ceil(
                (int) draw_command_rotated_width(command),
                numerator, denominator);
            unsigned h = (unsigned) viewport_scale_ceil(
                (int) draw_command_rotated_height(command),
                numerator, denominator);
            if (w > 65535u) w = 65535u;
            if (h > 65535u) h = 65535u;
            draw_command_set_rotated_extent(command, w, h);
            command->radius = viewport_scale_floor(
                command->radius, numerator, denominator);
            command->scale = viewport_scale_floor(
                command->scale, numerator, denominator);
        } else if (command->type == DRAW_BORDER_TRIANGLE) {
            command->text_length = (uint32_t) viewport_scale_ceil(
                (int) command->text_length, numerator, denominator);
            command->font_size = viewport_scale_ceil(
                command->font_size, numerator, denominator);
        } else if (command->type == DRAW_IMAGE) {
            /* Replaced images pack object-position into radius/font_size.
               Preserve each percentage and scale only its signed CSS-pixel
               offset. Treating these words as radius codes corrupts both
               axes in a scaled visual clone. */
            uint16_t x = (uint16_t) draw_command_image_offset_x(command);
            uint16_t y = (uint16_t) draw_command_image_offset_y(command);
            draw_command_set_object_position(
                command,
                style_object_position_encode(
                    style_object_position_percent(x),
                    viewport_scale_floor(
                        style_object_position_offset(x),
                        numerator, denominator)),
                style_object_position_encode(
                    style_object_position_percent(y),
                    viewport_scale_floor(
                        style_object_position_offset(y),
                        numerator, denominator)));
        } else {
            command->radius = layout_scale_radius_code(
                command->radius, numerator, denominator);
        }
        if (command->type == DRAW_IMAGE
            && style_border_radius_maximum(command->scale) > 0) {
            command->scale = layout_scale_radius_code(
                command->scale, numerator, denominator);
        }
        if ((command->type == DRAW_STROKE_RECT
             || command->type == DRAW_SHADOW_RECT)
            && command->scale > 0) {
            command->scale = viewport_scale_ceil(
                command->scale, numerator, denominator);
            if (command->scale < 1) command->scale = 1;
        }
        if (command->type == DRAW_SHADOW_RECT
            && command->image_fit == LAYOUT_SHADOW_OUTSIDE_BOX)
            draw_command_set_shadow_offset(command,
                viewport_scale_floor((int16_t) command->text_length,
                                     numerator, denominator),
                viewport_scale_floor((int16_t) (command->text_length >> 16),
                                     numerator, denominator));
    }
    for (size_t i = 0; i < visual->link_count; i++) {
        viewport_css_box_to_device(&source->viewport, &visual->links[i].x,
                                   &visual->links[i].width);
        viewport_css_box_to_device(&source->viewport, &visual->links[i].y,
                                   &visual->links[i].height);
    }
    for (size_t i = 0; i < visual->control_count; i++) {
        viewport_css_box_to_device(&source->viewport, &visual->controls[i].x,
                                   &visual->controls[i].width);
        viewport_css_box_to_device(&source->viewport, &visual->controls[i].y,
                                   &visual->controls[i].height);
    }
    for (size_t i = 0; i < visual->sticky_count; i++) {
        StickyRange *range = &visual->sticky_ranges[i];
        range->origin_y = viewport_scale_floor(
            range->origin_y, numerator, denominator);
        range->top = viewport_scale_floor(
            range->top, numerator, denominator);
        range->bottom_y = viewport_scale_floor(
            range->bottom_y, numerator, denominator);
        if (range->maximum_offset != INT_MAX) {
            range->maximum_offset = viewport_scale_floor(
                range->maximum_offset, numerator, denominator);
        }
    }
    for (size_t i = 0; i < visual->fixed_count; i++) {
        FixedRange *range = &visual->fixed_ranges[i];
        range->origin_y = viewport_scale_floor(range->origin_y, numerator,
                                               denominator);
        range->height = viewport_scale_ceil(range->height, numerator,
                                            denominator);
        range->inset = viewport_scale_floor(range->inset, numerator,
                                            denominator);
        if (range->scroll_end != INT_MAX) {
            range->scroll_end = viewport_scale_floor(
                range->scroll_end, numerator, denominator);
        }
    }
    for (size_t i = 0; i < visual->node_box_count; i++) {
        LayoutNodeBox *box = &visual->node_boxes[i];
        int clip_radius_code = layout_node_box_clip_radius_code(visual, box);
        viewport_css_box_to_device(&source->viewport, &box->x, &box->width);
        viewport_css_box_to_device(&source->viewport, &box->y, &box->height);
        box->client_width = viewport_scale_ceil(box->client_width, numerator,
                                                denominator);
        box->client_height = viewport_scale_ceil(box->client_height, numerator,
                                                 denominator);
        box->content_width = viewport_scale_ceil(box->content_width, numerator,
                                                 denominator);
        box->content_height = viewport_scale_ceil(box->content_height, numerator,
                                                  denominator);
        box->scroll_x = viewport_scale_floor(box->scroll_x, numerator,
                                             denominator);
        box->scroll_y = viewport_scale_floor(box->scroll_y, numerator,
                                             denominator);
        clip_radius_code = layout_scale_radius_code(
            clip_radius_code, numerator, denominator);
        unsigned clip_radius = (unsigned)
            style_border_radius_maximum(clip_radius_code);
        unsigned clip_margin = (unsigned) viewport_scale_ceil(
            (int) layout_node_box_clip_margin(box), numerator, denominator);
        layout_node_box_set_clip_geometry(
            box, clip_radius, clip_margin, layout_node_box_clip_box(box));
        if (!layout_set_node_clip_radius_code(
                visual, i, clip_radius_code)) {
            layout_destroy(visual);
            return false;
        }
        layout_node_index_insert(visual, box->node, i);
    }
    layout_build_focus_index(visual);
    /* Parsed documents carry the authoritative stacking order. Small
       synthetic/embedder layouts may intentionally omit it, so retain the
       historical source-order fallback for that case. */
    if ((visual->paint_order_count != visual->count
         && !build_paint_order(visual, NULL))
        || !build_spatial_index(visual, NULL)) {
        layout_destroy(visual);
        return false;
    }
    return true;
}

struct LayoutGridColumns {
    const lxb_dom_node_t *node;
    int widths[GRID_TRACK_REPEAT_LIMIT];
    uint8_t count;
    uint8_t origin;
};

void layout_record_grid_columns(LayoutDocument *layout,
    lxb_dom_node_t *node, const int *widths, unsigned count, unsigned origin)
{
    enum { GRID_GEOMETRY_LIMIT = 128 };
    if (count == 0 || count > GRID_TRACK_REPEAT_LIMIT || node == NULL) return;
    size_t at = 0;
    for (; at < layout->grid_column_count; at++)
        if (layout->grid_columns[at].node == node) break;
    if (at == layout->grid_column_count) {
        if (layout->grid_columns_refused || at == GRID_GEOMETRY_LIMIT) return;
        if (at == layout->grid_column_capacity) {
            size_t capacity = at == 0 ? 8u : at * 2u;
            if (capacity > GRID_GEOMETRY_LIMIT) capacity = GRID_GEOMETRY_LIMIT;
            LayoutGridColumns *columns = budget_realloc(layout->budget,
                layout->grid_columns, capacity * sizeof(*columns));
            if (columns == NULL) {
                layout->grid_columns_refused = true;
                return;
            }
            layout->grid_columns = columns;
            layout->grid_column_capacity = capacity;
        }
        layout->grid_column_count++;
    }
    LayoutGridColumns *columns = &layout->grid_columns[at];
    columns->node = node;
    columns->count = (uint8_t) count;
    columns->origin = (uint8_t) origin;
    memcpy(columns->widths, widths, count * sizeof(*widths));
}

size_t layout_grid_columns_for_node(const LayoutDocument *layout,
    const lxb_dom_node_t *node, const int **widths, unsigned *origin)
{
    if (widths != NULL) *widths = NULL;
    if (origin != NULL) *origin = 0;
    if (layout == NULL || node == NULL) return 0;
    for (size_t i = 0; i < layout->grid_column_count; i++) {
        const LayoutGridColumns *columns = &layout->grid_columns[i];
        if (columns->node != node) continue;
        if (widths != NULL) *widths = columns->widths;
        if (origin != NULL) *origin = columns->origin;
        return columns->count;
    }
    return 0;
}

void layout_destroy(LayoutDocument *layout)
{
    if (layout == NULL) return;
    if (layout->budget != NULL) {
        budget_free(layout->budget, layout->commands);
        budget_free(layout->budget, layout->links);
        budget_free(layout->budget, layout->node_interaction_ranges);
        budget_free(layout->budget, layout->controls);
        budget_free(layout->budget, layout->generated_text_storage);
        budget_free(layout->budget, layout->bidi_glyphs);
        budget_free(layout->budget, layout->bidi_commands);
        budget_free(layout->budget, layout->sticky_ranges);
        budget_free(layout->budget, layout->fixed_ranges);
        budget_free(layout->budget, layout->node_boxes);
        budget_free(layout->budget, layout->grid_columns);
        budget_free(layout->budget, layout->scroll_containers);
        budget_free(layout->budget, layout->scroll_snap_candidates);
        budget_free(layout->budget, layout->node_container_padding_sums);
        budget_free(layout->budget, layout->node_container_contained_heights);
        budget_free(layout->budget, layout->node_clip_radius_codes);
        budget_free(layout->budget, layout->visual_priority_targets);
        budget_free(layout->budget, layout->node_index);
        budget_free(layout->budget, layout->focus_index);
        budget_free(layout->budget, layout->focus_pseudo_texts);
        budget_free(layout->budget, layout->paint_order);
        budget_free(layout->budget, layout->command_flags);
        budget_free(layout->budget, layout->spatial_band_offsets);
        budget_free(layout->budget, layout->spatial_band_orders);
        budget_free(layout->budget, layout->spatial_global_orders);
        budget_free(layout->budget, layout->overflow_orders);
        budget_free(layout->budget, layout->late_positioned_orders);
    }
    /* A failed build destroys its partial document on the way out; the
       owner still needs to know whether that build was cancelled. */
    bool cancelled = layout->performance.cancelled;
    memset(layout, 0, sizeof(*layout));
    layout->performance.cancelled = cancelled;
}

const char *layout_retain_generated_text(
    LayoutDocument *layout, const char *text, size_t length)
{
    if (layout == NULL || layout->budget == NULL || text == NULL
        || length == 0 || length + 1 > LAYOUT_GENERATED_TEXT_LIMIT) {
        return NULL;
    }
    /* Repeated bullets and short counter values dominate real lists. Reuse
       their retained bytes before consuming the fixed PSP arena. */
    for (size_t at = 0; at < layout->generated_text_used;) {
        const char *candidate = layout->generated_text_storage + at;
        size_t candidate_length = strlen(candidate);
        if (candidate_length == length
            && memcmp(candidate, text, length) == 0) {
            return candidate;
        }
        at += candidate_length + 1;
    }
    if (layout->generated_text_used
        > LAYOUT_GENERATED_TEXT_LIMIT - length - 1) return NULL;
    if (layout->generated_text_storage == NULL) {
        layout->generated_text_storage = budget_malloc(
            layout->budget, LAYOUT_GENERATED_TEXT_LIMIT);
        if (layout->generated_text_storage == NULL) return NULL;
    }
    char *retained =
        layout->generated_text_storage + layout->generated_text_used;
    memcpy(retained, text, length);
    retained[length] = '\0';
    layout->generated_text_used += length + 1;
    return retained;
}

static bool point_in_rounded_clip(const LayoutDocument *layout,
                                  const LayoutNodeBox *box, int x, int y)
{
    if (box == NULL || !box->clips_x || !box->clips_y
        || layout_node_box_clip_radius(box) == 0) return true;
    int left, top, right, bottom;
    layout_node_box_clip_rect(box, box->x, box->y,
                              &left, &top, &right, &bottom);
    int radius_code =
        layout_node_box_effective_clip_radius_code(layout, box);
    int width = right - left;
    int height = bottom - top;
    int radii[4];
    style_border_radius_resolve_corners(
        radius_code, width, height, radii);
    if (style_border_radius_maximum(radius_code) <= 0) return true;
    int64_t px = x;
    int64_t py = y;
    int radius = 0;
    int64_t cx = px;
    int64_t cy = py;
    if (px < left + radii[0] && py < top + radii[0]) {
        radius = radii[0];
        cx = left + radius - 1;
        cy = top + radius - 1;
    } else if (px >= right - radii[1] && py < top + radii[1]) {
        radius = radii[1];
        cx = right - radius;
        cy = top + radius - 1;
    } else if (px >= right - radii[2] && py >= bottom - radii[2]) {
        radius = radii[2];
        cx = right - radius;
        cy = bottom - radius;
    } else if (px < left + radii[3] && py >= bottom - radii[3]) {
        radius = radii[3];
        cx = left + radius - 1;
        cy = bottom - radius;
    } else {
        return true;
    }
    if (radius <= 0) return true;
    int64_t dx = px - cx;
    int64_t dy = py - cy;
    uint64_t absolute_x = dx < 0 ? (uint64_t) (-dx) : (uint64_t) dx;
    uint64_t absolute_y = dy < 0 ? (uint64_t) (-dy) : (uint64_t) dy;
    uint64_t limit = (uint64_t) radius;
    if (absolute_x >= limit || absolute_y >= limit) return false;
    uint64_t x_squared = absolute_x * absolute_x;
    uint64_t radius_squared = limit * limit;
    return absolute_y * absolute_y < radius_squared - x_squared;
}

/* Whether (x, y) lands on a region laid out at (region_x, region_y) inside
   node's ancestors: every clipping ancestor must contain the point, and
   ancestor scroll offsets move the region. Shared by link and control hit
   tests so both honour the same clips. */
static bool layout_region_hit(const LayoutDocument *layout,
                              const lxb_dom_node_t *node,
                              int region_x, int region_y,
                              int width, int height, int x, int y)
{
    for (lxb_dom_node_t *parent = node == NULL ? NULL : node->parent;
         parent != NULL; parent = parent->parent) {
        const LayoutNodeBox *box = layout_box_for_node(layout, parent);
        if (box == NULL) continue;
        int clip_left, clip_top, clip_right, clip_bottom;
        layout_node_box_clip_rect(
            box, box->x, box->y,
            &clip_left, &clip_top, &clip_right, &clip_bottom);
        bool outside = (box->clips_x
                        && (x < clip_left || x >= clip_right))
                       || (box->clips_y
                           && (y < clip_top || y >= clip_bottom));
        if (outside || !point_in_rounded_clip(layout, box, x, y))
            return false;
        region_x -= box->scroll_x;
        region_y -= box->scroll_y;
    }
    return x >= region_x && x < region_x + width
        && y >= region_y && y < region_y + height;
}

const LinkRegion *layout_link_at(const LayoutDocument *layout, int x, int y)
{
    if (layout == NULL) return NULL;
    const LinkRegion *hit = NULL;
    for (size_t i = 0; i < layout->link_count; i++) {
        const LinkRegion *link = &layout->links[i];
        if ((hit == NULL || link->z_index >= hit->z_index)
            && layout_region_hit(layout, link->node, link->x, link->y,
                                 link->width, link->height, x, y))
            hit = link;
    }
    return hit;
}

const ControlRegion *layout_control_at(const LayoutDocument *layout,
                                       int x, int y)
{
    if (layout == NULL) return NULL;
    const ControlRegion *hit = NULL;
    for (size_t i = 0; i < layout->control_count; i++) {
        const ControlRegion *control = &layout->controls[i];
        if ((hit == NULL || control->z_index >= hit->z_index)
            && layout_region_hit(layout, control->node, control->x,
                                 control->y, control->width,
                                 control->height, x, y))
            hit = control;
    }
    return hit;
}

const LayoutNodeBox *layout_box_for_node(const LayoutDocument *layout,
                                         const lxb_dom_node_t *node)
{
    if (layout == NULL || node == NULL) return NULL;
    size_t index = layout_node_box_index(layout, node);
    return index == SIZE_MAX ? NULL : &layout->node_boxes[index];
}

LayoutNodeBox *layout_box_for_node_mutable(
    LayoutDocument *layout, const lxb_dom_node_t *node)
{
    if (layout == NULL || node == NULL) return NULL;
    size_t index = layout_node_box_index(layout, node);
    return index == SIZE_MAX ? NULL : &layout->node_boxes[index];
}

bool layout_focus_for_node(const LayoutDocument *layout,
                           const lxb_dom_node_t *node,
                           bool *control, size_t *index)
{
    if (layout == NULL || node == NULL || layout->focus_index == NULL
        || layout->focus_index_capacity == 0) return false;
    size_t mask = layout->focus_index_capacity - 1u;
    size_t slot = layout_pointer_hash(node) & mask;
    for (size_t probes = 0; probes < layout->focus_index_capacity;
         probes++, slot = (slot + 1u) & mask) {
        const LayoutFocusIndexEntry *entry = &layout->focus_index[slot];
        if (entry->node == NULL) return false;
        if (entry->node != node) continue;
        if (entry->link_plus_one != 0) {
            if (control != NULL) *control = false;
            if (index != NULL) *index = entry->link_plus_one - 1u;
            return true;
        }
        if (entry->control_plus_one != 0) {
            if (control != NULL) *control = true;
            if (index != NULL) *index = entry->control_plus_one - 1u;
            return true;
        }
        return false;
    }
    return false;
}

bool layout_apply_focus_border_paint(
    LayoutDocument *layout, const Stylesheet *stylesheet,
    lxb_dom_node_t *node, const ComputedStyle *style, StyleFocusChange change, bool dry_run,
    int *left, int *top, int *right, int *bottom)
{
    int border_radius_code = stylesheet_border_radius_code(
        stylesheet, style);
    if (layout == NULL || stylesheet == NULL || node == NULL || style == NULL) return false;
    bool inset_changed = change == STYLE_FOCUS_CHANGE_INSET_PAINT_ONLY;
    bool outset_changed = change == STYLE_FOCUS_CHANGE_OUTSET_PAINT_ONLY;
    bool border = style_border_radius_maximum(border_radius_code) > 0
        && style->border.top > 0 && style->border.top == style->border.right
        && style->border.top == style->border.bottom && style->border.top == style->border.left;
    if (!border && !inset_changed && !outset_changed) return false;
    const LayoutNodeBox *box = layout_box_for_node(layout, node);
    if (box == NULL || box->width <= 0 || box->height <= 0
        || box->command_start > box->command_end
        || box->command_end > layout->count) return false;

    uint8_t alpha = 0;
    uint32_t color = stylesheet_border_color(
        stylesheet, style, STYLE_BORDER_TOP, &alpha);
    for (unsigned side = 0; border && side < STYLE_BORDER_SIDE_COUNT; side++) {
        uint8_t side_alpha = 0;
        uint32_t side_color = stylesheet_border_color(
            stylesheet, style, (StyleBorderSide) side, &side_alpha);
        if (side_color != color || side_alpha != alpha
            || computed_style_border_line(
                   style, (StyleBorderSide) side) != STYLE_BORDER_SOLID) {
            return false;
        }
    }

    size_t match = SIZE_MAX, inset = SIZE_MAX;
    for (size_t i = box->command_start; i < box->command_end; i++) {
        const DrawCommand *command = &layout->commands[i];
        if (outset_changed && command->type == DRAW_SHADOW_RECT
            && command->text == (const char *) node && command->scale == 0) {
            if (inset != SIZE_MAX) return false;
            inset = i;
            continue;
        }
        if (inset_changed && command->type == DRAW_STROKE_RECT
            && command->image_fit == LAYOUT_STROKE_FOCUS_INSET
            && command->text == (const char *) node
            && command->radius == border_radius_code) {
            if (inset != SIZE_MAX) return false;
            inset = i;
            continue;
        }
        if (command->type != DRAW_STROKE_RECT
            || command->text != (const char *) node
            || command->scale != style->border.top
            || command->radius != border_radius_code
            || command->image_fit != LAYOUT_STROKE_SOLID) continue;
        if (match != SIZE_MAX) return false;
        match = i;
    }
    if (border && match == SIZE_MAX) return false;
    uint32_t inset_color = 0;
    uint8_t inset_alpha = 0;
    int inset_width = 1;
    if (inset_changed || outset_changed) {
        if (inset == SIZE_MAX) return false;
        size_t count = stylesheet_box_shadow_count(stylesheet, style);
        size_t layer = outset_changed
            ? (layout->commands[inset].font_weight & LAYOUT_COMMAND_FONT_WEIGHT_MASK) : 0u;
        layer = layer == 0 ? 0 : layer - 1u;
        if (count != layer && count != layer + 1u) return false;
        if (count == layer + 1u) {
            const StyleBoxShadow *shadow = stylesheet_box_shadow(stylesheet, style, layer);
            if (shadow == NULL || style_box_shadow_is_inset(shadow) != inset_changed
                || style_box_shadow_blur(shadow) != 0
                || (inset_changed && (shadow->offset_x != 0
                    || shadow->offset_y != 0 || shadow->spread <= 0))) return false;
            inset_color = style_box_shadow_uses_current_color(shadow)
                ? style->color : shadow->argb & UINT32_C(0x00ffffff);
            inset_alpha = style_box_shadow_uses_current_color(shadow)
                ? style->color_alpha : (uint8_t) (shadow->argb >> 24);
            inset_width = shadow->spread;
            if (outset_changed) {
                const DrawCommand *c = &layout->commands[inset];
                /* Flex stretching can update the node box after decoration.
                   The owned slot retains the formatter's exact shadow box;
                   its admitted spread, not a reconstructed box, is the proof. */
                if (c->font_size != inset_width || c->width <= 0 || c->height <= 0)
                    return false;
            }
        }
    }
    if (!dry_run) {
        if (border) layout->commands[match].color = color;
        /* Commands in the retained list are already normalized; UINT16_MAX
           is only an insertion-time transparent sentinel. */
        if (border) layout->commands[match].opacity_scale = alpha == 0 ? 0 : alpha_opacity_scale(alpha);
        if (inset_changed || outset_changed) {
            layout->commands[inset].color = inset_color;
            if (inset_changed) layout->commands[inset].scale = inset_width;
            layout->commands[inset].opacity_scale = inset_alpha == 0
                ? 0 : alpha_opacity_scale(inset_alpha);
        }
    }
    if (left != NULL) *left = box->x - style->border.top;
    if (top != NULL) *top = box->y - style->border.top;
    if (right != NULL) *right = box->x + box->width + style->border.right;
    if (bottom != NULL) *bottom = box->y + box->height + style->border.bottom;
    const size_t painted[2] = {match, inset};
    for (unsigned i = 0; i < 2; i++) {
        if (painted[i] == SIZE_MAX) continue;
        const DrawCommand *c = &layout->commands[painted[i]];
        if (left != NULL && c->x < *left) *left = c->x;
        if (top != NULL && c->y < *top) *top = c->y;
        if (right != NULL && c->x + c->width > *right) *right = c->x + c->width;
        if (bottom != NULL && c->y + c->height > *bottom) *bottom = c->y + c->height;
    }
    return true;
}

bool layout_apply_focus_pseudo_translation(
    LayoutDocument *layout, lxb_dom_node_t *node,
    const StyleFocusPseudoTranslation *translation, bool focused, bool dry_run,
    int *left, int *top, int *right, int *bottom)
{
    const LayoutNodeBox *box = layout_box_for_node(layout, node);
    if (box == NULL || translation == NULL || box->command_end > layout->count
        || box->command_start > box->command_end) return false;
    size_t slots[2] = {SIZE_MAX, SIZE_MAX};
    for (size_t i = box->command_start; i < box->command_end; i++) {
        const DrawCommand *c = &layout->commands[i];
        unsigned pseudo = draw_command_focus_pseudo(c, node);
        if (c->type != DRAW_FILL_RECT || pseudo == PSEUDO_NONE) continue;
        unsigned p = pseudo - PSEUDO_BEFORE;
        if (slots[p] != SIZE_MAX) return false;
        slots[p] = i;
    }
    int bounds[4] = {INT_MAX, INT_MAX, INT_MIN, INT_MIN};
    for (unsigned p = 0; p < 2; p++) {
        if (translation->dx[p] == 0 && translation->dy[p] == 0) continue;
        if (slots[p] == SIZE_MAX) return false;
        const DrawCommand *c = &layout->commands[slots[p]];
        bool changed = c->font_weight != (unsigned) focused;
        int dx = !changed ? 0 : focused ? translation->dx[p] : -translation->dx[p];
        int dy = !changed ? 0 : focused ? translation->dy[p] : -translation->dy[p];
        int64_t x = (int64_t) c->x + dx, y = (int64_t) c->y + dy;
        if (x < INT_MIN || x > INT_MAX || y < INT_MIN || y > INT_MAX
            || (int64_t)c->x + c->width > INT_MAX || x + c->width > INT_MAX
            || (int64_t)c->y + c->height > INT_MAX || y + c->height > INT_MAX)
            return false;
        /* Preserve both spatial indices and the scroll extent. Moving a
           retained fill between bands needs an authoritative rebuild. */
        if (c->width <= 0 || c->height <= 0 || c->x < 0 || x < 0
            || c->x + c->width > layout->scroll_width || x + c->width > layout->scroll_width
            || c->y < 0 || y < 0 || c->y + c->height > layout->height
            || y + c->height > layout->height
            || c->y / LAYOUT_SPATIAL_BAND_HEIGHT != y / LAYOUT_SPATIAL_BAND_HEIGHT
            || (c->y + c->height - 1) / LAYOUT_SPATIAL_BAND_HEIGHT
               != (y + c->height - 1) / LAYOUT_SPATIAL_BAND_HEIGHT) return false;
        int ox = c->x, oy = c->y, ow = c->width, oh = c->height;
        int nx = (int) x, ny = (int) y, nw = ow, nh = oh;
        viewport_css_box_to_device(&layout->viewport, &ox, &ow);
        viewport_css_box_to_device(&layout->viewport, &oy, &oh);
        viewport_css_box_to_device(&layout->viewport, &nx, &nw);
        viewport_css_box_to_device(&layout->viewport, &ny, &nh);
        if (oh <= 0 || nh <= 0
            || oy / LAYOUT_SPATIAL_BAND_HEIGHT != ny / LAYOUT_SPATIAL_BAND_HEIGHT
            || (oy + oh - 1) / LAYOUT_SPATIAL_BAND_HEIGHT
               != (ny + nh - 1) / LAYOUT_SPATIAL_BAND_HEIGHT) return false;
        int l = dx < 0 ? (int) x : c->x;
        int t = dy < 0 ? (int) y : c->y;
        int r = (dx > 0 ? (int) x : c->x) + c->width;
        int b = (dy > 0 ? (int) y : c->y) + c->height;
        if (l < bounds[0]) bounds[0] = l;
        if (t < bounds[1]) bounds[1] = t;
        if (r > bounds[2]) bounds[2] = r;
        if (b > bounds[3]) bounds[3] = b;
    }
    if (bounds[0] == INT_MAX) return false;
    if (!dry_run) for (unsigned p = 0; p < 2; p++) {
        if (translation->dx[p] == 0 && translation->dy[p] == 0) continue;
        if (layout->commands[slots[p]].font_weight == (unsigned) focused) continue;
        layout->commands[slots[p]].x += focused ? translation->dx[p] : -translation->dx[p];
        layout->commands[slots[p]].y += focused ? translation->dy[p] : -translation->dy[p];
        layout->commands[slots[p]].font_weight = focused;
    }
    if (left) *left = bounds[0];
    if (top) *top = bounds[1];
    if (right) *right = bounds[2];
    if (bottom) *bottom = bounds[3];
    return true;
}

bool layout_apply_focus_pseudo_visibility(
    LayoutDocument *layout, lxb_dom_node_t *node,
    const StyleFocusPseudoTranslation *change, bool focused, bool dry_run,
    int *left, int *top, int *right, int *bottom)
{
    if (layout == NULL || node == NULL || change == NULL
        || !change->visibility_changed) return false;
    size_t slots[2] = {SIZE_MAX, SIZE_MAX};
    int bounds[4] = {INT_MAX, INT_MAX, INT_MIN, INT_MIN};
    /* Ordinary inline links need not own a LayoutNodeBox. Their absolute
       decorations retain native identity in the bounded display list, and
       the build indexes them. */
    bool indexed = layout->focus_pseudo_texts_indexed;
    size_t visits = indexed ? layout->focus_pseudo_text_count : layout->count;
    for (size_t at = 0; at < visits; at++) {
        size_t i = indexed ? layout->focus_pseudo_texts[at] : at;
        if (i >= layout->count) return false;
        const DrawCommand *c = &layout->commands[i];
        unsigned pseudo = draw_command_focus_pseudo(c, node);
        if (c->type != DRAW_TEXT || pseudo == PSEUDO_NONE) continue;
        unsigned p = pseudo - PSEUDO_BEFORE;
        if (!(change->visibility_changed & (1u << p))) continue;
        if (slots[p] != SIZE_MAX || c->width <= 0 || c->height <= 0
            || (int64_t) c->x + c->width > INT_MAX
            || (int64_t) c->y + c->height > INT_MAX) return false;
        slots[p] = i;
        if (c->x < bounds[0]) bounds[0] = c->x;
        if (c->y < bounds[1]) bounds[1] = c->y;
        if (c->x + c->width > bounds[2]) bounds[2] = c->x + c->width;
        if (c->y + c->height > bounds[3]) bounds[3] = c->y + c->height;
    }
    for (unsigned p = 0; p < 2; p++)
        if ((change->visibility_changed & (1u << p)) && slots[p] == SIZE_MAX)
            return false;
    if (!dry_run) {
        unsigned hidden = focused ? change->focused_hidden : change->normal_hidden;
        for (unsigned p = 0; p < 2; p++) {
            if (slots[p] == SIZE_MAX) continue;
            layout->commands[slots[p]].opacity_scale = hidden & (1u << p)
                ? 0 : alpha_opacity_scale(change->visibility_alpha[p]);
        }
    }
    if (left) *left = bounds[0];
    if (top) *top = bounds[1];
    if (right) *right = bounds[2];
    if (bottom) *bottom = bounds[3];
    return true;
}

typedef struct {
    const char *start[STYLE_FOCUS_COLOR_TEXT_LIMIT];
    const char *end[STYLE_FOCUS_COLOR_TEXT_LIMIT];
    size_t count;
} LayoutTextSpans;

static void layout_text_spans(LayoutTextSpans *spans,
                              lxb_dom_node_t *const *nodes, size_t count)
{
    spans->count = 0;
    for (size_t i = 0; i < count && i < STYLE_FOCUS_COLOR_TEXT_LIMIT; i++) {
        size_t length = 0;
        const char *data = nodes[i]->type == LXB_DOM_NODE_TYPE_ELEMENT
            ? document_attribute(nodes[i], "alt", &length)
            : layout_text_data(nodes[i], &length);
        if (data == NULL || length == 0) continue;
        spans->start[spans->count] = data;
        spans->end[spans->count++] = data + length;
    }
}

/* Whether a text command's UTF-8 is borrowed from one of `spans`. */
static bool layout_text_borrowed_from(const DrawCommand *command,
                                      const LayoutTextSpans *spans)
{
    if (command->text == NULL
        || (command->radius & LAYOUT_TEXT_IMAGE_ALT) != 0) return false;
    for (size_t i = 0; i < spans->count; i++)
        if (command->text >= spans->start[i] && command->text < spans->end[i])
            return true;
    return false;
}

/* Whether a boxed alt-text command (LAYOUT_TEXT_IMAGE_ALT) belongs to an
   img listed in `nodes`: it borrows no text node, but names its img. */
static bool layout_alt_text_listed(const DrawCommand *command,
                                   lxb_dom_node_t *const *nodes, size_t count)
{
    if ((command->radius & LAYOUT_TEXT_IMAGE_ALT) == 0) return false;
    for (size_t i = 0; i < count && i < STYLE_FOCUS_COLOR_TEXT_LIMIT; i++)
        if ((const void *) nodes[i] == (const void *) command->image) return true;
    return false;
}

static bool layout_rects_overlap(int ax, int ay, int aw, int ah,
                                 int bx, int by, int bw, int bh)
{
    return aw > 0 && ah > 0 && bw > 0 && bh > 0
        && (int64_t) ax < (int64_t) bx + bw && (int64_t) bx < (int64_t) ax + aw
        && (int64_t) ay < (int64_t) by + bh && (int64_t) by < (int64_t) ay + ah;
}

static void layout_bounds_add(int *bounds, int x, int y, int width, int height)
{
    if (x < bounds[0]) bounds[0] = x;
    if (y < bounds[1]) bounds[1] = y;
    if ((int64_t) x + width > bounds[2])
        bounds[2] = (int64_t) x + width > INT_MAX ? INT_MAX : x + width;
    if ((int64_t) y + height > bounds[3])
        bounds[3] = (int64_t) y + height > INT_MAX ? INT_MAX : y + height;
}

/* Whether a text command in the element's area that the recolour leaves
   alone is an injected ellipsis touching the recoloured runs: it would keep
   the old colour beside them. */
static bool layout_focus_ellipsis_near(const DrawCommand *c,
                                       const int *bounds)
{
    return c->text_length == 3 && c->text != NULL
        && memcmp(c->text, "\xe2\x80\xa6", 3) == 0
        && layout_rects_overlap(c->x - 2, c->y, c->width + 4, c->height,
               bounds[0], bounds[1], bounds[2] - bounds[0],
               bounds[3] - bounds[1]);
}

/* Whether an owned text command is painted in `style`'s colour (or, when
   `either`, in `other`'s) and casts no text shadow. */
static bool layout_focus_text_painted(const DrawCommand *c,
                                      const ComputedStyle *style,
                                      const ComputedStyle *other)
{
    bool painted = c->color == style->color
        && c->opacity_scale == alpha_opacity_scale(style->color_alpha);
    return !draw_command_is_text_shadow(c)
        && (painted || (c->color == other->color
                        && c->opacity_scale
                            == alpha_opacity_scale(other->color_alpha)));
}

bool layout_apply_focus_color_paint(
    LayoutDocument *layout, lxb_dom_node_t *node,
    const ComputedStyle *style, const ComputedStyle *other,
    const StyleFocusColorTexts *texts, LayoutFocusColorRuns *runs,
    bool dry_run, int *left, int *top, int *right, int *bottom)
{
    enum { AREA_LIMIT = 16, ELLIPSIS_LIMIT = 16 };
    if (layout == NULL || node == NULL || style == NULL || other == NULL
        || texts == NULL) return false;
    bool color = style->color != other->color
        || style->color_alpha != other->color_alpha;
    bool background = style->background != other->background
        || style->background_alpha != other->background_alpha;
    int bounds[4] = {INT_MAX, INT_MAX, INT_MIN, INT_MIN};
    const LayoutNodeBox *box = layout_box_for_node(layout, node);
    uint16_t text_opacity = alpha_opacity_scale(style->color_alpha);
    if (color && !dry_run && runs != NULL && runs->complete) {
        /* The dry run scanned the display list and listed every run it
           owns; since then only colours changed. Re-check those runs'
           paint, then recolour them. */
        for (size_t k = 0; k < runs->count; k++) {
            if (runs->owned[k] >= layout->count) return false;
            const DrawCommand *c = &layout->commands[runs->owned[k]];
            if (c->type != DRAW_TEXT
                || !layout_focus_text_painted(c, style, other)) return false;
            layout_bounds_add(bounds, c->x, c->y, c->width, c->height);
        }
#ifndef __PSP__
        tilefinch_test_faults()->focus_color_command_visits += runs->count;
#endif
        for (size_t k = 0; k < runs->count; k++) {
            DrawCommand *c = &layout->commands[runs->owned[k]];
            c->color = style->color;
            c->opacity_scale = text_opacity;
        }
    } else if (color) {
        /* The element's own area: its link fragments, else its box. */
        int areas[AREA_LIMIT][4];
        size_t area_count = 0;
        for (size_t i = 0; i < layout->link_count; i++) {
            const LinkRegion *link = &layout->links[i];
            if (link->node != node) continue;
            if (area_count == AREA_LIMIT) return false;
            areas[area_count][0] = link->x;
            areas[area_count][1] = link->y;
            areas[area_count][2] = link->width;
            areas[area_count][3] = link->height;
            area_count++;
        }
        if (area_count == 0 && box != NULL) {
            areas[0][0] = box->x;
            areas[0][1] = box->y;
            areas[0][2] = box->width;
            areas[0][3] = box->height;
            area_count = 1;
        }
        if (area_count == 0) return false;
        LayoutTextSpans owned_spans, kept_spans;
        layout_text_spans(&owned_spans, texts->texts, texts->text_count);
        layout_text_spans(&kept_spans, texts->kept, texts->kept_count);
        /* One pass: the owned runs (listed for the apply), and the text
           they leave alone. An unowned ellipsis is judged against the
           final bounds of the owned runs, so it waits for the pass end. */
        LayoutFocusColorRuns local_runs;
        LayoutFocusColorRuns *listed = runs != NULL ? runs : &local_runs;
        listed->count = 0;
        listed->complete = true;
        uint32_t ellipses[ELLIPSIS_LIMIT];
        size_t ellipsis_count = 0;
        bool ellipsis_overflow = false;
        size_t recoloured = 0;
#ifndef __PSP__
        tilefinch_test_faults()->focus_color_command_visits += layout->count;
#endif
        for (size_t i = 0; i < layout->count; i++) {
            const DrawCommand *c = &layout->commands[i];
            if (c->type != DRAW_TEXT) continue;
            bool alt = (c->radius & LAYOUT_TEXT_IMAGE_ALT) != 0;
            bool owned = alt
                ? layout_alt_text_listed(c, texts->texts, texts->text_count)
                : layout_text_borrowed_from(c, &owned_spans);
            if (owned) {
                if (!layout_focus_text_painted(c, style, other)) return false;
                layout_bounds_add(bounds, c->x, c->y, c->width, c->height);
                recoloured++;
                if (listed->count < LAYOUT_FOCUS_COLOR_RUN_LIMIT)
                    listed->owned[listed->count++] = (uint32_t) i;
                else listed->complete = false;
                continue;
            }
            /* Text in the element's area that no listed node owns (an
               injected ellipsis, a copied run) would keep its colour. */
            bool kept = alt
                ? layout_alt_text_listed(c, texts->kept, texts->kept_count)
                : layout_text_borrowed_from(c, &kept_spans);
            if (alt && !kept) {
                /* Boxed alt text of an img the proof did not list borrows
                   no text node: refuse it inside the element. */
                for (const lxb_dom_node_t *at =
                         (const lxb_dom_node_t *) c->image;
                     at != NULL; at = at->parent)
                    if (at == node) return false;
            }
            for (size_t a = 0; a < area_count && !kept; a++)
                if (layout_rects_overlap(c->x, c->y, c->width, c->height,
                        areas[a][0], areas[a][1], areas[a][2], areas[a][3]))
                    return false;
            if (c->text_length == 3 && c->text != NULL
                && memcmp(c->text, "\xe2\x80\xa6", 3) == 0) {
                if (ellipsis_count < ELLIPSIS_LIMIT)
                    ellipses[ellipsis_count++] = (uint32_t) i;
                else ellipsis_overflow = true;
            }
        }
        if (recoloured != 0) {
            for (size_t k = 0; k < ellipsis_count; k++)
                if (layout_focus_ellipsis_near(
                        &layout->commands[ellipses[k]], bounds)) return false;
            if (ellipsis_overflow) {
#ifndef __PSP__
                tilefinch_test_faults()->focus_color_command_visits +=
                    layout->count;
#endif
                for (size_t i = 0; i < layout->count; i++) {
                    const DrawCommand *c = &layout->commands[i];
                    if (c->type != DRAW_TEXT
                        || !layout_focus_ellipsis_near(c, bounds)
                        || ((c->radius & LAYOUT_TEXT_IMAGE_ALT) != 0
                            ? layout_alt_text_listed(c, texts->texts,
                                                     texts->text_count)
                            : layout_text_borrowed_from(c, &owned_spans)))
                        continue;
                    return false;
                }
            }
        }
        if (!dry_run) {
            if (listed->complete) {
                for (size_t k = 0; k < listed->count; k++) {
                    DrawCommand *c = &layout->commands[listed->owned[k]];
                    c->color = style->color;
                    c->opacity_scale = text_opacity;
                }
            } else {
#ifndef __PSP__
                tilefinch_test_faults()->focus_color_command_visits +=
                    layout->count;
#endif
                for (size_t i = 0; i < layout->count; i++) {
                    DrawCommand *c = &layout->commands[i];
                    if (c->type != DRAW_TEXT
                        || !((c->radius & LAYOUT_TEXT_IMAGE_ALT) != 0
                             ? layout_alt_text_listed(c, texts->texts,
                                                      texts->text_count)
                             : layout_text_borrowed_from(c, &owned_spans)))
                        continue;
                    c->color = style->color;
                    c->opacity_scale = text_opacity;
                }
            }
        }
    }
    if (background) {
        /* The element's own solid fill: inside its border box, within its
           border widths of each edge, painted in either state's colour. */
        if (box == NULL || box->command_start > box->command_end
            || box->command_end > layout->count) return false;
        uint16_t opacity = alpha_opacity_scale(style->background_alpha);
        uint16_t other_opacity = alpha_opacity_scale(other->background_alpha);
        size_t match = SIZE_MAX;
        for (size_t i = box->command_start; i < box->command_end; i++) {
            const DrawCommand *c = &layout->commands[i];
            if (c->type != DRAW_FILL_RECT || c->text != NULL
                || c->image != NULL || c->image_fit != 0) continue;
            int64_t inset_left = (int64_t) c->x - box->x;
            int64_t inset_top = (int64_t) c->y - box->y;
            int64_t inset_right = ((int64_t) box->x + box->width)
                - ((int64_t) c->x + c->width);
            int64_t inset_bottom = ((int64_t) box->y + box->height)
                - ((int64_t) c->y + c->height);
            if (inset_left < 0 || inset_top < 0 || inset_right < 0
                || inset_bottom < 0 || inset_left > style->border.left
                || inset_top > style->border.top
                || inset_right > style->border.right
                || inset_bottom > style->border.bottom) continue;
            if (!((c->color == style->background && c->opacity_scale == opacity)
                  || (c->color == other->background
                      && c->opacity_scale == other_opacity))) continue;
            if (match != SIZE_MAX) return false;
            match = i;
        }
        if (match == SIZE_MAX) return false;
        DrawCommand *c = &layout->commands[match];
        if (draw_command_backdrop_blur(c) != 0) return false;
        layout_bounds_add(bounds, c->x, c->y, c->width, c->height);
        if (!dry_run) {
            c->color = style->background;
            c->opacity_scale = opacity;
        }
    }
    if (bounds[0] > bounds[2] || bounds[1] > bounds[3]) return false;
    /* Glyph ink and decorations can reach just past a text run's box. */
    if (left != NULL) *left = bounds[0] > INT_MIN + 2 ? bounds[0] - 2 : bounds[0];
    if (top != NULL) *top = bounds[1] > INT_MIN + 2 ? bounds[1] - 2 : bounds[1];
    if (right != NULL) *right = bounds[2] < INT_MAX - 2 ? bounds[2] + 2 : bounds[2];
    if (bottom != NULL) *bottom = bounds[3] < INT_MAX - 2 ? bounds[3] + 2 : bounds[3];
    return true;
}

bool layout_scroll_node(LayoutDocument *layout, lxb_dom_node_t *node,
                        int scroll_x, int scroll_y)
{
    if (layout == NULL || node == NULL) return false;
    size_t index = layout_node_box_index(layout, node);
    if (index != SIZE_MAX) {
        LayoutNodeBox *box = &layout->node_boxes[index];
        int maximum_x = box->content_width - box->client_width;
        int maximum_y = box->content_height - box->client_height;
        if (!box->clips_x || box->clip_only_x || maximum_x < 0) maximum_x = 0;
        if (!box->clips_y || box->clip_only_y || maximum_y < 0) maximum_y = 0;
        if (scroll_x < 0) scroll_x = 0;
        if (scroll_y < 0) scroll_y = 0;
        if (scroll_x > maximum_x) scroll_x = maximum_x;
        if (scroll_y > maximum_y) scroll_y = maximum_y;
        if (box->scroll_x != scroll_x || box->scroll_y != scroll_y)
            layout->scroll_generation++;
        box->scroll_x = scroll_x;
        box->scroll_y = scroll_y;
        return true;
    }
    return false;
}

void layout_transfer_scroll_state(const LayoutDocument *previous,
                                  LayoutDocument *replacement)
{
    if (previous == NULL || replacement == NULL) return;
    for (size_t i = 0; i < previous->node_box_count; i++) {
        const LayoutNodeBox *box = &previous->node_boxes[i];
        if (box->node == NULL || (box->scroll_x == 0 && box->scroll_y == 0)) {
            continue;
        }
        /* Reapply through the public clamp so an axis that changed to
           overflow:clip resets to zero while the still-scrollable axis
           preserves its visual position. */
        (void) layout_scroll_node(
            replacement, box->node, box->scroll_x, box->scroll_y);
    }
}
