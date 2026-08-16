#ifndef TILEFINCH_PSP_BOOT_ORDER_H
#define TILEFINCH_PSP_BOOT_ORDER_H

#include <stdbool.h>

/*
 * What the browser puts on screen first, and what it still owes the user
 * afterwards.
 *
 * The device main loop that acts on this decision is PSP-only and cannot be
 * exercised by the host suite, so the decision itself lives here: a pure
 * function over the boot facts, with no engine, filesystem, or UI in it.
 *
 * A direct URL, trace replay, or document-dependent probe keeps the
 * engine-first order. Passive automation is intentionally absent: input
 * scripts and cadence tests must see the same native chrome as a user.
 */

typedef struct {
    /* boot.cfg named a URL other than the built-in start page. An explicit
       URL is a direct-navigation override and is honoured as one. */
    bool url_overridden;
    /* A recorded trace is being replayed instead of live navigation. */
    bool trace_replay;
    /* A document-dependent validation knob is armed for this boot. */
    bool validation_mode;
    /* The profile asked for its last page back and a checkpoint was found. */
    bool restore_last_page;
    /* The profile opted into its own homepage document. */
    bool custom_homepage;
} PspBootInputs;

typedef enum {
    /* Today's order: the engine loads a document, then the browser becomes
       interactive. */
    PSP_BOOT_SURFACE_ENGINE_FIRST = 0,
    /* Native HOME draws first and is interactive with no document open. */
    PSP_BOOT_SURFACE_NATIVE_HOME
} PspBootSurface;

typedef struct {
    PspBootSurface surface;
    /*
     * A navigation boot owes the user once the browser is interactive. It is
     * started behind the first frame rather than in front of it, so a slow or
     * unreachable page can no longer hold the whole browser closed.
     */
    bool deferred_navigation;
} PspBootPlan;

PspBootPlan psp_boot_plan(const PspBootInputs *inputs);

/*
 * The navigation the surface is waiting on, whether it came from boot or from
 * a tile the user activated before the browser finished coming up. Exactly
 * one is held: a later request replaces an earlier one, because the user's
 * most recent choice is the one they are looking at.
 */
#define PSP_BOOT_QUEUE_URL_CAPACITY 1024

typedef struct {
    bool ready;
    bool pending;
    char url[PSP_BOOT_QUEUE_URL_CAPACITY];
} PspBootQueue;

void psp_boot_queue_init(PspBootQueue *queue);
/* Records a wanted navigation. Returns true when it can start immediately. */
bool psp_boot_queue_request(PspBootQueue *queue, const char *url);
void psp_boot_queue_set_ready(PspBootQueue *queue, bool ready);
/* Returns the queued URL and clears it, or NULL while nothing is owed. */
const char *psp_boot_queue_take(PspBootQueue *queue);

#endif
