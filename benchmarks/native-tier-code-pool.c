#include "native-tier-code-pool.h"

#include <stdatomic.h>
#include <string.h>

typedef enum { IMAGE_FREE, IMAGE_WRITABLE, IMAGE_PUBLISHED, IMAGE_RETIRED } ImageState;
typedef struct {
    ImageState state;
    uint64_t generation;
    uint64_t leases[NATIVE_POOL_LEASE_LIMIT];
    void *memory;
    size_t capacity, used;
    BudgetReservation reservation;
} NativeCodeImage;

struct NativeCodePool {
    Budget *budget;
    NativeCodeHeap heap;
    NativeCodeBackend backend;
    uint64_t owner, next_lease;
    size_t limit, charged;
    bool retiring;
    NativeCodeImage images[NATIVE_POOL_IMAGE_LIMIT];
};

/* Identity only, never a global code cache. Bounded CAS admission fails soft
   on contention or exhaustion instead of wrapping and accepting stale handles. */
static _Atomic uint64_t owner_sequence;

static uint64_t new_owner(void)
{
    uint64_t old = atomic_load_explicit(&owner_sequence, memory_order_relaxed);
    for (unsigned attempt = 0; attempt < 32; attempt++) {
        if (old == UINT64_MAX) return 0;
        if (atomic_compare_exchange_weak_explicit(&owner_sequence, &old, old + 1,
                memory_order_relaxed, memory_order_relaxed)) return old + 1;
    }
    return 0;
}

size_t native_code_pool_metadata_bytes(void) { return sizeof(NativeCodePool); }
size_t native_code_pool_charged_bytes(const NativeCodePool *pool)
{
    return pool ? pool->charged : 0;
}

NativeCodePool *native_code_pool_create(Budget *budget, NativeCodeHeap heap,
                                       NativeCodeBackend backend, size_t limit)
{
    if (!budget || !heap.reserve || !heap.release || !backend.map_writable ||
        !backend.publish || !backend.unmap || !backend.page_size ||
        backend.page_size > NATIVE_POOL_BYTE_LIMIT ||
        limit > NATIVE_POOL_BYTE_LIMIT || limit < sizeof(NativeCodePool)) return NULL;
    uint64_t owner = new_owner();
    if (!owner || !heap.reserve(heap.opaque, sizeof(NativeCodePool))) return NULL;
    NativeCodePool *pool = budget_calloc_category(budget, BUDGET_CATEGORY_JAVASCRIPT,
                                                  1, sizeof(*pool));
    if (!pool) {
        heap.release(heap.opaque, sizeof(NativeCodePool));
        return NULL;
    }
    pool->budget = budget;
    pool->heap = heap;
    pool->backend = backend;
    pool->owner = owner;
    pool->limit = limit;
    pool->charged = sizeof(*pool);
    return pool;
}

static NativeCodeImage *lookup(NativeCodePool *pool, NativeCodeHandle handle)
{
    if (!pool || handle.owner != pool->owner || handle.slot >= NATIVE_POOL_IMAGE_LIMIT)
        return NULL;
    NativeCodeImage *image = &pool->images[handle.slot];
    return image->state != IMAGE_FREE && image->generation == handle.generation ? image : NULL;
}

static bool collect(NativeCodePool *pool, NativeCodeImage *image)
{
    if (image->state != IMAGE_RETIRED) return false;
    for (unsigned i = 0; i < NATIVE_POOL_LEASE_LIMIT; i++)
        if (image->leases[i]) return false;
    if (!pool->backend.unmap(pool->backend.opaque, image->memory, image->capacity))
        return false;
    size_t bytes = image->capacity;
    budget_reservation_release(&image->reservation);
    pool->heap.release(pool->heap.opaque, bytes);
    pool->charged -= bytes;
    uint64_t generation = image->generation;
    memset(image, 0, sizeof(*image));
    image->generation = generation;
    return true;
}

bool native_code_pool_allocate(NativeCodePool *pool, size_t bytes, NativeCodeHandle *handle)
{
    if (!pool || pool->retiring || !handle || !bytes || bytes > pool->limit) return false;
    size_t remainder = bytes % pool->backend.page_size;
    size_t padding = remainder ? pool->backend.page_size - remainder : 0;
    if (padding > pool->limit - bytes) return false;
    size_t capacity = bytes + padding;
    if (capacity > pool->limit - pool->charged) return false;
    unsigned slot;
    for (slot = 0; slot < NATIVE_POOL_IMAGE_LIMIT; slot++)
        if (pool->images[slot].state == IMAGE_FREE && pool->images[slot].generation != UINT64_MAX)
            break;
    if (slot == NATIVE_POOL_IMAGE_LIMIT || !pool->heap.reserve(pool->heap.opaque, capacity))
        return false;
    NativeCodeImage *image = &pool->images[slot];
    if (!budget_reservation_acquire(&image->reservation, pool->budget,
                                     BUDGET_CATEGORY_JAVASCRIPT, capacity)) {
        pool->heap.release(pool->heap.opaque, capacity);
        return false;
    }
    void *memory = pool->backend.map_writable(pool->backend.opaque, capacity);
    if (!memory) {
        budget_reservation_release(&image->reservation);
        pool->heap.release(pool->heap.opaque, capacity);
        return false;
    }
    image->memory = memory;
    image->capacity = capacity;
    image->state = IMAGE_WRITABLE;
    image->generation++;
    pool->charged += capacity;
    *handle = (NativeCodeHandle){ pool->owner, image->generation, slot };
    return true;
}

bool native_code_pool_write(NativeCodePool *pool, NativeCodeHandle handle,
                            size_t offset, const void *bytes, size_t count)
{
    NativeCodeImage *image = lookup(pool, handle);
    if (!image || pool->retiring || image->state != IMAGE_WRITABLE || !bytes ||
        !count || offset > image->capacity || count > image->capacity - offset) return false;
    memmove((unsigned char *)image->memory + offset, bytes, count);
    if (offset + count > image->used) image->used = offset + count;
    return true;
}

bool native_code_pool_publish(NativeCodePool *pool, NativeCodeHandle handle)
{
    NativeCodeImage *image = lookup(pool, handle);
    if (!image || pool->retiring || image->state != IMAGE_WRITABLE || !image->used) return false;
    if (!pool->backend.publish(pool->backend.opaque, image->memory, image->capacity)) {
        image->state = IMAGE_RETIRED;
        (void) collect(pool, image);
        return false;
    }
    image->state = IMAGE_PUBLISHED;
    return true;
}

static bool enter_image(NativeCodePool *pool, const NativeCodeHandle *handle,
                        size_t offset, NativeCodeLease *lease)
{
    if (!handle) return false;
    NativeCodeImage *image = lookup(pool, *handle);
    if (!image || !lease || pool->retiring || image->state != IMAGE_PUBLISHED ||
        offset >= image->used || pool->next_lease == UINT64_MAX) return false;
    for (unsigned i = 0; i < NATIVE_POOL_LEASE_LIMIT; i++) {
        if (image->leases[i]) continue;
        uint64_t id = ++pool->next_lease;
        image->leases[i] = id;
        *lease = (NativeCodeLease){*handle, id, (const unsigned char *)image->memory + offset};
        return true;
    }
    return false;
}

static bool leave_image(NativeCodePool *pool, const NativeCodeLease *lease)
{
    if (!lease) return false;
    NativeCodeImage *image = lookup(pool, lease->image);
    if (!image || !lease->id) return false;
    for (unsigned i = 0; i < NATIVE_POOL_LEASE_LIMIT; i++) {
        if (image->leases[i] != lease->id) continue;
        image->leases[i] = 0;
        if (image->state == IMAGE_RETIRED) (void) collect(pool, image);
        return true;
    }
    return false;
}

bool native_code_pool_enter(NativeCodePool *pool, NativeCodeHandle handle,
                            size_t offset, NativeCodeLease *lease)
{
    /* Keep the original value-ABI control intact. Taking &handle in a shared
       wrapper makes o32 materialize another 24-byte copy, biasing the A/B. */
    NativeCodeImage *image = lookup(pool, handle);
    if (!image || !lease || pool->retiring || image->state != IMAGE_PUBLISHED ||
        offset >= image->used || pool->next_lease == UINT64_MAX) return false;
    for (unsigned i = 0; i < NATIVE_POOL_LEASE_LIMIT; i++) {
        if (image->leases[i]) continue;
        uint64_t id = ++pool->next_lease;
        image->leases[i] = id;
        *lease = (NativeCodeLease){handle, id, (const unsigned char *)image->memory + offset};
        return true;
    }
    return false;
}
bool native_code_pool_leave(NativeCodePool *pool, NativeCodeLease lease)
{
    return leave_image(pool, &lease);
}
bool native_code_pool_enter_ref(NativeCodePool *pool, const NativeCodeHandle *handle,
                                size_t offset, NativeCodeLease *lease)
{
    return enter_image(pool, handle, offset, lease);
}
bool native_code_pool_leave_ref(NativeCodePool *pool, const NativeCodeLease *lease)
{
    return leave_image(pool, lease);
}

bool native_code_pool_retire(NativeCodePool *pool, NativeCodeHandle handle)
{
    NativeCodeImage *image = lookup(pool, handle);
    if (!image) return false;
    image->state = IMAGE_RETIRED;
    (void) collect(pool, image);
    return true;
}

bool native_code_pool_destroy(NativeCodePool **owner)
{
    if (!owner || !*owner) return true;
    NativeCodePool *pool = *owner;
    pool->retiring = true;
    bool idle = true;
    for (unsigned i = 0; i < NATIVE_POOL_IMAGE_LIMIT; i++) {
        NativeCodeImage *image = &pool->images[i];
        if (image->state == IMAGE_FREE) continue;
        image->state = IMAGE_RETIRED;
        if (!collect(pool, image)) idle = false;
    }
    if (!idle) return false;
    pool->heap.release(pool->heap.opaque, sizeof(*pool));
    Budget *budget = pool->budget;
    budget_free(budget, pool);
    *owner = NULL;
    return true;
}
