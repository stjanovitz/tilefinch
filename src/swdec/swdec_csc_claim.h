#ifndef TILEFINCH_SWDEC_CSC_CLAIM_H
#define TILEFINCH_SWDEC_CSC_CLAIM_H

#include <stdbool.h>

/*
 * Ownership of a colour-conversion slot between the decoding thread (main
 * CPU) and the Media Engine.
 *
 * The CPU offers one slot at a time (`active`). The ME converts the offered
 * slot's final rows in small batches; before touching the slot it announces
 * the batch (`busy`) and then checks that the slot is still offered. The CPU
 * withdraws the offer (`active = -1`) and then waits until no batch is
 * announced. With a full barrier between each side's store and its load of
 * the other word (Dekker's handshake), either the ME sees the withdrawal and
 * backs off without touching the slot, or the CPU sees the announced batch
 * and waits for it to end. So once the CPU's wait returns, no ME batch can
 * write the slot's pixels or its completion mask until the CPU offers a slot
 * again: the CPU may finish the picture, publish it, rebind the slot to the
 * next picture or release it.
 *
 * For an already decoded picture, the offered range may be only a prefix
 * of complete eight-row units. The CPU may convert the cache-line-disjoint
 * suffix concurrently, but must still withdraw/join before changing the
 * binding or completion mask, finishing prefix rows, publishing, or freeing
 * the destination. No new frame is decoded during that synchronous split.
 *
 * The ME ends a batch only after its pixels and the mask bits it set are in
 * RAM (its D-cache written back), so the CPU reads a final mask.
 *
 * Both cores reach the words through the uncached alias on the PSP; the
 * struct owns its cache line so no cached neighbour's writeback can carry a
 * stale copy over them.
 */
typedef struct {
    volatile int active;      /* slot offered to the ME, -1 = none */
    volatile int busy;        /* slot of the ME's batch in progress, -1 = none */
    volatile unsigned taken;  /* ME: batches it started (diagnostics) */
    volatile unsigned backed; /* ME: offers withdrawn under it (diagnostics) */
    volatile unsigned pad[12];
} SwdecCscClaim;
_Static_assert(sizeof(SwdecCscClaim) == 64u,
               "the conversion claim must own its cache line");

#if defined(__mips__)
#define SWDEC_CSC_CLAIM_FENCE() __asm__ volatile ("sync" ::: "memory")
#else
#include <stdatomic.h>
#define SWDEC_CSC_CLAIM_FENCE() atomic_thread_fence(memory_order_seq_cst)
#endif

/* Host tests widen the ME's window between reading the offer and
   announcing its batch, where a missing re-check would let it through. */
#ifndef SWDEC_CSC_CLAIM_TAKE_GAP
#define SWDEC_CSC_CLAIM_TAKE_GAP() ((void) 0)
#endif

static inline void swdec_csc_claim_init(SwdecCscClaim *claim)
{
    claim->active = -1;
    claim->busy = -1;
    claim->taken = 0;
    claim->backed = 0;
    SWDEC_CSC_CLAIM_FENCE();
}

/* CPU: offer `slot` once its binding (destination, cleared mask) is in
   place. Only after a withdrawal has completed (or from idle). */
static inline void swdec_csc_claim_offer(SwdecCscClaim *claim, int slot)
{
    SWDEC_CSC_CLAIM_FENCE();
    claim->active = slot;
    SWDEC_CSC_CLAIM_FENCE();
}

/* CPU: withdraw the offer; returns the slot that was offered (-1 = none).
   Wait with swdec_csc_claim_held() before touching the slot. */
static inline int swdec_csc_claim_withdraw(SwdecCscClaim *claim)
{
    int slot = claim->active;
    claim->active = -1;
    SWDEC_CSC_CLAIM_FENCE();
    return slot;
}

/* CPU, after a withdrawal: true while an ME batch may still run. */
static inline bool swdec_csc_claim_held(const SwdecCscClaim *claim)
{
    SWDEC_CSC_CLAIM_FENCE();
    return claim->busy >= 0;
}

/* ME: start a batch on the offered slot; -1 = nothing offered (or the offer
   was withdrawn meanwhile), and then the slot must not be touched. */
static inline int swdec_csc_claim_take(SwdecCscClaim *claim)
{
    int slot = claim->active;
    if (slot < 0) return -1;
    SWDEC_CSC_CLAIM_TAKE_GAP();
    claim->busy = slot;
    SWDEC_CSC_CLAIM_FENCE();
    if (claim->active != slot) {
        claim->busy = -1;
        claim->backed = claim->backed + 1u;
        SWDEC_CSC_CLAIM_FENCE();
        return -1;
    }
    claim->taken = claim->taken + 1u;
    return slot;
}

/* ME: end the batch, after its pixels and mask bits are in RAM. */
static inline void swdec_csc_claim_release(SwdecCscClaim *claim)
{
    SWDEC_CSC_CLAIM_FENCE();
    claim->busy = -1;
    SWDEC_CSC_CLAIM_FENCE();
}

/* CPU: take the slot back, bounded. A batch is at most a few eight-row
   units and one cache sweep (about 1.5 ms), so a batch still announced
   after `timeout_us` means the ME is wedged. Its writes have then not
   provably ended, so the CPU must not finish, publish or reuse the slot on
   the strength of the timeout: the ME is stopped first (`stop_me`, which
   returns true once the ME is held in reset and can write nothing more),
   and when it cannot be stopped the slot stays the ME's
   (SWDEC_CSC_RECLAIM_HELD, slot in *held_slot): the caller drops its
   picture and never hands the slot out again until a reclaim finds no
   batch announced. */
typedef enum {
    SWDEC_CSC_RECLAIM_OK = 0,       /* no batch was announced, or it ended */
    SWDEC_CSC_RECLAIM_STOPPED = 1,  /* timed out; the ME was stopped */
    SWDEC_CSC_RECLAIM_HELD = 2      /* timed out; the ME may still write */
} SwdecCscReclaim;

typedef struct {
    unsigned (*now_us)(void *context);
    void (*pause)(void *context, unsigned waited_us);
    bool (*stop_me)(void *context);
    void *context;
} SwdecCscReclaimOps;

static inline SwdecCscReclaim swdec_csc_claim_reclaim(
    SwdecCscClaim *claim, unsigned timeout_us,
    const SwdecCscReclaimOps *ops, int *held_slot)
{
    if (held_slot != NULL) *held_slot = -1;
    (void) swdec_csc_claim_withdraw(claim);
    if (!swdec_csc_claim_held(claim)) return SWDEC_CSC_RECLAIM_OK;
    unsigned started = ops->now_us(ops->context);
    for (;;) {
        if (!swdec_csc_claim_held(claim)) return SWDEC_CSC_RECLAIM_OK;
        unsigned waited = ops->now_us(ops->context) - started;
        if (waited >= timeout_us) break;
        ops->pause(ops->context, waited);
    }
    int slot = claim->busy;
    if (slot < 0) return SWDEC_CSC_RECLAIM_OK;   /* ended at the limit */
    if (ops->stop_me(ops->context)) {
        /* Held in reset: the announced batch can never end by itself. */
        claim->busy = -1;
        SWDEC_CSC_CLAIM_FENCE();
        return SWDEC_CSC_RECLAIM_STOPPED;
    }
    if (held_slot != NULL) *held_slot = slot;
    return SWDEC_CSC_RECLAIM_HELD;
}

/* The ME's share of one picture, as bucket 0 (none) .. 5 (all): 1-4 are
   quarters of the picture's units (up to 25 %, 50 %, 75 %, under 100 %). */
#define SWDEC_CSC_SHARE_BUCKETS 6u
static inline unsigned swdec_csc_share_bucket(unsigned me_units,
                                              unsigned all_units)
{
    if (me_units == 0u || all_units == 0u) return 0u;
    if (me_units >= all_units) return 5u;
    unsigned quarter = (me_units * 4u + all_units - 1u) / all_units;
    return quarter < 1u ? 1u : quarter > 4u ? 4u : quarter;
}

#endif
