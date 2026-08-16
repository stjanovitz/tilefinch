#include "tilefinch/budget.h"
#include "tilefinch/budget_quickjs.h"

#include <errno.h>
#include <quickjs.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MIB (1024u * 1024u)
#define MAX_RUNS 20

#if defined(__GNUC__) || defined(__clang__)
#define BENCH_LIKELY(value) __builtin_expect(!!(value), 1)
#define BENCH_UNLIKELY(value) __builtin_expect(!!(value), 0)
#else
#define BENCH_LIKELY(value) (value)
#define BENCH_UNLIKELY(value) (value)
#endif

typedef enum {
    ALLOCATOR_DEFAULT,
    ALLOCATOR_BOUNDED,
    ALLOCATOR_LEAN,
    ALLOCATOR_POOL
} AllocatorMode;

typedef struct {
    size_t limit, current, peak;
    size_t allocations, frees, failures;
} LeanAllocator;

typedef union {
    struct { size_t size; } data;
    max_align_t alignment;
} LeanHeader;

static void *lean_malloc(void *opaque, size_t size)
{
    LeanAllocator *state = opaque;
    if (size == 0) size = 1;
    if (size > SIZE_MAX - sizeof(LeanHeader)
        || sizeof(LeanHeader) + size > state->limit - state->current) {
        state->failures++;
        return NULL;
    }
    LeanHeader *header = malloc(sizeof(*header) + size);
    if (header == NULL) {
        state->failures++;
        return NULL;
    }
    header->data.size = size;
    state->current += sizeof(*header) + size;
    if (state->current > state->peak) state->peak = state->current;
    state->allocations++;
    return header + 1;
}

static void *lean_calloc(void *opaque, size_t count, size_t size)
{
    if (count != 0 && size > SIZE_MAX / count) {
        ((LeanAllocator *) opaque)->failures++;
        return NULL;
    }
    size_t total = count * size;
    void *value = lean_malloc(opaque, total);
    if (value != NULL) memset(value, 0, total);
    return value;
}

static void lean_free(void *opaque, void *pointer)
{
    if (pointer == NULL) return;
    LeanAllocator *state = opaque;
    LeanHeader *header = (LeanHeader *) pointer - 1;
    state->current -= sizeof(*header) + header->data.size;
    state->frees++;
    free(header);
}

static void *lean_realloc(void *opaque, void *pointer, size_t size)
{
    if (pointer == NULL) return lean_malloc(opaque, size);
    if (size == 0) {
        lean_free(opaque, pointer);
        return NULL;
    }
    LeanAllocator *state = opaque;
    LeanHeader *old_header = (LeanHeader *) pointer - 1;
    size_t old_charge = sizeof(*old_header) + old_header->data.size;
    size_t base = state->current - old_charge;
    if (size > SIZE_MAX - sizeof(LeanHeader)
        || sizeof(LeanHeader) + size > state->limit - base) {
        state->failures++;
        return NULL;
    }
    LeanHeader *header = realloc(old_header, sizeof(*header) + size);
    if (header == NULL) {
        state->failures++;
        return NULL;
    }
    header->data.size = size;
    state->current = base + sizeof(*header) + size;
    if (state->current > state->peak) state->peak = state->current;
    return header + 1;
}

static size_t lean_usable_size(const void *pointer)
{
    return pointer == NULL ? 0 : ((const LeanHeader *) pointer - 1)->data.size;
}

static const JSMallocFunctions lean_functions = {
    .js_calloc = lean_calloc,
    .js_malloc = lean_malloc,
    .js_free = lean_free,
    .js_realloc = lean_realloc,
    .js_malloc_usable_size = lean_usable_size,
};

#define POOL_CLASS_COUNT 21
#define POOL_DIRECT_CLASS UINT16_MAX
#define POOL_COLLECT_DIAGNOSTICS 0

static const size_t pool_class_sizes[POOL_CLASS_COUNT] = {
    16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128,
    160, 192, 224, 256, 320, 384, 512, 640, 768, 1024
};
static uint8_t pool_class_table[1025];

static void pool_initialize_classes(void)
{
    uint16_t class_index = 0;
    for (size_t size = 0; size <= 1024; size++) {
        while (class_index + 1 < POOL_CLASS_COUNT
               && size > pool_class_sizes[class_index]) {
            class_index++;
        }
        pool_class_table[size] = (uint8_t) class_index;
    }
}

typedef union PoolHeader PoolHeader;
union PoolHeader {
    struct {
        PoolHeader *next;
        uint32_t requested;
        uint16_t class_index;
        uint16_t reserved;
    } data;
    max_align_t alignment;
};

typedef struct {
    size_t limit, reserved, reserved_peak;
    size_t allocations, frees, failures, hits, misses;
    PoolHeader *free_lists[POOL_CLASS_COUNT];
} PoolAllocator;

static uint16_t pool_class_for(size_t size)
{
    return size <= 1024 ? pool_class_table[size] : POOL_DIRECT_CLASS;
}

static void *pool_malloc(void *opaque, size_t size)
{
    PoolAllocator *state = opaque;
    if (size == 0) size = 1;
    if (BENCH_UNLIKELY(size > UINT32_MAX - sizeof(PoolHeader))) {
        state->failures++;
        return NULL;
    }
    uint16_t class_index = pool_class_for(size);
    size_t capacity = class_index == POOL_DIRECT_CLASS
        ? size : pool_class_sizes[class_index];
    size_t reserve_charge = sizeof(PoolHeader) + capacity;
    PoolHeader *header = NULL;
    if (BENCH_LIKELY(class_index != POOL_DIRECT_CLASS
                     && state->free_lists[class_index] != NULL)) {
        header = state->free_lists[class_index];
        state->free_lists[class_index] = header->data.next;
        if (POOL_COLLECT_DIAGNOSTICS) state->hits++;
    } else {
        if (BENCH_UNLIKELY(
                reserve_charge > state->limit - state->reserved)) {
            state->failures++;
            return NULL;
        }
        header = malloc(reserve_charge);
        if (BENCH_UNLIKELY(header == NULL)) {
            state->failures++;
            return NULL;
        }
        state->reserved += reserve_charge;
        if (state->reserved > state->reserved_peak) {
            state->reserved_peak = state->reserved;
        }
        if (POOL_COLLECT_DIAGNOSTICS) state->misses++;
    }
    header->data.requested = (uint32_t) size;
    header->data.class_index = class_index;
    if (POOL_COLLECT_DIAGNOSTICS) state->allocations++;
    return header + 1;
}

static void *pool_calloc(void *opaque, size_t count, size_t size)
{
    if (count != 0 && size > SIZE_MAX / count) {
        ((PoolAllocator *) opaque)->failures++;
        return NULL;
    }
    size_t total = count * size;
    void *value = pool_malloc(opaque, total);
    if (value != NULL) memset(value, 0, total);
    return value;
}

static void pool_free(void *opaque, void *pointer)
{
    if (pointer == NULL) return;
    PoolAllocator *state = opaque;
    PoolHeader *header = (PoolHeader *) pointer - 1;
    if (POOL_COLLECT_DIAGNOSTICS) state->frees++;
    if (header->data.class_index == POOL_DIRECT_CLASS) {
        state->reserved -= sizeof(*header) + header->data.requested;
        free(header);
        return;
    }
    uint16_t class_index = header->data.class_index;
    header->data.next = state->free_lists[class_index];
    state->free_lists[class_index] = header;
}

static void *pool_realloc(void *opaque, void *pointer, size_t size)
{
    if (pointer == NULL) return pool_malloc(opaque, size);
    if (size == 0) {
        pool_free(opaque, pointer);
        return NULL;
    }
    PoolAllocator *state = opaque;
    if (BENCH_UNLIKELY(size > UINT32_MAX - sizeof(PoolHeader))) {
        state->failures++;
        return NULL;
    }
    PoolHeader *header = (PoolHeader *) pointer - 1;
    size_t old_size = header->data.requested;
    uint16_t new_class = pool_class_for(size);
    size_t old_capacity = header->data.class_index == POOL_DIRECT_CLASS
        ? old_size : pool_class_sizes[header->data.class_index];
    if (header->data.class_index != POOL_DIRECT_CLASS
        && size <= old_capacity) {
        header->data.requested = (uint32_t) size;
        return pointer;
    }
    if (header->data.class_index == POOL_DIRECT_CLASS
        && new_class == POOL_DIRECT_CLASS) {
        size_t reserved_base = state->reserved - sizeof(*header) - old_size;
        if (sizeof(*header) + size > state->limit - reserved_base) {
            state->failures++;
            return NULL;
        }
        PoolHeader *replacement = realloc(header, sizeof(*header) + size);
        if (replacement == NULL) {
            state->failures++;
            return NULL;
        }
        replacement->data.requested = (uint32_t) size;
        state->reserved = reserved_base + sizeof(*header) + size;
        if (state->reserved > state->reserved_peak) {
            state->reserved_peak = state->reserved;
        }
        return replacement + 1;
    }
    void *replacement = pool_malloc(opaque, size);
    if (replacement == NULL) return NULL;
    memcpy(replacement, pointer, old_size < size ? old_size : size);
    pool_free(opaque, pointer);
    return replacement;
}

static size_t pool_usable_size(const void *pointer)
{
    return pointer == NULL ? 0 : ((const PoolHeader *) pointer - 1)->data.requested;
}

static void pool_destroy(PoolAllocator *state)
{
    for (size_t index = 0; index < POOL_CLASS_COUNT; index++) {
        PoolHeader *header = state->free_lists[index];
        while (header != NULL) {
            PoolHeader *next = header->data.next;
            state->reserved -= sizeof(*header) + pool_class_sizes[index];
            free(header);
            header = next;
        }
        state->free_lists[index] = NULL;
    }
}

static const JSMallocFunctions pool_functions = {
    .js_calloc = pool_calloc,
    .js_malloc = pool_malloc,
    .js_free = pool_free,
    .js_realloc = pool_realloc,
    .js_malloc_usable_size = pool_usable_size,
};

static uint64_t monotonic_ms(void)
{
    struct timespec value;
    (void) timespec_get(&value, TIME_UTC);
    return (uint64_t) value.tv_sec * 1000u
           + (uint64_t) value.tv_nsec / 1000000u;
}

static void usage(const char *program)
{
    fprintf(stderr,
            "usage: %s --script FILE --allocator default|bounded|lean|pool "
            "[--runs N]\n", program);
}

static char *read_file(const char *path, size_t *length)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL || fseek(file, 0, SEEK_END) != 0) {
        if (file != NULL) fclose(file);
        return NULL;
    }
    long measured = ftell(file);
    if (measured < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    char *source = malloc((size_t) measured + 1);
    if (source == NULL) {
        fclose(file);
        return NULL;
    }
    size_t received = fread(source, 1, (size_t) measured, file);
    fclose(file);
    if (received != (size_t) measured) {
        free(source);
        return NULL;
    }
    source[received] = '\0';
    *length = received;
    return source;
}

static int compare_u64(const void *left, const void *right)
{
    uint64_t first = *(const uint64_t *) left;
    uint64_t second = *(const uint64_t *) right;
    return first < second ? -1 : first > second ? 1 : 0;
}

static bool run_once(AllocatorMode mode, const char *source,
                     size_t source_length, uint64_t *elapsed_ms,
                     Budget *measured_budget, PoolAllocator *measured_pool,
                     char *summary,
                     size_t summary_capacity)
{
    Budget budget;
    bool pool_balanced = true;
    budget_init(&budget, 8u * MIB);
    LeanAllocator lean = {.limit = 8u * MIB};
    PoolAllocator pool = {.limit = 8u * MIB};
    JSRuntime *runtime = mode == ALLOCATOR_BOUNDED
        ? JS_NewRuntime2(budget_quickjs_allocator(), &budget)
        : mode == ALLOCATOR_LEAN
          ? JS_NewRuntime2(&lean_functions, &lean)
          : mode == ALLOCATOR_POOL
            ? JS_NewRuntime2(&pool_functions, &pool) : JS_NewRuntime();
    if (runtime == NULL) return false;
    JS_SetMemoryLimit(runtime, 4u * MIB);
    JS_SetMaxStackSize(runtime, 256u * 1024u);
    JSContext *context = JS_NewContext(runtime);
    if (context == NULL) {
        JS_FreeRuntime(runtime);
        return false;
    }
    uint64_t started = monotonic_ms();
    JSValue evaluated = JS_Eval(context, source, source_length,
                                "<allocator-benchmark>",
                                JS_EVAL_TYPE_GLOBAL);
    bool ok = !JS_IsException(evaluated);
    JS_FreeValue(context, evaluated);
    while (ok && JS_IsJobPending(runtime)) {
        JSContext *job_context = NULL;
        ok = JS_ExecutePendingJob(runtime, &job_context) >= 0;
    }
    *elapsed_ms = monotonic_ms() - started;
    if (!ok) {
        JSValue exception = JS_GetException(context);
        const char *message = JS_ToCString(context, exception);
        fprintf(stderr, "allocator-bench exception=\"%s\"\n",
                message == NULL ? "unknown" : message);
        if (message != NULL) JS_FreeCString(context, message);
        JS_FreeValue(context, exception);
    } else {
        JSValue global = JS_GetGlobalObject(context);
        JSValue value = JS_GetPropertyStr(context, global, "pocSummary");
        const char *text = JS_ToCString(context, value);
        if (text != NULL) {
            snprintf(summary, summary_capacity, "%s", text);
            JS_FreeCString(context, text);
        }
        JS_FreeValue(context, value);
        JS_FreeValue(context, global);
    }
    JS_FreeContext(context);
    JS_FreeRuntime(runtime);
    if (mode == ALLOCATOR_LEAN) {
        budget.limit = lean.limit;
        budget.current = lean.current;
        budget.peak = lean.peak;
        budget.allocation_count = lean.allocations;
        budget.free_count = lean.frees;
        budget.failure_count = lean.failures;
    } else if (mode == ALLOCATOR_POOL) {
        budget.limit = pool.limit;
        budget.current = 0;
        budget.peak = pool.reserved_peak;
        budget.allocation_count = pool.allocations;
        budget.free_count = pool.frees;
        budget.failure_count = pool.failures;
        pool_destroy(&pool);
        pool_balanced = pool.reserved == 0;
        *measured_pool = pool;
    }
    *measured_budget = budget;
    return ok && (mode == ALLOCATOR_DEFAULT ||
                  (mode == ALLOCATOR_POOL ? pool_balanced
                                          : budget.current == 0));
}

int main(int argc, char **argv)
{
    pool_initialize_classes();
    const char *script_path = NULL;
    AllocatorMode mode = ALLOCATOR_DEFAULT;
    bool mode_set = false;
    unsigned long runs = 5;
    for (int i = 1; i < argc; i++) {
        if (i + 1 >= argc) {
            usage(argv[0]);
            return 2;
        }
        if (strcmp(argv[i], "--script") == 0) {
            script_path = argv[++i];
        } else if (strcmp(argv[i], "--allocator") == 0) {
            const char *selected = argv[++i];
            if (strcmp(selected, "default") == 0) {
                mode = ALLOCATOR_DEFAULT;
            } else if (strcmp(selected, "bounded") == 0) {
                mode = ALLOCATOR_BOUNDED;
            } else if (strcmp(selected, "lean") == 0) {
                mode = ALLOCATOR_LEAN;
            } else if (strcmp(selected, "pool") == 0) {
                mode = ALLOCATOR_POOL;
            } else {
                usage(argv[0]);
                return 2;
            }
            mode_set = true;
        } else if (strcmp(argv[i], "--runs") == 0) {
            char *end = NULL;
            errno = 0;
            runs = strtoul(argv[++i], &end, 10);
            if (errno != 0 || end == NULL || *end != '\0'
                || runs < 1 || runs > MAX_RUNS) {
                usage(argv[0]);
                return 2;
            }
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (script_path == NULL || !mode_set) {
        usage(argv[0]);
        return 2;
    }
    size_t source_length = 0;
    char *source = read_file(script_path, &source_length);
    if (source == NULL) {
        fprintf(stderr, "allocator-bench could not read \"%s\"\n",
                script_path);
        return 1;
    }
    uint64_t times[MAX_RUNS] = {0};
    size_t total_allocations = 0, total_frees = 0, total_failures = 0;
    size_t maximum_peak = 0;
    size_t total_pool_hits = 0, total_pool_misses = 0;
    size_t maximum_reserved_peak = 0;
    char expected_summary[128] = {0};
    bool ok = true;
    for (unsigned long run = 0; run < runs; run++) {
        Budget budget = {0};
        PoolAllocator pool = {0};
        char summary[128] = {0};
        if (!run_once(mode, source, source_length, &times[run], &budget,
                      &pool, summary, sizeof(summary))) {
            ok = false;
            break;
        }
        if (run == 0) snprintf(expected_summary, sizeof(expected_summary),
                               "%s", summary);
        else if (strcmp(expected_summary, summary) != 0) ok = false;
        total_allocations += budget.allocation_count;
        total_frees += budget.free_count;
        total_failures += budget.failure_count;
        if (budget.peak > maximum_peak) maximum_peak = budget.peak;
        total_pool_hits += pool.hits;
        total_pool_misses += pool.misses;
        if (pool.reserved_peak > maximum_reserved_peak) {
            maximum_reserved_peak = pool.reserved_peak;
        }
    }
    free(source);
    if (!ok) return 1;
    qsort(times, runs, sizeof(times[0]), compare_u64);
    printf("allocator-bench allocator=%s runs=%lu median-ms=%llu min-ms=%llu "
           "max-ms=%llu allocations=%zu frees=%zu failures=%zu "
           "peak=%zu pool-hits=%zu pool-misses=%zu reserved-peak=%zu "
           "summary=\"%s\"\n",
           mode == ALLOCATOR_BOUNDED ? "bounded"
           : mode == ALLOCATOR_LEAN ? "lean"
           : mode == ALLOCATOR_POOL ? "pool" : "default", runs,
           (unsigned long long) times[runs / 2],
           (unsigned long long) times[0],
           (unsigned long long) times[runs - 1],
           total_allocations, total_frees, total_failures, maximum_peak,
           total_pool_hits, total_pool_misses, maximum_reserved_peak,
           expected_summary);
    return 0;
}
