# Memory optimization experiment ledger

Read this before repeating an allocator, source-retention or navigation-memory
experiment. Keep rejected approaches rejected until the stated premise changes.
Record an exact source baseline, a complete-work acceptance check, ownership at
teardown and comparable timing—not just a lower allocation number. Raw response
captures, browsing journeys and detailed allocation logs stay outside the public
repository. Measurements below are optimized 64-bit host results, not PSP costs.

## Current reference (2026-09-08)

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

## Accepted: consolidate weak DOM-wrapper bookkeeping (2026-09-08)

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
| Halve arenas only for the JSObject-sized class (2026-09-08) | New per-class evidence justified this narrower experiment: the 72-byte host class had 306,576 unused bytes across 486 arenas. Halving only that class reduced unused bytes to 265,752, but added 504 live allocations. JS allocation improved by just 8,576 bytes; total Budget worsened by 79,488 bytes and its peak by 46,656 bytes. All work completed and timing was similar, but total memory lost. Reverted. | A change to lifetime clustering or allocator/header ownership removes the measured overhead. Do not try adjacent sizes of this same class as another blind sweep. |

## Accepted: reuse bounded parsed watch facts (2026-09-09)

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
