#include "tilefinch/psp_input_route.h"

#include <stdio.h>

#define CHECK(condition) do {                                              \
    if (!(condition)) {                                                    \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n",                    \
                __FILE__, __LINE__, #condition);                           \
        return 1;                                                          \
    }                                                                      \
} while (0)

static PspInputRoute route(PspInputOwner owner, PspInputLoopState loop,
                           uint32_t pressed, bool cancelling)
{
    PspInputRouteContext context = {
        .owner = owner, .loop = loop, .pressed = pressed,
        .cancelling = cancelling, .acknowledge_busy = true
    };
    return psp_input_route(&context);
}

int main(void)
{
    const PspInputOwner loading = PSP_INPUT_OWNER_LOADING_PAGE;
    const PspInputOwner service = PSP_INPUT_OWNER_PAGE_SERVICE;
    const PspInputOwner media = PSP_INPUT_OWNER_MEDIA;
    const PspInputLoopState on_page = PSP_INPUT_LOOP_ON_PAGE;
    const PspInputLoopState in_menu = PSP_INPUT_LOOP_IN_MENU;
    const PspInputLoopState behind = PSP_INPUT_LOOP_BEHIND;

    /* A captured game's buttons must never become raster cancellation,
       page scrolling, optional-work preemption, or a BUSY notification. */
    PspInputRouteContext game = {
        .owner = service, .loop = on_page,
        .page_gamepad_capture = true, .owner_thread = true,
        .optional_preemptible = true, .acknowledge_busy = true,
        .analog_active = true,
        .pressed = PSP_UI_BUTTON_CANCEL | PSP_UI_BUTTON_CONFIRM
            | PSP_UI_BUTTON_TOOLBAR | PSP_UI_BUTTON_PAGE_DOWN
    };
    CHECK(psp_input_route(&game) == PSP_INPUT_ROUTE_NONE);
    game.owner_thread = false;
    CHECK(psp_input_route(&game) == PSP_INPUT_ROUTE_NONE);
    game.owner = loading;
    CHECK(psp_input_route(&game) == PSP_INPUT_ROUTE_NONE);
    game.owner = media; game.optional_preemptible = false;
    CHECK(psp_input_route(&game) == PSP_INPUT_ROUTE_CANCEL);

    /* A loading page on screen: the supervisor pages the preview and stops
       the load; focus, activation and the menu go to the browser loop. */
    CHECK(route(loading, on_page, PSP_UI_BUTTON_PAGE_DOWN, false)
          == PSP_INPUT_ROUTE_SCROLL);
    CHECK(route(loading, on_page, PSP_UI_BUTTON_UP, false)
          == PSP_INPUT_ROUTE_SCROLL);
    CHECK(route(loading, on_page, PSP_UI_BUTTON_CANCEL, false)
          == PSP_INPUT_ROUTE_CANCEL);
    CHECK(route(loading, on_page, PSP_UI_BUTTON_RIGHT, false)
          == PSP_INPUT_ROUTE_FORWARD);
    CHECK(route(loading, on_page, PSP_UI_BUTTON_CONFIRM, false)
          == PSP_INPUT_ROUTE_FORWARD);
    CHECK(route(loading, on_page, PSP_UI_BUTTON_MENU, false)
          == PSP_INPUT_ROUTE_FORWARD);
    /* The loop shows a menu, or has presses it has not taken: every press
       keeps its order, Circle and paging included. */
    CHECK(route(loading, in_menu, PSP_UI_BUTTON_CANCEL, false)
          == PSP_INPUT_ROUTE_FORWARD);
    CHECK(route(loading, in_menu, PSP_UI_BUTTON_DOWN, false)
          == PSP_INPUT_ROUTE_FORWARD);
    CHECK(route(loading, behind, PSP_UI_BUTTON_CANCEL, false)
          == PSP_INPUT_ROUTE_FORWARD);
    CHECK(route(loading, behind, PSP_UI_BUTTON_PAGE_DOWN, false)
          == PSP_INPUT_ROUTE_FORWARD);
    /* Stopping: only the reassurance. */
    CHECK(route(loading, on_page, PSP_UI_BUTTON_PAGE_DOWN, true)
          == PSP_INPUT_ROUTE_STILL_STOPPING);
    CHECK(route(loading, on_page, PSP_UI_BUTTON_CANCEL, true)
          == PSP_INPUT_ROUTE_STILL_STOPPING);

    /* A page service: Circle stops it, the rest waits for the page. */
    CHECK(route(service, on_page, PSP_UI_BUTTON_CANCEL, false)
          == PSP_INPUT_ROUTE_CANCEL);
    CHECK(route(service, on_page, PSP_UI_BUTTON_PAGE_DOWN, false)
          == PSP_INPUT_ROUTE_QUEUE_PAGE);
    CHECK(route(service, on_page, PSP_UI_BUTTON_CONFIRM, false)
          == PSP_INPUT_ROUTE_QUEUE_PAGE);

    /* The player: its controls, Circle stops it, others are "busy". */
    CHECK(route(media, on_page, PSP_UI_BUTTON_RIGHT, false)
          == PSP_INPUT_ROUTE_MEDIA);
    CHECK(route(media, on_page, PSP_UI_BUTTON_CANCEL, false)
          == PSP_INPUT_ROUTE_CANCEL);
    CHECK(route(media, on_page, PSP_UI_BUTTON_MENU, false)
          == PSP_INPUT_ROUTE_BUSY);
    CHECK(route(media, on_page, PSP_UI_BUTTON_RIGHT, true)
          == PSP_INPUT_ROUTE_STILL_STOPPING);
    /* A highlighted scrub time: Circle drops it instead of stopping the
       video, unless a stop is already under way. */
    PspInputRouteContext highlighted = {
        .owner = media, .loop = on_page, .pressed = PSP_UI_BUTTON_CANCEL,
        .acknowledge_busy = true, .media_preview_active = true
    };
    CHECK(psp_input_route(&highlighted) == PSP_INPUT_ROUTE_MEDIA);
    highlighted.cancelling = true;
    CHECK(psp_input_route(&highlighted) == PSP_INPUT_ROUTE_STILL_STOPPING);
    /* The flag means nothing to a loading page. */
    highlighted.owner = loading;
    highlighted.cancelling = false;
    CHECK(psp_input_route(&highlighted) == PSP_INPUT_ROUTE_CANCEL);

    /* Nothing pressed: nothing to do. */
    CHECK(route(loading, on_page, 0, false) == PSP_INPUT_ROUTE_NONE);

    /* Optional preemptible work yields to any input, even the stick; a
       press a native menu already took is done. */
    PspInputRouteContext context = {
        .owner = service, .owner_thread = true,
        .optional_preemptible = true, .analog_active = true
    };
    CHECK(psp_input_route(&context) == PSP_INPUT_ROUTE_YIELD);
    context.optional_preemptible = false;
    context.priority_handled = true;
    context.pressed = PSP_UI_BUTTON_MENU;
    CHECK(psp_input_route(&context) == PSP_INPUT_ROUTE_PRIORITY_DONE);
    /* Off the browser thread, optional work is not interrupted this way. */
    context.owner_thread = false;
    context.optional_preemptible = true;
    context.priority_handled = false;
    CHECK(psp_input_route(&context) == PSP_INPUT_ROUTE_QUEUE_PAGE);

    /* The rest of the layout the reader waits for at the bottom: pressing
       on down queues for it (cancelling would restart it every press and
       the page would never grow); anything else still preempts it. */
    PspInputRouteContext awaited = {
        .owner = service, .owner_thread = true,
        .optional_preemptible = true, .forward_awaited = true,
        .pressed = PSP_UI_BUTTON_PAGE_DOWN
    };
    CHECK(psp_input_route(&awaited) == PSP_INPUT_ROUTE_QUEUE_PAGE);
    awaited.pressed = PSP_UI_BUTTON_DOWN;
    CHECK(psp_input_route(&awaited) == PSP_INPUT_ROUTE_QUEUE_PAGE);
    awaited.pressed = PSP_UI_BUTTON_PAGE_UP;
    CHECK(psp_input_route(&awaited) == PSP_INPUT_ROUTE_YIELD);
    awaited.pressed = PSP_UI_BUTTON_CANCEL;
    CHECK(psp_input_route(&awaited) == PSP_INPUT_ROUTE_YIELD);
    awaited.pressed = PSP_UI_BUTTON_PAGE_DOWN | PSP_UI_BUTTON_MENU;
    CHECK(psp_input_route(&awaited) == PSP_INPUT_ROUTE_YIELD);
    awaited.pressed = PSP_UI_BUTTON_PAGE_DOWN;
    awaited.analog_active = true;
    CHECK(psp_input_route(&awaited) == PSP_INPUT_ROUTE_YIELD);
    /* Not waiting at the bottom: the same press preempts as before. */
    awaited.analog_active = false;
    awaited.forward_awaited = false;
    CHECK(psp_input_route(&awaited) == PSP_INPUT_ROUTE_YIELD);

    /* The rest of a provisional layout that lets the supervisor page the
       committed page: page presses scroll it and the layout keeps going;
       anything else (a link, the menu, the stick, a chord) still stops it. */
    PspInputRouteContext served = {
        .owner = service, .owner_thread = true,
        .optional_preemptible = true, .scroll_served = true,
        .pressed = PSP_UI_BUTTON_PAGE_DOWN
    };
    CHECK(psp_input_route(&served) == PSP_INPUT_ROUTE_SCROLL);
    served.pressed = PSP_UI_BUTTON_UP;
    CHECK(psp_input_route(&served) == PSP_INPUT_ROUTE_SCROLL);
    served.pressed = PSP_UI_BUTTON_CONFIRM;
    CHECK(psp_input_route(&served) == PSP_INPUT_ROUTE_YIELD);
    served.pressed = PSP_UI_BUTTON_PAGE_DOWN | PSP_UI_BUTTON_MENU;
    CHECK(psp_input_route(&served) == PSP_INPUT_ROUTE_YIELD);
    served.pressed = PSP_UI_BUTTON_DOWN;
    served.analog_active = true;
    CHECK(psp_input_route(&served) == PSP_INPUT_ROUTE_YIELD);
    /* Waiting at the bottom for that layout, down still queues for the
       page; up is served. */
    served.analog_active = false;
    served.forward_awaited = true;
    CHECK(psp_input_route(&served) == PSP_INPUT_ROUTE_QUEUE_PAGE);
    served.pressed = PSP_UI_BUTTON_PAGE_UP;
    CHECK(psp_input_route(&served) == PSP_INPUT_ROUTE_SCROLL);
    /* A provisional relayout's build is kept, not optional: page presses
       are served at its checkpoints and anything else waits for the page. */
    PspInputRouteContext relayout = {
        .owner = service, .owner_thread = true, .scroll_served = true,
        .pressed = PSP_UI_BUTTON_PAGE_DOWN
    };
    CHECK(psp_input_route(&relayout) == PSP_INPUT_ROUTE_SCROLL);
    relayout.pressed = PSP_UI_BUTTON_CONFIRM;
    CHECK(psp_input_route(&relayout) == PSP_INPUT_ROUTE_QUEUE_PAGE);
    relayout.pressed = PSP_UI_BUTTON_DOWN;
    relayout.analog_active = true;
    CHECK(psp_input_route(&relayout) == PSP_INPUT_ROUTE_QUEUE_PAGE);
    /* Off the browser thread nothing is served or preempted this way. */
    served.owner_thread = false;
    served.forward_awaited = false;
    served.scroll_served = false;
    CHECK(psp_input_route(&served) == PSP_INPUT_ROUTE_QUEUE_PAGE);

    puts("psp-input-route-tests status=PASS");
    return 0;
}
