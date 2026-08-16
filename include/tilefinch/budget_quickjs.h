#ifndef TILEFINCH_BUDGET_QUICKJS_H
#define TILEFINCH_BUDGET_QUICKJS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include <quickjs.h>

#include "tilefinch/budget.h"

/* QuickJS integration is deliberately separate from the generic allocator
   contract so DOM/layout-only translation units do not inherit VM headers. */
typedef struct BudgetQuickJSPool BudgetQuickJSPool;

const JSMallocFunctions *budget_quickjs_allocator(void);
BudgetQuickJSPool *budget_quickjs_pool_create(Budget *budget);
bool budget_quickjs_pool_destroy(BudgetQuickJSPool *pool);
size_t budget_quickjs_pool_trim(BudgetQuickJSPool *pool,
                                size_t retain_per_class);
void budget_quickjs_pool_set_cache_limits(BudgetQuickJSPool *pool,
                                          size_t small_class_limit,
                                          size_t large_class_limit);
const JSMallocFunctions *budget_quickjs_pool_allocator(void);
size_t budget_quickjs_pool_reserved_peak(const BudgetQuickJSPool *pool);
/* Current enforced QuickJS malloc_size (O(1), maintained at alloc/free). */
size_t budget_quickjs_pool_js_malloc_current(const BudgetQuickJSPool *pool);

/* Largest single page-heap request seen (diagnostic). */
size_t budget_quickjs_pool_largest_request(const BudgetQuickJSPool *pool);
void budget_quickjs_pool_set_stack_dump_hook(void (*hook)(void *opaque),
                                             void *opaque);

/* Print any recorded page-heap allocation rejections (diagnostic). */
void budget_quickjs_pool_report_rejects(FILE *stream);

/* High-water mark of the enforced QuickJS malloc_size (transient peak). */
size_t budget_quickjs_pool_js_malloc_peak(const BudgetQuickJSPool *pool);

/* Print the live per-class allocation census (requested vs capacity). */
void budget_quickjs_pool_report_classes(const BudgetQuickJSPool *pool,
                                        FILE *output);

#endif
