# Host test modes

For contributors: the command-line modes that run one behavior of a host test
executable in isolation, and the host qualification lanes built on them. A
mode never replaces the complete suite
([Development](../DEVELOPMENT.md#test-suite)). Paths assume the optimized
`build-preset-release` tree.

## Probes and switches

Standalone C measurement probes must inherit the target's public compile
definitions and include paths, preferably through a CMake target linked to
`tilefinch_core`. `TILEFINCH_OWNER_CHECKS`, for example, changes the `Budget`
layout: a probe compiled without it and linked against an owner-checked
library is an ABI mismatch, not a pixel or memory oracle. When a probe
disagrees with CTest, compare its flags with the build's compile commands
before changing an expected result.

Diagnostic flags in `src/diagnostic_trace.h` are sampled once per translation
unit on the owner thread, so set them before launching the process. They are
constant false in `TILEFINCH_NO_TRACE` builds. Every switch is listed in
[Diagnostic switches](../DIAGNOSTIC_SWITCHES.md).

## Rendering, layout, and style

`tilefinch-tests --filter web-runtime` includes focus dependency checks for
boolean selectors and retained values, and exact cached-paint against
full-flow comparisons for translated pseudo decorations and simple focus
shadows. Geometry, complex effects, and spatial-band changes keep the
full-layout fallback.

`tilefinch-layout-tests` modes:

| Mode | Checks |
|---|---|
| `--visual-regression N` | one reduced visual regression: 1 compact percentage controls, 2 generated intrinsic spacing, 3 placeholder styling, 4 table-header defaults, 5 inline badge geometry, 6 generated mask sizing, 7 rounded border sides |
| `--auto-grid-align-only` | center/end alignment after content-sized grid rows finish sizing |
| `--fractional-tracking-only` | signed fractional tracking across inline boundaries and wrapping |
| `--inline-boundaries-only` | intrinsic spacing after generated glyphs, joined no-wrap fragments, valid whitespace/punctuation breaks, per-line ellipsis, marker refusal |
| `--nowrap-boundary-only` | the no-wrap checks above without icon sizing |
| `--generated-padding-only` | generated glyphs stay inside their inline owner's padding |
| `--page-font-leading-only` | downloaded-face line metrics without changing fallback leading |
| `--positioned-percentage-only` | percentage heights and vertical insets against an auto-height containing block |
| `--nested-flex-baseline-only` | first-item baseline propagation through an inline-block wrapper |
| `--css-replacement-image-only` | `content:url()` sizing, painting, and replacement of the original subtree through the budgeted image loader |
| `--unknown-media-dimensions-only` | default video/frame dimensions without an invented aspect ratio; canvas and authored ratios kept |
| `--button-content-centering-only` | vertical centering in definite-height flow buttons without moving positioned decorations or overriding flex/intrinsic-height alignment |
| `--oversized-style-cache-only` | scan-resistant retention at the style-cache ceiling and exact output after invalidation; the cache limits are unchanged |
| `--container-cache-only` | cached container-query probe/final passes against an uncached reference, including conditional pseudo-elements |

`tilefinch-style-index-tests` modes:

| Mode | Checks |
|---|---|
| `--attribute-names-only` | the per-sheet set of selector attribute names behind `data-*` style reuse (prefix lookup, escaped/namespaced opacity, rebuilds, bounds) |
| `--attribute-bench-only` | prints that set's build and query cost on a 5,000-rule sheet |
| `--transition-candidates-only` | conservative transition-selector rejection against the full matcher, including class changes and escaped/functional selectors |
| `--retained-handoff-only`, `--retained-nested-only`, `--retained-focus-only`, `--quoted-punctuation-only` | the retained style-cache review cases |

## Browser engine

`tilefinch-browser-engine-tests` modes:

| Mode | Checks |
|---|---|
| `--youtube-account-actions-only` | provider-supplied watched progress, exact resume links, deduplicated recent-history cards, and Like/Unlike confirmation, refusal, cancellation and control provenance, on synthetic responses (no live account is touched) |
| `--native-text-sync-only` | native text synchronization without duplicate relayout |
| `--fixed-images-only` | fixed-image publication, retained scaled paint commands, exact pixels against full flow, clean teardown |
| `--fixed-image-repaint-only` | repaint notification and focus continuity across image completion |
| `--image-admission-only` | input deferral before and between optional requests, exact final pixels, preserved retry counts, teardown of paused or partly admitted jobs |
| `--font-focus-policy-only [1-3]` | prompt Smart font-pressure relief, exact sibling-focus cache reuse, and static recovery after a real required-layout refusal (the number picks one) |
| `--computed-style-layout-only` | paint-style reads use the current cascade without forcing layout; geometry and changed sheets still synchronize |
| `--opacity-geometry-only` | paint-only opacity geometry reuse, selector/generated-content invalidation, mixed writes, repaint safety |
| `--font-staging-only` | optional-face publication, input cancellation, rollback, and retry |
| `--cleanup-drain-only` | wrapper cleanup drains at the frame loop's two-task budget (below) |
| `--pointer-search-only` | pointer down/up, committed click, native text replacement, implicit form submission, sliced navigation, and a rendered result link on a hostname-neutral fixture |
| `--script-free-journey-only` | the script-free search journey and the inherited-focus-style cache regression |
| `--provider-navigation-only` | provider navigation and cancellation, also under sanitizers |
| `--deferred-startup-only` | link and section interaction while images are pending, source and subtree replacement between pumps, cancellation and teardown ownership |
| `--background-interruption-only` | focus feedback precedes the author handler, the handler runs once, native menus stay usable; optional image-relayout refusal and shell retirement |

In opacity-geometry reports, `snapshot-builds` and `snapshot-hits` distinguish
real rebuilds from reuse; `synchronous-layouts` counts flush requests, not
full rebuilds.

`--cleanup-drain-only` drives a page through `browser_engine_advance_runtime`
at the PSP frame loop's two-task budget. Hosts advance every frame whether or
not author work is pending, so an idle page drains its queued wrapper cleanups
(and their DOM handle slots) without counting them as author work. When due
tasks take every slot, one cleanup is admitted ahead of the turn's tasks after
four starved turns, so such a page gives up at most one task slot in five.

### Captured-page replays

These take a captured trace and URL. They are performance probes, not
request-policy tests: response-keyed replay tolerates capture-era request
metadata.

- `--font-staging-replay TRACE URL` profiles staged font publication: elapsed
  time, the largest cooperative gap and its ending phase, checkpoints, and
  style-cache work. Parent-key misses and evictions separate inheritance
  changes from capacity churn. The font-usage line counts old text commands
  whose face changes, not affected subtrees or pixels. Top, middle, and bottom
  raster checksums run after the timed publication. Baseline fonts are
  prepared before loading, as on the PSP.
- `--navigation-staging-replay TRACE URL` runs incremental navigation with the
  PSP's 2 KiB parser slices, staged fonts, external stylesheets, and document
  JavaScript, then profiles font publication. Use it for initial-load work;
  the font-only probe does not load external resources. Set
  `TILEFINCH_TRACE_LAYOUT_PROFILE=1` for phase attribution and
  `TILEFINCH_TRACE_IMAGE_PROFILE=1` to split the image traversal into element
  styles, pseudo styles, and transport drain (validation PSP builds log these
  phases automatically). For device flow attribution, configure the
  validation tree with `-DTILEFINCH_PROFILE_LAYOUT_FLOW=ON`, rebuild its named
  target, read `tilefinch-layout-phase-flow`, then turn the option off and
  rebuild. Compare raster checksums as well as times: a completed input script
  on an error page is not a successful navigation.
- `--font-interrupt-replay TRACE URL DELAY_US` interrupts optional font
  publication `DELAY_US` after it starts and reports
  request-to-acknowledgement, acknowledgement-to-idle (rollback drain), and
  their total. It asserts that cancellation keeps the fallback page, font
  bytes, and relayout count, then moves focus, composites the host lab's
  native focus outline, and requires changed pixels. `font-input-feedback` is
  request-to-framebuffer time, not physical sampling or scanout latency. The
  probe fails if publication finishes before the interruption, so choose a
  shorter delay.
- `--navigation-interrupt-replay TRACE URL DELAY_US` applies the same check
  after incremental navigation with external stylesheets and scripts; prefer
  it for realistic page costs. `navigation-responsiveness` separates the
  longest pump from the longest cooperative gap inside it. A long pump can
  still service native controls; it does not make DOM edits safe while the
  layout transaction owns its scratch state.
- `--native-navigation-replay TRACE URL [RUNS]` profiles the default
  JavaScript-Off path with a 24 MiB Budget, bounded resources, and staged
  baseline fonts (`--native-navigation-strict-replay` uses 16 MiB). `RUNS`
  defaults to 1, at most 30. Each run checks navigation, three rendered
  viewports, and zero teardown ownership. Request-begin time is reported apart
  from processing gaps, because captured-body verification and host
  curl/proxy setup are not device stalls. A preview absent under pressure is
  `available=0`, not a zero-latency paint. `first-frame-us` includes the first
  committed render; the 128 idle pumps after it measure eligible optional
  work, not that every delayed resource settled.
- `--search-journey-replay TRACE URL` applies the pointer-search journey to a
  captured encyclopedia home page and the query `psp`, with the installed PSP
  admission settings (the qualification runner adds the 4 MiB startup
  window). The check needs an actionable article link, because an exact query
  may legitimately land on a disambiguation page.
- `--navigation-settle-replay TRACE URL [TURNS]` replays 240 wall-clock turns,
  or with `TURNS` that many logical 16 ms turns. The two are not equivalent
  amounts of initialization work.

Capture device behavior before treating a host-only pass as the fix for an
unobserved device failure.

## Web fonts

`psp-browser-interactive-lab --no-web-fonts` disables only page font loading,
for same-binary comparisons; built-in fonts, images, styles, and script
settings are unchanged. Compare first paint, commit, final memory, and font
usage. The post-run `web-font-usage` lists encoded and backend payload bytes
and the text commands selecting each face. Selection is not proof of a
visible gain, and backend bytes exclude encoded storage and Budget
bookkeeping. Shared faces report storage once (`shared=1` on later aliases).

`--web-font-mode on|smart|off` selects the persisted menu policy (the lab
defaults to Smart); use `on` for font-completion comparisons. Smart reserves
one eighth of the page budget, bounded to 512 KiB–3 MiB, plus the capped
incoming font working set before starting another load. Pressure fallback
latches per page and runs at a clean idle checkpoint, never from a JavaScript
allocation hook. A refused or interrupted fallback restores faces and the
incumbent layout. Allocation refusal stops automatic retries; input
interruption retries at a later idle turn.

`tilefinch-web-font-tests` modes:

| Mode | Checks |
|---|---|
| `--font-perf-only` | style retention, deferred face activation, shared-face lifetimes, exact cached advances |
| `--variations-only` | authored two-axis fixtures, weight/optical metrics, bounded instance reuse, dead-table removal, refusals while opening, changing coordinates, and loading glyphs |
| `--opentype-kerning-only` | bounded horizontal GPOS pairs/classes and extension tables across TTF, WOFF and WOFF2, script selection, shared-font lifetime, cache sizing, malformed tables, Budget refusals |

The kerning path borrows FreeType's immutable decoded tables; it does not copy
GPOS or expand class matrices. Default-language `kern` plans cover Latin,
Greek, and Cyrillic, falling back to DFLT. Contextual, mark-filtered,
placement, and variation positioning stay outside this pair-advance API and
keep the `kern` fallback. The same suite checks reuse across equivalent script
plans and pixel sizes, independent rounding for multiple lookups, and the
256-byte ASCII glyph-index cache.

## JavaScript runtime

`tilefinch-js-responsiveness-tests` modes:

| Mode | Checks |
|---|---|
| `--motion-cache-only` | bounded motion parsing across owner turns, focus/promise progress, class-only cache reuse, stylesheet/CSSOM invalidation, animation publication, teardown |
| `--xhr-buffered-events-only` | the synthetic network queue and buffered XHR upload/response order in a fresh realm, including listener-free states and final `loadend`; run it under the sanitizer preset to guard dispatcher stack growth |
| `--cssom-noop-only` | identical CSSOM writes skip mutation and layout invalidation; changed values, newly authorized styles, and DOM attribute writes still publish |
| `--coalesced-tokens-only` | the retained style-cache token coalescing case |
| `--css-name-only` | canonical CSS property-name fast paths, coercion, custom-property case, camel-case fallback, resistance to author changes to `RegExp.prototype.exec` |
| `--html-tag-case-only` | case-insensitive HTML tag queries and compound selectors; SVG names and class tokens stay case-sensitive |
| `--finalizers-only` | finalization cleanup does not starve promise reactions (below) |
| `--component-stack-only` | nested component connection and stylesheet adoption under the sanitizer stack, and that unbounded recursion still reaches the recoverable stack guard |
| `--external-compile-pressure-only`, `--task-time-slice-only` | external compile pressure and task time slicing |

`--finalizers-only` queues 330 cyclic finalization records beside an
eight-reaction promise chain. The chain must finish in one 64-job checkpoint
without running cleanup callbacks. Cleanups then run as low-priority engine
tasks, at most 16 per advance within the task budget, with a promise
checkpoint after each. The fixture also checks eventual cleanup and teardown
with undrained work. `TILEFINCH_TRACE_RUNTIME_STEPS` reports `runtime-cleanup`
apart from `runtime-checkpoint`. Cleanup is not startup work and must not
delay resource admission or page recovery.

Other JavaScript modes:

- `tilefinch-script-lazy-tests path/to/script.js [...]` inspects the given
  scripts instead of the default fixtures.
- `tilefinch-quickjs-oom-tests N` refuses allocation after `N` successful
  allocations (a count, not a byte budget).
- `tilefinch-quickjs-oom-tests --context-refusal-only` refuses each allocation
  of raw and full context creation, once and persistently, and checks that the
  runtime releases everything.

## Memory pressure

`tilefinch-memory-pressure-tests` sweeps initialization limits, HTML-fragment
parser failures, and every Budget allocation boundary in ten operations:
navigation, dynamic DOM mutation, Reader and Basic preparation, CSS changes,
rendering, history, container-query layout, deferred images, and JS-driven
DOM/query growth. Each case must recover with a fresh navigation and release
all owned bytes. Pass a scenario number (`0`–`9`) to run one operation, or a
scenario and allocation index to reproduce one refusal. The sweep verifies
these boundaries; it cannot prove every page is crash-free.

`tilefinch-image-decode-scaled-tests --only allocation-refusals` refuses each
PNG, JPEG, and WebP decoder allocation in turn and requires a byte-identical
retry with no retained ownership.

When measuring pressure coverage, count distinct allocation sites actually
refused, not attempts or lines executed by ordinary tests. For a repeatable
census, configure a disposable host Clang build with
`-fprofile-instr-generate -fcoverage-mapping` and
`-DTILEFINCH_BUDGET_CALLSITE_TRACE=ON`. Run the named tests with
`LLVM_PROFILE_FILE` set to a per-process `%p.profraw` path, capturing stderr,
merge the raw files with `llvm-profdata merge -sparse`, and run:

```sh
python3 scripts/budget-callsite-report.py \
  --binary <build>/libtilefinch_core.dylib --profile <merged.profdata> \
  --fault-log <test.log> --llvm-cov <llvm-cov>
```

The report inventories every direct first-party Budget
malloc/calloc/realloc expression in that core and separates source reach from
an actual injected refusal; label a partial run as partial. Its
`coverage_targets` track the resource, JS-module, media, and storage
families. A targeted campaign includes `tilefinch-stylesheet-resource-tests`,
`tilefinch-image-decode-scaled-tests`, `tilefinch-module-bytecode-tests`,
`tilefinch-media-mp4-tests`, `tilefinch-media-http-range-tests`,
`tilefinch-offline-app-limits-tests`, and
`tilefinch-session-site-storage-tests`; pass `--require-target-refusal
resource` (or another family name) to reject a campaign that never refuses
there. This is source-site coverage, not path coverage. The probe is off by
default and forbidden in PSP builds; site logs and profiles stay out of the
repository.

## Responsiveness rules

Total relayout time is not input latency; check cooperative gaps separately.
The native UI can update the retained frame at these checkpoints without
entering the DOM. Native computed-style inheritance participates in the task
watchdog rather than hiding an ancestor chain between VM polls. Layout keeps
its four-node quota for cheap work but cooperates at the next node boundary
after eight milliseconds; one node can still overrun, so this is a safe-point
rule, not a frame deadline. Do not infer PSP responsiveness from host times.

The interactive lab accepts `--stylesheet-count N` (1–32) for diagnostic
captures. Omitting it keeps the profile default (24 sheets at the lab's
24 MiB limit). It changes only the count allowance, so a higher-count capture
is not evidence that a page fits the PSP profile.

## Host browsing responsiveness lane

```sh
python3 benchmarks/run-host-browsing-responsiveness.py \
  --wikipedia-home-trace /path/to/home-capture \
  --wikipedia-article-trace /path/to/article-capture \
  --search-trace /path/to/home-search-capture
```

The runner builds the optimized host, then runs font rollback and retry,
visible focus and held scrolling, disclosure expansion, deferred images,
provider navigation, pointer search, native UI and supervisor, and media
presentation and state checks. They are composable host checks, not a
hardware playback claim: the lane never builds for PSP, launches an
emulator, plays audio, or contacts a live site. Captures are optional; the
synthetic gates run without them. Keep capture bodies and raw journey logs
out of source control. Output (raw logs, raster checksums, `summary.json`)
goes to the ignored build tree.

The script-free journey submits a native search form, selects a result,
expands an article, scrolls, and restores the results and the form through
Back, at 16 and 24 MiB. It checks the selected URL, zero owned memory at
shutdown, repeated Back/Forward restoration, usable focus while a delayed
navigation times out or is cancelled, and that a deferred image timeout
neither strands later images nor retires the page. Replacing an in-flight
request uses the frontend's cancel-then-begin sequence and must keep the
replacement's generation, destination, and controls without a stale error.
The provider journey renders with the ordinary eight-tile cache after
down/up input, verifies both Play destinations through repeated selections
and a shell replacement, and stops at the native-player handoff.

With captures, the lane also measures JavaScript-Off focus and scrolling in
the interactive lab, apart from the scripted stress lane
(`--skip-scripted-captures` runs only this capture lane). `summary.json`
reports median, p95, and maximum dispatch and paint time per command;
`focus-outline-us` is outline style resolution, not rasterization. Do not mix
page-load or resource-drain time into these samples. Timing is observational:
the gates assert completion, cache reuse, destinations, and pixels, not
microsecond thresholds.

`--startup-trace /path/to/capture --startup-url https://example.test/article`
adds delayed script initialization: 1,800 logical 16 ms turns
(`--startup-ticks 2..4096`) with installed-app admission (a 5 MiB base heap
plus the 4 MiB boot window). It checks focus rendering before and after
settling and requires completed dynamic scripts without uncaught errors.
Caught heap refusals are reported separately. The last dynamic-script
completion is measured from commit and does not prove that later timers start
no more work.

## Providers

`TILEFINCH_TRACE_PROVIDER=1` reports provider transform slices and timing in
host builds.

`tilefinch-browser-engine-tests --youtube-login-journey` covers synthetic sign-in form entry, cancellation,
synchronous and asynchronous redirects into the native YouTube provider, and
verified-login notifications across refreshes and account changes. The CTest
journey adds recommendation, subscription, Like/Unlike confirmation, and
sign-out checks. `tilefinch-youtube-login-tests` checks consent choices,
bounded cookie persistence, corruption and refusal rollback, removal, and
host symlink safety. The fixtures hold no real credentials or authenticated
captures, and no full provider HTML or script runs.

The same lane reports `watch-reuse` cold and warm Description and Comments
loads: request and body-byte counts, build steps, and peak extra Budget
ownership, from synthetic replay data. Tests compare cached and fresh HTML
byte for byte and cover link activation and scroll targets, cookie and
language changes, expiry, clear and reclaim, another video, cancellation,
refusal, and a failed-token retry. A 768 KiB fixture pins Description to zero
downloads and seven build steps after a normal watch load (an uncached
expansion takes sixty) and the initial watch build to its 114-step ceiling,
so reuse cannot add a blocking preflight.
