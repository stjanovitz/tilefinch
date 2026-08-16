# Interactive candidate acceptance

The five newer sites remain a candidate suite. They are intentionally separate
from `benchmarks/acceptance-sites.tsv` until each replay qualifies under its
declared memory, JavaScript, content, and rendering envelope.

Run the suite against a caller-supplied, consolidated qualified replay corpus
outside the repository:

```sh
./benchmarks/run-candidate-interactive-acceptance.sh \
  build-root-dev \
  ../tilefinch-corpora/acceptance-candidates-qualified-CAPTURE_DATE \
  /tmp/tilefinch-candidate-interactive
```

`benchmarks/acceptance-sites-interactive-candidates.tsv` owns all site-specific
URLs, content expectations, and bounded launch budgets. The runner itself has no
host-name branches. Every run uses replay seed 42, executes
`psp-browser-interactive-lab` with explicit memory/script/tick ceilings, and
requires:

- a successful engine run and zero-byte teardown;
- a reconciling stable and teardown memory ledger;
- an allocator peak and failure count within the manifest ceilings;
- an exact 480 x 272 PPM output frame;
- the expected final document HTTP status and title;
- minimum laid-out height and retained body text;
- a first-page marker in the runtime DOM body-text trace;
- a separate required target marker somewhere in the retained replay bodies;
- manifest-declared minimum loaded scripts, DOM mutations, and completed
  fetch/XHR operations;
- an optional maximum script-failure count and an optional requirement that the
  JavaScript error be empty; and
- absence of a declared fallback-shell marker from the runtime DOM.

The runtime marker prevents a source document with the right title from passing
when the engine actually renders an error or blank shell. The separate corpus
check also catches a retained access-denied document replacing the intended
content. Neither is a pixel-similarity assertion. Visual reference review
remains a separate qualification step because a hard pixel hash would reject
harmless font and rasterization improvements.

The hydration fields are deliberately generic counters. For example, the
Mastodon row requires loaded modules, DOM mutation, a completed API operation,
and rejects its `please enable JavaScript` fallback text. These expectations are
manifest data; the runner and engine contain no Mastodon branch. Omitted
hydration columns use zero/disabled defaults, while the checked-in candidate
manifest requires positive evidence for every required dynamic site.

Each summary preserves the original result columns and appends loaded/failed
script counts, DOM mutations, completed asynchronous fetch/XHR operations, the
configured byte ceiling and remaining headroom, externally reserved bytes,
first-DOM/first-paint/completion timing, summed measured engine work, maximum
cooperative slice and phase, maximum compile/callback/frame time, Git revision
and worktree state, and the lab-binary SHA-256 (when a host SHA-256 tool is
available) and byte length. The same identity is written once to `run.meta`.
This does not claim host timings are PSP
predictions; it makes regressions and indivisible work visible before hardware
qualification.

## Visual references

`compare-reference-frame.py` accepts only a binary P6 candidate and a canonical,
non-interlaced 8-bit PNG reference. It validates file contents against the
suffix before decoding, so a JPEG saved with a `.png` name is a hard diagnostic,
not silently treated as a PNG or as missing evidence.

Normalize a correctly named JPEG, PNG, or PPM once before comparison:

```sh
python3 benchmarks/normalize-reference-frame.py \
  reference.jpg /tmp/reference-480x272.png
python3 benchmarks/compare-reference-frame.py \
  candidate.ppm /tmp/reference-480x272.png
```

The comparator keeps those raw RGB diagnostics and also reports PSP-normalized
RGB565 error, local and multi-scale luminance SSIM, one-pixel-tolerant edge F1,
foreground coverage, and blank-frame status. It remains diagnostic by default;
explicit thresholds or `--qualify` make a miss exit nonzero. JSON output and
optional independently measured geometry anchors are supported. See
[`../FIDELITY.md`](../FIDELITY.md) for metric definitions, exit
codes, qualification examples, and the geometry schema.

For a known legacy artifact whose suffix is wrong, the mismatch must be
acknowledged explicitly:

```sh
python3 benchmarks/normalize-reference-frame.py \
  legacy-reference.png /tmp/reference-480x272.png \
  --accept-mislabeled-input
```

Normalization verifies 480x272 geometry by default, decodes the generated PNG
again with the acceptance decoder, and rewrites one canonical RGB PNG. PNG and
PPM use the standard-library implementation. JPEG needs Pillow, ImageMagick,
macOS `sips`, or ffmpeg; the selected decoder is printed in the result.

`REPLAY_ROOT` must contain one subdirectory per `replay_dir` in the manifest.
The current development captures are scattered across several local roots; the
suite intentionally treats those as missing until the final qualified captures
are consolidated under one caller-supplied root. It never copies a corpus into
the repository.

Google currently has an explicit `external` block policy in the candidate
manifest. That policy is narrow: the engine must still run and tear down cleanly,
and the final result must match both the declared HTTP 403 and the declared
`Error 403` marker. Such a result is reported as `EXTERNAL_BLOCKED`, never
`PASS`. An engine failure, a missing corpus, another HTTP result, or a marker
without the declared status is a hard failure.

Exit status is `0` only when every selected candidate passes, `1` for any hard
failure, `2` for invalid inputs, and `3` only when all runnable rows pass while
one or more explicitly declared external rows are positively classified as
blocked. Set `CANDIDATE_SITE_FILTER` to one manifest name for a bounded
single-site iteration.
