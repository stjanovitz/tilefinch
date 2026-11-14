/* The page-capture worker's transfer loop and block-pair custody
   (tilefinch/media_capture_input.h). */
#include "tilefinch/media_capture_input.h"

#include <string.h>

MediaCaptureInputState media_capture_input_run(
    const MediaCaptureInputBackend *backend, int16_t *pair, bool *pending)
{
    int16_t *blocks[2] = {pair, pair + MEDIA_CAPTURE_BLOCK_SAMPLES};
    unsigned current = 0;
    bool in_flight = backend->begin(backend->user, blocks[current]) >= 0;
    MediaCaptureInputState state = in_flight ? MEDIA_CAPTURE_INPUT_STARTING
                                             : MEDIA_CAPTURE_INPUT_NO_DEVICE;
    uint32_t waited_us = 0;
    /* Bounded: every pass either sleeps MEDIA_CAPTURE_INPUT_POLL_US toward
       the stall limit or completes a block; a stop ends it at the next
       pass. */
    while (in_flight && !backend->stop_requested(backend->user)) {
        int poll = backend->poll(backend->user);
        if (poll < 0) {
            /* An error says nothing about the transfer: it stays in
               flight until a poll answers 0. */
            state = state == MEDIA_CAPTURE_INPUT_STARTING
                ? MEDIA_CAPTURE_INPUT_NO_DEVICE : MEDIA_CAPTURE_INPUT_FAILED;
            break;
        }
        if (poll != 0) {
            if (waited_us >= MEDIA_CAPTURE_INPUT_STALL_US) {
                state = state == MEDIA_CAPTURE_INPUT_STARTING
                    ? MEDIA_CAPTURE_INPUT_NO_DEVICE
                    : MEDIA_CAPTURE_INPUT_STALLED;
                break;
            }
            backend->sleep_us(backend->user, MEDIA_CAPTURE_INPUT_POLL_US);
            waited_us += MEDIA_CAPTURE_INPUT_POLL_US;
            continue;
        }
        waited_us = 0;
        unsigned done = current;
        current ^= 1u;
        in_flight = backend->begin(backend->user, blocks[current]) >= 0;
        backend->arrived(backend->user, blocks[done]);
        if (state == MEDIA_CAPTURE_INPUT_STARTING) {
            state = MEDIA_CAPTURE_INPUT_RUNNING;
            backend->running(backend->user);
        }
        if (!in_flight) state = MEDIA_CAPTURE_INPUT_FAILED;
    }
    /* Give a transfer in flight a short while to end, so an ordinary stop
       frees its pair; only a poll answering 0 proves it ended. */
    for (uint32_t waited = 0; in_flight; waited += MEDIA_CAPTURE_INPUT_POLL_US) {
        if (backend->poll(backend->user) == 0) {
            in_flight = false;
            break;
        }
        if (waited >= MEDIA_CAPTURE_INPUT_DRAIN_US) break;
        backend->sleep_us(backend->user, MEDIA_CAPTURE_INPUT_POLL_US);
    }
    if (state == MEDIA_CAPTURE_INPUT_RUNNING) state = MEDIA_CAPTURE_INPUT_EXITED;
    *pending = in_flight;
    return state;
}

int16_t *media_capture_input_pair_acquire(
    MediaCaptureInputCustody *custody, Budget *budget,
    const MediaCaptureInputBackend *backend, MediaCaptureVerdict *verdict)
{
    if (custody->held != NULL) {
        /* One poll, on the browser thread: the held pair is reused once
           the input is idle, and a start waits until it is. */
        if (backend->poll(backend->user) != 0) {
            *verdict = MEDIA_CAPTURE_BUSY;
            return NULL;
        }
        int16_t *pair = custody->held;
        custody->held = NULL;
        memset(pair, 0, MEDIA_CAPTURE_INPUT_PAIR_BYTES);
        *verdict = MEDIA_CAPTURE_GRANTED;
        return pair;
    }
    int16_t *pair = budget_malloc_cacheline_category(
        budget, BUDGET_CATEGORY_RESOURCE, MEDIA_CAPTURE_INPUT_PAIR_BYTES);
    if (pair == NULL) {
        *verdict = MEDIA_CAPTURE_FAILED;
        return NULL;
    }
    memset(pair, 0, MEDIA_CAPTURE_INPUT_PAIR_BYTES);
    *verdict = MEDIA_CAPTURE_GRANTED;
    return pair;
}

bool media_capture_input_settle(MediaCaptureInputCustody *custody,
                                Budget *budget,
                                const MediaCaptureInputBackend *backend)
{
    if (custody->held == NULL) return true;
    if (backend->poll(backend->user) != 0) return false;
    int16_t *pair = custody->held;
    custody->held = NULL;
    memset(pair, 0, MEDIA_CAPTURE_INPUT_PAIR_BYTES);
    budget_free(budget, pair);
    return true;
}

void media_capture_input_pair_release(MediaCaptureInputCustody *custody,
                                      Budget *budget, int16_t *pair,
                                      bool pending)
{
    if (pair == NULL) return;
    /* Nothing heard outlives the capture (a held pair is zeroed again when
       it is reused). */
    memset(pair, 0, MEDIA_CAPTURE_INPUT_PAIR_BYTES);
    if (pending) {
        /* Starts are refused while a pair is held, so this is the only
           one: the input may still write it, so it is never freed. */
        custody->held = pair;
        return;
    }
    budget_free(budget, pair);
}
