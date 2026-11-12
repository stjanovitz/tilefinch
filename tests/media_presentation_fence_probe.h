/* A stand-in for the PSP presentation fence (AU-05) around a media
 * presentation. The loading supervisor draws a copy of the player state
 * whose presentation pointer still borrows the session's storage, so its
 * strings, arrays, the counts that index them and the rotation are stored
 * under the fence. The probe digests those fields at every fence call and
 * counts the changes made while the fence was not held, and the budget
 * allocations (text shaping) made while it was held. Test-only.
 */
#ifndef TILEFINCH_TEST_MEDIA_PRESENTATION_FENCE_PROBE_H
#define TILEFINCH_TEST_MEDIA_PRESENTATION_FENCE_PROBE_H

#include "tilefinch/budget.h"
#include "tilefinch/psp_ui.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static struct {
    const PspUiMediaPresentation *presentation;
    const Budget *budget;
    uint32_t seen;
    size_t allocations_at_acquire;
    bool held;
    unsigned acquires;
    unsigned unfenced;
    /* Shaping (a budget allocation) done while the fence was held. */
    unsigned held_across_work;
} media_fence_probe;

static uint32_t media_fence_probe_mix(uint32_t hash, const void *data,
                                      size_t length)
{
    const unsigned char *bytes = data;
    for (size_t at = 0; at < length; at++) {
        hash ^= bytes[at];
        hash *= UINT32_C(16777619);
    }
    return hash;
}

static uint32_t media_fence_probe_text(uint32_t hash, const char *text,
                                       size_t capacity)
{
    const char *end = memchr(text, '\0', capacity);
    return media_fence_probe_mix(hash, text,
                                 end == NULL ? capacity : (size_t) (end - text));
}

#define MEDIA_FENCE_PROBE_FIELD(hash, p, field) \
    media_fence_probe_mix((hash), &(p)->field, sizeof((p)->field))

/* What a copy drawn on the other thread reads through the pointer. */
static uint32_t media_fence_probe_digest(const PspUiMediaPresentation *p)
{
    uint32_t hash = UINT32_C(2166136261);
    hash = MEDIA_FENCE_PROBE_FIELD(hash, p, audio_track_count);
    hash = MEDIA_FENCE_PROBE_FIELD(hash, p, subtitle_track_count);
    for (size_t at = 0; at < PSP_UI_MEDIA_TRACK_LIMIT; at++) {
        hash = media_fence_probe_text(hash, p->audio_tracks[at].label,
                                      sizeof(p->audio_tracks[at].label));
        hash = media_fence_probe_text(hash, p->subtitle_tracks[at].label,
                                      sizeof(p->subtitle_tracks[at].label));
    }
    hash = MEDIA_FENCE_PROBE_FIELD(hash, p, chapter_count);
    for (size_t at = 0; at < MEDIA_CHAPTER_LIMIT; at++) {
        hash = MEDIA_FENCE_PROBE_FIELD(hash, p, chapters[at].seconds);
        hash = media_fence_probe_text(hash, p->chapters[at].title,
                                      sizeof(p->chapters[at].title));
    }
    hash = media_fence_probe_text(hash, p->subtitle_text,
                                  sizeof(p->subtitle_text));
    hash = MEDIA_FENCE_PROBE_FIELD(hash, p, subtitle_right_to_left);
    hash = MEDIA_FENCE_PROBE_FIELD(hash, p, clockwise);
    hash = MEDIA_FENCE_PROBE_FIELD(hash, p, portrait_crop);
    hash = MEDIA_FENCE_PROBE_FIELD(hash, p, queue_active);
    hash = MEDIA_FENCE_PROBE_FIELD(hash, p, queue_loading);
    hash = MEDIA_FENCE_PROBE_FIELD(hash, p, queue_failed);
    hash = MEDIA_FENCE_PROBE_FIELD(hash, p, queue_next);
    hash = MEDIA_FENCE_PROBE_FIELD(hash, p, queue_retry);
    hash = MEDIA_FENCE_PROBE_FIELD(hash, p, queue_terminal);
    hash = MEDIA_FENCE_PROBE_FIELD(hash, p, queue_count);
    hash = MEDIA_FENCE_PROBE_FIELD(hash, p, queue_position);
    for (size_t at = 0; at < 6u; at++)
        hash = media_fence_probe_text(hash, p->queue_titles[at],
                                      sizeof(p->queue_titles[at]));
    hash = media_fence_probe_text(hash, p->queue_error,
                                  sizeof(p->queue_error));
    hash = media_fence_probe_text(hash, p->shown_title,
                                  sizeof(p->shown_title));
    hash = media_fence_probe_text(hash, p->audio_detail,
                                  sizeof(p->audio_detail));
    hash = media_fence_probe_text(hash, p->audio_source,
                                  sizeof(p->audio_source));
    hash = MEDIA_FENCE_PROBE_FIELD(hash, p, audio_unencrypted);
    return hash;
}

#undef MEDIA_FENCE_PROBE_FIELD

/* A change since the last look counts as unfenced unless the fence is held
   now; either way the current fields become the baseline. */
static void media_fence_probe_observe(void)
{
    uint32_t now = media_fence_probe_digest(media_fence_probe.presentation);
    if (!media_fence_probe.held && now != media_fence_probe.seen)
        media_fence_probe.unfenced++;
    media_fence_probe.seen = now;
}

static bool media_fence_probe_acquire(void)
{
    media_fence_probe_observe();
    media_fence_probe.held = true;
    media_fence_probe.acquires++;
    media_fence_probe.allocations_at_acquire =
        media_fence_probe.budget == NULL
            ? 0u : media_fence_probe.budget->allocation_count;
    return true;
}

static void media_fence_probe_release(bool held)
{
    if (!held || !media_fence_probe.held) media_fence_probe.unfenced++;
    if (media_fence_probe.budget != NULL
        && media_fence_probe.budget->allocation_count
            != media_fence_probe.allocations_at_acquire)
        media_fence_probe.held_across_work++;
    media_fence_probe.seen =
        media_fence_probe_digest(media_fence_probe.presentation);
    media_fence_probe.held = false;
}

/* Call after psp_ui_media_bind_presentation, which clears the fence. */
static void media_fence_probe_install(PspUiMediaPresentation *presentation,
                                      const Budget *budget)
{
    memset(&media_fence_probe, 0, sizeof(media_fence_probe));
    media_fence_probe.presentation = presentation;
    media_fence_probe.budget = budget;
    media_fence_probe.seen = media_fence_probe_digest(presentation);
    presentation->fence.acquire = media_fence_probe_acquire;
    presentation->fence.release = media_fence_probe_release;
}

/* After an operation: every drawn change since the last check was made
   under the fence, no shaping ran under it, and it is not left held. */
static bool media_fence_probe_clean(void)
{
    media_fence_probe_observe();
    return media_fence_probe.unfenced == 0 && !media_fence_probe.held
        && media_fence_probe.held_across_work == 0;
}

#endif
