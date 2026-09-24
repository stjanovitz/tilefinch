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

    puts("psp-input-route-tests status=PASS");
    return 0;
}
