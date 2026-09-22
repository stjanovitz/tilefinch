#ifndef TILEFINCH_FRAME_PUMPS_H
#define TILEFINCH_FRAME_PUMPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Optional browser-thread work is admitted through one declared policy
 * table rather than through a private predicate at each call site.
 *
 * The resident frame loop is single-threaded, so two pumps never race. They
 * do contend: for the frame's latency allowance and for the Memory Stick.
 * (Transport descriptors are arbitrated by the worker's own slot admission.)
 * Every pump therefore states
 *
 *   - which frame facts refuse it,
 *   - which other pumps it yields to (while they merely have work, or only
 *     once they consumed a slice in this frame), and
 *   - which contended resources one of its slices claims.
 *
 * Readiness ("do I have work?") stays with the pump's owner. Admission
 * ("may I run given everything else?") is answered only here. The host test
 * enumerates every fact and workload combination and pins the exact set of
 * pump pairs able to claim the same resource in one frame, so adding a pump
 * forces a stated relationship with the existing ones.
 *
 * Identifiers are in resident-loop order. A pump is admitted at its fixed
 * position in the frame. Every call site both asks for admission and records
 * the slice it then consumed; out-of-order admissions and slices recorded
 * without an admission are counted, and the counts are published at exit.
 */
typedef enum {
    FRAME_PUMP_PRECONNECT = 0,
    FRAME_PUMP_UPDATE_SESSION,
    FRAME_PUMP_SCREENSHOT,
    FRAME_PUMP_OFFLINE_DOWNLOAD,
    FRAME_PUMP_SITE_RESTORE_STORAGE,
    FRAME_PUMP_SITE_RESTORE_CACHE,
    FRAME_PUMP_PROVIDER_PRERESOLVE,
    FRAME_PUMP_PAGE_IDLE,
    FRAME_PUMP_DEFERRED_IMAGE,
    FRAME_PUMP_BASELINE_FONTS,
    FRAME_PUMP_COUNT
} FramePumpId;

#define FRAME_PUMP_BIT(id) ((uint16_t) (1u << (unsigned) (id)))

/* Facts are sampled by the frontend immediately before an admission. */
enum {
    FRAME_FACT_INPUT_BUTTONS       = 1u << 0,  /* pad held or pressed */
    FRAME_FACT_INPUT_INTENT        = 1u << 1,  /* action, pointer or scroll */
    FRAME_FACT_PAGE_WORK_PAUSED    = 1u << 2,
    FRAME_FACT_PAGE_DIRTY          = 1u << 3,
    FRAME_FACT_RENDER_JOB          = 1u << 4,
    FRAME_FACT_NAVIGATION_PENDING  = 1u << 5,
    FRAME_FACT_MEDIA_VISIBLE       = 1u << 6,
    FRAME_FACT_MEDIA_OPEN          = 1u << 7,
    FRAME_FACT_MEDIA_DECODE        = 1u << 8,
    FRAME_FACT_MEDIA_PLAYBACK      = 1u << 9,  /* a pipeline is resident */
    FRAME_FACT_COOPERATE_ACTIVE    = 1u << 10, /* a supervised scope is open */
    FRAME_FACT_NETWORK_WARMING     = 1u << 11, /* association in progress */
    /* A preconnect handshake is in flight and association has finished. */
    FRAME_FACT_PRECONNECT_CONTENDS = 1u << 12,
    FRAME_FACT_ALL                 = (1u << 13) - 1u
};

/* Contended resources claimed by one slice of a pump. */
enum {
    FRAME_RESOURCE_MEMORY_STICK = 1u << 0,
    /* A slice long enough to need its own share of the frame allowance. */
    FRAME_RESOURCE_IDLE_SLICE   = 1u << 1
};

typedef struct {
    const char *name;
    uint32_t blocked_by;       /* FRAME_FACT_* */
    uint16_t yields_to_active; /* pumps which currently have work */
    uint16_t yields_to_ran;    /* pumps which consumed a slice this frame */
    uint8_t resources;         /* FRAME_RESOURCE_* */
} FramePumpPolicy;

typedef struct {
    uint16_t admitted;  /* pumps the policy let through this frame */
    uint16_t ran;       /* pumps which then consumed a slice */
    uint8_t next_position;  /* lowest identifier still admissible in order */
    /* These accumulate across frames so an exit report can publish them. */
    uint32_t order_violations;   /* admitted behind a later pump */
    uint32_t wiring_violations;  /* a slice recorded without an admission */
    uint32_t admissions[FRAME_PUMP_COUNT];
    uint32_t slices[FRAME_PUMP_COUNT];
    /* Refusals owed to another pump (yields_to_active or yields_to_ran), as
       opposed to a frame fact: the evidence that a declared overlap decision
       actually arbitrated something in a run. */
    uint32_t yields[FRAME_PUMP_COUNT];
} FramePumpFrame;

const FramePumpPolicy *frame_pump_policy(FramePumpId id);

/* Start a frame: forget this frame's admissions and consumed slices. */
static inline void frame_pumps_begin(FramePumpFrame *frame)
{
    frame->admitted = 0;
    frame->ran = 0;
    frame->next_position = 0;
}

/*
 * Ask the policy whether this pump may take a slice now.
 *
 * `facts` and `active` are what the caller sampled immediately before the
 * call. Only the bits the pump's own policy names are consulted, so a caller
 * may sample just those (frame_pump_policy(id)->blocked_by and
 * ->yields_to_active) and may stop at the first refusing one.
 */
bool frame_pumps_admit(FramePumpFrame *frame, FramePumpId id,
                       uint32_t facts, uint16_t active);

/* Record that an admitted pump actually consumed its slice. Recording one
   that was not admitted this frame is a wiring defect and is counted. */
void frame_pumps_mark_ran(FramePumpFrame *frame, FramePumpId id);

/* Structural validation of the table itself; false names the first defect. */
bool frame_pumps_policy_valid(const char **defect);

#endif
