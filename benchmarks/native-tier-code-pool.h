/* Experimental code ownership. Not exposed to page JavaScript. */
#ifndef TILEFINCH_EXPERIMENTAL_NATIVE_CODE_POOL_H
#define TILEFINCH_EXPERIMENTAL_NATIVE_CODE_POOL_H

#include "tilefinch/budget.h"

#define NATIVE_POOL_IMAGE_LIMIT 16u
#define NATIVE_POOL_LEASE_LIMIT 8u
#define NATIVE_POOL_BYTE_LIMIT (256u * 1024u)

typedef struct NativeCodePool NativeCodePool;
typedef struct {
    uint64_t owner, generation;
    unsigned slot;
} NativeCodeHandle;
typedef struct {
    NativeCodeHandle image;
    uint64_t id;
    const void *entry;
} NativeCodeLease;

/* Single-owner callbacks; no page callbacks or re-entry into the pool.
   Heap reserve/release account external bytes against the JS admission limit.
   They must not replace a pending exception or allocate a dummy payload. */
typedef struct {
    void *opaque;
    bool (*reserve)(void *opaque, size_t bytes);
    void (*release)(void *opaque, size_t bytes);
} NativeCodeHeap;

typedef struct {
    void *opaque;
    size_t page_size;
    void *(*map_writable)(void *opaque, size_t bytes);
    /* Publish synchronizes instruction visibility and, where the platform has
       memory protection, establishes RX (never RWX). PSP RAM has no W^X:
       admission is experimental and never grants author access to the image.
       A failed publish retires the image; it cannot be rewritten. */
    bool (*publish)(void *opaque, void *memory, size_t bytes);
    /* On refusal, the mapping and every charge remain retained for retry. */
    bool (*unmap)(void *opaque, void *memory, size_t bytes);
} NativeCodeBackend;

NativeCodePool *native_code_pool_create(Budget *budget, NativeCodeHeap heap,
                                       NativeCodeBackend backend, size_t limit);
size_t native_code_pool_metadata_bytes(void);
size_t native_code_pool_charged_bytes(const NativeCodePool *pool);
bool native_code_pool_allocate(NativeCodePool *pool, size_t bytes,
                               NativeCodeHandle *handle);
bool native_code_pool_write(NativeCodePool *pool, NativeCodeHandle handle,
                            size_t offset, const void *bytes, size_t count);
bool native_code_pool_publish(NativeCodePool *pool, NativeCodeHandle handle);
/* Entry offsets are trusted emitter output, not author input. This validates
   range/lifetime only; instruction boundaries belong to the future compiler. */
bool native_code_pool_enter(NativeCodePool *pool, NativeCodeHandle handle,
                            size_t offset, NativeCodeLease *lease);
bool native_code_pool_leave(NativeCodePool *pool, NativeCodeLease lease);
/* Synchronous transport control for 32-bit ABIs. Never retain these caller
   pointers; identity and lease validation are identical to the value API. */
bool native_code_pool_enter_ref(NativeCodePool *pool, const NativeCodeHandle *handle,
                                size_t offset, NativeCodeLease *lease);
bool native_code_pool_leave_ref(NativeCodePool *pool, const NativeCodeLease *lease);
/* True means retirement was recorded, not that unmapping has completed. */
bool native_code_pool_retire(NativeCodePool *pool, NativeCodeHandle handle);
/* Retirement is terminal for this pool and refuses all subsequent admission.
   Destroy returns false while leases or unreleased mappings remain. */
bool native_code_pool_destroy(NativeCodePool **pool);

#endif
