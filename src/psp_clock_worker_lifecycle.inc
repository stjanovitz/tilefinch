/* Owner-side firmware transitions, shared with host refusal/order tests. */
bool psp_clock_worker_owns_resources(const PspClockWorker *worker)
{
    return worker != NULL && worker->initialized
        && (worker->thread >= 0 || worker->event >= 0);
}

bool psp_clock_worker_start(PspClockWorker *worker)
{
    if (worker == NULL || psp_clock_worker_owns_resources(worker)) return false;
    memset(worker, 0, sizeof(*worker));
    worker->initialized = true;
    worker->thread = -1;
    worker->event = -1;
    atomic_init(&worker->desired_idle, 0);
    atomic_init(&worker->applied_idle, 0);
    atomic_init(&worker->transitioning, 0);
    atomic_init(&worker->stop_requested, 0);
    atomic_init(&worker->completions, 0);
    atomic_init(&worker->failures, 0);
    atomic_init(&worker->last_transition_us, 0);
    atomic_init(&worker->maximum_transition_us, 0);
    worker->event = sceKernelCreateEventFlag(
        "tilefinch_clock", PSP_EVENT_WAITSINGLE, 0, NULL);
    if (worker->event < 0) return false;
    worker->thread = sceKernelCreateThread(
        "tilefinch_clock", clock_worker_main,
        TILEFINCH_PSP_THREAD_PRIORITY_CLOCK,
        CLOCK_WORKER_STACK_BYTES, PSP_THREAD_ATTR_USER, NULL);
    if (worker->thread < 0) {
        (void) psp_clock_worker_shutdown(worker);
        return false;
    }
    PspClockWorker *pointer = worker;
    if (sceKernelStartThread(
            worker->thread, sizeof(pointer), &pointer) < 0) {
        (void) psp_clock_worker_shutdown(worker);
        return false;
    }
    worker->started = true;
    return true;
}

bool psp_clock_worker_request(
    PspClockWorker *worker, bool idle, uint64_t *request_us)
{
    if (worker == NULL || !worker->started
        || atomic_load_explicit(&worker->stop_requested, memory_order_acquire))
        return false;
    uint64_t started_us = sceKernelGetSystemTimeWide();
    atomic_store_explicit(
        &worker->desired_idle, idle ? 1 : 0, memory_order_release);
    int result = sceKernelSetEventFlag(worker->event, CLOCK_WORKER_WAKE);
    if (request_us != NULL)
        *request_us = sceKernelGetSystemTimeWide() - started_us;
    return result >= 0;
}

bool psp_clock_worker_wait_applied(
    PspClockWorker *worker, bool idle, unsigned timeout_ms)
{
    if (worker == NULL || !worker->started
        || atomic_load_explicit(&worker->stop_requested, memory_order_acquire))
        return false;
    uint64_t deadline =
        sceKernelGetSystemTimeWide() + (uint64_t) timeout_ms * 1000u;
    do {
        bool applied = atomic_load_explicit(
            &worker->applied_idle, memory_order_acquire) != 0;
        bool desired = atomic_load_explicit(
            &worker->desired_idle, memory_order_acquire) != 0;
        bool transitioning = atomic_load_explicit(
            &worker->transitioning, memory_order_acquire) != 0;
        if (applied == idle && desired == idle && !transitioning)
            return true;
        sceKernelDelayThread(1000);
    } while ((uint64_t) sceKernelGetSystemTimeWide() < deadline);
    return false;
}

bool psp_clock_worker_shutdown(PspClockWorker *worker)
{
    if (worker == NULL || !worker->initialized) return true;
    if (worker->started && !worker->joined) {
        atomic_store_explicit(
            &worker->stop_requested, 1, memory_order_release);
        (void) sceKernelSetEventFlag(worker->event, CLOCK_WORKER_WAKE);
        if (psp_thread_wait_end_bounded(worker->thread, 500000u) < 0)
            return false;
        worker->joined = true;
    }
    /* Join, thread deletion and event deletion are separate obligations.
       Keep each surviving handle on refusal, including an unstarted thread
       from failed initialization. Retrying never rejoins a deleted worker. */
    if (worker->thread >= 0) {
        if (sceKernelDeleteThread(worker->thread) < 0) return false;
        worker->thread = -1;
        worker->started = false;
    }
    if (worker->event >= 0) {
        if (sceKernelDeleteEventFlag(worker->event) < 0) return false;
        worker->event = -1;
    }
    memset(worker, 0, sizeof(*worker));
    return true;
}
