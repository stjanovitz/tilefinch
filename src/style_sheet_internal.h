#ifndef TILEFINCH_STYLE_SHEET_INTERNAL_H
#define TILEFINCH_STYLE_SHEET_INTERNAL_H

/* Stylesheet and cascade internals: the modern and sparse property gates,
   text chunks and web-font families, cascade levels, the resolve scratch,
   deferred declaration programs and revert masks, the custom-rule limits,
   tracing, and the declaration/sheet prototypes (style_properties.c,
   style_sheet.c, style_compat_reveal.c). Part of style_internal.h. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "tilefinch/work_vector.h"
#include "style_cache_internal.h"
#include "style_scopes.h"

enum {
    STYLE_MODERN_USER_SELECT = 1u << 0,
    STYLE_MODERN_TOUCH_ACTION = 1u << 1,
    STYLE_MODERN_TEXT_SIZE_ADJUST = 1u << 2,
    STYLE_MODERN_RESIZE = 1u << 3,
    STYLE_MODERN_TEXT_WRAP = 1u << 4,
    STYLE_MODERN_TRANSLATE = 1u << 5,
    STYLE_MODERN_ROTATE = 1u << 6,
    STYLE_MODERN_SCALE = 1u << 7,
    STYLE_MODERN_ISOLATION = 1u << 8,
    STYLE_MODERN_LOGICAL_RADIUS = 1u << 9,
    STYLE_MODERN_TYPOGRAPHY = 1u << 10,
    STYLE_MODERN_MIX_BLEND = 1u << 11,
    STYLE_MODERN_BACKDROP_FILTER = 1u << 12,
    STYLE_MODERN_BACKFACE_VISIBILITY = 1u << 13,
    STYLE_MODERN_TRANSFORM_STYLE = 1u << 14,
    STYLE_MODERN_COLOR_SCHEME = 1u << 15,
    STYLE_MODERN_BORDER_IMAGE = 1u << 16
};

enum {
    STYLE_MODERN_TYPOGRAPHY_HYPHENS = 1u << 0,
    STYLE_MODERN_TYPOGRAPHY_TAB_SIZE = 1u << 1,
    STYLE_MODERN_TYPOGRAPHY_KERNING = 1u << 2,
    STYLE_MODERN_TYPOGRAPHY_TEXT_RENDERING = 1u << 3
};

/* Records the sparse declarations of one style attribute in the sheet's
   monotonic presence summary: the modern resolver bits and the layout
   gates (multicolumn, cursor, scroll). Style resolution calls it for each
   element's attribute before that element's modern pass, which makes it
   the one route for every way an attribute is written (markup, setAttribute,
   cssText, CSSOM, including parse-time scripts whose sheet is later
   replaced). A bit can therefore turn on midway through a style pass. That
   is safe because the bits are pure gates: with a bit on, an element that
   declares none of its properties, and whose ancestors declare none,
   resolves exactly as with the bit off (an absent text-size-adjust leaves
   font_size_unit 0, an absent text-wrap or tab-size inherits the parent's
   default, user-select resets to auto, and the rest change nothing), and
   ancestors resolve before descendants. So every element resolved before
   the first declaring element is unaffected by the late bit, and that
   element and everything after it see it. A new resolver branch must keep
   that property; the layout test "modern properties set only by style
   attributes" compares such a pass with one whose bits were all preset. */
void style_sparse_note_inline(const Stylesheet *sheet, const char *text,
                              size_t length);

struct StyleTextChunk {
    StyleTextChunk *next;
    size_t used;
    size_t capacity;
    char data[];
};

typedef struct {
    char *references[STYLE_WEB_FONT_SOURCE_LIMIT];
    char *source_bases[STYLE_WEB_FONT_SOURCE_LIMIT];
    char source_referrer_policies[STYLE_WEB_FONT_SOURCE_LIMIT]
                                 [STYLE_WEB_FONT_REFERRER_POLICY_CAPACITY];
    size_t source_count;
    uint8_t text_priority[STYLE_WEB_FONT_SOURCE_LIMIT];
} StyleWebFontFaceSource;

typedef struct {
    char name[STYLE_WEB_FONT_NAME_CAPACITY];
    union {
        struct { StyleWebFontFaceSource regular; StyleWebFontFaceSource bold; };
        StyleWebFontFaceSource faces[TILEFINCH_WEB_FONT_WEIGHT_LIMIT];
    };
} StyleWebFontFamily;

struct StyleWebFonts {
    WebFontSet loaded;
    StyleWebFontFamily families[TILEFINCH_WEB_FONT_FAMILY_LIMIT];
    size_t family_count;
    size_t retained_source_bytes;
    StylesheetWebFontStats stats;
};

enum {
    CASCADE_AUTHOR_NORMAL,
    CASCADE_USER_NORMAL,
    CASCADE_AUTHOR_IMPORTANT,
    CASCADE_USER_IMPORTANT
};

#define STYLE_VARIABLE_RESOLVING_LIMIT 4u
#define STYLE_VARIABLE_SUBSTITUTION_LIMIT 256u

struct StyleResolveScratch {
    /* Class-token scopes in progress: class-token sets (on the Stylesheet)
       are valid only while this is non-zero. */
    uint32_t class_tokens_depth;
    /* Image-source provenance for the declarations currently being
       parsed (external stylesheet base URL / referrer policy / slot). */
    const char *current_image_source_base;
    const char *current_image_source_referrer_policy;
    uint8_t current_image_source_slot;
    /* The node currently being resolved and its font-relative bases,
       consulted by var()/em/rem reparsing. */
    lxb_dom_node_t *resolution_node;
    PseudoElement resolution_pseudo;
    /* Custom-property declarations whose var() references are being
       resolved (innermost last). A nested lookup of one of them is a
       reference cycle; see variable_resolve_at_declaration. */
    struct {
        const lxb_dom_node_t *node;
        PseudoElement pseudo;
        const char *name;
        size_t name_length;
    } variable_resolving[STYLE_VARIABLE_RESOLVING_LIMIT];
    unsigned variable_resolving_count;
    /* var() expansion work under the outermost resolution in progress;
       see lookup_variable. */
    unsigned variable_work_nesting;
    unsigned variable_substitutions;
    int font_resolution_parent;
    int font_resolution_inherited;
    int font_resolution_root;
    bool font_resolution_active;
    bool font_resolution_locked;
    int font_ch_basis;
    bool font_ch_basis_active;
    bool font_ch_pending;
    bool font_relative_applied;
    bool non_font_relative_applied;
    bool logical_applied;
    bool logical_axes_locked;
    bool logical_axes_mismatch;
    uint8_t logical_axes;
    /* Transient and owned by one layout build, never by the retained sheet,
       or lent by `variable_cache_lease` (which may create it on demand) for
       the lease owner's resolutions. */
    struct StyleVariableCache *variable_cache;
    struct StyleVariableCacheLease *variable_cache_lease;
    struct StyleAncestorBloomCache *ancestor_bloom_cache;
    struct StyleContainerState *container_states;
    struct StyleContainerMatchCacheEntry *container_match_cache;
    size_t container_state_capacity;
    size_t container_state_count;
    size_t container_match_cache_capacity;
    int container_inline_basis;
    int container_block_basis;
    bool container_basis_active;
    /* Container-query evaluations of one probe layout pass, re-checked
       against the geometry that pass measured (style_queries.c). Held by
       pointer: nested resolutions save and restore this whole struct, and
       must not roll the log back. */
    struct StyleContainerLog *container_log;
    /* Retained matched-rule table attached by the layout reuse owner for one
       element resolution, plus the element currently being recorded. */
    struct StyleRetainedMatches *retained_matches;
    uint16_t retained_pending_rules[STYLE_RETAINED_MATCH_RULE_LIMIT];
    uint8_t retained_pending_count;
    bool retained_pending_active;
    bool retained_pending_valid;
    /* The adopted-scope mask of the element last asked about, valid in the
       class-token scope it was computed in (the DOM cannot change there). */
    const lxb_dom_node_t *adopted_scope_node;
    uint32_t adopted_scope_tokens_scope;
    uint64_t adopted_scope_mask;
};

#define STYLE_DEFERRED_FONT_RELATIVE UINT16_C(1)
#define STYLE_DEFERRED_NON_FONT_RELATIVE UINT16_C(2)
#define STYLE_DEFERRED_LOGICAL UINT16_C(4)
#define STYLE_DEFERRED_DYNAMIC UINT16_C(8)
#define STYLE_DEFERRED_CH UINT16_C(16)
/* `all:inherit` changes the cascade baseline rather than one property bit.
   Retain it on the compiled declaration without growing StyleDeclaration. */
#define STYLE_DECLARATION_ALL_INHERIT UINT16_C(32)
/* The deferred text has no "font-size" at all, so the per-element scan for
   an absolute font basis (declaration_absolute_font_basis) cannot find one.
   Set only where proven; absent means scan. */
#define STYLE_DEFERRED_NO_FONT_SIZE UINT16_C(64)
_Static_assert((STYLE_DEFERRED_NO_FONT_SIZE
                & (STYLE_DEFERRED_FONT_RELATIVE
                   | STYLE_DEFERRED_NON_FONT_RELATIVE
                   | STYLE_DEFERRED_LOGICAL | STYLE_DEFERRED_DYNAMIC
                   | STYLE_DEFERRED_CH | STYLE_DECLARATION_ALL_INHERIT))
                   == 0, "deferred declaration flags must not overlap");

/* A declaration's values encode as (first word, word count) byte pairs,
   each followed by that many nonzero 32-bit words; absent words are zero.
   The worst case alternates zero and nonzero words. */
#define STYLE_DECLARATION_VALUE_WORDS (sizeof(ComputedStyle) / 4u)
#define STYLE_DECLARATION_VALUES_ENCODED_MAX \
    (sizeof(ComputedStyle) + 2u * ((STYLE_DECLARATION_VALUE_WORDS + 1u) / 2u))
_Static_assert(sizeof(ComputedStyle) % 4u == 0,
               "declaration values encode whole 32-bit words");
_Static_assert(sizeof(ComputedStyle) / 4u <= UINT8_MAX,
               "declaration value word indices must fit one byte");
_Static_assert(STYLE_DECLARATION_VALUES_ENCODED_MAX <= UINT16_MAX,
               "encoded declaration values must fit values_length");
size_t style_declaration_values_encode(const ComputedStyle *values,
                                       uint8_t *encoded, size_t capacity);

struct StyleDeferredInstruction {
    uint32_t value_offset;
    uint16_t value_length;
    uint8_t property_index;
    uint8_t important;
};

_Static_assert(sizeof(StyleDeferredInstruction) == 8,
               "deferred declaration instructions must remain PSP-compact");

struct StyleRevertRuleMask {
    uint64_t mask;
    uint64_t mask_high;
    uint32_t order;
};

_Static_assert(sizeof(StyleCustomRule) <= 3u * sizeof(void *) + 24u,
               "retained sparse-rule metadata must stay compact");
_Static_assert(STYLE_CUSTOM_SELECTOR_CAPACITY <= UINT16_MAX + 1u
               && STYLE_CUSTOM_NAME_CAPACITY <= UINT8_MAX + 1u,
               "custom rule text lengths must fit their compact fields");
_Static_assert(STYLE_CUSTOM_VALUE_CAPACITY <= UINT16_MAX
               && STYLE_CUSTOM_SHORT_VALUE_CAPACITY
                  < STYLE_CUSTOM_RESOLVED_CAPACITY
               && STYLE_CUSTOM_RESOLVED_CAPACITY
                  <= STYLE_CUSTOM_VALUE_CAPACITY
               && STYLE_CUSTOM_SELECTOR_CAPACITY + STYLE_CUSTOM_NAME_CAPACITY
                  + STYLE_CUSTOM_VALUE_CAPACITY
                  < STYLE_SELECTOR_CHUNK_MAX_BYTES,
               "custom-property value limits must nest and fit the arena");

/* Tracing.  Mirrors the layout subsystem: environment is read once per
   stylesheet build; TILEFINCH_NO_TRACE compiles the trace bodies out. */
#ifndef TILEFINCH_NO_TRACE
#define TILEFINCH_TRACE_COMPILED_IN 1
#else
#define TILEFINCH_TRACE_COMPILED_IN 0
#endif

#define STYLE_TRACE_PAINT  (UINT8_C(1) << 0)
#define STYLE_TRACE_LAYOUT (UINT8_C(1) << 1)

#define STYLE_TRACE(sheet, FLAG) \
    (TILEFINCH_TRACE_COMPILED_IN != 0 \
     && ((sheet)->trace_flags & STYLE_TRACE_##FLAG) != 0)

/* style_properties.c */
bool declaration_is_important(const char *value, size_t length);
bool style_deferred_focus_shadow(const Stylesheet *sheet,
    const StyleDeclaration *declaration, bool outset);
bool style_deferred_focus_shadow_exceeds(const Stylesheet *sheet,
    const StyleDeclaration *declaration, lxb_dom_node_t *node, size_t layers);
size_t declaration_value_length(const char *value, size_t length);
size_t skip_css_space_and_comments(const char *text, size_t length,
                                   size_t at);
size_t find_declaration_end(const char *text, size_t length, size_t at);
bool style_parse_declarations(Stylesheet *sheet, const char *text,
                              size_t length, ComputedStyle *style,
                              uint64_t *mask, uint64_t *mask_high,
                              uint64_t *inherit_mask,
                              bool collect_variables, int important_filter,
                              uint64_t *revert_rule_mask,
                              uint64_t *revert_rule_mask_high);
bool style_parse_declarations_split(Stylesheet *sheet, const char *text,
                                    size_t length,
                                    ComputedStyle *normal_style,
                                    uint64_t *normal_mask,
                                    uint64_t *normal_mask_high,
                                    uint64_t *normal_inherit_mask,
                                    ComputedStyle *important_style,
                                    uint64_t *important_mask,
                                    uint64_t *important_mask_high,
                                    uint64_t *important_inherit_mask,
                                    uint64_t *normal_revert_rule_mask,
                                    uint64_t *normal_revert_rule_mask_high,
                                    uint64_t *important_revert_rule_mask,
                                    uint64_t *important_revert_rule_mask_high);
bool style_compile_deferred_program(Stylesheet *sheet, const char *text,
                                    size_t length, uint32_t *program_offset,
                                    uint16_t *program_count);
bool style_property_name_is_logical(const char *name, size_t length);
bool style_apply_deferred_program_basis(
    Stylesheet *sheet, const char *text, size_t length,
    uint32_t program_offset, uint16_t program_count, bool important,
    bool font_size_possible, ComputedStyle *style, uint64_t *mask,
    uint64_t *mask_high, uint64_t *inherit_mask);
bool style_apply_deferred_program(
    Stylesheet *sheet, const char *text, size_t length,
    uint32_t program_offset, uint16_t program_count, bool important,
    ComputedStyle *style, uint64_t *mask, uint64_t *mask_high,
    uint64_t *inherit_mask);

/* style_sheet.c */
bool style_set_variable(Stylesheet *sheet, const char *name,
                        size_t name_length, const char *value,
                        size_t value_length);
void stylesheet_rule_revert_mask(const Stylesheet *sheet,
                                 const StyleRule *rule,
                                 uint64_t *mask, uint64_t *mask_high);

/* style_compat_reveal.c: the post-cascade site-compatibility reveals,
   applied to `style` (resolved with `parent`) for `node`. */
void style_compat_post_cascade(const Stylesheet *sheet,
                               lxb_dom_node_t *node,
                               const ComputedStyle *parent,
                               ComputedStyle *style);

#endif /* TILEFINCH_STYLE_SHEET_INTERNAL_H */
