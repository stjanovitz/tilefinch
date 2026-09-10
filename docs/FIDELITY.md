# Visual fidelity workflow

Tilefinch treats Chrome device emulation at 480×272 as its visual oracle. The
comparison is intentionally offline and deterministic: both browsers consume
the same retained response-keyed trace, use the same viewport and replay
clock, and must close their request ledgers before a frame is eligible.

The corpus is git-ignored because it contains third-party page material.
Scenario definitions, trace digests, commands, metrics, and floor values are
committed, so an authorized maintainer can rebuild the evidence without
redistributing site captures.

## What is measured

The always-available [search form pixel regression](../tests/visual/search-form/README.md)
also covers the reduced script-free search form, middle/lower results,
pagination and a bottom-to-top scroll revisit. It uses
repository-owned markup and fonts and exact engine-render goldens, so it runs
without the private live-page corpus. This complements, rather than replaces,
the Chrome-reference scoreboard below.

Each scenario records checkpoints such as the top of the page, a selector or
anchor, and the bottom. The comparator normalizes both frames through RGB565
and reports:

- luminance SSIM;
- multi-scale luminance SSIM;
- Sobel edge precision, recall, and F1 with a one-pixel tolerance;
- RGB565 MAE and pixel-mismatch percentage;
- foreground-coverage difference and blank-frame status.

SSIM measures local tone and structure. Edge F1 makes missing controls,
incorrect wrapping, and shifted box geometry visible. RGB error identifies
color and antialiasing changes which structural metrics can underweight. No
single score is treated as a complete visual judgment.

For typography investigations, the lab can emit bounded text-run geometry and
computed font data. `benchmarks/compare-text-metrics.py` then separates glyph
advance, baseline, line-height, and wrapping differences from broad screenshot
noise.

## Qualification boundary

Fidelity scenarios run the general engine:

- reader mode and site adapters are disabled;
- content blocking is disabled unless the manifest explicitly declares a
  symmetric origin exclusion;
- Tilefinch and the reference browser receive the same retained responses;
- unmatched, conflicting, invalid, or incompletely served replay requests make
  the checkpoint ineligible;
- interstitial or fallback content is rejected by title and state markers.

The native HOME and YouTube provider are not scored against website Chrome
pages because they are intentional product surfaces, not alternative renders
of the same document.

## Local corpus layout

The default ignored paths are:

```text
fidelity/
├── captures/       response-keyed Tilefinch traces
├── references/     canonical Chrome PNGs and state records
└── scoreboard/     generated candidate frames and reports
```

`benchmarks/fidelity-scenarios.tsv` is the authority for URL, trace digest,
viewport, replay clock, resource expectations, checkpoints, and policy. The
committed floors live in `tests/fidelity-baselines.tsv`.

## Rebuilding a scenario

1. Capture the engine's request shape into a new private trace:

   ```sh
   psp-browser-lab --url URL \
     --capture-http fidelity/captures/NAME \
     --fetch-css --fetch-images \
     --max-download-kb 4096 --max-images 48
   ```

2. Add or update its manifest row and record the inspected trace digest.

3. Build an eligible Chrome reference from the same trace:

   ```sh
   python3 benchmarks/build-fidelity-corpus.py \
     --scenario NAME \
     --manifest benchmarks/fidelity-scenarios.tsv \
     --trace-root fidelity/captures \
     --output-root fidelity/references
   ```

The builder uses the replay/acquisition contract in
[Replay and reference lab](engineering/REPLAY_LAB.md). Live acquisition is a
separate, explicit operation; ordinary scoring never contacts a site.

## Running the scoreboard

```sh
python3 benchmarks/run-fidelity-scoreboard.py \
  --manifest benchmarks/fidelity-scenarios.tsv \
  --trace-root fidelity/captures \
  --reference-root fidelity/references \
  --work-dir fidelity/scoreboard \
  --output fidelity/scoreboard.tsv
```

The Release CTest target `tilefinch-fidelity-floor-tests` runs the same
scoreboard with floor checking. It skips cleanly when the private corpus is
absent; that skip is not evidence that fidelity passed.

## Checkpoint settling

Reference capture waits for replay work to settle before sampling. The engine
mirrors that with the lab's bounded `drain` command around selector, anchor,
and text checkpoints. A drain advances eligible runtime, resource, tile, and
media work until one complete turn is quiet. If callbacks require time, it may
advance only the configured replay-clock allowance. Iteration and clock caps
remain explicit, and a truncated drain makes the checkpoint fail rather than
silently sampling a partially hydrated page.

Draining occurs before the requested scroll position is resolved. This avoids
pinning an offset against geometry which changes during final hydration.

## Symmetric origin exclusions

The optional `blocked_origins` manifest field exists for origins whose
per-visit URLs or unbounded media cannot be represented by the retained trace.
Suffix matching is applied identically to Tilefinch and the reference browser,
and every denied request appears in a separate evidence channel.

This is not a general permission to improve a screenshot by removing content.
An exclusion is acceptable only when:

1. both renderers deny the same origin;
2. the denied traffic remains visible in the replay report;
3. the remaining page is still representative of the behavior being scored;
4. the manifest explains any excluded first-party content.

Call this a symmetric origin exclusion, not ad blocking.

## The ratchet rule

The floor check allows a small fixed tolerance below each committed SSIM,
MS-SSIM, and edge-F1 value. A floor can move upward when rendering improves.
It can move downward only when a standards correction produces a demonstrably
more correct frame and the same change includes that explanation. A failing
page, an incomplete replay, or an unexplained score change is never
re-baselined away.

When changing a floor:

1. preserve the before and after candidate frames locally;
2. verify the response ledger and reference eligibility;
3. inspect the visual difference, not only the scalar score;
4. run every checkpoint, not just the affected row;
5. update `tests/fidelity-baselines.tsv` with the rendering correction.

All committed rows are currently expected to pass. The baseline file is the
single source of truth for exact values; this guide deliberately does not
duplicate a second table which could drift.

## Direct frame comparison

For a single candidate/reference pair:

```sh
python3 benchmarks/compare-reference-frame.py \
  candidate.ppm reference.png
```

The default is diagnostic. Thresholds make it a qualification run and return
failure when missed:

```sh
python3 benchmarks/compare-reference-frame.py \
  candidate.ppm reference.png \
  --max-mae-rgb565 12 \
  --min-luma-ms-ssim 0.88 \
  --min-edge-f1 0.82 \
  --require-nonblank
```

`--geometry-anchors` adds independently measured box coordinates for cases
where a pixel score cannot identify which layout boundary moved. See the
tool's `--help` output for the versioned JSON schema and complete threshold
set.

## Known evidence limits

### Long-article layout cost (September 4, 2026)

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

- A host score predicts visual structure, not PSP execution time.
- A closed retained trace does not prove the same page remains fetchable from
  the live origin.
- WAF or challenge pages are excluded unless the ordinary Tilefinch request
  path reaches representative content without bypassing the site policy.
- The comparator cannot determine whether a visually similar result came from
  correct semantics; focused tests and selected upstream WPT provide that
  second axis.
