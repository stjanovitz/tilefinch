/* Host-only validation, compiled out of the PSP and trace-free builds
   (included by layout_reuse.c under !TILEFINCH_NO_TRACE).

   TILEFINCH_TRACE_INVALIDATION_VERIFY=1: every computed style a build takes
   from retained state is re-resolved straight from the sheet (reuse cache
   and retained matched-rule lists bypassed) and compared byte for byte, the
   borrowed strings by what they spell:

     - a reuse-cache hit (from=cache): the entry survived every
       invalidation since it was stored, so a difference is a change the
       invalidation missed;
     - a miss resolved through the retained matched-rule lists
       (from=lists): a list the token invalidation kept stale;
     - a ::before/::after/::placeholder the cache remembers as absent
       (from=pseudo) that now generates content.

   The bar is the page's cache: builds from the load commit on and every
   mutation-driven build. A streaming preview's cache (the one cache that
   withholds retained matched-rule lists while the parser still appends,
   until the commit adopts it) is checked too, but its differences count
   apart as `streaming`, outside `mismatches`.

   Each difference prints (the first eight per period)

     invalidation-verify-diff from=cache phase=page node=html>body>div#a.b>span:2 fields=color,display

   and every build period (layout_reuse_cache_prepare to the next one, and
   the cache's teardown) prints a summary when it checked anything:

     invalidation-verify checked=N cache=H lists=L pseudo=P mismatches=M streaming=S

   The check resolves every served style a second time, so it is for proving
   equality, never for timing. */
#include "style_cache_internal.h"

static struct {
    int enabled;  /* -1 until the environment is read */
    bool active;
    size_t checked, cache, lists, pseudo, mismatches, streaming, printed;
} layout_verify = {-1, false, 0, 0, 0, 0, 0, 0, 0};

static bool layout_verify_enabled(void)
{
    if (layout_verify.enabled < 0)
        layout_verify.enabled =
            getenv("TILEFINCH_TRACE_INVALIDATION_VERIFY") != NULL;
    return layout_verify.enabled > 0 && !layout_verify.active;
}

static void layout_verify_summary(void)
{
    if (layout_verify.enabled <= 0 || layout_verify.checked == 0) return;
    fprintf(stderr,
            "invalidation-verify checked=%zu cache=%zu lists=%zu pseudo=%zu "
            "mismatches=%zu streaming=%zu\n", layout_verify.checked,
            layout_verify.cache, layout_verify.lists, layout_verify.pseudo,
            layout_verify.mismatches, layout_verify.streaming);
    layout_verify.checked = layout_verify.cache = layout_verify.lists = 0;
    layout_verify.pseudo = layout_verify.mismatches = 0;
    layout_verify.streaming = 0;
    layout_verify.printed = 0;
}

/* `html>body>div#id.class>span:3`: tag, id, first class, and the element
   index among its siblings when it has no id; the top is cut first. */
static void layout_verify_path(lxb_dom_node_t *node, char *out, size_t size)
{
    enum { DEPTH = 64 };
    lxb_dom_node_t *chain[DEPTH];
    size_t depth = 0;
    bool cut = false;
    for (lxb_dom_node_t *at = node; at != NULL
         && at->type == LXB_DOM_NODE_TYPE_ELEMENT; at = at->parent) {
        if (depth == DEPTH) {
            cut = true;
            break;
        }
        chain[depth++] = at;
    }
    size_t used = 0;
    out[0] = '\0';
    if (cut) used += (size_t) snprintf(out, size, "...");
    while (depth != 0 && used + 1 < size) {
        lxb_dom_node_t *at = chain[--depth];
        size_t name_length = 0, id_length = 0, class_length = 0;
        const char *name = document_element_name(at, &name_length);
        const char *id = document_attribute(at, "id", &id_length);
        const char *klass = document_attribute(at, "class", &class_length);
        size_t first = 0;
        while (klass != NULL && first < class_length
               && klass[first] != ' ') first++;
        if (first > 24) first = 24;
        size_t index = 0;
        for (lxb_dom_node_t *sibling = at->parent == NULL ? NULL
             : at->parent->first_child; sibling != NULL && sibling != at;
             sibling = sibling->next)
            index += sibling->type == LXB_DOM_NODE_TYPE_ELEMENT;
        int wrote = snprintf(
            out + used, size - used, "%s%.*s%s%.*s%s%.*s", used ? ">" : "",
            (int) (name == NULL ? 0 : (name_length > 16 ? 16 : name_length)),
            name == NULL ? "" : name, id != NULL && id_length ? "#" : "",
            (int) (id == NULL ? 0 : (id_length > 32 ? 32 : id_length)),
            id == NULL ? "" : id, first ? "." : "", (int) first,
            klass == NULL ? "" : klass);
        if (wrote < 0) break;
        used += (size_t) wrote < size - used ? (size_t) wrote
                                             : size - used - 1;
        if ((id == NULL || id_length == 0) && at->parent != NULL
            && at->parent->type == LXB_DOM_NODE_TYPE_ELEMENT
            && used + 1 < size) {
            wrote = snprintf(out + used, size - used, ":%zu", index + 1u);
            if (wrote < 0) break;
            used += (size_t) wrote < size - used ? (size_t) wrote
                                                 : size - used - 1;
        }
    }
}

static bool layout_verify_strings_equal(const char *a, const char *b,
                                        size_t length)
{
    if (a == b) return true;
    if (a == NULL || b == NULL) return false;
    return strncmp(a, b, length) == 0;
}

/* The ComputedStyle members, named for the report. Members not listed
   still compare (the whole struct does); their bytes report as `?+N`. */
#define LAYOUT_VERIFY_FIELDS(F, B) \
    F(display) F(color) F(color_alpha) F(background) F(background_alpha) \
    B(has_background) B(background_current_color) \
    F(text_decoration_state) F(background_image_kind) F(background_fit) \
    F(background_width) F(background_height) F(background_position_x) \
    F(background_position_y) F(background_size_flags) F(object_fit) \
    F(font_scale) F(line_height_factor) F(font_size) F(font_size_fraction) \
    F(font_size_unit) F(root_font_size) F(root_font_size_fraction) \
    F(font_family) F(font_weight) F(font_bold) F(font_italic) \
    F(vertical_align) F(word_spacing) F(line_height) F(text_indent) \
    F(text_align) F(float_mode) F(clear_mode) F(white_space_mode) \
    F(overflow_wrap) F(word_break_mode) F(letter_spacing_fixed) F(margin) \
    B(margin_top_auto) B(margin_left_auto) B(margin_right_auto) \
    B(margin_bottom_auto) B(margin_top_percent) B(margin_left_percent) \
    B(margin_right_percent) B(margin_bottom_percent) F(padding) F(border) \
    F(border_color) F(border_alpha) F(line_clamp) F(outline_state) \
    F(border_radius) F(gap) F(row_gap) F(grid_columns) \
    F(grid_min_column_width) F(flex_direction) F(flex_wrap) \
    F(flex_wrap_reverse) F(overflow_clip_margin) F(clip_path_state) \
    F(grid_placement) F(flex_grow) F(order) F(flex_shrink) \
    F(has_flex_basis) F(flex_basis_percent) F(flex_basis) \
    F(flex_basis_offset) F(has_width) F(width_max_content) \
    F(width_percent) F(min_width_auto) F(min_width_percent) \
    F(max_width_percent) F(min_height_auto) F(width) F(width_offset) \
    F(has_height) F(height_percent) F(min_height_percent) \
    F(max_height_percent) F(height) F(aspect_width) F(aspect_height) \
    F(min_width) F(min_width_offset) F(min_height) F(max_width) \
    F(max_width_offset) F(max_height) F(box_sizing_border_box) \
    F(align_items) F(justify_items) F(align_self) F(align_content) \
    F(justify_content) F(list_style_none) F(generated_content) \
    F(generated_text_length) F(table_layout_fixed) F(outline_color) \
    F(generated_attr_length) F(outline_alpha) F(border_color_set) \
    F(generated_image) F(has_top) F(has_right) F(has_bottom) F(has_left) \
    F(top) F(right) F(bottom) F(left) F(inset_percent_mask) B(hidden) \
    B(clip_rect_empty) B(out_of_flow) B(relative_position) \
    B(sticky_position) B(fixed_position) B(overflow_x_scroll) \
    B(overflow_y_scroll) B(overflow_x_clip_only) B(overflow_y_clip_only) \
    B(has_z_index) B(grid_auto_column_type) B(grid_auto_row_type) \
    B(visibility_hidden) B(shadow_host) B(overflow_y_hidden) \
    F(grid_auto_column_value) F(grid_auto_row_value) \
    F(grid_auto_column_second) F(grid_auto_row_second) F(z_index) \
    F(opacity) B(has_transform) B(has_perspective) B(has_filter) \
    B(has_layout_containment) B(will_change_transform) \
    B(pointer_events_none) B(table_border_collapse) B(list_item) \
    B(containing_block_reserved) B(individual_rotate_quadrants) \
    B(resize_mode) B(touch_action) B(content_visibility) \
    F(transform_scale_q14) F(transform_x_percent) F(transform_y_percent) \
    F(transform_x) F(transform_y) F(appearance) F(text_transform) \
    F(paint_stack_state) F(filter_code) F(counter_reset_id) \
    F(counter_increment_id) F(counter_set_id) B(list_style_type) \
    B(list_style_inside) B(generated_expression) B(unicode_bidi) \
    B(scrollbar_gutter_stable) F(object_position_x) F(object_position_y)

/* The style with its borrowed strings cleared (they compare by value). */
static ComputedStyle layout_verify_bare(const ComputedStyle *style)
{
    ComputedStyle bare = *style;
    bare.background_image = NULL;
    bare.mask_image = NULL;
    bare.generated_text = NULL;
    if (bare.generated_attr_length != UINT8_MAX /* scalar sentinel */)
        bare.generated_attr = NULL;
    return bare;
}

/* The differing members of `a` and `b`, comma-separated, into `out`. */
static void layout_verify_fields(const ComputedStyle *a,
                                 const ComputedStyle *b, char *out,
                                 size_t size)
{
    ComputedStyle left = layout_verify_bare(a), right = layout_verify_bare(b);
    const unsigned char *x = (const unsigned char *) &left;
    const unsigned char *y = (const unsigned char *) &right;
    unsigned char named[sizeof(ComputedStyle)];
    memset(named, 0, sizeof(named));
    size_t used = 0;
    out[0] = '\0';
#define LAYOUT_VERIFY_NOTE(name) do { \
        if (used + 1 < size) { \
            int wrote = snprintf(out + used, size - used, "%s%s", \
                                 used ? "," : "", name); \
            if (wrote > 0) used += (size_t) wrote < size - used \
                ? (size_t) wrote : size - used - 1; \
        } \
    } while (0)
#define LAYOUT_VERIFY_FIELD(member) { \
        size_t at = offsetof(ComputedStyle, member); \
        size_t span = sizeof(left.member); \
        memset(named + at, 1, span); \
        if (memcmp(x + at, y + at, span) != 0) \
            LAYOUT_VERIFY_NOTE(#member); \
    }
#define LAYOUT_VERIFY_BITS(member) { \
        ComputedStyle mask; \
        memset(&mask, 0, sizeof(mask)); \
        mask.member = mask.member - 1; /* every bit of the field */ \
        const unsigned char *m = (const unsigned char *) &mask; \
        bool differs = false; \
        for (size_t i = 0; i < sizeof(mask); i++) { \
            named[i] |= m[i] != 0; \
            differs |= ((x[i] ^ y[i]) & m[i]) != 0; \
        } \
        if (differs) LAYOUT_VERIFY_NOTE(#member); \
    }
    LAYOUT_VERIFY_FIELDS(LAYOUT_VERIFY_FIELD, LAYOUT_VERIFY_BITS)
    for (size_t i = 0; i < sizeof(ComputedStyle); i++) {
        if (named[i] || x[i] == y[i]) continue;
        char unnamed[16];
        snprintf(unnamed, sizeof(unnamed), "?+%zu", i);
        LAYOUT_VERIFY_NOTE(unnamed);
    }
    if (!layout_verify_strings_equal(a->background_image,
                                     b->background_image, 4096))
        LAYOUT_VERIFY_NOTE("background_image");
    if (!layout_verify_strings_equal(a->mask_image, b->mask_image, 4096))
        LAYOUT_VERIFY_NOTE("mask_image");
    if (!layout_verify_strings_equal(a->generated_text, b->generated_text,
                                     a->generated_text_length))
        LAYOUT_VERIFY_NOTE("generated_text");
    if (a->generated_attr_length != UINT8_MAX
        && !layout_verify_strings_equal(a->generated_attr, b->generated_attr,
                                        a->generated_attr_length))
        LAYOUT_VERIFY_NOTE("generated_attr");
#undef LAYOUT_VERIFY_BITS
#undef LAYOUT_VERIFY_FIELD
#undef LAYOUT_VERIFY_NOTE
}

static bool layout_verify_equal(const ComputedStyle *a,
                                const ComputedStyle *b)
{
    ComputedStyle left = layout_verify_bare(a), right = layout_verify_bare(b);
    return memcmp(&left, &right, sizeof(left)) == 0
        && layout_verify_strings_equal(a->background_image,
                                       b->background_image, 4096)
        && layout_verify_strings_equal(a->mask_image, b->mask_image, 4096)
        && layout_verify_strings_equal(a->generated_text, b->generated_text,
                                       a->generated_text_length)
        && (a->generated_attr_length == UINT8_MAX
            || layout_verify_strings_equal(a->generated_attr,
                                           b->generated_attr,
                                           a->generated_attr_length));
}

/* A streaming preview's cache, until the page adopts it. */
static bool layout_verify_streaming(const LayoutReuseCache *cache)
{
    return cache != NULL && cache->streaming_preview;
}

static void layout_verify_report(const char *from, bool streaming,
                                 lxb_dom_node_t *node, const char *fields)
{
    if (streaming) layout_verify.streaming++;
    else layout_verify.mismatches++;
    if (layout_verify.printed++ >= 8) return;
    char path[512];
    layout_verify_path(node, path, sizeof(path));
    fprintf(stderr, "invalidation-verify-diff from=%s phase=%s node=%s "
            "fields=%s\n", from, streaming ? "streaming" : "page", path,
            fields);
}

static void layout_resolve_canonical_style(
    const Stylesheet *sheet, const FontSet *fonts,
    const WebFontSet *web_fonts, lxb_dom_node_t *node,
    const ComputedStyle *parent, ComputedStyle *result,
    bool *font_dependent, StyleRetainedMatches *matches);

/* `served` (from the cache, or resolved through retained lists) against a
   resolution straight from the sheet. */
static void layout_verify_served(const LayoutReuseCache *cache,
                                 const Stylesheet *sheet,
                                 const FontSet *fonts, lxb_dom_node_t *node,
                                 const ComputedStyle *parent,
                                 const ComputedStyle *served, bool hit)
{
    if (sheet == NULL || node == NULL || !layout_verify_enabled()) return;
    layout_verify.active = true;
    Stylesheet *mutable_sheet = (Stylesheet *) sheet;
    uint64_t names = mutable_sheet->variable_read_names;
    StyleRetainedMatches *previous = style_retained_matches_attach(sheet,
                                                                   NULL);
    ComputedStyle fresh;
    bool font_dependent = false;
    uint32_t consults = style_container_consults(sheet);
    layout_resolve_canonical_style(sheet, fonts,
                                   stylesheet_web_font_set(sheet), node,
                                   parent, &fresh, &font_dependent, NULL);
    bool transient = style_container_log_active(sheet)
        && style_container_consults(sheet) != consults;
    (void) style_retained_matches_attach(sheet, previous);
    mutable_sheet->variable_read_names = names;
    layout_verify.active = false;
    if (transient || style_selector_cooperation_cancelled(sheet)) return;
    layout_verify.checked++;
    if (hit) layout_verify.cache++;
    else layout_verify.lists++;
    if (layout_verify_equal(served, &fresh)) return;
    char fields[384];
    layout_verify_fields(served, &fresh, fields, sizeof(fields));
    layout_verify_report(hit ? "cache" : "lists",
                         layout_verify_streaming(cache), node, fields);
}

void layout_reuse_verify_pseudo_absent(const LayoutReuseCache *cache,
                                       const Stylesheet *sheet,
                                       lxb_dom_node_t *node,
                                       PseudoElement pseudo,
                                       const ComputedStyle *parent)
{
    if (sheet == NULL || node == NULL || !layout_verify_enabled()) return;
    layout_verify.active = true;
    Stylesheet *mutable_sheet = (Stylesheet *) sheet;
    uint64_t names = mutable_sheet->variable_read_names;
    StyleRetainedMatches *previous = style_retained_matches_attach(sheet,
                                                                   NULL);
    uint32_t consults = style_container_consults(sheet);
    ComputedStyle fresh = style_for_layout_pseudo(sheet, node, pseudo,
                                                  parent);
    /* While a one-pass container log runs, an answer may move with the
       live boxes; the log re-checks every answer and a moved one rebuilds
       the pass, clearing the absences (layout_reuse_container_state_changed). */
    bool transient = style_container_log_active(sheet)
        && style_container_consults(sheet) != consults;
    (void) style_retained_matches_attach(sheet, previous);
    mutable_sheet->variable_read_names = names;
    layout_verify.active = false;
    if (transient || style_selector_cooperation_cancelled(sheet)) return;
    layout_verify.checked++;
    layout_verify.pseudo++;
    if (fresh.generated_content)
        layout_verify_report("pseudo", layout_verify_streaming(cache), node,
                             "generated_content");
}
