/* The work ledger's exclusive accounting (include/tilefinch/work_ledger.h):
   nested kinds, the enclosing kind restored on leave, the counted thread
   only, and nothing while disabled. A fake platform clock makes every
   interval exact. */
#include "tilefinch/platform.h"
#include "tilefinch/work_ledger.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                #condition); \
        return 1; \
    } \
} while (0)

static uint64_t fake_now_us;

static uint64_t fake_clock_ns(void *context)
{
    (void) context;
    return fake_now_us * UINT64_C(1000);
}

static void advance(uint64_t us)
{
    fake_now_us += us;
}

static void *foreign_thread(void *unused)
{
    (void) unused;
    /* A site reached from another thread (the callback presenter) must not
       move time between kinds, even while the owner has one open. */
    unsigned token = work_ledger_enter(WORK_LEDGER_SLEEP);
    advance(50);
    work_ledger_leave(token);
    return NULL;
}

static int test_nested_kinds_are_exclusive(void)
{
    WorkLedgerTotals start, end;
    work_ledger_set_enabled(true);
    work_ledger_snapshot(&start);
    advance(10);                                   /* other */
    unsigned script = work_ledger_enter(WORK_LEDGER_SCRIPT);
    advance(100);                                  /* script */
    unsigned layout = work_ledger_enter(WORK_LEDGER_LAYOUT);
    advance(40);                                   /* layout forced by script */
    unsigned other = work_ledger_enter(WORK_LEDGER_OTHER);
    advance(5);                                    /* a checkpoint inside it */
    work_ledger_leave(other);
    advance(20);                                   /* layout again */
    unsigned nested = work_ledger_enter(WORK_LEDGER_SCRIPT);
    advance(7);                                    /* script the relayout ran */
    work_ledger_leave(nested);
    work_ledger_leave(layout);
    CHECK(work_ledger_current() == WORK_LEDGER_SCRIPT);
    /* Entering the kind already open changes nothing. */
    unsigned same = work_ledger_enter(WORK_LEDGER_SCRIPT);
    advance(3);
    work_ledger_leave(same);
    CHECK(work_ledger_current() == WORK_LEDGER_SCRIPT);
    advance(1);
    work_ledger_leave(script);
    CHECK(work_ledger_current() == WORK_LEDGER_OTHER);
    unsigned sleep = work_ledger_enter(WORK_LEDGER_SLEEP);
    advance(200);
    work_ledger_leave(sleep);
    advance(2);                                    /* other, still open */
    work_ledger_snapshot(&end);
    CHECK(end.us[WORK_LEDGER_OTHER] - start.us[WORK_LEDGER_OTHER] == 17);
    CHECK(end.us[WORK_LEDGER_SCRIPT] - start.us[WORK_LEDGER_SCRIPT] == 111);
    CHECK(end.us[WORK_LEDGER_LAYOUT] - start.us[WORK_LEDGER_LAYOUT] == 60);
    CHECK(end.us[WORK_LEDGER_SLEEP] - start.us[WORK_LEDGER_SLEEP] == 200);
    CHECK(end.us[WORK_LEDGER_NATIVE] == start.us[WORK_LEDGER_NATIVE]);
    return 0;
}

static int test_only_the_enabling_thread_counts(void)
{
    WorkLedgerTotals start, end;
    work_ledger_set_enabled(true);
    work_ledger_snapshot(&start);
    unsigned script = work_ledger_enter(WORK_LEDGER_SCRIPT);
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, foreign_thread, NULL) == 0);
    CHECK(pthread_join(thread, NULL) == 0);
    work_ledger_leave(script);
    work_ledger_snapshot(&end);
    /* The foreign thread's 50 us went on the owner's open kind. */
    CHECK(end.us[WORK_LEDGER_SCRIPT] - start.us[WORK_LEDGER_SCRIPT] == 50);
    CHECK(end.us[WORK_LEDGER_SLEEP] == start.us[WORK_LEDGER_SLEEP]);
    return 0;
}

static int test_disabled_ledger_counts_nothing(void)
{
    WorkLedgerTotals start, end;
    work_ledger_set_enabled(true);
    unsigned open = work_ledger_enter(WORK_LEDGER_LAYOUT);
    work_ledger_set_enabled(false);
    work_ledger_snapshot(&start);
    unsigned token = work_ledger_enter(WORK_LEDGER_SCRIPT);
    advance(500);
    work_ledger_leave(token);
    work_ledger_leave(open);
    work_ledger_snapshot(&end);
    CHECK(memcmp(&start, &end, sizeof(start)) == 0);
    /* Re-enabling starts in bookkeeping: a token from before is inert. */
    work_ledger_set_enabled(true);
    CHECK(work_ledger_current() == WORK_LEDGER_OTHER);
    work_ledger_leave(open);
    CHECK(work_ledger_current() == WORK_LEDGER_OTHER);
    work_ledger_set_enabled(false);
    return 0;
}

int main(void)
{
    TilefinchPlatformServices services = {
        .monotonic_time_ns = fake_clock_ns
    };
    tilefinch_platform_set_services(&services);
    fake_now_us = 1000000;
    CHECK(test_nested_kinds_are_exclusive() == 0);
    CHECK(test_only_the_enabling_thread_counts() == 0);
    CHECK(test_disabled_ledger_counts_nothing() == 0);
    tilefinch_platform_set_services(NULL);
    puts("work ledger tests passed");
    return 0;
}
