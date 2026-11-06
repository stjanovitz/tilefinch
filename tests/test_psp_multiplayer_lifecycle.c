#include "tilefinch/budget.h"
#include "tilefinch/multiplayer.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

/* Firmware and ownership carriers only: all transitions below are compiled
   from the same lifecycle include as the device backend. No socket thread. */
#define PSP_MULTIPLAYER_EVENT_STORAGE 10u
#define PSP_MULTIPLAYER_THREAD_STACK (16u * 1024u)
#define PSP_NETWORK_REQUEST_MULTIPLAYER 1
#define PSP_NETWORK_SUPERVISOR_STATE_COUNT 99
#define PSP_NETWORK_READY 1
#define TILEFINCH_PSP_THREAD_PRIORITY_TRANSPORT_SETUP 20
#define PSP_THREAD_ATTR_USER 0
typedef struct { int status; } PspNetwork;
typedef struct { bool ready; } PspNetworkLifecycle;
typedef struct { struct { unsigned network_profile; } config; } PspProcessResources;
typedef struct { int unused; } PspBrowserResources;
typedef struct {
    TilefinchMultiplayerEvent event;
    unsigned char data[TILEFINCH_MULTIPLAYER_MESSAGE_LIMIT];
} PspMultiplayerQueuedEvent;
typedef struct {
    Budget *budget;
    BudgetReservation stack_reservation;
    int thread;
    bool thread_needs_join;
    atomic_uint event_read, event_write;
    atomic_int stop_requested, worker_done;
    PspMultiplayerQueuedEvent events[PSP_MULTIPLAYER_EVENT_STORAGE];
    uint64_t id, created_us;
    bool close_queued;
} PspMultiplayerSession;
typedef struct {
    PspProcessResources *process;
    PspBrowserResources *browser;
    PspNetwork *network;
    PspNetworkLifecycle *network_lifecycle;
    PspMultiplayerSession *session;
    bool network_requested, cancel_pending;
} PspMultiplayerContext;
typedef struct {
    PspProcessResources *process;
    PspNetwork *network;
    PspNetworkLifecycle *network_lifecycle;
} PspApp;

static PspMultiplayerContext psp_multiplayer;
static atomic_bool multiplayer_network_stop, multiplayer_network_owned;
static bool transport_quiesced;
static unsigned transport_stop_requests;
static unsigned wait_calls, unbounded_waits, delete_calls, create_calls, start_calls, lease_releases;
static unsigned last_wait_timeout;
static int wait_result, delete_result, start_result;
static uint64_t now_us;

static int sceKernelWaitThreadEnd(int thread, unsigned *timeout)
{
    (void) thread;
    wait_calls++;
    last_wait_timeout = timeout == NULL ? 0 : *timeout;
    if (timeout == NULL || *timeout == 0 || *timeout > 500000u) unbounded_waits++;
    return wait_result;
}
static int psp_thread_wait_end_bounded(int thread, unsigned timeout)
{
    return sceKernelWaitThreadEnd(thread, &timeout);
}
static int sceKernelDeleteThread(int thread)
{
    (void) thread;
    delete_calls++;
    return delete_result;
}
static int multiplayer_worker(size_t size, void *arguments)
{
    (void) size; (void) arguments;
    return 0;
}
static int sceKernelCreateThread(const char *name, int (*entry)(size_t, void *),
                                int priority, size_t stack, unsigned attr, void *options)
{
    (void) name; (void) entry; (void) priority; (void) stack; (void) attr; (void) options;
    create_calls++;
    return 17;
}
static int sceKernelStartThread(int thread, size_t size, void *arguments)
{
    (void) thread; (void) size; (void) arguments;
    start_calls++;
    return start_result;
}
static bool psp_network_lifecycle_ready(const PspNetworkLifecycle *lifecycle)
{
    return lifecycle->ready;
}
static void psp_network_lifecycle_request(PspNetworkLifecycle *lifecycle, int request,
    bool enabled, int profile, PspNetwork *network, int state, const char *reason)
{
    (void) lifecycle; (void) request; (void) profile; (void) network; (void) state; (void) reason;
    if (!enabled) lease_releases++;
}
static uint64_t tilefinch_platform_monotonic_time_us(void) { return now_us; }
static void multiplayer_close_event(PspMultiplayerSession *session, uint16_t code,
                                     bool clean, const char *detail)
{
    if (session->close_queued) return;
    unsigned write = atomic_load(&session->event_write);
    session->events[write % PSP_MULTIPLAYER_EVENT_STORAGE].event =
        (TilefinchMultiplayerEvent) {
            .kind = TILEFINCH_MULTIPLAYER_EVENT_CLOSE, .close_code = code, .clean = clean
        };
    snprintf(session->events[write % PSP_MULTIPLAYER_EVENT_STORAGE].event.detail,
             sizeof(session->events[0].event.detail), "%s", detail);
    atomic_store(&session->event_write, write + 1u);
    session->close_queued = true;
}
#include "../src/psp_multiplayer_lifecycle.inc"
static void fetch_background_transport_request_quiesce(void) { transport_stop_requests++; }
static bool fetch_background_transport_is_quiesced(void) { return transport_quiesced; }
#include "../src/psp_network_workers.inc"

typedef struct {
    Budget budget;
    PspProcessResources process;
    PspBrowserResources browser;
    PspNetwork network;
    PspNetworkLifecycle lifecycle;
    PspApp app;
    bool registered;
} Fixture;
static uint64_t fixture_open(void *opaque, Budget *budget,
                             const TilefinchMultiplayerRequest *request)
{
    PspMultiplayerContext *context = opaque;
    return budget != NULL && request != NULL && context->session != NULL
        ? context->session->id : 0;
}
static const TilefinchMultiplayerBackend backend = {
    .open = fixture_open, .cancel = multiplayer_backend_cancel,
    .take_event = multiplayer_backend_take
};
static bool fixture_begin(Fixture *fixture, bool threaded)
{
    memset(fixture, 0, sizeof(*fixture));
    memset(&psp_multiplayer, 0, sizeof(psp_multiplayer));
    wait_calls = unbounded_waits = delete_calls = create_calls = start_calls = lease_releases = 0;
    last_wait_timeout = 0;
    wait_result = delete_result = start_result = 0;
    now_us = 1;
    atomic_store(&multiplayer_network_stop, false);
    atomic_store(&multiplayer_network_owned, threaded);
    transport_quiesced = true;
    transport_stop_requests = 0;
    budget_init(&fixture->budget, 1024u * 1024u);
    PspMultiplayerSession *session = budget_calloc(&fixture->budget, 1, sizeof(*session));
    if (session == NULL) return false;
    session->budget = &fixture->budget;
    session->id = 11;
    session->thread = threaded ? 7 : -1;
    session->thread_needs_join = threaded;
    atomic_init(&session->event_read, 0);
    atomic_init(&session->event_write, 0);
    atomic_init(&session->stop_requested, 0);
    atomic_init(&session->worker_done, 0);
    if (!budget_reservation_acquire(&session->stack_reservation, &fixture->budget,
            BUDGET_CATEGORY_RESOURCE, PSP_MULTIPLAYER_THREAD_STACK)) return false;
    fixture->process.config.network_profile = 2;
    fixture->network.status = PSP_NETWORK_READY;
    fixture->lifecycle.ready = true;
    psp_multiplayer = (PspMultiplayerContext) {
        .process = &fixture->process, .browser = &fixture->browser,
        .network = &fixture->network, .network_lifecycle = &fixture->lifecycle,
        .session = session, .network_requested = true
    };
    fixture->app = (PspApp) {
        .process = &fixture->process, .network = &fixture->network,
        .network_lifecycle = &fixture->lifecycle
    };
    if (!tilefinch_multiplayer_install_backend(&backend, &psp_multiplayer)) return false;
    TilefinchMultiplayerRequest request = {.mode = TILEFINCH_MULTIPLAYER_MODE_HOST};
    snprintf(request.game_id, sizeof(request.game_id), "lifecycle");
    snprintf(request.peer_name, sizeof(request.peer_name), "Test");
    fixture->registered = tilefinch_multiplayer_open(&fixture->budget, &request) == 11;
    return fixture->registered;
}
static bool fixture_end(Fixture *fixture)
{
    if (psp_multiplayer.session != NULL) {
        wait_result = delete_result = 0;
        atomic_store(&psp_multiplayer.session->worker_done, 1);
        if (!tilefinch_multiplayer_cancel(11))
            (void) multiplayer_release(&psp_multiplayer);
    }
    return tilefinch_multiplayer_install_backend(NULL, NULL)
        && psp_multiplayer_network_quiesced()
        && fixture->budget.current == 0 && fixture->budget.external_reserved == 0;
}
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: %s\n", name, __LINE__, #c); ok = false; goto cleanup; \
} } while (0)

static bool run_case(const char *name)
{
    Fixture fixture;
    bool ok = fixture_begin(&fixture, strcmp(name, "unstarted") != 0
        && strcmp(name, "timeout") != 0 && strcmp(name, "start-refusal") != 0
        && strcmp(name, "network-unstarted") != 0);
    if (!ok) return false;
    PspMultiplayerSession *session = psp_multiplayer.session;
    size_t charged = fixture.budget.current;
    if (!strcmp(name, "live-cancel")) {
        CHECK(!tilefinch_multiplayer_cancel(11));
        CHECK(psp_multiplayer.session == session && psp_multiplayer.cancel_pending
            && atomic_load(&session->stop_requested) && fixture.budget.current == charged
            && lease_releases == 0 && wait_calls == 0 && delete_calls == 0);
        CHECK(!tilefinch_multiplayer_install_backend(NULL, NULL));
        atomic_store(&session->worker_done, 1);
        psp_multiplayer_pump(&fixture.app);
        CHECK(psp_multiplayer.session == NULL && lease_releases == 1);
    } else if (!strcmp(name, "join-refusal") || !strcmp(name, "delete-refusal")) {
        bool refuse_join = !strcmp(name, "join-refusal");
        atomic_store(&session->worker_done, 1);
        wait_result = refuse_join ? -7 : 0;
        delete_result = refuse_join ? 0 : -8;
        CHECK(!tilefinch_multiplayer_cancel(11));
        CHECK(psp_multiplayer.session == session && fixture.budget.current == charged
            && lease_releases == 0 && session->thread == 7 && unbounded_waits == 0);
        CHECK(last_wait_timeout == 1000u);
        unsigned joined = wait_calls;
        wait_result = delete_result = 0;
        CHECK(tilefinch_multiplayer_cancel(11) && psp_multiplayer.session == NULL
            && lease_releases == 1 && fixture.budget.current == 0);
        CHECK(refuse_join || wait_calls == joined);
    } else if (!strcmp(name, "close")) {
        multiplayer_close_event(session, 1000, true, "done");
        TilefinchMultiplayerEvent event;
        CHECK(!tilefinch_multiplayer_take_event(11, NULL, 0, &event));
        CHECK(psp_multiplayer.session == session && atomic_load(&session->event_read) == 0
            && lease_releases == 0 && wait_calls == 0);
        atomic_store(&session->worker_done, 1);
        wait_result = -7;
        CHECK(!tilefinch_multiplayer_take_event(11, NULL, 0, &event));
        CHECK(psp_multiplayer.session == session && atomic_load(&session->event_read) == 0);
        wait_result = 0;
        CHECK(tilefinch_multiplayer_take_event(11, NULL, 0, &event)
            && event.kind == TILEFINCH_MULTIPLAYER_EVENT_CLOSE
            && event.close_code == 1000 && psp_multiplayer.session == NULL);
    } else if (!strcmp(name, "shutdown")) {
        wait_result = -7;
        CHECK(!psp_multiplayer_shutdown(&fixture.app));
        CHECK(psp_multiplayer.session == session && psp_multiplayer.process == &fixture.process
            && psp_multiplayer.network_requested && fixture.budget.current == charged
            && lease_releases == 0 && wait_calls == 1 && unbounded_waits == 0);
        CHECK(last_wait_timeout == 500000u);
        CHECK(!tilefinch_multiplayer_install_backend(NULL, NULL));
        wait_result = 0;
        CHECK(psp_multiplayer_shutdown(&fixture.app)
            && psp_multiplayer.session == NULL && psp_multiplayer.process == NULL
            && psp_multiplayer.network == NULL && psp_multiplayer.network_lifecycle == NULL
            && fixture.budget.current == 0 && lease_releases == 1);
    } else if (!strcmp(name, "network-gate")) {
        CHECK(!psp_network_workers_is_quiesced());
        psp_network_workers_request_quiesce();
        CHECK(transport_stop_requests == 1 && atomic_load(&multiplayer_network_stop)
            && psp_multiplayer.session == session && fixture.budget.current == charged);
        psp_multiplayer_pump(&fixture.app);
        CHECK(!psp_multiplayer.cancel_pending && atomic_load(&session->stop_requested)
            && !psp_network_workers_is_quiesced() && lease_releases == 0);
        atomic_store(&session->worker_done, 1);
        psp_multiplayer_pump(&fixture.app);
        CHECK(psp_multiplayer.session == session && session->thread == -1
            && session->close_queued && psp_network_workers_is_quiesced()
            && lease_releases == 0 && fixture.budget.external_reserved == 0);
        TilefinchMultiplayerEvent event;
        CHECK(tilefinch_multiplayer_take_event(11, NULL, 0, &event)
            && event.kind == TILEFINCH_MULTIPLAYER_EVENT_CLOSE
            && event.close_code == 1001 && !event.clean
            && psp_multiplayer.session == NULL && lease_releases == 1
            && fixture.budget.current == 0);
        transport_quiesced = false;
        CHECK(!psp_network_workers_is_quiesced());
        transport_quiesced = true;
        CHECK(psp_network_workers_is_quiesced());
    } else if (!strcmp(name, "network-unstarted")) {
        CHECK(psp_network_workers_is_quiesced());
        psp_network_workers_request_quiesce();
        psp_multiplayer_pump(&fixture.app);
        CHECK(psp_multiplayer.session == session && session->close_queued
            && session->thread == -1 && create_calls == 0 && wait_calls == 0
            && fixture.budget.external_reserved == 0 && psp_network_workers_is_quiesced());
        TilefinchMultiplayerEvent event;
        CHECK(tilefinch_multiplayer_take_event(11, NULL, 0, &event)
            && event.kind == TILEFINCH_MULTIPLAYER_EVENT_CLOSE
            && event.close_code == 1001 && psp_multiplayer.session == NULL);
    } else if (!strcmp(name, "timeout")) {
        fixture.lifecycle.ready = false;
        now_us = UINT64_C(31000000);
        psp_multiplayer_pump(&fixture.app);
        CHECK(session->close_queued && atomic_load(&session->worker_done));
        fixture.lifecycle.ready = true;
        psp_multiplayer_pump(&fixture.app);
        CHECK(create_calls == 0 && start_calls == 0 && session->thread == -1);
    } else if (!strcmp(name, "start-refusal")) {
        start_result = -9;
        delete_result = -8;
        psp_multiplayer_pump(&fixture.app);
        CHECK(create_calls == 1 && start_calls == 1
            && psp_multiplayer.session == session && session->thread == 17
            && !session->thread_needs_join && session->close_queued);
        TilefinchMultiplayerEvent event;
        CHECK(!tilefinch_multiplayer_take_event(11, NULL, 0, &event));
        CHECK(psp_multiplayer.session == session && lease_releases == 0);
        delete_result = 0;
        CHECK(tilefinch_multiplayer_take_event(11, NULL, 0, &event)
            && psp_multiplayer.session == NULL && wait_calls == 0);
    } else if (!strcmp(name, "unstarted")) {
        CHECK(psp_network_workers_is_quiesced());
        CHECK(tilefinch_multiplayer_cancel(11) && psp_multiplayer.session == NULL
            && wait_calls == 0 && delete_calls == 0 && lease_releases == 1);
    } else {
        PspMultiplayerQueuedEvent *queued = &session->events[0];
        queued->event = (TilefinchMultiplayerEvent) {
            .kind = TILEFINCH_MULTIPLAYER_EVENT_MESSAGE, .length = 3
        };
        memcpy(queued->data, "abc", 3);
        atomic_store(&session->event_write, 1);
        TilefinchMultiplayerEvent event;
        unsigned char bytes[3];
        CHECK(!tilefinch_multiplayer_take_event(11, bytes, 2, &event));
        CHECK(tilefinch_multiplayer_take_event(11, bytes, sizeof(bytes), &event)
            && memcmp(bytes, "abc", 3) == 0 && psp_multiplayer.session == session
            && atomic_load(&session->event_read) == 1 && lease_releases == 0);
    }
cleanup:
    ok = fixture_end(&fixture) && ok;
    printf("multiplayer lifecycle %s: %s\n", name, ok ? "pass" : "fail");
    return ok;
}
int main(int argc, char **argv)
{
    static const char *cases[] = {
        "live-cancel", "join-refusal", "delete-refusal", "close",
        "shutdown", "timeout", "start-refusal", "unstarted", "message",
        "network-gate", "network-unstarted"
    };
    bool selected = false;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (argc > 1 && strcmp(argv[1], cases[i])) continue;
        selected = true;
        if (!run_case(cases[i])) return 1;
    }
    return selected ? 0 : 2;
}
