#ifndef TILEFINCH_PSP_LIFECYCLE_H
#define TILEFINCH_PSP_LIFECYCLE_H

#include <stdbool.h>
#include <stdatomic.h>

typedef enum {
    PSP_LIFECYCLE_RUNNING = 0,
    PSP_LIFECYCLE_QUIESCING,
    PSP_LIFECYCLE_SUSPENDED,
    PSP_LIFECYCLE_RECOVERING
} PspLifecycleState;

typedef enum {
    PSP_LIFECYCLE_ACTION_NONE = 0,
    PSP_LIFECYCLE_ACTION_QUIESCE,
    PSP_LIFECYCLE_ACTION_RECOVER
} PspLifecycleAction;

/*
 * The two notice counters and presentation gate are callback-thread state.
 * Everything else is owned by the main browser thread. A counter rather than
 * a boolean keeps a second suspend cycle from disappearing while the first is
 * being recovered.
 */
typedef struct {
    atomic_uint suspend_epoch;
    atomic_uint resume_epoch;
    atomic_bool presentation_allowed;
    unsigned handled_suspend_epoch;
    unsigned handled_resume_epoch;
    unsigned quiesce_count;
    unsigned recovery_count;
    PspLifecycleState state;
} PspLifecycle;

void psp_lifecycle_init(PspLifecycle *lifecycle);

/* Callback-thread entry points. They are allocation-free and non-blocking. */
void psp_lifecycle_notify_suspend(PspLifecycle *lifecycle);
void psp_lifecycle_notify_resume(PspLifecycle *lifecycle);

/*
 * Main-thread transition driver. When suspend and resume accumulated while
 * the CPU slept, QUIESCE is returned first and RECOVER only after the caller
 * marks quiescing complete.
 */
PspLifecycleAction psp_lifecycle_poll(PspLifecycle *lifecycle);
void psp_lifecycle_complete_quiesce(PspLifecycle *lifecycle);
void psp_lifecycle_complete_recovery(PspLifecycle *lifecycle);

bool psp_lifecycle_presentation_allowed(const PspLifecycle *lifecycle);
PspLifecycleState psp_lifecycle_state(const PspLifecycle *lifecycle);

#endif
