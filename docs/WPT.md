# Web Platform Tests

Tilefinch runs focused panels from the upstream Web Platform Tests repository
through its real interactive engine. The panels are deliberately selected:
the PSP cannot host the complete WPT server and browser automation stack, but
unchanged upstream assertions remain a valuable independent oracle for the web
features Tilefinch claims.

The exact upstream revision is pinned in `benchmarks/wpt/REVISION`. Sparse
checkout paths and per-test expectations are committed under
`benchmarks/wpt/`; upstream test bodies are not copied into this repository.

## Preparing the checkout

By default the scripts expect WPT in the sibling directory `../wpt`:

```sh
./benchmarks/prepare-upstream-wpt.sh
```

The script clones or updates a clean checkout, detaches at the pinned revision,
and materializes only paths used by Tilefinch's manifests. It refuses to alter
a dirty checkout. A different destination may be passed as the first argument.

## How the runner works

`benchmarks/run_upstream_wpt.py` loads one manifest row at a time through
`psp-browser-interactive-lab` and injects a small Tilefinch report adapter. The
page's own `testharness.js` assertions remain authoritative. The runner records
test status, subtest status, completion, timeout, unsupported prerequisites,
and the engine's bounded diagnostics in a versioned JSON result.

This is not a screenshot substitution for WPT. Script-driven tests execute in
QuickJS with Tilefinch's DOM, style, layout, event, navigation, and rendering
paths. Reftests are routed through the visual machinery named by their
manifest panel.

Every manifest entry declares its expected outcome. An unexpected pass is
reported as drift just like an unexpected failure; support boundaries should
be updated deliberately rather than hidden by a permissive runner.

## Test lanes

| Lane | Runner | Main surface |
|---|---|---|
| compact report card | `run-web-platform-correctness.sh` | small repository-owned HTML/CSS/DOM gate |
| selected upstream | `run-upstream-wpt.py` with `selected.tsv` | core HTML, cascade, layout, and rendering |
| expanded | `run-upstream-wpt-expanded.sh` | broad CSS and HTML correctness |
| DOM interaction | `run-upstream-wpt-dom-interaction.sh` | focus, forms, selection, events, navigation |
| text flow | `run-upstream-wpt-text-flow.sh` | inline layout, wrapping, decoration, writing behavior |
| component reactivity | `run-upstream-wpt-component-reactivity.sh` | custom elements, shadow DOM, observers |
| modern component APIs | `run-upstream-wpt-modern-component-apis.sh` | browser APIs used by component frameworks |
| customizable select | `run-upstream-wpt-customizable-select.sh` | modern select structure and interaction |
| secondary-site CSS | `run-upstream-wpt-secondary-sites.sh` | focused stylesheet and cascade boundaries |
| scroll interaction | `run-upstream-wpt-scroll-interaction.sh` | overflow, snapping, anchoring, focus scrolling |
| modern mobile CSS | `run-upstream-wpt-modern-mobile-css.sh` | mobile presentation and interaction properties |
| intrinsic/clamp/visibility | `run-upstream-wpt-intrinsic-clamp-visibility.sh` | intrinsic sizing, line clamp, content visibility |

The path lists are larger than the executable manifests because they include
support files, reference pages, fonts, stylesheets, and scripts required by
selected tests.

## Typical commands

Build the optimized lab first:

```sh
cmake --build build-preset-release -j8
```

Run a focused lane:

```sh
./benchmarks/run-upstream-wpt-modern-mobile-css.sh \
  build-preset-release ../wpt /tmp/tilefinch-wpt-mobile
```

Run the general selected manifest directly:

```sh
python3 benchmarks/run_upstream_wpt.py \
  --lab build-preset-release/psp-browser-interactive-lab \
  --wpt-root ../wpt \
  --manifest benchmarks/wpt/selected.tsv \
  --output /tmp/tilefinch-wpt-selected
```

Each output directory contains `results.json` plus bounded per-case artifacts.
The JSON is the machine-readable authority; terminal summaries are for quick
orientation.

## Adding coverage

1. Choose the smallest unchanged upstream file that isolates the behavior.
2. Add its directory or support files to the appropriate sparse path list.
3. Add a manifest row with a reasoned expected result and sufficient bounded
   ticks/heap for that test, not for an imagined desktop workload.
4. Run the focused lane against the pinned checkout.
5. Verify a regression test fails when the implementation is removed or
   reverted.
6. Keep expected failures only for explicit unsupported behavior; do not use
   them to absorb flaky completion or engine crashes.

If Tilefinch must adapt a harness assumption—such as replacing server-only WPT
metadata—the adapter belongs in the runner and must not change the assertion
being tested.

## Reading failures

Classify a failure before changing engine code:

- **assertion failure** — the test completed and Tilefinch disagreed;
- **harness failure** — the test never reached its assertions because a WPT
  server feature or unsupported setup was required;
- **resource failure** — a support file was absent from the sparse checkout;
- **budget refusal** — the configured PSP-like limit rejected the page;
- **timeout** — bounded work did not settle; inspect whether the page is making
  progress before raising a quota;
- **render mismatch** — inspect geometry and pixel artifacts through the
  [fidelity workflow](FIDELITY.md).

A larger heap, tick count, or timeout is not automatically a fix. It is
appropriate only when the test's declared workload fits the product contract
and the existing bound is demonstrably below that workload.

## Relationship to other gates

WPT answers “does this behavior agree with an independent standards test?” It
does not answer every release question:

- repository unit tests exercise allocation failure, rollback, and PSP
  ownership conditions which upstream tests cannot see;
- the fidelity scoreboard compares complete site geometry and pixels;
- response-keyed replay proves both browsers consumed the intended inputs;
- the PSP cross-build exposes 32-bit ABI, text-size, and hot-symbol costs;
- hardware tests remain required for firmware and timing behavior.

The canonical optimized host CTest suite includes Tilefinch's selected web
platform gate and runner tests. The optional upstream panels are run when work
touches their surface or when broad compatibility is being qualified.
