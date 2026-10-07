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

An entry may also be `host/path-prefix` (for example
`developer.mozilla.org/pong/`), which excludes one first-party route, such as
an ad placement, and keeps the rest of the origin in the ledger.

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

A fidelity score covers visual structure only, within these limits:

- A host score predicts visual structure, not PSP execution time.
- A closed retained trace does not prove the same page remains fetchable from
  the live origin.
- WAF or challenge pages are excluded unless the ordinary Tilefinch request
  path reaches representative content without bypassing the site policy.
- The comparator cannot determine whether a visually similar result came from
  correct semantics; focused tests and selected upstream WPT provide that
  second axis.

Timing, interactivity, and layout-cost evidence gathered alongside fidelity
work, such as the long-article layout cost and the live input runs, is
recorded in the [performance ledger](engineering/PERFORMANCE_LEDGER.md).

## MDN and Guardian references

`mdn` (developer.mozilla.org, `Array.prototype.map()`) and `guardian-home`
(theguardian.com/international) started from their October 2026 site-census
traces (`fidelity/captures/mdn-r1` and `guardian-home-r1`, copied unmodified;
the census trace format is the fidelity capture format). Neither closed
Chrome's ledger: Chrome loads resources the engine never requests, namely the
WOFF2 web fonts (Tilefinch takes the WOFF/TTF alternatives), MDN's
browser-icon SVGs and two lazily imported chunks, and the Guardian's card
images. A fresh engine capture would not contain them either.

With explicit approval of exactly those routes, they were acquired live once
with `benchmarks/acquire-trace-plan.py` (honest UA, no cookies or
credentials, every response 200): 21 GETs from `developer.mozilla.org` and
31 from `assets.guim.co.uk` and `i.guim.co.uk` (13 font files and 18 card
images), giving `mdn-r2` and `guardian-home-r2`. Both references are
eligible with closed ledgers. Two tool fixes were needed on the way: the
acquisition tool now expects the v13 record the recorder writes (cookie
provenance fields, `set-cookies-truncated`, the `@cache=` request shape), and
it accepts a trace's `NNNN.request` request-body sidecars when they match
their record's length and hash. A source-level test pins the tool's
expected version to the one `src/fetch/trace_capture.inc` writes.

MDN's exclusions:

- `mdnplay.dev` serves the code runner from a per-visit UUID origin, and
  `incoming.telemetry.mozilla.org` only receives beacons. Both are symmetric
  origin exclusions.
- `developer.mozilla.org/pong/` is excluded as a route. MDN's ad placement
  POSTs to the first-party `/pong/get`. The engine's census capture sent it
  and retained the response, while the reference's read-only policy denies
  every POST before the network, so the two renderers could never see the
  same ad slot, and the denial kept the reference ineligible. Excluding the
  route is the same decision as the telemetry host: it is an ad endpoint, not
  page content, and the rest of the origin stays in the ledger. A
  `blocked_origins` entry may therefore be `host/path-prefix`; both
  renderers apply the same rule (`--block-origin` in the engine,
  `blockedUrlMatches` in `capture-reference.js`), and denied requests stay
  in the blocked evidence channel.

MDN also sets `html { scroll-behavior: smooth }`, which left the
paused-clock reference mid-animation at the `bottom` checkpoint. Checkpoint
scrolls in `capture-reference.js` now use `behavior: "instant"`: checkpoints
are positions, and the engine never animates a scroll. The Guardian frames
are byte-identical with and without that change.

## Floor history

Floors move up only with the change that earned them. The values live in
`tests/fidelity-baselines.tsv`; this log records which rows moved and why
the frame is more faithful.

- **2026-10, MDN and Guardian rows added.** First floors, set from main
  32de7330's renders against the new eligible references. The renders are
  deterministic: two scoreboard runs were identical. Visible differences that
  remain:
  - MDN `selector-1`: the interactive example. Chrome draws the code in a
    proportional face under a visible "JavaScript Demo" title; Tilefinch
    draws it monospaced, with the title clipped under the sticky header.
  - MDN `top`: the "Widely available" badge, which wraps to two lines in
    Chrome and stays on one in Tilefinch.
  - Guardian `selector-2`: the lead story's photo, which Chrome draws and
    Tilefinch leaves as a grey placeholder because its own srcset choice is
    not in the trace.
  - Guardian `top`: the card backgrounds' tints.

  No existing row moved.

- **2026-10, MDN/Guardian parity (inline backgrounds).** `reddit top`
  (SSIM about 0.569 to 0.585, edge F1 0.856 to 0.864) and
  `wikipedia-homepage selector-1` (SSIM 0.843 to 0.847, edge F1 0.947 to
  0.954) rose. Non-replaced inline boxes now paint their background over
  the content area and padding of each line fragment (they painted only
  borders before); inline boxes with backgrounds in both frames, such as
  old.reddit's flair labels, had drawn without theirs.
  The other rows did not move.
- **2026-10, weather.gov parity (float sizing, clearfix, float scoping, UA
  list defaults).** Both `reddit` rows rose: top SSIM by about 0.04 and
  MS-SSIM by about 0.06, bottom edge F1 by about 0.15. old.reddit's listing
  rows float their thumbnails and vote arrows inside clearfix and overflow
  containers. With block-level `::after { clear: both }` honoured,
  formatting context roots no longer inheriting the enclosing context's
  floats, and floats that have already ended no longer evicting the
  bounded active-float slots, the thumbnails render beside their titles and
  the rows and footer land where the reference has them. The wikipedia,
  wikipedia-homepage and hackernews rows did not move.
- **2026-10, weather.gov parity (inline vertical alignment, line-height
  inheritance, form controls).** `wikipedia top` and `selector-1`,
  `wikipedia-homepage top` and `selector-2`, and both `reddit` rows rose
  (wikipedia `selector-1` SSIM from about 0.79 to 0.99, homepage `top`
  from about 0.67 to 0.80). Atomic inline boxes now sit on the line
  baseline with the block's strut instead of hanging from the line top,
  and a unitless line-height inherits as the number: Minerva's 13px
  hatnote and tab labels get Chrome's 21px and 19px lines instead of the
  parent's 26px and 22px, so the article lead, the infobox and the main
  page's welcome box land within a pixel or two of the reference. The
  hackernews rows and both `bottom` rows did not move.
