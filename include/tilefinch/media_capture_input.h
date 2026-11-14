#ifndef TILEFINCH_MEDIA_CAPTURE_INPUT_H
#define TILEFINCH_MEDIA_CAPTURE_INPUT_H

/*
 * The page-capture worker's transfer loop and the custody of the block pair
 * the hardware writes, behind a seam: the PSP frontend supplies
 * sceAudioInput and sceAudioPollInputEnd (src/psp_app/psp_app_media_capture.c),
 * and the host tests a scripted input (tests/test_media_capture.c).
 *
 * The firmware cannot cancel a transfer, and its only wait
 * (sceAudioWaitInputEnd) has no timeout. A poll that answers 0 is the only
 * proof that the input finished writing a block; neither a stop, a poll
 * error nor the worker thread's exit proves it. So a pair whose last
 * transfer was never seen to end is never returned to the budget: it stays
 * in custody, the next start reuses it once a poll shows the input idle,
 * and a start while the input is still busy is refused. Starts are refused
 * while a pair is held, so custody holds at most one pair, however many
 * times capture stops on a stalled input.
 */

#include "tilefinch/budget.h"
#include "tilefinch/media_capture.h"

#include <stdbool.h>
#include <stdint.h>

/* The worker polls a transfer's end this often. */
#define MEDIA_CAPTURE_INPUT_POLL_US 4000u
/* A transfer that has not ended in this long means the input stopped. */
#define MEDIA_CAPTURE_INPUT_STALL_US 500000u
/* After a stop, how long the worker waits for a transfer in flight to end
   before it leaves the pair in custody (a block takes about 23 ms). Well
   inside the frontend's bounded join. */
#define MEDIA_CAPTURE_INPUT_DRAIN_US 100000u
/* The pair: two MEDIA_CAPTURE_BLOCK_SAMPLES blocks. */
#define MEDIA_CAPTURE_INPUT_PAIR_BYTES \
    (2u * MEDIA_CAPTURE_BLOCK_SAMPLES * sizeof(int16_t))

typedef enum {
    MEDIA_CAPTURE_INPUT_STARTING = 0,
    MEDIA_CAPTURE_INPUT_RUNNING,
    MEDIA_CAPTURE_INPUT_NO_DEVICE,
    MEDIA_CAPTURE_INPUT_FAILED,
    MEDIA_CAPTURE_INPUT_STALLED,
    MEDIA_CAPTURE_INPUT_EXITED
} MediaCaptureInputState;

typedef struct {
    void *user;
    /* Starts a non-blocking transfer of MEDIA_CAPTURE_BLOCK_SAMPLES into
       BLOCK (making its cache lines the hardware's first); >= 0 started. */
    int (*begin)(void *user, int16_t *block);
    /* 0 once the last transfer ended, 1 while it runs, < 0 an error. */
    int (*poll)(void *user);
    void (*sleep_us)(void *user, uint32_t microseconds);
    /* The input finished BLOCK: drop stale cache lines and publish it. */
    void (*arrived)(void *user, const int16_t *block);
    bool (*stop_requested)(void *user);
    /* The first block arrived (MEDIA_CAPTURE_INPUT_RUNNING). */
    void (*running)(void *user);
} MediaCaptureInputBackend;

/* Moves blocks through PAIR until a stop or a failure. Every pass either
   sleeps toward MEDIA_CAPTURE_INPUT_STALL_US or completes a block. Returns
   the final state (EXITED after a stop once running, NO_DEVICE when no
   block ever arrived); *PENDING is whether a transfer was started and
   never seen to end, so PAIR must stay in custody. */
MediaCaptureInputState media_capture_input_run(
    const MediaCaptureInputBackend *backend, int16_t *pair, bool *pending);

typedef struct {
    /* A pair whose last transfer was never seen to end (NULL: none). */
    int16_t *held;
} MediaCaptureInputCustody;

/* A zeroed, cache-line-aligned pair for a start, from BUDGET, or the held
   one once BACKEND's poll shows the input idle. NULL with *VERDICT BUSY
   while the held pair may still be written, FAILED when BUDGET refuses. */
int16_t *media_capture_input_pair_acquire(
    MediaCaptureInputCustody *custody, Budget *budget,
    const MediaCaptureInputBackend *backend, MediaCaptureVerdict *verdict);
/* After the worker ended: PAIR is zeroed and returned to BUDGET, or, when
   PENDING, held. */
void media_capture_input_pair_release(MediaCaptureInputCustody *custody,
                                      Budget *budget, int16_t *pair,
                                      bool pending);
/* After the worker ended: whether the input is free for another user of
   it (voice typing's own capture). True with no pair held; with one held,
   one poll on BACKEND, no wait: once it answers 0 the pair is zeroed and
   returned to BUDGET. */
bool media_capture_input_settle(MediaCaptureInputCustody *custody,
                                Budget *budget,
                                const MediaCaptureInputBackend *backend);

#endif
