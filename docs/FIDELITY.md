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

- A host score predicts visual structure, not PSP execution time.
- A closed retained trace does not prove the same page remains fetchable from
  the live origin.
- WAF or challenge pages are excluded unless the ordinary Tilefinch request
  path reaches representative content without bypassing the site policy.
- The comparator cannot determine whether a visually similar result came from
  correct semantics; focused tests and selected upstream WPT provide that
  second axis.
