/* Host ownership/publication probe, not a browser execution tier. */
#include "../third_party/quickjs/quickjs.c"
#include "tilefinch/budget_quickjs.h"
#include "native-tier-code-pool.h"

/* The fake OS backend owns external backing already covered by the pool's
   BudgetReservation. Only that backend uses calloc/free, not engine storage. */
#undef free

#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define CHECK(value) do { if (!(value)) { \
    fprintf(stderr, "pool check failed at %d: %s\n", __LINE__, #value); exit(1); \
} } while (0)

typedef struct { size_t used, limit; bool refuse; } HeapLedger;
static bool heap_reserve(void *opaque, size_t bytes)
{
    HeapLedger *heap = opaque;
    if (heap->refuse || heap->used > heap->limit || bytes > heap->limit - heap->used) return false;
    heap->used += bytes;
    return true;
}
static void heap_release(void *opaque, size_t bytes)
{
    HeapLedger *heap = opaque;
    CHECK(bytes <= heap->used);
    heap->used -= bytes;
}

typedef struct {
    void *memory[16];
    size_t size[16];
    bool published[16];
    bool refuse_map, refuse_publish, refuse_unmap, real;
    unsigned map_attempts, maps, publications, unmaps;
} Backend;

static unsigned backend_slot(Backend *backend, void *memory)
{
    for (unsigned i = 0; i < 16; i++) if (backend->memory[i] == memory) return i;
    CHECK(false);
    return 0;
}
static void *map_writable(void *opaque, size_t bytes)
{
    Backend *backend = opaque;
    backend->map_attempts++;
    if (backend->refuse_map) return NULL;
    unsigned slot = backend_slot(backend, NULL);
    void *memory = backend->real ? mmap(NULL, bytes, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) : calloc(1, bytes);
    if (memory == MAP_FAILED || !memory) return NULL;
    backend->memory[slot] = memory;
    backend->size[slot] = bytes;
    backend->maps++;
    return memory;
}
static bool publish(void *opaque, void *memory, size_t bytes)
{
    Backend *backend = opaque;
    unsigned slot = backend_slot(backend, memory);
    CHECK(bytes == backend->size[slot]);
    CHECK(!backend->published[slot]);
    /* Simulate a backend that changed permissions before reporting failure. */
    if (backend->real && mprotect(memory, bytes, PROT_READ | PROT_EXEC) != 0) return false;
    __builtin___clear_cache(memory, (char *)memory + bytes);
    backend->published[slot] = true;
    backend->publications++;
    return !backend->refuse_publish;
}
static bool unmap(void *opaque, void *memory, size_t bytes)
{
    Backend *backend = opaque;
    unsigned slot = backend_slot(backend, memory);
    CHECK(bytes == backend->size[slot]);
    if (backend->refuse_unmap) return false;
    if (backend->real) {
        if (munmap(memory, bytes) != 0) return false;
    } else free(memory);
    backend->memory[slot] = NULL;
    backend->size[slot] = 0;
    backend->published[slot] = false;
    backend->unmaps++;
    return true;
}
static NativeCodeBackend backend_api(Backend *backend, size_t page_size)
{
    return (NativeCodeBackend){backend, page_size, map_writable, publish, unmap};
}
static NativeCodeHeap heap_api(HeapLedger *heap)
{
    return (NativeCodeHeap){heap, heap_reserve, heap_release};
}

static uint64_t now_ns(void)
{
    struct timespec time;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &time) == 0);
    return (uint64_t)time.tv_sec * 1000000000u + (uint64_t)time.tv_nsec;
}

static void refusal_tests(void)
{
    for (unsigned failure = 0; failure < 3; failure++) {
        Budget budget;
        budget_init(&budget, 1024u * 1024u);
        HeapLedger heap = {.limit = NATIVE_POOL_BYTE_LIMIT};
        Backend backend = {0};
        budget_inject_failure_after(&budget, failure);
        NativeCodePool *pool = native_code_pool_create(&budget, heap_api(&heap), backend_api(&backend, 256), NATIVE_POOL_BYTE_LIMIT);
        if (failure == 0) CHECK(pool == NULL);
        else {
            CHECK(pool != NULL);
            NativeCodeHandle image = {9, 8, 7};
            bool admitted = native_code_pool_allocate(pool, 257, &image);
            CHECK(admitted == (failure == 2));
            if (!admitted) CHECK(image.owner == 9 && image.generation == 8 && image.slot == 7);
        }
        budget_clear_failure_injection(&budget);
        CHECK(native_code_pool_destroy(&pool));
        CHECK(budget.current == 0 && heap.used == 0 && backend.maps == backend.unmaps);
    }
    for (unsigned failure = 0; failure < 4; failure++) {
        Budget budget;
        budget_init(&budget, 1024u * 1024u);
        HeapLedger heap = {.limit = NATIVE_POOL_BYTE_LIMIT};
        Backend backend = {0};
        NativeCodePool *pool = native_code_pool_create(&budget, heap_api(&heap), backend_api(&backend, 256), NATIVE_POOL_BYTE_LIMIT);
        CHECK(pool != NULL);
        size_t baseline = budget.current;
        heap.refuse = failure == 0;
        backend.refuse_map = failure == 1;
        backend.refuse_publish = failure >= 2;
        backend.refuse_unmap = failure == 3;
        NativeCodeHandle image;
        bool admitted = native_code_pool_allocate(pool, 257, &image);
        CHECK(admitted == (failure >= 2));
        if (admitted) {
            const uint8_t bytes[] = {1, 2, 3, 4};
            CHECK(native_code_pool_write(pool, image, 0, bytes, sizeof(bytes)));
            CHECK(!native_code_pool_publish(pool, image));
            CHECK(!native_code_pool_write(pool, image, 0, bytes, sizeof(bytes)));
            NativeCodeLease lease;
            CHECK(!native_code_pool_enter(pool, image, 0, &lease));
        }
        if (failure == 3) {
            CHECK(budget.current > baseline);
            CHECK(heap.used == native_code_pool_metadata_bytes() + 512);
            CHECK(!native_code_pool_destroy(&pool) && pool != NULL);
        } else CHECK(budget.current == baseline);
        backend.refuse_unmap = false;
        CHECK(native_code_pool_destroy(&pool));
        CHECK(budget.current == 0 && heap.used == 0 && backend.maps == backend.unmaps);
    }
    puts("pool refusal rollback=pass");
}

static void byte_limit_tests(void)
{
    Budget budget;
    budget_init(&budget, 1024u * 1024u);
    HeapLedger heap = {.limit = NATIVE_POOL_BYTE_LIMIT};
    Backend backend = {0};
    CHECK(native_code_pool_create(&budget, heap_api(&heap), backend_api(&backend, 0), NATIVE_POOL_BYTE_LIMIT) == NULL);
    heap.refuse = true;
    CHECK(native_code_pool_create(&budget, heap_api(&heap), backend_api(&backend, 256), NATIVE_POOL_BYTE_LIMIT) == NULL);
    CHECK(budget.current == 0 && heap.used == 0);
    heap.refuse = false;
    size_t limit = native_code_pool_metadata_bytes() + 256;
    NativeCodePool *pool = native_code_pool_create(&budget, heap_api(&heap), backend_api(&backend, 256), limit);
    CHECK(pool != NULL);
    NativeCodeHandle image;
    CHECK(!native_code_pool_allocate(pool, 0, &image));
    CHECK(!native_code_pool_allocate(pool, 257, &image));
    CHECK(backend.map_attempts == 0);
    CHECK(native_code_pool_allocate(pool, 256, &image));
    CHECK(native_code_pool_charged_bytes(pool) == limit);
    CHECK(!native_code_pool_allocate(pool, 1, &image));
    CHECK(backend.map_attempts == 1);
    CHECK(native_code_pool_destroy(&pool));
    CHECK(budget.current == 0 && heap.used == 0);
    puts("pool rounded-byte ceiling=pass");
}

static bool enter_control(NativeCodePool *pool, NativeCodeHandle handle,
                           size_t offset, NativeCodeLease *lease, bool by_ref)
{
    return by_ref ? native_code_pool_enter_ref(pool, &handle, offset, lease) :
        native_code_pool_enter(pool, handle, offset, lease);
}
static bool leave_control(NativeCodePool *pool, NativeCodeLease lease, bool by_ref)
{
    return by_ref ? native_code_pool_leave_ref(pool, &lease) : native_code_pool_leave(pool, lease);
}
static void lifecycle_tests(bool by_ref)
{
    Budget budget;
    budget_init(&budget, 1024u * 1024u);
    HeapLedger heap = {.limit = NATIVE_POOL_BYTE_LIMIT};
    Backend backend = {0};
    NativeCodePool *pool = native_code_pool_create(&budget, heap_api(&heap), backend_api(&backend, 256), NATIVE_POOL_BYTE_LIMIT);
    CHECK(pool != NULL);
    NativeCodeHandle handles[NATIVE_POOL_IMAGE_LIMIT];
    const uint8_t code[] = {1, 2, 3, 4};
    for (unsigned i = 0; i < NATIVE_POOL_IMAGE_LIMIT; i++) {
        CHECK(native_code_pool_allocate(pool, 4, &handles[i]));
        CHECK(!native_code_pool_publish(pool, handles[i]));
        CHECK(!native_code_pool_write(pool, handles[i], SIZE_MAX, code, 4));
        CHECK(!native_code_pool_write(pool, handles[i], 253, code, 4));
        CHECK(native_code_pool_write(pool, handles[i], 0, code, 4));
        CHECK(native_code_pool_publish(pool, handles[i]));
        CHECK(!native_code_pool_publish(pool, handles[i]));
        CHECK(!native_code_pool_write(pool, handles[i], 0, code, 4));
    }
    NativeCodeHandle spare;
    CHECK(!native_code_pool_allocate(pool, 4, &spare));
    CHECK(!native_code_pool_allocate(pool, SIZE_MAX, &spare));
    NativeCodeLease leases[NATIVE_POOL_LEASE_LIMIT], spare_lease;
    CHECK(!native_code_pool_enter_ref(pool, NULL, 0, &spare_lease));
    CHECK(!native_code_pool_enter_ref(NULL, &handles[0], 0, &spare_lease));
    CHECK(!native_code_pool_enter_ref(pool, &handles[0], 0, NULL));
    CHECK(!native_code_pool_leave_ref(pool, NULL));
    CHECK(!enter_control(pool, handles[0], 4, &spare_lease, by_ref));
    for (unsigned i = 0; i < NATIVE_POOL_LEASE_LIMIT; i++)
        CHECK(enter_control(pool, handles[0], 0, &leases[i], by_ref));
    NativeCodeLease invalid = leases[0];
    invalid.id = UINT64_MAX;
    CHECK(!leave_control(pool, invalid, by_ref));
    invalid = leases[0];
    invalid.image.generation++;
    CHECK(!leave_control(pool, invalid, by_ref));
    CHECK(!enter_control(pool, handles[0], 0, &spare_lease, by_ref));
    CHECK(native_code_pool_retire(pool, handles[0]));
    CHECK(backend.unmaps == 0);
    CHECK(!enter_control(pool, handles[0], 0, &spare_lease, by_ref));
    for (unsigned i = 0; i < NATIVE_POOL_LEASE_LIMIT; i++) {
        CHECK(leave_control(pool, leases[i], by_ref));
        CHECK(!leave_control(pool, leases[i], by_ref));
    }
    CHECK(backend.unmaps == 1);
    CHECK(native_code_pool_allocate(pool, 4, &spare));
    CHECK(spare.slot == handles[0].slot && spare.generation != handles[0].generation);
    CHECK(!native_code_pool_write(pool, handles[0], 0, code, 4));
    CHECK(!native_code_pool_retire(pool, handles[0]));
    CHECK(!native_code_pool_enter_ref(pool, &handles[0], 0, &spare_lease));
    CHECK(!native_code_pool_leave_ref(pool, &leases[0]));
    CHECK(enter_control(pool, handles[1], 0, &spare_lease, by_ref));
    CHECK(!native_code_pool_destroy(&pool));
    CHECK(!enter_control(pool, handles[1], 0, &leases[0], by_ref));
    CHECK(!native_code_pool_allocate(pool, 4, &spare));
    CHECK(leave_control(pool, spare_lease, by_ref));
    CHECK(native_code_pool_destroy(&pool));
    CHECK(budget.current == 0 && heap.used == 0 && backend.maps == backend.unmaps);
    pool = native_code_pool_create(&budget, heap_api(&heap), backend_api(&backend, 256), NATIVE_POOL_BYTE_LIMIT);
    CHECK(pool != NULL);
    CHECK(native_code_pool_allocate(pool, 4, &spare));
    CHECK(spare.owner != handles[0].owner);
    CHECK(!native_code_pool_retire(pool, handles[0]));
    CHECK(!native_code_pool_enter_ref(pool, &handles[0], 0, &spare_lease));
    CHECK(!native_code_pool_leave_ref(pool, &leases[0]));
    CHECK(native_code_pool_destroy(&pool));
    CHECK(budget.current == 0 && heap.used == 0);
    printf("pool leases, bounds, stale handles, retirement=pass pointer-abi=%u\n", by_ref);
}

/* This controlled experiment owns the runtime's memory-limit setter. The
   browser's dynamic limit-growth policy is NOT integrated by this adapter. */
typedef struct { JSRuntime *rt; size_t logical_limit, reserved; } RuntimeHeap;
static bool runtime_reserve(void *opaque, size_t bytes)
{
    RuntimeHeap *heap = opaque;
    JSMallocState *state = &heap->rt->malloc_ctx.malloc_state;
    if (heap->reserved > heap->logical_limit) return false;
    size_t limit = heap->logical_limit - heap->reserved;
    if (state->malloc_limit != limit || state->malloc_size > limit || bytes > limit - state->malloc_size)
        return false;
    heap->reserved += bytes;
    JS_SetMemoryLimit(heap->rt, heap->logical_limit - heap->reserved);
    return true;
}
static void runtime_release(void *opaque, size_t bytes)
{
    RuntimeHeap *heap = opaque;
    CHECK(bytes <= heap->reserved);
    CHECK(heap->rt->malloc_ctx.malloc_state.malloc_limit == heap->logical_limit - heap->reserved);
    heap->reserved -= bytes;
    JS_SetMemoryLimit(heap->rt, heap->logical_limit - heap->reserved);
}

static void runtime_and_executable_tests(void)
{
    Budget budget;
    budget_init(&budget, 16u * 1024u * 1024u);
    BudgetQuickJSPool *allocator = budget_quickjs_pool_create(&budget);
    CHECK(allocator != NULL);
    JSRuntime *rt = JS_NewRuntime2(budget_quickjs_pool_allocator(), allocator);
    CHECK(rt != NULL);
    JSContext *ctx = JS_NewContext(rt);
    CHECK(ctx != NULL);
    JS_RunGC(rt);
    size_t live = rt->malloc_ctx.malloc_state.malloc_size;
    RuntimeHeap heap = {rt, live + native_code_pool_metadata_bytes() + 24576, 0};
    JS_SetMemoryLimit(rt, heap.logical_limit);
    Backend backend = {0};
    NativeCodeHeap heap_callbacks = {&heap, runtime_reserve, runtime_release};
    NativeCodePool *pool = native_code_pool_create(&budget, heap_callbacks, backend_api(&backend, 4096), NATIVE_POOL_BYTE_LIMIT);
    CHECK(pool != NULL);
    NativeCodeHandle image;
    CHECK(native_code_pool_allocate(pool, 16384, &image));
    void *allocation = js_malloc_rt(rt, 12288);
    CHECK(allocation == NULL);
    CHECK(native_code_pool_retire(pool, image));
    allocation = js_malloc_rt(rt, 12288);
    CHECK(allocation != NULL);
    js_free_rt(rt, allocation);
    JSValue exception = JS_NewObject(ctx);
    CHECK(!JS_IsException(exception));
    JS_Throw(ctx, JS_DupValue(ctx, exception));
    CHECK(!native_code_pool_allocate(pool, SIZE_MAX, &image));
    size_t before = budget.current;
    unsigned attempts = backend.map_attempts;
    backend.refuse_map = true;
    CHECK(!native_code_pool_allocate(pool, 4096, &image));
    CHECK(backend.map_attempts == attempts + 1);
    CHECK(budget.current == before && heap.reserved == native_code_pool_metadata_bytes());
    backend.refuse_map = false;
    JSValue retained = JS_GetException(ctx);
    CHECK(JS_VALUE_GET_PTR(retained) == JS_VALUE_GET_PTR(exception));
    JS_FreeValue(ctx, retained);
    JS_FreeValue(ctx, exception);
    CHECK(native_code_pool_destroy(&pool));
    CHECK(heap.reserved == 0 && rt->malloc_ctx.malloc_state.malloc_limit == heap.logical_limit);

    JS_SetMemoryLimit(rt, 8u * 1024u * 1024u);
    heap.logical_limit = 8u * 1024u * 1024u;
    long page_size = sysconf(_SC_PAGESIZE);
    CHECK(page_size > 0);
    backend.real = true;
    pool = native_code_pool_create(&budget, heap_callbacks, backend_api(&backend, (size_t)page_size), NATIVE_POOL_BYTE_LIMIT);
    CHECK(pool != NULL);
    CHECK(native_code_pool_allocate(pool, 64, &image));
#if defined(__aarch64__)
    const uint32_t program[] = {0xd503245fu, 0x52800540u, 0xd65f03c0u}; /* bti c; mov w0,#42; ret */
#elif defined(__x86_64__)
    const uint8_t program[] = {0xf3, 0x0f, 0x1e, 0xfa, 0xb8, 42, 0, 0, 0, 0xc3}; /* endbr64; mov eax,42; ret */
#else
#error This executable publication probe currently supports ARM64 and x86-64 hosts only
#endif
    /* Leave a readable zero prefix for UBSan's indirect-call metadata probe. */
    CHECK(native_code_pool_write(pool, image, 16, program, sizeof(program)));
    CHECK(native_code_pool_publish(pool, image));
    NativeCodeLease lease;
    int (*entry)(void);
    _Static_assert(sizeof(entry) == sizeof(lease.entry), "host function-pointer ABI");
    uint64_t samples[9];
    for (unsigned repeat = 0; repeat < 9; repeat++) {
        uint64_t begin = now_ns();
        unsigned result = 0;
        for (unsigned iteration = 0; iteration < 1000000; iteration++) {
            CHECK(native_code_pool_enter(pool, image, 16, &lease));
            memcpy(&entry, &lease.entry, sizeof(entry));
            result += (unsigned)entry();
            CHECK(native_code_pool_leave(pool, lease));
        }
        CHECK(result == 42000000u);
        samples[repeat] = now_ns() - begin;
        printf("pool-execution repeat=%u million-calls-ns=%llu\n", repeat, (unsigned long long)samples[repeat]);
    }
    for (unsigned i = 1; i < 9; i++) {
        uint64_t sample = samples[i];
        unsigned j = i;
        while (j && sample < samples[j - 1]) { samples[j] = samples[j - 1]; j--; }
        samples[j] = sample;
    }
    printf("pool-execution median-million-calls-ns=%llu\n", (unsigned long long)samples[4]);
    CHECK(native_code_pool_enter(pool, image, 16, &lease));
    memcpy(&entry, &lease.entry, sizeof(entry));
    CHECK(entry() == 42);
    CHECK(native_code_pool_retire(pool, image));
    CHECK(entry() == 42); /* a held lease survives retirement */
    CHECK(native_code_pool_leave(pool, lease));
    CHECK(native_code_pool_destroy(&pool));
    CHECK(heap.reserved == 0);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    CHECK(budget_quickjs_pool_destroy(allocator));
    CHECK(budget.current == 0 && backend.maps == backend.unmaps);
    puts("pool JS ceiling, exception preservation, RX execution, Budget baseline=pass");
}

int main(void)
{
    refusal_tests();
    byte_limit_tests();
    lifecycle_tests(false);
    lifecycle_tests(true);
    runtime_and_executable_tests();
    return 0;
}
