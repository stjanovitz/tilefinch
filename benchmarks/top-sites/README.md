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

The separate `benchmarks/acceptance-sites-secondary.tsv` contains only five
diverse page targets selected from the census. It is also opt-in and must not
be wired into routine testing.

`benchmarks/fidelity-top-sites-candidates.tsv` is the corresponding opt-in
visual-diagnosis manifest. Its response bodies and Chrome references remain
under the git-ignored `fidelity/` tree; the checked-in manifest contains only
URLs, bounded replay settings, and trace digests. Candidate rows must not be
added to the fidelity floor gate until their hermetic captures have been
reviewed and the intended visual behavior is stable.
