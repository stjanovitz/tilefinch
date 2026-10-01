#include "tilefinch/work_ledger.h"

#ifdef TILEFINCH_WORK_LEDGER

#include "tilefinch/platform.h"

#include <string.h>

#if defined(__PSP__)
#include <pspthreadman.h>
typedef SceUID WorkLedgerThread;
static WorkLedgerThread work_ledger_self(void)
{
    return sceKernelGetThreadId();
}
static bool work_ledger_same_thread(WorkLedgerThread a, WorkLedgerThread b)
{
    return a == b;
}
#else
#include <pthread.h>
typedef pthread_t WorkLedgerThread;
static WorkLedgerThread work_ledger_self(void)
{
    return pthread_self();
}
static bool work_ledger_same_thread(WorkLedgerThread a, WorkLedgerThread b)
{
    return pthread_equal(a, b) != 0;
}
#endif

static struct {
    bool enabled;
    WorkLedgerThread owner;
    unsigned current;
    uint64_t last_us;
    uint64_t us[WORK_LEDGER_KIND_COUNT];
} ledger;

static bool work_ledger_counting(void)
{
    return ledger.enabled
        && work_ledger_same_thread(ledger.owner, work_ledger_self());
}

/* Charge the open interval to the current kind. */
static void work_ledger_close(void)
{
    uint64_t now = tilefinch_platform_monotonic_time_us();
    if (now > ledger.last_us) ledger.us[ledger.current] += now - ledger.last_us;
    ledger.last_us = now;
}

void work_ledger_set_enabled(bool enabled)
{
    if (enabled == ledger.enabled
        && (!enabled
            || work_ledger_same_thread(ledger.owner, work_ledger_self())))
        return;
    ledger.enabled = enabled;
    if (!enabled) return;
    ledger.owner = work_ledger_self();
    ledger.current = WORK_LEDGER_OTHER;
    ledger.last_us = tilefinch_platform_monotonic_time_us();
}

bool work_ledger_enabled(void)
{
    return ledger.enabled;
}

unsigned work_ledger_enter(WorkLedgerKind kind)
{
    if (!work_ledger_counting() || (unsigned) kind >= WORK_LEDGER_KIND_COUNT)
        return WORK_LEDGER_KIND_COUNT;
    unsigned previous = ledger.current;
    if (previous == (unsigned) kind) return previous;
    work_ledger_close();
    ledger.current = (unsigned) kind;
    return previous;
}

void work_ledger_leave(unsigned token)
{
    /* A token from a disabled or foreign-thread enter restores nothing. */
    if (token >= WORK_LEDGER_KIND_COUNT || !work_ledger_counting()) return;
    if (token == ledger.current) return;
    work_ledger_close();
    ledger.current = token;
}

void work_ledger_snapshot(WorkLedgerTotals *totals)
{
    if (totals == NULL) return;
    if (work_ledger_counting()) work_ledger_close();
    memcpy(totals->us, ledger.us, sizeof(totals->us));
}

WorkLedgerKind work_ledger_current(void)
{
    return (WorkLedgerKind) ledger.current;
}

#endif
