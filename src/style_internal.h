#ifndef TILEFINCH_STYLE_INTERNAL_H
#define TILEFINCH_STYLE_INTERNAL_H

/* Shared internals for the style subsystem translation units
   (style.c, style_math.c, style_values.c, style_properties.c,
   style_sheet.c, style_match.c, style_resolve.c).  Nothing here is part
   of the public tilefinch API; consumers must keep including
   tilefinch/style.h only. */

/* An umbrella: the style internals are split by consumer into
   style_cssom.h (what the DOM bindings use), style_values_internal.h,
   style_match_internal.h, style_scopes.h and style_sheet_internal.h, each
   including the ones it builds on. A translation unit may include the
   narrowest one it needs. */
#include "style_cssom.h"
#include "style_values_internal.h"
#include "style_match_internal.h"
#include "style_scopes.h"
#include "style_sheet_internal.h"

#endif /* TILEFINCH_STYLE_INTERNAL_H */
