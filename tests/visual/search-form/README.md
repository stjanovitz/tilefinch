# Search form pixel regression

`tilefinch-search-form-visual-tests` renders the repository-authored
`tests/fixtures/search-form-visual.html` with JavaScript disabled, no network
resources, pinned repository fonts, and a 480×272 device/CSS viewport. It
compares every RGB pixel with `search.png` (empty form) and `results.png`
(populated query, button and article snippets after scrolling 320 pixels).

It also renders `tests/fixtures/search-results-scroll-visual.html`, an
eight-result document with wrapped headings, redirect/section links,
highlighted query text, snippets, metadata, pagination and a footer:

- `results-middle.png`: scroll 240, including a wrapped result snippet.
- `results-lower.png`: scroll 816, well below the first screen.
- `results-end.png`: scroll to the actual bottom (currently 1248), including
  the last result, pagination and footer.

After visiting the bottom, the test returns to scroll zero and requires an
exact match with the original top frame. All prior PPMs are removed before
each render so a stale sample cannot conceal a missing capture. The original
two form references are unchanged.

The new captures were visually inspected alongside live script-free
Wikipedia full-text results for `psp` at 480×272 on 2026-09-09, including
scroll positions 544, 1088 and 1632 and the footer. Result titles, snippets
and metadata remained separated and readable. Live content is not committed
or used as a network-dependent golden; this remains a reduced layout gate,
not a claim that every live search result or downloaded image is covered.

The reduced markup preserves the nested percentage-width text input,
`appearance:none`, anonymous CSS table, and 1% submit column which exposed
the Wikipedia search layout regression. It is not a redistributed live page,
nor a claim that these engine goldens are Chrome fidelity references. The
separate fidelity scoreboard retains that role.

Run after building the optimized host lab:

```sh
ctest --test-dir build-preset-release -R tilefinch-search-form-visual-tests --output-on-failure
```

Candidate PNGs and the render log remain in
`build-preset-release/search-form-visual/` for inspection on failure. The test
never updates references. A deliberate reference change requires inspecting
the candidate images, explaining the improved behavior, and re-running the
full host suite; do not accept a new reference merely to clear a failure.

Verified against pre-fix layout commit `1428c21f`: the visual gate fails with
6,286 changed pixels on the empty form and 6,390 on the results view. Restoring
`0c0145e2` makes both captures pixel-identical. No private capture corpus is
required for this check.

Scroll-gate negative control: temporarily hiding the sixth result while
preserving its layout makes `results-lower` fail with 16,759 changed pixels.
Removing that fault restores the exact match; the references were not changed.
