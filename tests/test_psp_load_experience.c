#include "tilefinch/psp_load_experience.h"

#include <stdio.h>

#define CHECK(condition) do {                                              \
    if (!(condition)) {                                                    \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n",                    \
                __FILE__, __LINE__, #condition);                           \
        return 1;                                                          \
    }                                                                      \
} while (0)

int main(void)
{
    /* A press resolves only on visible movement; repeated presses while one
       waits do not reset its clock. */
    PspLoadScroll scroll;
    psp_load_scroll_reset(&scroll);
    psp_load_scroll_press(&scroll, 1000u);
    psp_load_scroll_press(&scroll, 5000u);
    psp_load_scroll_moved(&scroll, 21000u);
    CHECK(scroll.presses == 2 && scroll.moved == 1
          && scroll.max_move_latency_us == 20000u
          && scroll.longest_wait_us == 20000u
          && scroll.pending_press_us == 0);
    /* A press that never finds content waits until the load settles. */
    psp_load_scroll_press(&scroll, 30000u);
    CHECK(psp_load_scroll_finish(&scroll, 2030000u)
          && scroll.longest_wait_us == 2000000u
          && scroll.max_move_latency_us == 20000u
          && !psp_load_scroll_finish(&scroll, 3000000u));
    /* 32-bit stamps wrap; the difference is still correct. */
    psp_load_scroll_reset(&scroll);
    psp_load_scroll_press(&scroll, 0xfffff000u);
    psp_load_scroll_moved(&scroll, 0x00001000u);
    CHECK(scroll.max_move_latency_us == 0x2000u);

    /* Preview reaches two screens at 4.1 s; the 350-screen page lands at
       33.5 s. Five screens are reachable only once the page lands. */
    PspLoadReach reach = {
        .first_present_us = 4000000u,
        .preview_reach_us = 4100000u,
        .preview_reach_px = 544,
        .loaded_us = 33500000u,
        .page_height_px = 94752,
        .screen_height_px = 272
    };
    CHECK(psp_load_reach_time(&reach, 1) == 4000000u);
    CHECK(psp_load_reach_time(&reach, 2) == 4100000u);
    CHECK(psp_load_reach_time(&reach, 5) == 33500000u);
    CHECK(psp_load_reach_time(&reach, 400) == UINT64_MAX);
    CHECK(psp_load_reach_screens_at(&reach, 1000000u) == 0);
    CHECK(psp_load_reach_screens_at(&reach, 4050000u) == 1);
    CHECK(psp_load_reach_screens_at(&reach, 10000000u) == 2);
    CHECK(psp_load_reach_screens_at(&reach, 40000000u) == 348);
    /* No preview: nothing is reachable before the page lands. */
    PspLoadReach no_preview = {
        .loaded_us = 5000000u, .page_height_px = 1000,
        .screen_height_px = 272
    };
    CHECK(psp_load_reach_time(&no_preview, 1) == 5000000u);
    CHECK(psp_load_reach_screens_at(&no_preview, 4000000u) == 0);
    puts("psp-load-experience-tests status=PASS");
    return 0;
}
