# Performance and qualification ledger

Dated measurements, experiments that were kept or reverted, and the engine
mechanisms they motivated. Each entry states whether it is a host, emulator,
or device result; host timings are never physical-PSP claims. The workflow
for producing these measurements is in [Development](../DEVELOPMENT.md), and
memory-specific experiments are recorded separately in the
[memory experiment ledger](MEMORY_EXPERIMENTS.md). Before repeating an
experiment listed here as rejected, check that its stated premise has changed.

## Reading the page while it loads (2026-09-23, device)

PSP-3000, `scroll-during-load-live` (reload of the PlayStation Portable
article, R every 0.6 s). Before: the load preview was two fixed snapshots;
1 of 20 presses moved it and a press waited 9.3 s. Now the stream keeps its
painted preview layout and the engine composes it at any scroll position.
A press moves the preview at once, with a checkerboard where tiles are
not rasterized or content is not laid out yet; checkpoints fill it a raster
slice at a time. (Holding a page press until its whole screen was ready was
tried and dropped: presses felt ignored.) The stream lays out a deeper
preview when the reader comes within two screens of its end. The commit
lays out from the top through the reader, who stays where they are.

- Every preview after about 30 KB was refused: a stylesheet checkpoint taken
  while the parser was inside a `<style>` element hashed its partial text,
  so each later append failed the unchanged-prefix proof. Previews now wait
  for the element to close (`tilefinch-preview-skip: reason=style-open`).
- A deeper preview is laid out from the top again. Following the reader
  a few screens at a time cost 24 layouts and stretched the load from
  17.7 s to 40.6 s. Laying out at least twice as deep each time bounds the
  count (8-14 layouts). With immediate moves 29 of 32 presses moved the
  page (worst wait 2 s, before the first preview) and the reader reached
  10,700 px during the 22.8 s load. Until the parser closes the infobox
  table, laid-out content stops at 446 px. Resumable layout would remove
  the repeated work.
- The loading page is usable (device, `preview-interaction-live`): D-pad
  Left/Right focus preview links and controls with a focus ring. Two quick
  presses made while the browser loop was busy both arrive. Select opens the
  menu over the loading page and Circle closes it without stopping the
  load. Cross on the site menu control waited for the commit, kept focus,
  and ran there. Before, every press but Circle and scrolling was
  "BUSY", and the browser loop presented nothing during a load.
- The "infobox wait" was mostly our own work. Between 18 and 35 KB the
  article is inline `<style>` (the infobox's TemplateStyles) and hidden
  markup. A preview covering all parsed content was rebuilt every 4 KB, so
  four full layouts (0.2-0.6 s each) ran in the parser and held it up.
  Rebuilds now also need 32 newly closed rendering elements (not style,
  script, link, meta or template). A rebuild that lays out no more content
  doubles that requirement. Device: one rebuild in that stretch; the only
  press slower than 300 ms is one made before the first preview exists
  (2 s); load 23.9 s (24.9-25.0 s before).
- A zoomed-out page's checkerboard below provisional content was drawn at
  the CSS offset in device space; the visual clone now scales
  `content_limit_y`.

## Initial-load attribution on the reference article (device)

A scoped variable/selector cache reduced the tested article's resource phase from 14.42 to 9.77 seconds on
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

For deeper device attribution the validation preset was configured with
`-DTILEFINCH_PROFILE_LAYOUT_FLOW=ON`. An instrumented run attributed 8.23 s
to element styles, 1.65 s to pseudo styles, 6.68 s to other flow work,
1.01 s to inline work, and only 0.052 s to cooperation (18.17 s total).
Bidi's separately measured 4.03 s overlaps those categories; do not add it
again.

## Focus, overlay, and capture profiles (2026-09-09)

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

## Startup scheduling and heap pacing

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

## Six-script startup replay and heap fixes (2026-09-08)

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
[memory experiment ledger](MEMORY_EXPERIMENTS.md). It records the
cached-function-source improvement, earlier rejected allocator changes, and
the evidence required before retrying them. Do not repeat blanket arena-size,
allocation-order or GC experiments without a changed premise.

## Initialization priority (2026-09-07)

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

## Style reuse and invalidation

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

## Retained-cache review regressions

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

## Publication, matching, and counter work (2026-09-07)

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

## Device checkpoint and GC profiles (2026-09-07)

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

## Text-layout polling and pseudo-element work (2026-09-06)

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

## Font publication and text measurement

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

## Selector-result cache and lazy frame intrinsics

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

## Speech engine extraction (2026-09-04)

The September 4, 2026 shipping build measured 4,127,224 bytes of `.text`
after extraction, versus 4,380,568 with speech linked in (253,344 bytes
removed). The host loader lane and silent PPSSPP probe passed; all three
model-init/cancel/unload cycles returned Budget ownership to zero and newlib
heap use to the identical 2,152-byte probe baseline. Physical microphone
recognition is still a release-validation requirement.

## Long-article layout cost and live input evidence (from 2026-09-04)

A fixed Wikipedia `PlayStation_Portable` capture (1,029,865 HTML bytes) was
replayed through an independently rebuilt v0.1.15 host lab and the current
optimized lab. Raw layout fell from 6,739 ms to 242 ms; the seven sampled
screens were byte-identical. A shared bounded generated-counter analysis
removes repeated full-style DOM-prefix walks, and late-positioned overflow
classification now searches nearby spatial bands while preserving fixed and
globally indexed exceptions. Neither change uses site-specific behavior.

Do not confuse that full-layout cost with first paint. The interactive host
replay painted its provisional page at 33 ms on v0.1.15 and 35 ms after the
changes, while completion fell from 6,842 ms to 548 ms (single-run figures,
not a claim about a 2 ms first-paint regression). On PPSSPP, the same captured
article's spatial-index phase fell from 10.39 s to 0.27 s; navigation completed
within the unchanged 35 s watchdog, with provisional paint at about 1.3 s.
Live HTTPS likewise completed and produced an article screenshot. These
numbers do not yet qualify post-load interaction or physical PSP performance;
those remain separate measurements. No fidelity floor was changed.

The live Wikipedia homepage was measured separately on the freshly rebuilt
validation EBOOT. It painted provisionally at about 2 s, completed initial
navigation in about 8.2 s (excluding Wi-Fi association), and completed 180
interactive-loop iterations with a clean exit. This is **not** a time-to-
interactive pass: script-driven relayouts took about 1.6 s each and a later
resource rebuild blocked one advance for about 7.4 s. Validation-only
`tilefinch-page-slow-advance` lines split script, layout, resource, and shell
damage time; a full rebuild is currently included in navigation runtime time
rather than all of its component counters. Do not infer an unmeasured phase
from a zero component counter.

Repeated identical `id`/`class` writes now preserve observer delivery without
invalidating layout, but this did not materially improve this homepage run.
Lowering the ancestor-selector filter threshold reduced host selector visits
but slightly worsened PSP layout time, so that experiment was reverted.
Moving the existing all-at-once image continuation behind first paint is not
an interaction fix: it would move the same stall into the input loop. A future
incremental path must preserve image/DOM ownership across author mutations.

The subsequent nonvisual-head mutation fix removes three unnecessary body
relayouts during script-loader activity. The captured homepage retains identical
PPM pixels, height, and link/control counts, with three semantic skips instead
of three relayouts. A fresh live PPSSPP run confirms the corresponding advances
fall from approximately 1.9 s to 0.28–0.30 s (about 5 s of blocking work removed).
The optimization preserves DOM observer delivery and refuses visible heads,
changes to head emptiness, mixed body mutations, and potentially affected
relational selectors. Its regression fails with the old child-list journal.
All 149 enabled host tests, the focused ASan/UBSan runtime test, and both named
PSP build configurations pass. This is not a complete TTI fix: later author
tasks and resource rebuilds still produce multi-second advances; live response
and scheduling variation also change how many tasks the fixed-length run reaches.

The `wikipedia-home-live` input scenario also delivers three Down presses and
one Up press through the normal receiver, captures the focus state, and exits
cleanly. **Do not treat this as latency qualification.** Its two busy presses
crossed supervised raster work; at that revision post-load
`psp_advance_page_runtime()` did not enter the navigation supervisor scope.
Native layout/resource cooperation
checkpoints therefore do not by themselves make the committed page interactive
during that call. The live input run still observed a 2.49 s advance following
a fieldset mutation and a 7.30 s advance after a stylesheet insertion. The
initial navigation supervisor was active (4,764 checkpoints, maximum observed
checkpoint gap about 270 ms at image decode), which is a different boundary.
That measurement predates the owner-thread runtime scope described in
`ARCHITECTURE.md`. The new scope services input only at browser-thread safe
checkpoints; it does not enable background presentation around arbitrary
WebGL-capable JavaScript. The separate `runtime-input-live` fixture removes
network/resource/raster timing from the post-load input test. Its bounded timer
work must produce a runtime queue entry, successful visible feedback and the
exact ordered receiver actions without drops. Raw logs retain asynchronous
script-cursor annotations, but those cursor positions are not goldened: queued
input executes after the task, while the script cursor may advance at a
checkpoint. Live Wikipedia still exposes long initial checkpoint gaps and is
not yet qualified as continuously interactive throughout every runtime call.

The `wikipedia-navigation-live` follow-up samples controls after committed page
paint without waiting for background resources to finish. Its captured targets
prove the focus moved from the menu control to the logo link, and its final
capture shows the opened navigation drawer with scripts still pending. The
first version exposed retained zero-area links from the closed drawer taking
sequential focus; the controller now skips those just as spatial focus does,
while clipped skip links remain eligible. This regression fails before the fix.
Actual input-to-successful-present instrumentation measures ordinary link focus
at about 33 ms, but menu-control focus at 1.68–1.84 s with a full relayout.
The live mobile stylesheet adds a border and inset shadow on focus; changed
shadows deliberately do not use the outline-only retained-paint path. This is
the interaction cost at that revision, not a successful low-latency qualification.
These are PPSSPP measurements, not physical PSP results, and the test starts
after committed paint rather than the earlier provisional preview.

The subsequent generic inset-focus paint path removes that focus relayout.
Two fresh PPSSPP runs measured the menu control at 67–83 ms, versus the earlier
1.68–1.84 s; ordinary links measured 33–50 ms. The host regression compares
focus application, removal and re-application byte-for-byte with full layout,
including transparent borders; a scaled retained-cache test covers changed
ring width. Reinstating the old shadow refusal makes the regression fail.
At most 64 eligible rounded controls reserve one command each (3.5 KiB of
PSP command payload, within the normal Budget and command allocation). No
commands are inserted, removed or allocated on the focus path. Complex
shadows, compositing ancestors, background-clip changes, missing slots and
relational effects retain full layout. No fidelity floor was changed.

Disclosure activation is **not** qualified as fast by this result. Opening
Wikipedia's navigation drawer still measured 1.94 s with one full relayout.
The host section test proves expand/collapse works while tasks remain pending,
not a device latency guarantee for a long article. Narrower subtree/flow
invalidation and incremental relayout remain the next work for those changes.
The separate `section-disclosure-live` fixture expands 200 prose paragraphs
through the normal controller while timers remain pending: activation took
about 283 ms to expand and 18 ms to collapse in PPSSPP. Captures confirm real
content appeared/disappeared. Those are engine activation times, not physical
button-to-panel measurements, and the fixture's small stylesheet is much
cheaper than Wikipedia's. It establishes a reproducible expansion baseline
without claiming that Wikipedia article sections share the same cost.

The follow-up now targets the actual PlayStation Portable article rather
than the synthetic disclosure. Its large section-class mutation exhausted
the bridge's cheap 256-node SVG check and then the navigation check, forcing
a stylesheet/image rebuild even though the captured article has no inline
SVGs. Navigation now resolves that uncertainty with one shared 65,536-node
budget, constant-stack traversal and cooperative checkpoints every 32 nodes.
Real SVG candidates retain their existing bounded refresh path; overflow
still falls back, and cancellation retires the mutated page safely.

On the same 32 MiB optimized host profile, live History/Hardware/Software/Games
clicks measured 260/231/216/220 ms before and 25/49/66/70 ms after. The actual
PPSSPP nub/Cross History route measured 4.49 s before and 1.41–1.49 s in two
fresh fixed runs (about 67–69% faster). The expanded article crop was byte-
identical before/after, excluding native chrome and the scrollbar. These are
activation times, not physical PSP measurements or a claim
that disclosure is instant. The remaining document refresh and flow layout
still matter; resource rebuilding is no longer charged to every section.
Final flow layout accounts for 1.03–1.11 s and spatial indexing for about
22 ms in those runs. The zero style timing is an unavailable instrumentation
field, not evidence that selector resolution costs nothing. The live harness
now refuses a resource rebuild or an activation over 1.8 s at normal PPSSPP
speed; underclocked pressure runs need a separately chosen threshold.
Regressions cover a large text subtree, an SVG beyond the old scan bound,
cooperative cancellation and zero ownership after teardown. The old short
scan fails the regression. No fidelity floor was changed.

The next flow-only pass (September 4) removes repeated selector work without
changing article content. A layout-scoped 128-entry ancestor-token cache and
4096-entry exact selector-result cache reuse immutable ancestry/matches;
the latter is an optional Budget-charged allocation of about 64 KiB on PSP,
enabled for sheets with at least 64 rules. Allocation refusal retains the
uncached path. Focus-marker edits invalidate both caches, and layout teardown
releases them. The existing rule index now partitions ordinary, `::before`
and `::after` candidates without adding a second rule-entry array.

Two fresh PPSSPP History activations measured 621,341/621,110 us of flow and
1,005,794/1,006,108 us total activation. Against the earlier 1,053,570 us
representative flow baseline, this is about **41% faster**, short of the 50%
target. The article crop remains byte-identical; no fidelity floor changed.
Doubling the local computed-style cache or the exact-result cache did not
earn its extra memory and was not retained. The live qualification now gates
activation at 1.25 s and flow at 750 ms at normal emulator speed. These remain
PPSSPP measurements, not physical PSP button-to-panel latency.

YouTube provider profiling separates transport, transformation, resources
and layout. Once all three first-valid identity fields have been found, the
bounded identity scanner now stops instead of scanning irrelevant trailing
bytes. Replaying captured search/detail responses reduced transform slices
from 66/193 to 40/133 and host transform time from 1,364/3,378 us to
1,146/2,919 us. Missing fields still cause the complete bounded scan. These
are small CPU savings, not equivalent network-load reductions. The detail
poster also uses the existing lazy-image queue (like result thumbnails),
so fetching it does not block the already-sized native-player card.
In PPSSPP, detail commit fell from 684,239 us (583,101 us resources) to
117,532/118,354 us (17,660/17,317 us resources). Network-inclusive loading
varied from 2.67 s before to 2.19/2.57 s after; transport is variable, so
the isolated commit timings are the useful comparison. An idle capture
confirms the poster subsequently fills the reserved card. The live search
run showed real results, with a 1.77 s total load and 162,677 us commit;
it already deferred thumbnails and did not have the blocking-poster cost.

The September 4 results-input follow-up reproduced a separate publication
bug: five normal Down presses moved the controller, but the old framebuffer
still showed the first card and its outline. Input completion had only updated
retained damage; the PSP shortcut presented stale pixels and cleared dirty
state. It now prepares one bounded input-priority slice and leaves unfinished
work pending. Captures show the selected lower-page card before thumbnails
load, then the same focus/viewport with all 12 thumbnails loaded. No video
scanout or GE ownership changes were needed.

Bounded frame preparation also charged resident tile hits against the raster
quota. A warm eight-tile viewport could therefore need eight one-unit turns
despite doing no raster work. Resident checks now remain deadline-bounded
without spending that quota. The host regression verifies a warm viewport
completes with zero raster units and a one-tile invalidation with exactly one;
it fails against the previous implementation. Cold raster work stays bounded.

The fresh PPSSPP live search loaded in 1.624 s: transport was 1.286 s,
provider commit 162.6 ms, including 137.6 ms layout. It still fetches no
thumbnail before first paint. Immediate completed focus publications measured
50–83 ms in this run; partial slices continue through the normal scheduler.
These are emulator input-sample-to-publication observations, not physical PSP
latency or a claim that every press completes in one frame. Live result text
and network timings vary, so this comparison does not claim a startup speedup.

The subsequent static-image planning change retains its complete layout when
all images defer and no script, inline SVG, CSS image, or video poster can
change geometry before commit. This uses the candidate page's existing
ownership and rollback, not a second preview tree. Eager-image pages retain
the normal rebuild. A host regression compares the adopted layout's pixels
and link count against a fresh full layout and checks Budget teardown; its
adoption assertion fails before the fix.

Fresh PPSSPP search/reload measurements reduced layout from about 138 ms to
68.8–70.0 ms and commit from 164–171 ms to 94–101 ms. These are emulator
measurements; live response contents vary, and no fidelity floor was changed.
The optimized host suite passed all 149 enabled tests (the optional
update-root prerequisite was skipped), as did both named PSP builds and the
focused sanitizer static-image lane. The whole sanitizer navigation runner
separately hit its existing parser-preload timing assertion, without a
sanitizer memory diagnostic.

A host response-consumption sample contained 481,963 bytes and 12 results.
The first two complete renderer records ended at bytes 278,641 and 286,751;
the second was received at 704.6 ms, versus EOF at 725.5 ms. Much of the
earlier wait occurred before the escaped initial-data payload arrived.
Bounded transformation took 4.7 ms inside the processing calls, with a
0.506 ms maximum slice. Pump elapsed time is not CPU time, and the sampling
harness's sleeps must not be attributed to parser work.

An early two-result preview is therefore not enabled by this change: in this
sample its network head start was only 21 ms, potentially less than another
layout/paint costs. A future experiment should stream complete, validated
records through the existing provisional-paint owner, preserve committed
focus and cancellation, and suppress speculative paint when EOF is imminent.
It must not rescan or lay out the entire response twice to produce a preview.
