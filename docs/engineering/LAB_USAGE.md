# Running the desktop labs

This is the complete command-line reference for the two host frontends. Build
them first with [DEVELOPMENT.md](../DEVELOPMENT.md); the PSP EBOOT is covered
by [Device qualification](DEVICE_QUALIFICATION.md).

## Static renderer (`psp-browser-lab`)

```sh
./build-preset-release/psp-browser-lab \
  --fixture fixtures/demo.html \
  --output-dir frames \
  --limit-mb 48 \
  --js-limit-mb 8
```

The program prints phase-by-phase memory telemetry and tile-cache statistics. It writes PPM frames because that format has no runtime dependency. Most image tools can convert them to PNG.

Use `--skip-js` to measure HTML/CSS/layout/rendering independently when a real page's scripts require browser APIs that Tilefinch does not implement. `--dump-links links.tsv` exports word-level hyperlink hit regions, while `--dump-layout layout.tsv` exports the retained paint list. On a live load, relative and protocol-relative links are made absolute. The final telemetry lines are intended for machine-readable comparisons.

`--challenge-diagnostic` is an explicitly diagnostic navigation policy. It retains an HTTP error page, records `cf-mitigated`, `Accept-CH`, `Critical-CH`, and server headers, runs only the page's initially present inline scripts, and reports external script URLs inserted into the DOM. It does not claim the error page is the requested site and does not fabricate or submit a challenge result.

The interactive lab can make a legitimate managed-challenge attempt with `--url ... --fetch-scripts`. It retains secure/HttpOnly response cookies, retries a safe GET once when `Critical-CH` requests truthful PSP client hints, and sends only the named high-entropy hints to the origin that requested them. A cross-origin retry redirect suppresses those hints for the rest of that redirect chain while truthful low-entropy hints continue normally. The lab loads scripts inserted by the bootstrap under normal quotas and reports challenge/network/clearance state without cookie values. It never copies clearance from another browser. The ordinary compatibility User-Agent carries iPhone/WebKit/Safari routing tokens so large sites choose their bounded mobile document, while also naming `PlayStation Portable` and `Tilefinch`; `navigator.platform` and low-entropy Client Hints remain explicitly PSP/Tilefinch rather than claiming the capabilities of Safari or Chrome.

### Managed challenges as a standards qualification

A production managed challenge is an external compatibility observation, not a
fixture whose changing program or answer belongs in Tilefinch. Diagnostic runs
may record API lookups, task failures, network shape, cookie names, and the
eventual clearance state; they must not patch the challenge, synthesize its
payload, import clearance from another browser, or add a site-specific client.

The probes and command scripts used for these observations are kept together,
with what each one reports, in
[`tests/fixtures/challenge-lab/`](../../tests/fixtures/challenge-lab/README.md).

The current qualification establishes these boundaries:

- Browser/API comparison found and fixed a real same-origin iframe difference:
  a parent may read and call the child `Window.eval` when the frame has
  `sandbox="allow-same-origin"` without `allow-scripts`. The sandbox suppresses
  child-owned script execution; it does not hide that same-origin Window
  property. The managed program now completes without JavaScript or callback
  errors, posts its verification response with HTTP 200, receives a Secure,
  HttpOnly `cf_clearance` cookie, and automatically navigates again with that
  cookie present.
- The edge nevertheless returns another managed challenge. In that
  qualification, five consecutive challenge documents—including the
  second managed challenge—completed their verification requests without a
  JavaScript or callback error, while the Secure/HttpOnly clearance cookie was
  retained and resent. Each follow-up still received a fresh challenge rather
  than the application. This rules out cookie persistence, navigation handoff,
  and an immediate ECMAScript exception as the remaining cause; it does not
  prove which server-side risk signal or custom-engine policy declined the
  session.
- A later re-observation found the edge escalating Tilefinch to
  Turnstile's interactive checkbox (the widget posts `interactiveBegin`;
  the parent's "Verification successful. Waiting for chatgpt.com to
  respond" is template text shown while it waits for a person). Tilefinch
  could not show that checkbox: three generic bugs hid it (width/height
  hints mapped onto a `<div height="10 em">`, a shrink-wrapped grid that
  ignored its implicit columns, and iframe snapshots painted at
  display/frame scale and never re-taken after a resize). With those
  fixed, the published force-interactive test sitekey renders like
  Chromium and a click inside the frame completes it. Completing the real
  checkbox is a human step: the lab must not click it, and whether the
  edge then serves the application remains unobserved.
- Chromium controlled through the browser lab reaches the ordinary ChatGPT
  application without being assigned this challenge, so it cannot provide an
  instruction-by-instruction control run. It remains useful for focused Web API
  comparisons, which is how the iframe `eval`, Window/HTMLDocument branding,
  XHR inheritance, Streams surface, and built-in reflection differences were
  isolated.
- The challenge has a roughly 9.1 MiB transient QuickJS peak but settles near
  6.1 MiB. Under the exact PSP script quotas, a 5 MiB steady heap plus the
  existing Budget-charged 4 MiB boot window completes verification within a
  roughly 24.9 MiB total page peak. The window is an allocation ceiling rather
  than a reservation and is returned after a bounded post-boot collection or
  realm teardown. Physical-device timing and memory remain a separate gate.
- Removing `Worker` selects the page's unsupported-browser branch, so the
  constructor and lifecycle still need to be standards-shaped. Tilefinch now
  runs bounded Blob workers in separate QuickJS realms, with isolated
  ECMAScript intrinsics and globals, a dedicated `WorkerGlobalScope`, truthful
  navigator/location views, bounded structured-clone mailboxes, asynchronous
  startup and delivery, and deterministic cancellation of timers, fetches, and
  the realm on termination or navigation. The implementation remains a
  first-use bootstrap so ordinary pages do not compile worker machinery.
  One observed probe constructs a Blob worker and immediately terminates it
  before the queued startup task. Its tiny `"you" === "bot"` body is
  consequently never evaluated and is not a verdict delivered to Tilefinch.
  Later challenge generations do run workers; a missing-capability trace over
  those runs reported no property absent from Tilefinch's supported Worker
  surface. Add Worker APIs from their standards contracts and focused tests,
  not by guessing from obfuscated challenge generations.
- A differential QuickJS test isolated a separate standards defect exposed by
  the managed program: shrinking a sparse array could skip indexed properties
  when property deletion compacted its shape. Compact-character arrays changed
  which stale indices survived, explaining a pre-fix challenge callback
  `TypeError`, but disabling that optimization did not make array shrinking
  correct. The engine now restarts its bounded deletion scan after shape
  compaction and the minimized sparse-array sequence is a regression test.
- A normal HTTP fixture loads Cloudflare's public Turnstile API, invokes its
  named global `onload` callback, and exposes `render`, `execute`, `getResponse`,
  `remove`, and `reset` as functions. The verification POST also completes with
  HTTP 200. External-script loading and Turnstile's loader callback are therefore
  not the missing step.
- The failure-adjacent API trace constructs `MutationObserver`,
  `PerformanceObserver`, `Blob`, and `Worker`; calls the crypto, history, and
  performance surfaces; inspects `ReadableStream.prototype` and
  `navigator.cookieEnabled`; and reads `navigator.gpu`. It never calls a WebGPU
  method. The last access is therefore a fingerprint observation, not evidence
  that this challenge requires WebGPU execution. Do not manufacture a GPU
  object or expose WebGPU until Tilefinch has an honest bounded implementation.

`--trace-page` reports three complementary views: `first=` is the bounded
first-use order with routine intrinsic noise removed, `tail=` preserves the
operations immediately before the first callback failure, and `types=` records
coarse return shapes without serializing page objects. The trace freezes at the
first failure so later recovery work cannot overwrite the causal window.

The current Worker profile deliberately admits classic retained-Blob workers.
It provides the bounded subset above plus timers, `fetch`, URL/Blob, encoding,
streams, crypto, performance, Trusted Types, and classic `importScripts`, all
under the document's immutable origin/CSP/network policy. Module workers remain
a separate later milestone; they must not be claimed until module fetch,
credentials, dependency graphs, and cancellation are implemented within fixed
PSP bounds.

Qualification for that milestone is generic: curated Worker/Blob Web Platform
Tests for realm and prototype isolation, constructor/error ordering, immediate
URL revocation, task-versus-microtask ordering, messaging, termination, CSP,
network cancellation, quota recovery, and navigation teardown; reflected
built-in names/descriptors for the APIs the runtime exposes; and a synthetic
server proof that any legitimately issued Secure/HttpOnly clearance-style
cookie survives the response and accompanies the following navigation. Only
after those pass should the production challenge be observed again. Success is
not promised: Cloudflare documents custom engines and embedded browsers as
having limited support, so a standards-correct Tilefinch may still be declined
by server policy.

Live document loads use the same hard allocation budget as parsing and rendering:

```sh
./build-preset-release/psp-browser-lab \
  --url https://en.wikipedia.org/wiki/PlayStation_Portable \
  --output-dir frames/wikipedia \
  --limit-mb 13 \
  --max-download-kb 4096 \
  --dump-links frames/wikipedia-links.tsv
```

`--reader-profile none` is the default, so ordinary browsing uses author CSS
without hostname-specific overrides. Reader mode is explicitly opt-in:
`--reader-profile auto` recognizes Wikipedia, Hacker News, Reddit, ChatGPT,
and NYTimes documents, while `wikipedia`, `hacker-news`, `reddit`, `chatgpt`,
and `nytimes` select one profile directly. By default the loader fetches only
the top-level HTML document. `--fetch-css` enables bounded external stylesheet
loading under `--max-stylesheets`, `--max-css-kb`, and `--max-css-file-kb`;
defaults are 6 files, 1 MiB aggregate, and 384 KiB per file. The configured
DejaVu sans, serif, and bounded sans-italic faces are loaded under
`--max-font-kb 1536`; `--sans-font`, `--serif-font`, and
`--sans-italic-font` select alternatives, and `--no-ttf` exercises the
zero-font fallback.

The interactive lab's `--reader-mode` option instead exercises the product's
bounded content-shape classifier and generic Reader stylesheet after the page
has committed. It cannot be combined with `--user-css`,
`--hide-cookie-banners`, or experimental compressed sections, keeping the
capture's presentation source unambiguous.

`--fetch-images` separately enables bounded raster/SVG, CSS-background, and CSS-mask loading. Defaults bound the pass to 64 unique attempts, 512 KiB aggregate encoded input, 256 KiB per response, and 512 KiB retained decoded SVG/mask data. Raster files remain compressed and are decoded one at a time through the tile cache; a source decode may use at most four times the output quota, capped at 8 MiB for sub-8-MiB profiles, and is repeatedly halved before painting when its RGBA target exceeds the quota. Larger decompression candidates are skipped before allocation. Responsive `<picture>` and width-descriptor `srcset` select a viewport-sized supported source. Change the limits with `--max-images`, `--max-image-kb`, `--max-image-file-kb`, and `--max-decoded-image-kb`. Duplicate URLs share resources, element and pseudo-element masks and CSS background images use the same quotas, unsupported or failed optional resources do not abort the page, and every retained buffer uses the shared page budget. Background `cover` and `contain` use centered bounded sampling; repetition is supported, and the common two-gradient layered-background form paints in source order. Arbitrary layer counts and independent per-layer URL positioning remain unsupported. External stylesheets may recursively import up to four levels while sharing the same URL-count, byte, timeout, cache, and page-memory limits; conditional imports use the bounded media/supports evaluators. Quoted and single-`attr(name)` pseudo-element content is decoded locally under a 64-string/63-byte-per-string cap; live attribute values reuse bounded DOM bytes. When external resources are enabled and FreeType 2.14.3 or newer is available, page fonts are limited to eight attempts, 256 KiB aggregate encoded input, 96 KiB per response, two regular/bold families, two ordered sources per face, and 256 KiB of backend allocation per face. Unsupported WOFF2, collections, CFF outlines, inline data URLs, failed CORS requests, and quota misses fall back without aborting the page. The interactive lab has a separate quota-controlled external-script pipeline.

A bounded mobile-CSS run is:

```sh
./build-preset-release/psp-browser-lab \
  --url https://en.wikipedia.org/wiki/PlayStation_Portable \
  --reader-profile auto \
  --fetch-css \
  --fetch-images \
  --max-stylesheets 6 \
  --max-css-kb 1024 \
  --max-css-file-kb 384 \
  --limit-mb 16 \
  --skip-js \
  --scroll-all \
  --output-dir frames/wikipedia-mobile
```

`--scroll-all` walks from top to bottom in half-viewport increments, writes `scroll-manifest.tsv`, rejects blank frames, and verifies that the top frame is identical after tile eviction. It saves top/middle/bottom PPMs; repeatable `--save-scroll Y` options add semantic checkpoints. `--viewport-width` and `--viewport-height` select the device render surface within guarded ranges; page viewport metadata selects the CSS layout viewport. `--no-render` runs fetch/parse/style/layout/link analysis while retaining the framebuffer reservation but writing no images.

`--psp-profile strict` selects a 16 MiB content ceiling, 4 MiB JavaScript ceiling, 1 MiB cumulative script-source allowance, and eight tiles. `--psp-profile realistic` selects a 24 MiB content ceiling, a measured 5 MiB JavaScript ceiling, and a 2 MiB source allowance. The VM value is a ceiling rather than an up-front reservation. The public `BrowserEngine` profile additionally records an 8 MiB minimum non-page reserve for UI, stacks, TLS/backend state, sockets, libc metadata, and fragmentation; that reserve is deliberately unavailable to page content. `browser_config_apply_psp_memory_profile()` applies the same strict or realistic policy to embedders. Both profiles enable generic allocation-pressure adaptation: as owned memory approaches guarded stage reserves, the loader may skip JavaScript, reduce stylesheet and image quotas, or retain four rather than eight tiles. Each decision is logged, depends only on remaining budget, and can be selected or disabled explicitly with `--adaptive-resources` or `--no-adaptive-resources`. Explicit `--limit-mb`, `--js-limit-mb`, or `--tile-count` values override the selected profile. `--navigation-stress N` repeatedly replaces a page through the bounded navigation owner and fails if teardown does not return to the pre-stress allocation level. `--resource-stage-ms` bounds each complete external CSS or image phase (100–60000 ms); requests inside the phase remain four-way concurrent and preserve document order when applied. CSS telemetry reports batches, the usable first batch, deadline cancellation, and total elapsed time.

Very large static documents can exercise the separate, explicitly experimental
path without changing the default architecture. With no explicit section,
the option is adaptive: it delegates documents whose estimated full-document
working set fits the current budget to the existing pipeline and selects the
compressed path only after a bounded prefix crosses generic byte/markup
pressure thresholds. `--experimental-section N` forces sectional mode for
diagnostics and arbitrary-section selection:

```sh
./build-preset-release/psp-browser-lab \
  --url https://html.spec.whatwg.org/ \
  --max-download-kb 32768 \
  --psp-profile strict \
  --experimental-compressed-sections \
  --experimental-section 619 \
  --output-dir frames/whatwg-section-619
```

The run reports the generic section and exact-anchor indexes. Sectioned network
responses go directly through a 16 KiB delivery buffer into exact-sized,
independently allocated compressed blocks; fixtures are read normally and then
converted. Small adaptive responses are retained only until the router commits
to the ordinary full-document pipeline. Each materialization
reconstructs the original document prefix and bounded ancestor context, then
uses the ordinary DOM, CSS, resource, layout, JavaScript, controller, and tile
pipelines. The interactive lab swaps at scroll boundaries and supports exact
fragment jumps, history, reload, and refetched cross-document return. A 512 KiB
hard cap bounds heading-free spans, and the lexical index avoids natural
heading splits inside tables, lists, form groups, inline flex/grid islands, and
simple tag/class/ID layout islands declared by bounded embedded CSS.
Bounded external CSS is preflighted with a style-only probe, rather than a
throwaway page/layout commit, and can transactionally refine those
layout-island boundaries before the observable commit. A late island starts a
fresh section when half the current allowance is already occupied.
For source-backed JavaScript queries, the selector Bloom index only rejects
sections that cannot match; candidate sections still use the normal semantic
matcher so node identity and source order remain exact. The Bloom index is
built lazily, and document-root matches remain local because `html`, `head`,
and `body` precede every sectional body descendant.

Structural indexing cooperates and can cancel at each independently decoded
block. The interactive lab uses those same boundaries to make the requested
section provisionally raster-ready before the remaining structural index is
complete, then performs the ordinary script/resource-enabled final commit.
`experimental-initial-load` reports transfer, first-section, provisional
commit/paint, full-index, and final-ready timestamps; `experimental-store-timing`
reports structural index work, yields, and maximum slice latency separately
from time spent in provisional-paint progress callbacks.

The retained compact/medium paired corpus can be compared with three median
runs per path using `./benchmarks/run-experimental-paired.sh`. Its defaults use
`benchmarks/experimental-paired-acceptance.tsv` and the matching interactive
replay corpus; every case rejects a missing or undersized captured main body
before timing either path.

The JavaScript realm survives adjacent swaps. Bounded stable-ID and anonymous
structural focus, form, selection, listener, handler, observer, and dirty-DOM
state is restored. Scripts execute in source document order with parser
visibility, `document.currentScript` identity, defer/module ordering, and
document-wide count/byte quotas. Simple and complex selectors search the
logical source in document order up to the existing 128-result DOM bound;
`NodeIterator`, `TreeWalker`, cross-section node relations, and bounded root
`textContent` use the same source-backed view. Bounded `html`, `head`, and
`body` attributes persist across destructive swaps; body or document-element
content replacement immediately retires the stale logical source without
discarding the compressed store. Retention-table
overflow follows logged, deterministic LRU/FIFO degradation, and a forced hard
split can still break cross-boundary layout relationships. Incompressible input
uses raw independent blocks and can be rejected by the shared memory budget.
The experiment remains opt-in; omitting
`--experimental-compressed-sections` follows the full-document architecture.
Speculative expanded HTML is deferred until after paint and only enabled after
sustained directional movement. Each swap logs state capture, decode,
teardown, conditional JavaScript collection, parse/build, restoration,
scroll readiness, and first-tile latency. Aggregate average/max fields make
the same run useful in the host lab and in physical PSP validation.

Mobile viewport telemetry distinguishes `width=device-width`, numeric widths,
and the standards-compatible 980 px legacy layout viewport for pages without a
declaration. Root horizontal overflow is retained and covered by a neutral
fixture. CSS media queries and viewport units, JavaScript viewport globals,
root CSSOM geometry, scrolling, hit testing, fixed/sticky overlays, and tile
composition share that CSS coordinate space. The renderer creates a bounded
device-scaled visual clone only when the layout and device viewports differ.
Viewport resolution and CSS/device conversion are owned by one immutable
`ViewportContext` value that is passed to style, layout, JavaScript,
navigation, controller input, and rendering. The older scalar viewport entry
points remain as compatibility wrappers. Replacing a scaled tile-cache layout
is transactional: if its visual clone or required overlay allocation fails,
the previously renderable layout and tiles remain active and the caller gets a
failure result.

## Interactive runtime lab (`psp-browser-interactive-lab`)

`--no-javascript` disables both the page scripts and the runtime, matching
the device's site JavaScript-Off control. Merely omitting `--fetch-scripts`
skips downloaded author scripts but leaves the runtime enabled. The native
search/edit/submit/link-focus/Back regression in
`tests/fixtures/no-javascript-search.commands` runs in the standard interactive
acceptance gate without JavaScript.

A page that sets `globalThis.pocSummary` has it printed as
`javascript summary="..."`, which is how small host micro-benchmarks report.
`benchmarks/fixtures/computed-style-reads.html` is one: it times
`getComputedStyle()` creation, reads and membership tests, and carries its run
command and reference numbers in its header. It is a before/after comparison
tool, not a gate.

### Work vector (`tilefinch-work`)

Host wall time does not transfer to the PSP's 333 MHz in-order MIPS core, so
a faster lab run is **not acceptance evidence**. Iterate on deterministic
work counts instead, and use PPSSPP (running the real PSP binary) and the
device to calibrate what a unit of each costs there. One record carries
them, printed identically by every tier:

```text
tilefinch-work: label=<mark> js.work_units=163959 js.polls=17 ... fetch.replay_served=1
```

- The lab prints it for `work [LABEL]`, after the tables of
  `profile [LABEL]`, and once as `label=final` in the closing summary.
- PSP validation builds print it at every input-script mark (label = mark
  name), under PPSSPP and on the device
  ([INPUT_SCRIPT_HARNESS.md](INPUT_SCRIPT_HARNESS.md)). Shipping PSP builds
  compile the record and its tallies out.

Every value is a plain integer count, or `n/a` for a counter the build does
not compile. There are no timings in it; they stay in their existing lines.
Field names are append-only. Counters are **cumulative for the current
document**: the tallies and the session-wide relayout and replay counters
are rebased when a navigation begins (`navigation_begin`; a cancelled
navigation rebases too), and the `js.*`/`dom.*` fields belong to the
committed page realm, which a commit replaces (a mark taken while a new
document streams still reads the previous realm). Child-frame realms are
not included. Subtract consecutive marks for the work between them.

| Field | Counts | Source |
| --- | --- | --- |
| `js.work_units` | QuickJS interrupt budget consumed: one unit per interrupt check (calls, backward jumps, bounded compile and native loops), over every realm of the page runtime (workers included) | `JS_GetWorkCounters` (vendored engine) |
| `js.polls` | interrupt-handler polls (one per 10,000 units per context) | same |
| `js.gc_runs` | QuickJS collections (`JS_RunGC`, automatic or explicit) | same |
| `js.calls` | bytecode function entries and resumes; `n/a` unless `PSP_BROWSER_JS_CALL_COUNTS` or `PSP_BROWSER_JS_OP_COUNTS` | same |
| `js.bytecode_ops` | interpreter opcodes dispatched; op-count builds only | same |
| `js.float64_boxes` | float64-tagged values the engine created (a boxed soft-float double on the PSP); op-count builds only | same |
| `js.lazy_compiles`, `js.lazy_bytes` | lazy function bodies compiled on first call, and their source bytes | `JSLazyFunctionStats` |
| `js.native_calls` | calls of profiled host natives; `n/a` unless the sampling profiler is on (`TILEFINCH_TRACE_JS_PROFILE=1` on the host; on by default in validation builds), since only it wraps natives | profiler native table |
| `js.native.<name>` | the five most-called natives, by calls then registration order (profiler on only) | same |
| `js.attribute_writes` | native `setAttribute` calls (not cleared by profile reports) | DOM bridge |
| `js.allocs`, `js.alloc_bytes` | QuickJS allocator malloc + realloc calls and the bytes they requested | `budget_quickjs_pool_activity` |
| `js.source_bytes` | script source admitted to the realm | script quota |
| `js.module_compiles`, `js.module_restores` | modules compiled from source, and restored from cached bytecode | runtime result |
| `dom.mutations` | native DOM mutations | runtime result |
| `dom.mutation_records` | MutationRecords queued for observers | bootstrap `retentionStats` |
| `dom.observer_visits` | observers examined per routed mutation | same |
| `style.resolutions` | layout style resolutions, every pass (previews and cancelled passes included) | `LayoutContext` at release |
| `style.cache_hits`, `style.cache_misses` | the split of those between the layout style cache and full resolution (see below) | same |
| `style.rule_queries`, `style.rule_candidates` | rule-index lookups (layout and DOM style queries) and the rules they returned | stylesheet |
| `style.var_lookups`, `style.var_cache_hits`, `style.var_cache_misses` | custom-property lookups and their cache outcome | stylesheet |
| `style.selector_matches`, `style.selector_hits` | whole-selector match attempts (rule matching, `matches`/`querySelector`) and successes | selector matcher |
| `layout.passes`, `layout.commands` | completed layout builds and the draw commands they produced | layout job |
| `layout.fast_relayouts`, `layout.full_relayouts` | relayouts since the navigation began | `NavigationPerformance` |
| `raster.tiles`, `raster.commands` | tiles rasterized and the draw commands drawn into them | tile cache |
| `raster.glyph_misses` | tile-cache glyph cache misses | same |
| `raster.frames` | frames composed | same |
| `fetch.load_bytes` | response body bytes the page load pumped | load scheduler |
| `fetch.image_bytes` | encoded image bytes loaded for the page | page image stats |
| `fetch.replay_served` | trace-replay records served since the navigation began | `fetch_trace_replay_served_count` |
| `parse.html_bytes` | HTML bytes fed to the streaming parser (`--fixture` pages bypass it) | document stream |
| `parse.css_bytes` | CSS text bytes compiled (stylesheets and `<style>` elements) | stylesheet parser |

What is and is not deterministic:

- Identical execution gives an identical record. `tests/test_work_vector_determinism.py`
  (`tilefinch-work-vector-determinism-tests`) runs a local fixture journey
  and a `--deterministic-replay-seed` HTTP replay twice each and requires
  identical vectors; 24 concurrent runs under load also matched.
- `style.cache_hits`/`style.cache_misses` are keyed by node address, so their
  split can move by one when allocation interleaving differs; compare
  `style.resolutions` and pass `--ignore style.cache_` to `--check-equal`.
- Page script that reads the clock can change behaviour between runs; use
  `--deterministic-replay-seed` (seeded clock and random) for such pages.
  Without it, `js.float64_boxes` can differ by a few (a timestamp that
  happens to be integral is not boxed).
- `--psp-profile strict|realistic` time-slices each advance (16 ms of host
  time), so a slower or faster build (an op-count build, a code change)
  splits tasks across turns differently and a busy page's vector moves by a
  few percent even with a seed (chatgpt.com's send window: up to ~10% in
  `js.work_units`). For A/B across builds, omit `--psp-profile` (the lab
  execution profile has no slice) and pass the explicit memory limits; two
  builds with identical page behaviour then print identical `js.*` fields.
- Turning the profiler on changes `js.allocs`/`js.alloc_bytes` and a few
  `js.work_units` (its reports and native wrappers): compare runs with the
  same profiler setting.
- On PPSSPP and the device, `raster.*` and relayout counts follow the frame
  cadence (a slower target composes fewer frames per mark), and every
  time-sliced step can split differently; the JS, style and parse fields are
  the ones to calibrate across tiers. PSP tallies are 32-bit (a per-document
  count wraps past 4G).

`tools/work_vector_report.py` reads the records from any lab, PPSSPP or
device log:

```sh
python3 tools/work_vector_report.py run.log                 # values per label
python3 tools/work_vector_report.py --steps run.log         # mark-to-mark deltas
python3 tools/work_vector_report.py before.log after.log    # B and B - A per label
python3 tools/work_vector_report.py --check-equal --ignore style.cache_ a.log b.log
```

`--fields js.` narrows any mode to one group. `--check-equal` is the n=2
determinism check: it exits 1 and lists every differing field unless both
runs printed the same labels with the same values.
`scripts/run-buffered-reply-replay.py` keeps each run's vectors in its
manifest and prints how later runs differ from the first.

**Opcode and float64 counts** are a permanent opt-in engine build, never
timed (the counters sit in the interpreter dispatch and every float64
construction). Configure a separate tree, and delete it when done (an
in-tree directory that `CMakePresets.json` does not name needs the
override):

```sh
cmake --preset release -B build-opcounts \
  -DPSP_BROWSER_JS_OP_COUNTS=ON -DTILEFINCH_ALLOW_BUILD_DIR=ON
cmake --build build-opcounts --target psp-browser-interactive-lab -j8
```

`PSP_BROWSER_JS_OP_COUNTS` defines `CONFIG_TILEFINCH_OP_COUNTS` for the
engine only: `js.calls`, `js.bytecode_ops` and `js.float64_boxes` then read
numbers instead of `n/a`; every other field is unchanged. The default build
compiles none of it. Call counts alone come from `PSP_BROWSER_JS_CALL_COUNTS`
builds (which also fill the profiler's per-function calls table).

An op-count engine also keeps a per-opcode dispatch histogram. With
`TILEFINCH_JS_OPCODE_HISTOGRAM=1` every `work`/`profile` record is followed
by `tilefinch-opcodes: label=L op=NAME count=N` lines (cumulative, most
frequent first); subtract consecutive labels for a window's opcode mix.

**Engine micro-benchmark.** `tilefinch-js-bench` (host) and boot.cfg
`validation_js_bench=N` (PSP validation build; see the input-script
harness) run the same synthetic kernels, collection, and compile of a trace's
JavaScript records, each timed on the platform clock with the engine's work
counters, and print `tilefinch-js-bench:` lines. `tools/js_bench_compare.py
host=A ppsspp=B device=C` joins reports into ratio tables: a PPSSPP column
over the host is the instruction-count ratio, a device column over PPSSPP the
effective CPI. The host tool's `--only a,b`, `--scale N`, `--repeat R` and
`--trace DIR` select kernels, iterations, best-of repeats and the trace.

`psp-browser-interactive-lab` exercises the persistent layers together. It retains JavaScript and session state, can advance the bounded timer clock, load quota-controlled same-origin scripts, repeat navigation to exercise HTTP validators and the script cache, drive controller focus/edit/activation, follow GET/POST form actions, and render the resulting page:

`--forced-dark` enables the same role-aware page-color mapping used by the PSP
night mode. It is intended for deterministic visual captures: page surfaces
and text are remapped while image pixels remain untouched.

For URL navigation, the persistent lab owns the same bounded external
stylesheet and image pipeline as the static renderer: at the default 24 MiB
limit, at most 24 stylesheets (2,112 KiB total, 768 KiB each) and 24 images
(1.5 MiB encoded total, 512 KiB each, 3 MiB decoded) with a 15-second
per-resource timeout; a limit of 16 MiB or less with adaptive resources
tightens those to 4 stylesheets (512 KiB total, 192 KiB each) and 12 images
(768 KiB encoded, 256 KiB each, 1.5 MiB decoded). `<link rel=preload
as=style>` responses use a separate lane (the same count, half the stylesheet
byte total), so preloads can never refuse an active stylesheet; an active link
for the same URL, mode and credentials replays the preloaded body and takes
over its charge. Resource counts and bytes are printed in the final status. Page-owned assets are destroyed on
navigation and reloaded after a DOM relayout; the user stylesheet is retained
as a session-level cascade layer instead of being lost on that rebuild.

DOM geometry in persistent navigation is layout-backed. Element rectangles,
client/offset dimensions, scroll extents, and nested `scrollTop`/`scrollLeft`
state come from bounded native layout boxes. `overflow`, `overflow-x`, and
`overflow-y` values of `auto`, `scroll`, and `hidden` establish clipped scroll
boxes, while `clip` establishes the same paint/hit boundary without exposing a
scroll offset; nested offsets affect descendant geometry and hit testing. The tile
renderer omits overflow-subtree commands from immutable document tiles and
recomposes them through a clipped overlay pass. Nested element scrolling
therefore moves pixels as well as DOM geometry without invalidating the base
page tiles. The same box-model pass distinguishes client and offset sizes and
supports content/border-box sizing plus maximum width/height constraints.

Page `fetch()` and asynchronous `XMLHttpRequest` use a cooperative libcurl-multi
scheduler. It permits at most four active same-origin transfers, eight queued
completions, 512 KiB per response, and 2 MiB of aggregate response reservation.
That runtime scheduler is one view of the page-wide 16-slot domain shared with
resource and child-frame runtime views; a simultaneous one-slot document load
makes the per-navigation transient scheduled-transfer maximum 17.
Each browser tick gives socket polling at most 4 ms and delivers at most the
runtime callback budget, so network work cannot take over the event loop.
Fetch `AbortSignal`, `XMLHttpRequest.abort()`, and XHR timeouts cancel the native
easy handle, release its reservation, deliver the appropriate terminal event or
DOMException, and ignore any late completion. Navigation destroys all active
handles and retained response buffers. Synchronous
XHR remains available only when a page explicitly requests `async=false`.

```sh
./build-preset-release/psp-browser-interactive-lab \
  --url http://127.0.0.1:8765/interactive.html \
  --fetch-scripts --reload 3 \
  --ticks 2 --tick-ms 10 \
  --focus-next 3 --type LAB \
  --output interactive.ppm
```

The deterministic fixtures include classic blocking/`async`/`defer` scripts, static and delayed dynamic module imports, a JSON `fetch`, cookies, validators, and a POST echo. The loopback regression verifies script order, conditional 304 reuse, cookie request/response transport, controller editing, actual form submission, and zero tracked bytes at teardown.

The browser-session asset cache is both entry- and byte-bounded. HTTP-backed
entries distinguish fresh and stale responses, honor `max-age`, `no-cache`,
`must-revalidate`, `immutable`, and `no-store`. It supports the transport's
stable `Vary: Accept-Encoding` representation and conservatively rejects
unsupported variants, including `Vary: *`. Stale CSS and
scripts use validators when available. Replacement allocates transactionally,
so an allocation failure leaves the prior usable entry intact.

The same executable also has a persistent PSP-style command loop. It keeps the
page, JavaScript runtime, cookies, history, tile cache, focus, and scroll state
alive while commands are read from a file or standard input:

```sh
./build-preset-release/psp-browser-interactive-lab \
  --fixture fixtures/interactive.html \
  --commands fixtures/lab-loop.commands \
  --loop-output-dir interactive-frames \
  --output interactive-final.ppm

./build-preset-release/psp-browser-interactive-lab \
  --url https://news.ycombinator.com/ \
  --user-css profiles/hacker-news.css \
  --interactive --loop-output-dir hn-session
```

Commands include accelerated `up`/`down`, `page-up`/`page-down`, `top`,
`bottom`, focus movement, screen-coordinate `tap`, `activate`, text editing,
selector clicks, timer ticks, direct navigation, reload, back/forward, status,
and frame rendering. PSP aliases include `dpad-up`, `dpad-down`,
`dpad-left`, `dpad-right`, `cross`, `circle`, `ltrigger`, and `rtrigger`.
Focus movement automatically scrolls the focused link or control into view,
and each history entry retains its own scroll position.
Same-document DOM relayouts retain the focused node, compare the old and new
display lists, and invalidate only intersecting cached tiles. Mutations that do
not change paint commands preserve every tile; stylesheet/link/image source
changes deliberately take the full rebuild path.
Focused links and controls receive a compositor-level white/blue outline in
loop frames; page display lists and cached tiles remain untouched. Use
`--no-loop-capture` to keep rendering and cache telemetry active without
retaining every intermediate PPM. The `mark-steady` command starts retained
memory min/max/growth sampling for long-session plateau checks.
The `script-report` command prints the JavaScript heap census and the
compile/restore counters of the page that is live at that point; the
post-load report covers only the first document of a page that reloads itself.
`--module-bytecode-cache-kb N` overrides the in-memory module bytecode ceiling
(0 disables it); `--reload 1` loads the page twice through the engine, which
is how a revisit's restores are measured.
`--module-cache-dir DIR` adds the persistent tier: modules missing from the
in-memory cache are restored from files in `DIR` when their key matches;
`--module-cache-write` also stores newly compiled modules there (never
overwriting a file). The two runs of a cold/warm comparison use the same
`DIR`, the first with `--module-cache-write`. Directory accounting visits at
most eight entries per maintenance slice (2 ms soft time limit, 4,096 entries
total). Writes wait for accounting to finish; idle work continues the scan
after presentation/resource work. Compilation never removes entries: a full
cache (a maximum-size module might not fit, or 512 files) declines the write,
and idle maintenance then removes this build's oldest records by
modification time, eight per slice, until the directory is at or under 75%
of both ceilings, rescanning for the next sixteen candidates as needed. A
scan that fails or reaches the entry limit defers writes and is retried
after 64 idle slices, doubling to 4,096. Only the tier's own file names are
ever removed, and a read-only tier removes nothing. Explicit cache clearing is
bounded by the same total-entry limit and reports incomplete removal. The
`javascript-module-bytecode-disk` report line times the whole synchronous
disk path (`load-us`, split into `read-us` and `verify-us`) apart from
deserialization (`restore-us` on the line above) and the copy into RAM
(`promote-us`); compare cold/warm runs by those, not `restore-us` alone.
`--no-progressive-first-paint` provides a same-build control for measuring the
provisional first-viewport tradeoff; final rendering and resource policy are
unchanged.

The host PSP frontend simulator installs the same asset, button-input, and
RGB565 presentation callbacks expected from a future PSP frontend. Its
accelerated session advances 30 minutes of logical browser time and enforces a
five-minute wall-clock ceiling:

```sh
./benchmarks/run-platform-session.sh build-dev /tmp/platform-session
```

This deliberately is not a CTest, development-test, or port-readiness member;
run it only when explicitly qualifying the simulator. The deterministic fault
recovery and selected neutral Web-platform checks are separate and fast:

```sh
./benchmarks/run-failure-recovery.sh build-dev /tmp/failure-recovery
./benchmarks/run-web-platform-correctness.sh build-dev /tmp/web-platform
```

The fault run proves timeout, TLS, truncated-response, cancellation, and
allocator recovery in one process against a loopback fixture. The selected
manifest is intentionally described as a small engine regression suite, not
as an upstream WPT conformance claim.

A separate opt-in lane runs a pinned, sparse 89-file subset of the actual
upstream Web Platform Tests side by side with those local regressions:

```sh
./benchmarks/prepare-upstream-wpt.sh /tmp/tilefinch-wpt
./benchmarks/run-web-platform-side-by-side.sh \
  build-dev /tmp/tilefinch-wpt /tmp/tilefinch-web-platform
```

Its Acid-style report card executes 68 upstream `testharness.js` pages and
pixel-compares 21 upstream reftest pairs, grouped into 24 HTML/CSS feature
panels. Upstream failures are compatibility measurements rather than routine
CTest failures; adapter/harness errors still fail the command. See
[WPT.md](../WPT.md) for the pinned revision, selection rationale,
baseline, strict mode, and current harness boundary.

A separate 65-page exploratory lane measures DOM tree APIs, event dispatch,
mutation observers, CSSOM View geometry, and focus/form interaction:

```sh
./benchmarks/run-upstream-wpt-dom-interaction.sh \
  build-dev /tmp/tilefinch-wpt /tmp/tilefinch-wpt-dom-interaction
```

It uses the same pinned checkout and static adapter, remains outside CTest,
and is not expected to be green while its newly exposed compatibility work is
being assessed.

The retained large-page corpus exercises incremental navigation, progressive
paint, and clean pressure rejection without making live requests:

```sh
./benchmarks/run-streaming-corpus.sh \
  build-preset-release \
  /path/to/streaming-corpus \
  /tmp/streaming-corpus
```

See [STREAMING_NAVIGATION.md](STREAMING_NAVIGATION.md) for the stream
contract, script/resource lifecycle, differential and fault coverage, memory
comparison, corpus results, and PSP integration boundaries.

The completed 16/24 MiB ownership, pressure, compact-layout, cooperative-work,
acceptance, and large-document qualification is documented in
[PSP_ENVELOPE.md](PSP_ENVELOPE.md).

For offline compatibility analysis, `psp-browser-interactive-lab --fixture PAGE --probe-script SCRIPT` evaluates an in-memory instrumented copy of a locally captured external script against the retained page runtime at the isolated `https://fixture.test/` origin. It traces missing global/document/element/API properties, swallowed XHR errors and unhandled Promise rejections, and prints a token-safe bounded DOM outline. It never modifies the saved script or sends a page-specific verification/application transaction.

The repository also contains an authorized Turnstile compatibility fixture using
Cloudflare's documented always-pass dummy sitekey. Start the fixture server and
run:

```sh
python3 fixtures/server.py
./build-preset-release/psp-browser-interactive-lab \
  --url http://127.0.0.1:8765/turnstile.html \
  --fetch-scripts --ticks 140 --tick-ms 100 \
  --focus-next 1 --activate --follow-action \
  --output turnstile.ppm
```

`--trace-frames` adds an isolated missing-property trace to the challenge frame;
it is diagnostic and is not used for the final truthful attempt. The current
uninstrumented run loads the official API and frame, completes the normal
challenge, receives Cloudflare's documented dummy token, submits the form, and
gets `TURNSTILE PASS ERRORS none` from Siteverify using the documented test
secret. See [DEVICE_QUALIFICATION.md](DEVICE_QUALIFICATION.md) for what this
host-side proof can and cannot establish.
