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

/* Validation builds use this monotonic snapshot to correlate rare runtime
   stalls with allocator churn without walking the QuickJS heap; the
   tilefinch-work record reports its call and byte totals. Host builds and
   PSP validation builds maintain the counters; shipping PSP builds return
   an all-zero snapshot. */
typedef struct {
    uint64_t allocation_calls;
    uint64_t free_calls;
    uint64_t reallocation_calls;
    uint64_t allocated_bytes;
    uint64_t freed_bytes;
    uint64_t reallocated_bytes;
    size_t live_bytes;
    size_t rejected_old_bytes;
    size_t rejected_new_bytes;
    size_t rejected_live_bytes;
    /* Blocks handed out by the pool (malloc, and realloc that changed
       class) and how many needed a fresh Budget allocation. */
    uint64_t pool_blocks;
    uint64_t pool_fresh_blocks;
} BudgetQuickJSActivity;

const JSMallocFunctions *budget_quickjs_allocator(void);
BudgetQuickJSPool *budget_quickjs_pool_create(Budget *budget);
#if !defined(__PSP__)
/* Host-only native-tier lab: recover the existing Budget owner from the
   allocator passed to JS_NewRuntime2. Never creates a second allocation. */
Budget *budget_quickjs_pool_owner(const BudgetQuickJSPool *pool);
#endif
bool budget_quickjs_pool_destroy(BudgetQuickJSPool *pool);
size_t budget_quickjs_pool_trim(BudgetQuickJSPool *pool,
                                size_t retain_per_class);
void budget_quickjs_pool_set_cache_limits(BudgetQuickJSPool *pool,
                                          size_t small_class_limit,
                                          size_t large_class_limit);
const JSMallocFunctions *budget_quickjs_pool_allocator(void);
size_t budget_quickjs_pool_reserved_peak(const BudgetQuickJSPool *pool);
/* Current allocator capacity, including bounded cached free blocks. */
size_t budget_quickjs_pool_reserved_current(const BudgetQuickJSPool *pool);
/* Current enforced QuickJS malloc_size (O(1), maintained at alloc/free). */
size_t budget_quickjs_pool_js_malloc_current(const BudgetQuickJSPool *pool);
/* Asked when an allocation would pass the realm's heap limit: `live` bytes
   are in use and `growth` more are requested against `limit`. Return a
   larger limit to admit the allocation, or 0 to refuse it. Runs inside the
   allocator: it must not allocate or run JavaScript. */
typedef size_t (*BudgetQuickJSLimitGrowth)(void *opaque, size_t live,
                                           size_t growth, size_t limit);
void budget_quickjs_pool_set_limit_growth(BudgetQuickJSPool *pool,
                                          BudgetQuickJSLimitGrowth grow,
                                          void *opaque);
/* Monotonic count of allocations refused by the realm's hard heap limit. */
size_t budget_quickjs_pool_rejection_count(const BudgetQuickJSPool *pool);
void budget_quickjs_pool_activity(const BudgetQuickJSPool *pool,
                                  BudgetQuickJSActivity *activity);

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

/* Per-class traffic (host and validation builds): cumulative blocks
   handed out and fresh Budget allocations, plus the live census, one
   `tilefinch-js-pool:` line per class. Diagnostic, allocation-free. */
void budget_quickjs_pool_report_traffic(const BudgetQuickJSPool *pool,
                                        const char *label, FILE *output);

#endif
