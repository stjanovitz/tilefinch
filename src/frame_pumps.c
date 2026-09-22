#include "tilefinch/frame_pumps.h"

#define PUMP(id) FRAME_PUMP_BIT(FRAME_PUMP_##id)

/* Optional page work never competes with the surfaces the user is looking
   at or acting on. */
#define FACTS_PAGE_BUSY \
    (FRAME_FACT_PAGE_WORK_PAUSED | FRAME_FACT_PAGE_DIRTY \
     | FRAME_FACT_RENDER_JOB)
#define FACTS_MEDIA_PIPELINE \
    (FRAME_FACT_MEDIA_OPEN | FRAME_FACT_MEDIA_DECODE \
     | FRAME_FACT_MEDIA_PLAYBACK)
#define FACTS_SITE_RESTORE \
    (FRAME_FACT_INPUT_BUTTONS | FACTS_PAGE_BUSY \
     | FRAME_FACT_NAVIGATION_PENDING | FRAME_FACT_MEDIA_VISIBLE \
     | FRAME_FACT_MEDIA_OPEN)
/* Memory Stick writers whose completion the user is waiting on. */
#define PUMPS_FOREGROUND_STORAGE \
    (PUMP(UPDATE_SESSION) | PUMP(SCREENSHOT) | PUMP(OFFLINE_DOWNLOAD))
#define PUMPS_SITE_RESTORE \
    (PUMP(SITE_RESTORE_STORAGE) | PUMP(SITE_RESTORE_CACHE))

static const FramePumpPolicy frame_pump_policies[FRAME_PUMP_COUNT] = {
    /* One bodyless warm-up for the highlighted HOME provider. Its own dwell
       machine owns eligibility; one slice is a non-blocking multi poll. */
    [FRAME_PUMP_PRECONNECT] = {
        .name = "preconnect",
    },
    /* User-started install. It owns the modal update surface. */
    [FRAME_PUMP_UPDATE_SESSION] = {
        .name = "update-session",
        .resources = FRAME_RESOURCE_MEMORY_STICK,
    },
    /* User-started capture: four PNG rows per frame until it is written. */
    [FRAME_PUMP_SCREENSHOT] = {
        .name = "screenshot",
        .resources = FRAME_RESOURCE_MEMORY_STICK,
    },
    /* Saving a video is mutually exclusive with playback on this memory
       budget, and it runs inside its own supervised scope. */
    [FRAME_PUMP_OFFLINE_DOWNLOAD] = {
        .name = "offline-download",
        .blocked_by = FRAME_FACT_NAVIGATION_PENDING | FACTS_MEDIA_PIPELINE
            | FRAME_FACT_COOPERATE_ACTIVE,
        .resources = FRAME_RESOURCE_MEMORY_STICK | FRAME_RESOURCE_IDLE_SLICE,
    },
    /* localStorage must finish before the first real commit, so only the
       user's own activity and foreground storage writers defer it. */
    [FRAME_PUMP_SITE_RESTORE_STORAGE] = {
        .name = "site-restore-storage",
        .blocked_by = FACTS_SITE_RESTORE,
        /* Also after the slice in which such a writer finished: the frame
           already paid for one Memory Stick operation. */
        .yields_to_active = PUMPS_FOREGROUND_STORAGE,
        .yields_to_ran = PUMPS_FOREGROUND_STORAGE,
        .resources = FRAME_RESOURCE_MEMORY_STICK | FRAME_RESOURCE_IDLE_SLICE,
    },
    /* Stale cache bodies are expendable: their Memory Stick reads also stay
       out of the latency-sensitive association ladder. */
    [FRAME_PUMP_SITE_RESTORE_CACHE] = {
        .name = "site-restore-cache",
        .blocked_by = FACTS_SITE_RESTORE | FRAME_FACT_NETWORK_WARMING,
        /* The restore phase machine starts cache records only after the
           storage records have been published. */
        /* ...and only after the baseline faces are resident: those are
           required before the first page measures text, while a cache body
           is an optimization. */
        .yields_to_active = PUMPS_FOREGROUND_STORAGE
            | PUMP(SITE_RESTORE_STORAGE) | PUMP(BASELINE_FONTS),
        .yields_to_ran = PUMPS_FOREGROUND_STORAGE
            | PUMP(SITE_RESTORE_STORAGE),
        .resources = FRAME_RESOURCE_MEMORY_STICK | FRAME_RESOURCE_IDLE_SLICE,
    },
    /* Speculative resolution of the focused provider result. It gets the
       first browser-thread slice after restoration. Transport contention is
       not a frame resource: the worker's own slot admission arbitrates it. */
    [FRAME_PUMP_PROVIDER_PRERESOLVE] = {
        .name = "provider-preresolve",
        .blocked_by = FRAME_FACT_INPUT_BUTTONS | FRAME_FACT_INPUT_INTENT
            | FRAME_FACT_PAGE_DIRTY | FRAME_FACT_RENDER_JOB
            | FRAME_FACT_NAVIGATION_PENDING | FRAME_FACT_MEDIA_VISIBLE
            | FACTS_MEDIA_PIPELINE | FRAME_FACT_COOPERATE_ACTIVE,
        .yields_to_active = PUMP(OFFLINE_DOWNLOAD),
        .yields_to_ran = PUMPS_SITE_RESTORE | PUMP(OFFLINE_DOWNLOAD),
        .resources = FRAME_RESOURCE_IDLE_SLICE,
    },
    /* Post-commit fonts and offscreen images, one bounded slice at a time. */
    [FRAME_PUMP_PAGE_IDLE] = {
        .name = "page-idle",
        .blocked_by = FRAME_FACT_INPUT_BUTTONS | FRAME_FACT_INPUT_INTENT
            | FACTS_PAGE_BUSY | FRAME_FACT_MEDIA_VISIBLE
            | FRAME_FACT_MEDIA_OPEN | FRAME_FACT_MEDIA_DECODE,
        /* One long slice per frame: a download's supervised slice has
           already spent this frame's idle allowance. */
        .yields_to_ran = PUMPS_SITE_RESTORE | PUMP(OFFLINE_DOWNLOAD),
        .resources = FRAME_RESOURCE_IDLE_SLICE,
    },
    /* Focus and scroll repaint first. After that presentation, one
       image-only continuation unit may run if nothing else took the frame's
       idle allowance. */
    [FRAME_PUMP_DEFERRED_IMAGE] = {
        .name = "deferred-image",
        .blocked_by = FRAME_FACT_INPUT_BUTTONS | FRAME_FACT_INPUT_INTENT
            | FRAME_FACT_PAGE_WORK_PAUSED | FRAME_FACT_RENDER_JOB
            | FRAME_FACT_NAVIGATION_PENDING | FRAME_FACT_MEDIA_VISIBLE
            | FACTS_MEDIA_PIPELINE | FRAME_FACT_COOPERATE_ACTIVE,
        .yields_to_active = PUMP(OFFLINE_DOWNLOAD),
        .yields_to_ran = PUMPS_SITE_RESTORE | PUMP(OFFLINE_DOWNLOAD)
            | PUMP(PAGE_IDLE),
        .resources = FRAME_RESOURCE_IDLE_SLICE,
    },
    /* The metric face loads during association wait frames; afterwards the
       selected-site handshake takes precedence over a font read. A frame
       performs one Memory Stick operation: the read waits out a frame in
       which a user-started writer or the (equally required, but smaller)
       storage restoration already took it. */
    [FRAME_PUMP_BASELINE_FONTS] = {
        .name = "baseline-fonts",
        .blocked_by = FRAME_FACT_PRECONNECT_CONTENDS,
        .yields_to_ran = PUMPS_FOREGROUND_STORAGE | PUMPS_SITE_RESTORE,
        .resources = FRAME_RESOURCE_MEMORY_STICK,
    },
};

const FramePumpPolicy *frame_pump_policy(FramePumpId id)
{
    return (unsigned) id < FRAME_PUMP_COUNT ? &frame_pump_policies[id] : NULL;
}

bool frame_pumps_admit(FramePumpFrame *frame, FramePumpId id,
                       uint32_t facts, uint16_t active)
{
    if (frame == NULL || (unsigned) id >= FRAME_PUMP_COUNT) return false;
    const FramePumpPolicy *policy = &frame_pump_policies[id];
    /* The table is in loop order. A pump admitted behind a later one would
       see yields_to_ran bits its declaration never considered. */
    if ((unsigned) id < frame->next_position) frame->order_violations++;
    else frame->next_position = (uint8_t) id;
    if ((facts & policy->blocked_by) != 0) return false;
    if ((active & policy->yields_to_active) != 0
        || (frame->ran & policy->yields_to_ran) != 0) {
        frame->yields[id]++;
        return false;
    }
    frame->admitted |= FRAME_PUMP_BIT(id);
    frame->admissions[id]++;
    return true;
}

void frame_pumps_mark_ran(FramePumpFrame *frame, FramePumpId id)
{
    if (frame == NULL || (unsigned) id >= FRAME_PUMP_COUNT) return;
    if ((frame->admitted & FRAME_PUMP_BIT(id)) == 0)
        frame->wiring_violations++;
    frame->ran |= FRAME_PUMP_BIT(id);
    frame->slices[id]++;
}

bool frame_pumps_policy_valid(const char **defect)
{
    const char *unused = NULL;
    if (defect == NULL) defect = &unused;
    for (unsigned id = 0; id < FRAME_PUMP_COUNT; id++) {
        const FramePumpPolicy *policy = &frame_pump_policies[id];
        uint16_t self = FRAME_PUMP_BIT(id);
        uint16_t earlier = (uint16_t) (self - 1u);
        if (policy->name == NULL || policy->name[0] == '\0') {
            *defect = "unnamed pump";
            return false;
        }
        if ((policy->blocked_by & ~(uint32_t) FRAME_FACT_ALL) != 0) {
            *defect = "unknown frame fact";
            return false;
        }
        if (((policy->yields_to_active | policy->yields_to_ran)
             >> FRAME_PUMP_COUNT) != 0
            || ((policy->yields_to_active | policy->yields_to_ran) & self)
                   != 0) {
            *defect = "pump yields to itself or to an unknown pump";
            return false;
        }
        /* Only an earlier pump can already have consumed a slice. */
        if ((policy->yields_to_ran & ~earlier) != 0) {
            *defect = "pump yields to a slice that cannot have run yet";
            return false;
        }
    }
    return true;
}
