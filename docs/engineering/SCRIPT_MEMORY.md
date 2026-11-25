# Script admission and source memory

This page is for engineers changing how page scripts are admitted, compiled,
and kept in memory: the admission rules and the parser-stage circuit breaker,
when a realm that runs out of memory is stopped, compiled-script reuse, lazy
webpack bundles, and the function-source store.
The rules are summarized in
[Architecture](../ARCHITECTURE.md#script-admission); this page holds the
mechanism. The memory profiles and the realm's heap are in
[PSP envelope](PSP_ENVELOPE.md#memory-profiles), the on-stick caches in
[Storage](../STORAGE.md#compiled-script-caches), and the A/B switches named
here in [Diagnostic switches](../DIAGNOSTIC_SWITCHES.md).

## Admission

Page scripts are admitted by source size, count, heap, time, and callback
work, and interrupt checks cover QuickJS execution and native callback
boundaries. The PSP app admits a script by the memory its compile needs
rather than by its size (`include/tilefinch/script_admission.h`).

- **Blocking scripts stay synchronous.** Parser-blocking classic scripts run
  synchronously and in source order, including `document.write()` insertion
  checkpoints. Their cumulative source work draws on the live realm's finite
  source allowance (clamped by the realm, raised by memory-based realm growth
  only within its hard ceiling), and fetched or validated bodies count even if
  execution fails. This is bounded synchronous execution with cooperative
  checkpoints, not resumable evaluation or a worker thread.
- **Size is not suspicion.** Large cross-origin classic scripts are treated
  like same-origin application bundles: size, loops, or dynamic-code tokens
  alone do not prove they should be refused. A lexical scan of their content
  is watchdog telemetry, not an admission oracle; per-file and count limits,
  working-set and presentation-reserve admission, the shared heap, and the
  compile/execution watchdog decide.
- **A compile that cannot fit is refused before it runs.** A compile unit (a
  whole script, or the largest factory or statement of a bundle the browser
  splits) is admitted when twice its source plus 256 KiB
  (`script_admission_compile_peak`) fits the QuickJS heap the realm can still
  grow to while the page keeps a 3 MiB presentation reserve
  (`SCRIPT_ADMISSION_PRESENTATION_RESERVE_BYTES`). That heap is what a compile
  can really reach (`script_runtime_heap_reachable`): the heap left under the
  realm's limit and the growth the free page Budget can back, which come out
  of the same free Budget and so are not added; an inline compile's copy of
  its text is taken from the Budget first when it needs one. Every script
  compile (inline, external, module, module import, inserted) is admitted
  this way, so one that cannot compile is refused before compiling, not after
  the compile runs out of memory: a document script is counted as memory
  pressure (the census tag `script-memory-pressure`), and an inserted one
  fails with an `error` event. A compile that needs more than planned is
  stopped by the heap limit and refused alone.
- **Room is reclaimed before a refusal.** A compile unit (a whole script,
  module or factory) the heap or Budget cannot admit gets one collection,
  then the releases heap growth would make before it refuses
  (`script_runtime_reclaim_for_admission`), as much as the unit lacks:
  optional caches, the cached bodies of classic scripts, and unconsumed
  preload responses, and, only when the Budget itself is short, stored
  function text no uncompiled function needs. A heap-only shortfall never
  costs stored text, so `toString()` keeps the text. Nothing is released
  when even all of it (`script_runtime_admission_reclaimable`) could not
  admit the unit, so a unit refused anyway leaves the caches alone. The body
  of a running script is kept. The admission of an inline script taken as
  data (the fast path that parses it without compiling) does not reclaim.
- **Doomed inline scripts are dropped while they stream.** For an inline
  classic script the parser asks the same rule as the text arrives
  (`script_inline_source_admissible`, through
  `document_parser_set_inline_script_admit_callback`) and drops the text of a
  script once it is too large to admit, since more text only needs more; the
  element is then empty and the refusal is reported as pressure.
- **An inserted script keeps the stop rule's headroom.** When the compile
  unit of a script a page inserts is at least 256 KiB
  (`SCRIPT_HEAVY_UNIT_BYTES`), the script is compiled only when the heap the
  realm can obtain (`script_runtime_heap_obtainable`) would still be 2 MiB
  after that compile (`SCRIPT_ADMISSION_MEMORY_STOP_HEADROOM_BYTES`, the
  threshold of [Exhaustion](#exhaustion)); otherwise it fails with an `error`
  event before compiling. Admitted into less, any refusal in its first run
  would stop the realm and every script the page already runs. The unit is
  the compile being admitted: the whole script, a lazy webpack bundle's
  largest factory, or a resource-loader aggregate's current statement. A lazy
  bundle's other factories compile at first call under their own admission,
  so a large bundle of small factories is not held to the headroom. Before
  refusing, the realm is collected unless it has allocated nothing since its
  last collection, and the session's cached responses give up the page Budget
  still short, only when more Budget would raise what the realm obtains and
  the cache could cover it (`js_rt_reclaim_cached_responses_for_heap`).
  Smaller units keep the reserve that scales with their source, and a
  continued classic script (one registration of a split aggregate) keeps
  only the work floor.
- **A large unit needs room for the expensive case.** Only the source size is
  known before a compile, and units of 1 MiB or more compile either near their
  source size or at several times it. Such a unit is compiled only when the
  heap it can obtain at that moment (`script_runtime_heap_obtainable`) is at
  least 4 times its source (`script_admission_large_unit_heap`), so a compile
  that would fail after seconds of PSP time is refused instead; a bytecode
  restore keeps only the floor.
- **Size ceilings are for hostile input.** A script may be up to 4 MiB
  (`BROWSER_PSP_APP_SCRIPT_FILE_KB`) and a page's scripts up to 16 MiB in all
  (`BROWSER_PSP_APP_SCRIPT_TOTAL_MB`); both are sanity ceilings, not
  estimates of what a page can compile.
- **Inline text is compiled in place.** A script of one text child (every
  parsed script) compiles from the DOM's own NUL-terminated text, so its
  compile needs no second copy of the source; nothing can change or free that
  text before the compile returns, and QuickJS keeps no pointer into it after.
  A script of several text children (built by DOM calls) is assembled into a
  copy, which its admission counts.
- **Large raw text is handed over, not copied.** The tokenizer gathers a
  script's or style's whole text in its own buffer before the tree builder
  sees it. For a parsed `<script>` or `<style>` of at least 64 KiB, the parser
  (`document_parser_adopt_raw_text` in `src/document.c`) gives that buffer to
  the document's text arena as the node's storage (`lexbor_mraw_adopt`, a
  Tilefinch Lexbor patch) and gives the tokenizer a fresh small buffer, so the
  text exists once instead of twice while the script waits to compile.
  Shorter text, text the tokenizer has already handed back to the network
  buffer, and any refused step take Lexbor's ordinary copy, which produces the
  same node.
- **Data, not code.** Inert JSON script types stay DOM data. A narrow
  classic-script subset (`window`, `globalThis`, `var`, or bare global
  assignments of strict JSON) installs its data without compiling bytecode or
  consuming executable quota.
- **No `nomodule` fallback.** Because the engine supports modules, classic
  `nomodule` scripts are rejected before preload, quota admission, or
  dynamic-script scheduling, and a module failure never enables the legacy
  alternative.
- **Hydration rollback.** Before the first parser-blocking author callback,
  the stream keeps at most one complete, visibly useful snapshot of the body.
  If later hydration fails or runs out of quota and collapses that content,
  commit restores the snapshot transactionally; a useful replacement that
  succeeds stays authoritative.

## Circuit breaker

One navigation's parser stage may spend at most the realm's source allowance
in source work, twenty seconds across its blocking checkpoints (the PSP shows
"PAGE SCRIPTS NS OF 20S" once a load has spent three seconds), or three
consecutive scripts that fail in a way that says the realm is in trouble: out
of memory, a watchdog interrupt, a compile admission refusal, a refused
mutation checkpoint, or another engine failure.

A script's own exception (a throw, or a SyntaxError) goes to the window
`error` event and the page goes on, as in Chromium, which has no such
breaker: it neither counts nor resets the run (a healthy script resets it),
and the time and work bounds already cap what failing scripts cost.
Transport, CSP, SRI and memory-admission skips never reached the evaluator and
do not count either.

When a bound trips, the remaining blocking scripts are soft-skipped, parsing
continues, the page is marked Limited, and the author realm is retired for
that document so queued deferred, asynchronous, and dynamic work cannot resume
the abandoned hydration (no element event fires for the skipped scripts: no
realm is left to observe one). The validation log and the host's
`TILEFINCH_TRACE_SCRIPT_FAILURES` trace print a `tilefinch-script-breaker`
line with the reason; the lab's parser-checkpoint line counts the author
exceptions the breaker left uncounted (`author-exceptions=`).

## Exhaustion

A realm whose allocation was refused is judged by the heap it can still
obtain, never by how the refusal reached the page
(`navigation_assess_script_memory`, `src/navigation/page_lifecycle.inc`).

- **The error is not evidence.** QuickJS throws a catchable
  `InternalError: out of memory` at the first refused allocation and does not
  collect and retry. Whether that becomes a caught exception, a promise
  rejection, a listener's reported error, a failed timer or a failed host
  call depends on which operation needed the next 4 KB arena, and any byte of
  heap in front of it moves that. A page can also throw the text itself.
- **When.** At each author-task boundary after a refusal: the document's
  scripts at commit, each runtime turn (before page script runs and after),
  input dispatch, stylesheet and frame events, and cross-frame message
  delivery. A refusal counts whether the realm's heap limit or the page
  Budget refused (`script_runtime_allocation_refusals`). So does the page
  Budget refusing the host envelope the realm needs to take a completed
  response (an inserted script's, a `fetch()`'s or an event stream's): the
  response stays queued for a later turn, and the turn itself succeeds.
- **Measure.** The realm is collected and trimmed. When it can obtain less
  than it needs to be kept, optional page memory is reclaimed only if freeing
  it could meet that threshold without exceeding the heap ceiling. This includes
  optional caches, compiled scripts' bodies, unconsumed preloads, and stored
  function text (`script_runtime_reclaim_for_admission`), then the session's
  other cached responses (`script_runtime_reclaim_heap_headroom`). The failed
  task's own memory is already free: its body goes with its task or with the
  cached script bodies, its garbage with the collection. What it can obtain
  is `script_runtime_heap_obtainable`: headroom under its limit only as far as
  the Budget backs it, plus the growth the growth hook would still grant.
- **Stop under 2 MiB.** If the page discarded its readable body after
  the refusal, restore the saved body when a useful snapshot is available,
  even if the script caught the exception. Otherwise keep the live body.
  The realm stops,
  the summary reads "JavaScript stopped at its memory limit", the
  static-content cascade applies, the page qualifies for
  [Basic view](../BASIC_VIEW.md#automatic-fallback), and the site is
  noted for opening in Basic view next time.
- **Keep otherwise.** The realm runs on and the exception is an ordinary
  author error; a turn that failed only because of it counts as a completed
  turn. A turn the watchdog stopped for time is still a failure.
- **Guard.** A realm kept three times with under 4 MiB to obtain stops at the
  third: it is living at its limit.
- **Child frames** are judged the same way on their own counts; stopping one
  leaves the page and its other frames running.
- **A challenge frame goes first.** While a frame of a managed challenge
  runs (the frame `navigation_script_heap_limit` raises) and under a quarter
  of the page Budget is free, each other realm of the page whose heap has
  grown an amortized step since its last collection is collected before the
  frame's turn and returns its cached pool blocks
  (`navigation_collect_for_challenge_frame`). An idle parent's own pacing
  can leave its garbage in place for megabytes.

A layout or style rebuild the Budget refuses stops the page's realms to give
their memory to the page; that is the page's need, not this rule. A page
with a few hundred bytes to spare still depends on whether it is ever
refused, and a stop costs one static relayout.

The validation log and the host's `TILEFINCH_TRACE_SCRIPT_FAILURES` trace
print a `tilefinch-script-memory-assess` line for each assessment: where it
ran, the verdict, the new refusals, and the obtainable, available and used
heap.

## Compiled-script reuse

ES modules and classic scripts each have a bounded session table of bytecode
keyed by the response, the top-level site, compile options, and the SHA-256 of
the exact source, consulted only after every fetch check and evicted by the
page Budget before it refuses any allocation; an opt-in tier keeps the records
on the Memory Stick across launches
([Storage](../STORAGE.md#compiled-script-caches)). Modules of 8 KiB or more
are compiled without their source text, so their functions' `toString()`
returns the native-code form. The only other compiler artifact written to
storage belongs to an installed offline app: bytecode bound to its retained
source and to a dedicated QuickJS bytecode ABI, with launch compiling the
source whenever restoration fails.

## Lazy webpack bundles

A classic script shaped like a webpack chunk registration
(`(self.webpackChunkX=self.webpackChunkX||[]).push([[ids],{key:factory,...}])`,
whose factories are most of its bytes) is not compiled whole. The planner
(`src/script_lazy.c`) finds the factories with a bounded lexer pass;
registration replaces each with a small native trampoline and keeps its exact
source compressed (`src/js_lazy_webpack.c`); a factory is compiled the first
time it runs and its bytecode is dropped after the call, so the many factories
a page never runs cost no compile. The functions a factory leaves behind read
their text from that compressed copy rather than keeping their own in the heap
([factory text](#factory-text)). Anything the planner does not take, and any
bundle whose plan fails or whose registration does not compile, goes through
the ordinary eager path, one compile of the whole script. A bundle record
([Storage](../STORAGE.md#compiled-script-caches)) lets a later load of the same
bytes skip the planner.

### Syntax errors: a deliberate deviation

A factory's syntax is checked when it first runs, not before. An error in a
factory that never runs is never reported; one in a factory that runs makes
that call (webpack's `require`) throw a `SyntaxError` naming the bundle's URL
and the error's position, which reaches the page's error reporting like any
exception, and the other factories, some of which may already have run, keep
working. Chrome would have rejected the whole file.

The justification is cost against risk: checking every factory up front is
most of a lazy bundle's load cost, mostly for factories that never run, and a
bundle that does not parse is broken in every browser. The planner, whose
wrong cut could turn valid code into a runtime `SyntaxError`, is checked token
for token against the census corpora and by `tests/test_script_lazy.c`, and a
plan that does not cover the whole registration takes the eager path.
`TILEFINCH_LAZY_WEBPACK_PREFLIGHT=1` restores the up-front check for A/B
measurement.

## Function source outside the heap

QuickJS keeps every function's source text for
`Function.prototype.toString()`, and a lazily compiled function compiles from
it on its first call; for a large script that is a second copy of the script
in the realm heap. A page script of at least 8 KiB (inline, external, or a
module) therefore compiles with `JS_STRIP_SOURCE`, and its text goes to the
realm's function-source store (`src/js_source_store.c`) in the page Budget,
counted as JavaScript memory. Each function records only where its text is
(`JS_SetExternalSourceHooks`,
[Vendored QuickJS](../../third_party/quickjs/README.md)). `toString()` returns
the same text a heap copy would, a lazy function reads its body from the store
on its first call, and stacks keep file:line:col. A module whose text the
store cannot take is still stripped and keeps no text: its functions'
`toString()` returns the `[native code]` form. Smaller scripts, eval,
`Function`, and worker scripts keep their text in the heap.

### Factory text

A lazy-webpack factory of 1 KiB or more compiles the same way, but its text
does not go to the store: the lazy tier already keeps it compressed, for as
long as the bundle lives. Its functions read a plain copy of it (the compile's
own program text, kept) or, once that copy is gone, decode the compressed copy
from its start up to what they need into a new plain copy. Plain factory text
follows the store's plain-text rule below (it counts against the same quarter
of the room, after the store's own plain text) and is the first to go when the
realm's growth would be refused, since dropping it costs no compression. The
text itself is never dropped, so a factory's `toString()` never falls back to
`[native code]`.

### Store rules

- **Plain first, compressed under pressure.** The store holds 16 KiB blocks,
  plain while the page has room. Once plain text passes a quarter of the room
  above the realm's growth reserve, the oldest blocks are compressed with the
  LZ4-compatible codec shared with the lazy-webpack tier
  (`src/script_text_lz.c`); a read decodes only the start of the block it
  needs and keeps it for the next read. A block that does not compress stays
  plain.
- **Never the reason a page runs out of memory.** When growth would be
  refused, cached script bodies give way first, then plain text is
  compressed, then whole scripts that no uncompiled lazy function needs are
  dropped, least recently read first. Text an uncompiled lazy function needs,
  or a compile or bytecode restore holds a lease on, is never dropped.
  `toString()` of a function whose text was dropped returns the
  `[native code]` form without allocating.
- **Admission sees the text as if it were in the heap.** The compile floor,
  the loader's pressure gate, and the heavy-page estimate subtract the stored
  text, and factory text read from the lazy tier, from their room, so a page
  admits the same scripts and the freed room goes to the scripts it already
  runs.
- **A failed lazy compile is bounded.** A lazily compiled function that cannot
  compile at the wall fails its call uncatchably and stays uncompiled; inside
  a promise reaction the checkpoint ends at that job and the rest of the queue
  runs on a later turn. Code that catches the out-of-memory error and retries
  is stopped once one entry into JavaScript has had 256 allocations refused.
  Both stops stay uncatchable when the error's stack text cannot be
  allocated: the error keeps a null `stack` (QuickJS layer
  `backtrace-pending-exception`).
- **The bytecode cache stores offsets, not text.** A restore names the script
  text (its digest is the cache key) and the store copies it only if restored
  functions refer to it, before the script runs. Bytecode whose lazy functions
  would have no text does not restore; the script compiles from source. A
  module record whose functions read the store is keyed apart from one that
  carries its text, and a module restore whose text the store refuses is
  dropped and the module compiled from source.

### Measuring

`TILEFINCH_JS_SOURCE_STORE` (lab builds) selects other policies for A/B
measurement, `TILEFINCH_JS_MODULE_SOURCE_STORE=0` strips module text with no
store copy, `TILEFINCH_LAZY_WEBPACK_FACTORY_TEXT=heap` keeps factory text in
the heap, `tilefinch-js-bench --only source_store,tostring` measures the
copy, encode, compile, and `toString()` costs, and the census prints a
`census source-store` line per realm.
