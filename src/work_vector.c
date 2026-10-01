#include "tilefinch/work_vector.h"

#include "tilefinch/fetch.h"
#include "tilefinch/js_runtime.h"
#include "tilefinch/navigation.h"
#include "tilefinch/platform.h"
#include "tilefinch/script_split.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef TILEFINCH_WORK_COUNTS

TilefinchWorkTally tilefinch_work_tally;

/* Session-wide counters as they stood when the document's navigation
   began; the record reports what has been added since. */
static struct {
    uint64_t fast_relayouts;
    uint64_t full_relayouts;
    uint64_t replay_served;
} tilefinch_work_base;

void tilefinch_work_begin_document(const NavigationSession *session)
{
    memset(&tilefinch_work_tally, 0, sizeof(tilefinch_work_tally));
    tilefinch_work_base.fast_relayouts =
        session == NULL ? 0 : session->performance.fast_relayouts;
    tilefinch_work_base.full_relayouts =
        session == NULL ? 0 : session->performance.full_relayouts;
    tilefinch_work_base.replay_served = fetch_trace_replay_served_count();
}

/* Appends " name=value", or " name=n/a" for a counter not compiled.
   Truncation keeps the line well-formed up to the last whole field. */
static size_t tilefinch_work_field(char *line, size_t used, size_t size,
                                   const char *name, uint64_t value,
                                   bool counted)
{
    int written = counted
        ? snprintf(line + used, size - used, " %s=%llu", name,
                   (unsigned long long) value)
        : snprintf(line + used, size - used, " %s=n/a", name);
    if (written < 0 || (size_t) written >= size - used) {
        line[used] = '\0';
        return used;
    }
    return used + (size_t) written;
}

static uint64_t tilefinch_work_since(uint64_t now, uint64_t base)
{
    return now >= base ? now - base : 0;
}

/* Field names are the record's contract (LAB_USAGE.md): append new ones
   rather than renaming. Each table follows its struct's member order. */
static const char *const tilefinch_work_js_names[] = {
    "js.work_units", "js.polls", "js.gc_runs", "js.calls", "js.bytecode_ops",
    "js.float64_boxes", "js.lazy_compiles", "js.lazy_bytes",
    "js.native_calls", "js.attribute_writes", "js.allocs", "js.alloc_bytes",
    "js.source_bytes", "js.module_compiles", "js.module_restores",
    "dom.mutations", "dom.mutation_records", "dom.observer_visits",
};
static const char *const tilefinch_work_tally_names[] = {
    "style.resolutions", "style.cache_hits", "style.cache_misses",
    "style.rule_queries", "style.rule_candidates", "style.var_lookups",
    "style.var_cache_hits", "style.var_cache_misses",
    "style.selector_matches", "style.selector_hits",
    "layout.passes", "layout.commands", "raster.tiles", "raster.commands",
    "raster.glyph_misses", "raster.frames", "fetch.load_bytes",
    "parse.html_bytes", "parse.css_bytes",
};
_Static_assert(sizeof(tilefinch_work_js_names)
               / sizeof(tilefinch_work_js_names[0]) == SCRIPT_WORK_COUNT,
               "js work names follow SCRIPT_WORK_*");
_Static_assert(sizeof(tilefinch_work_tally_names)
               / sizeof(tilefinch_work_tally_names[0])
               == TILEFINCH_WORK_TALLIES
               && sizeof(TilefinchWorkTally)
                  == TILEFINCH_WORK_TALLIES * sizeof(TilefinchWorkCount),
               "tally names follow TilefinchWorkTally");

/* Opt-in (TILEFINCH_JS_OPCODE_HISTOGRAM=1), op-count engines only: the
   cumulative dispatch count of every opcode with a non-zero count, most
   frequent first, as `tilefinch-opcodes: label=L op=NAME count=N`. Turns
   the bytecode-op total into a mix that per-opcode costs can weight. */
static void tilefinch_work_print_opcodes(const char *label,
                                         const ScriptRuntime *runtime)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *value = getenv("TILEFINCH_JS_OPCODE_HISTOGRAM");
        enabled = value != NULL && value[0] != '\0' && value[0] != '0';
    }
    if (!enabled) return;
    const char *names[256];
    uint64_t counts[256];
    size_t entries = script_runtime_opcode_counts(runtime, names, counts, 256);
    unsigned char order[256];
    for (size_t i = 0; i < entries; i++) order[i] = (unsigned char) i;
    /* Insertion sort by count, descending; 256 entries at most. */
    for (size_t i = 1; i < entries; i++) {
        unsigned char at = order[i];
        size_t j = i;
        while (j > 0 && counts[order[j - 1]] < counts[at]) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = at;
    }
    for (size_t i = 0; i < entries; i++) {
        unsigned char op = order[i];
        if (counts[op] == 0) break;
        printf("tilefinch-opcodes: label=%.64s op=%s count=%llu\n",
               label == NULL || label[0] == '\0' ? "-" : label,
               names[op] == NULL ? "?" : names[op],
               (unsigned long long) counts[op]);
    }
}

void tilefinch_work_print(const char *label, const NavigationSession *session)
{
    static char line[1536];
    uint64_t js[SCRIPT_WORK_COUNT];
    const ScriptRuntime *runtime =
        session == NULL ? NULL : session->page.runtime;
    uint32_t missing = script_runtime_work_counters(runtime, js);
    size_t used = (size_t) snprintf(line, sizeof(line),
                                    "tilefinch-work: label=%.64s",
                                    label == NULL || label[0] == '\0'
                                        ? "-" : label);
    for (size_t i = 0; i < SCRIPT_WORK_COUNT; i++)
        used = tilefinch_work_field(line, used, sizeof(line),
                                    tilefinch_work_js_names[i], js[i],
                                    (missing & (1u << i)) == 0);
    for (size_t i = 0; i < TILEFINCH_WORK_TALLIES; i++)
        used = tilefinch_work_field(line, used, sizeof(line),
                                    tilefinch_work_tally_names[i],
                                    tilefinch_work_tally.all[i], true);
    uint64_t extra[4] = {0};
    if (session != NULL) {
        extra[0] = tilefinch_work_since(session->performance.fast_relayouts,
                                        tilefinch_work_base.fast_relayouts);
        extra[1] = tilefinch_work_since(session->performance.full_relayouts,
                                        tilefinch_work_base.full_relayouts);
        extra[2] = session->page.images.stats.encoded_bytes;
    }
    extra[3] = tilefinch_work_since(fetch_trace_replay_served_count(),
                                    tilefinch_work_base.replay_served);
    static const char *const extra_names[4] = {
        "layout.fast_relayouts", "layout.full_relayouts", "fetch.image_bytes",
        "fetch.replay_served",
    };
    for (size_t i = 0; i < 4; i++)
        used = tilefinch_work_field(line, used, sizeof(line), extra_names[i],
                                    extra[i], true);
    /* The most-called profiled natives, as js.native.<name>=<calls> (host
       native names are C identifiers). */
    const char *names[5];
    uint64_t calls[5];
    size_t natives = script_runtime_work_top_natives(runtime, 5, names, calls);
    for (size_t i = 0; i < natives; i++) {
        char name[64];
        snprintf(name, sizeof(name), "js.native.%s", names[i]);
        used = tilefinch_work_field(line, used, sizeof(line), name, calls[i],
                                    true);
    }
    /* Not "%s\n": GCC would turn that into puts, which the PSP build
       does not otherwise link. */
    printf("%.*s\n", (int) used, line);
#ifdef TILEFINCH_SCRIPT_SPLIT
    /* Timings, so beside the record rather than in it: cumulative
       microseconds of script by kind (script_split.h); subtract marks. */
    script_split_log(label);
#endif
    tilefinch_work_print_opcodes(label, runtime);
    static int pool_traffic = -1;
    if (pool_traffic < 0) {
        const char *value = getenv("TILEFINCH_JS_POOL_HISTOGRAM");
        pool_traffic = value != NULL && value[0] != '\0' && value[0] != '0';
    }
    if (pool_traffic)
        script_runtime_report_pool_traffic(runtime, label, stdout);
}

#else

void tilefinch_work_begin_document(const NavigationSession *session)
{
    (void) session;
}

void tilefinch_work_print(const char *label, const NavigationSession *session)
{
    (void) label;
    (void) session;
}

#endif
