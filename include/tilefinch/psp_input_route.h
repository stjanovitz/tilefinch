#ifndef TILEFINCH_PSP_INPUT_ROUTE_H
#define TILEFINCH_PSP_INPUT_ROUTE_H

#include <stdbool.h>
#include <stdint.h>

#include "tilefinch/psp_ui.h"

/*
 * Where one button press goes while the browser thread is inside bounded
 * work and the input supervisor (the callback thread, or an owner-thread
 * checkpoint) receives it. Pure: the supervisor gathers its context, asks
 * for a route, and performs it.
 *
 * Who owns input is the supervisor's state:
 *  - a loading page: a navigation is pending. The browser loop stays live
 *    between pumps and acts on forwarded presses; the supervisor itself
 *    scrolls the preview and cancels the load. While the loop shows a menu,
 *    or has presses it has not taken yet, it gets every press (Circle
 *    included) so their order holds.
 *  - a page service: the committed page is borrowed by bounded work
 *    (runtime turn, raster). Native menus and the cursor may be handled
 *    first (priority); other presses queue for the page.
 *  - media: the native player's controls.
 * Optional work (a font batch, the rest of a provisional layout) yields to
 * input and reruns, except that scrolling down while the reader waits at
 * the bottom for that very layout queues for the page instead.
 * Precedence below is the order of the checks in psp_input_route().
 */

typedef enum {
    PSP_INPUT_OWNER_PAGE_SERVICE = 0,
    PSP_INPUT_OWNER_LOADING_PAGE,
    PSP_INPUT_OWNER_MEDIA
} PspInputOwner;

typedef enum {
    /* The browser loop shows the page and has taken every forwarded press. */
    PSP_INPUT_LOOP_ON_PAGE = 0,
    /* It shows a menu or another native screen over the loading page. */
    PSP_INPUT_LOOP_IN_MENU,
    /* It has forwarded presses it has not taken yet. */
    PSP_INPUT_LOOP_BEHIND
} PspInputLoopState;

typedef struct {
    PspInputOwner owner;
    PspInputLoopState loop;
    uint32_t pressed;
    bool analog_active;
    /* The supervisor runs on the browser thread (a checkpoint). */
    bool owner_thread;
    /* The work in progress yields to any input and reruns. */
    bool optional_preemptible;
    /* That work lays out the screens the reader is waiting for at the
       bottom of the page: scrolling further down cannot move the page, so
       those presses wait for it rather than cancel it. */
    bool forward_awaited;
    /* A native menu or cursor already took this press. */
    bool priority_handled;
    /* A cancellation is already in flight. */
    bool cancelling;
    bool acknowledge_busy;
} PspInputRouteContext;

typedef enum {
    PSP_INPUT_ROUTE_NONE = 0,
    /* Cancel the optional work; queue the press for the page. */
    PSP_INPUT_ROUTE_YIELD,
    /* Already applied by the native menu or cursor. */
    PSP_INPUT_ROUTE_PRIORITY_DONE,
    /* To the browser loop's queue. */
    PSP_INPUT_ROUTE_FORWARD,
    /* Stop the work (Circle). */
    PSP_INPUT_ROUTE_CANCEL,
    /* The player's controls. */
    PSP_INPUT_ROUTE_MEDIA,
    /* Page the loading preview (Up/Down/L/R). */
    PSP_INPUT_ROUTE_SCROLL,
    /* A cancellation is in flight: say it is still stopping. */
    PSP_INPUT_ROUTE_STILL_STOPPING,
    /* Hold for the committed page's controller. */
    PSP_INPUT_ROUTE_QUEUE_PAGE,
    /* Acknowledge as busy (Circle cancels). */
    PSP_INPUT_ROUTE_BUSY
} PspInputRoute;

#define PSP_INPUT_ROUTE_SCROLL_BUTTONS \
    (PSP_UI_BUTTON_UP | PSP_UI_BUTTON_DOWN | PSP_UI_BUTTON_PAGE_UP \
     | PSP_UI_BUTTON_PAGE_DOWN)
#define PSP_INPUT_ROUTE_FORWARD_BUTTONS \
    (PSP_UI_BUTTON_DOWN | PSP_UI_BUTTON_PAGE_DOWN)
#define PSP_INPUT_ROUTE_MEDIA_BUTTONS \
    (PSP_UI_BUTTON_LEFT | PSP_UI_BUTTON_RIGHT | PSP_UI_BUTTON_PAGE_UP \
     | PSP_UI_BUTTON_PAGE_DOWN | PSP_UI_BUTTON_CONFIRM)

static inline PspInputRoute psp_input_route(const PspInputRouteContext *c)
{
    if (c == NULL) return PSP_INPUT_ROUTE_NONE;
    if (c->owner_thread && c->optional_preemptible
        && (c->pressed != 0 || c->analog_active)
        && !(c->forward_awaited && !c->analog_active
             && (c->pressed & ~PSP_INPUT_ROUTE_FORWARD_BUTTONS) == 0))
        return PSP_INPUT_ROUTE_YIELD;
    if (c->priority_handled) return PSP_INPUT_ROUTE_PRIORITY_DONE;
    if (c->pressed == 0) return PSP_INPUT_ROUTE_NONE;
    bool loading = c->owner == PSP_INPUT_OWNER_LOADING_PAGE;
    bool media = c->owner == PSP_INPUT_OWNER_MEDIA;
    if (loading && c->loop != PSP_INPUT_LOOP_ON_PAGE)
        return PSP_INPUT_ROUTE_FORWARD;
    if ((c->pressed & PSP_UI_BUTTON_CANCEL) != 0 && !c->cancelling)
        return PSP_INPUT_ROUTE_CANCEL;
    if (media && (c->pressed & PSP_INPUT_ROUTE_MEDIA_BUTTONS) != 0
        && !c->cancelling)
        return PSP_INPUT_ROUTE_MEDIA;
    if (loading && (c->pressed & PSP_INPUT_ROUTE_SCROLL_BUTTONS) != 0
        && !c->cancelling)
        return PSP_INPUT_ROUTE_SCROLL;
    if (c->cancelling) return PSP_INPUT_ROUTE_STILL_STOPPING;
    if (c->owner == PSP_INPUT_OWNER_PAGE_SERVICE)
        return PSP_INPUT_ROUTE_QUEUE_PAGE;
    if (loading) return PSP_INPUT_ROUTE_FORWARD;
    return c->acknowledge_busy ? PSP_INPUT_ROUTE_BUSY : PSP_INPUT_ROUTE_NONE;
}

#endif
