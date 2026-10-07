/*
 * Public struct layout probe (cmake/CheckAbiLayout.cmake).
 *
 * tilefinch_core, the PSP frontend and its static libraries are compiled
 * with different private defines (TILEFINCH_NO_TRACE is private to the
 * core; the psp_ui and app-support libraries never see
 * TILEFINCH_PSP_VALIDATION_LOG). A public header whose struct layout
 * depended on one of those would give the two sides different offsets for
 * the same object. This file is compiled once with each side's exact
 * compile definitions, include paths and options, never linked, and the
 * check compares the sizes of the arrays below between the objects: each
 * array's size is a struct size or a field offset, so any divergence fails
 * the build.
 */
#include <stddef.h>

#include "tilefinch/browser_engine.h"
#include "tilefinch/budget.h"
#include "tilefinch/controller.h"
#include "tilefinch/js_runtime.h"
#include "tilefinch/layout.h"
#include "tilefinch/navigation.h"
#include "tilefinch/render.h"

#define TILEFINCH_ABI_SIZE(type) \
    unsigned char tilefinch_abi_size_##type[sizeof(type)];
#define TILEFINCH_ABI_OFFSET(type, field) \
    unsigned char tilefinch_abi_offset_##type##_##field[ \
        offsetof(type, field) + 1u];

TILEFINCH_ABI_SIZE(BrowserController)
TILEFINCH_ABI_OFFSET(BrowserController, focus_node)
TILEFINCH_ABI_OFFSET(BrowserController, pointer_node)
TILEFINCH_ABI_OFFSET(BrowserController, has_authored_focus_outline)
TILEFINCH_ABI_SIZE(NavigationSession)
TILEFINCH_ABI_SIZE(NavigationPerformance)
TILEFINCH_ABI_SIZE(NavigationFrame)
TILEFINCH_ABI_SIZE(ScriptResult)
TILEFINCH_ABI_SIZE(BrowserEngineMetrics)
TILEFINCH_ABI_SIZE(TileCache)
TILEFINCH_ABI_SIZE(Budget)
TILEFINCH_ABI_SIZE(LayoutDocument)
