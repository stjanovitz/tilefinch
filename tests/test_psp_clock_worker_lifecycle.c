#include "tilefinch/psp_clock_worker.h"

#include <stdio.h>
#include <string.h>

/* Firmware effects only are substituted; lifecycle code is exactly the
   owner-side implementation linked into the PSP browser. No real thread. */
#define PSP_EVENT_WAITSINGLE 0
#define PSP_THREAD_ATTR_USER 0
#define TILEFINCH_PSP_THREAD_PRIORITY_CLOCK 33
#define CLOCK_WORKER_WAKE 1u
#define CLOCK_WORKER_STACK_BYTES (8u * 1024u)
static int event_create_result, thread_create_result, start_result;
static int wait_result, thread_delete_result, event_delete_result, signal_result;
static unsigned creates, starts, waits, thread_deletes, event_deletes, signals;
static unsigned wait_timeout;
static uint64_t now_us;

static int clock_worker_main(size_t size, void *arguments)
{
    (void) size; (void) arguments;
    return 0;
}
static int sceKernelCreateEventFlag(const char *name, unsigned attr, unsigned bits, void *options)
{
    (void) name; (void) attr; (void) bits; (void) options;
    creates++;
    return event_create_result;
}
static int sceKernelCreateThread(const char *name, int (*entry)(size_t, void *),
    int priority, size_t stack, unsigned attr, void *options)
{
    (void) name; (void) entry; (void) priority; (void) stack; (void) attr; (void) options;
    creates++;
    return thread_create_result;
}
static int sceKernelStartThread(int thread, size_t size, void *arguments)
{
    (void) thread; (void) size; (void) arguments;
    starts++;
    return start_result;
}
static int psp_thread_wait_end_bounded(int thread, unsigned timeout)
{
    (void) thread;
    waits++;
    wait_timeout = timeout;
    return wait_result;
}
static int sceKernelDeleteThread(int thread)
{
    (void) thread;
    thread_deletes++;
    return thread_delete_result;
}
static int sceKernelDeleteEventFlag(int event)
{
    (void) event;
    event_deletes++;
    return event_delete_result;
}
static int sceKernelSetEventFlag(int event, unsigned bits)
{
    (void) event; (void) bits;
    signals++;
    return signal_result;
}
static uint64_t sceKernelGetSystemTimeWide(void) { return now_us++; }
static void sceKernelDelayThread(unsigned us) { now_us += us; }
#include "../src/psp_clock_worker_lifecycle.inc"

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: %s\n", name, __LINE__, #c); return false; \
} } while (0)
static bool run_case(const char *name)
{
    PspClockWorker worker = {0};
    creates = starts = waits = thread_deletes = event_deletes = signals = wait_timeout = 0;
    event_create_result = 23;
    thread_create_result = 17;
    start_result = wait_result = thread_delete_result = event_delete_result = signal_result = 0;
    now_us = 0;
    if (!strcmp(name, "fresh")) {
        CHECK(!psp_clock_worker_owns_resources(&worker));
        CHECK(psp_clock_worker_shutdown(&worker));
        CHECK(!waits && !thread_deletes && !event_deletes);
    } else if (!strcmp(name, "start-thread-delete-refused")) {
        start_result = -1;
        thread_delete_result = -2;
        CHECK(!psp_clock_worker_start(&worker));
        CHECK(psp_clock_worker_owns_resources(&worker)
            && worker.thread == 17 && worker.event == 23 && !worker.started);
        CHECK(waits == 0 && event_deletes == 0);
        unsigned before = creates;
        CHECK(!psp_clock_worker_start(&worker) && creates == before
            && worker.thread == 17 && worker.event == 23);
        thread_delete_result = 0;
        CHECK(psp_clock_worker_shutdown(&worker));
        CHECK(!psp_clock_worker_owns_resources(&worker) && waits == 0
            && thread_deletes == 2 && event_deletes == 1);
    } else if (!strcmp(name, "create-thread-event-delete-refused")) {
        thread_create_result = -1;
        event_delete_result = -2;
        CHECK(!psp_clock_worker_start(&worker));
        CHECK(psp_clock_worker_owns_resources(&worker) && worker.event == 23);
        CHECK(waits == 0 && thread_deletes == 0);
        event_delete_result = 0;
        CHECK(psp_clock_worker_shutdown(&worker));
        CHECK(!psp_clock_worker_owns_resources(&worker) && event_deletes == 2);
    } else if (!strcmp(name, "start-cleanup")) {
        start_result = -1;
        CHECK(!psp_clock_worker_start(&worker));
        CHECK(!psp_clock_worker_owns_resources(&worker) && waits == 0
            && thread_deletes == 1 && event_deletes == 1);
    } else if (!strcmp(name, "event-create-refused")) {
        event_create_result = -1;
        CHECK(!psp_clock_worker_start(&worker));
        CHECK(!psp_clock_worker_owns_resources(&worker));
        CHECK(psp_clock_worker_shutdown(&worker) && waits == 0 && thread_deletes == 0
            && event_deletes == 0 && starts == 0);
    } else {
        CHECK(psp_clock_worker_start(&worker));
        CHECK(psp_clock_worker_owns_resources(&worker));
        if (!strcmp(name, "join-refused")) {
            wait_result = -1;
            CHECK(!psp_clock_worker_shutdown(&worker));
            CHECK(psp_clock_worker_owns_resources(&worker) && worker.thread == 17
                && worker.event == 23 && thread_deletes == 0 && event_deletes == 0);
            CHECK(wait_timeout == 500000u);
            wait_result = 0;
            CHECK(psp_clock_worker_shutdown(&worker) && waits == 2);
        } else if (!strcmp(name, "thread-delete-refused")) {
            thread_delete_result = -1;
            CHECK(!psp_clock_worker_shutdown(&worker));
            CHECK(psp_clock_worker_owns_resources(&worker) && worker.thread == 17
                && worker.event == 23 && waits == 1 && event_deletes == 0);
            thread_delete_result = 0;
            CHECK(psp_clock_worker_shutdown(&worker) && waits == 1
                && thread_deletes == 2 && event_deletes == 1);
        } else if (!strcmp(name, "event-delete-refused")) {
            event_delete_result = -1;
            CHECK(!psp_clock_worker_shutdown(&worker));
            CHECK(psp_clock_worker_owns_resources(&worker) && worker.thread < 0
                && worker.event == 23 && waits == 1 && thread_deletes == 1);
            event_delete_result = 0;
            CHECK(psp_clock_worker_shutdown(&worker) && waits == 1
                && thread_deletes == 1 && event_deletes == 2);
        } else if (!strcmp(name, "request-after-stop")) {
            wait_result = -1;
            CHECK(!psp_clock_worker_shutdown(&worker));
            unsigned before = signals;
            CHECK(!psp_clock_worker_request(&worker, false, NULL) && signals == before);
            CHECK(!psp_clock_worker_wait_applied(&worker, false, 10));
            wait_result = 0;
            CHECK(psp_clock_worker_shutdown(&worker));
        } else if (!strcmp(name, "restart-retained")) {
            wait_result = -1;
            CHECK(!psp_clock_worker_shutdown(&worker));
            unsigned before = creates;
            CHECK(!psp_clock_worker_start(&worker) && creates == before
                && worker.thread == 17 && worker.event == 23);
            wait_result = 0;
            CHECK(psp_clock_worker_shutdown(&worker));
        } else if (!strcmp(name, "request-control")) {
            signal_result = -1;
            CHECK(!psp_clock_worker_request(&worker, true, NULL));
            CHECK(!psp_clock_worker_wait_applied(&worker, true, 1));
            signal_result = 0;
            uint64_t cost = 0;
            CHECK(psp_clock_worker_request(&worker, false, &cost) && cost > 0);
            CHECK(psp_clock_worker_wait_applied(&worker, false, 1));
            CHECK(psp_clock_worker_shutdown(&worker));
        } else if (!strcmp(name, "normal-restart")) {
            CHECK(psp_clock_worker_shutdown(&worker));
            CHECK(!psp_clock_worker_owns_resources(&worker));
            CHECK(psp_clock_worker_shutdown(&worker) && waits == 1);
            CHECK(psp_clock_worker_start(&worker));
            CHECK(psp_clock_worker_shutdown(&worker) && waits == 2);
        } else return false;
        CHECK(!psp_clock_worker_owns_resources(&worker));
    }
    return true;
}
int main(int argc, char **argv)
{
    const char *cases[] = {"fresh", "start-thread-delete-refused",
        "create-thread-event-delete-refused", "start-cleanup", "event-create-refused",
        "join-refused", "thread-delete-refused", "event-delete-refused", "request-after-stop",
        "restart-retained", "request-control", "normal-restart"};
    bool ok = true;
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (argc > 1 && strcmp(argv[1], cases[i])) continue;
        bool passed = run_case(cases[i]);
        printf("%s: %s\n", cases[i], passed ? "PASS" : "FAIL");
        ok = passed && ok;
    }
    return ok ? 0 : 1;
}
