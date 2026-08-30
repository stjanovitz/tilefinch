# Top-sites census

This directory contains an explicit, host-only compatibility census. It does
not run from CMake, CTest, the normal acceptance runner, or a release build.

The checked-in TSV is a dated transcription of Cloudflare Radar's public US
top-100 table. The runner excludes entries categorized only as CDN,
advertising, analytics, or APIs, then visits the remaining likely page origins
in Chromium at a 480×272 CSS-pixel mobile viewport, DPR 1, touch enabled, and
the same mobile compatibility user agent that Tilefinch sends. It retains
aggregate HTML, authored-CSS, and computed
layout counts, normalized values for a small decision-oriented CSS property
set, and API tokens found in JavaScript ranges that V8 reports as executed.
The API counts are compatibility signals rather than proof that a call
completed or that the entire API is required. It does not retain response
bodies, script or stylesheet bodies, cookies, or screenshots.

Run it only when deliberately refreshing the report:

```sh
node benchmarks/top-sites/test-census-features.js
node benchmarks/top-sites/run-census.js \
  --output /tmp/tilefinch-top-sites-census \
  --limit 100 --concurrency 4 --timeout-ms 12000 --settle-ms 800
```

`results.json` is the detailed ephemeral result and `report.md` is the
generated report. Review the failures and source date before acting on it;
live rankings and pages can change independently. Production replay bodies,
screenshots, and generated reports remain local-only.

Any visual comparison made from this census must verify DPR 1, the mobile user
agent, and a 480×272 output bitmap. Its recorded `innerWidth` is normally 480
when the page declares a mobile viewport and Chrome's standard 980-pixel
fallback when it does not; Tilefinch implements the same distinction. A
480-pixel bitmap made from a desktop response auto-shrunk into a mobile frame
is not a valid reference.

The local-only mobile reference helper enforces that contract:

```sh
node benchmarks/top-sites/capture-mobile-references.js \
  --output /tmp/tilefinch-mobile-references --limit 50
```

Use `--domain example.com` to refresh one affected reference without
recapturing the rest of the ranking.

After rendering matching 480×272 Tilefinch frames, compose pixel-exact rows
without padding or resampling:

```sh
python3 benchmarks/top-sites/render-tilefinch-mobile.py \
  --references /tmp/tilefinch-mobile-references \
  --output /tmp/tilefinch-frames
python3 benchmarks/top-sites/compose-mobile-comparisons.py \
  --references /tmp/tilefinch-mobile-references \
  --candidates /tmp/tilefinch-frames \
  --output /tmp/tilefinch-mobile-comparisons
```

Those commands preserve the original quick, top-of-page comparison. For a
deliberate full audit, add `--full-audit` to all three commands:

```sh
node benchmarks/top-sites/capture-mobile-references.js \
  --output /tmp/tilefinch-mobile-references --limit 50 --full-audit
python3 benchmarks/top-sites/render-tilefinch-mobile.py \
  --references /tmp/tilefinch-mobile-references \
  --output /tmp/tilefinch-frames --full-audit
python3 benchmarks/top-sites/compose-mobile-comparisons.py \
  --references /tmp/tilefinch-mobile-references \
  --candidates /tmp/tilefinch-frames \
  --output /tmp/tilefinch-mobile-comparisons --full-audit
```

Full mode captures the top, document midpoint, and bottom in the same Chrome
page. Before each Chrome checkpoint it disables smooth scrolling, scroll snap,
and scroll anchoring, then repeatedly requests the current checkpoint until two
bounded samples agree within two CSS pixels. The JSON records the requested,
pre-capture, and post-capture scroll positions plus the convergence status. If
a page keeps moving or changes extent during capture, the diagnostic bitmap is
written as `*-POSITION-invalid.png`; the canonical `*-POSITION.png` is removed
so an invalid viewport cannot silently enter a comparison strip. Check
`audit.valid` and `audit.invalidCaptures` before treating a full run as a valid
oracle.

Tilefinch uses the interactive lab's current script, cookie, and resource
pipeline and records those checkpoints separately for the raw page and the
product `--reader-mode` transform. It writes raw and Reader frames below
`tilefinch-frames/raw/` and `tilefinch-frames/reader/`, plus a bounded
`audit-summary.json`. Each Tilefinch checkpoint records its requested,
actual, desired, and maximum scroll positions. A capture farther than two
pixels from its current-document checkpoint is retained only as
`*-POSITION-invalid.ppm`; the compositor also requires the matching valid
summary record, so a stale or clamped frame cannot enter a strip. Middle
captures retry against their own freshly measured extent up to four times,
which handles bounded late-image reflow without accepting the wrong viewport. The
compositor creates per-site three-checkpoint strips
and per-position sheets below `mobile-comparisons/raw/` and
`mobile-comparisons/reader/`. Reader strips intentionally have no Chrome side:
they review Tilefinch's content-shape presentation rather than claim that a
browser-specific Reader implementation is an oracle.

The candidate summary also records host-only usability signals beside every
frame: retained body-text bytes, paint commands, links and controls, dominant
and foreground pixel shares, JavaScript termination, and Reader
classification/availability. Automatic blank-page Reader recovery is recorded
separately, including whether the complete bounded tree was applied, so a
recovered page cannot be confused with an ordinarily useful raw layout. A
bounded RAW classification preserves the raw
capture and records Reader as unavailable rather than manufacturing a Reader
tree or treating the page as a load failure. The raw top capture performs one
post-capture, read-only target probe: it reports whether the settled layout has
a link/control target and whether focus was already authored. It deliberately
does not move focus or activate the target, because either operation can
dispatch author JavaScript. Because this runs after the PPM is written, it
also cannot alter the visual oracle. These are triage signals, not a claim that an account flow or
arbitrary application is supported; a useful destination still needs manual
task qualification before it enters a release claim.

The live corpus remains opt-in. Capture/comparison tooling and its offline
check require Python 3 with Pillow available (the bundled Codex workspace
Python includes it). The tooling itself has a small offline check:

```sh
node benchmarks/top-sites/test-mobile-audit-tools.js
python3 benchmarks/top-sites/test-mobile-audit-tools.py
```

The separate `benchmarks/acceptance-sites-secondary.tsv` contains only five
diverse page targets selected from the census. It is also opt-in and must not
be wired into routine testing.

`benchmarks/fidelity-top-sites-candidates.tsv` is the corresponding opt-in
visual-diagnosis manifest. Its response bodies and Chrome references remain
under the git-ignored `fidelity/` tree; the checked-in manifest contains only
URLs, bounded replay settings, and trace digests. Candidate rows must not be
added to the fidelity floor gate until their hermetic captures have been
reviewed and the intended visual behavior is stable.
