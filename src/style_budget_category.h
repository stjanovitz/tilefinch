#ifndef TILEFINCH_STYLE_BUDGET_CATEGORY_H
#define TILEFINCH_STYLE_BUDGET_CATEGORY_H

/* Charge a style translation unit's allocations to BUDGET_CATEGORY_STYLE.
   Include this first in each style subsystem .c file, never from a header:
   a shared header that redefines the allocators re-categorizes the memory
   of every file that borrows one style helper (see fetch.c and render.c for
   the same per-file pattern). */

#include "tilefinch/budget.h"

#undef budget_malloc
#undef budget_calloc
#undef budget_realloc
#define budget_malloc(b, s) budget_malloc_category((b), BUDGET_CATEGORY_STYLE, (s))
#define budget_calloc(b, n, s) budget_calloc_category((b), BUDGET_CATEGORY_STYLE, (n), (s))
#define budget_realloc(b, p, s) budget_realloc_category((b), BUDGET_CATEGORY_STYLE, (p), (s))

#endif
