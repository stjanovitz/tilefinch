#ifndef TILEFINCH_LAYOUT_INTERNAL_H
#define TILEFINCH_LAYOUT_INTERNAL_H

/* Shared internals for the layout subsystem translation units
   (layout.c, layout_paint.c, layout_inline.c, layout_flex.c,
   layout_table.c, layout_block.c).  Nothing here is part of the public
   tilefinch API; consumers must keep including tilefinch/layout.h only. */

#include "tilefinch/layout.h"
#include "tilefinch/integer_math.h"
#include "style_paint_internal.h"

#include <limits.h>

#define budget_malloc(b, s) budget_malloc_category((b), BUDGET_CATEGORY_LAYOUT, (s))
#define budget_calloc(b, n, s) budget_calloc_category((b), BUDGET_CATEGORY_LAYOUT, (n), (s))
#define budget_realloc(b, p, s) budget_realloc_category((b), BUDGET_CATEGORY_LAYOUT, (p), (s))

typedef struct {
    lxb_dom_node_t *cell;
    uint8_t column;
    uint8_t column_span;
    uint8_t row_span;
} TableCellPlacement;

typedef struct {
    lxb_dom_node_t *table;
    int available_width;
    size_t count;
    int widths[16];
    TableCellPlacement *placements;
    size_t placement_count;
    lxb_dom_node_t *first_row;
    lxb_dom_node_t *last_row;
    int collapsed_left_gutter;
    int collapsed_right_gutter;
    int collapsed_top_gutter;
    int collapsed_bottom_gutter;
    uint8_t spacing_x;
    uint8_t spacing_y;
    bool border_collapse;
} TableTracks;

#define LAYOUT_MARGIN_CACHE_CAPACITY 128
#define LAYOUT_MARGIN_CACHE_PROBE_LIMIT 4

typedef struct {
    lxb_dom_node_t *node;
    uint32_t style_signature;
    int top;
    int bottom;
    uint64_t stamp;
    uint8_t valid_mask;
} LayoutMarginCacheEntry;

/* Layout repeatedly asks for the same node's style while measuring intrinsic
   sizes and then placing flex/table content.  Styles cannot change during one
   layout build, so this bounded local cache avoids re-running the complete
   selector cascade without retaining page-lifetime state.  The 512-entry
   table is transient: it is released with the LayoutContext and therefore
   does not increase the committed page footprint.

   Measured on the same large article fixture in a release build (2026-07),
   raising the table from 128 to 512 reduced median layout time from about
   159 ms to 131 ms while adding about 192 KiB only during layout.  That is a
   useful PSP trade because it removes repeated selector work without
   increasing the retained cache/tile set.  Re-measure with
   TILEFINCH_TRACE_LAYOUT_PROFILE if ComputedStyle grows materially. */
#define LAYOUT_STYLE_CACHE_CAPACITY 512
/* Must divide the capacity; bounds the open-addressing probe window of the
   per-build style cache. */
#define LAYOUT_STYLE_CACHE_PROBE_LIMIT 8
/* Intrinsic size walks revisit the same flex/table subtree with the same
   containing-width bound.  Cache completed measurements only for the life of
   one layout build; node content, styles, images, and fonts are immutable at
   that boundary.  The key includes every varying input (node, limit, maximum
   versus minimum, and the min-width exception), so this changes no sizing
   semantics and adds only a small transient table.  Three-run article
   section-expansion medians (release, 2026-07) favored 256 entries: 47.3 ms
   versus 50.9 ms at 128 and 48.1 ms at 512, with identical output frames and
   unchanged retained/global-peak budget readings. */
#define LAYOUT_INTRINSIC_CACHE_CAPACITY 256
#define LAYOUT_INTRINSIC_CACHE_PROBE_LIMIT 8
#if (LAYOUT_STYLE_CACHE_CAPACITY & (LAYOUT_STYLE_CACHE_CAPACITY - 1)) != 0 \
    || LAYOUT_STYLE_CACHE_PROBE_LIMIT > LAYOUT_STYLE_CACHE_CAPACITY
#error "layout style cache must be a power of two and cover its probe window"
#endif
#if (LAYOUT_INTRINSIC_CACHE_CAPACITY \
       & (LAYOUT_INTRINSIC_CACHE_CAPACITY - 1)) != 0 \
    || LAYOUT_INTRINSIC_CACHE_PROBE_LIMIT > LAYOUT_INTRINSIC_CACHE_CAPACITY
#error "layout intrinsic cache must be a power of two and cover its probe window"
#endif
#define LAYOUT_WORK_QUOTA 16
/* Auto-table sizing normally inspects every row before placing the first
   one. The ephemeral first-paint pass needs only a representative bounded
   prefix; the authoritative pass still measures every row. */
#define LAYOUT_PREVIEW_TABLE_ROW_LIMIT 16
/* Authored DOM depth is untrusted.  Layout still has a few stateful,
   post-order algorithms whose complete worklist conversion would duplicate
   their state machines.  Their large per-level state lives in the bounded
   heap scratch arena, while the audited native frames stay small.  Sixty-four
   levels covers ordinary framework and news-site wrapper depth without
   giving hostile input unbounded C-stack growth; deeper trees retain the
   deterministic content-preserving fallback. */
#ifndef LAYOUT_TREE_CALL_DEPTH_LIMIT
/* Keep ordinary framework wrapper depth (the 40-level regression case)
   exact, then enter the bounded content-only fallback before the large block
   layout frame can consume either the PSP main stack or an ASan host stack.
   At 64 levels the current feature-complete frame exhausted an 8 MiB ASan
   stack before the guard could run. */
#define LAYOUT_TREE_CALL_DEPTH_LIMIT 48
#endif
/* Block layout carries substantially more per-level state than the other
   recursive walkers.  Allocate that state in small, per-build pages so the
   larger correctness bound does not reserve its worst-case footprint for
   shallow documents.  Pages form a bounded cache for sibling subtrees and
   are all released as soon as the layout build completes.

   Measured with identical 24 MiB replay runs (2026-07): a compact news page
   used 64,256 scratch bytes instead of the old 96,384; a long article used
   the same 96,384; a deep news front used 160,640 to eliminate 357 depth
   fallbacks, while its whole-page peak remained unchanged at 11,656,153 bytes
   because layout was
   not the global high-water phase.  Those figures predate the margin-collapse
   state moving into this arena, which grew each entry by about a third; the
   page count is unchanged because it is fixed by the depth bound. */
#ifndef LAYOUT_BLOCK_SCRATCH_PAGE_DEPTH
#define LAYOUT_BLOCK_SCRATCH_PAGE_DEPTH 8
#endif
#if LAYOUT_TREE_CALL_DEPTH_LIMIT < 1 || LAYOUT_BLOCK_SCRATCH_PAGE_DEPTH < 1
#error "layout depth and scratch page bounds must be positive"
#endif
#define LAYOUT_BLOCK_SCRATCH_PAGE_COUNT \
    ((LAYOUT_TREE_CALL_DEPTH_LIMIT + LAYOUT_BLOCK_SCRATCH_PAGE_DEPTH - 1) \
     / LAYOUT_BLOCK_SCRATCH_PAGE_DEPTH)
#define LAYOUT_FALLBACK_VISIT_LIMIT 65536
/* A single CSS length and the retained scroll coordinate use the same
   bounded domain.  This prevents thousands of legal maximum-sized boxes
   from overflowing int coordinates or forcing a multi-megabyte spatial
   index for a document no handheld can usefully navigate. */
#define LAYOUT_COORDINATE_LIMIT STYLE_LENGTH_DIRECT_LIMIT

static inline int layout_clamp_coordinate(int64_t value)
{
    if (value > LAYOUT_COORDINATE_LIMIT) return LAYOUT_COORDINATE_LIMIT;
    if (value < -LAYOUT_COORDINATE_LIMIT) return -LAYOUT_COORDINATE_LIMIT;
    return (int) value;
}

static inline int layout_add_coordinate(int left, int right)
{
    return layout_clamp_coordinate((int64_t) left + right);
}

static inline int layout_subtract_coordinate(int left, int right)
{
    return layout_clamp_coordinate((int64_t) left - right);
}

static inline int layout_scale_dimension(int value, int numerator, int denominator)
{
    if (value <= 0 || numerator <= 0 || denominator <= 0) return 0;
    int result = tilefinch_mul_div_int(value, numerator, denominator);
    return result > LAYOUT_COORDINATE_LIMIT
        ? LAYOUT_COORDINATE_LIMIT : result;
}

typedef struct LayoutBlockScratch LayoutBlockScratch;

enum {
    LAYOUT_NODE_LINK_START,
    LAYOUT_NODE_LINK_END,
    LAYOUT_NODE_CONTROL_START,
    LAYOUT_NODE_CONTROL_END,
    LAYOUT_NODE_INTERACTION_STRIDE
};

/* Stacking contexts are needed only while the retained display list is
   ordered.  Keep their nesting metadata in the transient LayoutContext
   rather than adding a word to every PSP DrawCommand or LayoutNodeBox. */
typedef struct {
    lxb_dom_node_t *node;
    uint32_t command_start;
    uint32_t command_end;
    uint32_t decoration_end;
    int z_index;
} LayoutStackingContext;

/* visibility participates in layout and is inherited, but a descendant may
   explicitly become visible again.  Retain only visibility boundaries while
   layout is in flight, then resolve them into command/interaction state and
   discard them with LayoutContext. */
typedef struct {
    uint32_t command_start;
    uint32_t command_end;
    uint32_t link_start;
    uint32_t link_end;
    uint32_t control_start;
    uint32_t control_end;
    bool hidden;
} LayoutVisibilityRange;

typedef struct {
    lxb_dom_node_t *node;
    ComputedStyle style;
    uint64_t stamp;
} LayoutStyleCacheEntry;

typedef struct {
    lxb_dom_node_t *node;
    int limit;
    int width;
    uint8_t kind;
    bool ignore_own_width;
    uint64_t stamp;
} LayoutIntrinsicCacheEntry;

/* A subgrid consumes only the parent tracks covered by its own grid area.
   Carry that settled slice through the recursive layout call instead of
   retaining another track table on every computed style.  Twelve is the
   engine's existing explicit-column bound; row spans are already clamped to
   eight, so the same arrays cover both axes. */
#define LAYOUT_ASSIGNED_GRID_TRACK_LIMIT GRID_TRACK_REPEAT_LIMIT
typedef struct {
    lxb_dom_node_t *node;
    uint8_t column_count;
    uint8_t row_count;
    uint8_t column_line_names[GRID_TRACK_REPEAT_LIMIT + 1]
                             [STYLE_GRID_LINE_NAMES_PER_LINE];
    uint8_t row_line_names[GRID_TRACK_REPEAT_LIMIT + 1]
                          [STYLE_GRID_LINE_NAMES_PER_LINE];
    int column_widths[LAYOUT_ASSIGNED_GRID_TRACK_LIMIT];
    int row_heights[LAYOUT_ASSIGNED_GRID_TRACK_LIMIT];
    int column_gap;
    int row_gap;
} LayoutAssignedGridTracks;

#define LAYOUT_REUSE_STYLE_CAPACITY 1024u
#define LAYOUT_REUSE_INTRINSIC_CAPACITY 128u
#define LAYOUT_REUSE_TABLE_ROW_CAPACITY 32u
#define LAYOUT_REUSE_TABLE_ROW_PROBE_LIMIT 8u
#if (LAYOUT_REUSE_TABLE_ROW_CAPACITY \
       & (LAYOUT_REUSE_TABLE_ROW_CAPACITY - 1)) != 0 \
    || LAYOUT_REUSE_TABLE_ROW_PROBE_LIMIT > LAYOUT_REUSE_TABLE_ROW_CAPACITY
#error "layout table-row reuse cache must be a power of two and cover its probe window"
#endif

typedef struct {
    lxb_dom_node_t *node;
    uint64_t parent_hash;
    ComputedStyle style;
    uint64_t stamp;
} LayoutReuseStyleEntry;

/* A viewport preview measures only a bounded prefix of an auto-layout table.
   Retain each completed row's intrinsic contribution, rather than the
   provisional aggregate tracks, so the authoritative pass can include every
   row without repeating the prefix's selector and text work. */
typedef struct {
    lxb_dom_node_t *row;
    int available_width;
    size_t count;
    int preferred[16];
    int minimum[16];
    uint64_t stamp;
} LayoutReuseTableRowEntry;

struct LayoutReuseCache {
    Budget *budget;
    const Stylesheet *sheet;
    uint64_t sheet_generation;
    const FontSet *fonts;
    const ImageResources *images;
    int viewport_width;
    uint64_t clock;
    LayoutReuseStyleEntry styles[LAYOUT_REUSE_STYLE_CAPACITY];
    LayoutIntrinsicCacheEntry intrinsic[LAYOUT_REUSE_INTRINSIC_CAPACITY];
    LayoutReuseTableRowEntry *table_rows;
    LayoutReuseStats stats;
    bool selector_has_has;
    bool selector_has_focus_within;
    bool selector_focus_has_sibling;
    bool selector_has_structure;
};

typedef struct {
    LayoutDocument *layout;
    const Stylesheet *sheet;
    const FontSet *fonts;
    const WebFontSet *web_fonts;
    const ImageResources *images;
    size_t trace_paint_lines;
    size_t trace_flex_translate_lines;
    size_t trace_flex_sizing_lines;
    uint64_t style_resolutions;
    uint64_t style_cache_hits;
    uint64_t style_cache_misses;
    uint64_t style_resolve_us;
    uint64_t style_rule_queries_at_start;
    uint64_t style_rule_candidates_at_start;
    uint64_t style_variable_lookups_at_start;
    uint64_t style_variable_rule_candidates_at_start;
    uint64_t style_variable_cache_hits_at_start;
    uint64_t style_variable_cache_misses_at_start;
    uint64_t style_variable_cache_negative_hits_at_start;
    uint64_t style_variable_cache_evictions_at_start;
    size_t style_variable_cache_bytes;
    bool style_variable_cache_owned;
    bool style_selector_cooperation_owned;
    uint64_t style_deferred_rule_applications_at_start;
    uint64_t style_deferred_rule_us_at_start;
    uint64_t style_cache_clock;
    uint64_t intrinsic_width_visits;
    uint64_t intrinsic_min_visits;
    uint64_t intrinsic_cache_hits;
    uint64_t intrinsic_cache_misses;
    uint64_t intrinsic_cache_clock;
    uint64_t intrinsic_paired_text_measurements;
    uint64_t margin_collapse_visits;
    uint64_t margin_cache_hits;
    uint64_t margin_cache_misses;
    uint64_t margin_cache_clock;
    uint64_t flat_iterator_passes;
    uint64_t flat_iterator_yields;
    uint64_t flex_iterator_passes;
    uint64_t flex_iterator_yields;
    uint64_t flex_basis_resolutions;
    uint64_t flex_minimum_resolutions;
    LayoutBlockScratch *block_scratch_pages[LAYOUT_BLOCK_SCRATCH_PAGE_COUNT];
    size_t block_scratch_page_count;
    size_t block_scratch_allocation_failures;
    size_t tree_call_depth;
    size_t max_tree_call_depth;
    size_t depth_fallback_count;
    size_t fallback_visits;
    bool depth_limit_reported;
    bool failure_reported;
    bool intrinsic_pair_mode;
    /* Prevent an intrinsic min/max constraint from recursively applying
       itself while its content contribution is being measured. */
    uint8_t intrinsic_constraint_depth;
    LayoutStyleCacheEntry style_cache[LAYOUT_STYLE_CACHE_CAPACITY];
    LayoutIntrinsicCacheEntry
        intrinsic_cache[LAYOUT_INTRINSIC_CACHE_CAPACITY];
    LayoutMarginCacheEntry margin_cache[LAYOUT_MARGIN_CACHE_CAPACITY];
    TableTracks table_tracks[32];
    size_t table_track_count;
    LayoutStackingContext *stacking_contexts;
    size_t stacking_context_count;
    size_t stacking_context_capacity;
    LayoutVisibilityRange *visibility_ranges;
    size_t visibility_range_count;
    size_t visibility_range_capacity;
    uint64_t slice_started_ns;
    size_t slice_units;
    lxb_dom_node_t *slice_last_node;
    /* Nonzero only for the ephemeral first-paint pass. Work whose normal-flow
       origin reaches this CSS y coordinate can be deferred to the
       authoritative build. */
    int preview_y_limit;
    bool preview_truncated;
    /* A grid container may assign a definite stretched row size without
       mutating the retained computed style. The node key makes this safe
       across recursive child layout. */
    lxb_dom_node_t *assigned_grid_node;
    int assigned_grid_height;
    bool assigned_grid_height_valid;
    LayoutAssignedGridTracks assigned_grid_tracks;
    lxb_dom_node_t *assigned_flex_node;
    int assigned_flex_height;
    bool assigned_flex_minimum;
    /* Per-command blur is separately bounded by radius and raster area.
       This counter bounds their aggregate per-frame work; overflow disables
       the cosmetic effect for the whole layout so fixed chrome can return to
       the retained cache instead of repeatedly traversing and blurring it. */
    uint8_t backdrop_filter_count;
    bool backdrop_filter_disabled;
    bool cancelled;
    LayoutReuseCache *reuse;
} LayoutContext;

void layout_note_unresolved_external_visual(
    LayoutContext *context, lxb_dom_node_t *node, const char *source,
    uint8_t kind, PseudoElement pseudo);

static inline bool layout_preview_limit_reached(LayoutContext *context,
                                                int y)
{
    if (context == NULL || context->preview_y_limit <= 0
        || y < context->preview_y_limit) return false;
    context->preview_truncated = true;
    return true;
}

#define ACTIVE_FLOAT_LIMIT 8

typedef struct {
    int x;
    int right;
    int top;
    int bottom;
    FloatMode side;
} FloatExclusion;

typedef struct {
    lxb_dom_node_t *node;
    int x;
    int y;
    int width;
    int height;
    /* CSS transforms, filters, perspective, containment, and an authored
       will-change establish a separate containing block for fixed
       descendants. Keep it alongside the ordinary positioned ancestor: a
       nearer position:relative ancestor must not erase an earlier transform. */
    lxb_dom_node_t *fixed_node;
    int fixed_x;
    int fixed_y;
    int fixed_width;
    int fixed_height;
    const FloatExclusion *float_exclusions;
    size_t float_count;
} PositionedBox;

static inline bool layout_style_establishes_fixed_containing_block(
    const ComputedStyle *style)
{
    return style != NULL
        && (style->has_transform || style->has_perspective
            || style->has_filter || style->has_layout_containment
            || style->will_change_transform);
}

/* During line construction, the high URL-length bit tags z_index as a
   one-based negative draw-command mapping. Line flush clears both before the
   retained LinkRegion becomes visible to hit testing or paint ordering. */
#define LAYOUT_LINK_TRANSIENT_COMMAND UINT32_C(0x80000000)
#define LAYOUT_LINK_URL_LENGTH_MASK UINT32_C(0x7fffffff)

typedef struct {
    int start_x;
    int base_left;
    int x;
    int x_fixed;
    int right;
    int base_right;
    int y;
    int y_fixed;
    int line_height;
    int line_height_fixed;
    int line_gap;
    /* The authored first-line indent is distinct from the temporary
       inline-start displacement imposed by active floats. */
    int first_line_indent;
    bool pending_space;
    bool find_block_start;
    bool has_text_character;
    bool previous_atomic;
    int8_t letter_boundary_spacing;
    int8_t pending_space_letter_spacing;
    bool x_fixed_valid;
    bool y_fixed_valid;
    uint32_t text_generation;
    LayoutDocument *layout;
    size_t command_start;
    size_t link_start;
    size_t control_start;
    size_t node_box_start;
    TextAlign text_align;
    bool direction_rtl;
    PositionedBox positioned_box;
    bool floats_enabled;
    bool first_inline_block_collapses_top;
    bool text_overflow_ended;
    uint8_t clamp_limit;
    uint8_t clamp_lines;
    bool clamp_pending;
    size_t clamp_command_start;
    size_t clamp_command_end;
    size_t clamp_link_start;
    size_t clamp_link_end;
    int clamp_right;
    size_t float_count;
    FloatExclusion floats[ACTIVE_FLOAT_LIMIT];
} LineState;

bool layout_line_clamp_overflow(LineState *line);

static inline int layout_fixed_from_integer(int value)
{
    int64_t fixed = (int64_t) value * 64;
    return fixed > INT_MAX ? INT_MAX
           : (fixed < INT_MIN ? INT_MIN : (int) fixed);
}

static inline int layout_fixed_floor(int value)
{
    int quotient = value / 64;
    if (value < 0 && value % 64 != 0) quotient--;
    return quotient;
}

static inline int layout_fixed_ceil(int value)
{
    int quotient = value / 64;
    if (value > 0 && value % 64 != 0) quotient++;
    return quotient;
}

static inline int layout_fixed_fraction(int value)
{
    int floor = layout_fixed_floor(value);
    return value - floor * 64;
}

static inline int layout_fixed_add(int left, int right)
{
    int64_t sum = (int64_t) left + right;
    return sum > INT_MAX ? INT_MAX
           : (sum < INT_MIN ? INT_MIN : (int) sum);
}

static inline int layout_fixed_subtract(int left, int right)
{
    int64_t difference = (int64_t) left - right;
    return difference > INT_MAX ? INT_MAX
           : (difference < INT_MIN ? INT_MIN : (int) difference);
}

typedef struct {
    bool active;
    int margin_top;
    int margin_right;
    int margin_bottom;
    int margin_left;
    int border_height;
} GeneratedPseudoFlow;

typedef struct {
    int largest_positive;
    int smallest_negative;
    bool valid;
} CollapsedMargin;

typedef struct {
    LayoutContext *context;
    lxb_dom_node_t *container;
    lxb_dom_node_t *cursor;
    ComputedStyle container_style;
    bool include_whitespace;
} FlatItemIterator;

typedef struct {
    lxb_dom_node_t *node;
    ComputedStyle parent_style;
    ComputedStyle style;
    bool anonymous_text;
} FlatItem;

typedef struct {
    Budget *budget;
    int *values;
    size_t count;
    size_t capacity;
    bool active;
    int inline_values[4];
} FlexOrderPlan;

typedef struct {
    FlatItemIterator source;
    const FlexOrderPlan *plan;
    size_t order_index;
} FlexItemIterator;

#define GRID_EXPLICIT_TRACK_LIMIT 8
#define GRID_TRACK_LIMIT GRID_TRACK_REPEAT_LIMIT
#define GRID_PLACEMENT_ROW_LIMIT 64

/* Grid auto-placement is transient and deliberately fixed-size. One word per
   possible row is an occupancy bitmap for the twelve supported columns; no
   placement matrix or per-item retained allocation reaches the page. */
typedef struct {
    uint16_t occupied[GRID_PLACEMENT_ROW_LIMIT];
    uint8_t columns;
    uint8_t rows;
    uint8_t explicit_columns;
    uint8_t explicit_rows;
    uint8_t column_origin;
    uint8_t row_origin;
    uint8_t cursor_row;
    uint8_t cursor_column;
    bool flow_column;
    bool dense;
} GridPlacementState;

typedef struct {
    uint8_t column;
    uint8_t column_span;
    uint8_t row;
    uint8_t row_span;
} GridItemPlacement;

/* Iterators retain two complete ComputedStyle values.  Keeping the active
   iterator and item in an explicitly bounded heap arena removes the largest
   objects from every recursive layout_block frame.  The union is safe because
   flat/grid and flex passes do not overlap within one block invocation. */
struct LayoutBlockScratch {
    ComputedStyle style;
    LineState line;
    PositionedBox descendant_positioned_box;
    FlexOrderPlan row_order;
    FlexOrderPlan column_order;
    union {
        struct {
            FlatItemIterator iterator;
            FlatItem item;
        } flat;
        struct {
            FlexItemIterator iterator;
            FlexItemIterator saved_iterator;
            FlatItem item;
            FlatItem lookahead_item;
        } flex;
        /* Margin collapsing is a third pass that never overlaps the flat or
           flex traversal of the same block: it runs before a child is laid
           out, so no layout_block frame occupies the depths it walks.  The
           fields are kept distinct per function rather than overlaid,
           because block_collapses_through is called at the same depth as
           collapsed_block_top_margin.  The top and bottom walks do share,
           and cannot nest: neither calls the other. */
        struct {
            ComputedStyle pseudo;
            ComputedStyle inline_child;
            ComputedStyle through_resolved;
            ComputedStyle through_child;
            FlatItemIterator iterator;
            FlatItem item;
            FlatItem last;
        } collapse;
    } traversal;
};

typedef struct {
    size_t count;
    size_t auto_main_margins;
    int basis;
    int grow;
} FlexLineMetrics;

struct LayoutNodeIndexEntry {
    lxb_dom_node_t *node;
    uint32_t index;
};

struct LayoutFocusIndexEntry {
    lxb_dom_node_t *node;
    uint32_t link_plus_one;
    uint32_t control_plus_one;
};

static inline size_t layout_pointer_hash(const void *pointer)
{
    uintptr_t value = (uintptr_t) pointer >> 3;
#if UINTPTR_MAX > UINT32_MAX
    value ^= value >> 33;
    value *= UINT64_C(0xff51afd7ed558ccd);
    value ^= value >> 33;
#else
    value ^= value >> 16;
    value *= UINT32_C(0x7feb352d);
    value ^= value >> 15;
#endif
    return (size_t) value;
}

/* CSS font-weight boundaries: at or above LAYOUT_BOLD_FACE_WEIGHT the bold
   face is selected; between LAYOUT_SYNTHETIC_WEIGHT and the bold boundary a
   synthetic emboldening pass runs when no bold face exists.  DrawCommand
   stores weights in ten-unit increments, so the encoded twin of the bold
   boundary is LAYOUT_BOLD_FACE_WEIGHT / 10. */
#define LAYOUT_BOLD_FACE_WEIGHT 650
#define LAYOUT_SYNTHETIC_WEIGHT 550

/* Tracing.  TILEFINCH_TRACE_* environment variables are read once per build
   into LayoutDocument.trace_flags; defining TILEFINCH_NO_TRACE compiles every
   trace body out entirely for the PSP profile. */
#ifndef TILEFINCH_NO_TRACE
#define TILEFINCH_TRACE_COMPILED_IN 1
#else
#define TILEFINCH_TRACE_COMPILED_IN 0
#endif

#define LAYOUT_TRACE_LAYOUT         (UINT32_C(1) << 0)
#define LAYOUT_TRACE_PAINT          (UINT32_C(1) << 1)
#define LAYOUT_TRACE_CLIP           (UINT32_C(1) << 2)
#define LAYOUT_TRACE_FLEX_TRANSLATE (UINT32_C(1) << 3)
#define LAYOUT_TRACE_LAYOUT_SLICES  (UINT32_C(1) << 4)
#define LAYOUT_TRACE_LAYOUT_PROFILE (UINT32_C(1) << 5)
#define LAYOUT_TRACE_SCROLL_WIDTH   (UINT32_C(1) << 6)

#define LAYOUT_TRACE(layout_document, FLAG) \
    (TILEFINCH_TRACE_COMPILED_IN != 0 \
     && ((layout_document)->trace_flags & LAYOUT_TRACE_##FLAG) != 0)

/* Cross-translation-unit prototypes.  These functions were file-local
   statics before the layout.c decomposition; they remain internal to
   the layout subsystem. */

AlignItems flex_item_alignment(const ComputedStyle *container, const ComputedStyle *item);
ComputedStyle layout_style_for_node(LayoutContext *context, lxb_dom_node_t *node, const ComputedStyle *parent);
DrawCommand *layout_add_command(LayoutDocument *layout, DrawCommand command);
bool layout_add_text_shadow_commands(
    LayoutContext *context, const ComputedStyle *style,
    const DrawCommand *text);
bool layout_insert_text_shadow_commands(
    LayoutContext *context, const ComputedStyle *style,
    const DrawCommand *text, size_t index, size_t *inserted);
bool layout_intern_gradient(LayoutDocument *layout, const StyleGradient *gradient, size_t *slot);
FlexLineMetrics flex_line_metrics(LayoutContext *context, FlexItemIterator *iterator, FlatItem *item, int content_width, int gap);
GeneratedPseudoFlow generated_pseudo_flow( LayoutContext *context, lxb_dom_node_t *node, const ComputedStyle *parent, PseudoElement pseudo, int width, int containing_height);
bool add_fixed_range(LayoutDocument *layout, size_t start, size_t end, size_t link_start, size_t link_end, size_t control_start, size_t control_end, int origin_y, int height, int inset, bool from_bottom);
bool add_node_box(LayoutDocument *layout, lxb_dom_node_t *node, int x, int y,
                  int width, int height, int client_width, int client_height,
                  int content_width, int content_height,
                  int padding_horizontal, int padding_vertical, bool clips_x,
                  bool clips_y, int clip_radius,
                  uint8_t overflow_clip_margin, int clip_inset_left,
                  int clip_inset_top, bool clip_only_x, bool clip_only_y,
                  uint8_t positioned_ancestor_distance,
                  bool cssom_geometry_authoritative,
                  size_t command_start, size_t command_end,
                  size_t scroll_command_start, size_t scroll_command_end,
                  size_t link_start, size_t link_end,
                  size_t control_start, size_t control_end);
LayoutNodeBox *layout_box_for_node_mutable(
    LayoutDocument *layout, const lxb_dom_node_t *node);
bool add_sticky_range(LayoutDocument *layout, size_t start, size_t end, int origin_y, int top);
bool attribute_is(lxb_dom_node_t *node, const char *name, const char *wanted);
bool layout_node_is_hidden_input(lxb_dom_node_t *node);
bool build_paint_order(LayoutDocument *layout, LayoutContext *context);
bool build_spatial_index(LayoutDocument *layout, LayoutContext *context);
bool draw_uses_bold_face(const DrawCommand *command);
bool flat_iterator_next(FlatItemIterator *iterator, FlatItem *item);
bool flex_iterator_next(FlexItemIterator *iterator, FlatItem *item);
bool flex_order_plan_build(FlexOrderPlan *plan, LayoutContext *context, lxb_dom_node_t *container, const ComputedStyle *style);
bool flow_inline(LayoutContext *context, lxb_dom_node_t *node, const ComputedStyle *parent, LineState *line, const char *link_url, size_t link_url_length, lxb_dom_node_t *link_node);
bool flow_subtree_fallback(LayoutContext *context, lxb_dom_node_t *root, const ComputedStyle *style, LineState *line, const char *inherited_link_url, size_t inherited_link_length, lxb_dom_node_t *inherited_link_node);
bool flow_text(LayoutContext *context, LineState *line, const char *text, size_t length, const ComputedStyle *style, const char *link_url, size_t link_url_length, lxb_dom_node_t *link_node);
int generated_inline_pseudo_width(LayoutContext *context, lxb_dom_node_t *node, const ComputedStyle *parent, PseudoElement pseudo);
bool flow_generated_inline_pseudo(LayoutContext *context, lxb_dom_node_t *node, const ComputedStyle *parent, PseudoElement pseudo, LineState *line, const char *link_url, size_t link_url_length, lxb_dom_node_t *link_node, bool *flowed);
bool generated_pseudo_is_flow_block(const ComputedStyle *style);
size_t list_marker_text(ListStyleType type, int position,
                        char *output, size_t capacity);
const char *layout_retain_generated_text(
    LayoutDocument *layout, const char *text, size_t length);
bool is_block_display(DisplayMode display);
bool layout_add_control(LayoutDocument *layout, int x, int y, int width, int height, ControlType type, lxb_dom_node_t *node);
ControlType layout_input_control_type(lxb_dom_node_t *node);
int layout_control_default_width(lxb_dom_node_t *node);
int layout_control_default_height(lxb_dom_node_t *node);
bool layout_paint_special_input(
    LayoutContext *context, lxb_dom_node_t *node,
    const ComputedStyle *style, int x, int y, int width, int height,
    bool *handled);
bool layout_paint_select_indicator(
    LayoutContext *context, lxb_dom_node_t *node,
    const ComputedStyle *style, int x, int y, int width, int height);
bool layout_paint_audio_control(
    LayoutContext *context, const ComputedStyle *style,
    int x, int y, int width, int height);
bool layout_add_link(LayoutDocument *layout, const DrawCommand *command,
                     size_t command_index, const char *url,
                     size_t url_length, lxb_dom_node_t *node);
bool layout_anonymous_text(LayoutContext *context, const FlatItem *item, lxb_dom_node_t *container, int x, int y, int width, int *bottom);
bool layout_batch_checkpoint(LayoutContext *context, size_t at, size_t count, size_t *checkpoint_at);
bool layout_batch_cooperate(LayoutContext *context, size_t work_units);
bool layout_block(LayoutContext *context, lxb_dom_node_t *node, const ComputedStyle *parent, int x, int y, int width, int containing_height, bool assigned_width, const PositionedBox *positioned_box, int *bottom);
bool layout_place_float(LayoutContext *context, lxb_dom_node_t *node,
                        const ComputedStyle *parent,
                        const ComputedStyle *style, LineState *line,
                        int containing_height);
int layout_collapsed_block_top_margin(
    LayoutContext *context, lxb_dom_node_t *node,
    const ComputedStyle *style);
bool layout_cooperate(LayoutContext *context, lxb_dom_node_t *node);
bool layout_insert_commands(LayoutContext *context, size_t index,
                            const DrawCommand *commands, size_t count);
bool layout_insert_command(LayoutContext *context, size_t index,
                           DrawCommand command);
bool layout_node_name_is(lxb_dom_node_t *node, const char *wanted);
bool layout_node_within(const lxb_dom_node_t *node, const lxb_dom_node_t *ancestor);
bool layout_positioned_command_escapes_clip(
    const LayoutDocument *layout, size_t command_index,
    const LayoutNodeBox *clip_box);
bool layout_tree_enter(LayoutContext *context, lxb_dom_node_t *node, const char *phase);
bool node_effectively_disabled(lxb_dom_node_t *node);
bool paint_pseudo(LayoutContext *context, lxb_dom_node_t *node, const ComputedStyle *parent, PseudoElement pseudo, int x, int y, int width, int height, size_t insertion_index, int forced_border_height);
bool resolve_computed_length(const Stylesheet *sheet, int value, bool percent, int reference, int *resolved);
bool style_maximum_width(const Stylesheet *sheet, const ComputedStyle *style, int containing_width, int *maximum);
void constrain_replaced_content_size(
    LayoutContext *context, lxb_dom_node_t *node,
    const ComputedStyle *parent, const ComputedStyle *style,
    int containing_width, int containing_height,
    bool width_definite, bool height_definite,
    int *width, int *height);
bool style_uses_bold_face(const ComputedStyle *style);
bool style_uses_synthetic_weight(const FontSet *fonts, const WebFontSet *web_fonts, const ComputedStyle *style, const FontFace *face);
const TableTracks *table_tracks_for_row( LayoutContext *context, lxb_dom_node_t *row, int available_width);
bool table_cell_uses_collapsed_geometry(
    const LayoutContext *context, const lxb_dom_node_t *cell);
const TableTracks *table_tracks_for_table( LayoutContext *context, lxb_dom_node_t *table, const ComputedStyle *table_style, int available_width);
int table_intrinsic_width(LayoutContext *context, lxb_dom_node_t *table,
                          const ComputedStyle *table_style,
                          int available_width);
const char *first_text_data(lxb_dom_node_t *node, size_t *length);
const char *ordered_list_marker(unsigned position, size_t *length);
int distribute_flex_rows(LayoutContext *context, lxb_dom_node_t *container, const ComputedStyle *style, const FlexOrderPlan *order_plan, int declared, int *stretch_free, size_t *stretch_lines);
int flex_child_basis(LayoutContext *context, const FlatItem *item, int content_width);
int flex_child_row_minimum(LayoutContext *context, const FlatItem *item, int content_width, bool css_table_row);
int grid_item_minimum_contribution(
    LayoutContext *context, const FlatItem *item, int available,
    int start, int span, const uint8_t track_types[GRID_TRACK_LIMIT]);
int intrinsic_min_text_width(LayoutContext *context, lxb_dom_node_t *node, const ComputedStyle *parent, int limit);
int intrinsic_min_text_width_ignoring_own_width(
    LayoutContext *context, lxb_dom_node_t *node,
    const ComputedStyle *parent, int limit);
void intrinsic_text_widths(
    LayoutContext *context, lxb_dom_node_t *node,
    const ComputedStyle *parent, int limit,
    int *maximum, int *minimum);
bool intrinsic_grid_subgrid_column_requirements(
    LayoutContext *context, lxb_dom_node_t *node,
    const ComputedStyle *style, int columns, int rows, int limit,
    int minimums[GRID_TRACK_LIMIT], int maximums[GRID_TRACK_LIMIT]);
int intrinsic_text_width(LayoutContext *context, lxb_dom_node_t *node, const ComputedStyle *parent, int limit);
int intrinsic_positioned_width(LayoutContext *context, lxb_dom_node_t *node,
                               const ComputedStyle *parent, int limit);
int layout_fixed_scale_floor(int value, int numerator, int denominator);
int measured_text_width_fixed(const FontFace *face, FontFamily metric_family, const char *text, size_t length, int font_size_fixed, bool synthetic_bold, bool metric_bold, int scale, int letter_spacing);
int measured_text_width_fixed_mode(const FontFace *face, FontFamily metric_family, const char *text, size_t length, int font_size_fixed, bool synthetic_bold, bool metric_bold, int scale, int letter_spacing, bool kerning);
int measured_text_width(const FontFace *face, FontFamily metric_family, const char *text, size_t length, int font_size_fixed, bool synthetic_bold, bool metric_bold, int scale, int letter_spacing);
bool layout_add_replaced_alt_text(
    LayoutContext *context, lxb_dom_node_t *node,
    const ComputedStyle *style, int x, int y, int width, int height);
int resolve_declared_length(const Stylesheet *sheet, int value, bool percent, int reference);
int constrain_border_box_width(
    LayoutContext *context, lxb_dom_node_t *node,
    const ComputedStyle *parent, const ComputedStyle *style,
    int containing_width, int candidate, bool *has_maximum);
int root_scroll_width_after_clipping(LayoutDocument *layout, int viewport_width);
int style_content_height(const Stylesheet *sheet, const ComputedStyle *style, int width_reference, int containing_height);
int style_minimum_width(const Stylesheet *sheet, const ComputedStyle *style, int containing_width);
int style_pixel_height(const Stylesheet *sheet, const ComputedStyle *style, int reference);
lxb_dom_node_t *select_display_option(lxb_dom_node_t *select);
size_t table_cell_span(lxb_dom_node_t *cell);
size_t table_cell_row_span(lxb_dom_node_t *cell);
bool table_cell_placement(const TableTracks *tracks, lxb_dom_node_t *cell,
                          size_t *column, size_t *column_span,
                          size_t *row_span);
size_t utf8_character_length(const char *text, size_t available);
size_t utf8_codepoints(const char *text, size_t length);
size_t utf8_line_segment_length(const char *text, size_t available,
                                bool keep_cjk_together, bool hyphens_none,
                                bool *discard);
uint16_t alpha_opacity_scale(uint8_t alpha);
uint32_t blend_color_over(uint32_t foreground, uint8_t alpha, uint32_t background);
uint8_t draw_font_weight(const ComputedStyle *style);
uint8_t text_decoration_bits(const ComputedStyle *style);
void align_flex_row_items(LayoutContext *context, lxb_dom_node_t *container, const ComputedStyle *style, const FlexOrderPlan *order_plan, int declared, int stretch_free, size_t stretch_lines);
bool apply_visual_range(LayoutContext *context, lxb_dom_node_t *node,
                        size_t command_start, size_t link_start,
                        size_t control_start, const ComputedStyle *style,
                        bool flex_or_grid_item);
bool layout_record_visibility_range(
    LayoutContext *context, size_t command_start, size_t link_start,
    size_t control_start, bool hidden);
bool layout_resolve_visibility(LayoutContext *context);
void clear_line_floats(LineState *line, ClearMode clear);
void flat_iterator_init(FlatItemIterator *iterator, LayoutContext *context, lxb_dom_node_t *container, const ComputedStyle *container_style);
void flat_text_link(lxb_dom_node_t *node, lxb_dom_node_t *container, const char **url, size_t *url_length, lxb_dom_node_t **link_node);
void flex_iterator_init(FlexItemIterator *iterator, LayoutContext *context, lxb_dom_node_t *container, const ComputedStyle *style, const FlexOrderPlan *plan);
void flex_order_plan_destroy(FlexOrderPlan *plan);
void grid_placement_init(GridPlacementState *state, int columns, int rows,
                         const ComputedStyle *container);
bool grid_place_item(GridPlacementState *state, const ComputedStyle *style,
                     GridItemPlacement *placement);
int grid_required_columns(const ComputedStyle *style, int current_columns);
void layout_finish_work_slice(LayoutContext *context);
void layout_flush_line(LineState *line);
void layout_scale_range(LayoutDocument *layout, size_t command_start, size_t link_start, size_t control_start, size_t node_box_start, int origin_x_twice, int origin_y_twice, uint8_t scale_q6, lxb_dom_node_t *source);
void layout_rotate_range_quadrants(LayoutDocument *layout,
                                   size_t command_start, size_t link_start,
                                   size_t control_start,
                                   size_t node_box_start,
                                   int origin_x_twice, int origin_y_twice,
                                   uint8_t quadrants,
                                   lxb_dom_node_t *source);
void layout_transform_origin_twice(
    const Stylesheet *sheet, const ComputedStyle *style,
    int box_x, int box_y, int box_width, int box_height,
    int *origin_x_twice, int *origin_y_twice);
void layout_transform_command_span(
    LayoutDocument *layout, size_t command_start, size_t command_end,
    int origin_x_twice, int origin_y_twice, uint8_t scale_q6,
    uint8_t rotate_quadrants, int dx, int dy);
void layout_style_for_node_into(LayoutContext *context, lxb_dom_node_t *node, const ComputedStyle *parent, ComputedStyle *result);
void layout_translate_range(LayoutDocument *layout, size_t command_start, size_t link_start, size_t control_start, size_t node_box_start, int dx, int dy, const char *phase, lxb_dom_node_t *source);
void layout_tree_leave(LayoutContext *context);
void layout_tree_note_fallback(LayoutContext *context, lxb_dom_node_t *node, const char *phase);
void line_cursor_set(LineState *line, int x);
int layout_single_text_advance_fixed(
    LayoutContext *context, lxb_dom_node_t *node,
    const ComputedStyle *style, int used_width);
void line_finish_vertical(LineState *line);
void make_float_slot(LineState *line);
void resolve_padding(const Stylesheet *sheet, ComputedStyle *style, int containing_inline_width);
void resolve_margin(const Stylesheet *sheet, ComputedStyle *style, int containing_inline_width);
void reverse_flex_column_items(LayoutContext *context, lxb_dom_node_t *container, const ComputedStyle *style, const FlexOrderPlan *order_plan, int content_top, int content_height);
bool place_wrapped_flex_columns(LayoutContext *context, lxb_dom_node_t *container, const ComputedStyle *style, const FlexOrderPlan *order_plan, int content_x, int content_width, int content_top, int content_height, size_t item_count);
void reverse_flex_cross_items(LayoutContext *context, lxb_dom_node_t *container, const ComputedStyle *style, const FlexOrderPlan *order_plan, int content_top, int content_height);
void reverse_flex_row_items(LayoutContext *context, lxb_dom_node_t *container, const ComputedStyle *style, const FlexOrderPlan *order_plan, int content_x, int content_width);
void trace_flex_sizing(LayoutContext *context, const char *phase, lxb_dom_node_t *container, lxb_dom_node_t *item_node, const ComputedStyle *item_style, int content_width, int total_basis, int remaining, int cursor_x, int basis, int used_width);
void trace_flex_translation(LayoutContext *context, const char *phase, lxb_dom_node_t *container, lxb_dom_node_t *item, int alignment, int content_x, int content_width, int box_x, int box_width, int target_x, int dx);
void translate_node_subtree(LayoutDocument *layout, lxb_dom_node_t *node, int dx, int dy);
void translate_table_cell_contents(LayoutDocument *layout, lxb_dom_node_t *node, int dy);
void update_float_bounds(LineState *line);
void update_float_band(LineState *line);
void update_float_bounds_for_span(LineState *line, int height);

#endif /* TILEFINCH_LAYOUT_INTERNAL_H */
