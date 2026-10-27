#ifndef TILEFINCH_STYLE_SPARSE_PROPERTIES_H
#define TILEFINCH_STYLE_SPARSE_PROPERTIES_H

/* The sparse ("retained layout") properties: the cascade does not decode
   them into ComputedStyle but retains their authored text for a later
   consumer (layout, getComputedStyle, the modern resolver). This one table
   decides what is retained, which summary bits a declaration sets, and how
   @supports validates a value; a new property is one row.

   X(name, alias, modern, typography, presence, validator)
     name        standard spelling; retained rules are stored under it
     alias       a vendor spelling stored under `name`, or NULL
     modern      STYLE_MODERN_* bit the modern resolver keys on, or 0
     typography  STYLE_MODERN_TYPOGRAPHY_* sub-bit, or 0
     presence    layout gate: NONE, MULTICOLUMN, CURSOR or SCROLL
     validator   suffix of the @supports value check, sparse_supports_<v>

   Lookup is a linear, case-insensitive scan in row order. */
#define STYLE_SPARSE_PROPERTIES(X) \
    X("container", NULL, 0, 0, NONE, container) \
    X("container-name", NULL, 0, 0, NONE, container_name) \
    X("container-type", NULL, 0, 0, NONE, container_type) \
    X("columns", NULL, 0, 0, MULTICOLUMN, columns) \
    X("column-count", NULL, 0, 0, MULTICOLUMN, column_count) \
    X("column-width", NULL, 0, 0, MULTICOLUMN, column_width) \
    X("column-gap", NULL, 0, 0, NONE, column_gap) \
    X("break-inside", NULL, 0, 0, NONE, break_inside) \
    X("cursor", NULL, 0, 0, CURSOR, cursor) \
    X("overscroll-behavior", NULL, 0, 0, SCROLL, overscroll_behavior) \
    X("overscroll-behavior-x", NULL, 0, 0, SCROLL, overscroll_behavior_axis) \
    X("overscroll-behavior-y", NULL, 0, 0, SCROLL, overscroll_behavior_axis) \
    X("overscroll-behavior-inline", NULL, 0, 0, SCROLL, \
      overscroll_behavior_axis) \
    X("overscroll-behavior-block", NULL, 0, 0, SCROLL, \
      overscroll_behavior_axis) \
    X("scroll-behavior", NULL, 0, 0, SCROLL, scroll_behavior) \
    X("scroll-margin", NULL, 0, 0, SCROLL, scroll_margin) \
    X("scroll-margin-top", NULL, 0, 0, SCROLL, scroll_margin_side) \
    X("scroll-margin-right", NULL, 0, 0, SCROLL, scroll_margin_side) \
    X("scroll-margin-bottom", NULL, 0, 0, SCROLL, scroll_margin_side) \
    X("scroll-margin-left", NULL, 0, 0, SCROLL, scroll_margin_side) \
    X("scroll-padding", NULL, 0, 0, SCROLL, scroll_padding) \
    X("scroll-padding-top", NULL, 0, 0, SCROLL, scroll_padding_side) \
    X("scroll-padding-right", NULL, 0, 0, SCROLL, scroll_padding_side) \
    X("scroll-padding-bottom", NULL, 0, 0, SCROLL, scroll_padding_side) \
    X("scroll-padding-left", NULL, 0, 0, SCROLL, scroll_padding_side) \
    X("scroll-snap-align", NULL, 0, 0, SCROLL, scroll_snap_align) \
    X("scroll-snap-stop", NULL, 0, 0, SCROLL, scroll_snap_stop) \
    X("scroll-snap-type", NULL, 0, 0, SCROLL, scroll_snap_type) \
    X("scrollbar-color", NULL, 0, 0, SCROLL, scrollbar_color) \
    X("scrollbar-width", NULL, 0, 0, SCROLL, scrollbar_width) \
    X("user-select", "-webkit-user-select", STYLE_MODERN_USER_SELECT, 0, \
      NONE, user_select) \
    X("touch-action", NULL, STYLE_MODERN_TOUCH_ACTION, 0, NONE, \
      touch_action) \
    X("text-size-adjust", "-webkit-text-size-adjust", \
      STYLE_MODERN_TEXT_SIZE_ADJUST, 0, NONE, text_size_adjust) \
    X("resize", NULL, STYLE_MODERN_RESIZE, 0, NONE, resize) \
    X("text-wrap", NULL, STYLE_MODERN_TEXT_WRAP, 0, NONE, text_wrap) \
    X("text-wrap-style", NULL, STYLE_MODERN_TEXT_WRAP, 0, NONE, text_wrap) \
    X("hyphens", NULL, STYLE_MODERN_TYPOGRAPHY, \
      STYLE_MODERN_TYPOGRAPHY_HYPHENS, NONE, hyphens) \
    X("tab-size", NULL, STYLE_MODERN_TYPOGRAPHY, \
      STYLE_MODERN_TYPOGRAPHY_TAB_SIZE, NONE, tab_size) \
    X("font-kerning", NULL, STYLE_MODERN_TYPOGRAPHY, \
      STYLE_MODERN_TYPOGRAPHY_KERNING, NONE, font_kerning) \
    X("text-rendering", NULL, STYLE_MODERN_TYPOGRAPHY, \
      STYLE_MODERN_TYPOGRAPHY_TEXT_RENDERING, NONE, text_rendering) \
    X("contain-intrinsic-size", NULL, 0, 0, NONE, contain_intrinsic_size) \
    X("translate", NULL, STYLE_MODERN_TRANSLATE, 0, NONE, translate) \
    X("rotate", NULL, STYLE_MODERN_ROTATE, 0, NONE, rotate) \
    X("scale", NULL, STYLE_MODERN_SCALE, 0, NONE, scale) \
    X("isolation", NULL, STYLE_MODERN_ISOLATION, 0, NONE, isolation) \
    X("mix-blend-mode", NULL, STYLE_MODERN_MIX_BLEND, 0, NONE, \
      mix_blend_mode) \
    X("backface-visibility", NULL, STYLE_MODERN_BACKFACE_VISIBILITY, 0, \
      NONE, backface_visibility) \
    X("transform-style", NULL, STYLE_MODERN_TRANSFORM_STYLE, 0, NONE, \
      transform_style) \
    X("color-scheme", NULL, STYLE_MODERN_COLOR_SCHEME, 0, NONE, \
      color_scheme) \
    X("border-image", NULL, STYLE_MODERN_BORDER_IMAGE, 0, NONE, \
      border_image_source) \
    X("border-image-source", NULL, STYLE_MODERN_BORDER_IMAGE, 0, NONE, \
      border_image_source) \
    X("border-image-slice", NULL, STYLE_MODERN_BORDER_IMAGE, 0, NONE, \
      border_image_geometry) \
    X("border-image-width", NULL, STYLE_MODERN_BORDER_IMAGE, 0, NONE, \
      border_image_geometry) \
    X("border-image-outset", NULL, STYLE_MODERN_BORDER_IMAGE, 0, NONE, \
      border_image_geometry) \
    X("border-image-repeat", NULL, STYLE_MODERN_BORDER_IMAGE, 0, NONE, \
      border_image_geometry) \
    X("backdrop-filter", "-webkit-backdrop-filter", \
      STYLE_MODERN_BACKDROP_FILTER, 0, NONE, backdrop_filter) \
    X("border-start-start-radius", NULL, STYLE_MODERN_LOGICAL_RADIUS, 0, \
      NONE, logical_radius) \
    X("border-start-end-radius", NULL, STYLE_MODERN_LOGICAL_RADIUS, 0, \
      NONE, logical_radius) \
    X("border-end-start-radius", NULL, STYLE_MODERN_LOGICAL_RADIUS, 0, \
      NONE, logical_radius) \
    X("border-end-end-radius", NULL, STYLE_MODERN_LOGICAL_RADIUS, 0, \
      NONE, logical_radius)

#endif
