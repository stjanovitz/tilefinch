# Development and test workflow

The short edit loop and the qualification loop are intentionally separate.
Use `scripts/dev.sh` for the fastest targeted rebuild, or the checked-in CMake
presets when reproducible configure/build/test directories are more useful.

## Fast edit loop

Test executables are created through `tilefinch_add_test_binary` in
`cmake/TilefinchTests.cmake`; that helper also registers their build dependency.
Do not add a bare `add_executable` there, which could leave `dev.sh test` running
a stale executable. Release tests using C `assert` must undefine `NDEBUG` before
including `<assert.h>`.

Diagnostic flags in `src/diagnostic_trace.h` are sampled once per translation
unit on the owner thread; set them before launching the host process. They are
constant false in `TILEFINCH_NO_TRACE` builds. Refresh diagnostic source paths
and counts with `python3 tests/test_diagnostic_switches.py . --refresh-sites`;
the registry test verifies those paths without fragile source line numbers.

Useful narrow host test modes (not replacements for the complete suite):

- `build-preset-release/tilefinch-browser-engine-tests --native-text-sync-only`
  checks native text synchronization without duplicate relayout.
- `build-preset-release/tilefinch-browser-engine-tests --computed-style-layout-only`
  checks that paint-style reads use the current cascade without forcing layout,
  while geometry and changed stylesheets still synchronize.
- `build-preset-release/tilefinch-browser-engine-tests --font-staging-only`
  checks optional-face publication, input cancellation, rollback and retry.
- `build-preset-release/tilefinch-browser-engine-tests --font-staging-replay TRACE URL`
  profiles staged font publication on a captured page: elapsed time, largest
  cooperative gap and its ending phase, checkpoints, and style-cache work.
  Parent-key misses and eviction counts distinguish inheritance changes from
  capacity churn. The font-usage line counts old text commands whose selected
  face changes, not affected layout subtrees or pixels.
  The ASCII-segment count records reused scan results. Top/middle/bottom
  raster checksums run after the timed publication, for output comparisons.
  This explicitly prepares the baseline fonts before loading, as the PSP
  frontend does. It uses response-keyed replay to tolerate capture-era request
  metadata; it is a performance probe, not a request-policy conformance test.
- `build-preset-release/tilefinch-browser-engine-tests --navigation-staging-replay TRACE URL`
  exercises incremental navigation with the PSP's 2 KiB parser slices, staged
  fonts, external stylesheets and document JavaScript enabled, then profiles
  font publication. Use this path for initial-load investigations: the smaller
  font-only probe above does not enable external resource loading. Set
  `TILEFINCH_TRACE_LAYOUT_PROFILE=1` for phase attribution. Compare raster
  checksums as well as elapsed time and cooperative work counts; a completed
  input script on an error page is not a successful navigation.
  `TILEFINCH_TRACE_IMAGE_PROFILE=1` also splits the initial image traversal
  into element styles, pseudo styles, and final transport drain. Validation
  PSP builds log these phases automatically. A scoped variable/selector cache
  reduced the tested article's resource phase from 14.42 to 9.77 seconds on
  PSP (host traversal 95.6 to 56.0 ms, identical three-view raster checksums).
  This alone did not clear its navigation watchdog: full layout still took
  about 18 seconds. Do not report this as complete article acceptance.
  The bounded counter-prefix cursor subsequently reduced full-flow work from
  181,508 to 15,929 units and checkpoints from 46,638 to 5,239, but device
  flow time only fell from 18.04 to 17.56 seconds. The same article still
  timed out at 35.28 seconds under the previous watchdog. With the shared
  navigation deadline raised to one bounded minute, an uninstrumented physical
  PSP run completed the article in 35.887 seconds (first preview 4.080 seconds).
  Main Page menu/settings, Triangle hide/show, and cursor input completed;
  YouTube firmware playback and a committed seek also passed and exited cleanly,
  with zero media state-machine mismatches. The section script focused an
  infobox label in this live page revision, so this run does not re-qualify
  section expansion. The longer deadline avoids premature cancellation; it
  does not eliminate remaining long script/layout slices or relax user cancel.
  For deeper device attribution, explicitly configure the validation preset
  with `-DTILEFINCH_PROFILE_LAYOUT_FLOW=ON`, rebuild its named target, and
  read `tilefinch-layout-phase-flow`. An instrumented run attributed 8.23 s
  to element styles, 1.65 s to pseudo styles, 6.68 s to other flow work,
  1.01 s to inline work, and only 0.052 s to cooperation (18.17 s total).
  Bidi's separately measured 4.03 s overlaps those categories; do not add it
  again. Turn the profiling option back OFF and rebuild after the probe.
- `build-preset-release/tilefinch-browser-engine-tests --font-interrupt-replay TRACE URL DELAY_US`
  schedules an interruption at `DELAY_US` microseconds after optional font
  publication starts. Reports request-to-cooperative-acknowledgement,
  acknowledgement-to-idle-return (rollback drain), and their total. Asserts
  that cancellation keeps the loaded fallback page, font bytes and relayout
  count unchanged. It then moves focus and finishes bounded raster slices,
  composites the interactive host lab's native focus outline, and requires
  different pixels. `font-input-feedback` reports request-to-framebuffer time,
  including rollback and rendering, not physical sampling or scanout latency.
  Choose a delay shorter than the measured
  publication; the probe fails if publication finishes without interruption.
- `--navigation-interrupt-replay TRACE URL DELAY_US` uses the same feedback
  check after incremental navigation with external stylesheets and scripts.
  Prefer this over the smaller font-only probe for realistic page costs.
  `navigation-responsiveness` distinguishes the longest pump from the longest
  cooperative gap inside it, naming both ends of the gap. A long pump can
  still service native controls; it does not make arbitrary DOM edits safe
  while the layout transaction owns its scratch state.
- `--native-navigation-replay TRACE URL [RUNS]` profiles the default
  JavaScript-Off path with a 24 MiB Budget, bounded resources and staged
  baseline fonts. `--native-navigation-strict-replay` uses 16 MiB. `RUNS`
  defaults to one and is bounded to 30; multiple runs in one process support
  host sampling without changing the navigation machinery. Each run checks
  successful navigation, three rendered viewports, and zero teardown ownership.
  Request-begin time is reported separately from processing checkpoint gaps:
  captured-body verification and host curl/proxy initialization are not device
  parser/layout stalls. Preview absence under pressure is recorded as
  `available=0`, not a zero-latency paint. `first-frame-us` includes the first
  committed render; the subsequent 128 idle pumps measure currently eligible
  optional work, not an assertion that every delayed resource has settled.
- `--pointer-search-only` checks pointer down/up, committed click, native text
  replacement, implicit form submission, sliced navigation and a rendered
  result link using a hostname-neutral fixture. `--search-journey-replay TRACE URL`
  applies that journey to a captured Wikipedia homepage and the query `psp`.
  Its admission settings mirror the installed v0.1.16 configuration; the
  qualification runner also supplies the 4 MiB temporary startup window.
  Wikipedia may legitimately redirect an exact query to a disambiguation page;
  the check requires its actionable article link, not a particular results CSS
  class. Capture actual device behavior before diagnosing a host-only pass as
  a fix for an unobserved device failure.
- `build-preset-release/tilefinch-script-lazy-tests path/to/script.js [...]`
  inspects the supplied script files instead of running the default fixtures.
- `build-preset-release/tilefinch-quickjs-oom-tests N` runs the allocation
  failure boundary after `N` successful allocations (not a byte budget).

Total relayout time is not input latency. Check cooperative gaps separately:
the native UI can update the retained frame without entering the DOM at these
checkpoints. Native computed-style inheritance participates in the existing
task watchdog rather than hiding a whole ancestor chain between VM polls.
Layout likewise keeps its four-node quota for cheap work but cooperates at
the next node boundary after eight milliseconds. A single node can still
overrun that interval; this is a safe-point rule, not a hard frame deadline.
Do not infer physical-PSP responsiveness from host elapsed times alone.

### Host browsing responsiveness qualification

```sh
python3 benchmarks/run-host-browsing-responsiveness.py \
  --wikipedia-home-trace /private/path/to/home-capture \
  --wikipedia-article-trace /private/path/to/article-capture \
  --search-trace /private/path/to/home-search-capture
```

The runner builds the optimized host first, then runs font rollback/retry,
visible focus and held scrolling, disclosure expansion, deferred images,
provider navigation, pointer search, native UI/supervisor, and media
presentation/state regressions. These are composable host checks, not a
single end-to-end hardware playback claim. It never builds for PSP, launches
an emulator, plays audio, or contacts a live site. Captures are optional;
without them the synthetic gates still run. Keep capture bodies and raw
journey logs outside public source control. Output defaults to the ignored
build tree and includes raw logs, raster checksums, and `summary.json`.

The script-free journey now submits a native search form, selects a result,
opens and expands an article, scrolls, and restores both results and the form
through Back. It checks the selected URL and zero owned memory at shutdown.
It runs at both 16 and 24 MiB, repeats Back/Forward restoration, keeps focus/rendering usable while
a delayed navigation times out or is cancelled, and checks a deferred image
timeout does not strand later images or retire the page.
Replacing an in-flight request uses the frontend's cancel-then-begin ownership
sequence; further pumps must retain the replacement generation, destination
and usable disclosure controls without a stale error.
The provider journey renders with the ordinary eight-tile cache after down/up
input, verifies both Play destinations through repeated selections and a shell
replacement, and stops at the native-player handoff (no audio or decoder).
`--script-free-journey-only` on `tilefinch-browser-engine-tests` runs the first
journey and the inherited-focus-style cache/invalidation regression directly.

Optional captured home/article runs also measure JavaScript-Off focus and
scrolling through the interactive lab, separately from the scripted stress
lane (`--skip-scripted-captures` selects only this default-policy capture lane).
`summary.json` reports median/p95/maximum dispatch and paint time per
command; `focus-outline-us` isolates authored outline style resolution, not
outline rasterization. Do not combine page loading or deferred-resource drain
time with these input-to-pixel samples. Timing is observational; the committed
gates assert bounded completion, cache reuse, correct destinations and pixels
rather than machine-dependent microsecond thresholds.

On 2026-09-09, five serial optimized-host article replays (30 directional
focus samples, one warm-up excluded) identified ancestor CSS resolution as
the dominant dispatch cost. Reusing the existing layout-style cache reduced
median dispatch from 402 to 130 microseconds; p95 fell from 425 to 257 and
maximum from 432 to 265. Median input-to-pixels fell from 828 to 577
microseconds; paint remained 424 microseconds. All 14 frame captures matched
byte-for-byte. The cache-reuse assertion fails against the old controller;
uncached output, stylesheet invalidation, and zero teardown ownership are
also checked. No new retained cache or per-input allocation was added.
These are host results, not a physical-PSP latency claim. Selector-match reuse
alone was tested and rejected as within noise; computed ancestor-style reuse
is the measured improvement.

The follow-up overlay profile on the same date uses the lab's existing
`setup-us`, `tiles-us`, `overflow-us`, `sticky-us`, and `fixed-us` counters.
Late-positioned commands already have document-y spatial-index entries;
merging their complete list again made repaint scan offscreen content.
Removing that redundant merge preserves paint order and all 14 captured
frames without another cache or allocation. Five serial before/after runs
(30 directional actions per variant, excluding warm-up) measured:

| Host duration (microseconds) | Before median / p95 / max | After median / p95 / max |
| --- | --- | --- |
| Overlay pass | 395 / 469 / 472 | 227 / 291 / 292 |
| Entire repaint | 420 / 496 / 499 | 252 / 321 / 321 |
| Input to pixels | 586 / 733 / 753 | 413 / 555 / 566 |

`--background-interruption-only` exercises the real native UI receiver and
compositor from font-publication, disclosure-layout, and image-publication
checkpoints. Input is queued **after** a checkpoint; acknowledgement is
timed at the next eligible checkpoint, and visible feedback requires changed
pixels from an immutable incumbent snapshot. Menu navigation, cursor motion,
and Triangle stay on that snapshot. Scroll is dispatched only after the
owner returns; its visible timestamp includes the resulting page render.
Font publication cancels transactionally and must retry successfully with
unchanged incumbent fonts/layout. This fixture explicitly removes the default
large-page optional-font policy guard to exercise that transaction.

Five serial runs measured maximum native-chrome request-to-pixels of 65 us,
maximum queued-scroll request-to-pixels of 1,671 us (disclosure expansion),
and maximum cooperate/pump-return gap of 287 us. These synthetic host numbers
do not include the PSP's input-poll cadence, thread scheduling, vblank or
scanout. No machine-dependent timing threshold is asserted in CTest.
Functional gates also check that a 400-paragraph native disclosure really
closes and reopens: native attribute defaults bypass the script journal, so
their style caches need explicit invalidation. That regression and the
offscreen-overlay candidate-count regression both fail against pre-fix code.

The extended default-policy capture lane also records first preview, committed
frame, request opening, maximum processing checkpoint gap, idle-pump cost and
Budget peak/retained bytes. Provider fixtures report transform and generic
commit phases separately. Five serial host samples on 2026-09-09 (one warm-up
excluded; no live network) measured:

| Capture / Budget | Median preview / committed frame | Maximum processing gap | Peak Budget |
| --- | --- | --- | --- |
| Main Page / 24 MiB | 45.2 / 54.2 ms | 1.45 ms | 8.92 MiB |
| Article / 24 MiB | 85.3 / 175.5 ms | 1.57 ms | 15.78 MiB |
| Article / 16 MiB | no preview / 170.5 ms | 1.71 ms | 15.29 MiB |

The article's largest pump was about 84 ms median, but contained cooperative
checkpoints; do not equate it with an 84 ms native-input freeze. Host sampling
attributed most CPU work to flow layout, while request opening's 6–8 ms in
fresh processes included replay integrity hashing and macOS proxy setup.
Captured article focus-next input-to-pixels was 379 / 587 / 682 us
median/p95/max (40 actions). The synthetic provider search completed in
4.08 ms median, with a 0.93 ms generic commit; this small provider fixture
does not establish live-network or firmware-playback speed.

Two measured experiments were reverted rather than advertised as improvements:

- Sharing full canonical layout styles with the counter-discovery prewalk
  reduced selector calls on a 96-label fixture (290 to 193), but article
  navigation median changed from 160.0 to 161.9 ms and preview was unchanged.
  Revisit only with evidence that the bounded cache survives into the relevant
  flow traversal, or a cheaper counter-specific resolution path.
- Skipping a second declaration-index rehash when its size stayed unchanged
  left the homepage's largest checkpoint gap essentially unchanged
  (1.360 versus 1.360 ms median) and did not improve navigation time.
  Revisit only when profiling identifies that unchanged-table path as material.

These are additional host qualification results, not a new engine speedup or
physical-PSP acceptance. Keep investigating the remaining flow-layout and
stylesheet-finalization work before changing cache or publication ownership.

Verification: all enabled optimized-host tests passed (external update-root
proof skipped for its absent prerequisite; device-cost test disabled), and
the named PSP targets built at 4,125,856 bytes of `.text` against 4,480,000.
No hardware, emulator, or audible playback run was made for this batch.

To cover delayed script initialization rather than stopping at the first
optional font publication, also supply `--startup-trace /private/path/to/capture`
and `--startup-url https://example.test/article`. This runs 1,800 logical 16 ms
runtime/idle turns (28.8 seconds of timer time, configurable with
`--startup-ticks 2..4096`) with installed-app admission (5 MiB base JavaScript heap plus the existing
4 MiB boot window), checks focus rendering before and after settling, and
requires completed dynamic scripts without uncaught errors. It reports caught
heap refusals separately: an allocation refusal is not itself evidence of an
uncaught initialization failure. The last dynamic-script completion time is
measured from navigation commit, not from the initial request; it does not
prove future timers cannot start additional work beyond that window. The old
240-turn wall-clock-only replay could finish before delayed initialization
timers became due. It remains available by invoking
`tilefinch-browser-engine-tests --navigation-settle-replay TRACE URL` directly;
append the bounded turn count to select logical-clock replay. Do not compare
the two as equivalent amounts of initialization work.

Focused synthetic checks are available as
`tilefinch-js-responsiveness-tests --external-compile-pressure-only` and
`--task-time-slice-only`. The dynamic-script suite additionally pins tail
continuation admission after the script has consumed its initial reserve.

PSP execution policies now stop starting additional author tasks after a
16 ms runtime-turn target. A task and its microtask checkpoint retain their
ordering, and one expensive task can exceed that target. This is not a hard
frame deadline. Optional font/image idle work yields to queued dynamic scripts,
classic-script continuations and runnable microtasks for at most 128 runtime
turns; the baseline page, input, and tile work remain available. Pending timers
and ordinary in-flight fetches do not impose that resource delay. Recovery's
broader pending-author-work predicate is unchanged.
External compilation under heap pressure collects dead cycles before parsing.
Its collection watermark is 64 KiB plus 16 times the upcoming source length,
saturating at 1 MiB before multiplication. This is a GC heuristic, not an
admission guarantee: the allocator still enforces the unchanged heap limit.
Tiny loader segments with ample headroom therefore avoid a full collection;
even tiny segments still collect when less than 64 KiB remains. Collection
time is included in compile attribution. The focused pressure test covers
both small and large compilations against unreachable cycles, including a
negative control that fails with the former unconditional 1 MiB watermark.
Automatic GC pacing likewise must not reset its threshold below the live
heap on every advance. When the desired reserve is already occupied, the
next threshold allows allocation of half the remaining headroom before
collection. A synthetic regression checks that nearly idle turns do not
collect repeatedly, while real subsequent heap growth still collects cycles.

Two bounds keep that yield and the optional font batch from starving a
device page. `BrowserConfig.startup_resource_deferral_us` (3 s by default)
caps the startup deferral by wall clock per runtime, because a device that
advances 100 ms script tasks reaches the 128-turn cap only after a quarter
of a minute while an in-flight external script keeps the predicate true.
`BrowserFontConfig.maximum_publication_work_units` (200,000 by default, about
4.4 s of layout on the PSP-3000) keeps the fallback faces on pages whose
initial layout needed more work than that: publishing the batch reflows the
whole page at roughly the same cost, and a cancelled attempt would only be
retried. Both are pinned by the staged-font lifecycle scenarios.

The logical-clock startup replay exposed an allocation failure while a DOM
selector result constructed native wrappers. Receiver-only wrapper methods
and accessors now share a native prototype layer; private handle/rebinding
closures remain per wrapper. Listener methods are shared and listener maps
are created only on registration. Lazily created `classList` objects also
share their method functions, accessors and numeric-index proxy handler. Each
list retains its own descriptors and strong owner link: do not replace this
with an unrooted handle or change native wrapper retirement timing. Borrowed
methods use the receiving list's owner, and invalid receivers throw. The
regression exercises all 48 lists, indexed access, iteration, atomic token
validation, independent mutations, and final Budget restoration.
Descriptors are copied, not evaluated. Receiver-only descriptors are inherited
from the shared native layer with their existing flags; individual overrides
remain independent. Custom-element/shadow promotion materializes the descriptors
before replacing that layer. Detached shim adoption likewise replaces its own
virtual accessors with native descriptors, preserving parent/text/owner state.
The original 48-wrapper regression used 428,624
bytes on the optimized host and pins a 512 KiB ceiling; it also checks independent
attributes/listeners and detached-node behavior. The previously failing
28.8-second replay now finishes without uncaught errors or failed dynamic
scripts, with unchanged final rasters. Four caught allocation refusals remain
visible; this is not a claim of unlimited memory or zero allocation pressure.
Five fresh-process logical-clock replays recorded 300.7 ms median total
runtime work (302.5 ms maximum), with a 114.2 ms maximum individual advance;
the final dynamic completion was 285.5 ms median after commit. These are
captured host workloads, not live-network or physical-PSP startup timings.
The sanitizer lifecycle fixture allows 200 ms per task because its former
20 ms deadline also interrupted setup with the old DOM wrappers. Its endless
handler interruption and recovery assertions remain enabled.

The larger, fresh September 8 capture exercises **six** dynamic scripts, not
the three in the earlier timing capture below. Its late initialization failure
required additional generic fixes: native constant-stack document-order queries
avoid constructing sibling wrappers; motion inspection wraps only actual
animation candidates; small late scripts use a source-scaled execution reserve;
optional bytecode serialization preserves execution headroom; and function-source
snapshots/joins retain shared strings rather than repeatedly flattening copies.
The 1,800-tick optimized host replay completes all six scripts with zero failed
scripts and an empty final `ScriptResult.error`, then moves focus and renders
three viewports. This is not sufficient to certify callback success: subsequent
console tracing found an uncaught focus-handler OOM which that final summary
misses. Three allocation refusals were counted during settle; do not classify
them all as harmless optional work. The 5 MiB base heap plus existing 4 MiB startup donation and overall Budget
are unchanged. Do not claim physical-PSP validation from this host result.
Focused negative controls restore the old source-snapshot implementation,
fixed script reserve, and wrapper-based document-order path; all three new
regressions must fail before accepting the restored fixes.

The September 8 transient-allocation follow-up keeps that same six-script
workload. Stylesheet structural-IR capture now stops when its monotonically
growing output cannot be smaller than the source, caps geometric growth by
that useful-output bound, and releases disqualified scratch immediately.
Rebuilds from document-retained CSS also skip new IR when the exact backing
HTTP-cache entry no longer exists. Fresh responses still capture IR, valid
artifacts still replay, and compiled selector fragments still seed the first
parse even when they cannot be retained for the next navigation.

In the captured article, this removes three discarded 137,841-byte artifacts
(each previously grew a 262,144-byte scratch buffer). The first useful large
capture's capacity falls from 262,144 to 170,212 bytes. Total Budget allocation
count falls from 11,826 to 11,823; realloc growth is not counted in that metric.
The global peak remains **28,624,466 bytes**, and final ownership and all three
raster checksums are unchanged: the page's largest peak is elsewhere. Three
serial optimized-host samples give median aggregate runtime work of 1,577 ms
before and 1,524 ms after; treat this small difference as no observed slowdown,
not a hardware speedup claim. All six scripts still complete and the same three
settle-phase heap refusals remain.

The stylesheet-resource tests pin small-capture allocation/peak bounds and
retained-response rebuilds without cache ownership; restoring either old path
must fail its regression. The navigation-settle replay prints global/category
current, peak, allocation, and free counts after its final rasters. Those are
Budget-ledger metrics, not physical allocator RSS or a sum of temporary bytes.
No heap limit, timeout, cache format, or device setting changes in this batch.

The allocation-refusal follow-up fixes two QuickJS ownership/serialization
paths without raising those ceilings. Failed Latin-1/UTF-16 CString conversion
now releases its duplicated input reference. Bytecode serialization indexes
only atoms encountered by that write instead of allocating a dense array up
to the runtime's highest atom ID. A late-script regression with 20,000 unrelated
interned names succeeds with 16 KiB of remaining heap; the old writer refuses.
Output bytes and encounter-order indices remain unchanged. Buffer or atom-table
refusal also stops serialization rather than returning a partial cache artifact.
The fault sweep reproduced a 9,137-byte partial result where 18,137 bytes were
required before this fix. Negative controls fail for each of these three cases;
the restored tests check complete teardown ownership as well as results.

The six-script captured replay no longer shows its original 48,711-to-73,062-byte
cache-buffer growth refusal. It still ends near the same 9 MiB heap ceiling
(9,434,295 bytes allocated), and the focus-handler OOM remains reproducible.
All three raster checksums are unchanged. Allocation-rejection deltas printed
at settle omit earlier and later operations; inspect the full rejection and
console log, not that delta or an empty final error alone. This is a partial
fix, not a clean Wikipedia interaction qualification. Optimized host and focused
ASan/UBSan verification passed; physical PSP validation remains outstanding.

The subsequent heap fix completes that host interaction qualification without
raising either ceiling. Function-source snapshots now share immutable ASCII
spans in their existing UTF-8 source backing; only short Unicode runs are
decoded into new strings. Standalone functions adopt their source on first
snapshot too. Rope hashing, comparison, indexing, flattening, rebalancing,
printing and repeated-prefix evaluation honor the span boundaries. The span
uses the rope's existing right-hand value for its offset, so ordinary ropes
do not grow. Source ownership never retains a function or its realm, and
the 16-KiB-headroom tests cover both release orders, mixed Unicode, joins,
atomization, JSON, evaluation and standalone functions. Empty objects also
start with one property slot instead of reserving an unused second slot;
shared-shape capacities and normal admitted growth are unchanged.

The replay now advances and checks errors after eight post-settle focus
interactions, including their heap-rejection delta. A saturating callback-error
count is published before diagnostic formatting, so failure to stringify an
exception cannot make the qualification pass silently. Restoring the pre-fix
engine fails the source-span test and the strengthened replay with a focus
handler OOM; suppressing the early counter fails the unprintable-error test.
The fixed six-script replay has no uncaught callbacks, no failed scripts and
no allocation refusals during those eight interactions; all three raster
checksums remain byte-identical. One caught allocation refusal remains in
the page's optional JSON cache serialization, confirmed by the allocator
stack, rather than being inferred from an empty error message. The live heap
is still close to its bound: this is not a promise that every future module
or action will fit. Device qualification remains required before release.
Three fresh serial host runs retain 9,387,156 JS-allocated bytes after the
interaction sequence versus 9,434,295 before the fix, with unchanged 9,437,184
admission. Median aggregate runtime work is 1,480,790 us and the largest
advance across those samples is 286,583 us; the rebuilt negative control is
1,481,001 us / 285,074 us, so this is a correctness/headroom improvement,
not a demonstrated throughput win. Global Budget peak is 28,531,869 bytes;
final ownership is 26,735,713 bytes, including the additional interactions.
The full optimized suite (151 tests) and focused QuickJS ASan/UBSan gate pass;
the update-root proof still skips for its unavailable external prerequisite.

Before attempting further memory reductions, consult the
[memory experiment ledger](engineering/MEMORY_EXPERIMENTS.md). It records the
cached-function-source improvement, earlier rejected allocator changes, and
the evidence required before retrying them. Do not repeat blanket arena-size,
allocation-order or GC experiments without a changed premise.

Five fresh-process article replay samples on 2026-09-07 compared scheduling
against a rebuilt control that already included the memory fixes (so both
executed all three dynamic scripts). Median last-dynamic-completion time after
commit fell from 370.5 to 181.7 ms; preceding optional idle work fell from
202.6 ms to 0.011 ms. Median worst runtime advance fell from 33.0 to 30.4 ms
(maximum across samples: 33.3 to 30.9 ms). The final run had no uncaught errors
or failed dynamic scripts; three caught heap refusals remained observable.
Initial resources were unchanged: 14 loaded and 37 ordinary images deferred.
This prioritizes interaction/initialization; it does not eliminate the later
font/image work or the roughly 100–114 ms cooperative layout/publication calls.

An 8 ms task target produced extra intermediate reflows in this replay and
was not adopted. A separate CSS-image deferral experiment reduced eager
loads but multiplied relayout cost; it too was reverted. Keep the 60-second
navigation watchdog until physical-PSP timing and remaining indivisible work
justify changing it. These host measurements are not hardware claims.

The next full-resource replay profile found that each late reflow also built
the same CSS-counter prefix, resolving up to 4,096 element styles outside
the ordinary style-cache path. Layout now transfers that compact index to
its existing reuse cache between unchanged builds; no second copy is made.
The index payload is at most 32 KiB on the PSP (64 KiB on a 64-bit host),
with allocator overhead also Budget-charged. It remains pressure-evictable
and is included in retained-byte
accounting. Every DOM/focus/style invalidation clears it. Container-query
sheets remain uncached because resource-driven geometry can change their
counter/display conditions. Counter cursors are freshly constructed each
build; canceled builds and allocation-refused prefixes are not retained.

The page reuse cache also retains each element's exact matched author-rule
list (`StyleRetainedMatches`, at most 12 rule indices per element in a
32,768-slot table: 1 MiB on the PSP, plus bounded table metadata, allocated
from the layout budget once
a committed page has at least 64 rules). The 1,024-entry computed-style
cache cannot cover a large article, so the candidate-image traversal and the
first layout each resolved every element from scratch, and the selector
work is the memory-bound part of style resolution on the device. A retained
list skips subject preparation, index planning and every candidate
evaluation; the rules are replayed per cascade range, so the computed style
is identical by construction. Lists are recorded only for complete,
uncancelled resolutions and never for container-query sheets or nested
resolutions. They are consulted only while the reuse cache attaches the
table for one canonical resolution, never across the focus-marker probe,
and they follow the same invalidation as retained computed styles: scoped
attribute/text mutations drop the changed subtree (the parent's subtree when
structural selectors exist), simple focus changes also drop the ancestor chain,
and focus with `:focus-within`, `:has()` or sibling dependencies resets the
cache because other branches can change without a parent-style hash change. Also,
child-list, innerHTML, head-script, unknown or overflowed journals reset the
cache. The streaming preview cache never opts in, because parser appends
are not journaled. `TILEFINCH_DISABLE_RETAINED_MATCHES=1` disables the table
on the host for A/B comparison; the lab's `layout-reuse` line and the device
`tilefinch-layout-reuse:` line report hits/misses/stores.

Three refinements build on that table. First, class and id attribute
mutations are journaled with the hashes of the tokens that changed
(`stylesheet_attribute_change_tokens`, up to eight per record), and each rule
filter lists the class/id tokens outside its rightmost compound
(`relational_tokens`, including functional arguments in the rightmost compound,
such as `p:is(.active p)`). Coalesced writes union all changed tokens; any
inexact write or token overflow keeps conservative invalidation. A journaled change
whose tokens no relational list mentions drops only the changed element's
own lists; otherwise the affected rules (capped at 64, else the old subtree
drop) select the lists to drop by their rightmost fast key
(`layout_reuse_cache_invalidate_attribute`, settled once per journal by
`layout_reuse_cache_flush_invalidations`). Rules whose dependency cannot be
listed (`:has()`, `of S` nth forms, escaped identifiers, `[class]`/`[id]`
attribute selectors) count as always affected. A body class toggle that no
rule references therefore keeps every retained list; the lab's
`layout-reuse ... token=` triple and the device `tilefinch-layout-invalidate:`
line count invalidations, fallbacks and dropped lists. Second, the table also
keys `::before`/`::after` lists (an empty list records absence), so pseudo
resolution replays or skips across builds; the layout and the image traversal
attach the table around their pseudo resolutions. Third, image discovery no
longer resolves every element's `::before`/`::after` styles:
`style_node_pseudo_rules_may_affect_discovery` runs the exact selector test
only for pseudo candidates flagged as able to change display/visibility or
supply an image (background, mask, generated content, deferred
declarations), and an element with no such match skips both pseudo
resolutions. Elements themselves are still resolved in full, because the
device measurement showed that skipping them costs more in the first layout
(their retained lists are no longer seeded) than it saves in the traversal.
`TILEFINCH_DISABLE_IMAGE_PREFILTER=1` restores the pseudo resolutions on the
host; the `image-attribution-us ... prefilter=` and device
`tilefinch-candidate-images: ... prefilter=` fields report skipped/checked
elements.

Late page initialization on large articles used to pay three full costs per
event. A script-inserted `<style>` element forced a stylesheet rebuild (about
8 s on the device) plus a full relayout with every retained style answer
reset; a subtree inserted from a detached tree reset the reuse cache as a
removal would; and a class toggle on a high element re-ran image discovery
over the whole document. Now: (1) `navigation_try_append_inserted_styles`
appends newly inserted `<style>` elements to the page sheet in place
(`stylesheet_append_style_elements_tracked`): the sheet records the inline
`<style>` elements it ingested (`style_source_nodes`), the new rules take
their document position by renumbering the later rules' `order` and
re-sorting (selector program and rule index rebuild lazily), and the reuse
cache receives an old-to-new rule index map so the retained matched-rule
lists survive, dropping only the lists a new rule can select
(`layout_reuse_cache_note_stylesheet_appended`): by fast key for keyed
rules, exactly (`rule_matches` per retained entry) for up to eight keyless
rules such as `.panel > *`, `:root` for the document element only, and the
whole table past that bound. The appended-rule list is budget-owned and
sized to the append (the article's module sheets insert 67 to 438 rules at
a time; the earlier 64-entry cap made every append drop every list), and a
parse-context move (a new custom property) no longer discards the lists,
since it changes declaration values, which the note already clears, not
selector answers. A `<link>` insertion, a
change inside an already ingested `<style>`, an innerHTML replacement that
carried stylesheet sources, or a source list the sheet could not bound still
rebuilds. (2) A child-list record whose node was inserted from a
detached tree removed nothing, so reuse invalidation scopes it to the parent
(structural selectors) instead of resetting, and head-script records
invalidate nothing; moves, removals, innerHTML and unknown records stay
destructive because freed elements may still be named by retained lists.
(3) A class/id change whose tokens no display, visibility or image rule
depends on (`stylesheet_tokens_may_affect_discovery`) no longer marks
descendants image-sensitive, so no discovery rescan follows. (4) Node
retirement: the reuse cache keys entries by node pointer, and a freed
address can be reused, so removals, moves and innerHTML replacements used
to reset the cache. The runtime now reports every detached subtree it is
about to free (`script_runtime_set_node_retirement_callback`, called from
the bridge's discard path while the nodes are still valid) and the page
cache evicts that subtree (`layout_reuse_cache_retire_subtree`); the
journal records the connected parent a removed or moved child left
(`ScriptMutationRecord.scope`, cleared if that parent dies too), so those
records reduce to a structural invalidation of that scope plus the child's
new parent (`layout_reuse_cache_invalidate_structure`). That invalidation is
`:has()`-aware: without `:has()` it is the parent scope; with `:has()` the
ancestor chain's exact lists and computed styles go as well, since any
ancestor may gain or lose a match; only `:has()` combined with a sibling
combinator, which can reach an ancestor's siblings, still resets. Before
this, the reference article's skin sheet (`:has(:target)`,
`:has([aria-expanded])`) made every child-list change a full reset.
Navigation binds the callback at the first journal consumption
(`navigation_bind_node_retirement`); frees before that keep the journal's
conservative overflow signal. Host switch `TILEFINCH_DISABLE_NODE_RETIREMENT`
restores the resets. The `retired_subtrees` reuse statistic counts evictions. Host switches:
`TILEFINCH_DISABLE_STYLE_APPEND`, `TILEFINCH_DISABLE_INSERT_SCOPED_REUSE`,
`TILEFINCH_DISABLE_DISCOVERY_GATE`; the lab's `layout-reuse ...
style-appends=` pair and the device `tilefinch-style-append:` line count
appends and fallbacks, and the device `tilefinch-mutation-journal:` line
lists each consumed journal's records.

Media queries are evaluated when a sheet is parsed, against the viewport the
sheet was built for (`Stylesheet.viewport_width/height`). The streaming
blocking stylesheet is first compiled at the parser-blocking script
checkpoint that needs it, and the stream's metadata scan runs at those
checkpoints and at `</head>`; a page whose `<meta name=viewport>` follows its
first stylesheet `<link>` or a head script therefore compiles the early sheet
for the 980 px legacy viewport and only later learns the device width. The
stream fingerprint includes the current viewport, so the next checkpoint sees
a stale fingerprint, but the continuation path
(`navigation_stream_try_continue_stylesheet`) used to append the suffix
sources onto the stale prefix and adopt the moved fingerprint, and the commit
then adopted the sheet because it compared the stream's current viewport,
not the sheet's. The reference article rendered every `min-width: 640px`
rule and none of the `max-width: 639px` rules at 480 px, which is what kept
its section bodies visible and every layout at 8,600 elements. Now a
continuation is refused when the sheet's viewport differs from the stream's
(`blocking_stylesheet_viewport_rebuilds`, printed as `viewport-rebuilds=` on
the lab's `blocking-stylesheets` line and the device's navigation job line),
so the checkpoint rebuilds the sheet at the resolved width, and commit
adoption also requires the sheet's own viewport to match. Regression:
`test_streaming_stylesheet_follows_late_viewport`
(`fixtures/http-viewport-after-stylesheet`). The same late viewport reached
scripts: the bootstrap copies `__tilefinchViewportWidth` into `innerWidth`
once when the realm is created, and a realm created at a parser-blocking
script before the meta scan kept answering 980 to `innerWidth`,
`matchMedia` and `documentElement.clientWidth` after the page committed at
480 (the article's section toggler treats `innerWidth >= 720` as a tablet
and expanded every section). `script_runtime_set_viewport` republishes the
viewport globals and `innerWidth`/`innerHeight`; the stream's metadata scan
and the commit call it after resolving the viewport, and the regression
above also asserts the script-visible width. Device effect on the article:
load 27.4 s to 10.4 s, initial layout 11.5 s to 0.7 s, script relayouts 16-25 s
to 0.7-1.9 s; with the sections collapsed the page stays at about 1,000
elements through its whole initialization.

The retained-cache review regressions cover handoff from the per-build
matched-range cache, nested-selector dependencies, coalesced attribute writes
and focus effects on another descendant. A warm range-cache hit must record
its rules into a newly attached retained list, just like a fresh match scan.
Selector admission treats quoted/escaped `::` as data and leaves functional
selector lists to their established matcher. Optional-font publication refusal
is local to the current layout and must not mark a font file permanently failed
for later provider navigations. Focused review modes are
`tilefinch-style-index-tests --retained-handoff-only`,
`--retained-nested-only`, `--retained-focus-only`, and
`--quoted-punctuation-only`, plus
`tilefinch-js-responsiveness-tests --coalesced-tokens-only` and the existing
`tilefinch-browser-engine-tests --font-staging-only`.

One warm-up plus five matched fresh-process article replays on 2026-09-07
reduced median font-publication idle calls from 106.7 to 77.7 ms and
image-publication calls from 114.7 to 85.2 ms. Maximum idle call fell from
116.1 to 87.0 ms; total optional idle work fell from 227.8 to 168.5 ms.
All three raster checksums, text fingerprint, page height and script outcomes
were identical. Initial navigation and script time were not materially changed.
`navigation-publication` now reports font/image call times, counter-index
builds/hits and cache bytes separately in the startup replay; the benchmark
also checks deterministic startup rasters.

The counter reuse regression fails with reuse disabled, and covers scoped
display mutations, stylesheet changes, container queries, cancellation after
ownership transfer, and retry/teardown Budget restoration. Host input probes
at 0.5, 2 and 4 ms into font publication still acknowledged and rolled back
within 0.264 ms, with visible focus feedback within 2.015 ms. This does not
promise a device latency or a sub-frame full rebuild: element-style matching
remains the largest remaining cost. A transient bidi-absence cache was also
measured and reverted because it did not improve this workload.

The subsequent element-matching profile found avoidable saturation in the
ancestor filter: each tag occupied both a name token and a numeric-ID token.
Rules and subjects now use the same canonical tag-name token. Sibling steps
exclude the sibling's own tokens but preserve already-proven ancestor tokens
and resume at its shared parent chain. Both are rejection-filter changes;
the exact selector matcher, cascade and memory ceilings are unchanged.
One warm-up plus five isolated, matched article replays reduced median compound
checks from 3,192,751 to 2,384,142 (25.3%). Exclusive style time in the two
resource reflows fell from 51.0 to 43.1 ms and 44.7 to 39.8 ms in three profiled
runs. End-to-end publication medians improved more modestly: font 76.4 to
74.0 ms, image 83.8 to 76.9 ms, total optional idle 166.0 to 156.4 ms.
All three raster checksums, text fingerprint and page height were unchanged.
This is not a claim that CSS time or total loading time was halved.

Counter lookup now starts at its existing traversal cursor and wraps for
out-of-order requests. The 96-label regression reduces index visits from
9,888 to 290, including a repeated font-publication build; reordered flex
labels still get DOM-order values. No lookup table or per-node allocation is
added. A backward lookup can still scan the bounded index, so this is not a
universal constant-time lookup guarantee.

With the narrower resource guard and counter hint also applied, five final
article samples completed all three dynamic scripts in a median 181.5 ms
after commit (30.1 ms median worst runtime advance), without uncaught errors.
Font/image publication medians were 72.4/78.8 ms and total optional idle was
155.9 ms. Three caught heap refusals remained; resource counts and raster
checksums were unchanged. Fifteen full-resource interruption samples at
0.5/2/4 ms into publication retained the page, acknowledged and unwound within
0.182 ms, and displayed changed focus pixels within 2.156 ms.

Computed-style checkpoint refusal now uses the shared uncatchable task-error
handoff, matching callback cancellation. The regression exercises an author
try/catch around the native style read, a deadline refusal, a catchable ordinary
property-conversion error, and successful evaluation afterward. Selector,
timer-deferral, cancellation and counter-work negative controls each failed
against their pre-fix implementation. The final optimized host suite and
focused ASan/UBSan gates cover these changes. The per-node safe-point clock
sampling and pressure-triggered external-compile collection remain unchanged:
their PSP cost needs measurement before weakening input responsiveness or the
compile-reserve fix. Runtime verification used the shipping Bellard engine;
the alternate QuickJS-NG exception branch was reviewed but not executed.
No device timing or shorter navigation timeout is claimed.

Physical PSP follow-up on 2026-09-07 used the 333 MHz device, the in-memory
`host0:` validation PRX and the existing `TILEFINCH_PROFILE_LAYOUT_FLOW` option.
One 1,024-call monotonic-clock calibration took 1,054 us. The large initial
layout performed 12,377 sub-quota clock checks and 5,216 cooperative yields;
exclusive cooperation time was 53.2 ms versus 7.48 s in style work. Two late
reflows recorded 59.4/60.2 ms of cooperation versus 7.18/6.63 s in style work.
Thus the extra checks were not the dominant cost in this run. Do not trade
away their input-service boundaries to recover that small fraction of layout.
These are instrumented observations, not an uninstrumented latency floor or
a cross-run network benchmark; detailed flow profiling itself adds overhead.

The validation-only `tilefinch-layout-checkpoints` line separates sub-quota
clock checks, time-triggered yields and total yields. Its `cooperate` time is
meaningful only with `profiled=1`. `tilefinch-compile-pressure-gc` measures only
collections actually triggered at the external-compile pressure boundary and
reports heap headroom before/after. None occurred in the short article control.
The existing end-of-run forced GC probe took 119.9 ms; that demonstrates a
potential pause, not that GC caused the observed 1.78 s runtime advance.
Saved probes use the engine logger because library `printf` output is not
automatically included in the PSP validation file. All new counters and
calibration work compile out of ordinary builds.
The extended control did trigger two external-compile collections: 125.6 ms
reclaimed 365,768 bytes before a 1,421-byte segment; 98.8 ms reclaimed only
3,336 bytes before a 254-byte segment. This identifies a concrete low-value
collection to optimize. The source-sized watermark above preserves the
tight-heap compile regression while avoiding these small-source collections
at the recorded headroom. Validation-only `tilefinch-script-exception` and
`tilefinch-dynamic-fetch-failure` lines distinguish runtime exceptions from
transport/status/resource-admission failures during longer startup runs;
they do not log response bodies or add release-build telemetry.

The subsequent in-memory device tests exposed a second GC cost: resetting
the automatic threshold below the live graph produced a repeating roughly
108 ms near-idle advance tail. Growth-sensitive pacing removed that tail in
the next run. This does not establish a clean live initialization pass:
after wrapper sharing, the final run still reached the unchanged 9 MiB
startup heap ceiling (215 bytes remaining, 15 refused allocations), with
three of five dynamic scripts completed. One runtime advance included a
16.6 s full relayout. The page remained loaded and the scripted run exited
cleanly, but those remaining initialization and relayout costs are open.
The captured host replay does not reproduce the device's entire live task
sequence; do not use its green result to certify the live page or increase
the heap ceiling without attributing the additional retained state.

The next device acceptance again reproduced that initialization failure. Its
17.78 s worst advance included 15.40 s of layout after temporary form probes.
The mutation journal now records whether a node's first child-list change
inserted previously detached content. If the entire batch is proven to have
returned such content to a detached tree, navigation can retain its existing
layout. This is not a general "detached nodes are harmless" rule: removals of
existing nodes, connected-node moves into the probe, retired targets, journal
overflow, resource changes, and visible edits retain conservative handling.
MutationObserver delivery is unchanged. The bounded regression removes one
unnecessary relayout per transient batch; it does not establish a clean live
initialization pass or resolve the separate 9 MiB script-heap exhaustion.
Moving an existing connected node into an already-detached container also
journals its old parent, after the DOM operation succeeds. Otherwise the
detached-construction fast path would hide a real removal, including when
that container is subsequently attached and removed in the same batch.

Validation builds report `tilefinch-heap-failure` once per runtime after its
first allocator refusal, including failures caught inside timer callbacks.
The snapshot separates function data, bytecode, objects, properties, shapes,
strings, atoms, arrays and binary storage; it records its own walk duration.
It does not trigger garbage collection, log author source, or change heap
admission. Keep transport-incomplete startup runs separate from full script
initialization: a run that never downloads the large script cannot certify
that the later memory failure has been resolved.

An experimental 500 ms deferral-cap run opened the native menu during reflow and published
the optional font batch while external initialization remained pending. Its
earlier font reflow took 16.4 s versus 12.0 s for the later control publication;
moving work earlier is not an overall throughput improvement. Improving the
remaining style and script work remains separate from bounding resource
starvation. An extended capped run failed a large external-script fetch, while
the extended control reached more initialization work but ended with a
microtask error. Neither is a clean full-initialization pass. The cap and its
experimental test were removed rather than adopting an unproven scheduling
tradeoff. A wall-clock starvation bound remains open. The unchanged
cancellation-cache reset and non-behavioral cleanup
items are not justified as device bottlenecks by these measurements.

One warm-up plus five navigation samples and fifteen interrupted font
publications per page on 2026-09-07 measured full-resource request-to-host-pixels
medians of 1.10–1.18 ms for the homepage and 1.45–1.48 ms for the article;
maxima were 1.27 and 1.75 ms respectively. Article requests included a point
50 ms into the reflow. All retained the fallback page and changed focus pixels.
The ordinary article navigation still had a median longest pump of 100 ms
with cooperation inside it (123 ms maximum). A separate feedback-preparation
run recorded a 27 ms cooperative gap, so the host measurements do not prove
a universal frame deadline. This is not a total-load-time fix and does not
justify shortening the shipping 60-second watchdog.

Text layout now polls within long runs after 256 loop-progress units, including
preserved-space and split-word emission. Short runs keep their existing node
checkpoints. In a 32 KiB single-text-node regression, five rebuilt control/fix
samples reduced the median largest cooperative gap from 1.418 to 0.184 ms;
the command count, text fingerprint and height were identical. The old path
made no intra-text polls and fails the new cancellation assertion; the fixed
path makes 76 polls, cancels cleanly, and rebuilds with Budget ownership back
at baseline. This bounds loop progress, not elapsed time inside one indivisible
font measurement or shaping call, and is not a physical-PSP timing guarantee.

Pseudo-element matching uses a 64-entry, eight-rule-per-range memo only inside
the immutable layout/selector-cooperation scope (under 4 KiB, Budget-owned as
part of the layout context). It retains exact rule indices, not resolved
styles: inherited values and font-relative lengths are recomputed. Temporary
focus mutations clear it, container-query sheets bypass it, and overflow uses
the ordinary complete scan. Normal element styles keep their existing cache;
memoizing their rule ranges too added overhead in the measured replay.

The 2026-09-06 host font replay comparison (one warm-up plus ten measured runs,
same response capture and layout-phase instrumentation) reduced median pseudo
work from 1.946 to 1.418 ms and total font publication from 9.034 to 8.317 ms.
Page height and text fingerprint were unchanged; fidelity remains separately
gated. Interruption probes at 0.5, 2 and 4 ms into publication (five each)
measured a 35 us median / 155 us maximum acknowledgement and 43 us median /
163 us maximum total cancellation response afterward. Those cancellation
samples do not demonstrate an improvement over the existing checkpoint policy;
the optimization reduces work, not the safe-point interval. These are host
observations, not hard deadlines or device-latency promises.

The follow-up pseudo census (`tilefinch-layout-pseudos`, under the existing
layout-profile flag) separates resolutions, generated-content results and
proven-absence exits. The captured font reflow made 10,724 pseudo requests,
only 192 with generated content. A layout-only early exit now avoids style
construction/finalization when the index has no candidates, or cached complete
matches prove no declaration can generate content. Empty-string content is
not absence; deferred content and inheritance remain conservative. Public
computed-style reads still resolve their normal inherited/default fields.

One warm-up plus ten matched host runs reduced median pseudo work from
1.414 to 0.946 ms (33%) and publication from 8.335 to 7.909 ms (5%). This did
not halve the remaining phase; together with the preceding match memo, it is
approximately half the original 1.946 ms measurement. The kept fast path
requires no new cache allocation. Separate absence and eight-entry resolved
style caches were tested and discarded: their small savings did not justify
extra memory or invalidation machinery. Fifteen interruption probes retained
the fallback page, with request-to-rollback completion at 26 us median /
343 us maximum; safe-point policy itself is unchanged.

The next font-publication experiment found a missed stylesheet-address rebind
at transactional page promotion. The generation survived, but the stale cache
binding forced another cold style pass on the first relayout. Promotion now
uses the existing generation-preserving rebind. Font publication clears sizing
and font-metric-dependent (`ch`) styles, retaining the other styles; their full
parent-style keys still invalidate inherited changes. A 128-byte dependency
bitset lives in the existing Budget-owned cache. During this one transaction,
occupied cache entries are not evicted by the document-order walk before it
can reuse the retained tail. End/rollback restores ordinary admission, and a
failed publication still resets the cache and keeps the fallback page.

One warm-up plus 30 measured host replay runs compared the previous full-reset
and promotion behavior with this path: median ordinary-style time fell from
2.193 to 1.622 ms (26%), style misses from 2,592 to 1,439, and total publication
from 7.808 to 7.488 ms (4%). Maximum publication was 8.462 versus 7.936 ms.
Height and text fingerprint were unchanged. Only 302 of 2,208 old text commands
selected a different face, but neighboring rewrap can still move unaffected
text: this does not justify skipping their geometry rebuild. Selective reflow
needs dependency-aware block/line reuse, not a face-only skip. The half-time
target remains unmet. Fifteen cancellation probes at 0.5/2/4 ms all retained
the fallback page (request-to-return median 25 us, maximum 113 us); these are
host observations, not a new safe-point guarantee or device claim.

The line/block-reuse investigation kept a narrower, allocation-free text-run
optimization. Final line rectangles already incorporate wrapping, baselines,
float exclusions, transformations and interaction ranges; an unchanged font
alone cannot authorize replaying them after neighboring content rewraps.
Instead, segmentation records whether the consumed segment is printable ASCII
and reuses that fact for the visible-character and strong-RTL checks. The
scanner skips Unicode classification for safe ASCII continuations while still
honoring CJK boundaries. Successful width measurement with zero letter-spacing
also needs no second grapheme-count walk. Unicode, negative tracking, missing
fonts, bidi shaping and the normal placement/hit-box path keep their fallbacks.
No persistent line cache or additional geometry ownership was introduced.

With the previous inline implementation rebuilt as the control, one warm-up
plus 30 measured host font replays reduced median inline work from 1.680 to
1.317 ms (22%) and publication from 7.292 to 6.730 ms (8%); maximum publication
was 7.957 versus 7.435 ms. The optimized pass reused 1,399 ASCII segment proofs.
Page height, text fingerprint and all three raster checksums matched. The
semantic fixtures pass both versions; the work-reuse assertion fails with the
original implementation. Fifteen interruption probes retained the fallback
page (request-to-return median 25 us, maximum 159 us). These are host results,
not device timing or a halving of the remaining publication cost.

A further font-measurement experiment reuses each STB glyph index for its
advance, its preceding kerning pair, and the next pair. This removes repeated
cmap searches within a single call, without a persistent cache, new ownership,
or changed rounding. Codepoint-zero and fallback boundaries still break
kerning; ignored characters still preserve the previous glyph. FreeType and
the separate humanist-compatible metric path are unchanged.

Two rebuilt A/B runs (one warm-up plus 30 samples each) reduced median inline
work by 8–10%: 1.310 to 1.207 ms, then 1.327 to 1.197 ms. Total font publication
improved only 1–2%: 6.823 to 6.775 ms, then 6.871 to 6.721 ms. The second run's
maximum was effectively unchanged (7.283 versus 7.280 ms). This is a small
throughput saving, not an improvement to the interruption deadline. Height,
text fingerprint and three raster checksums matched. A 640-case width digest
recorded from the rebuilt old implementation also matches (four faces,
fractional sizes, kerning/bold combinations, NUL, ignorable characters,
fallback sequences and malformed UTF-8). The semantic test intentionally
passes both implementations. Fifteen cancellation probes retained the page;
host request-to-return median was 30 us, maximum 119 us.
The optimized host suite passed (external update-root proof skipped; device
cost test disabled), and the named PSP targets passed at 4,096,376 bytes of
`.text` (+40 bytes). No device-performance gain is claimed from this host run.

The next experiment adds an inclusive first/last-key check before searching
either generated built-in fallback glyph table. Ordinary Latin text previously
searched both the CJK and emoji tables even though its codepoints cannot occur
there. Bounds come from the actual sorted tables, not hard-coded script ranges;
empty tables fail cleanly and in-range holes retain the existing binary search.
There is no new cache, allocation, or change to optional glyph-pack precedence.
Boundary fixtures cover both key widths, inclusive endpoints and nearby misses.
These and the existing width oracle pass the rebuilt old and new code.

Two A/B comparisons (one warm-up plus 30 samples each) reduced median inline
work from 1.223 to 0.916 ms and 1.185 to 0.917 ms (23–25%). Whole publication
improved from 6.819 to 6.446 ms and 6.654 to 6.534 ms (2–5%); maxima were
7.249 versus 7.003 ms and 7.348 versus 6.934 ms. Height, text fingerprint and
all three raster checksums matched. Fifteen cancellation probes retained the
fallback page (host request-to-return median 30 us, maximum 77 us). The
safe-point policy is unchanged; these are host timings, not a device claim.
The full optimized host gate and named PSP targets passed; ordinary `.text`
is 4,096,432 bytes (+56 bytes), with no retained-memory increase.

```sh
./scripts/dev.sh                         # interactive frontend only
./scripts/dev.sh unit foundation         # one aggregate unit-suite filter
./scripts/dev.sh unit                    # core: foundation + runtime + layout
./scripts/dev.sh run --fixture fixtures/interactive.html --ticks 2
./scripts/dev.sh test                    # build registered test binaries + CTest
```

The helper's `build-dev` tree uses `Debug` with `-O0 -g0` and disables the
unrelated JavaScriptCore spike. Debug, Release, and sanitizer helper trees all
use the vendored Bellard QuickJS in `third_party/quickjs`, matching the
presets and the correctness-qualified PSP-facing engine configuration. The
engine is committed with the repository's changes already applied and its
two engine files fingerprint-pinned at configure time; see
`third_party/quickjs/README.md` for the list and the update procedure. The
experimental VM patch is history only and configure refuses
`-DPSP_BROWSER_APPLY_BELLARD_QUICKJS_PATCH=ON`. `unit` builds only `tilefinch_core` and the aggregate
`tilefinch-tests` executable before running the requested filter; it is the
shortest loop for subsystem edits. `test` deliberately builds every
executable-backed registered CTest, plus the frontends used by script-backed
tests, before invoking CTest. This keeps a clean tree from producing `Not Run`
results while leaving the default build command small. Release, sanitizer,
network/replay, and long-session qualification remain explicit. Use a separate
raw CMake tree for an intentional experimental-patch comparison.

The portable Bellard configuration enables one source-neutral interpreter
shortcut by default: a two-instruction function that only returns one captured
value can return that value without constructing an otherwise unobservable
interpreter frame. It adds no fields or caches to QuickJS objects. Configure a
fresh comparison tree with
`-DPSP_BROWSER_QUICKJS_CAPTURE_GETTER_FASTPATH=OFF` when an exact upstream
dispatch control is needed; the control is built from a copy of the vendored
engine in the binary directory with that layer reversed, checked against a
pinned fingerprint, and the vendored tree itself is never modified.

### Property-fault and frame-message diagnostics

The portable Bellard baseline has an opt-in lab-only property-read diagnostic.
It is compiled out of ordinary builds and adds no fields to their QuickJS
objects. Like the other lab variants it is applied to a binary-directory
copy of the vendored engine (the build directory below is not a preset, so
it needs the explicit allowance):

```sh
cmake -S . -B build-property-trace -DCMAKE_BUILD_TYPE=Debug \
  -DPSP_BROWSER_JS_PROPERTY_FAULT_TRACE=ON \
  -DTILEFINCH_ALLOW_BUILD_DIR=ON
cmake --build build-property-trace -j8
TILEFINCH_TRACE_JS_PROPERTY_FAULTS=32 \
  ./build-property-trace/psp-browser-interactive-lab ...
```

The numeric environment value is bounded to 1--128 faults. Each record gives
the source position, function, bytecode position/opcode, base and key types,
property preview, arguments, locals, and nearby bytecode. The diagnostic is
for reducing a failure to the first invalid value; it must not be used to
rewrite third-party source or manufacture browser capabilities.
Set the value to `absent` to suppress reads of missing properties and record
only failed JavaScript `in` checks. This keeps feature-detection traces useful
when ordinary optional-property reads would otherwise consume the record cap.
Set it to `fault` to record only accesses whose base is `null` or `undefined`;
this isolates the operation that throws after long-running third-party code
without spending the bounded record budget on ordinary feature detection.

`TILEFINCH_TRACE_FRAME_MESSAGES=2048` independently records bounded frame
`postMessage` JSON (256--65,536 characters). It is available in trace-enabled
lab builds and compiled out by `PSP_BROWSER_DISABLE_TRACE`. Frame payloads can
contain transient tokens, so keep their logs out of Git and redact them before
sharing.

## CMake presets

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev

cmake --preset release
cmake --build --preset release
ctest --preset release

cmake --preset sanitize
cmake --build --preset sanitize
./scripts/run-sanitizer-tests.sh
```

The presets create `build-preset-dev`, `build-preset-release`, and
`build-preset-sanitize`. `dev` uses `RelWithDebInfo`; `release` is an optimized
host build; `sanitize` enables
AddressSanitizer and UndefinedBehaviorSanitizer with frame pointers. It keeps
the FFmpeg media backend in the gate but disables SDL audio output, avoiding
SDL's Cocoa/LaunchServices bootstrap in dozens of parallel command-line tests.
A Release
configure does not silently select the PSP runtime policy: invoke a frontend
with `--psp-profile` when that behavior is the subject of a lab run. All
presets use the pinned Bellard QuickJS configuration, enable tests, disable the
unrelated JavaScriptCore spike, and use a compiler cache when available.

Host tools and tests load one shared `tilefinch_core` image. Consequently an
engine implementation edit rebuilds that image without relinking every test
executable; the logical build dependency still guarantees that the new image
is complete before any test starts. PSP configurations continue to link a
static core and retain the same `.text` and hot-symbol ratchets. The Release
fidelity gate reserves four CTest processor slots and renders its independent
scenarios concurrently, while keeping scoreboard rows in manifest order.

On macOS, `scripts/run-sanitizer-tests.sh` runs the suite in parallel with
symbolization disabled, then reruns only failed tests serially with normal
symbols while preserving the failed status. This keeps the green path parallel
and prevents several tests exposing the same defect from overwhelming
CoreSymbolication/LaunchServices and hiding the first useful report. Override
the default two workers on macOS (eight elsewhere) with
`TILEFINCH_SANITIZER_JOBS`.

The hostile-input parser harness is opt-in:

```sh
cmake --preset hostile
cmake --build --preset hostile
ctest --preset hostile
```

It runs under the sanitizer configuration and is not a replacement for a
general-purpose fuzzer. Its build preset requests only the hostile-parser
harness; the matching test preset runs only that test.

Host builds make `tilefinch_core` depend on
`check_tilefinch_bootstrap_generated`. Editing `src/bootstrap/*.js` therefore
fails the next core build if the checked-in source or bytecode image is stale.
Regenerate both deterministic artifacts explicitly with:

```sh
cmake --build build-preset-release --target regenerate_tilefinch_bootstrap
```

That target regenerates both embedded C files and derives
`src/bootstrap/generated.sha256` from the exact authored/generated tree.
Do not edit the manifest by hand. Host checks verify the same manifest that
PSP-only builds consume.

Host builds hash the complete bootstrap source/artifact set every time, but
memoize a successful regeneration comparison by the manifest contents,
generator executable, and verification implementation. Unchanged C-only edits
therefore avoid recompiling JavaScript just to compare identical artifacts.
The explicit `tilefinch-bootstrap-generated-check` CTest always regenerates
and compares; it does not consume the memo.

For a disposable, exclusively owned worktree, measure the complete targeted
build rather than individual compiler commands:

```sh
python3 benchmarks/measure-incremental-build.py \
  --build build-preset-release --output /tmp/tilefinch-build-timings \
  --reconfigure
```

The benchmark temporarily touches source mtimes, restores them afterward,
requires actual compile evidence for edit samples, and keeps full logs. Do not
run it against a worktree another process is editing or building. See
[the September build-speed experiment](engineering/BUILD_SPEED_EXPERIMENT.md)
for measured results and remaining costs.

The PSP cross-build consumes those checked-in artifacts and does not run a
host QuickJS generator through the PSP toolchain.

### Canvas and WebGL game qualification

The public [WebGL authoring guide](WEBGL.md) defines the bounded PSP profile
that this lane qualifies and the patterns page authors should use.

The focused game-compatibility lane exercises a bounded Canvas paddle/ball
loop, indexed WebGL, point/line/triangle depth parity, uniform snapshots,
packed-wire boundary refusal, page-visibility suspension, Gamepad sampling,
drawing-buffer limits, pixel readback, object deletion, and invalid
buffer/index ranges. It also runs the complete installable
`examples/prism-break-3d/` game through deterministic physics, collision,
level-transition, multiball, power-up, particle, and dynamic-mesh scenarios.
The same lane runs `examples/treadline-arena/` through its PSP-realistic
startup, offline manifest, fixed-step tread movement, firing, pause, one-bounce
shells, destructible barrier swaps, three armor/speed classes, class-specific
secondary weapons, directional armor, the opt-in Command meter, all five
gadgets, Survival, Team Control, Convoy Escort, objective-aware bot movement,
saved setup preferences, bounded entity pools, and teardown ownership.

```sh
cmake --build build-preset-release \
  --target tilefinch-canvas-webgl-conformance-tests
ctest --test-dir build-preset-release \
  -R '^tilefinch-canvas-webgl-conformance-tests$' --output-on-failure
```

The ordinary lane stays short. A bounded 600-frame allocation soak is opt-in:

```sh
TILEFINCH_WEBGL_SOAK_FRAMES=600 \
  build-preset-release/tilefinch-canvas-webgl-conformance-tests
```

It reports the QuickJS heap before, after, and at its high-water mark together
with the number of charged allocations. The retained heap must remain within
256 KiB of its pre-soak value after collection.

The synthetic pages live under
`tests/fixtures/canvas-webgl-games/`. They are suitable for the host lab and
the isolated PPSSPP localhost harness; they contain no captured third-party
game code. PPSSPP proves lifecycle, rendering, and cleanup behavior, but its
software-renderer timing is not a substitute for the real-PSP performance
qualification.

Physical-device WebGL cadence runs use
`tests/input-scripts/webgl-game-cadence.txt` with Prism's explicit
`?qualification=ordinary`, `?qualification=heavy`, and
`?qualification=profile` URLs. The first mark resets validation-only counters
after warm-up. The profile mode also splits the example's JavaScript cost into
update, geometry construction, upload, command setup, and HUD work. The
terminal report separates author callback time from synchronous native WebGL
time and records canvas-presentation median, p95, maximum, and 60/30 Hz
deadline counts. These probes are absent from shipping builds and do not expose
a higher-resolution clock to arbitrary page JavaScript.

Treadline Arena also provides `?qualification=long-soak`. It keeps the player
alive, drives the player with the game's bounded target/avoidance planner,
aims and fires through the ordinary cooldown and projectile paths, cycles
arenas, and repeats collisions, particles, smoke, and destruction. The run
therefore proves real movement and combat instead of tracing a blind route
that can park against scenery or settling into an idle title or victory
screen. Use
`tests/input-scripts/treadline-long-soak.txt`: its 2,700-frame measured window
is roughly ninety seconds at the PSP's intended 30 Hz presentation cadence,
with marks bracketing the interval. The validation writer
persists one marked frame per run, so archive a gameplay frame at the first
mark and use the final active-game summary plus near-zero HTML-overlay time to
prove that the run did not fall back to a title panel. Use a second paired run
or a PSPLink screenshot when a visual end-frame comparison is required.
Validation retains percentile samples for the first 1,024 presented pipelines
and separately counts every over-34-ms pipeline and the global worst pipeline
for the complete run; use the latter two values to judge the long tail. Compare
the pre-interaction and controlled budget/category reports as well as the
QuickJS retained heap so a smooth run cannot hide gradual growth.

The long soak intentionally calls the game's bounded combat planner directly;
it is a simulation/render stress test, not an input-routing test. Before a
release, also run `tests/input-scripts/treadline-offline-controls.txt` from
native Home with the current Treadline package installed as Saved row one
(the device fixture keeps Prism Break in row zero).
That scenario opens **Library → Saved**, launches the sealed offline resource
pack, activates Deploy, exits and re-enters Page controls with Start+Select,
then drives nub, face, shoulder, gadget, and command inputs. Its terminal
`tilefinch-input-gamepad:` line is the receiver proof: connected, button, and
analog frame counts must all be nonzero, the connection count must cover the
automatic entry and manual re-entry, and `buttons` must include the exercised
standard Gamepad slots. Use its canvas cadence report to assess manual-input
action frames. A direct qualification URL cannot substitute for this run.

Automated `treadline-*` scenarios mute PPSSPP's host output in the disposable
run configuration. Emulated audio remains enabled so mixer and audio-syscall
work is still exercised. Do not change the game's audio preferences or the
user's normal emulator/system volume to silence a background test.

To test post-load input during JavaScript (not merely input during raster or
network work), serve `tests/fixtures` on loopback and run the freshly rebuilt
validation EBOOT:

```sh
python3 -m http.server 8779 --bind 127.0.0.1 --directory tests/fixtures
# In another terminal:
scripts/run-ppsspp-input-script.sh --script runtime-input-live --runs 2 \
  --url http://127.0.0.1:8779/cooperative-runtime-input.html
```

The harness retains raw traces and compares an ordered semantic trace for this
asynchronous scenario. It requires `scope=page-runtime` queue and successful
presentation evidence; a raster-only busy press cannot pass. Read `max-gap`
and `runtime-cooperation: unserviced` alongside `max-ack`: the latter starts
when input is sampled, not when a physical button was first pressed. This test
does not certify sub-frame focus movement or physical-device latency.

`runtime-cancel-live` uses the same fixture to verify Circle acknowledgement
and safe runtime cancellation. `wikipedia-navigation-live`, with
`--url https://en.wikipedia.org/wiki/Main_Page`, instead tests actual focus
and navigation-menu activation. Its `-page` commands wait for the committed
page's paint, but deliberately do **not** wait for background resources.
Ordinary commands retain the stricter idle gate; `-live` commands may advance
inside a cooperative checkpoint, whereas `-page` commands never do. This does
not make the initial provisional navigation preview into an interactive DOM.
Record foreground/idle separation, focus-tail timings, and lifecycle coverage
with raw journey logs outside the repository; rerun live scenarios when page
structure changes instead of treating investigation captures as fixtures.
The focused host `tilefinch-browser-engine-tests --deferred-startup-only`
checks link/section interaction while ordinary images are pending, source and
subtree replacement between pumps, and cancellation/teardown ownership.
Inspect all three captured frames as well as the visible/distinct focus gates
and pending-task evidence. `tilefinch-focus-feedback` measures an accepted
focus publication from the frame's input sample, including a repaint that
finishes in later render slices. It requires a newer completed engine frame,
the same navigation generation, and a successful display publication. A
superseded input is reported as incomplete, not a zero-latency sample. A later marked screenshot
includes intervening runtime and frame scheduling and is not that latency.

`youtube-results-focus-live`, with a normal provider results URL, sends five
Down presses starting at the first committed paint, before thumbnails finish.
Inspect `result-next`, `result-below-fold`, and `results-filled`: the selected
card and scroll position must change before images arrive and remain stable
afterward. The receiver golden pins input delivery, **not** visual correctness.
For authored outlines, `visible=0` in the native focus probe means the native
fallback outline is hidden; inspect the composed CSS outline in the capture.
Input completion updates retained damage, not framebuffer pixels. The PSP
focus path must complete bounded frame preparation and refresh its views
before publishing and clearing dirty state. Cached tiles do not consume
raster-work slots, but still obey the slice deadline and viewport bound.

If an isolated background PPSSPP launch hangs before PSP boot on macOS, sample
the process before blaming the EBOOT. PPSSPP 1.20.4's OpenGL path can block in
`Cocoa_GL_SwapWindow` while the window is occluded. The safe launcher accepts
`--graphics=vulkan`; the input-script runner exposes the same choice with
`TILEFINCH_PPSSPP_GRAPHICS=vulkan` (default `opengl`). The runner must set both
the command-line option and append-config `GraphicsBackend` consistently
(OpenGL 0, Vulkan 3). A former hardcoded OpenGL append-config overrode the
requested backend; the environment variable alone did not prove Vulkan ran.
Verify Vulkan initialization in PPSSPP's own log, plus a fresh `Booted` line
and current validation output. Keep the actual host graphics backend identical
for A/B captures, without foregrounding the window.
This is an emulator-window workaround, not a PSP rendering change.

With the same loopback fixture server, `section-disclosure-live` and
`--url http://127.0.0.1:8779/cooperative-disclosure.html` exercise opening and
closing 200 paragraphs while timers remain pending. Inspect `before`,
`expanded`, and `collapsed` captures and `tilefinch-control-activation` timings.
This isolates disclosure layout from a live site's stylesheet and network;
it is not a substitute for the actual article section test:

```sh
PPSSPP=/absolute/path/to/PPSSPPSDL scripts/run-ppsspp-input-script.sh \
  --script wikipedia-article-section-live \
  --url 'https://en.wikipedia.org/wiki/PlayStation_Portable#History' --runs 2
```

This uses 15 R-trigger page-down presses, nub movement and Cross on the
History heading, then one more page-down to capture the revealed prose,
while background work may still be pending. The runner requires native
pointer-target `id=History`, not just successful input delivery. Inspect the
three `section-*` captures: the last must show History prose, not merely a
focus box. Live site layout changes can invalidate the cursor route; update
it only after inspecting the page. `tilefinch-control-activation` measures
the receiver; `tilefinch-control-layout` reports its final layout phases and
cumulative full/fast relayout counts. A zero phase counter can mean that
instrumentation is unavailable, not that the work was free.
At normal emulator speed this scenario requires activation at most 1.25 s,
flow layout at most 750 ms, and no resource-rebuilding relayout. The optional
host-only `TILEFINCH_DISABLE_ANCESTOR_CACHE=1` switch disables layout-scoped
selector caches for timing comparisons; it is not a shipping preference.

The selector-result cache keeps 4,096 slots and the same hash/collision
behavior, but stores sheet-local rule indices and bounded instruction offsets
instead of a second pointer. Its payload is 64 KiB on the 64-bit host and
48 KiB on PSP (previously 96/64 KiB), plus one Budget allocation header.
`tilefinch-style-index-tests` checks capacity, warm-match work, invalidation,
allocation refusal, and complete release at the layout boundary. Head-script
mutation classification separately caches a stylesheet-generation lexical
summary: up to 16 simple `:has()` subjects, with conservative relayout for
excess or complex subjects. Live head/html matching and computed display are
never cached. The test pins zero selector rescans on repeated checks and
rechecks after both DOM attribute changes and stylesheet edits.

Local frame globals retain one permanent QuickJS context and WindowProxy.
Core intrinsics are installed before exposing that global; Date, RegExp,
JSON, collections, typed arrays, Promise, and weak-reference intrinsics are
deferred until script evaluation or script-facing property/reflection access.
Document identity, messaging, and installation of the document's host
adapters do not force those groups to load. This is partial initialization,
not a replacement realm: global bindings, intrinsic identity, and retained
closures must survive activation. Initialization refusal is terminal for that
context. A matched eager/lazy A/B of the host footprint probe measures a
script-free frame at roughly 192/164 KiB (about 15% less), and separately
measures activation. The older bootstrap measured 205 KiB, but lazy module
allocation attribution shifts with bootstrap changes: use the matched A/B
for the saving, not that older total. These are host heap measurements, not
physical-PSP timing claims.

`TILEFINCH_TRACE_PROVIDER=1` reports provider transform slices and timing in
host builds. `tilefinch-browser-engine-tests --provider-navigation-only`
runs the provider navigation/cancellation regressions in isolation, including
under sanitizers; it does not replace the full suite.

That lane also reports `watch-reuse` cold/warm Description and Comments loads,
request/body-byte counts, build steps and peak extra Budget ownership. It uses
synthetic replay data, not live-network timing. Tests compare cached and fresh
HTML byte-for-byte and cover actual link activation/scroll targets, cookie and
language changes, expiry, clear/reclaim, another video, cancellation, allocation
refusal and a failed-token retry. A 768 KiB fixture pins Description to zero
downloads and seven build steps after a normal watch load; an uncached expansion
still takes sixty build steps. Keep the initial watch build at its existing
114-step ceiling in that fixture: reuse must not add a blocking preflight.

For the provider detail page, use `--script youtube-detail-poster-live` with
a current YouTube watch URL. Inspect `poster-ready`: the reserved play card
must contain the downloaded poster after idle work, not just a play glyph.
The receiver golden proves script completion, not image content. Compare
`tilefinch-adapter-commit` resource time separately from variable transport
time. No video is played by this scenario.

For a deterministic PPSSPP pressure bracket, set
`TILEFINCH_PPSSPP_CPU_MHZ=166` or `111` when invoking
`scripts/run-ppsspp-input-script.sh`. The harness writes PPSSPP 1.20's
canonical `[CPU] CPUSpeed` setting and defaults to `0` (normal emulation).
`TREADLINE_CPU_MHZ` remains a compatibility alias for older local runners.
Always inspect the isolated run's generated `ppsspp.ini` before trusting a
new clock experiment: the obsolete `LockedCPUSpeed` spelling is silently
ignored by current PPSSPP builds.

#### Staging games on a PSP without stale assets

An example directory is the only authored source of its game. A PSP test can
nevertheless involve two other copies: a temporary HTTP staging tree and an
installed, integrity-protected offline snapshot. Rebuilding Tilefinch,
regenerating its JavaScript bootstrap, or loading a new browser PRX does not
update either copy. In particular, changing only an `index.html` query does
not invalidate separately cached JavaScript, CSS, or images.

Run the focused host lane above first. For a direct network-backed device run,
use the content-addressed staging helper. It hashes the complete authored tree,
copies it to an immutable directory, verifies an existing directory before
reuse, and writes the exact digest-bearing document URL to the validation
`boot.cfg`. Because the digest is part of the directory, every subresource gets
a fresh URL when any authored byte changes:

```sh
scripts/stage-psp-game.sh \
  --source examples/treadline-arena \
  --stage-root /tmp/tilefinch-game-stage \
  --base-url http://HOST_LAN_IP:8770 \
  --boot-config build-preset-psp-validation/boot.cfg \
  --name treadline \
  --query 'qualification=long-soak&profile=ordinary-cadence'
python3 -m http.server 8770 --bind HOST_LAN_IP \
  --directory /tmp/tilefinch-game-stage
```

The helper prints the full tree digest and final URL. Preserve that output with
the device log: it is the revision identity for the run. It also selects the
Game Profile's bounded 384 KiB per-script envelope in the validation boot
configuration, so a direct device run and an installed offline game admit the
same authored package. Never edit a staged
directory. If its contents no longer match the marker, the helper refuses it
instead of silently testing a mixed revision. It also records a managed binding
beside `boot.cfg`; `scripts/psplink-device.sh` rehashes and restages the current
source automatically before every subsequent memory or slot load. Pointing
`boot.cfg` at another URL disables that binding without rewriting the new task.
Leave `input_script=` empty for a manual run, exit any resident Tilefinch
instance normally with HOME, and then use `scripts/psplink-device.sh memory`.
Do not force-unload a live browser module.

Use the same helper for Prism Break, Treadline Arena, and every other packaged
game. Do not stage with an ordinary `cp`, reuse a stable game directory, or
claim a revision from the EBOOT alone. An end capture showing a title, pause,
Deploy, or completion panel invalidates a gameplay soak even if the WebGL
counters look healthy; it measured the overlay, not sustained gameplay.

Prove the loaded revision instead of relying on the screen alone:

- the HTTP server must record PSP requests for `index.html`, `game.css`, and
  `game.js` from the device address;
- the requested directory digest must match the helper's `stage-digest` output;
- `tilefinch-validation.txt` must report one discovered and loaded game script,
  no JavaScript error, and an interactive-ready page;
- capture a PSPLink screenshot before input, then press Play and verify the
  Page-controls notice and Start+Select exit;
- when measuring cadence, use the committed input script and its explicit
  qualification URL rather than the manual staging configuration.

An installed copy in **Library → Offline apps** is a sealed snapshot. Source or
staging changes do not mutate it. Open the current network-backed page, choose
**Page tools → Install offline app**, confirm that the preview says Update or
Reinstall and lists the expected resources, and complete that operation before
testing the library entry. An integrity error means the stored document and
resource pack no longer agree; never repair it by replacing individual files.
Uninstall and reinstall the whole snapshot. Finally, verify offline launch with
the network unavailable—the launch itself must not associate Wi-Fi.

### Optional ARK-4 XMB redirect

The XMB redirect is a small kernel PRX with no dependency on the browser
engine. Build it explicitly with the PSP toolchain:

```sh
PSPDEV=/path/to/pspdev cmake --preset psp
cmake --build build-preset-psp --target tilefinch-xmb-redirect
```

The result is
`build-preset-psp/xmb-redirect/tilefinch_xmb.prx`. The release install-tree
target stages the same file under `TILEFINCH/OPTIONAL/`; it is never placed in
an A/B application slot or an in-app update package. Its module-start hook is
name-based (`htmlviewer_plugin_module`) rather than firmware-offset-based,
chains the previously registered ARK handler, and signals an already-waiting
worker rather than allocating inside ARK's pre-start callback. The worker
requires an XMB confirmation press, honors the L-trigger bypass, and waits a
bounded interval for Sony's module start to finish before LoadExec. Incidental
HTML-viewer loads during XMB startup, a missing launcher, controller-read
failure, worker setup failure, or LoadExec failure all leave Sony's browser
available.

### Lazy PSP speech engine

The ordinary `psp` build stages `tilefinch-voice.prx` beside the browser
EBOOT. PocketSphinx and its model parser are **not** linked into the browser.
The native loader uses the installed browser slot directory, never a page URL
or a model-provided path. Enabling the experimental preference reserves the
existing recognition working set; the PRX is first loaded when voice input is
actually requested. Its code/data has a separate 768 KiB Budget reservation
and a checked segment-size limit. There is no second newlib heap: allocations
are forwarded to the browser heap under the voice reservation. Ordinary engine
eviction (including navigation/pressure) destroys the engine, then stops/unloads
the idle component and releases its separate code/data reservation. The next
voice request reloads it. Disabling voice additionally releases the recognition
working-set reservation. Failed unloads retain their module identity and charge.

A positively observed killed decoder is different: its heap and libc state may
be inconsistent. The module owner enters process-lifetime quarantine, clears
the callable API, and refuses both reuse and stop/unload. Keep both reservations
charged, including through preference changes and shutdown; only process exit
reclaims that state. Never run PRX `module_stop`/stdio cleanup on this path.

The optional signed voice-model download stays model-only. Browser updates
and first-install trees carry the matching engine PRX with their A/B slot.
Do not deploy only EBOOT.PBP when testing this change. Older updaters that do
not admit the new `tilefinch-voice.prx` package path need a full-install upgrade
or a transitional updater release before distributing a package containing it.

Host loader tests exercise ABI refusal, memory refusal, missing modules,
repeated use, and stop/unload failures. For actual PSP/PPSSPP module and model
initialization, build the opt-in named target:

```sh
cmake --build build-preset-psp --target psp-voice-component-probe -j8
```

Stage `voice-probe/EBOOT.PBP`, `tilefinch-voice.prx`, and `voice-model/` together
in an isolated game directory. Launch through `scripts/launch-ppsspp-safe.sh`
with an isolated `HOME`; it preserves that HOME through background
LaunchServices. `voice-probe.txt` records three load/create/cancel/destroy/
unload cycles, Budget ownership and real newlib heap use. It has no microphone
or audio output. This catches module-local cwd initialization and newlib
floating-point-parser freelist leaks that a successful link cannot detect.
Real microphone recognition still requires a physical PSP.

The September 4, 2026 shipping build measured 4,127,224 bytes of `.text`
after extraction, versus 4,380,568 with speech linked in (253,344 bytes
removed). The host loader lane and silent PPSSPP probe passed; all three
model-init/cancel/unload cycles returned Budget ownership to zero and newlib
heap use to the identical 2,152-byte probe baseline. Physical microphone
recognition is still a release-validation requirement.

### Optional PSP software decoder

H.264 High-profile playback uses a replaceable, user-built PRX so the official
EBOOT and release archives remain independent of FFmpeg decoder binaries.
Baseline/Main MP4 and MPEG-TS HLS with AAC-LC use the PSP firmware backend,
including compatible active YouTube live streams and premieres.
Prepare the narrow LGPL n8.1.2 build once, then point an opt-in PSP configure
at that ignored workspace:

```sh
PSPDEV=/path/to/pspdev \
  ./scripts/prepare-swdec-ffmpeg.sh build-swdec
PSPDEV=/path/to/pspdev cmake --preset psp \
  -DTILEFINCH_PSP_ENABLE_SWDEC_COMPONENT=ON \
  -DTILEFINCH_SWDEC_SOURCE_DIR="$PWD/build-swdec"
cmake --build build-preset-psp --target tilefinch-swdec-bundle
```

The preparation script checks out the exact upstream tag, applies the
committed patch, enables only H.264, `aac_fixed`, and the H.264 parser, and
can optionally consume device-trained profiles by passing their directory as
the second argument. Without them the component is compatible but may be
materially slower near the PSP's real-time limit.

The target writes a three-file add-on directory at
`build-preset-psp/tilefinch-swdec-addon/`: `tilefinch-swdec.prx`, the resident
`swdec-meload.prx` helper, and `component-info.txt`. Copy all three into
`PSP/GAME/TILEFINCH/components/swdec/`; do not put them in `slot-a` or
`slot-b`. The ordinary browser EBOOT always contains the bounded loader, so
it does not need to be replaced with a custom EBOOT. Signed app updates leave
the shared component directory untouched.

`component-info.txt` records the loader ABI. Every browser update manifest
also carries the ABI it expects. The update UI warns **Decoder rebuild
needed** when an installed add-on differs; rebuild the bundle with the new
source and replace all three files together. No rebuild is needed merely
because the Tilefinch version changes.
The player reserves the component's resident memory before loading it and
keeps all stream buffering in RAM.

The live-network PSP build also consumes project-owned, hash-pinned curl,
Mbed TLS, and optional nghttp2 archives. Populate the ignored offline cache
once before configuring a fresh PSP tree:

```sh
./scripts/fetch-psp-transport-deps.sh
cmake --preset psp
cmake --build --preset psp
```

`psp-http1` is the current-stack HTTP/1.1 comparison build;
`psp-legacy-transport` is an explicit non-release SDK-stack escape hatch.
See `docs/engineering/PSP_TRANSPORT.md` for provenance and validation.

### PSP compiler-hardening candidate

The real GCC 15.2.0/newlib PSP toolchain supports both
`-fstack-protector-strong` and `_FORTIFY_SOURCE=2`. The candidate preset keeps
those flags separate from the shipping preset until their device cost is
qualified:

```sh
cmake --preset psp-hardened
cmake --build build-preset-psp-hardening \
  --target tilefinch_core psp-browser-fixture psp-browser-script
```

The cross-build measurement used the same source revision and MinSizeRel
configuration for every row:

| PSP C hardening | `.text` | Delta from baseline | `.rodata` |
| --- | ---: | ---: | ---: |
| none | 3,648,612 B | — | 1,840,932 B |
| fortify level 2 | 3,649,688 B | +1,076 B (+0.03%) | 1,841,204 B |
| stack protector strong | 3,729,684 B | +81,072 B (+2.22%) | 1,841,188 B |
| combined candidate | 3,730,744 B | +82,132 B (+2.25%) | 1,841,252 B |

The combined image has roughly 2,012 protected return sites and remains below
the ordinary 4,480,000-byte `.text` ratchet. The five hot-function size
ratchets also pass. Newlib initializes one process-wide fixed stack guard
rather than a random per-process guard, so this is useful corruption detection
and exploit friction, not a desktop-grade randomized canary.

Do not turn the candidate on in the `psp` preset from build evidence alone.
Promotion requires a real PSP boot/navigation/media smoke, a before/after
input and frame-cadence comparison, and the normal named-target cross-build.
This avoids charging every protected call on the 333 MHz CPU without measuring
the effect. The `psp-hardened` preset exists so that device comparison is
reproducible rather than dependent on hand-edited C flags.

## Focused suites

The aggregate `tilefinch-tests` executable is available for an explicit
whole-program qualification run. Its test program has three
registered filters, and the layout suite lives in its own executable next to
the layout translation units it exercises:

```sh
./build-preset-dev/tilefinch-tests --list
./build-preset-dev/tilefinch-tests --filter foundation
./build-preset-dev/tilefinch-tests --filter web-runtime
./build-preset-dev/tilefinch-tests --filter sections
./build-preset-dev/tilefinch-layout-tests
```

`core` is an alias for foundation and web-runtime (`scripts/dev.sh unit core`
also runs the layout executable). CTest exposes the same suites as
`tilefinch-foundation-tests`, `tilefinch-web-runtime-tests`,
`tilefinch-layout-tests`, and `tilefinch-section-tests`, plus focused executables for
URL/security, session security, fetch streaming, the engine facade, JavaScript
responsiveness, pumpable navigation, and supported loopback redirect behavior.
For example:

```sh
ctest --test-dir build-preset-dev -R 'tilefinch-(url|session-security)-tests' \
  --output-on-failure
ctest --test-dir build-preset-dev -R tilefinch-navigation-load-tests \
  --output-on-failure
ctest --test-dir build-preset-dev -L security --output-on-failure
```

Use the narrowest relevant filter while editing, then run all registered
Release and sanitizer gates plus the aggregate executable before treating a
change as ready. Network/replay
acceptance scripts and the long platform simulator are separate qualification
tools; they should not be hidden in every local build or unit-test invocation.

The one-hour media cadence check advances a virtual clock in a tight loop and
usually finishes in under 10 ms, but it is still an occasional policy check.
It is neither registered with CTest nor included in a default build target:

```sh
cmake --build build-preset-release \
  --target tilefinch-occasional-media-timing
```

Media delivery also has an explicit real-time resilience gate. It generates a
temporary, video-only fragmented MP4 and serves it through deterministic local
fault profiles; no third-party media or network service is required. The
profiles cover healthy burst delivery, per-request setup delay, truncated
responses, slow trickles, asymmetric audio/video supply, a persistent
30-second path outage followed by recovery, buffering hysteresis, and the
PSP's publication quantum:

```sh
./scripts/run-media-resilience-gate.sh build-preset-release
```

This is intentionally outside CTest because it preserves wall-clock delivery
cadence and the longest profile takes tens of seconds. It complements rather
than replaces a hardware playback soak: host curl cannot reproduce PSP WLAN,
firmware decoder, DMA, or thread-priority timing.

## Source and target boundaries

`include/tilefinch/` contains the exposed Tilefinch contracts. Most subsystem
headers have a same-named implementation in `src/`; `remote_selector.h` and
`budget_quickjs.h` are intentionally header-only shared types/adapters. New
frontend code should prefer `browser_engine.h`; direct subsystem headers remain
available primarily for tests and the compatibility seams described in
`ARCHITECTURE.md`.

`tilefinch_core` contains engine/subsystem implementations only. The entry points
`main.c`, `interactive_main.c`, and `failure_recovery_main.c` are separate
frontend or diagnostic targets. They and the host-portable `psp_ui.c` support
module are included in the generated portability-source manifest. Keep
`TILEFINCH_CORE_SOURCES` in `cmake/TilefinchCore.cmake` as the canonical core
inventory so normal linking and portability auditing do not silently diverge.
The root `CMakeLists.txt` establishes project-wide policy and includes focused
modules for dependencies, core sources, product targets, and tests. PSP
SDK-only entry points are checked by the cross-build rather than the host
compile audit.

Large, tightly coupled owners may be divided into private implementation seams
under `src/<owner>/`. These `.inc` files are included exactly once by the
same-named `.c` owner, in dependency order; they are not standalone translation
units and must not be added to `TILEFINCH_CORE_SOURCES`. This keeps private state
and PSP code generation unchanged while making policy, transport, lifecycle,
and transaction responsibilities reviewable in isolation. Aggregate test
executables use the same pattern under `tests/suites/` so allocator and
process-lifetime semantics remain unchanged.

The current high-level maps are visible directly in their owner files:
`src/js_runtime.c` orders bridge state, host primitives, evaluation, dynamic
scripts, document state, event-loop work, result snapshots, runtime creation,
document evaluation, runtime advancement, dispatch, and teardown;
`src/style_properties.c` orders visual basics, typography, box model, layout,
positioning/visual effects, and property dispatch. The redirect, navigation,
and dynamic-script test owners similarly include scenario-family units from
`tests/suites/`. Start in the owner file and open only the relevant unit.

## Dependency integrity

The first configure downloads pinned Lexbor, QuickJS, stb, NanoSVG, and DejaVu
archives and verifies their hashes. `PSP_BROWSER_VENDOR_DIR` can supply the
prepared dependencies supported by its documented layout; individual selected
FetchContent sources can also be supplied with
`FETCHCONTENT_SOURCE_DIR_<NAME>`. Treat a checksum mismatch as a dependency or
configuration failure; do not bypass it to make a build proceed.

## Configuring and building

This longer build reference includes dependency acquisition and vendored-source
overrides in addition to the preset and fast-edit-loop sections above.

The first configure downloads hash-pinned Lexbor, the selected QuickJS engine,
stb, NanoSVG, and DejaVu sources. Raw CMake defaults to QuickJS-NG; the checked-in
presets and `scripts/dev.sh` select pinned Bellard QuickJS. Existing prepared
sources can be supplied through `PSP_BROWSER_VENDOR_DIR` (for its supported
dependency layout) or CMake's `FETCHCONTENT_SOURCE_DIR_<NAME>` overrides. The
build also requires libcurl development headers and a TLS-capable libcurl.

```sh
cmake --preset release
cmake --build build-preset-release -j8
ctest --test-dir build-preset-release -j8 --output-on-failure
```

Equivalent checked-in presets provide isolated development, Release,
ASan/UBSan, and opt-in hostile-input trees:

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

See the preset and focused-suite sections above for all presets, the three focused
unit-suite filters, security/network/architecture tests, and the boundary
between short iteration and explicit qualification runs.

For the normal Bellard QuickJS development loop, use the targeted helper
instead. It selects the pinned upstream engine used by the presets, detects the
host's parallel job count, disables the unrelated JavaScriptCore spike, and
builds only the binary needed for the current edit. Engine edits go directly
into `third_party/quickjs` and update the fingerprint pins in
`cmake/TilefinchDependencies.cmake`. The default tree uses `-O0`
for short compile latency; Release is kept in a second tree for live-site,
performance, and final validation:

```sh
./scripts/dev.sh                 # interactive lab only
./scripts/dev.sh run --fixture fixtures/interactive.html --ticks 2
./scripts/dev.sh static          # static renderer only
./scripts/dev.sh unit foundation # fastest focused aggregate unit suite
./scripts/dev.sh test            # build registered binaries and run CTest
./scripts/dev.sh release         # optimized interactive lab
./scripts/dev.sh test-release    # optimized local test gate
./scripts/dev.sh verify          # optimized tests plus three network/replay gates
./scripts/dev.sh sanitize        # separate ASan/UBSan tree and tests
```

The portable Bellard default includes a small, allocation-free fast path for a
general closure getter that only returns one captured value. It can be disabled
in a fresh control tree with
`-DPSP_BROWSER_QUICKJS_CAPTURE_GETTER_FASTPATH=OFF`; it does not inspect source
text, URLs, or call sites.

The first command creates `build-dev`; optimized commands use `build-release`.
The release and sanitizer configurations reuse dependency source trees from
`build-dev` instead of downloading and extracting them again. Override the
trees or job count with `PSP_BROWSER_BUILD_DIR`,
`PSP_BROWSER_RELEASE_BUILD_DIR`, `PSP_BROWSER_SANITIZE_BUILD_DIR`, and
`PSP_BROWSER_JOBS`. Set `PSP_BROWSER_DEP_SOURCE_BUILD` when dependency sources
live in another build tree. CMake automatically uses `ccache` or `sccache` when
one is installed. Full optimized tests and sanitizer checks remain explicit
gates instead of running during every edit/build cycle.

The helper deliberately keeps its Bellard feature configuration aligned across
Debug, Release, and sanitizer trees. This ensures `test-release`, `verify`, and
the consolidated pre-cross-build readiness workflow qualify the same portable
VM behavior intended for the eventual PSP build. Use a separate raw CMake tree
when an upstream, unpatched Bellard comparison is required.

To use existing dependency checkouts:

```sh
cmake --preset release \
  -DPSP_BROWSER_VENDOR_DIR=/path/to/vendor
```

Running the built frontends is documented separately in
[LAB_USAGE.md](engineering/LAB_USAGE.md).

## Build directories

Configuring into a directory inside the source tree that `CMakePresets.json`
does not name fails on a fresh configure. Existing trees are unaffected, and
`-DTILEFINCH_ALLOW_BUILD_DIR=ON` admits a deliberate experiment; delete such
a tree when the experiment is done. A few non-preset trees are allowlisted
because scripts default to them or other trees reference them:
`build-preset-psp-validation`, `build-psp-update-proof`,
`build-preset-psp-update-e2e`, the `scripts/dev.sh` trees (`build-dev`,
`build-release`, `build-dev-sanitize`), the lab scripts' `build-bellard-*`
defaults, and `build-challenge-compression` (it hosts the fetched WAMR
source every current tree's cache points at; relocate that source before
pruning it). Also preserve `build-challenge-compression-bellard`:
it contains the cached pristine QuickJS tarball used by the current workspace.
Check dependency-source and download-cache references before pruning any
experimental tree; a non-preset name alone does not prove it is disposable.

## Test fault injection and diagnostic switches

Host tests inject failures through one struct, `TilefinchTestFaults` in
`src/tilefinch_test_faults.h` (`tilefinch_test_faults()` returns it): refuse
the next wasm alias buffer, worker realm, document refresh, stream
scheduler, static-fallback layout, same-document relayout, or background
font relayout or render-shell initialization once; expire the wasm task at the
Nth checkpoint; inject a parser-checkpoint fault. Production code reads a field where the real
failure would surface; the struct does not exist in device builds. Add a
field there rather than a new static in an engine file.

Every `TILEFINCH_*` environment variable the engine reads is listed in
`docs/DIAGNOSTIC_SWITCHES.md`, and `tests/test_diagnostic_switches.py` fails
when a `getenv` appears in `src/` without a row, or a row outlives its
`getenv`. The table also says whether each read survives a device build.
