/* Shared device ownership/retirement logic, included by the PSP backend and
   host tests which substitute firmware calls and ownership carriers. */
void psp_multiplayer_network_request_quiesce(void)
{
    atomic_store_explicit(&multiplayer_network_stop, true, memory_order_release);
}

bool psp_multiplayer_network_quiesced(void)
{
    return !atomic_load_explicit(&multiplayer_network_owned, memory_order_acquire);
}

static bool multiplayer_join_worker(PspMultiplayerSession *session, bool wait)
{
    if (session->thread < 0 || !session->thread_needs_join) return true;
    /* Ordinary navigation/callback turns must not wait for a running worker.
       After done, ordinary turns spend at most a short join timeout; exit
       may use the longer bounded wait. Done alone is not a join: the flag
       is published just before the worker returns. */
    if (!wait && atomic_load_explicit(
            &session->worker_done, memory_order_acquire) == 0) return false;
    if (psp_thread_wait_end_bounded(
            session->thread, wait ? 500000u : 1000u) < 0)
        return false;
    session->thread_needs_join = false;
    return true;
}

static bool multiplayer_retire_worker(PspMultiplayerSession *session, bool wait)
{
    if (session->thread >= 0) {
        if (!multiplayer_join_worker(session, wait)
            || sceKernelDeleteThread(session->thread) < 0) return false;
        session->thread = -1;
    }
    budget_reservation_release(&session->stack_reservation);
    atomic_store_explicit(&multiplayer_network_owned, false, memory_order_release);
    return true;
}

static bool multiplayer_release(PspMultiplayerContext *context)
{
    PspMultiplayerSession *session = context == NULL ? NULL : context->session;
    if (session == NULL) return true;
    if (!multiplayer_retire_worker(session, false)) return false;
    psp_network_lifecycle_request(
        context->network_lifecycle, PSP_NETWORK_REQUEST_MULTIPLAYER,
        false, (int) context->process->config.network_profile,
        context->network, PSP_NETWORK_SUPERVISOR_STATE_COUNT,
        "multiplayer-close");
    Budget *budget = session->budget;
    context->session = NULL;
    context->network_requested = false;
    context->cancel_pending = false;
    budget_free(budget, session);
    return true;
}

static bool multiplayer_backend_cancel(void *opaque, uint64_t id)
{
    PspMultiplayerContext *context = opaque;
    if (context == NULL || context->session == NULL
        || context->session->id != id) return false;
    atomic_store_explicit(
        &context->session->stop_requested, 1, memory_order_release);
    context->cancel_pending = true;
    return multiplayer_release(context);
}

static bool multiplayer_backend_take(
    void *opaque, uint64_t id, unsigned char *data, size_t capacity,
    TilefinchMultiplayerEvent *event)
{
    PspMultiplayerContext *context = opaque;
    PspMultiplayerSession *session =
        context == NULL ? NULL : context->session;
    if (session == NULL || session->id != id || event == NULL) return false;
    unsigned read = atomic_load_explicit(
        &session->event_read, memory_order_relaxed);
    unsigned write = atomic_load_explicit(
        &session->event_write, memory_order_acquire);
    if (read == write) return false;
    PspMultiplayerQueuedEvent *queued =
        &session->events[read % PSP_MULTIPLAYER_EVENT_STORAGE];
    if (queued->event.length > capacity
        || (queued->event.length != 0 && data == NULL)) return false;
    *event = queued->event;
    if (event->length != 0) memcpy(data, queued->data, event->length);
    if (event->kind == TILEFINCH_MULTIPLAYER_EVENT_CLOSE)
        return multiplayer_release(context);
    atomic_store_explicit(
        &session->event_read, read + 1u, memory_order_release);
    return true;
}

void psp_multiplayer_pump(PspApp *app)
{
    PspMultiplayerSession *session = psp_multiplayer.session;
    if (app == NULL || app->process != psp_multiplayer.process
        || app->network != psp_multiplayer.network || session == NULL) return;
    if (psp_multiplayer.cancel_pending) {
        (void) tilefinch_multiplayer_cancel(session->id);
        return;
    }
    if (atomic_load_explicit(&multiplayer_network_stop, memory_order_acquire)) {
        atomic_store_explicit(&session->stop_requested, 1, memory_order_release);
        if (!multiplayer_retire_worker(session, false)) return;
        /* A live realm still owns its terminal event after the physical
           worker/lease retires. Cancellation of a retired realm takes the
           branch above instead and needs no author notification. */
        multiplayer_close_event(session, 1001u, false, "network stopped");
        atomic_store_explicit(&session->worker_done, 1, memory_order_release);
        return;
    }
    if (session->thread >= 0 || session->close_queued
        || atomic_load_explicit(&session->worker_done, memory_order_acquire)
        || atomic_load_explicit(&session->stop_requested, memory_order_acquire))
        return;
    if (!psp_network_lifecycle_ready(app->network_lifecycle)
        || app->network->status != PSP_NETWORK_READY) {
        uint64_t now_us = tilefinch_platform_monotonic_time_us();
        if (!session->close_queued
            && now_us - session->created_us > UINT64_C(30000000)) {
            multiplayer_close_event(
                session, 1006u, false, "network connection timed out");
            atomic_store_explicit(
                &session->worker_done, 1, memory_order_release);
        }
        return;
    }
    session->thread = sceKernelCreateThread(
        "tilefinch_multiplayer", multiplayer_worker,
        TILEFINCH_PSP_THREAD_PRIORITY_TRANSPORT_SETUP,
        PSP_MULTIPLAYER_THREAD_STACK, PSP_THREAD_ATTR_USER, NULL);
    if (session->thread >= 0)
        atomic_store_explicit(&multiplayer_network_owned, true, memory_order_release);
    PspMultiplayerSession *argument = session;
    if (session->thread < 0
        || sceKernelStartThread(
               session->thread, sizeof(argument), &argument) < 0) {
        if (session->thread >= 0) {
            if (sceKernelDeleteThread(session->thread) >= 0) {
                session->thread = -1;
                atomic_store_explicit(
                    &multiplayer_network_owned, false, memory_order_release);
            }
        }
        multiplayer_close_event(session, 1006u, false, "worker unavailable");
        atomic_store_explicit(&session->worker_done, 1, memory_order_release);
    } else session->thread_needs_join = true;
}

bool psp_multiplayer_shutdown(PspApp *app)
{
    if (app == NULL || app->process != psp_multiplayer.process) return true;
    if (psp_multiplayer.session != NULL) {
        atomic_store_explicit(
            &psp_multiplayer.session->stop_requested, 1,
            memory_order_release);
        psp_multiplayer.cancel_pending = true;
        if (!multiplayer_retire_worker(psp_multiplayer.session, true)
            || !tilefinch_multiplayer_cancel(psp_multiplayer.session->id))
            return false;
    }
    if (!tilefinch_multiplayer_install_backend(NULL, NULL)) return false;
    psp_multiplayer.process = NULL;
    psp_multiplayer.browser = NULL;
    psp_multiplayer.network = NULL;
    psp_multiplayer.network_lifecycle = NULL;
    return true;
}
