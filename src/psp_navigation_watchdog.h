#ifndef TILEFINCH_PSP_NAVIGATION_WATCHDOG_H
#define TILEFINCH_PSP_NAVIGATION_WATCHDOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The PSP navigation watchdog cancels a load that has stopped making
 * progress, not one that is merely slow. A fixed one-minute total cancelled
 * chatgpt.com mid-load on hardware: its ~60 ES modules over the PSP's Wi-Fi
 * took 46-88 s of steady progress. The load expires after
 * PSP_NAVIGATION_STALL_US without its progress counter moving (a stalled
 * module fetch was seen waiting 15 s), or at the absolute cap. Circle still
 * cancels at any time. Time the user spends answering a page's alert(),
 * confirm() or prompt(), or typing in the keyboard opened over the loading
 * page, is paused out: the load waits on the reader, not the network.
 */
#define PSP_NAVIGATION_STALL_US UINT64_C(30000000)
#define PSP_NAVIGATION_CAP_US UINT64_C(180000000)

typedef struct {
    uint64_t started_us;
    /* In paused-out time: the clock less paused_us. */
    uint64_t progress_us;
    uint64_t paused_us;
    size_t progress_units;
} PspNavigationWatchdog;

static inline void psp_navigation_watchdog_start(
    PspNavigationWatchdog *watchdog, uint64_t now_us, size_t units)
{
    watchdog->started_us = now_us;
    watchdog->progress_us = now_us;
    watchdog->paused_us = 0;
    watchdog->progress_units = units;
}

/* Leave PAUSED_US out of both the stall and the cap. */
static inline void psp_navigation_watchdog_pause(
    PspNavigationWatchdog *watchdog, uint64_t paused_us)
{
    watchdog->paused_us += paused_us;
}

/* Record the current progress counter; true once the load should be
   cancelled. Clocks are compared by elapsed time. */
static inline bool psp_navigation_watchdog_expired(
    PspNavigationWatchdog *watchdog, uint64_t now_us, size_t units)
{
    now_us = now_us - watchdog->started_us > watchdog->paused_us
        ? now_us - watchdog->paused_us : watchdog->started_us;
    if (units != watchdog->progress_units) {
        watchdog->progress_units = units;
        watchdog->progress_us = now_us;
    }
    return (now_us > watchdog->progress_us
            && now_us - watchdog->progress_us >= PSP_NAVIGATION_STALL_US)
        || now_us - watchdog->started_us >= PSP_NAVIGATION_CAP_US;
}

#endif
