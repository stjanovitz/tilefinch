#ifndef TILEFINCH_PSP_MEDIA_PRESENTATION_GATE_H
#define TILEFINCH_PSP_MEDIA_PRESENTATION_GATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * What holds back the first picture and the sound after an open or a seek
 * (psp_media_session.c). Three small pieces, each replacing flags whose
 * combinations could disagree:
 *
 * The startup preroll gates initial playback. FILLING waits for the decoder's
 * ready-frame target before the head is taken; CLAIMED has taken the head and
 * waits for it to reach the LCD (the display count passing the baseline
 * recorded at begin) before Priming completes and audio starts. A claimed head
 * without a preroll could be written with the old pair of flags; it cannot be
 * here.
 *
 * The seek floor gates a committed seek. Random-access decode starts at the
 * preceding keyframe; frames before the floor are prerequisites and never
 * presented. The floor is armed or not, with its time: a seek or a backward
 * reopen to 0 has a real floor at 0 (a value doubling as the "armed" flag
 * once froze playback exactly there).
 *
 * The audio hold is what the controller wants (Priming, Seeking, Recovering,
 * a preview, a startup preroll) against what playback accepted. The two
 * differ only while playback refuses the change, and the next projection
 * retries it.
 */

typedef enum {
    PSP_MEDIA_STARTUP_NONE = 0,
    PSP_MEDIA_STARTUP_FILLING,
    PSP_MEDIA_STARTUP_CLAIMED
} PspMediaStartupPhase;

typedef struct {
    PspMediaStartupPhase phase;
    /* Displayed-frame count when the preroll began; the claimed head has
       reached the LCD once the count passes it. */
    size_t displayed_baseline;
} PspMediaStartupPreroll;

static inline bool psp_media_startup_active(const PspMediaStartupPreroll *s)
{
    return s->phase != PSP_MEDIA_STARTUP_NONE;
}

static inline bool psp_media_startup_filling(const PspMediaStartupPreroll *s)
{
    return s->phase == PSP_MEDIA_STARTUP_FILLING;
}

static inline bool psp_media_startup_claimed(const PspMediaStartupPreroll *s)
{
    return s->phase == PSP_MEDIA_STARTUP_CLAIMED;
}

/* Arms the preroll once; a second begin keeps the running one. True when
   this call armed it. */
static inline bool psp_media_startup_begin(PspMediaStartupPreroll *s,
                                           size_t displayed)
{
    if (psp_media_startup_active(s)) return false;
    s->phase = PSP_MEDIA_STARTUP_FILLING;
    s->displayed_baseline = displayed;
    return true;
}

/* The head was taken for display. */
static inline void psp_media_startup_claim(PspMediaStartupPreroll *s)
{
    if (psp_media_startup_filling(s)) s->phase = PSP_MEDIA_STARTUP_CLAIMED;
}

/* The claimed head has been shown. */
static inline bool psp_media_startup_presented(
    const PspMediaStartupPreroll *s, size_t displayed)
{
    return psp_media_startup_claimed(s) && displayed > s->displayed_baseline;
}

static inline void psp_media_startup_end(PspMediaStartupPreroll *s)
{
    s->phase = PSP_MEDIA_STARTUP_NONE;
    s->displayed_baseline = 0;
}

typedef struct {
    bool armed;
    /* Meaningful only while armed. */
    uint64_t us;
} PspMediaSeekFloor;

static inline void psp_media_seek_floor_arm(PspMediaSeekFloor *floor,
                                            uint64_t us)
{
    floor->armed = true;
    floor->us = us;
}

static inline void psp_media_seek_floor_clear(PspMediaSeekFloor *floor)
{
    floor->armed = false;
    floor->us = 0;
}

/* A frame at or after an armed floor is the first presentable one. */
static inline bool psp_media_seek_floor_reached(
    const PspMediaSeekFloor *floor, uint64_t pts_us)
{
    return floor->armed && pts_us >= floor->us;
}

typedef struct {
    /* The controller's request. */
    bool wanted;
    /* What playback accepted: audio submission is blocked now. */
    bool applied;
} PspMediaAudioHold;

/* The request differs from what playback has; the caller applies it. */
static inline bool psp_media_audio_hold_pending(const PspMediaAudioHold *hold)
{
    return hold->wanted != hold->applied;
}

#endif
