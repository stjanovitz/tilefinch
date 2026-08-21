/*
 * Fixed arena behind libavcodec's allocator.
 *
 * ffmpeg is configured with --malloc-prefix=swdec_ so libavutil/mem.c
 * calls swdec_malloc / swdec_memalign / swdec_posix_memalign /
 * swdec_realloc / swdec_free instead of libc. Every byte the decoder
 * touches therefore comes from the region the caller hands to swdec_open().
 *
 * First-fit free list with address-ordered coalescing over one region;
 * every block is 16-byte aligned and memalign honours larger alignments by
 * over-allocating. Steady-state H.264 decode does not allocate (frame pools
 * are reused), so fragmentation is not a runtime concern; peak is recorded
 * so the arena can be sized from measurement.
 */
#ifndef SWDEC_ARENA_H
#define SWDEC_ARENA_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void swdec_arena_bind(void *base, size_t bytes);   /* single-threaded by design */
void swdec_arena_unbind(void);
/* aux arena for the second core (audio on the ME): allocations route to it
   while route_aux is on or when the caller's $sp is inside the aux range;
   frees route by pointer */
void swdec_arena_bind_aux(void *base, size_t bytes);
void swdec_arena_unbind_aux(void);
void swdec_arena_aux_sp_range(void *lo, void *hi);
void swdec_arena_route_aux(int on);
size_t swdec_arena_aux_peak(void);
void *swdec_arena_aux_base(void);
size_t swdec_arena_aux_bytes(void);
void swdec_arena_debug(int which, unsigned out[4]);
extern unsigned swdec_arena_aux_routed_cpu;
size_t swdec_arena_peak(void);
size_t swdec_arena_in_use(void);

/* The libavutil MALLOC_PREFIX targets. */
void *swdec_malloc(size_t bytes);
void *swdec_memalign(size_t align, size_t bytes);
int swdec_posix_memalign(void **out, size_t align, size_t bytes);
void *swdec_realloc(void *pointer, size_t bytes);
void swdec_free(void *pointer);

#ifdef __cplusplus
}
#endif
#endif
