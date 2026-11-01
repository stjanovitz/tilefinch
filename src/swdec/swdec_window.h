#ifndef SWDEC_WINDOW_H
#define SWDEC_WINDOW_H

#include <stdbool.h>
#include <stdint.h>

/*
 * The steady-playback measurement window (validation builds): fixed in
 * advance, never chosen after a run. It opens on the first unique picture
 * published at least SWDEC_WINDOW_LEAD_US after the first one (startup is
 * reported apart) and closes on the first published at least
 * SWDEC_WINDOW_SPAN_US after it opened. A playback that stops earlier
 * reports the part it covered, marked incomplete.
 *
 * Inside the window, the interval between consecutive unique published
 * pictures goes into a 1 ms histogram (median and p95 from it), with the
 * maximum, the count held longer than 1.5 frame periods, and the three
 * longest holds with the picture time that ended each.
 */
#define SWDEC_WINDOW_LEAD_US UINT64_C(5000000)
#define SWDEC_WINDOW_SPAN_US UINT64_C(30000000)
#define SWDEC_WINDOW_HIST_MS 256u
#define SWDEC_WINDOW_WORST 3u

typedef enum {
    SWDEC_WINDOW_WAITING = 0,   /* before the first published picture */
    SWDEC_WINDOW_LEADING,       /* startup and the lead */
    SWDEC_WINDOW_OPEN,
    SWDEC_WINDOW_CLOSED
} SwdecWindowPhase;

typedef struct {
    SwdecWindowPhase phase;
    uint64_t first_us;      /* first unique picture published */
    uint64_t open_us;
    uint64_t last_us;       /* previous unique picture, in the window */
    uint32_t intervals;
    uint32_t held;          /* intervals over 1.5 frame periods */
    uint32_t max_us;
    uint16_t hist[SWDEC_WINDOW_HIST_MS];   /* last bucket: 255 ms and up */
    uint32_t worst_us[SWDEC_WINDOW_WORST];
    uint64_t worst_pts_us[SWDEC_WINDOW_WORST];
} SwdecWindow;

/* What a published unique picture did to the window. */
typedef enum {
    SWDEC_WINDOW_NOTE_NONE = 0,
    SWDEC_WINDOW_NOTE_OPENED,
    SWDEC_WINDOW_NOTE_CLOSED
} SwdecWindowNote;

static inline void swdec_window_interval(SwdecWindow *window,
                                         uint32_t interval_us,
                                         uint64_t pts_us,
                                         uint32_t period_us)
{
    window->intervals++;
    unsigned ms = interval_us / 1000u;
    if (ms >= SWDEC_WINDOW_HIST_MS) ms = SWDEC_WINDOW_HIST_MS - 1u;
    if (window->hist[ms] != UINT16_MAX) window->hist[ms]++;
    if (interval_us > window->max_us) window->max_us = interval_us;
    if (period_us != 0 && (uint64_t) interval_us * 2u > (uint64_t) period_us * 3u)
        window->held++;
    for (unsigned i = 0; i < SWDEC_WINDOW_WORST; i++) {
        if (interval_us <= window->worst_us[i]) continue;
        for (unsigned j = SWDEC_WINDOW_WORST - 1u; j > i; j--) {
            window->worst_us[j] = window->worst_us[j - 1u];
            window->worst_pts_us[j] = window->worst_pts_us[j - 1u];
        }
        window->worst_us[i] = interval_us;
        window->worst_pts_us[i] = pts_us;
        break;
    }
}

/* A unique picture published at now_us (pts_us, nominal period_us). */
static inline SwdecWindowNote swdec_window_note(SwdecWindow *window,
                                                uint64_t now_us,
                                                uint64_t pts_us,
                                                uint32_t period_us)
{
    switch (window->phase) {
    case SWDEC_WINDOW_WAITING:
        window->first_us = now_us;
        window->phase = SWDEC_WINDOW_LEADING;
        return SWDEC_WINDOW_NOTE_NONE;
    case SWDEC_WINDOW_LEADING:
        if (now_us - window->first_us < SWDEC_WINDOW_LEAD_US)
            return SWDEC_WINDOW_NOTE_NONE;
        window->phase = SWDEC_WINDOW_OPEN;
        window->open_us = window->last_us = now_us;
        return SWDEC_WINDOW_NOTE_OPENED;
    case SWDEC_WINDOW_OPEN: {
        uint64_t interval = now_us - window->last_us;
        swdec_window_interval(
            window, interval > UINT32_MAX ? UINT32_MAX : (uint32_t) interval,
            pts_us, period_us);
        window->last_us = now_us;
        if (now_us - window->open_us < SWDEC_WINDOW_SPAN_US)
            return SWDEC_WINDOW_NOTE_NONE;
        window->phase = SWDEC_WINDOW_CLOSED;
        return SWDEC_WINDOW_NOTE_CLOSED;
    }
    case SWDEC_WINDOW_CLOSED:
    default:
        return SWDEC_WINDOW_NOTE_NONE;
    }
}

/* The interval at `permille` of the window's intervals, in whole
   milliseconds (the bucket's lower edge); 0 with none. */
static inline unsigned swdec_window_percentile_ms(const SwdecWindow *window,
                                                  unsigned permille)
{
    if (window->intervals == 0) return 0;
    uint64_t rank = ((uint64_t) window->intervals * permille + 999u) / 1000u;
    if (rank == 0) rank = 1;
    uint64_t seen = 0;
    for (unsigned ms = 0; ms < SWDEC_WINDOW_HIST_MS; ms++) {
        seen += window->hist[ms];
        if (seen >= rank) return ms;
    }
    return SWDEC_WINDOW_HIST_MS - 1u;
}

#endif
