# Memory optimization experiment ledger

Read this before repeating an allocator, source-retention or navigation-memory
experiment. Keep rejected approaches rejected until the stated premise changes.
Record an exact source baseline, a complete-work acceptance check, ownership at
teardown and comparable timing—not just a lower allocation number. Raw response
captures, browsing journeys and detailed allocation logs stay outside the public
repository. Measurements below are optimized 64-bit host results, not PSP costs.

## Not built: a slab allocator for the pool's small classes (2026-09-28)

Proposed: carve the QuickJS pool's small classes (16-64 bytes) from
header-less 16-64 KiB slabs, on the premise that every QuickJS allocation
was its own Budget allocation (about 56 bytes of pool, Budget and newlib
headers on 24-48 byte objects, plus a cold list-neighbour write per
alloc/free) and that this is why allocation-heavy kernels run at 2.3-3.9x
their PPSSPP time on the device.

The premise is false. The vendored engine allocates every block of up to
504 bytes from its own 4 KiB arenas (`JSMallocArena` in quickjs.c: fixed
block classes, LIFO free list per arena, empty arenas returned at once);
their 8-byte block header is the reference count and GC tag, not
allocator overhead. The pool sees only arenas (its 4096 class) and larger
blocks. `TILEFINCH_JS_POOL_HISTOGRAM=1` on the chatgpt-ask send76 lab
replay (lab execution profile, seed 42, 64-bit host), whole run:

| Pool class | Blocks handed out | Fresh from Budget |
|---|---:|---:|
| 16-512 | 0 | 0 |
| 640-3,072 | 75,902 | 8,166 |
| 4,096 (arenas) | 18,411 | 4,028 |
| 5,120-16,384 and direct | 2,555 | 380 |

A temporary engine census (not committed) counted 3.56 million
small-block allocations over the same run: 1.13M in the load, 0.51M load
-> usable, 1.64M in the send window, served from 16,993 arena creations.
The engine's arenas are already the slab allocator proposed here; a pool
slab would receive no traffic. `tilefinch-js-bench` agrees: closure_create,
alloc_arrays, alloc_objects_hot, number_to_string, regexp and spread_args
make zero pool allocations per timed loop, on the host and under PPSSPP,
and the pool and plain-malloc runs matched within 1% on the device.

Live at first usable input (post-collection census, host): 5,159 pool
blocks, 15.2 MB requested; 2,909 arenas with 10.28 of 11.65 MB block
capacity in use (88%); pool and Budget headers 64 bytes per pool block on
the host, 56 on the PSP (about 1.4% of an arena). No change was made.

Revisit only if the engine is built with `JS_MALLOC_LARGE_BLOCKS_ONLY` or
its arena allocator is removed. (quickjs.c sets that only when
`__SANITIZE_ADDRESS__` is defined, which GCC does and Apple clang does not:
the host sanitize preset keeps the arenas too, with zero pool traffic in
the small classes, so AddressSanitizer does not see inside them.) A per-object allocator
experiment belongs in quickjs.c's arena code, and needs device evidence
that data misses are the cost first: the device CPI pattern points at the
instruction side (see the performance ledger, 2026-09-28).

## Experimental only: exact-sized native plan maps (2026-09-27)

The isolated native-tier prototype's fixed plan costs 20,616 bytes per
function even for tiny bodies. An exact-sized entry map and compile-only
branch-boundary scratch reduce the mixed fixture to 308 bytes (16-bit map)
or 472 bytes (32-bit map). Applied to the already captured hottest 16 bodies,
metadata falls from 329,856 to 18,730 / 35,156 bytes. Compilation scratch,
executable mappings and plan storage remain Budget/JS-heap charged; refusal
and teardown return ownership to baseline. No production allocator changes.

Do not describe this as accepted with no performance tradeoff: the getter
fixture slows down under both compact layouts, despite essentially unchanged
own-data timing. The width control rules out blaming narrowing alone; mapped
samples do not resolve the remaining difference. Fixed and compact explicit
targets remain for comparison. The next cache experiment must include that
fallback cost and executable-page padding, rather than raising the ceiling
or turning instruction coverage into a time-savings claim. Details and raw
evidence are retained privately; see [the summary](NATIVE_TIER_INVESTIGATION.md)
for the decision and measurement limits.

## Accepted: batch-local image-refresh ancestor memo (2026-09-27)

Multi-resource refresh repeatedly resolved the same root-to-parent style
prefix. A scratch-only memo now retains at most eight prefix entries during
one synchronous multi-node refresh on a large stylesheet. It is statically
bounded to 4 KiB (3,072 bytes measured on host), Budget charged, released on
every scope exit, and skipped on allocation refusal or single-node refresh.
Nothing is retained by the page after the refresh. Document, stylesheet and
container-generation checks prevent reuse across changed style inputs.

Matched host refresh samples fell from 5.206 to 4.229 ms (18.8%); whole-runtime
timings were noisy, so there is no end-to-end claim. Tests pin prefix divergence,
new-batch inherited-color changes, authoritative pixel parity, 32 refusal points,
unchanged resident ownership and zero teardown ownership. See the dated
[performance entry](PERFORMANCE_LEDGER.md) for replay comparisons. Do not grow
this cache or extend its lifetime without new measured evidence.

### Reuse the existing layout cache, not a larger memo (2026-09-30)

Selected refresh now shares the owner's mutation-invalidated retained style
cache when available, just as complete image discovery already does. This
removes repeated deep ancestry resolution without enlarging the eight-entry
scratch memo or retaining new page allocations; that memo remains the bounded
fallback for uncached embedders. The six-icon host refresh improves 51.5–62.4%
with unchanged authoritative pixels, 32 refusal-point ownership checks and
zero-owned teardown. Additive-only scans are unchanged; there is no device or
end-to-end claim. See the dated performance entry for attribution and limits.

## Accepted: lazy function compilation

Premise, measured with a temporarily instrumented engine on the
chatgpt-send35 replay (module bytecode cache off, so both documents compile
their whole graph): QuickJS emits bytecode for every function body when a
script compiles, and most of those bodies never run. The second document
held 2.05 MB of page function bytecode (with line tables) for about 5,400
functions; 919 of them (17%) were ever called. The 1,774 uncalled functions
whose parent had run held 1.42 MB (69%) of that bytecode in their subtrees,
for 602 KB of their own source text: bytecode is about 2.4 times the size of
the minified text it comes from. Host compile time (both documents) split
into parse and emission 46%, variable resolution 23%, label resolution and
peephole optimization 23%, stack sizing 6%, object creation 2%.

Design (`third_party/quickjs/README.md`, item 17). A true pre-parser that
skips emitting a body (V8's approach) would save the 46% parse share too,
but QuickJS's parser reads back the bytecode it has emitted (assignment
targets are recognised from the last opcode), so skipping emission means a
second grammar with its own copy of every early-error rule, and closure
resolution done without QuickJS's resolver. Instead the whole script is
still parsed, so early errors stay compile-time errors, and a deferred
function's variables (and those of the functions nested in it) are still
resolved by `resolve_variables`, which is what decides its closure
variables and what its enclosing functions capture. Only the later passes
and the bytecode object are skipped. The stub keeps its closure variables,
name, length, flags, definition position and text; on the first call the
body is parsed again alone under a synthetic parent that offers the stub's
closure variables by name (QuickJS's direct-eval mechanism) and so compiles
to the same closure layout, and is moved into the stub every existing
closure shares. Functions near a direct eval or inside `with`, class
constructors, field initializers and static blocks stay eager. Page scripts
and modules defer functions whose text is at least 128 bytes
(`TILEFINCH_JS_LAZY_FUNCTIONS`); eval and Function bodies are always eager.

Source retention. With retained source the `toString()` text doubles as the
compile text, so deferral costs no text. A module stripped by the policy
above keeps a private copy of each deferred function's text (about 0.4 of
the bytecode it replaces, freed when the function compiles); a function
whose text is longer than its unresolved bytecode (comments, whitespace)
stays eager, so the stripping policy still holds for it when it is never
called. `toString()` of a stripped module's functions keeps the native
form before and after the first call.

Module bytecode cache. Stubs serialize as stubs (a flag bit in the function
tag; format 6 is unchanged for eagerly compiled functions) and a restored
stub compiles on its first call. Compiling every deferred body before
storing a module was rejected: that would put the whole compile back into
the first load and restore full bytecode on every later one.

chatgpt.com, the earlier `capture` replay (the send journey without a
module bytecode cache): the module graph (58 modules on the second document)
is identical in every variant, and the memory figures were identical in all
three runs of each.

| Threshold | 0 (eager) | 64 | 128 (default) | 256 |
|---|---:|---:|---:|---:|
| JS malloc after load | 7,831,243 B | 6,883,342 B | 6,924,333 B | 7,067,399 B |
| JS retained (census) | 5,551,620 B | 4,741,311 B | 4,796,497 B | 4,888,612 B |
| Whole-Budget peak | 19,745,826 B | 18,972,887 B | 19,004,569 B | 19,065,625 B |
| JS malloc peak | 7,977,415 B | 7,991,657 B | 8,010,249 B | 8,063,647 B |
| Page compile time (host) | 164.3-165.2 ms | 132.5 ms | 136.8-138.1 ms | 137.8 ms |
| Script run time incl. first-call compiles | 135.6-137.8 ms | 128.6 ms | 128.7-129.4 ms | 130.8 ms |
| Functions deferred / compiled on first call | 0 / 0 | 2,935 / 561 | 2,048 / 403 | 1,167 / 255 |

The settled heap falls 0.91 MB (11.6%) and the whole-Budget peak 0.74 MB.
The JS malloc peak does not fall: it is set by when automatic GC runs, not
by the live graph. Compile time falls 16% and run time, which now includes
every first-call compile, 5% (less heap for the collector to trace). 128
bytes gives up 41 KB against 64 for 30% fewer first-call compiles, each of
which has a fixed cost on the device.

With the cache on and the page loaded twice (`--reload 1`), the second load
restores the same 9 modules in both variants, but their bytes fall from
418,981 to 326,958 (restore 1,246 us to 427 us), and the cache ends holding
52 modules in 1,025,091 bytes instead of 18 in 1,048,250: 43 stores where
eager compilation skipped 35 for lack of heap headroom. A later load would
restore most of the graph. Classic scripts' cached bytecode fell from
253,872 to 105,268 bytes.

chatgpt-send35 (send journey, cache off): the first document compiles the
same 9 modules in both variants; its heap falls from 5,489,087 to 5,164,421
bytes and its compile time from 40.0 to 33.2 ms. The second document's
graph is not stable in this capture: module prefetches expire on wall-clock
time, so the replay serves different response occurrences as timing
changes (two eager runs loaded 70 and 80 modules).
Lazy runs consistently get further (87-98 modules); with 18 more modules the
whole-Budget peak was still 32,030,234 bytes against 33,097,181.

Costs and limits. A function's first call now pays a compile of its body
(included in the run times above), and needs more native stack than the
call alone: a function first called near the stack limit fails with the
same stack-overflow error a deeper call would, and a watchdog interrupt
that lands in a first-call compile fails the call as an interrupted
execution does (uncatchable); the function stays lazy and a later call
compiles it. Implementation limits that are compile-time errors for eager
functions (for example a stack size beyond 65,535) surface at the first
call. Host timings are not PSP costs and this has not run on the device;
the device check is `TILEFINCH_JS_LAZY_FUNCTIONS=0` against the default on
a validation build, comparing the load advance and the page heap.

`tilefinch-quickjs-lazy-function-tests` runs every case lazily and eagerly
and requires identical results, deferral counts and exact Budget return;
`tilefinch-module-bytecode-tests` covers page modules (deferral and
first-call compile, stripped `toString()`, stack positions, eval staying
eager, the comment-heavy guard). Each fails with its mechanism removed.

## Rejected: subtree-only JavaScript variable-cache invalidation (2026-09-27)

At `ee7f27b4`, a temporary prototype preserved unrelated `var()` cache entries
when the computed-style cache invalidated only a mutated subtree. It retained
the existing capacity and full invalidation for entry epochs, stylesheet
changes and relational-selector changes. Deleted entries became tombstones so
bounded open-addressed lookups could still reach colliding keys.

On the same optimized host replay, send-side variable misses moved only from
7,172 to 7,008 (2.3%), while total native time was 46 versus 48 ms and sampled
JavaScript 293 versus 320 ms. These single-run timings do not establish a
regression, but provide no speedup to justify the extra invalidation logic.
The replay did not reproduce the captured server reply, so it is not an
end-to-end response-time measurement. The prototype and timing probes were
removed; no cache or runtime behavior changed.

The per-property probe instead found repeated cold 20–23-element ancestor
cascades: visibility reads accounted for about 20 ms of 30 ms of native
computed-style work in that replay. Synchronous layout flushing inside those
reads was negligible. Reconsider subtree variable-cache retention only if a
complete-work profile shows scoped invalidation, rather than cold entry
epochs or necessary cascade work, dominating misses.

## Rejected: enlarge the JavaScript custom-property cache (2026-09-26)

At `4c9b5d84`, the captured send replay was compared with the existing
`TILEFINCH_VARIABLE_CACHE_ENTRIES` override at 32, 64 (default), and 256.
The same optimized host lab, script and source-bootstrap profiler were used.
At 64 versus 256 entries, cache misses barely changed (6,268 versus 6,168),
native computed-style time was 21 versus 22 ms, and sampled send JS was
294 versus 295 ms. The retained style/variable caches grew from 35,360 to
67,616 host bytes. At 32, misses increased to 8,283 and native style reads
took 23 ms. No default changed. Do not repeat enlargement for this workload
without evidence that capacity, rather than invalidation or uncacheable
values, now dominates. These are single-run diagnostics backed by the
three-run before/after noise range in PERFORMANCE_LEDGER.md, not a device
speed comparison.

## Accepted: compact declaration values and custom-property text

Corpus: the chatgpt.com mobile capture (two StyleX sheets, 346,947 bytes),
as a static fixture (scripts removed, both sheets inlined) and as the
no-JavaScript response-keyed replay; both parse 3,432 rules, 2,132 unique
declaration blocks and 1,006 custom-property records. On the static DOM
(674 elements) 712 rules (21%) match any element and they use 569
declarations (27%); 1,025 rules (30%) have a rightmost class/id/tag key
that occurs in the document at all.

What an unmatched rule cost before (distinct atomic rule, new style-index
test): 629 retained and 928 peak bytes. Of that, 424 bytes was its
StyleDeclaration, which embedded a whole ComputedStyle; the rest is the
StyleRule (40), its StyleRuleFilter (52), index/selector-program entries
and selector text. The declaration array (903,968 bytes) was the largest
style allocation and its doubling to 4,096 entries during the build set
the style peak. Custom-property records carried fixed 192/48/96-byte
buffers: 360 bytes each, 362,160 bytes in all, for 45 KB of text; only
395 of them match. Cascade time for unmatched rules is already near zero:
the rule index buckets rules by rightmost key (3,428 of 3,432 are keyed),
so a rule whose key is absent is never a candidate. Declaration parsing
is about 7-10% of the sheet build in a host sample.

Implemented instead of key-presence deferral: declarations keep masks,
deferred text and flags, and store their parsed values as runs of nonzero
32-bit words in a per-sheet pool, decoded bit for bit into caller storage
(`stylesheet_declaration_values`). Custom-property records point into the
selector arena. Rule sets, interning, cascade order and every parse side
effect are unchanged. Measured on the optimized host build, median of 9
runs, `65aa8f37` against this change:

| chatgpt.com measurement | Before | After |
|---|---:|---:|
| Style category current (fixture and replay) | 2,165,739 B | 1,132,417 B |
| Style category peak (fixture) | 2,710,943 B | 1,132,417 B |
| Style category peak (replay) | 2,767,165 B | 1,132,855 B |
| Whole-engine peak (no-JS replay) | 10,686,312 B | 9,653,062 B |
| Stable-page engine current (fixture) | 8,136,262 B | 7,103,012 B |
| Stylesheet build (fixture, lab css_ms) | 17.4 ms | 16.1 ms |
| First layout (replay, first-layout-us) | 58,158 us | 52,005 us |
| Style resolutions in the final layout | 2,785 | 2,784 |
| Per distinct unmatched atomic rule | 629 / 928 B | 269 / 263 B |
| Per custom-property record | 368 B | 84 B |

With JavaScript (the device-like replay, 200 ticks, one sheet loaded) the
style category falls from 877,086 / 1,915,660 to 452,032 / 904,064 bytes
(current / peak) and the whole-engine peak from 32,639,884 to 32,290,661
bytes. The page then does more work in the same ticks (a module compiles
and runs, 17 instead of 3 DOM mutations), so the JavaScript category keeps
some of the freed headroom (+390 KB current); that is progress, not a
leak. Fidelity floors hold with identical scores.

The named PSP targets build within the ratchet: ordinary `.text`
4,275,948 bytes against 4,273,116 at `65aa8f37` (+2,832), `.rodata`
+112 bytes. Budget ownership returns to zero at teardown in both new
tests. Device memory and timing were not measured.

Hidden markup was audited in the same pass. display:none (inline or by
class), [hidden], closed dialogs and <template> already cost one style
resolution for the hidden root and no boxes, draw commands, text segments
or intrinsic visits; SVG symbol content costs nothing until a <use>. Two
image-path gaps were fixed: the markup image-priority fallback fetched
and decoded <img> below author-hidden ancestors (twelve 64x64 images,
196,608 decoded bytes, on a fixture), and zero-size SVG sprite sheets were
serialized, counted against the image quota and refused by the decoder
(chatgpt.com: 6 refused decodes, 30 -> 24 discovered images).

Not implemented: deferring declaration parsing or rule/IR construction
for rules whose key is absent. It would save at most the ~10% parse share
of the build (the memory is already recovered above) while making the
rule set depend on DOM state: fragment-cache identity, sheet flags set at
parse time, font fixups, image-source context and materialization on
class/id insertion. Revisit only if the remaining per-rule index cost
(356,032 index bytes, ~104 B per rule on this corpus) becomes the dominant
style cost, or device profiling shows declaration parsing is material.

## Accepted: in-memory module bytecode cache

chatgpt.com reloads itself once during startup and its module graph was
compiled on every load. After an external module compiles, its record is now
serialized (`JS_WriteObject`) into a session cache keyed by module name,
response URL, top-level site, strip policy and SHA-256 of the exact source;
the same bytes later restore with `JS_ReadObject` + `JS_ResolveModule`. It
needed an engine fix: a failed restore or import resolution left the module
registered (see `third_party/quickjs/README.md`, item 16).

Memory discipline, and why the default is 1 MiB:

- The cache has its own ceiling, not the 640 KiB/1 MiB response cache's: that
  cache holds a handful of chatgpt.com's modules and got zero hits across the
  replay. The whole graph is 1.78 MB of stripped bytecode.
- Stores need 3 MiB of Budget reserve beyond the copy and `64 KiB + 4 x source`
  of JS heap for the serialization buffer (a 348 KB module's buffer is 575 KB).
- Entries a realm has used are never evicted to admit another module of the
  same load (a cyclic LRU would trade a certain hit for a later miss); the
  cache stores the first modules of a load and hits them on the next.
- A Budget reclaim hook evicts entries before the page Budget refuses any
  allocation. Without it, a 2 MiB cache made the replay's self-reload (which
  overlaps the incumbent document) fail layout at the 32 MiB ceiling; with
  it the same run passes (two entries evicted). The hook does not help
  predictive admission checks that read `budget_remaining()`: a fuller cache
  still makes those refuse optional work sooner. At 1 MiB the combined change
  stays at or below the pre-change peaks in both replays below, which is the
  reason for the default; 2 MiB would hold chatgpt.com's whole graph and is
  the device experiment to run next.

chatgpt.com replay, optimized host, three identical runs each. "Second load"
is `--reload 1` (two engine loads of the URL, the second compiling the whole
graph); "self-reload" is the capture's own startup-recovery reload, whose
document overlaps the incumbent and is starved of scripts in every variant.

| Second load | Before | Stripped | Stripped + cache (1 MiB) | + cache (2 MiB) |
|---|---:|---:|---:|---:|
| Modules compiled / restored | 64 / 0 | 64 / 0 | 4 / 60 | 0 / 64 |
| Host compile time, all scripts | 159.3 ms | 149.5 ms | 47.5 ms | 6.5 ms |
| Module restore time | - | - | 2.9 ms | 4.5 ms |
| JS malloc after load | 11,193,685 B | 10,197,137 B | 10,133,375 B | 10,051,674 B |
| Whole-Budget peak | 28,873,948 B | 27,694,478 B | 28,693,620 B | 29,323,054 B |

| First load + self-reload | Before | Stripped | Stripped + cache (1 MiB) |
|---|---:|---:|---:|
| First-load module compile (72, inclusive) | 179.3 ms | 169.1 ms | 196.6 ms |
| First-load JS malloc peak | 14,462,805 B | 12,957,153 B | 12,957,153 B |
| Whole-Budget peak | 33,088,861 B | 32,562,063 B | 32,939,965 B |

The first load pays for serialization and hashing (about 16% of its module
compile time on the host); every later load of the same bytes restores in a
few milliseconds. Host timings are not PSP costs; the device check is the
restore/compile split in `tilefinch-input-script-modules` on a validation
build.

`tilefinch-module-bytecode-tests` covers: a second realm restoring every
module with no compile and identical results (import.meta, response-URL
relative imports, dynamic import); changed bytes, another top-level site and
another strip policy missing; a damaged entry falling back to source and
being replaced; a restored module whose import fails behaving exactly as a
compiled one, including a successful later retry; the ceiling, per-realm
protection, captive-portal bypass and exact Budget return; and the reclaim
hook turning a refusal into an eviction. Each fails with the feature removed.

## Accepted: drop retained source text of large page modules

QuickJS keeps every function's source text for `Function.prototype.toString`.
With shared source spans that is about one copy of each compiled script in the
page heap. External ES modules of 8 KiB or more are now compiled with
`JS_STRIP_SOURCE` (Bellard engine; the strip flag is set only around that
module's parse and cleared inside the loader callback). The line/column table
and file name stay, so `Error.stack`, uncaught-error reports and sites' own
fatal-error beacons keep `file:line:col`. `JS_STRIP_DEBUG` would drop those and
is not used for page code. Classic scripts (inline or external), inline
modules, `eval`/`Function` bodies and modules under 8 KiB keep their source.

The trade-off: `toString()` of a function compiled from a stripped module
returns QuickJS's NativeFunction form, `function name() {\n    [native code]\n}`.
Code that parses its own functions' text (argument-name dependency injection,
a Worker built from `fn.toString()`, self-integrity checks) sees that form
instead. Evidence for accepting it: an instrumented engine counted
`Function.prototype.toString` calls on bytecode functions across the
chatgpt.com replay's startup and found none on module code; a static scan of
its 106 captured scripts found one module that reads its own functions' text
(an optional `__octane_loc:` location marker, preceded by a property read, with
no failure when absent). Revisit if a site is found that depends on its
module functions' text: the threshold or a per-origin exception is the lever,
not reverting the policy.

chatgpt.com replay (interactive-lab response-keyed replay, optimized host,
identical three runs each; `--reload 1` loads the page twice through the
engine so the second load compiles the full graph):

| Measurement | Before (`f785026a`) | Stripped |
|---|---:|---:|
| Second load: JS malloc after load | 11,193,685 B | 10,197,137 B |
| Second load: JS malloc peak | 11,203,589 B | 10,205,417 B |
| Second load: whole-Budget peak | 28,873,948 B | 27,694,478 B |
| Second load: module compile (64 modules, inclusive) | 152.5 ms | 143.9 ms |
| First load: JS malloc after load | 12,083,998 B | 11,045,757 B |
| First load: JS malloc peak | 14,462,805 B | 12,957,153 B |
| First load run, whole-Budget peak | 33,088,861 B | 32,562,063 B |

The first-load peak falls further than the retained heap because parsing no
longer makes a transient copy of each nested function's text before the
copies are merged into shared spans. A 64 KiB comment inside one function body
grew the realm's heap by 74,150 bytes before and 8,576 bytes after
(`tilefinch-module-bytecode-tests`, which also checks the stripped and
retained `toString()` forms, `eval`/`Function` bodies inside a stripped module,
and the stack line of an error thrown from one).

## Fixed media response slabs and one-byte page probes

The PSP background transport now retains one 256 KiB fixed-response slab
after its first admitted fixed request and grows to two only if two fixed
requests coexist. The earlier first-request allocation of both slabs may have
guaranteed a second lane without a later `malloc` under fragmentation. No
recorded device run demonstrated two active fixed requests or measured that
benefit. Media metadata is normally scheduled sequentially, so restoring the
upfront 512 KiB reservation without such evidence would consume a material
part of the measured PSP free-block margin.

The production capacity helper is exercised by a host Budget test: first
admission owns one slab, injected refusal of the second preserves the first
and its exact Budget charge, retry succeeds, and partial growth from zero
rolls back to baseline. The test fails when first admission is changed to
reserve both slabs. Validation EBOOTs log `tilefinch-fixed-slab` only when a
slab allocation is needed: required and existing counts, outcome, allocation
microseconds, Budget charge, total free memory, and largest free block before
and after. The existing transport summary reports `peak-fixed`. Shipping
builds contain none of the new timing or free-memory calls.

A one-byte page-media range probe used to allocate a 64 KiB scheduler handoff
chunk. Its scheduler reserves only one response byte; the handoff allocation
now follows that bound, while ordinary schedulers keep the 64 KiB ceiling.
This removes 65,535 requested bytes of transient probe storage. The host
policy test covers the one-byte and ordinary bounds; the optimized host suite
and named ordinary/validation PSP targets compile and pass. The change does
not shrink the separate worker-owned streaming pool.

Physical PSP comparison is still needed. In a validation build, open a page
video, seek to create later range requests, then exercise a source with
separate audio and video metadata. Record each `tilefinch-fixed-slab` line,
the final `peak-fixed`, admission deferrals, and displayed-frame maxima. Check
the page probe's Budget charge on success, cancellation, and allocation
refusal, including return to its pre-probe baseline. If
`peak-fixed` reaches two and second-slab allocation produces a visible stall,
reserve the second slab at the media-loading boundary and compare again.
Retain raw device logs outside the public repository.

## Current reference

Baseline `f4650dc8` completes the captured six-script article initialization and
eight post-settle focus interactions. JavaScript allocation is 9,387,156 bytes,
with a 9,426,846-byte peak against the unchanged 9,437,184-byte limit (5 MiB base
plus the existing 4 MiB startup window). The window remains active. One caught
refusal in optional page JSON-cache serialization remains; there are no failed
scripts, uncaught callbacks or interaction-time refusals. An empty error string
alone is insufficient evidence—keep the early numeric callback-error counter.

Global Budget current/peak is 26,735,713 / 28,531,869 bytes against this replay's
32 MiB outer limit. That includes the inner JS allocation; do not add the two.
The JS arena census reports 6,709,032 bytes capacity and 5,954,488 bytes occupied
across 1,675 arenas. The difference is **not** all reclaimable: partially live
arenas cannot be freed, and requested-size versus rounded-block slack differs
from wholly unused capacity.

## Accepted: preserve source sharing through cached bytecode

Prior work (`75d954d8`, then `f4650dc8`) shares immutable nested-function source
while compiling and taking `toString()` snapshots. Its serialization regression
checked exact results, not read-side storage. Reopening discarded sharing and
allocated every nested source independently. This was a distinct, untried path.

The new versioned encoding stores a byte offset for a child contained in its
parent's shared source. No source search, per-source table, parent-function
retention or page-controlled ownership flag is introduced. A failed optional
owner allocation falls back to an admitted copy. Reader and writer restore
their enclosing source scope on both success and failure. The serialized
format and Bellard offline cache ABI advance together; incompatible compiled
caches fall back to their existing author source, not an app reinstall.

`test_quickjs_oom.c` uses a 65,667-byte nested-function fixture:

| Measurement | Rebuilt old implementation | Shared cached spans |
|---|---:|---:|
| Serialized bytes | 197,016 | 65,856 |
| Reopened allocation delta | 196,818 | 65,650 |
| 128 read/free operations, median CPU time (3 serial runs) | 653 us | 400 us |

The memory regression fails against the old engine. The microbenchmark is too
small to project a page-load or device speedup. Tests also cover reserialization,
Unicode byte offsets, sibling scopes, retained-child/source lifetime, truncated
reads, allocation refusal, old-version refusal, subsequent successful evaluation
and zero Budget ownership at teardown. Existing cache-hit and offline-install
tests cover the browser integration.

The cold article replay retains exactly the reference JS allocation and arena
census above; all three raster checksums and eight focus interactions remain
unchanged. Its session cache does shrink: whole-Budget current/peak falls to
26,596,073 / 28,392,229 bytes, a 139,640-byte reduction, entirely in SESSION.
This optimization does **not** claim to resolve its remaining inner JS-heap
pressure. Keep the source-span encoding unless a comparable full-work
case demonstrates a regression. Hardware timing and the named PSP cross-build
remain outstanding before release.

Verification: optimized build and all 151 enabled host tests pass (the external
update-root proof skips for its missing prerequisite); focused QuickJS ASan and
UBSan pass. Both generated bootstrap artifacts were regenerated and checked.

## Accepted: consolidate weak DOM-wrapper bookkeeping

Against `95ff4261`, `dom.js` now uses one private record for the weak-node cache
entry, finalizer held value and unregister token, instead of three objects.
The record contains a WeakRef, not a strong wrapper reference. Native handle
leases, stale-release rejection, connected-node preservation and actual
retirement remain unchanged. Register/unregister methods are captured before
author code so prototype replacements cannot acquire the shared private record.

The fresh-wrapper test retains 48 newly created nodes: object growth falls from
336 to 240. Parsed nodes alone were an insufficient fixture because bootstrap
discovery had already created most of their wrappers; measure fresh nodes.
Restoring the three-record implementation while keeping captured methods makes
the new object-count assertion fail. The existing finalizer/lease churn tests
continue to exercise connected reacquisition, stale leases and live detached
wrappers. Teardown still returns all Budget ownership.

Three serial article replays complete all six scripts and eight focus actions,
with unchanged raster checksums and no interaction-time refusals or uncaught
callbacks. One caught optional JSON-cache serialization refusal remains.

| Measurement | Before | Shared record |
|---|---:|---:|
| Final JS allocation | 9,387,156 B | 9,329,860 B |
| JS peak | 9,426,846 B | 9,417,398 B |
| Final whole Budget | 26,596,073 B | 26,537,833 B |
| Whole-Budget peak | 28,392,229 B | 28,388,069 B |
| Median aggregate runtime work | 1,485,694 us | 1,482,107 us |
| Maximum advance across 3 runs | 286,383 us | 289,383 us |

Timing is effectively unchanged at this sample size, not a throughput claim.
The JS limit remains 9,437,184 bytes; final headroom increases from 50,028 to
107,324 bytes. The startup window is still active. No caches are evicted, no
extra collections are scheduled and no browser features are disabled.

The optimized build and all 151 enabled host tests pass (external update-root
proof skipped). The full JavaScript responsiveness/lifecycle executable and
focused QuickJS OOM tests pass ASan/UBSan. PSP timing and cross-build remain
deferred; these results establish host behavior, not a hardware speedup.

## Rejected / do not repeat unchanged

These older local experiments were audited before starting the cached-source
change. Their historical timing is not a controlled comparison against today's
tree; failure to complete the work is itself a rejection.

| Approach | Observed result | Revisit only if… |
|---|---|---|
| Shrink all 4 KiB JS arenas to 1 KiB | 9,398,691 bytes retained, 7,867 allocations; timer OOM remained. Aggregate runtime 1,645,829 us. Older 1/2 KiB and small-block variants also have local evidence; a blanket size sweep is not new work. | A per-size-class occupancy/lifetime census identifies specific classes whose savings exceed added headers and allocation cost. Test those classes, not another global sweep. |
| Bypass small arenas entirely | 8,515,647 bytes retained but 95,420 allocations; runtime advancement assertion failed. Lower memory did not establish usability or acceptable throughput. | The allocator/lifetime mechanism changes and a bounded same-work benchmark prices the extra allocations, especially on PSP. |
| Change arena allocation order | 9,408,202 bytes retained, but three failed scripts and microtask OOM. Its shorter runtime skipped work and is not a speedup. | New occupancy evidence supports a different policy and all scripts/interactions complete. |
| Lazily allocate ordinary objects' initial property storage | About 26 KiB saved, timer/source-snapshot OOM remained; aggregate runtime 2,016,208 us and worst advance 554,511 us. Reverted. This is distinct from the accepted initial-capacity 2→1 change. | A different representation eliminates the measured slow path, with full-work timing and source-lifetime tests. |
| Trim the atom table's unused tail | 18,207 slots versus 16,848 highest live position; about 11 KiB potential on that host sample. Not implemented: too little to address the main pressure. | A new atom census shows materially larger reclaimable tails, or a cheap existing teardown boundary can reclaim them without hot-path work. |
| More frequent GC as a blanket heap remedy | Earlier collection/headroom work is already present (`d6a4841c`, `147e7471`, `fc13b81a`). A retained live graph is not collectible. | A census proves newly collectible cycles and measures the pause cost; do not infer garbage from allocator capacity. |
| Shorten the generic task target to 8 ms | Extra intermediate reflows; reverted in the earlier responsiveness work. | Publication/scheduling changes remove the extra work and a same-completion benchmark improves. |
| Defer CSS images indiscriminately | Fewer eager loads but multiplied relayout cost; reverted. | Dimension-aware/coalesced publication changes that cost model. |
| Halve arenas only for the JSObject-sized class | New per-class evidence justified this narrower experiment: the 72-byte host class had 306,576 unused bytes across 486 arenas. Halving only that class reduced unused bytes to 265,752, but added 504 live allocations. JS allocation improved by just 8,576 bytes; total Budget worsened by 79,488 bytes and its peak by 46,656 bytes. All work completed and timing was similar, but total memory lost. Reverted. | A change to lifetime clustering or allocator/header ownership removes the measured overhead. Do not try adjacent sizes of this same class as another blind sweep. |

## Accepted: reuse bounded parsed watch facts

Against `0c00df01`, Description and Comments each downloaded the watch response
again. Retain at most 16 KiB of already-parsed facts in the existing two-entry
adapter cache instead: no raw response, no new cache slots, and no page pointers.
Cookie authority, language/date preferences, two-minute expiry, cache reclaim,
site-data clearing and portal isolation remain authoritative. Comments still
requests fresh API data; a rejected cached token gets one ordinary-path retry.
Token discovery shares existing bounded build steps and may remain incomplete;
that case uses the ordinary Comments path, without delaying the initial page.

Fifteen optimized-host synthetic replay runs measured:

| Provider operation (excluding DOM/layout) | Fresh | Reused |
|---|---:|---:|
| Description requests / downloaded bytes | 1 / 845 | 0 / 0 |
| Comments requests / downloaded bytes | 2 / 1,190 | 1 / 345 |
| Description median load work | 168 us | 14 us |
| Comments median load work | 235 us | 137 us |
| Description peak extra Budget ownership | 1,120,737 B | 54,976 B |
| Comments peak extra Budget ownership | 1,160,240 B | 1,175,425 B |

The tiny Comments fixture's transient peak rises about 15 KiB because its copied
build state overlaps the API request; no blanket memory saving is claimed there.
The retained facts payload is 16,112 bytes on this host, charged to SESSION and
reclaimable. A separate 768 KiB watch fixture verifies identical Description
HTML with zero repeated downloads and 7 build steps instead of 60. The original
watch build stays at 114 steps. All owned bytes return at session teardown.

The actual click-to-rendered-page replay medians improved from 845 to 631 us for
Description and 880 to 735 us for Comments; the control watch route changed from
880 to 851 us. These are host replay timings, not physical-PSP costs or internet
latency estimates. The avoided request is the main expected device benefit.

A smaller Comments scheduler reservation with replacement on retry did **not**
reduce measured peak ownership: the fixed curl reservation dominates this tiny
fixture. That extra scheduler lifecycle was removed. Revisit only with evidence
that changing the reservation actually reduces allocation or worker contention.

## Already done / evidence needed before another experiment

- Sparse bytecode atom mapping and atomic serialization refusal are in
  `8b307fb3`; do not propose another dense-to-sparse conversion.
- Structural stylesheet cache output already skips unusable scratch and bounds
  growth by useful source output (`4eb2eb37`).
- DOM receiver/listener metadata and wrapper operations already share storage
  (`0f092cb4`, `fc13b81a`); verify a remaining allocation site first.
- Layout already shrinks node-box storage at finalization. The retained style
  cache intentionally avoids expensive font/publication rematching. Before
  shrinking or evicting it, measure unused capacity **and** lost cache hits on
  expansion, font publication and navigation; DOM/layout savings in the outer
  Budget do not automatically increase the inner JS quota.

`JS_DumpMemoryUsage` now emits a host-only, allocation-free per-class arena
census, including occupancy and arenas at or below 25% full. It runs only when
the existing diagnostic dump is requested, never on allocation hot paths, and
is compiled out on PSP. Before wrapper consolidation, the 56- and 72-byte
classes accounted for 508,904 of 754,544 unused arena bytes. This evidence led
to the targeted, rejected experiment above—not permission for another global
policy change.

The next allocator experiment requires a changed ownership/lifetime premise
as well as the size-class census above. The next
DOM/layout experiment requires a retained-capacity breakdown. Neither should
repeat the rejected global policies merely because the total heap is large.
