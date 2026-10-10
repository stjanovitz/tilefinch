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

Ordinary component selectors are admitted up to 1023 bytes, retaining only
their actual text in the stylesheet arena. Rewrite/nesting scratch remains
bounded to 256 bytes; neither ordinary nor sparse rule records grow. Reduced
229- and 521-byte selectors check functional alternatives, specificity,
ancestor/child combinators, variable-backed badge colors and full-bleed sizing.
The former 191/255-byte admission ceilings dropped these valid rules. Packed
compound offsets reserve ten bits independently of the six container-query
bits; indexed and fallback matching must agree.

Definite single-line flex rows establish stretched cross sizes before laying
out percentage-height descendants. Reduced tests include nested links,
padding/borders/margins and non-stretched centered items.

The Chrome replay's bounded Intl facade includes the engine's limited
`Segmenter` behavior, checked against the bootstrap implementation. It must
neither substitute native ICU segmentation nor stop otherwise compatible
page initialization by omitting that constructor.

Explicit `box-sizing: inherit` follows the originating element's parent,
including generated boxes and variable-backed declarations. Universal
border-box resets therefore retain the available width through nested padded
containers. Reduced tests cover content-box overrides, initial/unset values
and declaration order; invalid keywords do not erase an earlier valid value.

Generated text decorations honor their used line height and horizontal
alignment. Solid, square generated decorations also retain bounded horizontal
`skew()` / `skewX()` painting without an offscreen surface. This is not general
affine-transform support: text, image, rounded, gradient and blurred spans
still decline that specialized path. Pixel tests cover both shear directions,
`transform:none` overrides and all-or-nothing span refusal.

Large sampled external stylesheets can be re-sampled transactionally when
script-inserted subtrees introduce new selector tokens. The document census
is capped at 16,384 nodes; eight optional hydration repairs are allowed per
page. A refused replacement keeps the published styles and layout intact.
These bounds do not change the fidelity floors.

Generated inline text ignores non-applicable width declarations, while
inline-block decorations reserve their full advance and align to the line
baseline. Single-line nested flex rows also re-align their children after
cross-axis stretching. External CSS animation metadata is retained within
64 KiB, 128 rules and 16 keyframes, including parsed-stylesheet cache reuse;
its source is available only to trusted animation initialization, not page
scripts. Optional metadata refusal does not fail ordinary styling.

The host trace recorder accepts an optional canonical HTTPS `--origin` for
anonymous cross-origin requests. Capture the actual Origin-sensitive response
instead of fabricating CORS metadata or disabling enforcement. A diagnostic
response replacement is not a qualified reference: ledger closure remains
required for scoreboard admission.

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

## Native diagnostic five-view capture

`benchmarks/native_capture.py` captures native 0/25/50/75/100-percent views
from an existing response-keyed offline trace at 480×272. It uses the lab's
native `top`, `scroll-by PIXELS`, and `bottom` commands, never page JavaScript.
Exact offsets avoid sticky headers changing the meaning of page-down steps.
This is a diagnostic strip, not a content-aligned Chrome pixel oracle.

```sh
python3 benchmarks/native_capture.py --output /tmp/native-diagnostic \
  --height-estimate 6000 --sweep-ticks 30 -- \
  build-preset-release/psp-browser-interactive-lab \
  --url https://example.test/ --replay-http-response-keyed /path/to/trace
```

The command sequence records every extra clock turn explicitly, with zero-time
drains. The runner preserves separate stdout/stderr, commands, invocation,
five PPM frames and artifact hashes. Ordered complete labels, matching
pre/render positions, stable geometry, both document edges and a clean
teardown receipt are mandatory. A stale initial height permits at most one
fresh retry using the first attempt's verified height; both attempts remain
on disk, and further instability fails. Existing output is never overwritten.
Resource acquisition and reference qualification remain separate steps.
The receipt binds the lab executable, trace and invocation by hash and refuses
input changes during either attempt. Verified frames, offsets and teardown
do not prove resource completion or completed page hydration.

For typography investigations, the lab can emit bounded text-run geometry and
computed font data. `benchmarks/compare-text-metrics.py` then separates glyph
advance, baseline, line-height, and wrapping differences from broad screenshot
noise.

The Verdana-compatible fallback's existing advances and kerning remain intact.
Its real regular, bold and italic platform faces can fit independently measured
horizontal ink bounds during cold glyph creation only. Admission requires at
least 25% positive advance mismatch, excess following sidebearing, and an outline
scale between 3/4 and 3/2. A sparse, bounded ASCII profile retains only qualified
candidates. Authored fonts, non-2048-em faces, synthetic styles and unqualified
characters preserve their native raster path. Cache keys distinguish fitted and
native ink without increasing cache-entry size or performing warm-path fitting.
A 36-byte membership bitset normalizes other characters to the native cache key;
warm accesses pay only that bounded integer lookup and the profile-key comparison.
Own-markup tests cover regular/bold/italic placement, unchanged layout metrics,
synthetic-style refusal, cache reuse, cancellation and allocation refusal.

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

For external SVG images, an intrinsic canvas larger than the decoded-image
quota can use a bounded initial raster while retaining its intrinsic dimensions
and markup. This fallback applies only to otherwise-refused ordinary images;
successful decodes, masks, backgrounds and referenced SVG sprites keep their
existing path. Subsequent display retargeting rasterizes the retained markup
at the painted size. Reduced tests pin exact pixels, refusal cleanup and the
unchanged successful-decode path.

SVG viewport defaults use `xMidYMid meet`, including root SVGs whose authored
dimensions and viewBox have different ratios. Explicit `none`, `slice` and
vertical alignment overrides remain independent per viewport. CSS scale
transforms also scale background tile dimensions and pixel offsets, including
natural-size tiles; scaling only the paint box would alter the crop and repeat
pattern. Repository-owned rectangle/checkerboard pixel tests cover these
paths, downscaled raster requests and allocation refusal cleanup.

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

### Full-document census captures

Use the maintained batch runner rather than an investigation-specific browser
wrapper. It checks the browser and PNG normalizer before starting, verifies every
trace pin, and saves each attempt in a new directory:

```sh
python3 benchmarks/capture-census.py \
  --manifest census-scenarios.tsv --trace-root captures \
  --output-root references --jobs 3 --resume
```

The runner is offline. Missing responses produce exact acquisition plans; it
does not download them, relax CORS, or change exclusions. Redirect hops are
intercepted individually, including cross-origin frame targets, with an
independent browser-request audit detecting missed interception and unexpected
request failures before browser shutdown. Capture uses the retained document
User-Agent.

Completion requires a bounded scroll to the bottom, five verified viewport
positions with unchanged mobile input/viewport settings, and loaded paintable
images. Actual offsets,
additional replay-clock turns, failures, and tool/input hashes are retained.
Resume requires all mandatory artifacts and matching hashes, not merely an
old success flag. A geometry change during the viewport sequence permits one
bounded recapture of all five views; the original images are preserved and
hashed too. Failed or corrupt attempts are retried without overwriting
their evidence. Access-denied, policy-refused, and unfinished pages remain
explicitly incomplete.

The diagnostic scope is `viewport-sweep-diagnostic-v2`. A coherent full-page
image is deliberately omitted: Chromium's full-page capture can clear touch
emulation while painting, selecting different CSS media rules. Enlarging the
surface can instead alter viewport units or omit offscreen pixels. Verified
viewport images remain accurate; comparison strips are labeled scroll
sequences, not stitched full-page screenshots. Old full-page receipts must
be recaptured rather than accepted as current evidence.

The full-document sweep advances additional clock turns. These captures are
diagnostic, not interchangeable with the ordinary manifest checkpoint oracle.
Compare native output only with matched clock turns and content anchors;
fractional scroll positions can show different content when layout heights
differ. Never move a fidelity floor using a mismatched comparison.

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
retained in private investigation records rather than the public source tree.

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
