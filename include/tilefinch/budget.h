#ifndef TILEFINCH_BUDGET_H
#define TILEFINCH_BUDGET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef enum {
    BUDGET_CATEGORY_UNCATEGORIZED = 0,
    BUDGET_CATEGORY_DOM,
    BUDGET_CATEGORY_JAVASCRIPT,
    BUDGET_CATEGORY_STYLE,
    BUDGET_CATEGORY_RESOURCE,
    BUDGET_CATEGORY_LAYOUT,
    BUDGET_CATEGORY_RENDER,
    BUDGET_CATEGORY_SESSION,
    BUDGET_CATEGORY_NAVIGATION,
    BUDGET_CATEGORY_COUNT
} BudgetCategory;

typedef enum {
    BUDGET_PRESSURE_JAVASCRIPT = 0,
    BUDGET_PRESSURE_STYLESHEET,
    BUDGET_PRESSURE_IMAGE,
    BUDGET_PRESSURE_TILE,
    BUDGET_PRESSURE_CACHE,
    BUDGET_PRESSURE_HISTORY,
    BUDGET_PRESSURE_SPECULATION,
    BUDGET_PRESSURE_COUNT
} BudgetPressureReason;

typedef struct {
    size_t decisions;
    size_t avoided_bytes;
    size_t saved_bytes;
} BudgetPressureStats;

typedef struct {
    size_t current;
    size_t peak;
    size_t active_allocations;
    size_t allocation_count;
    size_t free_count;
    size_t phase_peak;
    char high_water_phase[24];
} BudgetCategoryStats;

/* Stored in otherwise-unused allocation-header bytes so transaction
   isolation has no per-allocation footprint cost on the PSP. */
typedef uint32_t BudgetAllocationOwner;

#define BUDGET_ALLOCATION_OWNER_NONE UINT32_C(0)

typedef struct {
    size_t limit;
    size_t current;
    size_t peak;
    size_t allocation_count;
    size_t free_count;
    size_t failure_count;
    size_t injected_failure_count;
    /* Nonzero means the empty-ledger recovery found accounting drift. Keep
       this historical so reconciliation cannot hide a repaired invariant. */
    size_t accounting_repair_count;
    size_t failure_injection_countdown;
    bool failure_injection_enabled;
    void *allocation_head;
    uint64_t next_sequence;
    BudgetAllocationOwner active_allocation_owner;
    BudgetAllocationOwner next_allocation_owner;
    BudgetCategoryStats categories[BUDGET_CATEGORY_COUNT];
    size_t global_peak_categories[BUDGET_CATEGORY_COUNT];
    BudgetPressureStats pressure[BUDGET_PRESSURE_COUNT];
    size_t pressure_decisions;
    size_t pressure_avoided_bytes;
    size_t pressure_saved_bytes;
    size_t external_reserved;
    size_t external_reserved_peak;
} Budget;

/* A logical charge for memory whose physical storage lives outside the
   Budget heap (for example inline facade tables or a separately synchronized
   transport pool). It participates in the ordinary intrusive ledger and
   category reconciliation without allocating a dummy payload. */
typedef struct {
    Budget *budget;
    void *token;
} BudgetReservation;

/*
 * A bounded allocator for foreign libraries which may invoke allocation
 * callbacks from worker threads.  Its state is intentionally opaque: the
 * parent Budget receives one conservative reservation on the owning thread,
 * while individual worker-thread allocations are tracked behind a small
 * independent lock.  This keeps the Budget intrusive list single-threaded
 * and avoids putting a lock on every DOM/style/layout allocation.
 *
 * Both BudgetReservation and BudgetConcurrentPool must be zero-initialized
 * before first use. The owner must stop and join every foreign worker before
 * destroy. Destroy fails closed while any pool allocation remains live and
 * leaves the parent reservation in place so a lifecycle error cannot
 * disappear from teardown accounting.
 */
#define BUDGET_CONCURRENT_POOL_STORAGE_SIZE 192
typedef union {
    max_align_t alignment;
    unsigned char bytes[BUDGET_CONCURRENT_POOL_STORAGE_SIZE];
} BudgetConcurrentPool;

typedef struct {
    size_t limit;
    size_t current;
    size_t peak;
    size_t active_allocations;
    size_t allocation_count;
    size_t free_count;
    size_t failure_count;
} BudgetConcurrentPoolMetrics;

void budget_init(Budget *budget, size_t limit);
void budget_inject_failure_after(Budget *budget, size_t successful_attempts);
void budget_clear_failure_injection(Budget *budget);
/* Lexbor's allocator table is process-global. Install refuses to replace an
   owner with live Lexbor allocations; uninstall likewise requires the
   supplied current owner to have no live Lexbor allocations. */
bool budget_install_lexbor(Budget *budget);
bool budget_uninstall_lexbor(Budget *budget);
bool budget_lexbor_is_installed(const Budget *budget);
void *budget_malloc_category(Budget *budget, BudgetCategory category,
                             size_t size);
/* Allocate a payload on a 64-byte boundary with no allocator metadata in
   either adjacent payload cache line. This is the only Budget allocation
   form suitable for buffers shared with the PSP Media Engine or DMAC. The
   usable size remains the requested size; the ledger also charges alignment
   and end padding so cache maintenance may safely round the range to 64. */
void *budget_malloc_cacheline_category(Budget *budget,
                                       BudgetCategory category,
                                       size_t size);
void *budget_calloc_category(Budget *budget, BudgetCategory category,
                             size_t count, size_t size);
void *budget_realloc_category(Budget *budget, BudgetCategory category,
                              void *ptr, size_t size);
void *budget_malloc(Budget *budget, size_t size);
void *budget_calloc(Budget *budget, size_t count, size_t size);
void *budget_realloc(Budget *budget, void *ptr, size_t size);
void budget_free(Budget *budget, void *ptr);
size_t budget_usable_size(const void *ptr);
bool budget_owns(const Budget *budget, const void *ptr);
size_t budget_remaining(const Budget *budget);
bool budget_pressure_required(const Budget *budget, size_t working_bytes,
                              size_t reserve_bytes);
const char *budget_pressure_reason_name(BudgetPressureReason reason);
void budget_record_pressure(Budget *budget, BudgetPressureReason reason,
                            size_t avoided_bytes, size_t saved_bytes);
void budget_report_pressure(const Budget *budget, FILE *stream);
/* Print every active allocation of a category at or above minimum_size
   (diagnostic; size, 24-bit owner tag, and allocation sequence). */
void budget_report_active(const Budget *budget, BudgetCategory category,
                          size_t minimum_size, FILE *stream);

size_t budget_active_allocations(const Budget *budget,
                                 size_t *largest_payload);
uint64_t budget_checkpoint(const Budget *budget);
void budget_rollback(Budget *budget, uint64_t checkpoint);
void budget_rollback_category(Budget *budget, uint64_t checkpoint,
                              BudgetCategory category);
/* Allocation owners isolate overlapping transactions which share a category.
   Enter/leave calls are nestable; newly allocated blocks inherit the active
   owner, while realloc preserves the original block's owner. */
bool budget_allocation_owner_create(Budget *budget,
                                    BudgetAllocationOwner *owner);
BudgetAllocationOwner budget_allocation_owner_enter(
    Budget *budget, BudgetAllocationOwner owner);
void budget_allocation_owner_leave(Budget *budget,
                                    BudgetAllocationOwner previous_owner);
void budget_rollback_owner_category(Budget *budget,
                                    BudgetAllocationOwner owner,
                                    BudgetCategory category);
const char *budget_category_name(BudgetCategory category);
bool budget_categories_reconcile(const Budget *budget);
void budget_mark_phase(Budget *budget, const char *phase);
void budget_report(Budget *budget, const char *phase, FILE *stream);
void budget_report_categories(Budget *budget, const char *phase,
                              FILE *stream);
void budget_dump_active(const Budget *budget, FILE *stream,
                        size_t maximum_entries);

bool budget_reservation_acquire(BudgetReservation *reservation,
                                Budget *budget, BudgetCategory category,
                                size_t bytes);
void budget_reservation_release(BudgetReservation *reservation);

bool budget_concurrent_pool_init(BudgetConcurrentPool *pool, Budget *budget,
                                 BudgetCategory category, size_t limit);
void *budget_concurrent_pool_malloc(BudgetConcurrentPool *pool, size_t size);
void *budget_concurrent_pool_calloc(BudgetConcurrentPool *pool,
                                    size_t count, size_t size);
void *budget_concurrent_pool_realloc(BudgetConcurrentPool *pool, void *ptr,
                                     size_t size);
void budget_concurrent_pool_free(BudgetConcurrentPool *pool, void *ptr);
size_t budget_concurrent_pool_usable_size(
    const BudgetConcurrentPool *pool, const void *ptr);
bool budget_concurrent_pool_metrics(const BudgetConcurrentPool *pool,
                                    BudgetConcurrentPoolMetrics *metrics);
bool budget_concurrent_pool_destroy(BudgetConcurrentPool *pool);
#endif
