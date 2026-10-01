#ifndef TILEFINCH_WORK_LEDGER_H
#define TILEFINCH_WORK_LEDGER_H

#include <stdbool.h>
#include <stdint.h>

/* Exclusive wall-time ledger of what the browser (owner) thread is doing:
   author script, bridge natives, layout, sleeping, and everything else,
   which is browser bookkeeping (event loop, scheduler, dispatch plumbing,
   hit testing, transport, DOM refresh).

   A site marks the start of a kind with work_ledger_enter and restores the
   enclosing kind with work_ledger_leave; nesting is exclusive, so layout a
   script forces is layout, and script a relayout dispatches is script.
   Time accrues to the innermost open kind. The PSP frame loop snapshots
   the totals at its phase boundaries and prints one tilefinch-loop-work
   record per frame (docs/engineering/INPUT_SCRIPT_HARNESS.md, "Where a
   frame's time goes"); tools/load_timeline.py splits the runtime and input
   phases with it.

   Compiled in host builds and PSP validation builds; shipping PSP builds
   compile every site to nothing. The ledger is off until a caller enables
   it, and only the thread that enabled it is counted: a site reached from
   another thread (the callback presenter, a transport worker) is ignored. */
#if !defined(__PSP__) || defined(TILEFINCH_PSP_VALIDATION_LOG)
#define TILEFINCH_WORK_LEDGER 1
#endif

typedef enum {
    WORK_LEDGER_OTHER = 0, /* browser bookkeeping: the default */
    WORK_LEDGER_SCRIPT,    /* author script: tasks, jobs, handlers, compile */
    WORK_LEDGER_NATIVE,    /* bridge natives called from script (profiler) */
    WORK_LEDGER_LAYOUT,    /* relayout: style, layout, damage */
    WORK_LEDGER_SLEEP,     /* blocked: vblank, delay, transport poll */
    WORK_LEDGER_KIND_COUNT
} WorkLedgerKind;

typedef struct {
    uint64_t us[WORK_LEDGER_KIND_COUNT];
} WorkLedgerTotals;

#ifdef TILEFINCH_WORK_LEDGER
/* Enabling on a thread makes it the counted thread and restarts nothing:
   totals are cumulative, callers subtract snapshots. */
void work_ledger_set_enabled(bool enabled);
bool work_ledger_enabled(void);
/* Returns the token work_ledger_leave needs to restore the enclosing
   kind. Both are cheap no-ops while the ledger is off. */
unsigned work_ledger_enter(WorkLedgerKind kind);
void work_ledger_leave(unsigned token);
/* Totals so far, the open interval included. */
void work_ledger_snapshot(WorkLedgerTotals *totals);
/* The kind time currently accrues to. */
WorkLedgerKind work_ledger_current(void);
#else
static inline void work_ledger_set_enabled(bool enabled) { (void) enabled; }
static inline bool work_ledger_enabled(void) { return false; }
static inline unsigned work_ledger_enter(WorkLedgerKind kind)
{
    (void) kind;
    return 0;
}
static inline void work_ledger_leave(unsigned token) { (void) token; }
static inline void work_ledger_snapshot(WorkLedgerTotals *totals)
{
    if (totals != 0)
        for (unsigned i = 0; i < WORK_LEDGER_KIND_COUNT; i++)
            totals->us[i] = 0;
}
static inline WorkLedgerKind work_ledger_current(void)
{
    return WORK_LEDGER_OTHER;
}
#endif

#endif
