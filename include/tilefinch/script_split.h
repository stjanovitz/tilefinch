#ifndef TILEFINCH_SCRIPT_SPLIT_H
#define TILEFINCH_SCRIPT_SPLIT_H

#include <stdbool.h>
#include <stdint.h>

/* Where the time inside page script goes, without the profiler.

   "Script time" in the frame ledger and the load timeline includes the
   synchronous host work script asks for: computed styles, DOM queries,
   forced layout, compiling lazily defined functions, collections. The
   sampling profiler can split it, but it swaps a timing trampoline in for
   every bridge native, so the fastest runs have no split. This ledger keeps
   the native call path as it is: a few bridge natives and host sites time
   themselves (an exclusive nested kind, as in work_ledger.h), and the rest
   (the interpreter, built-ins, every other native) stays SCRIPT_SPLIT_JS.
   JS time is further split by sampling at the VM's interrupt polls (about
   every 10,000 work units): the time since the previous poll is charged to
   the bootstrap when the innermost bytecode frame is a stripped bootstrap
   function, to a native when a C function is polling (a regexp or other
   bounded built-in loop), and to page code otherwise. JS time an entry
   leaves after its last poll stays unattributed. That split is an
   estimate; the timed kinds are exact.

   Compiled in host builds and PSP validation builds; shipping PSP builds
   compile every site to nothing. Off until enabled; only the enabling
   (owner) thread is counted. */
#if !defined(__PSP__) || defined(TILEFINCH_PSP_VALIDATION_LOG)
#define TILEFINCH_SCRIPT_SPLIT 1
#endif

typedef enum {
    SCRIPT_SPLIT_JS = 0,  /* interpreter, built-ins and untimed natives */
    SCRIPT_SPLIT_STYLE,   /* getComputedStyle (__tilefinchComputedStyleGet) */
    SCRIPT_SPLIT_QUERY,   /* querySelector(All), closest, matches */
    SCRIPT_SPLIT_MUTATE,  /* setAttribute, insertBefore, remove, append... */
    SCRIPT_SPLIT_FETCH,   /* fetch() setup (__tilefinchFetchAsync) */
    SCRIPT_SPLIT_LAYOUT,  /* synchronous layout a native forced */
    SCRIPT_SPLIT_COMPILE, /* script, module and lazy function compiles */
    SCRIPT_SPLIT_GC,      /* collections */
    SCRIPT_SPLIT_HOST,    /* UI and input service inside VM polls */
    SCRIPT_SPLIT_KIND_COUNT
} ScriptSplitKind;

/* How the JS kind's time was attributed at polls (an estimate). */
typedef enum {
    SCRIPT_SPLIT_SAMPLE_PAGE = 0,
    SCRIPT_SPLIT_SAMPLE_BOOTSTRAP,
    SCRIPT_SPLIT_SAMPLE_NATIVE,
    SCRIPT_SPLIT_SAMPLE_COUNT
} ScriptSplitSample;

typedef struct {
    uint64_t us[SCRIPT_SPLIT_KIND_COUNT];
    uint64_t calls[SCRIPT_SPLIT_KIND_COUNT];
    uint64_t sampled_us[SCRIPT_SPLIT_SAMPLE_COUNT];
    uint64_t polls;
} ScriptSplitTotals;

#define SCRIPT_SPLIT_FORMAT_BYTES 512u

#ifdef TILEFINCH_SCRIPT_SPLIT
void script_split_set_enabled(bool enabled);
bool script_split_enabled(void);
/* Whether VM polls sample the JS kind (on by default). Off, the timed
   kinds still count and polls attribute nothing: the same binary with and
   without the per-poll frame walk (boot key validation_script_split=1). */
void script_split_set_sampling(bool sampling);
bool script_split_sampling(void);
/* Returns the token script_split_leave needs; entering the open kind is a
   no-op (and does not count a call). */
unsigned script_split_enter(ScriptSplitKind kind);
void script_split_leave(unsigned token);
/* The kind time currently accrues to; SCRIPT_SPLIT_KIND_COUNT outside
   script. */
ScriptSplitKind script_split_current(void);
/* One VM poll: charges the JS time since the previous poll to `sample`. */
void script_split_poll(ScriptSplitSample sample);
/* Totals so far, the open interval included. */
void script_split_snapshot(ScriptSplitTotals *totals);
/* `after` minus `before`, field by field. */
void script_split_difference(const ScriptSplitTotals *after,
                             const ScriptSplitTotals *before,
                             ScriptSplitTotals *difference);
/* "js=... style=... query=... mutate=... fetch=... layout=... compile=...
   gc=... host=... page=... bootstrap=... native=... polls=..." in
   microseconds, then call counts of the timed kinds as "style-calls=..."
   etc. Returns the characters written (truncated to size). */
int script_split_format(const ScriptSplitTotals *totals, char *output,
                        unsigned size);
/* Logs the cumulative totals as `tilefinch-script-split: label=L
   at-us=T ...` through the platform log (the PSP validation log);
   tools/script_split_report.py subtracts consecutive labels. */
void script_split_log(const char *label);
#else
static inline void script_split_set_enabled(bool enabled) { (void) enabled; }
static inline bool script_split_enabled(void) { return false; }
static inline void script_split_set_sampling(bool sampling)
{
    (void) sampling;
}
static inline bool script_split_sampling(void) { return false; }
static inline unsigned script_split_enter(ScriptSplitKind kind)
{
    (void) kind;
    return 0;
}
static inline void script_split_leave(unsigned token) { (void) token; }
static inline ScriptSplitKind script_split_current(void)
{
    return SCRIPT_SPLIT_KIND_COUNT;
}
static inline void script_split_poll(ScriptSplitSample sample)
{
    (void) sample;
}
static inline void script_split_log(const char *label) { (void) label; }
#endif

#endif
