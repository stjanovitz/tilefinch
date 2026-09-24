#ifndef TILEFINCH_PSP_LOAD_EXPERIENCE_H
#define TILEFINCH_PSP_LOAD_EXPERIENCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * What a reader can do while a page is still loading, as opposed to how long
 * the page takes to finish. Validation builds report one line per navigation:
 *
 *  - reach: how far down the page content can be brought on screen at a
 *    given moment. Before commit that is the deepest preview frame; after it
 *    the whole page. "Time to N screens" is when reach first covers N
 *    screen heights (never, if the page is shorter).
 *  - load-time scrolling: page-scroll presses made while the navigation was
 *    pending, how many visibly moved the page, the slowest press-to-movement
 *    latency, and the longest a press waited for content that did not yet
 *    exist (still waiting when the load ended counts until that moment).
 *
 * A toast acknowledging a press is not movement: only a presented page frame
 * at a new position resolves a press. Pure data; host-tested; the PSP
 * frontend feeds it from the input supervisor and the preview scroller.
 * Times are 32-bit microsecond stamps so the supervisor thread can publish
 * them with single aligned stores.
 */

typedef struct {
    uint32_t pending_press_us;  /* oldest unresolved press, 0 = none */
    unsigned presses;
    unsigned moved;
    uint32_t max_move_latency_us;
    uint32_t longest_wait_us;
} PspLoadScroll;

static inline void psp_load_scroll_reset(PspLoadScroll *scroll)
{
    if (scroll == NULL) return;
    scroll->pending_press_us = 0;
    scroll->presses = 0;
    scroll->moved = 0;
    scroll->max_move_latency_us = 0;
    scroll->longest_wait_us = 0;
}

static inline void psp_load_scroll_press(PspLoadScroll *scroll, uint32_t now)
{
    if (scroll == NULL) return;
    scroll->presses++;
    if (scroll->pending_press_us == 0)
        scroll->pending_press_us = now != 0 ? now : 1u;
}

/* A page frame at a new position was presented. */
static inline void psp_load_scroll_moved(PspLoadScroll *scroll, uint32_t now)
{
    if (scroll == NULL) return;
    scroll->moved++;
    if (scroll->pending_press_us == 0) return;
    uint32_t latency = now - scroll->pending_press_us;
    if (latency > scroll->max_move_latency_us)
        scroll->max_move_latency_us = latency;
    if (latency > scroll->longest_wait_us)
        scroll->longest_wait_us = latency;
    scroll->pending_press_us = 0;
}

/* The navigation settled; a press still waiting never moved the preview. */
static inline bool psp_load_scroll_finish(PspLoadScroll *scroll, uint32_t now)
{
    if (scroll == NULL || scroll->pending_press_us == 0) return false;
    uint32_t waited = now - scroll->pending_press_us;
    if (waited > scroll->longest_wait_us) scroll->longest_wait_us = waited;
    scroll->pending_press_us = 0;
    return true;
}

typedef struct {
    uint64_t first_present_us;  /* 0 = no preview was presented */
    uint64_t preview_reach_us;
    int preview_reach_px;
    uint64_t loaded_us;         /* navigation settled with the full page */
    int page_height_px;
    int screen_height_px;
} PspLoadReach;

/* When content at least `screens` screen heights deep first became
   reachable, or UINT64_MAX when it never did (page shorter, or failed). */
static inline uint64_t psp_load_reach_time(const PspLoadReach *reach,
                                           unsigned screens)
{
    if (reach == NULL || reach->screen_height_px <= 0 || screens == 0)
        return UINT64_MAX;
    int64_t needed = (int64_t) screens * reach->screen_height_px;
    /* The first preview frame is one screen; deeper frames come later. */
    if (reach->first_present_us != 0 && needed <= reach->screen_height_px)
        return reach->first_present_us;
    if (reach->first_present_us != 0 && reach->preview_reach_px >= needed)
        return reach->preview_reach_us;
    if (reach->loaded_us != 0 && reach->page_height_px >= needed)
        return reach->loaded_us;
    return UINT64_MAX;
}

/* Screen heights of content reachable at `at_us` after navigation start. */
static inline unsigned psp_load_reach_screens_at(const PspLoadReach *reach,
                                                 uint64_t at_us)
{
    if (reach == NULL || reach->screen_height_px <= 0) return 0;
    int px = 0;
    if (reach->loaded_us != 0 && at_us >= reach->loaded_us) {
        px = reach->page_height_px;
    } else if (reach->first_present_us != 0
               && at_us >= reach->first_present_us) {
        px = at_us >= reach->preview_reach_us
            ? reach->preview_reach_px : reach->screen_height_px;
    }
    return px <= 0 ? 0u : (unsigned) (px / reach->screen_height_px);
}

#endif
