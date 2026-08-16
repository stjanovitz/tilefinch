#include "tilefinch/budget.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define KIB (1024u)
#define MIB (1024u * 1024u)
#define THREAD_COUNT 8
#define SLOT_COUNT 16
#define ITERATION_COUNT 4000

#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "CONCURRENT BUDGET CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition);                             \
        return 1;                                                            \
    }                                                                        \
} while (0)

typedef struct {
    unsigned char *pointer;
    size_t length;
    unsigned char marker;
} Slot;

typedef struct {
    BudgetConcurrentPool *pool;
    uint32_t random;
    bool ok;
} Worker;

static uint32_t next_random(Worker *worker)
{
    uint32_t value = worker->random;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    worker->random = value;
    return value;
}

static bool slot_valid(const Slot *slot)
{
    return slot->pointer == NULL
        || (slot->length != 0 && slot->pointer[0] == slot->marker
            && slot->pointer[slot->length / 2] == slot->marker
            && slot->pointer[slot->length - 1] == slot->marker);
}

static void *run_worker(void *opaque)
{
    Worker *worker = opaque;
    Slot slots[SLOT_COUNT] = {0};
    worker->ok = true;
    for (size_t iteration = 0;
         iteration < ITERATION_COUNT && worker->ok; iteration++) {
        size_t index = next_random(worker) % SLOT_COUNT;
        Slot *slot = &slots[index];
        if (!slot_valid(slot)) {
            worker->ok = false;
            break;
        }
        budget_concurrent_pool_free(worker->pool, slot->pointer);
        memset(slot, 0, sizeof(*slot));

        size_t length = 1 + next_random(worker) % 1024;
        unsigned char marker = (unsigned char) (1 + next_random(worker) % 254);
        unsigned char *pointer = budget_concurrent_pool_malloc(
            worker->pool, length);
        if (pointer == NULL) {
            worker->ok = false;
            break;
        }
        memset(pointer, marker, length);
        if ((iteration & 3u) == 0) {
            size_t resized_length = length + next_random(worker) % 512;
            unsigned char *resized = budget_concurrent_pool_realloc(
                worker->pool, pointer, resized_length);
            if (resized == NULL) {
                worker->ok = false;
                budget_concurrent_pool_free(worker->pool, pointer);
                break;
            }
            if (resized[0] != marker
                || resized[length / 2] != marker
                || resized[length - 1] != marker) {
                worker->ok = false;
                budget_concurrent_pool_free(worker->pool, resized);
                break;
            }
            memset(resized + length, marker, resized_length - length);
            pointer = resized;
            length = resized_length;
        }
        slot->pointer = pointer;
        slot->length = length;
        slot->marker = marker;
    }
    for (size_t i = 0; i < SLOT_COUNT; i++) {
        if (!slot_valid(&slots[i])) worker->ok = false;
        budget_concurrent_pool_free(worker->pool, slots[i].pointer);
    }
    return NULL;
}

int main(void)
{
    Budget budget;
    budget_init(&budget, 2u * MIB);
    uint64_t checkpoint = budget_checkpoint(&budget);
    BudgetConcurrentPool pool = {0};
    CHECK(budget_concurrent_pool_init(
              &pool, &budget, BUDGET_CATEGORY_RESOURCE, 512u * KIB)
          && budget.current >= 512u * KIB
          && budget.external_reserved == 512u * KIB
          && budget.external_reserved_peak == 512u * KIB
          && budget.categories[BUDGET_CATEGORY_RESOURCE].active_allocations
                 == 1
          && budget_categories_reconcile(&budget));

    /* A generic transaction rollback must not retire a reservation while
       foreign workers can still own allocations from it. */
    size_t reserved_current = budget.current;
    budget_rollback(&budget, checkpoint);
    CHECK(budget.current == reserved_current
          && budget_categories_reconcile(&budget));

    CHECK(budget_concurrent_pool_malloc(&pool, 512u * KIB) == NULL);
    Worker workers[THREAD_COUNT];
    pthread_t threads[THREAD_COUNT];
    size_t started = 0;
    for (; started < THREAD_COUNT; started++) {
        workers[started] = (Worker) {
            .pool = &pool,
            .random = UINT32_C(0x9e3779b9)
                      ^ (uint32_t) (started * UINT32_C(0x85ebca6b))
        };
        if (pthread_create(&threads[started], NULL, run_worker,
                           &workers[started]) != 0) break;
    }
    CHECK(started == THREAD_COUNT);
    for (size_t i = 0; i < started; i++)
        CHECK(pthread_join(threads[i], NULL) == 0 && workers[i].ok);

    BudgetConcurrentPoolMetrics metrics;
    CHECK(budget_concurrent_pool_metrics(&pool, &metrics)
          && metrics.limit == 512u * KIB
          && metrics.current == 0
          && metrics.active_allocations == 0
          && metrics.peak <= metrics.limit
          && metrics.allocation_count == metrics.free_count
          && metrics.allocation_count >= THREAD_COUNT * ITERATION_COUNT
          && metrics.failure_count == 1);

    unsigned char *retained = budget_concurrent_pool_calloc(&pool, 8, 16);
    unsigned char *zero = budget_concurrent_pool_calloc(&pool, 0, 16);
    CHECK(retained != NULL
          && budget_concurrent_pool_usable_size(&pool, retained) == 128
          && retained[0] == 0 && retained[127] == 0
          && zero != NULL
          && budget_concurrent_pool_usable_size(&pool, zero) == 1
          && zero[0] == 0
          && !budget_concurrent_pool_destroy(&pool)
          && budget.current == reserved_current);
    budget_concurrent_pool_free(&pool, zero);
    budget_concurrent_pool_free(&pool, retained);
    CHECK(budget_concurrent_pool_destroy(&pool)
          && !budget_concurrent_pool_metrics(&pool, &metrics)
          && budget.current == 0
          && budget.external_reserved == 0
          && budget.allocation_count == budget.free_count
          && budget.categories[BUDGET_CATEGORY_RESOURCE].allocation_count
                 == budget.categories[BUDGET_CATEGORY_RESOURCE].free_count
          && budget_categories_reconcile(&budget));

    puts("tilefinch-budget-concurrent-tests: all checks passed");
    return 0;
}
