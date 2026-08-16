#include "tilefinch/voice_job_lifecycle.h"

#include <string.h>

static bool voice_job_platform_valid(
    const VoiceJobLifecyclePlatform *platform)
{
    return platform != NULL
        && platform->spawn != NULL
        && platform->poll_complete != NULL
        && platform->now_microseconds != NULL;
}

VoiceJobLifecycleResult voice_job_lifecycle_start(
    VoiceJobLifecycle *lifecycle,
    const VoiceJobLifecyclePlatform *platform,
    uint64_t timeout_microseconds)
{
    if (lifecycle == NULL || !voice_job_platform_valid(platform)
        || timeout_microseconds == 0) {
        return VOICE_JOB_LIFECYCLE_START_FAILED;
    }
    memset(lifecycle, 0, sizeof(*lifecycle));
    atomic_init(&lifecycle->cancellation_requested, false);
    lifecycle->started_microseconds =
        platform->now_microseconds(platform->user);
    lifecycle->timeout_microseconds = timeout_microseconds;
    if (!platform->spawn(platform->user)) {
        return VOICE_JOB_LIFECYCLE_START_FAILED;
    }
    lifecycle->started = true;
    return VOICE_JOB_LIFECYCLE_RUNNING;
}

void voice_job_lifecycle_request_cancel(VoiceJobLifecycle *lifecycle)
{
    if (lifecycle == NULL || !lifecycle->started
        || lifecycle->completed) return;
    atomic_store_explicit(
        &lifecycle->cancellation_requested, true, memory_order_release);
}

bool voice_job_lifecycle_cancelled(const VoiceJobLifecycle *lifecycle)
{
    return lifecycle != NULL
        && atomic_load_explicit(
               &lifecycle->cancellation_requested, memory_order_acquire);
}

bool voice_job_lifecycle_disables_voice(const VoiceJobLifecycle *lifecycle)
{
    return lifecycle != NULL && lifecycle->timed_out;
}

VoiceJobLifecycleResult voice_job_lifecycle_pump(
    VoiceJobLifecycle *lifecycle,
    const VoiceJobLifecyclePlatform *platform)
{
    if (lifecycle == NULL || !lifecycle->started
        || !voice_job_platform_valid(platform)) {
        return VOICE_JOB_LIFECYCLE_START_FAILED;
    }
    if (lifecycle->completed) {
        return lifecycle->timed_out
            ? VOICE_JOB_LIFECYCLE_COMPLETED_AFTER_TIMEOUT
            : VOICE_JOB_LIFECYCLE_COMPLETED;
    }
    if (platform->poll_complete(platform->user)) {
        lifecycle->completed = true;
        return lifecycle->timed_out
            ? VOICE_JOB_LIFECYCLE_COMPLETED_AFTER_TIMEOUT
            : VOICE_JOB_LIFECYCLE_COMPLETED;
    }
    if (!lifecycle->timed_out) {
        uint64_t now = platform->now_microseconds(platform->user);
        if (now - lifecycle->started_microseconds
            >= lifecycle->timeout_microseconds) {
            lifecycle->timed_out = true;
            voice_job_lifecycle_request_cancel(lifecycle);
            return VOICE_JOB_LIFECYCLE_CANCEL_REQUESTED;
        }
    }
    return lifecycle->timed_out
        ? VOICE_JOB_LIFECYCLE_CANCEL_REQUESTED
        : VOICE_JOB_LIFECYCLE_RUNNING;
}
