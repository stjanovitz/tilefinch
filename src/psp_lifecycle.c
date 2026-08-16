#include "tilefinch/psp_lifecycle.h"

#include <string.h>

void psp_lifecycle_init(PspLifecycle *lifecycle)
{
    if (lifecycle == NULL) return;
    memset(lifecycle, 0, sizeof(*lifecycle));
    atomic_init(&lifecycle->suspend_epoch, 0);
    atomic_init(&lifecycle->resume_epoch, 0);
    atomic_init(&lifecycle->presentation_allowed, true);
    lifecycle->state = PSP_LIFECYCLE_RUNNING;
}

void psp_lifecycle_notify_suspend(PspLifecycle *lifecycle)
{
    if (lifecycle == NULL) return;
    /*
     * Disable callback-thread scanout before publishing the epoch. The main
     * thread may already be frozen, but the loading supervisor must not race
     * display rearm when callbacks resume first.
     */
    atomic_store_explicit(
        &lifecycle->presentation_allowed, false, memory_order_release);
    (void) atomic_fetch_add_explicit(
        &lifecycle->suspend_epoch, 1u, memory_order_acq_rel);
}

void psp_lifecycle_notify_resume(PspLifecycle *lifecycle)
{
    if (lifecycle == NULL) return;
    unsigned suspend_epoch = atomic_load_explicit(
        &lifecycle->suspend_epoch, memory_order_acquire);
    if (suspend_epoch == 0) return;
    /*
     * RESUMING and RESUME_COMPLETE may both be delivered for one sleep. Store
     * the matching suspend epoch so that duplicate callbacks coalesce.
     */
    atomic_store_explicit(
        &lifecycle->resume_epoch, suspend_epoch, memory_order_release);
}

PspLifecycleAction psp_lifecycle_poll(PspLifecycle *lifecycle)
{
    if (lifecycle == NULL) return PSP_LIFECYCLE_ACTION_NONE;
    unsigned suspend_epoch = atomic_load_explicit(
        &lifecycle->suspend_epoch, memory_order_acquire);
    if (suspend_epoch != lifecycle->handled_suspend_epoch) {
        lifecycle->handled_suspend_epoch = suspend_epoch;
        lifecycle->state = PSP_LIFECYCLE_QUIESCING;
        lifecycle->quiesce_count++;
        return PSP_LIFECYCLE_ACTION_QUIESCE;
    }
    if (lifecycle->state == PSP_LIFECYCLE_QUIESCING) {
        return PSP_LIFECYCLE_ACTION_NONE;
    }
    unsigned resume_epoch = atomic_load_explicit(
        &lifecycle->resume_epoch, memory_order_acquire);
    if (resume_epoch != lifecycle->handled_resume_epoch
        && resume_epoch == lifecycle->handled_suspend_epoch) {
        lifecycle->handled_resume_epoch = resume_epoch;
        lifecycle->state = PSP_LIFECYCLE_RECOVERING;
        lifecycle->recovery_count++;
        return PSP_LIFECYCLE_ACTION_RECOVER;
    }
    return PSP_LIFECYCLE_ACTION_NONE;
}

void psp_lifecycle_complete_quiesce(PspLifecycle *lifecycle)
{
    if (lifecycle == NULL
        || lifecycle->state != PSP_LIFECYCLE_QUIESCING) return;
    lifecycle->state = PSP_LIFECYCLE_SUSPENDED;
}

void psp_lifecycle_complete_recovery(PspLifecycle *lifecycle)
{
    if (lifecycle == NULL
        || lifecycle->state != PSP_LIFECYCLE_RECOVERING) return;
    lifecycle->state = PSP_LIFECYCLE_RUNNING;
    atomic_store_explicit(
        &lifecycle->presentation_allowed, true, memory_order_release);
}

bool psp_lifecycle_presentation_allowed(const PspLifecycle *lifecycle)
{
    return lifecycle != NULL
        && atomic_load_explicit(
            &lifecycle->presentation_allowed, memory_order_acquire);
}

PspLifecycleState psp_lifecycle_state(const PspLifecycle *lifecycle)
{
    return lifecycle == NULL
        ? PSP_LIFECYCLE_SUSPENDED : lifecycle->state;
}
