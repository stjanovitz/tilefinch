#include "tilefinch/budget.h"
#include "tilefinch/budget_quickjs.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/core/base.h>

#if defined(__PSP__)
#include <pspkernel.h>
#endif

#define BUDGET_MAGIC UINT32_C(0x42554447)
#define BUDGET_ALLOCATION_OWNER_MAX UINT32_C(0x00ffffff)
#define BUDGET_CATEGORY_VALUE_MASK UINT8_C(0x3f)
#define BUDGET_ALLOCATION_EXTERNAL UINT8_C(0x40)
#define BUDGET_ALLOCATION_LEXBOR UINT8_C(0x80)
#define BUDGET_CONCURRENT_MAGIC UINT32_C(0x434f4e43)
#define BUDGET_CONCURRENT_STATE_MAGIC UINT32_C(0x43504f4c)

typedef union AllocationHeader AllocationHeader;

union AllocationHeader {
    struct {
        size_t size;
        Budget *owner;
        uint32_t magic;
        uint8_t category;
        uint8_t allocation_owner[3];
        uint64_t sequence;
        AllocationHeader *previous;
        AllocationHeader *next;
    } data;
    max_align_t alignment;
};

static Budget *lexbor_budget;
static size_t lexbor_active_allocations;

static AllocationHeader *header_from_payload(const void *ptr);

#if !defined(__PSP__)
#include <execinfo.h>
static void budget_debug_trap(uint64_t sequence)
{
    static long target = -2;
    if (target == -2) {
        const char *value = getenv("TILEFINCH_BUDGET_TRAP_SEQ");
        target = value == NULL ? -1 : atol(value);
    }
    if (target < 0 || (uint64_t) target != sequence) return;
    void *frames[24];
    int count = backtrace(frames, 24);
    fprintf(stderr, "budget-trap sequence=%llu\n",
            (unsigned long long) sequence);
    backtrace_symbols_fd(frames, count, 2);
}
#else
static void budget_debug_trap(uint64_t sequence) { (void) sequence; }
#endif


static const char *const category_names[BUDGET_CATEGORY_COUNT] = {
    "uncategorized", "dom", "javascript", "style", "resource",
    "layout", "render", "session", "navigation"
};

static BudgetCategory valid_category(BudgetCategory category)
{
    return (unsigned) category < BUDGET_CATEGORY_COUNT
        ? category : BUDGET_CATEGORY_UNCATEGORIZED;
}

static BudgetCategory header_category(const AllocationHeader *header)
{
    return valid_category((BudgetCategory)
        (header->data.category & BUDGET_CATEGORY_VALUE_MASK));
}

static bool header_is_lexbor(const AllocationHeader *header)
{
    return (header->data.category & BUDGET_ALLOCATION_LEXBOR) != 0;
}

static bool header_is_external(const AllocationHeader *header)
{
    return (header->data.category & BUDGET_ALLOCATION_EXTERNAL) != 0;
}

static void header_mark_lexbor(void *pointer)
{
    AllocationHeader *header = header_from_payload(pointer);
    if (header == NULL || header->data.magic != BUDGET_MAGIC
        || header_is_lexbor(header)) return;
    header->data.category |= BUDGET_ALLOCATION_LEXBOR;
    lexbor_active_allocations++;
}

const char *budget_category_name(BudgetCategory category)
{
    return category_names[valid_category(category)];
}

static void budget_capture_global_peak(Budget *budget)
{
    if (budget->current <= budget->peak) return;
    budget->peak = budget->current;
    for (size_t i = 0; i < BUDGET_CATEGORY_COUNT; i++) {
        budget->global_peak_categories[i] = budget->categories[i].current;
    }
}

static void budget_category_grow(Budget *budget, BudgetCategory category,
                                 size_t charge, bool allocation)
{
    category = valid_category(category);
    BudgetCategoryStats *stats = &budget->categories[category];
    stats->current += charge;
    if (stats->current > stats->peak) stats->peak = stats->current;
    if (allocation) {
        stats->active_allocations++;
        stats->allocation_count++;
    }
}

static void budget_category_shrink(Budget *budget, BudgetCategory category,
                                   size_t charge, bool release)
{
    category = valid_category(category);
    BudgetCategoryStats *stats = &budget->categories[category];
    stats->current = stats->current >= charge
        ? stats->current - charge : 0;
    if (release) {
        if (stats->active_allocations != 0) stats->active_allocations--;
        stats->free_count++;
    }
}

static bool checked_product(size_t left, size_t right, size_t *result)
{
    if (left != 0 && right > SIZE_MAX / left) {
        return false;
    }
    *result = left * right;
    return true;
}

static AllocationHeader *header_from_payload(const void *ptr)
{
    if (ptr == NULL) {
        return NULL;
    }
    return ((AllocationHeader *) ptr) - 1;
}

static BudgetAllocationOwner header_allocation_owner(
    const AllocationHeader *header)
{
    return (BudgetAllocationOwner) header->data.allocation_owner[0]
        | (BudgetAllocationOwner) header->data.allocation_owner[1] << 8
        | (BudgetAllocationOwner) header->data.allocation_owner[2] << 16;
}

static void header_set_allocation_owner(
    AllocationHeader *header, BudgetAllocationOwner owner)
{
    header->data.allocation_owner[0] = (uint8_t) owner;
    header->data.allocation_owner[1] = (uint8_t) (owner >> 8);
    header->data.allocation_owner[2] = (uint8_t) (owner >> 16);
}

void budget_init(Budget *budget, size_t limit)
{
    memset(budget, 0, sizeof(*budget));
    budget->limit = limit;
}

void budget_inject_failure_after(Budget *budget, size_t successful_attempts)
{
    if (budget == NULL) return;
    budget->failure_injection_countdown = successful_attempts;
    budget->failure_injection_enabled = true;
}

void budget_clear_failure_injection(Budget *budget)
{
    if (budget == NULL) return;
    budget->failure_injection_countdown = 0;
    budget->failure_injection_enabled = false;
}

static bool budget_should_inject_failure(Budget *budget)
{
    if (budget == NULL || !budget->failure_injection_enabled) return false;
    if (budget->failure_injection_countdown != 0) {
        budget->failure_injection_countdown--;
        return false;
    }
    budget->failure_injection_enabled = false;
    budget->failure_count++;
    budget->injected_failure_count++;
    return true;
}

void *budget_malloc_category(Budget *budget, BudgetCategory category,
                             size_t size)
{
    if (budget == NULL) {
        return NULL;
    }

    if (size == 0) {
        size = 1;
    }

    if (budget_should_inject_failure(budget)) return NULL;

    if (size > SIZE_MAX - sizeof(AllocationHeader)) {
        budget->failure_count++;
        return NULL;
    }
    size_t charge = sizeof(AllocationHeader) + size;
    if (charge > budget_remaining(budget)) {
        budget->failure_count++;
        return NULL;
    }

    AllocationHeader *header = malloc(sizeof(*header) + size);
    if (header == NULL) {
        budget->failure_count++;
        return NULL;
    }

    header->data.size = size;
    header->data.owner = budget;
    header->data.magic = BUDGET_MAGIC;
    header->data.category = (uint8_t) valid_category(category);
    header->data.sequence = ++budget->next_sequence;
    budget_debug_trap(header->data.sequence);
    header_set_allocation_owner(header, budget->active_allocation_owner);
    header->data.previous = NULL;
    header->data.next = budget->allocation_head;
    if (header->data.next != NULL) {
        header->data.next->data.previous = header;
    }
    budget->allocation_head = header;
    budget->current += charge;
    budget_category_grow(budget, category, charge, true);
    budget_capture_global_peak(budget);
    budget->allocation_count++;

    return header + 1;
}

void *budget_malloc(Budget *budget, size_t size)
{
    return budget_malloc_category(
        budget, BUDGET_CATEGORY_UNCATEGORIZED, size);
}

/* Reserve external backing without allocating the reserved payload.  The
   header remains in the ordinary ledger, so category reconciliation and
   teardown gates still see the entire conservative capacity. */
static void *budget_reserve_external(Budget *budget, BudgetCategory category,
                                     size_t size)
{
    if (budget == NULL || size == 0
        || size > SIZE_MAX - sizeof(AllocationHeader)) return NULL;
    if (budget_should_inject_failure(budget)) return NULL;
    size_t charge = sizeof(AllocationHeader) + size;
    if (charge > budget_remaining(budget)) {
        budget->failure_count++;
        return NULL;
    }
    AllocationHeader *header = malloc(sizeof(*header));
    if (header == NULL) {
        budget->failure_count++;
        return NULL;
    }
    header->data.size = size;
    header->data.owner = budget;
    header->data.magic = BUDGET_MAGIC;
    header->data.category = (uint8_t) valid_category(category)
                            | BUDGET_ALLOCATION_EXTERNAL;
    header->data.sequence = ++budget->next_sequence;
    budget_debug_trap(header->data.sequence);
    header_set_allocation_owner(header, budget->active_allocation_owner);
    header->data.previous = NULL;
    header->data.next = budget->allocation_head;
    if (header->data.next != NULL)
        header->data.next->data.previous = header;
    budget->allocation_head = header;
    budget->current += charge;
    budget_category_grow(budget, category, charge, true);
    budget_capture_global_peak(budget);
    budget->allocation_count++;
    budget->external_reserved += size;
    if (budget->external_reserved > budget->external_reserved_peak)
        budget->external_reserved_peak = budget->external_reserved;
    return header + 1;
}

bool budget_reservation_acquire(BudgetReservation *reservation,
                                Budget *budget, BudgetCategory category,
                                size_t bytes)
{
    if (reservation == NULL || reservation->token != NULL
        || reservation->budget != NULL) return false;
    void *token = budget_reserve_external(budget, category, bytes);
    if (token == NULL) return false;
    reservation->budget = budget;
    reservation->token = token;
    return true;
}

void budget_reservation_release(BudgetReservation *reservation)
{
    if (reservation == NULL || reservation->budget == NULL
        || reservation->token == NULL) return;
    Budget *budget = reservation->budget;
    void *token = reservation->token;
    memset(reservation, 0, sizeof(*reservation));
    budget_free(budget, token);
}

void *budget_calloc_category(Budget *budget, BudgetCategory category,
                             size_t count, size_t size)
{
    size_t total;
    if (!checked_product(count, size, &total)) {
        if (budget != NULL) {
            budget->failure_count++;
        }
        return NULL;
    }

    size_t initialized = total == 0 ? 1 : total;
    void *ptr = budget_malloc_category(budget, category, initialized);
    if (ptr != NULL) {
        memset(ptr, 0, initialized);
    }
    return ptr;
}

void *budget_calloc(Budget *budget, size_t count, size_t size)
{
    return budget_calloc_category(
        budget, BUDGET_CATEGORY_UNCATEGORIZED, count, size);
}

void *budget_realloc_category(Budget *budget, BudgetCategory category,
                              void *ptr, size_t size)
{
    if (ptr == NULL) {
        return budget_malloc_category(budget, category, size);
    }
    AllocationHeader *old_header = header_from_payload(ptr);
    if (budget == NULL || old_header->data.magic != BUDGET_MAGIC
        || old_header->data.owner != budget
        || header_is_external(old_header)) {
        return NULL;
    }
    if (size == 0) {
        budget_free(budget, ptr);
        return NULL;
    }

    if (budget_should_inject_failure(budget)) return NULL;

    size_t old_size = old_header->data.size;
    BudgetCategory retained_category = header_category(old_header);
    size_t old_charge = sizeof(AllocationHeader) + old_size;
    size_t base = budget->current - old_charge;
    if (size > SIZE_MAX - sizeof(AllocationHeader)) {
        budget->failure_count++;
        return NULL;
    }
    size_t new_charge = sizeof(AllocationHeader) + size;
    if (new_charge > budget->limit - base) {
        budget->failure_count++;
        return NULL;
    }

    AllocationHeader *new_header = realloc(old_header, sizeof(*new_header) + size);
    if (new_header == NULL) {
        budget->failure_count++;
        return NULL;
    }

    new_header->data.size = size;
    new_header->data.owner = budget;
    new_header->data.magic = BUDGET_MAGIC;
    if (new_header != old_header) {
        if (new_header->data.previous != NULL) {
            new_header->data.previous->data.next = new_header;
        } else {
            budget->allocation_head = new_header;
        }
        if (new_header->data.next != NULL) {
            new_header->data.next->data.previous = new_header;
        }
    }
    budget->current = base + new_charge;
    if (new_charge >= old_charge) {
        budget_category_grow(budget, retained_category,
                             new_charge - old_charge, false);
    } else {
        budget_category_shrink(budget, retained_category,
                               old_charge - new_charge, false);
    }
    budget_capture_global_peak(budget);
    return new_header + 1;
}

void *budget_realloc(Budget *budget, void *ptr, size_t size)
{
    return budget_realloc_category(
        budget, BUDGET_CATEGORY_UNCATEGORIZED, ptr, size);
}

void budget_free(Budget *budget, void *ptr)
{
    if (ptr == NULL) {
        return;
    }

    AllocationHeader *header = header_from_payload(ptr);
    if (budget == NULL || header->data.magic != BUDGET_MAGIC
        || header->data.owner != budget) {
        return;
    }

    size_t size = header->data.size;
    size_t charge = sizeof(AllocationHeader) + size;
    BudgetCategory category = header_category(header);
    bool external = header_is_external(header);
    if (header->data.previous != NULL) {
        header->data.previous->data.next = header->data.next;
    } else {
        budget->allocation_head = header->data.next;
    }
    if (header->data.next != NULL) {
        header->data.next->data.previous = header->data.previous;
    }
    header->data.magic = 0;
    if (header_is_lexbor(header) && lexbor_active_allocations != 0) {
        lexbor_active_allocations--;
    }
    if (budget->current >= charge) {
        budget->current -= charge;
    } else {
        budget->current = 0;
    }
    budget_category_shrink(budget, category, charge, true);
    if (external) {
        budget->external_reserved = budget->external_reserved >= size
            ? budget->external_reserved - size : 0;
    }
    /* The intrusive list is authoritative if accounting drift reached the
       final free. Preserve the recovery, but make it permanently visible to
       reconciliation and diagnostics instead of silently erasing evidence. */
    if (budget->allocation_head == NULL) {
        bool repaired = budget->current != 0
            || budget->external_reserved != 0
            || (budget == lexbor_budget && lexbor_active_allocations != 0);
        for (size_t i = 0; i < BUDGET_CATEGORY_COUNT; i++) {
            repaired = repaired || budget->categories[i].current != 0
                || budget->categories[i].active_allocations != 0;
        }
        if (repaired) budget->accounting_repair_count++;
        budget->current = 0;
        budget->external_reserved = 0;
        if (budget == lexbor_budget) lexbor_active_allocations = 0;
        for (size_t i = 0; i < BUDGET_CATEGORY_COUNT; i++) {
            budget->categories[i].current = 0;
            budget->categories[i].active_allocations = 0;
        }
    }
    budget->free_count++;
    free(header);
}

size_t budget_usable_size(const void *ptr)
{
    AllocationHeader *header = header_from_payload(ptr);
    if (header == NULL || header->data.magic != BUDGET_MAGIC) {
        return 0;
    }
    return header->data.size;
}

bool budget_owns(const Budget *budget, const void *ptr)
{
    AllocationHeader *header = header_from_payload(ptr);
    return budget != NULL && header != NULL
        && header->data.magic == BUDGET_MAGIC
        && header->data.owner == budget;
}

size_t budget_remaining(const Budget *budget)
{
    if (budget == NULL || budget->current >= budget->limit) {
        return 0;
    }
    return budget->limit - budget->current;
}

static size_t saturating_add(size_t left, size_t right)
{
    return right > SIZE_MAX - left ? SIZE_MAX : left + right;
}

bool budget_pressure_required(const Budget *budget, size_t working_bytes,
                              size_t reserve_bytes)
{
    return budget_remaining(budget)
           < saturating_add(working_bytes, reserve_bytes);
}

const char *budget_pressure_reason_name(BudgetPressureReason reason)
{
    static const char *const names[BUDGET_PRESSURE_COUNT] = {
        "javascript", "stylesheet", "image", "tile", "cache", "history",
        "speculation"
    };
    return (unsigned) reason < BUDGET_PRESSURE_COUNT
        ? names[reason] : "invalid";
}

void budget_record_pressure(Budget *budget, BudgetPressureReason reason,
                            size_t avoided_bytes, size_t saved_bytes)
{
    if (budget == NULL || reason >= BUDGET_PRESSURE_COUNT) return;
    BudgetPressureStats *stats = &budget->pressure[reason];
    stats->decisions = saturating_add(stats->decisions, 1);
    stats->avoided_bytes = saturating_add(stats->avoided_bytes,
                                          avoided_bytes);
    stats->saved_bytes = saturating_add(stats->saved_bytes, saved_bytes);
    budget->pressure_decisions = saturating_add(
        budget->pressure_decisions, 1);
    budget->pressure_avoided_bytes = saturating_add(
        budget->pressure_avoided_bytes, avoided_bytes);
    budget->pressure_saved_bytes = saturating_add(
        budget->pressure_saved_bytes, saved_bytes);
}

void budget_report_pressure(const Budget *budget, FILE *stream)
{
    if (budget == NULL || stream == NULL) return;
    fprintf(stream,
            "pressure-summary decisions=%zu avoided-bytes=%zu saved-bytes=%zu\n",
            budget->pressure_decisions, budget->pressure_avoided_bytes,
            budget->pressure_saved_bytes);
    for (size_t i = 0; i < BUDGET_PRESSURE_COUNT; i++) {
        const BudgetPressureStats *stats = &budget->pressure[i];
        if (stats->decisions == 0) continue;
        fprintf(stream,
                "pressure-reason name=%s decisions=%zu avoided-bytes=%zu "
                "saved-bytes=%zu\n",
                budget_pressure_reason_name((BudgetPressureReason) i),
                stats->decisions, stats->avoided_bytes, stats->saved_bytes);
    }
}

size_t budget_active_allocations(const Budget *budget,
                                 size_t *largest_payload)
{
    size_t count = 0, largest = 0;
    for (const AllocationHeader *header = budget == NULL ? NULL
         : budget->allocation_head; header != NULL;
         header = header->data.next) {
        count++;
        if (header->data.size > largest) largest = header->data.size;
    }
    if (largest_payload != NULL) *largest_payload = largest;
    return count;
}

void budget_report_active(const Budget *budget, BudgetCategory category,
                          size_t minimum_size, FILE *stream)
{
    if (budget == NULL || stream == NULL) return;
    for (const AllocationHeader *header = budget->allocation_head;
         header != NULL; header = header->data.next) {
        if ((BudgetCategory) (header->data.category
                              & BUDGET_CATEGORY_VALUE_MASK) != category
            || header->data.size < minimum_size) {
            continue;
        }
        fprintf(stream,
                "active-allocation category=%s size=%zu owner=%u "
                "sequence=%llu\n",
                category_names[header->data.category
                               & BUDGET_CATEGORY_VALUE_MASK],
                header->data.size,
                (unsigned int) header_allocation_owner(header),
                (unsigned long long) header->data.sequence);
    }
}

uint64_t budget_checkpoint(const Budget *budget)
{
    return budget == NULL ? 0 : budget->next_sequence;
}

void budget_rollback(Budget *budget, uint64_t checkpoint)
{
    if (budget == NULL) return;
    AllocationHeader *header = budget->allocation_head;
    while (header != NULL) {
        /* Headers are inserted at the head and realloc preserves sequence and
           list position. Once this ordered walk reaches the checkpoint, no
           remaining allocation can belong to the rollback window. */
        if (header->data.sequence <= checkpoint) break;
        AllocationHeader *next = header->data.next;
        if (header->data.sequence > checkpoint
            && !header_is_external(header)) {
            budget_free(budget, header + 1);
        }
        header = next;
    }
}

void budget_rollback_category(Budget *budget, uint64_t checkpoint,
                              BudgetCategory category)
{
    if (budget == NULL) return;
    category = valid_category(category);
    AllocationHeader *header = budget->allocation_head;
    while (header != NULL) {
        if (header->data.sequence <= checkpoint) break;
        AllocationHeader *next = header->data.next;
        if (header->data.sequence > checkpoint
            && header_category(header) == category
            && !header_is_external(header)) {
            budget_free(budget, header + 1);
        }
        header = next;
    }
}

bool budget_allocation_owner_create(Budget *budget,
                                    BudgetAllocationOwner *owner)
{
    if (budget == NULL || owner == NULL
        || budget->next_allocation_owner == BUDGET_ALLOCATION_OWNER_MAX)
        return false;
    budget->next_allocation_owner++;
    *owner = budget->next_allocation_owner;
    return true;
}

BudgetAllocationOwner budget_allocation_owner_enter(
    Budget *budget, BudgetAllocationOwner owner)
{
    if (budget == NULL) return BUDGET_ALLOCATION_OWNER_NONE;
    BudgetAllocationOwner previous = budget->active_allocation_owner;
    budget->active_allocation_owner = owner <= BUDGET_ALLOCATION_OWNER_MAX
        ? owner : BUDGET_ALLOCATION_OWNER_NONE;
    return previous;
}

void budget_allocation_owner_leave(Budget *budget,
                                    BudgetAllocationOwner previous_owner)
{
    if (budget == NULL) return;
    budget->active_allocation_owner =
        previous_owner <= BUDGET_ALLOCATION_OWNER_MAX
            ? previous_owner : BUDGET_ALLOCATION_OWNER_NONE;
}

void budget_rollback_owner_category(Budget *budget,
                                    BudgetAllocationOwner owner,
                                    BudgetCategory category)
{
    if (budget == NULL || owner == BUDGET_ALLOCATION_OWNER_NONE
        || owner > BUDGET_ALLOCATION_OWNER_MAX) return;
    category = valid_category(category);
    AllocationHeader *header = budget->allocation_head;
    while (header != NULL) {
        AllocationHeader *next = header->data.next;
        if (header_allocation_owner(header) == owner
            && header_category(header) == category
            && !header_is_external(header)) {
            budget_free(budget, header + 1);
        }
        header = next;
    }
}

bool budget_categories_reconcile(const Budget *budget)
{
    if (budget == NULL || budget->accounting_repair_count != 0) return false;
    size_t total = 0, active = 0, external_reserved = 0;
    for (size_t i = 0; i < BUDGET_CATEGORY_COUNT; i++) {
        if (budget->categories[i].current > SIZE_MAX - total) return false;
        total += budget->categories[i].current;
        if (budget->categories[i].active_allocations > SIZE_MAX - active) {
            return false;
        }
        active += budget->categories[i].active_allocations;
    }
    for (const AllocationHeader *header = budget->allocation_head;
         header != NULL; header = header->data.next) {
        if (!header_is_external(header)) continue;
        if (header->data.size > SIZE_MAX - external_reserved) return false;
        external_reserved += header->data.size;
    }
    return total == budget->current
        && active == budget_active_allocations(budget, NULL)
        && external_reserved == budget->external_reserved;
}

static void budget_record_phase(Budget *budget, const char *phase)
{
    if (budget == NULL || phase == NULL) return;
    for (size_t i = 0; i < BUDGET_CATEGORY_COUNT; i++) {
        BudgetCategoryStats *stats = &budget->categories[i];
        if (stats->peak <= stats->phase_peak) continue;
        stats->phase_peak = stats->peak;
        (void) snprintf(stats->high_water_phase,
                        sizeof(stats->high_water_phase), "%s", phase);
    }
}

void budget_mark_phase(Budget *budget, const char *phase)
{
    budget_record_phase(budget, phase);
}

void budget_report(Budget *budget, const char *phase, FILE *stream)
{
    if (budget == NULL || stream == NULL) return;
    budget_record_phase(budget, phase);
    const double mib = 1024.0 * 1024.0;
    fprintf(stream,
            "memory %-14s current=%6.2f MiB peak=%6.2f MiB remaining=%6.2f MiB allocs=%zu frees=%zu failures=%zu repairs=%zu external-reserved=%zu\n",
            phase,
            (double) budget->current / mib,
            (double) budget->peak / mib,
            (double) budget_remaining(budget) / mib,
            budget->allocation_count,
            budget->free_count,
            budget->failure_count, budget->accounting_repair_count,
            budget->external_reserved);
}

void budget_report_categories(Budget *budget, const char *phase,
                              FILE *stream)
{
    if (budget == NULL || stream == NULL) return;
    budget_record_phase(budget, phase);
    size_t current_sum = 0, global_peak_sum = 0;
    size_t payloads[BUDGET_CATEGORY_COUNT] = {0};
    size_t largest[BUDGET_CATEGORY_COUNT] = {0};
    for (const AllocationHeader *header = budget->allocation_head;
         header != NULL; header = header->data.next) {
        BudgetCategory category = header_category(header);
        payloads[category] += header->data.size;
        if (header->data.size > largest[category]) {
            largest[category] = header->data.size;
        }
    }
    for (size_t i = 0; i < BUDGET_CATEGORY_COUNT; i++) {
        current_sum += budget->categories[i].current;
        global_peak_sum += budget->global_peak_categories[i];
    }
    fprintf(stream,
            "memory-categories phase=%s current=%zu expected=%zu "
            "global-peak=%zu expected-peak=%zu repairs=%zu "
            "external-reserved=%zu reconcile=%s\n",
            phase == NULL ? "unknown" : phase, current_sum, budget->current,
            global_peak_sum, budget->peak, budget->accounting_repair_count,
            budget->external_reserved,
            budget_categories_reconcile(budget) ? "yes" : "no");
    for (size_t i = 0; i < BUDGET_CATEGORY_COUNT; i++) {
        const BudgetCategoryStats *stats = &budget->categories[i];
        fprintf(stream,
                "memory-category name=%s current=%zu peak=%zu "
                "global-peak=%zu payload=%zu overhead=%zu largest=%zu "
                "active=%zu allocs=%zu frees=%zu high-water=%s\n",
                budget_category_name((BudgetCategory) i), stats->current,
                stats->peak, budget->global_peak_categories[i],
                payloads[i], stats->current - payloads[i], largest[i],
                stats->active_allocations, stats->allocation_count,
                stats->free_count,
                stats->high_water_phase[0] == '\0'
                    ? "none" : stats->high_water_phase);
    }
}

void budget_dump_active(const Budget *budget, FILE *stream,
                        size_t maximum_entries)
{
    if (budget == NULL || stream == NULL || maximum_entries == 0) return;
    const AllocationHeader *header = budget->allocation_head;
    size_t count = 0;
    while (header != NULL && count < maximum_entries) {
        fprintf(stream,
                "active-allocation sequence=%llu owner=%u payload=%zu "
                "category=%s\n",
                (unsigned long long) header->data.sequence,
                (unsigned) header_allocation_owner(header),
                header->data.size,
                budget_category_name(header_category(header)));
        header = header->data.next;
        count++;
    }
    if (header != NULL) {
        fprintf(stream, "active-allocation remaining=yes shown=%zu\n",
                count);
    }
}

typedef struct BudgetConcurrentPoolState BudgetConcurrentPoolState;

typedef union {
    struct {
        BudgetConcurrentPoolState *owner;
        size_t size;
        uint32_t magic;
    } data;
    max_align_t alignment;
} BudgetConcurrentHeader;

struct BudgetConcurrentPoolState {
    uint32_t magic;
    BudgetReservation reservation;
    size_t limit;
    size_t current;
    size_t peak;
    size_t active_allocations;
    size_t allocation_count;
    size_t free_count;
    size_t failure_count;
    bool active;
#if defined(__PSP__)
    SceUID lock_semaphore;
#else
    atomic_flag lock;
#endif
};

_Static_assert(sizeof(BudgetConcurrentPoolState)
                   <= BUDGET_CONCURRENT_POOL_STORAGE_SIZE,
               "BudgetConcurrentPool opaque storage is too small");

static BudgetConcurrentPoolState *concurrent_pool_state(
    const BudgetConcurrentPool *pool)
{
    if (pool == NULL) return NULL;
    BudgetConcurrentPoolState *state =
        (BudgetConcurrentPoolState *) (void *) pool->bytes;
    return state->magic == BUDGET_CONCURRENT_STATE_MAGIC ? state : NULL;
}

static bool concurrent_pool_lock(BudgetConcurrentPoolState *state)
{
#if defined(__PSP__)
    /* PSP user threads are strict-priority scheduled. A higher-priority
       worker spinning here can permanently starve the lock owner. WaitSema
       already blocks for ordinary contention; a negative result is a broken
       semaphore/lifecycle boundary, not a reason to retry forever. */
    return sceKernelWaitSema(state->lock_semaphore, 1, NULL) >= 0;
#else
    while (atomic_flag_test_and_set_explicit(
               &state->lock, memory_order_acquire)) {
    }
    return true;
#endif
}

static void concurrent_pool_unlock(BudgetConcurrentPoolState *state)
{
#if defined(__PSP__)
    (void) sceKernelSignalSema(state->lock_semaphore, 1);
#else
    atomic_flag_clear_explicit(&state->lock, memory_order_release);
#endif
}

static void concurrent_pool_record_failure(BudgetConcurrentPoolState *state)
{
    if (state == NULL) return;
    if (!concurrent_pool_lock(state)) return;
    state->failure_count++;
    concurrent_pool_unlock(state);
}

bool budget_concurrent_pool_init(BudgetConcurrentPool *pool, Budget *budget,
                                 BudgetCategory category, size_t limit)
{
    if (pool == NULL || concurrent_pool_state(pool) != NULL
        || budget == NULL || limit == 0) {
        return false;
    }
    memset(pool, 0, sizeof(*pool));
    BudgetConcurrentPoolState *state =
        (BudgetConcurrentPoolState *) (void *) pool->bytes;
#if defined(__PSP__)
    state->lock_semaphore = sceKernelCreateSema(
        "tilefinch_budget", 0, 1, 1, NULL);
    if (state->lock_semaphore < 0) {
        memset(pool, 0, sizeof(*pool));
        return false;
    }
#else
    atomic_flag_clear_explicit(&state->lock, memory_order_release);
#endif
    state->magic = BUDGET_CONCURRENT_STATE_MAGIC;
    if (!budget_reservation_acquire(
            &state->reservation, budget, category, limit)) {
#if defined(__PSP__)
        (void) sceKernelDeleteSema(state->lock_semaphore);
#endif
        memset(pool, 0, sizeof(*pool));
        return false;
    }
    state->limit = limit;
    state->active = true;
    return true;
}

void *budget_concurrent_pool_malloc(BudgetConcurrentPool *pool, size_t size)
{
    BudgetConcurrentPoolState *state = concurrent_pool_state(pool);
    if (state == NULL) return NULL;
    if (size == 0) size = 1;
    if (size > SIZE_MAX - sizeof(BudgetConcurrentHeader)) {
        concurrent_pool_record_failure(state);
        return NULL;
    }
    size_t charge = sizeof(BudgetConcurrentHeader) + size;
    if (!concurrent_pool_lock(state)) return NULL;
    if (!state->active || charge > state->limit - state->current) {
        state->failure_count++;
        concurrent_pool_unlock(state);
        return NULL;
    }
    /* Reserve before calling malloc so simultaneous workers cannot transiently
       exceed the pool's physical allocation ceiling. */
    state->current += charge;
    state->active_allocations++;
    state->allocation_count++;
    if (state->current > state->peak) state->peak = state->current;
#if !defined(__PSP__)
    concurrent_pool_unlock(state);
#endif

    BudgetConcurrentHeader *header = malloc(charge);
    if (header == NULL) {
#if !defined(__PSP__)
        if (!concurrent_pool_lock(state)) return NULL;
#endif
        state->current -= charge;
        state->active_allocations--;
        state->allocation_count--;
        state->failure_count++;
        concurrent_pool_unlock(state);
        return NULL;
    }
    header->data.owner = state;
    header->data.size = size;
    header->data.magic = BUDGET_CONCURRENT_MAGIC;
#if defined(__PSP__)
    /* PSP newlib's allocator is process-global and not reentrant. Keep the
       same blocking semaphore across the raw allocation, not merely the
       accounting update, now that curl can allocate on a transport worker
       while the browser thread owns another easy handle. */
    concurrent_pool_unlock(state);
#endif
    return header + 1;
}

void *budget_concurrent_pool_calloc(BudgetConcurrentPool *pool,
                                    size_t count, size_t size)
{
    size_t total;
    BudgetConcurrentPoolState *state = concurrent_pool_state(pool);
    if (!checked_product(count, size, &total)) {
        concurrent_pool_record_failure(state);
        return NULL;
    }
    void *pointer = budget_concurrent_pool_malloc(pool, total);
    if (pointer != NULL) memset(pointer, 0, total == 0 ? 1 : total);
    return pointer;
}

void budget_concurrent_pool_free(BudgetConcurrentPool *pool, void *ptr)
{
    if (ptr == NULL) return;
    BudgetConcurrentPoolState *state = concurrent_pool_state(pool);
    BudgetConcurrentHeader *header = ((BudgetConcurrentHeader *) ptr) - 1;
    if (state == NULL || header->data.magic != BUDGET_CONCURRENT_MAGIC
        || header->data.owner != state) return;
    size_t charge = sizeof(*header) + header->data.size;
    if (!concurrent_pool_lock(state)) return;
    if (!state->active || state->current < charge
        || state->active_allocations == 0) {
        state->failure_count++;
        concurrent_pool_unlock(state);
        return;
    }
    header->data.magic = 0;
    state->current -= charge;
    state->active_allocations--;
    state->free_count++;
#if !defined(__PSP__)
    concurrent_pool_unlock(state);
#endif
    free(header);
#if defined(__PSP__)
    concurrent_pool_unlock(state);
#endif
}

void *budget_concurrent_pool_realloc(BudgetConcurrentPool *pool, void *ptr,
                                     size_t size)
{
    if (ptr == NULL) return budget_concurrent_pool_malloc(pool, size);
    if (size == 0) {
        budget_concurrent_pool_free(pool, ptr);
        return NULL;
    }
    BudgetConcurrentPoolState *state = concurrent_pool_state(pool);
    BudgetConcurrentHeader *old_header =
        ((BudgetConcurrentHeader *) ptr) - 1;
    if (state == NULL || old_header->data.magic != BUDGET_CONCURRENT_MAGIC
        || old_header->data.owner != state
        || size > SIZE_MAX - sizeof(*old_header)) return NULL;
    size_t old_charge = sizeof(*old_header) + old_header->data.size;
    size_t new_charge = sizeof(*old_header) + size;
    size_t growth = new_charge > old_charge ? new_charge - old_charge : 0;
#if defined(__PSP__)
    if (!concurrent_pool_lock(state)) return NULL;
    if (growth != 0
        && (!state->active || growth > state->limit - state->current)) {
        state->failure_count++;
        concurrent_pool_unlock(state);
        return NULL;
    }
    if (growth != 0) {
        state->current += growth;
        if (state->current > state->peak) state->peak = state->current;
    }
#else
    if (growth != 0) {
        if (!concurrent_pool_lock(state)) return NULL;
        if (!state->active || growth > state->limit - state->current) {
            state->failure_count++;
            concurrent_pool_unlock(state);
            return NULL;
        }
        state->current += growth;
        if (state->current > state->peak) state->peak = state->current;
        concurrent_pool_unlock(state);
    }
#endif
    BudgetConcurrentHeader *new_header = realloc(old_header, new_charge);
    if (new_header == NULL) {
#if !defined(__PSP__)
        if (!concurrent_pool_lock(state)) return NULL;
#endif
        if (growth != 0) state->current -= growth;
        state->failure_count++;
        concurrent_pool_unlock(state);
        return NULL;
    }
    new_header->data.owner = state;
    new_header->data.size = size;
    new_header->data.magic = BUDGET_CONCURRENT_MAGIC;
    if (new_charge < old_charge) {
#if !defined(__PSP__)
        if (!concurrent_pool_lock(state)) return new_header + 1;
#endif
        state->current -= old_charge - new_charge;
#if !defined(__PSP__)
        concurrent_pool_unlock(state);
#endif
    }
#if defined(__PSP__)
    concurrent_pool_unlock(state);
#endif
    return new_header + 1;
}

size_t budget_concurrent_pool_usable_size(
    const BudgetConcurrentPool *pool, const void *ptr)
{
    if (ptr == NULL) return 0;
    BudgetConcurrentPoolState *state = concurrent_pool_state(pool);
    const BudgetConcurrentHeader *header =
        ((const BudgetConcurrentHeader *) ptr) - 1;
    return state != NULL && header->data.magic == BUDGET_CONCURRENT_MAGIC
            && header->data.owner == state
        ? header->data.size : 0;
}

bool budget_concurrent_pool_metrics(const BudgetConcurrentPool *pool,
                                    BudgetConcurrentPoolMetrics *metrics)
{
    BudgetConcurrentPoolState *state = concurrent_pool_state(pool);
    if (state == NULL || metrics == NULL) return false;
    if (!concurrent_pool_lock(state)) return false;
    *metrics = (BudgetConcurrentPoolMetrics) {
        .limit = state->limit,
        .current = state->current,
        .peak = state->peak,
        .active_allocations = state->active_allocations,
        .allocation_count = state->allocation_count,
        .free_count = state->free_count,
        .failure_count = state->failure_count
    };
    concurrent_pool_unlock(state);
    return true;
}

bool budget_concurrent_pool_destroy(BudgetConcurrentPool *pool)
{
    BudgetConcurrentPoolState *state = concurrent_pool_state(pool);
    if (state == NULL) return false;
    if (!concurrent_pool_lock(state)) return false;
    bool clean = state->active && state->current == 0
        && state->active_allocations == 0
        && state->allocation_count == state->free_count;
    if (clean) state->active = false;
    concurrent_pool_unlock(state);
    if (!clean) return false;
    budget_reservation_release(&state->reservation);
#if defined(__PSP__)
    (void) sceKernelDeleteSema(state->lock_semaphore);
#endif
    memset(pool, 0, sizeof(*pool));
    return true;
}

static void *lexbor_malloc_hook(size_t size)
{
    void *pointer = budget_malloc_category(
        lexbor_budget, BUDGET_CATEGORY_DOM, size);
    header_mark_lexbor(pointer);
    return pointer;
}

static void *lexbor_realloc_hook(void *ptr, size_t size)
{
    if (ptr == NULL) return lexbor_malloc_hook(size);
    if (size == 0) {
        budget_free(lexbor_budget, ptr);
        return NULL;
    }
    return budget_realloc_category(
        lexbor_budget, BUDGET_CATEGORY_DOM, ptr, size);
}

static void *lexbor_calloc_hook(size_t count, size_t size)
{
    void *pointer = budget_calloc_category(
        lexbor_budget, BUDGET_CATEGORY_DOM, count, size);
    header_mark_lexbor(pointer);
    return pointer;
}

static void lexbor_free_hook(void *ptr)
{
    budget_free(lexbor_budget, ptr);
}

bool budget_install_lexbor(Budget *budget)
{
    if (budget == NULL) return false;
    if (lexbor_budget == budget) return true;
    if (lexbor_active_allocations != 0) return false;
    Budget *previous = lexbor_budget;
    lexbor_budget = budget;
    if (lexbor_memory_setup(lexbor_malloc_hook, lexbor_realloc_hook,
                            lexbor_calloc_hook, lexbor_free_hook)
            != LXB_STATUS_OK) {
        lexbor_budget = previous;
        return false;
    }
    return true;
}

bool budget_uninstall_lexbor(Budget *budget)
{
    if (budget == NULL || lexbor_budget != budget
        || lexbor_active_allocations != 0) return false;
    /* Keep the hooks installed but make unowned allocation fail closed. This
       avoids falling back to an allocator which escapes the shared budget. */
    lexbor_budget = NULL;
    return true;
}

bool budget_lexbor_is_installed(const Budget *budget)
{
    return budget != NULL && lexbor_budget == budget;
}

#if !defined(PSP_BROWSER_BELLARD_QUICKJS)
static void *js_calloc_hook(void *opaque, size_t count, size_t size)
{
    return budget_calloc_category(
        (Budget *) opaque, BUDGET_CATEGORY_JAVASCRIPT, count, size);
}
#endif

static void *js_malloc_hook(void *opaque, size_t size)
{
    return budget_malloc_category(
        (Budget *) opaque, BUDGET_CATEGORY_JAVASCRIPT, size);
}

static void js_free_hook(void *opaque, void *ptr)
{
    budget_free((Budget *) opaque, ptr);
}

static void *js_realloc_hook(void *opaque, void *ptr, size_t size)
{
    return budget_realloc_category(
        (Budget *) opaque, BUDGET_CATEGORY_JAVASCRIPT, ptr, size);
}

static size_t js_usable_size_hook(const void *ptr)
{
    return budget_usable_size(ptr);
}

#if defined(PSP_BROWSER_BELLARD_QUICKJS)
static bool bellard_growth_allowed(const JSMallocState *state,
                                   size_t old_size, size_t new_size)
{
    if (state == NULL) return false;
    if (new_size <= old_size) return true;
    size_t growth = new_size - old_size;
    return state->malloc_size <= state->malloc_limit
        && growth <= state->malloc_limit - state->malloc_size;
}

static void *bellard_malloc(JSMallocState *state, size_t size)
{
    /* Bellard QuickJS delegates JS_SetMemoryLimit enforcement to a custom
       allocator. Keep the exported non-pool adapter honest too; benchmarks
       are callers, and future production use must not silently escape the
       configured realm limit. */
    if (size == 0 || !bellard_growth_allowed(state, 0, size)) return NULL;
    void *pointer = js_malloc_hook(state->opaque, size);
    if (pointer != NULL) {
        state->malloc_count++;
        state->malloc_size += js_usable_size_hook(pointer);
    }
    return pointer;
}

static void bellard_free(JSMallocState *state, void *pointer)
{
    if (pointer != NULL) {
        state->malloc_count--;
        state->malloc_size -= js_usable_size_hook(pointer);
    }
    js_free_hook(state->opaque, pointer);
}

static void *bellard_realloc(JSMallocState *state, void *pointer, size_t size)
{
    if (pointer == NULL)
        return size == 0 ? NULL : bellard_malloc(state, size);
    size_t old_size = js_usable_size_hook(pointer);
    if (old_size == 0) return NULL;
    if (size == 0) {
        bellard_free(state, pointer);
        return NULL;
    }
    if (!bellard_growth_allowed(state, old_size, size)) return NULL;
    void *resized = js_realloc_hook(state->opaque, pointer, size);
    if (resized == NULL) return NULL;
    state->malloc_size -= old_size;
    state->malloc_size += js_usable_size_hook(resized);
    return resized;
}
#endif

const JSMallocFunctions *budget_quickjs_allocator(void)
{
#if defined(PSP_BROWSER_BELLARD_QUICKJS)
    static const JSMallocFunctions functions = {
        .js_malloc = bellard_malloc,
        .js_free = bellard_free,
        .js_realloc = bellard_realloc,
        .js_malloc_usable_size = js_usable_size_hook,
    };
#else
    static const JSMallocFunctions functions = {
        .js_calloc = js_calloc_hook,
        .js_malloc = js_malloc_hook,
        .js_free = js_free_hook,
        .js_realloc = js_realloc_hook,
        .js_malloc_usable_size = js_usable_size_hook,
    };
#endif
    return &functions;
}

#define QUICKJS_POOL_CLASS_COUNT 28
#define QUICKJS_POOL_DIRECT_CLASS UINT16_MAX
#define QUICKJS_POOL_MAX_SIZE 1024u
#define QUICKJS_POOL_MAGIC UINT16_C(0x5150)

static const size_t quickjs_pool_class_sizes[QUICKJS_POOL_CLASS_COUNT] = {
    16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128,
    160, 192, 224, 256, 320, 384, 512, 640, 768, 1024,
    1536, 2048, 3072, 4096, 5120, 8192, 16384
};

typedef union QuickJSPoolHeader QuickJSPoolHeader;
union QuickJSPoolHeader {
    struct {
        QuickJSPoolHeader *next;
        uint32_t requested;
        uint16_t class_index;
        uint16_t magic;
    } data;
    max_align_t alignment;
};

struct BudgetQuickJSPool {
    Budget *budget;
    size_t reserved;
    size_t reserved_peak;
    QuickJSPoolHeader *free_lists[QUICKJS_POOL_CLASS_COUNT];
    uint8_t cached_counts[QUICKJS_POOL_CLASS_COUNT];
    uint8_t small_cache_limit;
    uint8_t large_cache_limit;
    uint8_t class_table[QUICKJS_POOL_MAX_SIZE + 1];
    /* Live-allocation census: requested vs class-capacity bytes per class,
       for locating where rounding waste concentrates. */
    size_t live_count[QUICKJS_POOL_CLASS_COUNT + 1];
    size_t live_requested[QUICKJS_POOL_CLASS_COUNT + 1];
    size_t live_capacity[QUICKJS_POOL_CLASS_COUNT + 1];
    /* High-water mark of the enforced QuickJS malloc_size, sampled at
       allocation time: the transient peak between GC checkpoints that a
       boot window must actually cover. */
    size_t js_malloc_peak;
    size_t js_malloc_current;
    size_t largest_request;
    size_t peak_dump_watermark;
};

static uint16_t quickjs_pool_class_for(const BudgetQuickJSPool *pool,
                                       size_t size)
{
    if (size <= QUICKJS_POOL_MAX_SIZE)
        return pool->class_table[size];
    for (uint16_t index = 21; index < QUICKJS_POOL_CLASS_COUNT; index++) {
        if (size <= quickjs_pool_class_sizes[index])
            return index;
    }
    return QUICKJS_POOL_DIRECT_CLASS;
}

/* Device-parity failure injection (diagnostic, host-only): report that the
   pool would exceed a simulated physical heap wall of
   TILEFINCH_JS_POOL_FAIL_OVER_KB once 'growth' more payload bytes are
   reserved.  Mimics the PSP's malloc wall rather than the JS ceiling,
   which fails through a different, cleanly-thrown path. */
static bool quickjs_pool_injected_wall(const BudgetQuickJSPool *pool,
                                       size_t growth)
{
#if defined(__PSP__)
    (void) pool;
    (void) growth;
    return false;
#else
    static long fail_over_kb = -2;
    if (fail_over_kb == -2) {
        const char *value = getenv("TILEFINCH_JS_POOL_FAIL_OVER_KB");
        fail_over_kb = value == NULL ? -1 : atol(value);
    }
    return fail_over_kb > 0
        && pool->reserved + sizeof(QuickJSPoolHeader) + growth
               > (size_t) fail_over_kb * 1024u;
#endif
}

static void *quickjs_pool_malloc(void *opaque, size_t size)
{
    BudgetQuickJSPool *pool = opaque;
    if (pool == NULL) return NULL;
    if (size == 0) size = 1;
    if (size > UINT32_MAX - sizeof(QuickJSPoolHeader)) {
        pool->budget->failure_count++;
        return NULL;
    }
    uint16_t class_index = quickjs_pool_class_for(pool, size);
    size_t capacity = class_index == QUICKJS_POOL_DIRECT_CLASS
        ? size : quickjs_pool_class_sizes[class_index];
    if (quickjs_pool_injected_wall(pool, capacity)
        && (class_index == QUICKJS_POOL_DIRECT_CLASS
            || pool->free_lists[class_index] == NULL)) {
        pool->budget->failure_count++;
        return NULL;
    }
    QuickJSPoolHeader *header;
    if (class_index != QUICKJS_POOL_DIRECT_CLASS
        && pool->free_lists[class_index] != NULL) {
        /* Cached blocks bypass the underlying Budget allocation, so consume
           the same deterministic failure cadence explicitly. */
        if (budget_should_inject_failure(pool->budget)) return NULL;
        header = pool->free_lists[class_index];
        pool->free_lists[class_index] = header->data.next;
        if (pool->cached_counts[class_index] > 0)
            pool->cached_counts[class_index]--;
    } else {
        size_t charge = sizeof(*header) + capacity;
        size_t injected_before = pool->budget->injected_failure_count;
        header = budget_malloc_category(
            pool->budget, BUDGET_CATEGORY_JAVASCRIPT, charge);
        if (header == NULL) {
            /* Last-ditch, allocation-safe recovery: the class caches can
               hold megabytes of idle blocks.  Release every cached block
               back to the underlying heap and retry once before the page
               sees an out-of-memory error.  Matters most on devices where
               the physical heap, not the configured ceiling, is the
               binding constraint.  TILEFINCH_JS_POOL_TRIM_RETRY=0 opts out.
               (Continuing past the first would-be OOM once trampled a
               JSString on the PSP build; that was the JS stack guard
               leaving too little physical stack margin for the OOM
               exception recursion — the guard now keeps a full megabyte
               of slack below the thread stack.) */
            bool injected_failure =
                pool->budget->injected_failure_count != injected_before;
            static int retry_enabled = -1;
            if (retry_enabled < 0) {
                const char *value = getenv("TILEFINCH_JS_POOL_TRIM_RETRY");
                retry_enabled = value == NULL || value[0] != '0';
            }
            if (!injected_failure && retry_enabled
                && budget_quickjs_pool_trim(pool, 0) != 0) {
                header = budget_malloc_category(
                    pool->budget, BUDGET_CATEGORY_JAVASCRIPT, charge);
            }
            if (header == NULL) return NULL;
        }
        pool->reserved += charge;
        if (pool->reserved > pool->reserved_peak) {
            pool->reserved_peak = pool->reserved;
        }
    }
    header->data.requested = (uint32_t) size;
    header->data.class_index = class_index;
    header->data.magic = QUICKJS_POOL_MAGIC;
    {
        size_t slot = class_index == QUICKJS_POOL_DIRECT_CLASS
            ? QUICKJS_POOL_CLASS_COUNT : class_index;
        pool->live_count[slot]++;
        pool->live_requested[slot] += size;
        pool->live_capacity[slot] += capacity;
    }
    return header + 1;
}

#if !defined(PSP_BROWSER_BELLARD_QUICKJS)
static void *quickjs_pool_calloc(void *opaque, size_t count, size_t size)
{
    BudgetQuickJSPool *pool = opaque;
    size_t total;
    if (!checked_product(count, size, &total)) {
        if (pool != NULL) pool->budget->failure_count++;
        return NULL;
    }
    void *value = quickjs_pool_malloc(opaque, total);
    if (value != NULL) memset(value, 0, total == 0 ? 1 : total);
    return value;
}
#endif

static void quickjs_pool_free(void *opaque, void *ptr)
{
    if (ptr == NULL) return;
    BudgetQuickJSPool *pool = opaque;
    QuickJSPoolHeader *header = (QuickJSPoolHeader *) ptr - 1;
    if (pool == NULL || header->data.magic != QUICKJS_POOL_MAGIC) return;
    uint16_t class_index = header->data.class_index;
    {
        size_t slot = class_index == QUICKJS_POOL_DIRECT_CLASS
            ? QUICKJS_POOL_CLASS_COUNT : class_index;
        size_t capacity = class_index == QUICKJS_POOL_DIRECT_CLASS
            ? header->data.requested
            : quickjs_pool_class_sizes[class_index];
        pool->live_count[slot]--;
        pool->live_requested[slot] -= header->data.requested;
        pool->live_capacity[slot] -= capacity;
    }
    if (class_index == QUICKJS_POOL_DIRECT_CLASS) {
        size_t charge = sizeof(*header) + header->data.requested;
        header->data.magic = 0;
        pool->reserved -= charge;
        budget_free(pool->budget, header);
        return;
    }
    size_t capacity = quickjs_pool_class_sizes[class_index];
    size_t cache_limit = capacity > QUICKJS_POOL_MAX_SIZE
        ? pool->large_cache_limit : pool->small_cache_limit;
    if (pool->cached_counts[class_index] >= cache_limit) {
        size_t charge = sizeof(*header) + capacity;
        header->data.magic = 0;
        pool->reserved -= charge;
        budget_free(pool->budget, header);
        return;
    }
    header->data.next = pool->free_lists[class_index];
    pool->free_lists[class_index] = header;
    if (pool->cached_counts[class_index] < UINT8_MAX)
        pool->cached_counts[class_index]++;
}

static void *quickjs_pool_realloc(void *opaque, void *ptr, size_t size)
{
    if (ptr == NULL) return quickjs_pool_malloc(opaque, size);
    if (size == 0) {
        quickjs_pool_free(opaque, ptr);
        return NULL;
    }
    BudgetQuickJSPool *pool = opaque;
    QuickJSPoolHeader *header = (QuickJSPoolHeader *) ptr - 1;
    if (pool == NULL || header->data.magic != QUICKJS_POOL_MAGIC
        || size > UINT32_MAX - sizeof(QuickJSPoolHeader)) {
        if (pool != NULL) pool->budget->failure_count++;
        return NULL;
    }
    size_t old_size = header->data.requested;
    uint16_t old_class = header->data.class_index;
    size_t old_capacity = old_class == QUICKJS_POOL_DIRECT_CLASS
        ? old_size : quickjs_pool_class_sizes[old_class];
    if (old_class != QUICKJS_POOL_DIRECT_CLASS && size <= old_capacity) {
        if (budget_should_inject_failure(pool->budget)) return NULL;
        pool->live_requested[old_class] += size;
        pool->live_requested[old_class] -= old_size;
        header->data.requested = (uint32_t) size;
        return ptr;
    }
    uint16_t new_class = quickjs_pool_class_for(pool, size);
    if (old_class == QUICKJS_POOL_DIRECT_CLASS
        && new_class == QUICKJS_POOL_DIRECT_CLASS) {
        size_t old_charge = sizeof(*header) + old_size;
        size_t new_charge = sizeof(*header) + size;
        if (size > old_size
            && quickjs_pool_injected_wall(pool, size - old_size)) {
            pool->budget->failure_count++;
            return NULL;
        }
        QuickJSPoolHeader *replacement = budget_realloc_category(
            pool->budget, BUDGET_CATEGORY_JAVASCRIPT, header, new_charge);
        if (replacement == NULL) return NULL;
        replacement->data.requested = (uint32_t) size;
        replacement->data.class_index = QUICKJS_POOL_DIRECT_CLASS;
        replacement->data.magic = QUICKJS_POOL_MAGIC;
        pool->live_requested[QUICKJS_POOL_CLASS_COUNT] += size;
        pool->live_requested[QUICKJS_POOL_CLASS_COUNT] -= old_size;
        pool->live_capacity[QUICKJS_POOL_CLASS_COUNT] += size;
        pool->live_capacity[QUICKJS_POOL_CLASS_COUNT] -= old_size;
        pool->reserved = pool->reserved - old_charge + new_charge;
        if (pool->reserved > pool->reserved_peak) {
            pool->reserved_peak = pool->reserved;
        }
        return replacement + 1;
    }
    void *replacement = quickjs_pool_malloc(opaque, size);
    if (replacement == NULL) return NULL;
    memcpy(replacement, ptr, old_size < size ? old_size : size);
    quickjs_pool_free(opaque, ptr);
    return replacement;
}

static size_t quickjs_pool_usable_size(const void *ptr)
{
    if (ptr == NULL) return 0;
    const QuickJSPoolHeader *header =
        (const QuickJSPoolHeader *) ptr - 1;
    return header->data.magic == QUICKJS_POOL_MAGIC
        ? header->data.requested : 0;
}

BudgetQuickJSPool *budget_quickjs_pool_create(Budget *budget)
{
    if (budget == NULL) return NULL;
    BudgetQuickJSPool *pool = budget_calloc_category(
        budget, BUDGET_CATEGORY_JAVASCRIPT, 1, sizeof(*pool));
    if (pool == NULL) return NULL;
    pool->budget = budget;
    /* Preserve the existing allocator policy unless a constrained caller
       explicitly selects tighter cache bounds. */
    pool->small_cache_limit = UINT8_MAX;
    pool->large_cache_limit = 8;
    uint16_t class_index = 0;
    for (size_t size = 0; size <= QUICKJS_POOL_MAX_SIZE; size++) {
        while (class_index + 1 < QUICKJS_POOL_CLASS_COUNT
               && size > quickjs_pool_class_sizes[class_index]) {
            class_index++;
        }
        pool->class_table[size] = (uint8_t) class_index;
    }
    return pool;
}

bool budget_quickjs_pool_destroy(BudgetQuickJSPool *pool)
{
    if (pool == NULL) return true;
    for (size_t index = 0; index < QUICKJS_POOL_CLASS_COUNT; index++) {
        QuickJSPoolHeader *header = pool->free_lists[index];
        while (header != NULL) {
            QuickJSPoolHeader *next = header->data.next;
            size_t charge = sizeof(*header)
                + quickjs_pool_class_sizes[index];
            header->data.magic = 0;
            pool->reserved -= charge;
            budget_free(pool->budget, header);
            header = next;
        }
        pool->free_lists[index] = NULL;
        pool->cached_counts[index] = 0;
    }
    bool balanced = pool->reserved == 0;
    Budget *budget = pool->budget;
    budget_free(budget, pool);
    return balanced;
}

size_t budget_quickjs_pool_trim(BudgetQuickJSPool *pool,
                                size_t retain_per_class)
{
    if (pool == NULL) return 0;
    size_t released = 0;
    for (size_t index = 0; index < QUICKJS_POOL_CLASS_COUNT; index++) {
        while (pool->free_lists[index] != NULL
               && pool->cached_counts[index] > retain_per_class) {
            QuickJSPoolHeader *header = pool->free_lists[index];
            pool->free_lists[index] = header->data.next;
            size_t charge = sizeof(*header) + quickjs_pool_class_sizes[index];
            header->data.magic = 0;
            pool->reserved -= charge;
            released += charge;
            budget_free(pool->budget, header);
            pool->cached_counts[index]--;
        }
    }
    return released;
}

void budget_quickjs_pool_set_cache_limits(BudgetQuickJSPool *pool,
                                          size_t small_class_limit,
                                          size_t large_class_limit)
{
    if (pool == NULL) return;
    pool->small_cache_limit = (uint8_t) (small_class_limit > UINT8_MAX
        ? UINT8_MAX : small_class_limit);
    pool->large_cache_limit = (uint8_t) (large_class_limit > UINT8_MAX
        ? UINT8_MAX : large_class_limit);
}

#if defined(PSP_BROWSER_BELLARD_QUICKJS)
typedef struct {
    size_t old_size;
    size_t size;
    int64_t live;
    int64_t limit;
} BellardPoolReject;
/* No stdio in the allocator: printf allocates.  Rejections are recorded
   here and printed by budget_quickjs_pool_report_rejects. */
static BellardPoolReject bellard_pool_rejects[4];
static size_t bellard_pool_reject_count;

static void bellard_pool_note_request(void *opaque, size_t size);

/* Host diagnostic: lets the JS runtime print its interpreter stack at
   the allocation-size trap (reentrant into the allocator, host-only). */
static void (*budget_js_stack_dump_hook)(void *opaque);
static void *budget_js_stack_dump_opaque;

void budget_quickjs_pool_set_stack_dump_hook(void (*hook)(void *opaque),
                                             void *opaque)
{
    budget_js_stack_dump_hook = hook;
    budget_js_stack_dump_opaque = opaque;
}

static void bellard_pool_record_reject(const JSMallocState *state,
                                       size_t old_size, size_t size)
{
    if (bellard_pool_reject_count
        >= sizeof(bellard_pool_rejects) / sizeof(bellard_pool_rejects[0])) {
        return;
    }
    BellardPoolReject *reject =
        &bellard_pool_rejects[bellard_pool_reject_count++];
    reject->old_size = old_size;
    reject->size = size;
    reject->live = state->malloc_size;
    reject->limit = state->malloc_limit;
}

/* Big-request forensics: a lone exact-size malloc is a rope
   linearization (string_buffer_init2 of the rope length); a 1.5x
   realloc chain is an accumulating StringBuffer (JSON.stringify and
   friends grow by size*3/2).  Recorded allocation-free, printed with
   the rejects. */
typedef struct {
    size_t old_size;
    size_t size;
    int64_t live;
    uint8_t is_realloc;
} BellardPoolBigRequest;
#define BELLARD_POOL_BIG_REQUEST_FLOOR (1024u * 1024u)
static BellardPoolBigRequest bellard_pool_big_requests[16];
static size_t bellard_pool_big_request_count;

#define BELLARD_POOL_PREFIX_FLOOR (3u * 1024u * 1024u)
static unsigned char bellard_pool_prefixes[2][96];
static unsigned char bellard_pool_fragments[2][64];
static size_t bellard_pool_tag_census[6];
static size_t bellard_pool_fragment_origins[2];
static size_t bellard_pool_string_sample_count;
static size_t bellard_pool_prefix_sizes[2];
static size_t bellard_pool_prefix_count;

/* Allocation-safe: copies bytes out of the still-live old buffer of a
   giant realloc so the report can say what the string being built IS. */
static void bellard_pool_capture_prefix(const void *pointer, size_t old_size,
                                        size_t size)
{
#if !defined(__PSP__)
    (void) old_size;
#endif
    if (pointer == NULL || size < BELLARD_POOL_PREFIX_FLOOR
        || bellard_pool_prefix_count
               >= sizeof(bellard_pool_prefix_sizes)
                      / sizeof(bellard_pool_prefix_sizes[0])) {
        return;
    }
    memcpy(bellard_pool_prefixes[bellard_pool_prefix_count], pointer,
           sizeof(bellard_pool_prefixes[0]));
    bellard_pool_prefix_sizes[bellard_pool_prefix_count++] = size;
    /* The buffer is a JSValue fast-array on 32-bit: {u32 ptr, i32 tag}
       pairs.  Sample the first string-tagged elements and copy the raw
       JSString bytes so the report shows what the strings say. */
#if defined(__PSP__)
    /* Consecutive elements of the per-character array are consecutive
       characters of the source string: reassemble a readable fragment
       from two offsets so the report shows what text was exploded. */
    {
        const uint32_t *words = (const uint32_t *) pointer;
        size_t pairs = old_size / 8;
        static const size_t origins[] = { 0, 0 }; /* second slot patched below */
        for (size_t which = 0;
             which < 2 && bellard_pool_string_sample_count < 2; which++) {
            size_t origin = which == 0 ? origins[0]
                : (pairs > 96 ? pairs - 96 : 0);
            if (origin + 64 > pairs) continue;
            unsigned char *out =
                bellard_pool_fragments[bellard_pool_string_sample_count];
            for (size_t at = 0; at < 64; at++) {
                uint32_t value = words[(origin + at) * 2];
                uint32_t tag = words[(origin + at) * 2 + 1];
                unsigned char c = '?';
                if (tag == 0xfffffff9u && value >= 0x08800000u
                    && value < 0x0c000000u) {
                    const uint32_t *str = (const uint32_t *) (uintptr_t) value;
                    uint32_t len = str[0] & 0x7fffffffu;
                    int wide = (int) (str[0] >> 31);
                    const unsigned char *data =
                        (const unsigned char *) (str + 3);
                    c = len == 0 ? '~' : data[0];
                    if (wide && len != 0 && data[1] != 0) c = '#';
                } else if (tag == 0u) {
                    c = '0';
                }
                out[at] = c;
            }
            bellard_pool_fragment_origins[bellard_pool_string_sample_count] =
                origin;
            bellard_pool_string_sample_count++;
        }
        if (bellard_pool_tag_census[0] == 0) {
            size_t limit = pairs < 4096u ? pairs : 4096u;
            for (size_t at = 0; at < limit; at++) {
                uint32_t tag = words[at * 2 + 1];
                size_t bucket = tag == 0xfffffff9u ? 1
                    : tag == 0u ? 2
                    : tag == 0xffffffffu ? 3
                    : tag == 3u ? 4 : 5;
                bellard_pool_tag_census[bucket]++;
            }
            bellard_pool_tag_census[0] = limit;
        }
    }
#endif
}

static void bellard_pool_record_big_request(const JSMallocState *state,
                                            size_t old_size, size_t size,
                                            int is_realloc)
{
    if (size < BELLARD_POOL_BIG_REQUEST_FLOOR
        || bellard_pool_big_request_count
               >= sizeof(bellard_pool_big_requests)
                      / sizeof(bellard_pool_big_requests[0])) {
        return;
    }
    BellardPoolBigRequest *request =
        &bellard_pool_big_requests[bellard_pool_big_request_count++];
    request->old_size = old_size;
    request->size = size;
    request->live = state == NULL ? 0 : state->malloc_size;
    request->is_realloc = (uint8_t) is_realloc;
}

void budget_quickjs_pool_report_rejects(FILE *stream)
{
    for (size_t i = 0; i < bellard_pool_reject_count; i++) {
        fprintf(stream,
                "js-heap-reject old=%zu size=%zu live=%lld limit=%lld\n",
                bellard_pool_rejects[i].old_size, bellard_pool_rejects[i].size,
                bellard_pool_rejects[i].live, bellard_pool_rejects[i].limit);
    }
    for (size_t i = 0; i < bellard_pool_big_request_count; i++) {
        fprintf(stream,
                "js-big-request kind=%s old=%zu size=%zu live=%lld\n",
                bellard_pool_big_requests[i].is_realloc ? "realloc" : "malloc",
                bellard_pool_big_requests[i].old_size,
                bellard_pool_big_requests[i].size,
                bellard_pool_big_requests[i].live);
    }
    for (size_t i = 0; i < bellard_pool_prefix_count; i++) {
        fprintf(stream, "js-big-prefix old=%zu bytes=",
                bellard_pool_prefix_sizes[i]);
        for (size_t j = 0; j < sizeof(bellard_pool_prefixes[0]); j++) {
            unsigned char c = bellard_pool_prefixes[i][j];
            fputc(c >= 32 && c < 127 ? c : '.', stream);
        }
        fputc('\n', stream);
        fprintf(stream, "js-big-prefix-hex old=%zu bytes=",
                bellard_pool_prefix_sizes[i]);
        for (size_t j = 0; j < 24; j++) {
            fprintf(stream, "%02x", bellard_pool_prefixes[i][j]);
        }
        fputc('\n', stream);
    }
    for (size_t i = 0; i < bellard_pool_string_sample_count; i++) {
        fprintf(stream, "js-big-fragment origin=%zu text=",
                bellard_pool_fragment_origins[i]);
        for (size_t j = 0; j < sizeof(bellard_pool_fragments[0]); j++) {
            unsigned char c = bellard_pool_fragments[i][j];
            fputc(c >= 32 && c < 127 ? c : '.', stream);
        }
        fprintf(stream, " hex=");
        for (size_t j = 0; j < 32; j++) {
            fprintf(stream, "%02x", bellard_pool_fragments[i][j]);
        }
        fputc('\n', stream);
    }
    if (bellard_pool_tag_census[0] != 0) {
        fprintf(stream,
                "js-big-tags sampled=%zu string=%zu int=%zu object=%zu "
                "undefined=%zu other=%zu\n",
                bellard_pool_tag_census[0], bellard_pool_tag_census[1],
                bellard_pool_tag_census[2], bellard_pool_tag_census[3],
                bellard_pool_tag_census[4], bellard_pool_tag_census[5]);
    }
}

static void *bellard_pool_malloc(JSMallocState *state, size_t size)
{
    /* With a custom allocator Bellard QuickJS delegates enforcement of
       JS_SetMemoryLimit() to these callbacks.  The default allocator performs
       this check itself; omitting it turns the advertised page heap limit into
       telemetry only and lets script consume the entire shared browser budget. */
    if (size == 0 || !bellard_growth_allowed(state, 0, size)) {
        if (size != 0) bellard_pool_record_reject(state, 0, size);
        return NULL;
    }
    bellard_pool_note_request(state->opaque, size);
    bellard_pool_record_big_request(state, 0, size, 0);
    void *pointer = quickjs_pool_malloc(state->opaque, size);
    if (pointer != NULL) {
        state->malloc_count++;
        state->malloc_size += quickjs_pool_usable_size(pointer);
        BudgetQuickJSPool *pool = state->opaque;
        pool->js_malloc_current = state->malloc_size;
        if (state->malloc_size > pool->js_malloc_peak) {
            pool->js_malloc_peak = state->malloc_size;
            /* Peak-composition census: at each 8 MB high-water step,
               print the live size-band census so the transient peak's
               makeup is measurable, not inferred (TILEFINCH_DUMP_JS_POOL_AT_PEAK). */
            static int at_peak = -1;
            if (at_peak < 0) {
                at_peak = getenv("TILEFINCH_DUMP_JS_POOL_AT_PEAK") != NULL;
            }
            if (at_peak
                && (size_t) pool->js_malloc_peak
                       >= pool->peak_dump_watermark + 8u * 1024u * 1024u) {
                pool->peak_dump_watermark = (size_t) pool->js_malloc_peak;
                fprintf(stderr, "quickjs-pool high-water=%lld\n",
                        (long long) pool->js_malloc_peak);
                budget_quickjs_pool_report_classes(pool, stderr);
            }
        }
    }
    return pointer;
}

static void bellard_pool_free(JSMallocState *state, void *pointer)
{
    if (pointer != NULL) {
        state->malloc_count--;
        state->malloc_size -= quickjs_pool_usable_size(pointer);
        ((BudgetQuickJSPool *) state->opaque)->js_malloc_current =
            state->malloc_size;
    }
    quickjs_pool_free(state->opaque, pointer);
}

static void *bellard_pool_realloc(JSMallocState *state, void *pointer,
                                  size_t size)
{
    if (pointer == NULL) {
        return size == 0 ? NULL : bellard_pool_malloc(state, size);
    }
    size_t old_size = quickjs_pool_usable_size(pointer);
    if (old_size == 0) return NULL;
    if (size == 0) {
        bellard_pool_free(state, pointer);
        return NULL;
    }
    if (!bellard_growth_allowed(state, old_size, size)) {
        bellard_pool_record_reject(state, old_size, size);
        bellard_pool_capture_prefix(pointer, old_size, size);
        return NULL;
    }
    bellard_pool_note_request(state->opaque, size);
    bellard_pool_record_big_request(state, old_size, size, 1);
    bellard_pool_capture_prefix(pointer, old_size, size);
    void *resized = quickjs_pool_realloc(state->opaque, pointer, size);
    if (resized == NULL) return NULL;
    state->malloc_size -= old_size;
    state->malloc_size += quickjs_pool_usable_size(resized);
    ((BudgetQuickJSPool *) state->opaque)->js_malloc_current =
        state->malloc_size;
    return resized;
}
#endif

const JSMallocFunctions *budget_quickjs_pool_allocator(void)
{
#if defined(PSP_BROWSER_BELLARD_QUICKJS)
    static const JSMallocFunctions functions = {
        .js_malloc = bellard_pool_malloc,
        .js_free = bellard_pool_free,
        .js_realloc = bellard_pool_realloc,
        .js_malloc_usable_size = quickjs_pool_usable_size,
    };
#else
    static const JSMallocFunctions functions = {
        .js_calloc = quickjs_pool_calloc,
        .js_malloc = quickjs_pool_malloc,
        .js_free = quickjs_pool_free,
        .js_realloc = quickjs_pool_realloc,
        .js_malloc_usable_size = quickjs_pool_usable_size,
    };
#endif
    return &functions;
}

size_t budget_quickjs_pool_reserved_peak(const BudgetQuickJSPool *pool)
{
    return pool == NULL ? 0 : pool->reserved_peak;
}

size_t budget_quickjs_pool_js_malloc_peak(const BudgetQuickJSPool *pool)
{
    return pool == NULL ? 0 : pool->js_malloc_peak;
}

static void bellard_pool_note_request(void *opaque, size_t size)
{
    BudgetQuickJSPool *pool = opaque;
    if (pool == NULL) return;
    if (size > pool->largest_request) {
        pool->largest_request = size;
        static long trap = -2;
        if (trap == -2) {
            const char *value = getenv("TILEFINCH_JS_TRAP_ALLOC_SIZE");
            trap = value == NULL ? -1 : atol(value);
        }
        if (trap > 0 && size >= (size_t) trap) {
            trap = -1;
            fprintf(stderr, "js-alloc-trap size=%zu\n", size);
#if !defined(__PSP__)
            void *frames[24];
            int count = backtrace(frames, 24);
            backtrace_symbols_fd(frames, count, 2);
#endif
#if defined(PSP_BROWSER_BELLARD_QUICKJS)
            if (budget_js_stack_dump_hook != NULL) {
                budget_js_stack_dump_hook(budget_js_stack_dump_opaque);
            }
#endif
        }
    }
}

size_t budget_quickjs_pool_largest_request(const BudgetQuickJSPool *pool)
{
    return pool == NULL ? 0 : pool->largest_request;
}

size_t budget_quickjs_pool_js_malloc_current(const BudgetQuickJSPool *pool)
{
    return pool == NULL ? 0 : pool->js_malloc_current;
}

void budget_quickjs_pool_report_classes(const BudgetQuickJSPool *pool,
                                        FILE *output)
{
    if (pool == NULL || output == NULL) return;
    size_t total_requested = 0, total_capacity = 0, total_count = 0;
    for (size_t slot = 0; slot <= QUICKJS_POOL_CLASS_COUNT; slot++) {
        if (pool->live_count[slot] == 0) continue;
        size_t capacity = slot == QUICKJS_POOL_CLASS_COUNT
            ? 0 : quickjs_pool_class_sizes[slot];
        fprintf(output,
                "quickjs-pool class=%zu%s live=%zu requested=%zu "
                "capacity=%zu waste=%zu\n",
                capacity, slot == QUICKJS_POOL_CLASS_COUNT ? " (direct)" : "",
                pool->live_count[slot], pool->live_requested[slot],
                pool->live_capacity[slot],
                pool->live_capacity[slot] - pool->live_requested[slot]);
        total_requested += pool->live_requested[slot];
        total_capacity += pool->live_capacity[slot];
        total_count += pool->live_count[slot];
    }
    fprintf(output,
            "quickjs-pool total live=%zu requested=%zu capacity=%zu "
            "waste=%zu headers=%zu\n",
            total_count, total_requested, total_capacity,
            total_capacity - total_requested,
            total_count * sizeof(QuickJSPoolHeader));
}
