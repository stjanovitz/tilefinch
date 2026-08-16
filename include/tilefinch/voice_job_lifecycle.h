#ifndef TILEFINCH_VOICE_JOB_LIFECYCLE_H
#define TILEFINCH_VOICE_JOB_LIFECYCLE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

typedef bool (*VoiceJobSpawn)(void *user);
typedef bool (*VoiceJobPollComplete)(void *user);
typedef uint64_t (*VoiceJobNowMicroseconds)(void *user);

typedef struct {
    VoiceJobSpawn spawn;
    VoiceJobPollComplete poll_complete;
    VoiceJobNowMicroseconds now_microseconds;
    void *user;
} VoiceJobLifecyclePlatform;

typedef enum {
    VOICE_JOB_LIFECYCLE_START_FAILED = 0,
    VOICE_JOB_LIFECYCLE_RUNNING,
    VOICE_JOB_LIFECYCLE_CANCEL_REQUESTED,
    VOICE_JOB_LIFECYCLE_COMPLETED,
    VOICE_JOB_LIFECYCLE_COMPLETED_AFTER_TIMEOUT
} VoiceJobLifecycleResult;

typedef struct {
    atomic_bool cancellation_requested;
    uint64_t started_microseconds;
    uint64_t timeout_microseconds;
    bool started;
    bool timed_out;
    bool completed;
} VoiceJobLifecycle;

/*
 * Start and advance the ownership protocol independently from any PSP
 * thread/event implementation. Once cancellation is requested, the owner
 * must continue polling until COMPLETED_AFTER_TIMEOUT before reclaiming
 * worker-owned memory.
 */
VoiceJobLifecycleResult voice_job_lifecycle_start(
    VoiceJobLifecycle *lifecycle,
    const VoiceJobLifecyclePlatform *platform,
    uint64_t timeout_microseconds);

VoiceJobLifecycleResult voice_job_lifecycle_pump(
    VoiceJobLifecycle *lifecycle,
    const VoiceJobLifecyclePlatform *platform);

void voice_job_lifecycle_request_cancel(VoiceJobLifecycle *lifecycle);
bool voice_job_lifecycle_cancelled(const VoiceJobLifecycle *lifecycle);
bool voice_job_lifecycle_disables_voice(const VoiceJobLifecycle *lifecycle);

#endif
