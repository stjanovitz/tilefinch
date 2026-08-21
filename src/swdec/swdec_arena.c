#include "swdec_arena.h"

#include <stdint.h>
#include <string.h>

/* Block header precedes every user pointer. `size` counts the whole block
   (header + payload + alignment slack); its low bit marks 'used'. `user`
   is the pointer handed out (payload may sit past the header when the
   requested alignment exceeds 16). */
typedef struct block {
    size_t size;
    struct block *next;   /* free-list link (valid only when free) */
    void *user;           /* payload pointer (valid only when used) */
    size_t pad;           /* keep sizeof(block) a multiple of 16 */
} block;

#define ALIGN 64u   /* one cache line: no two heap objects share a line (ME/CPU split) */
#define HDR ((sizeof(block) + ALIGN - 1u) & ~(ALIGN - 1u))

/* Two independent arenas: [0] is the main one (video, CPU); [1] is the aux
   arena for code running on the other core (audio on the Media Engine).
   Allocation routes to [1] when the caller's stack pointer lies in the
   registered aux range (the ME's stack) or while the route-aux flag is set
   (aux setup on the CPU); free/realloc route by pointer containment, so a
   block is always returned to the arena it came from. Each core only ever
   touches its own arena concurrently. */
typedef struct arena_state {
    uint8_t *arena_base;
    size_t arena_bytes;
    block *free_head;
    size_t in_use;
    size_t peak;
    uint8_t pad[64 - 5 * sizeof(void *) % 64];   /* one cache line per state: the two
        arenas are owned by different cores; a shared line would let one core's
        writeback clobber the other's state in RAM */
} arena_state;
_Static_assert(sizeof(struct arena_state) % 64 == 0, "arena_state must fill whole cache lines");
static arena_state ar[2] __attribute__((aligned(64)));
static volatile uintptr_t aux_sp_lo, aux_sp_hi;
static volatile int route_aux;

static arena_state *by_sp(void)
{
    uintptr_t sp, lo = aux_sp_lo, hi = aux_sp_hi;
#if defined(PSP) || defined(__mips__)
    __asm__ volatile ("move %0, $sp" : "=r"(sp));
    /* the ME runs its stack through the kseg0 alias of the same RAM the CPU
       registered as a kuseg address: compare physical addresses */
    sp &= 0x1FFFFFFFu; lo &= 0x1FFFFFFFu; hi &= 0x1FFFFFFFu;
#else
    int probe; sp = (uintptr_t) &probe;
#endif
    if (route_aux || (sp >= lo && sp < hi)) {
        swdec_arena_aux_routed_cpu++;      /* counted on both cores; the CPU share dominates when misrouting */
        return &ar[1];
    }
    return &ar[0];
}
static arena_state *by_ptr(const void *p)
{
    uintptr_t q = (uintptr_t) p, b = (uintptr_t) ar[1].arena_base;
#if defined(PSP) || defined(__mips__)
    q &= 0x1FFFFFFFu; b &= 0x1FFFFFFFu;
#endif
    if (ar[1].arena_base != NULL && q >= b && q < b + ar[1].arena_bytes)
        return &ar[1];
    return &ar[0];
}

static size_t round_up(size_t n) { return (n + ALIGN - 1u) & ~(ALIGN - 1u); }

static void bind_state(arena_state *s, void *base, size_t bytes)
{
    s->arena_base = (uint8_t *) base;
    s->arena_bytes = bytes;
    s->in_use = 0;
    s->peak = 0;
    uintptr_t start = ((uintptr_t) base + ALIGN - 1u) & ~(uintptr_t) (ALIGN - 1u);
    size_t usable = bytes - (size_t) (start - (uintptr_t) base);
    s->free_head = (block *) start;
    s->free_head->size = usable & ~(size_t) 1u;
    s->free_head->next = NULL;
}

void swdec_arena_bind(void *base, size_t bytes) { bind_state(&ar[0], base, bytes); }
void swdec_arena_bind_aux(void *base, size_t bytes) { bind_state(&ar[1], base, bytes); }
void swdec_arena_unbind_aux(void)
{
    ar[1].arena_base = NULL;
    ar[1].arena_bytes = 0;
    ar[1].free_head = NULL;
    ar[1].in_use = 0;
    ar[1].peak = 0;
}
void swdec_arena_aux_sp_range(void *lo, void *hi) { aux_sp_lo = (uintptr_t) lo; aux_sp_hi = (uintptr_t) hi; }
void swdec_arena_route_aux(int on) { route_aux = on; }
void swdec_arena_unbind(void) { ar[0].arena_base = NULL; ar[0].arena_bytes = 0; ar[0].free_head = NULL; }
size_t swdec_arena_peak(void) { return ar[0].peak; }
size_t swdec_arena_in_use(void) { return ar[0].in_use; }
size_t swdec_arena_aux_peak(void) { return ar[1].peak; }
void *swdec_arena_aux_base(void) { return ar[1].arena_base; }
size_t swdec_arena_aux_bytes(void) { return ar[1].arena_bytes; }

/* Payload for `align` may need up to align-16 extra bytes past HDR. */
static block *carve(size_t payload, size_t align)
{
    arena_state *s = by_sp();
    payload = (payload + 63u) & ~(size_t) 63u;   /* whole cache lines per object */
    if (s->arena_base == NULL) return NULL;
    size_t slack = align > ALIGN ? align - ALIGN : 0;
    size_t need = HDR + slack + round_up(payload == 0 ? 1u : payload);
    block **link = &s->free_head;
    for (block *b = s->free_head; b != NULL; link = &b->next, b = b->next) {
        size_t bsize = b->size & ~(size_t) 1u;
        if (bsize < need) continue;
        if (bsize - need >= HDR + ALIGN) {
            block *rest = (block *) ((uint8_t *) b + need);
            rest->size = (bsize - need) & ~(size_t) 1u;
            rest->next = b->next;
            *link = rest;
            b->size = need | 1u;
        } else {
            *link = b->next;
            b->size = bsize | 1u;
        }
        uintptr_t p = (uintptr_t) b + HDR;
        p = (p + align - 1u) & ~(uintptr_t) (align - 1u);
        b->user = (void *) p;
        s->in_use += b->size & ~(size_t) 1u;
        if (s->in_use > s->peak) s->peak = s->in_use;
        return b;
    }
    return NULL;
}

/* Find the header for a user pointer: walk back to the block whose `user`
   equals it. Because payload sits at most `align-16` past HDR and blocks are
   contiguous, scanning backwards from p-HDR over 16-byte steps finds it. */
static block *owner(arena_state *s, void *pointer)
{
    uintptr_t p = (uintptr_t) pointer;
    for (uintptr_t h = p - HDR; h >= (uintptr_t) s->arena_base; h -= ALIGN) {
        block *b = (block *) h;
        if ((b->size & 1u) && b->user == pointer) return b;
    }
    return NULL;
}

/* diagnostics */
unsigned swdec_arena_aux_routed_cpu;   /* allocations routed to aux via the route flag or sp range */
void swdec_arena_debug(int which, unsigned out[4])
{
    arena_state *s = &ar[which & 1];
    unsigned n = 0; size_t largest = 0;
    for (block *b = s->free_head; b != NULL && n < 10000; b = b->next) {
        size_t sz = b->size & ~(size_t) 1u;
        if (sz > largest) largest = sz;
        n++;
    }
    out[0] = n; out[1] = (unsigned) largest; out[2] = (unsigned) s->in_use; out[3] = (unsigned) (uintptr_t) s->free_head;
}

void *swdec_malloc(size_t bytes)
{
    block *b = carve(bytes, ALIGN);
    return b ? b->user : NULL;
}

void *swdec_memalign(size_t align, size_t bytes)
{
    if (align < ALIGN) align = ALIGN;
    block *b = carve(bytes, align);
    return b ? b->user : NULL;
}

int swdec_posix_memalign(void **out, size_t align, size_t bytes)
{
    void *p = swdec_memalign(align, bytes);
    if (p == NULL) return 12; /* ENOMEM */
    *out = p;
    return 0;
}

void swdec_free(void *pointer)
{
    arena_state *s;
    if (pointer == NULL) return;
    s = by_ptr(pointer);
    if (s->arena_base == NULL) return;
    block *b = owner(s, pointer);
    if (b == NULL) return;
    size_t bsize = b->size & ~(size_t) 1u;
    s->in_use -= bsize;
    b->size = bsize;
    block **link = &s->free_head;
    block *cur = s->free_head;
    while (cur != NULL && cur < b) { link = &cur->next; cur = cur->next; }
    b->next = cur;
    *link = b;
    if (cur != NULL && (uint8_t *) b + b->size == (uint8_t *) cur) {
        b->size += cur->size;
        b->next = cur->next;
    }
    if (link != &s->free_head) {
        block *prev = (block *) ((uint8_t *) link - offsetof(block, next));
        if ((uint8_t *) prev + prev->size == (uint8_t *) b) {
            prev->size += b->size;
            prev->next = b->next;
        }
    }
}

void *swdec_realloc(void *pointer, size_t bytes)
{
    if (pointer == NULL) return swdec_malloc(bytes);
    if (bytes == 0) { swdec_free(pointer); return NULL; }
    block *b = owner(by_ptr(pointer), pointer);
    if (b == NULL) return NULL;
    size_t old_payload = (b->size & ~(size_t) 1u) - (size_t) ((uint8_t *) b->user - (uint8_t *) b);
    if (old_payload >= bytes) return pointer;
    void *n = swdec_malloc(bytes);
    if (n == NULL) return NULL;
    memcpy(n, pointer, old_payload);
    swdec_free(pointer);
    return n;
}
