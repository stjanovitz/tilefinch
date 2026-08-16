#ifndef TILEFINCH_STYLE_CACHE_INTERNAL_H
#define TILEFINCH_STYLE_CACHE_INTERNAL_H

#include "tilefinch/style.h"

typedef bool (*StyleSelectorCooperate)(
    void *opaque, lxb_dom_node_t *node, size_t completed_visits);

/*
 * A variable cache is valid only while one immutable layout snapshot is
 * being built.  Keeping its lifetime at this boundary prevents DOM/style
 * mutations between layouts from observing stale custom-property values.
 */
bool style_variable_cache_begin(Stylesheet *sheet, Budget *budget);
void style_variable_cache_end(Stylesheet *sheet);
void style_variable_cache_invalidate_node(
    Stylesheet *sheet, const lxb_dom_node_t *node);
size_t style_variable_cache_bytes(const Stylesheet *sheet);

/* Selector programs can contain bounded but still expensive ancestor and
   sibling walks. Layout installs this transient callback for one immutable
   build so those walks share its input/cancellation boundary. */
bool style_selector_cooperation_begin(
    Stylesheet *sheet, StyleSelectorCooperate cooperate, void *opaque);
void style_selector_cooperation_end(Stylesheet *sheet);
bool style_selector_cooperation_cancelled(const Stylesheet *sheet);

#endif
