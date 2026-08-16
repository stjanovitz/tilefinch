# Top-sites census

This directory contains an explicit, host-only compatibility census. It does
not run from CMake, CTest, the normal acceptance runner, or a release build.

The checked-in TSV is a dated transcription of Cloudflare Radar's public US
top-100 table. The runner excludes entries categorized only as CDN,
advertising, analytics, or APIs, then visits the remaining likely page origins
in Chromium at 480×272. It retains aggregate HTML, authored-CSS, and computed
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

The separate `benchmarks/acceptance-sites-secondary.tsv` contains only five
diverse page targets selected from the census. It is also opt-in and must not
be wired into routine testing.

`benchmarks/fidelity-top-sites-candidates.tsv` is the corresponding opt-in
visual-diagnosis manifest. Its response bodies and Chrome references remain
under the git-ignored `fidelity/` tree; the checked-in manifest contains only
URLs, bounded replay settings, and trace digests. Candidate rows must not be
added to the fidelity floor gate until their hermetic captures have been
reviewed and the intended visual behavior is stable.
