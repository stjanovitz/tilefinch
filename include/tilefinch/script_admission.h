#ifndef TILEFINCH_SCRIPT_ADMISSION_H
#define TILEFINCH_SCRIPT_ADMISSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/budget.h"

/*
 * The measured cost of page JavaScript, for memory-based script admission
 * and for the heavy-page estimate shown before a big app starts.
 *
 * The figures come from compiling 84 real bundles (16 KiB to 3.0 MiB, from
 * the October 2026 site census and the m.vk.com capture) once each on a
 * fresh page-configured QuickJS runtime (tilefinch-js-bench --only
 * compile_peak), on the host and, for 25 of them, in the PSP build under
 * PPSSPP at 111 MHz. See docs/engineering/MEMORY_EXPERIMENTS.md, "Script
 * admission by compile working set".
 *
 * - The QuickJS heap peak while one unit compiles is 1.0-11.8 times its
 *   source on the host (median 2.0, 90th percentile 8.8). The PSP's 32-bit
 *   build peaks at 0.82-1.00 of the host figure (median 0.92), so host lab
 *   memory is a slight overestimate of the device.
 * - Source size does not predict where in that range a script falls (no
 *   lexical feature tried correlated above 0.15), so admission plans on the
 *   median and lets the realm's own heap limit decide the rest: a compile
 *   that needs more than the page can give fails alone, as a refusal, and
 *   the realm stays usable (js_rt_compile_source_type).
 * - Compiling a whole file costs 0.9-11.1 ms per KiB of source on a
 *   PSP-3000 at 333 MHz (the same 25 bundles, 2026-10-06), 4.34 ms/KiB over
 *   all 12.1 MiB, median 3.2. The device takes 0.84 of PPSSPP's time at
 *   111 MHz and peaks at exactly the same heap.
 * - Pages compile less than their files (lazy functions compile when they
 *   first run) and run longer than they compile. On the device (2026-10-06,
 *   xe.com, weather.com, m.vk.ru; 7.0 MiB of script in all) compiling took
 *   2.9 s per MiB of script loaded and running it 7.1 s/MiB, from 2.3
 *   (xe.com) to 13.0 (weather.com). Garbage collection near the heap limit
 *   is not in these figures (xe.com: another 25.6 s, an engine pacing
 *   problem rather than a cost of the script), nor is the layout and image
 *   work of the page the scripts build. See
 *   docs/engineering/PERFORMANCE_LEDGER.md, "Device check of the heavy-page
 *   estimate".
 */

/*
 * Memory every admission keeps free. Script admission (script_loader.c),
 * dynamic scripts and fetch() (js_fetch_cors.c), lazy factory compiles
 * (js_lazy_webpack.c) and the compiled-script stores (js_module_loader.c)
 * all take their reserves from here, so recalibrating one moves every path
 * that protects the same thing.
 */

/* The page Budget that stays free for layout and the stylesheet parse
   window whatever scripts want: the stylesheet loader's 2 MiB layout
   reserve (STYLESHEET_LAYOUT_RESERVE, resources.c, which sizes its own
   response bound by parse staging) plus a bounded 1 MiB parse window.
   Script admission, the dynamic-script and fetch() response bounds and
   every compiled-script store keep it. */
#define SCRIPT_ADMISSION_PRESENTATION_RESERVE_BYTES (3u * 1024u * 1024u)

/* Memory kept free beside a compile for the code's own first run: the
   QuickJS heap network staging keeps, the ceiling of the dynamic-script
   heap reserve, the floor of the inline-script one, and the page Budget
   kept beside a lazy factory's compile (its bundle was admitted with the
   presentation reserve; the factory's first call keeps only this). */
#define SCRIPT_ADMISSION_EXECUTION_RESERVE_BYTES (512u * 1024u)

/* The per-script unit before memory-based admission. It still bounds work
   that is not a compile: the inline script text the parser keeps in the
   DOM, the parser stage's spent-work threshold, and the size above which a
   dynamic resource-loader aggregate compiles one registration at a time. */
#define SCRIPT_ADMISSION_LEGACY_UNIT_BYTES (512u * 1024u)

/* Heap kept free beside source-sized work: a fixed floor for parser and
   dispatch state plus `multiplier` times the source, at most `ceiling`
   (SIZE_MAX for none); saturating. These are not compile peaks
   (script_admission_compile_peak is); each multiplier names what its
   reserve is for:
   - STORE: serializing a compiled script into a bytecode table
     (JS_WriteObject's growing buffer and atom table, about four times the
     source), for the module and classic stores, uncapped.
   - DYNAMIC: what a dynamic script keeps beside its planned compile for
     its first run. It reaches the execution reserve (its ceiling) at
     56 KiB of source, so only small late registrations keep less.
   - GC: the pressure watermark below which an external compile may
     collect first (js_rt_compile_source_type), at most 1 MiB. A
     collection heuristic, not an admission. */
#define SCRIPT_ADMISSION_WORK_FLOOR_BYTES (64u * 1024u)
#define SCRIPT_ADMISSION_STORE_HEAP_MULTIPLIER 4u
#define SCRIPT_ADMISSION_DYNAMIC_HEAP_MULTIPLIER 8u
#define SCRIPT_ADMISSION_GC_HEAP_MULTIPLIER 16u
size_t script_admission_work_reserve(size_t source_bytes, size_t multiplier,
                                     size_t ceiling);

/* The bytes of one response the page Budget can stage while the
   presentation reserve stays free, never less than `floor` (a response
   that small is always worth starting; the Budget still refuses what does
   not fit). With `make_room`, optional caches (responses, compiled
   scripts) give way first whenever the Budget is short of the reserve
   plus four floors. */
size_t script_admission_affordable_bytes(Budget *budget, size_t floor,
                                         bool make_room);

/* The planning estimate of one compile unit's QuickJS heap peak: the
   measured median (2.0 times the source) plus fixed parser state. */
size_t script_admission_compile_peak(size_t unit_bytes);

/* The smallest heap a compile of this unit has ever needed (1.0 times the
   source plus a little parser state). A unit whose floor does not fit
   cannot compile; there is no point fetching or trying it. Only units of
   at least SCRIPT_ADMISSION_FLOOR_MINIMUM_UNIT_BYTES are refused on it: a
   small script is left to the heap limit. */
#define SCRIPT_ADMISSION_FLOOR_MINIMUM_UNIT_BYTES (64u * 1024u)
size_t script_admission_compile_floor(size_t unit_bytes);

/* PSP milliseconds to restore `bytes` of source from a bytecode-cache hit
   (about 50 ms per MiB). */
uint32_t script_admission_restore_ms(size_t bytes);

/* PSP milliseconds to read the bytecode of `bytes` of source from the
   Memory Stick tier (Keep compiled scripts), on top of the restore: about
   1 MB/s, the card's documented rate, with bytecode about the size of its
   source (docs/engineering/MEMORY_EXPERIMENTS.md, "Keep compiled
   scripts"). */
uint32_t script_admission_disk_read_ms(size_t bytes);

/* PSP milliseconds a page spends running `bytes` of compiled script to its
   first settled state, without the compile (a bytecode-cache hit): 7.1 s
   per MiB on the device. */
uint32_t script_admission_run_ms(size_t bytes);

/* PSP milliseconds a page spends running `bytes` of freshly loaded script
   to its first settled state, compile included: 10 s per MiB on the
   device (2.9 compiling what runs, 7.1 running it). */
uint32_t script_admission_start_ms(size_t bytes);

/*
 * Heavy pages. A page is heavy when the author script it is loading (what
 * has run since the page committed plus what is waiting to run) would take
 * at least SCRIPT_HEAVY_START_MS of PSP time to start, by
 * script_admission_start_ms(). What the user is offered depends on what
 * the server sent:
 *
 * - CONTENT (a): the page already shows text a reader can use. Scripts run
 *   in the background; reading is never blocked.
 * - SHELL (b): the page shows almost no text (fewer than
 *   SCRIPT_HEAVY_SHELL_TEXT_BYTES visible bytes): it is an empty shell for
 *   an app, so Basic view cannot help and the user is told the cost first.
 * - OVER (c): the waiting scripts cannot fit even in the best case: their
 *   largest compile unit's floor, or their planned memory, is beyond the
 *   heap the realm can still reach.
 */
typedef enum {
    SCRIPT_HEAVY_CLASS_NONE = 0,
    SCRIPT_HEAVY_CLASS_CONTENT,
    SCRIPT_HEAVY_CLASS_SHELL,
    SCRIPT_HEAVY_CLASS_OVER
} ScriptHeavyClass;

/* A dynamic script at least this large is weighed (and, when the user is
   asked, waits for the answer); smaller ones always run. */
#define SCRIPT_HEAVY_UNIT_BYTES (256u * 1024u)
/* 15 s of device script time: about 1.5 MiB of fresh script. The page's
   own layout and image work comes on top (on xe.com and weather.com about
   as much again as the scripts), so a page this heavy takes at least
   twice this long to settle. */
#define SCRIPT_HEAVY_START_MS 15000u
/* An empty shell shows less visible text than this. */
#define SCRIPT_HEAVY_SHELL_TEXT_BYTES 512u

/* Planned QuickJS heap growth once `bytes` of script has run: the compiled
   functions and the data their first run builds. Three times the source:
   m.vk.com's 3.0 MiB application chunk grew the heap by 8.7 MB (2.7 times);
   the census's script-heavy apps settle at 3 to 9 times their source
   (mastodon 2.0 MB of source, 17.7 MB of heap). */
size_t script_admission_run_memory(size_t bytes);

ScriptHeavyClass script_admission_classify(
    size_t visible_text_bytes, uint32_t start_ms,
    size_t largest_unit_bytes, size_t memory_needed, size_t memory_free);

#endif
