#ifndef TILEFINCH_SWDEC_AUDIO_FENCE_H
#define TILEFINCH_SWDEC_AUDIO_FENCE_H

#include <stdatomic.h>
#include <stdbool.h>

/*
 * Ownership of the decoded-PCM ring between the audio output thread and a
 * seek on the browser thread.
 *
 * The output call hands the hardware a pointer into the ring. It returns
 * once the block is accepted, and the hardware goes on reading it while the
 * next call blocks. A seek frees the ring, rebinds its arena and reopens the
 * AAC decoder in that memory, so it may start only once no block of the old
 * ring can still be read: the audio thread has stopped taking ring blocks,
 * and an output call that began after that has returned (which drains the
 * last ring block the hardware held).
 *
 * The browser raises the fence and polls `parked` (it never blocks on the
 * audio thread, which outranks it). The audio thread, before each block,
 * asks `may_output`; while the fence is raised it plays silence from its own
 * buffer instead, and acknowledges the raise after that silence returns.
 * A raise acknowledged once stays acknowledged until it is lowered, so a
 * further seek while one is pending reuses it.
 */
typedef struct {
    atomic_uint raised;     /* generation of the current raise; 0 = lowered */
    atomic_uint parked;     /* generation the audio thread acknowledged */
    atomic_uint generation; /* browser: last generation handed out */
    atomic_bool absent;     /* no audio thread is (or will be) running */
} SwdecAudioFence;

static inline void swdec_audio_fence_init(SwdecAudioFence *fence)
{
    atomic_init(&fence->raised, 0u);
    atomic_init(&fence->parked, 0u);
    atomic_init(&fence->generation, 0u);
    atomic_init(&fence->absent, true);
}

/* Audio thread side: the thread exists (true) or has ended (false). An
   ended or never-started thread holds no block, so the fence is open. */
static inline void swdec_audio_fence_set_thread(SwdecAudioFence *fence,
                                                bool running)
{
    atomic_store_explicit(&fence->absent, !running, memory_order_release);
}

/* Browser: stop the audio thread taking ring blocks. Idempotent while
   raised; returns the generation to poll. */
static inline unsigned swdec_audio_fence_raise(SwdecAudioFence *fence)
{
    unsigned current = atomic_load_explicit(
        &fence->raised, memory_order_relaxed);
    if (current != 0u) return current;
    unsigned generation = atomic_load_explicit(
        &fence->generation, memory_order_relaxed) + 1u;
    if (generation == 0u) generation = 1u;
    atomic_store_explicit(&fence->generation, generation,
                          memory_order_relaxed);
    atomic_store_explicit(&fence->raised, generation, memory_order_seq_cst);
    return generation;
}

/* Browser: true once no old ring block can be read any more. */
static inline bool swdec_audio_fence_parked(const SwdecAudioFence *fence)
{
    unsigned raised = atomic_load_explicit(
        &fence->raised, memory_order_relaxed);
    if (raised == 0u) return false;
    return atomic_load_explicit(&fence->absent, memory_order_acquire)
        || atomic_load_explicit(&fence->parked, memory_order_acquire)
            == raised;
}

/* Browser: the ring is valid again (after the epoch the audio thread
   resynchronises on has moved). */
static inline void swdec_audio_fence_lower(SwdecAudioFence *fence)
{
    atomic_store_explicit(&fence->raised, 0u, memory_order_release);
}

/* Audio thread: before taking a ring block. Zero when it may; otherwise the
   raise to acknowledge once the silence it plays instead has returned. */
static inline unsigned swdec_audio_fence_hold(const SwdecAudioFence *fence)
{
    return atomic_load_explicit(&fence->raised, memory_order_seq_cst);
}

/* Audio thread: an output call that STARTED after `hold` returned the raise
   has returned SUCCESSFULLY. Every ring block handed over before then is
   drained. Never acknowledge a failed call: it proves nothing about the
   block before it, so the fence stays up and the seek times out instead. */
static inline void swdec_audio_fence_acknowledge(SwdecAudioFence *fence,
                                                 unsigned raise)
{
    if (raise != 0u)
        atomic_store_explicit(&fence->parked, raise, memory_order_release);
}

#endif
