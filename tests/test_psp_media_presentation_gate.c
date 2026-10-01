#include "tilefinch/psp_media_presentation_gate.h"

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
    /* Startup preroll: FILLING until the head is taken, CLAIMED until that
       head reaches the LCD. */
    PspMediaStartupPreroll startup = {0};
    CHECK(!psp_media_startup_active(&startup));
    CHECK(!psp_media_startup_presented(&startup, 100));
    /* A claim with no preroll does nothing: the old flag pair could record a
       claimed head outside any preroll. */
    psp_media_startup_claim(&startup);
    CHECK(!psp_media_startup_active(&startup));

    CHECK(psp_media_startup_begin(&startup, 7));
    CHECK(psp_media_startup_filling(&startup));
    CHECK(!psp_media_startup_presented(&startup, 8));
    /* A second begin keeps the running preroll and its baseline. */
    CHECK(!psp_media_startup_begin(&startup, 20));
    CHECK(startup.displayed_baseline == 7);

    psp_media_startup_claim(&startup);
    CHECK(psp_media_startup_claimed(&startup));
    CHECK(!psp_media_startup_filling(&startup));
    CHECK(!psp_media_startup_presented(&startup, 7));
    CHECK(psp_media_startup_presented(&startup, 8));
    /* Claiming again (a later frame) stays claimed. */
    psp_media_startup_claim(&startup);
    CHECK(psp_media_startup_claimed(&startup));

    psp_media_startup_end(&startup);
    CHECK(!psp_media_startup_active(&startup));
    CHECK(startup.displayed_baseline == 0);
    CHECK(psp_media_startup_begin(&startup, 3));
    CHECK(psp_media_startup_filling(&startup));

    /* Seek floor: armed at 0 is a real floor. */
    PspMediaSeekFloor floor = {0};
    CHECK(!psp_media_seek_floor_reached(&floor, 0));
    psp_media_seek_floor_arm(&floor, 0);
    CHECK(floor.armed);
    CHECK(psp_media_seek_floor_reached(&floor, 0));
    psp_media_seek_floor_arm(&floor, 66776015u);
    CHECK(!psp_media_seek_floor_reached(&floor, 61895166u));
    CHECK(psp_media_seek_floor_reached(&floor, 66776015u));
    CHECK(psp_media_seek_floor_reached(&floor, 66800000u));
    psp_media_seek_floor_clear(&floor);
    CHECK(!floor.armed && floor.us == 0);
    CHECK(!psp_media_seek_floor_reached(&floor, 99999999u));

    /* Audio hold: pending exactly while playback has not taken the
       request. */
    PspMediaAudioHold hold = {0};
    CHECK(!psp_media_audio_hold_pending(&hold));
    hold.wanted = true;
    CHECK(psp_media_audio_hold_pending(&hold));
    hold.applied = true;
    CHECK(!psp_media_audio_hold_pending(&hold));
    hold.wanted = false;
    CHECK(psp_media_audio_hold_pending(&hold));

    printf("tilefinch-psp-media-presentation-gate: outcome=pass\n");
    return 0;
}
