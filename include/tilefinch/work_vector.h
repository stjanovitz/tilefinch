#ifndef TILEFINCH_WORK_VECTOR_H
#define TILEFINCH_WORK_VECTOR_H

#include <stdint.h>

/* The deterministic work record,

     tilefinch-work: label=<mark> key=value ...

   printed at PSP input-script marks, by the lab's `work` and `profile`
   commands, and in the lab's final summary (docs/engineering/LAB_USAGE.md,
   "Work vector"). Every value is a plain integer count, or `n/a` for a
   counter this build does not compile; no timings. Identical execution
   gives an identical record, so host iterations compare work, not host
   wall time, and PPSSPP/device runs of the same journey calibrate it.

   Counters are cumulative for the current document: the tallies below and
   the session-wide counters are rebased when a navigation begins
   (navigation_begin), and the js.* fields belong to the committed page
   realm, which a commit replaces. A tool subtracts consecutive marks.

   The tallies are counted by every host build and by PSP validation
   builds; shipping PSP builds compile them (and the record) out. */
#if !defined(__PSP__) || defined(TILEFINCH_PSP_VALIDATION_LOG)
#define TILEFINCH_WORK_COUNTS 1
#endif

/* 32 bits on the PSP, where each tally site is inlined into hot code
   (a per-document count wraps only past 4G); 64 bits on the host. */
#if defined(__PSP__)
typedef uint32_t TilefinchWorkCount;
#else
typedef uint64_t TilefinchWorkCount;
#endif

#define TILEFINCH_WORK_TALLIES 19
typedef union {
    struct {
        TilefinchWorkCount style_resolutions;  /* layout style resolutions */
        TilefinchWorkCount style_cache_hits;   /* ... from the style cache */
        TilefinchWorkCount style_cache_misses;
        TilefinchWorkCount style_rule_queries; /* rule-index lookups */
        TilefinchWorkCount style_rule_candidates;
        TilefinchWorkCount style_variable_lookups;
        TilefinchWorkCount style_variable_cache_hits;
        TilefinchWorkCount style_variable_cache_misses;
        TilefinchWorkCount selector_matches;   /* whole-selector attempts */
        TilefinchWorkCount selector_match_successes;
        TilefinchWorkCount layout_passes;      /* completed layout builds */
        TilefinchWorkCount layout_commands;    /* draw commands built */
        TilefinchWorkCount raster_tiles;       /* tiles rasterized */
        TilefinchWorkCount raster_commands;    /* commands drawn into them */
        TilefinchWorkCount glyph_misses;       /* tile glyph cache misses */
        TilefinchWorkCount frames;             /* frames composed */
        TilefinchWorkCount load_bytes;         /* page-load body bytes */
        TilefinchWorkCount parser_bytes;       /* HTML fed to the parser */
        TilefinchWorkCount css_bytes;          /* CSS text compiled */
    };
    TilefinchWorkCount all[TILEFINCH_WORK_TALLIES];
} TilefinchWorkTally;

#ifdef TILEFINCH_WORK_COUNTS
extern TilefinchWorkTally tilefinch_work_tally;
#define TILEFINCH_WORK_ADD(field, amount) \
    ((void) (tilefinch_work_tally.field += (TilefinchWorkCount) (amount)))
#else
#define TILEFINCH_WORK_ADD(field, amount) ((void) 0)
#endif

typedef struct NavigationSession NavigationSession;

/* Rebase the per-document counters (navigation_begin calls this). */
void tilefinch_work_begin_document(const NavigationSession *session);
/* Print one tilefinch-work record for `label` (no-op when not compiled). */
void tilefinch_work_print(const char *label, const NavigationSession *session);

#endif
