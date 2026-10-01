#include "tilefinch/script_split.h"
#include "tilefinch/script_census.h"

#ifdef TILEFINCH_SCRIPT_SPLIT

#include "tilefinch/platform.h"

#include <stdio.h>
#include <string.h>

#if defined(__PSP__)
#include <pspthreadman.h>
typedef SceUID ScriptSplitThread;
static ScriptSplitThread script_split_self(void)
{
    return sceKernelGetThreadId();
}
static bool script_split_same_thread(ScriptSplitThread a, ScriptSplitThread b)
{
    return a == b;
}
#else
#include <pthread.h>
typedef pthread_t ScriptSplitThread;
static ScriptSplitThread script_split_self(void)
{
    return pthread_self();
}
static bool script_split_same_thread(ScriptSplitThread a, ScriptSplitThread b)
{
    return pthread_equal(a, b) != 0;
}
#endif

/* Outside script: time is not counted. It is the kind in force until a
   script entry enters SCRIPT_SPLIT_JS, and the one the outermost leave
   restores. */
#define SCRIPT_SPLIT_OUTSIDE SCRIPT_SPLIT_KIND_COUNT

static struct {
    bool enabled;
    bool no_sampling;
    ScriptSplitThread owner;
    unsigned current;
    uint64_t last_us;
    /* JS time already attributed by a poll. */
    uint64_t js_polled_us;
    ScriptSplitTotals totals;
} split = { .current = SCRIPT_SPLIT_OUTSIDE };

static bool script_split_counting(void)
{
    return split.enabled
        && script_split_same_thread(split.owner, script_split_self());
}

/* The kind transitions and polls: every such site runs page script's own
   work, on the thread that owns the runtime. On the PSP asking the kernel
   for the thread on each of them cost more than the timing (PPSSPP put
   the transitions at 80 ms of a 17 s window), so there only the enabled
   flag is read; the host keeps the check, which its tests exercise. Reads
   from other threads (script_split_snapshot, script_split_log) always
   check. */
static bool script_split_counting_hot(void)
{
#if defined(__PSP__)
    return split.enabled;
#else
    return script_split_counting();
#endif
}

static void script_split_close(void)
{
    uint64_t now = tilefinch_platform_monotonic_time_us();
    if (split.current < SCRIPT_SPLIT_KIND_COUNT && now > split.last_us)
        split.totals.us[split.current] += now - split.last_us;
    split.last_us = now;
}

void script_split_set_enabled(bool enabled)
{
    if (enabled == split.enabled
        && (!enabled
            || script_split_same_thread(split.owner, script_split_self())))
        return;
    split.enabled = enabled;
    if (!enabled) return;
    split.owner = script_split_self();
    split.current = SCRIPT_SPLIT_OUTSIDE;
    script_census_phase(split.current);
    split.last_us = tilefinch_platform_monotonic_time_us();
    split.js_polled_us = split.totals.us[SCRIPT_SPLIT_JS];
}

bool script_split_enabled(void)
{
    return split.enabled;
}

void script_split_set_sampling(bool sampling)
{
    split.no_sampling = !sampling;
}

bool script_split_sampling(void)
{
    return split.enabled && !split.no_sampling;
}

unsigned script_split_enter(ScriptSplitKind kind)
{
    if (!script_split_counting_hot() || (unsigned) kind >= SCRIPT_SPLIT_KIND_COUNT)
        return SCRIPT_SPLIT_KIND_COUNT + 1u;
    unsigned previous = split.current;
    if (previous == (unsigned) kind) return previous;
    /* A timed kind entered outside script (a collection or compile the
       host triggers) is not script time. */
    if (previous == SCRIPT_SPLIT_OUTSIDE && kind != SCRIPT_SPLIT_JS)
        return previous;
    script_split_close();
    /* JS time the previous entry left unpolled stays unattributed rather
       than going to whatever this entry polls first. */
    if (previous == SCRIPT_SPLIT_OUTSIDE)
        split.js_polled_us = split.totals.us[SCRIPT_SPLIT_JS];
    split.current = (unsigned) kind;
    script_census_phase(split.current);
    split.totals.calls[kind]++;
    return previous;
}

void script_split_leave(unsigned token)
{
    /* A token from a disabled or foreign-thread enter restores nothing. */
    if (token > SCRIPT_SPLIT_OUTSIDE || !script_split_counting_hot()) return;
    if (token == split.current) return;
    script_split_close();
    split.current = token;
    script_census_phase(split.current);
}

ScriptSplitKind script_split_current(void)
{
    return split.current < SCRIPT_SPLIT_KIND_COUNT
        ? (ScriptSplitKind) split.current : SCRIPT_SPLIT_KIND_COUNT;
}

void script_split_poll(ScriptSplitSample sample)
{
    if (!script_split_counting_hot() || split.no_sampling
        || (unsigned) sample >= SCRIPT_SPLIT_SAMPLE_COUNT) return;
    if (split.current == SCRIPT_SPLIT_JS) script_split_close();
    uint64_t js = split.totals.us[SCRIPT_SPLIT_JS];
    if (js > split.js_polled_us)
        split.totals.sampled_us[sample] += js - split.js_polled_us;
    split.js_polled_us = js;
    split.totals.polls++;
}

void script_split_snapshot(ScriptSplitTotals *totals)
{
    if (totals == NULL) return;
    if (script_split_counting()) script_split_close();
    *totals = split.totals;
}

void script_split_difference(const ScriptSplitTotals *after,
                             const ScriptSplitTotals *before,
                             ScriptSplitTotals *difference)
{
    if (after == NULL || before == NULL || difference == NULL) return;
    for (unsigned i = 0; i < SCRIPT_SPLIT_KIND_COUNT; i++) {
        difference->us[i] = after->us[i] >= before->us[i]
            ? after->us[i] - before->us[i] : 0;
        difference->calls[i] = after->calls[i] >= before->calls[i]
            ? after->calls[i] - before->calls[i] : 0;
    }
    for (unsigned i = 0; i < SCRIPT_SPLIT_SAMPLE_COUNT; i++) {
        difference->sampled_us[i] =
            after->sampled_us[i] >= before->sampled_us[i]
                ? after->sampled_us[i] - before->sampled_us[i] : 0;
    }
    difference->polls = after->polls >= before->polls
        ? after->polls - before->polls : 0;
}

static const char *const script_split_names[SCRIPT_SPLIT_KIND_COUNT] = {
    "js", "style", "query", "mutate", "fetch", "layout", "compile", "gc",
    "host"
};

int script_split_format(const ScriptSplitTotals *totals, char *output,
                        unsigned size)
{
    if (output == NULL || size == 0) return 0;
    output[0] = '\0';
    if (totals == NULL) return 0;
    unsigned used = 0;
    for (unsigned i = 0; i < SCRIPT_SPLIT_KIND_COUNT && used < size; i++) {
        int written = snprintf(output + used, size - used, "%s%s=%llu",
                               i == 0 ? "" : " ", script_split_names[i],
                               (unsigned long long) totals->us[i]);
        if (written < 0) break;
        used += (unsigned) written;
    }
    if (used < size) {
        int written = snprintf(
            output + used, size - used,
            " page=%llu bootstrap=%llu native=%llu polls=%llu",
            (unsigned long long) totals->sampled_us[SCRIPT_SPLIT_SAMPLE_PAGE],
            (unsigned long long)
                totals->sampled_us[SCRIPT_SPLIT_SAMPLE_BOOTSTRAP],
            (unsigned long long) totals->sampled_us[SCRIPT_SPLIT_SAMPLE_NATIVE],
            (unsigned long long) totals->polls);
        if (written > 0) used += (unsigned) written;
    }
    for (unsigned i = 1; i < SCRIPT_SPLIT_KIND_COUNT && used < size; i++) {
        int written = snprintf(output + used, size - used, " %s-calls=%llu",
                               script_split_names[i],
                               (unsigned long long) totals->calls[i]);
        if (written < 0) break;
        used += (unsigned) written;
    }
    if (used >= size) used = size - 1u;
    return (int) used;
}

void script_split_log(const char *label)
{
    if (!split.enabled) return;
    ScriptSplitTotals totals;
    char text[SCRIPT_SPLIT_FORMAT_BYTES];
    char message[SCRIPT_SPLIT_FORMAT_BYTES + 128u];
    /* Another thread (the live-mark observer) reads the totals as they
       stood at the owner's last transition. */
    script_split_snapshot(&totals);
    (void) script_split_format(&totals, text, sizeof(text));
    snprintf(message, sizeof(message),
             "tilefinch-script-split: label=%.64s at-us=%llu %s",
             label == NULL || label[0] == '\0' ? "-" : label,
             (unsigned long long) tilefinch_platform_monotonic_time_us(),
             text);
    tilefinch_platform_log_message(message);
    script_census_log(label);
}

#endif
