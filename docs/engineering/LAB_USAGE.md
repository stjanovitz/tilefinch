# Running the desktop labs

This is the command-line reference for the two host frontends,
`psp-browser-lab` (static renderer) and `psp-browser-interactive-lab`
(persistent runtime). Build them first with
[DEVELOPMENT.md](../DEVELOPMENT.md); the PSP EBOOT is covered by
[Device qualification](DEVICE_QUALIFICATION.md). Signing the lab into a test
account is covered by [Private account lab](PRIVATE_ACCOUNT_LAB.md); replay
and reference capture by [Replay and reference lab](REPLAY_LAB.md).

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

The interactive lab's closing summary reports `bot-wall detected=yes site=... vendor=... status=...` when the committed top-level document is a bot-protection wall: a refusal status (403, 405, 429 or 503, or one vendor's 202) with a known vendor signature in its headers or title (the table is in `src/bot_wall.c`), or a refusal-status page that paints nothing after its script was refused. The PSP chrome raises the same two-line notice ("<site> blocked Tilefinch / Bot protection: this site may not work") instead of leaving a silent blank page. Detection is generic, reads only the response and the committed page, and never attempts the challenge.

The interactive lab can make a legitimate managed-challenge attempt with `--url ... --fetch-scripts`. It retains secure/HttpOnly response cookies, retries a safe GET once when `Critical-CH` requests truthful PSP client hints, and sends only the named high-entropy hints to the origin that requested them. A cross-origin retry redirect suppresses those hints for the rest of that redirect chain while truthful low-entropy hints continue normally. The lab loads scripts inserted by the bootstrap under normal quotas and reports challenge/network/clearance state without cookie values. It never copies clearance from another browser. The lab sends the same User-Agent and per-site identity exceptions as the PSP (`src/site_identity.c`).

### Managed challenges as a standards qualification

A production managed challenge is an external compatibility observation, not a
fixture whose changing program or answer belongs in Tilefinch. Diagnostic runs
may record API lookups, task failures, network shape, cookie names, and the
eventual clearance state; they must not patch the challenge, synthesize its
payload, import clearance from another browser, or add a site-specific client.
Fix what such a run exposes from the standards contract and a focused test,
never by matching the challenge program.

The probes and command scripts used for these observations are kept together,
with what each one reports, in
[`tests/fixtures/challenge-lab/`](../../tests/fixtures/challenge-lab/README.md).

`--trace-page` reports three complementary views: `first=` is the bounded
first-use order with routine intrinsic noise removed, `tail=` preserves the
operations immediately before the first callback failure, and `types=` records
coarse return shapes without serializing page objects. The trace freezes at the
first failure so later recovery work cannot overwrite the causal window.

Live document loads use the same hard allocation budget as parsing and rendering:

```sh
./build-preset-release/psp-browser-lab \
  --url https://example.test/article \
  --output-dir frames/article \
  --limit-mb 13 \
  --max-download-kb 4096 \
  --dump-links frames/article-links.tsv
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
has committed. It cannot be combined with `--user-css` or
`--hide-cookie-banners`, keeping the capture's presentation source
unambiguous.

### Images

`--fetch-images` separately enables raster/SVG, CSS-background, and CSS-mask
loading. Defaults limit the pass to 64 unique attempts, 512 KiB aggregate
encoded input, 256 KiB per response, and 512 KiB retained decoded SVG/mask
data. Raster files remain compressed and are decoded one at a time through
the tile cache; a source decode may use at most four times the output quota,
capped at 8 MiB for sub-8-MiB profiles, and is repeatedly halved before
painting when its RGBA target exceeds the quota. Larger decompression
candidates are skipped before allocation.

Responsive `<picture>` and width-descriptor `srcset` select a viewport-sized
supported source. Change the limits with `--max-images`, `--max-image-kb`,
`--max-image-file-kb`, and `--max-decoded-image-kb`. Duplicate URLs share
resources; element and pseudo-element masks and CSS background images use
the same quotas. Unsupported or failed optional resources do not abort the
page, and every retained buffer uses the shared page budget. Background
`cover` and `contain` use centered sampling; repetition is supported, and
the common two-gradient layered-background form paints in source order.
Arbitrary layer counts and independent per-layer URL positioning remain
unsupported.

### Stylesheets and generated content

External stylesheets may recursively import up to four levels while sharing
the same URL-count, byte, timeout, cache, and page-memory limits; conditional
imports use the media/supports evaluators. Quoted and single-`attr(name)`
pseudo-element content is decoded locally under a 64-string/63-byte-per-string
cap; live attribute values reuse the DOM's bytes. The static renderer uses
its configured local fonts; use the [interactive lab](#page-fonts) to
exercise page-provided fonts and its separate quota-controlled
external-script pipeline.

A bounded mobile-CSS run is:

```sh
./build-preset-release/psp-browser-lab \
  --url https://example.test/article \
  --reader-profile auto \
  --fetch-css \
  --fetch-images \
  --max-stylesheets 6 \
  --max-css-kb 1024 \
  --max-css-file-kb 384 \
  --limit-mb 16 \
  --skip-js \
  --scroll-all \
  --output-dir frames/article-mobile
```

`--scroll-all` walks from top to bottom in half-viewport increments, writes `scroll-manifest.tsv`, rejects blank frames, and verifies that the top frame is identical after tile eviction. It saves top/middle/bottom PPMs; repeatable `--save-scroll Y` options add semantic checkpoints. `--viewport-width` and `--viewport-height` select the device render surface within guarded ranges; page viewport metadata selects the CSS layout viewport. `--no-render` runs fetch/parse/style/layout/link analysis while retaining the framebuffer reservation but writing no images.

`--psp-profile strict` selects a 16 MiB content ceiling, 4 MiB JavaScript ceiling, 1 MiB cumulative script-source allowance, and eight tiles. `--psp-profile realistic` is the PSP application's own configuration, taken from the `BROWSER_PSP_APP_*` constants that `src/psp_boot_config.c` and the PSP app use (`browser_config_apply_psp_app_defaults()`): a 32 MiB page envelope, a 5 MiB JavaScript heap with the app's 4 MiB boot window, GC growth and array-cap engine knobs (an explicit environment value wins), 256 scripts, memory-based script admission with a 16 MiB source ceiling and 4 MiB per file, a 60-second script timeout, an 8 MiB document cap, four history entries, a 640 KiB session cache, 24 tiles, and the app's stylesheet and image limits. A lab replay therefore admits what the device admits without explicit limit flags. The VM value is a ceiling rather than an up-front reservation. The public `BrowserEngine` profile additionally records an 8 MiB minimum non-page reserve for UI, stacks, TLS/backend state, sockets, libc metadata, and fragmentation; that reserve is deliberately unavailable to page content. `browser_config_apply_psp_memory_profile()` applies the same strict or realistic policy to embedders (realistic there is `browser_config_apply_psp_app_defaults()` too). Both profiles enable generic allocation-pressure adaptation: as owned memory approaches guarded stage reserves, the loader may skip JavaScript, reduce stylesheet and image quotas, or retain four rather than eight tiles. Each decision is logged, depends only on remaining budget, and can be selected or disabled explicitly with `--adaptive-resources` or `--no-adaptive-resources`. Explicit `--limit-mb`, `--js-limit-mb`, or `--tile-count` values override the selected profile. `--navigation-stress N` repeatedly replaces a page through the bounded navigation owner and fails if teardown does not return to the pre-stress allocation level. `--resource-stage-ms` bounds each complete external CSS or image phase (100–60000 ms); requests inside the phase remain four-way concurrent and preserve document order when applied. CSS telemetry reports batches, the usable first batch, deadline cancellation, and total elapsed time.

Mobile viewport telemetry distinguishes `width=device-width`, numeric widths,
and the 980 px legacy layout viewport for pages without a declaration;
[Viewport and page zoom](VIEWPORT.md) describes how the layout viewport is
resolved and shared.

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

### Page fonts

The interactive lab's resource pipeline loads page-provided TrueType, WOFF,
and WOFF2 fonts, with variable-font weight and optical-size selection and
OpenType pair kerning. The default build uses the pinned FreeType;
`-DPSP_BROWSER_SYSTEM_FREETYPE=ON` selects a system copy, which must meet the
configured version floor and drops WOFF2, and
`-DPSP_BROWSER_ENABLE_WEB_FONTS=OFF` removes page fonts from the build.

`BrowserConfig` defaults allow eight attempts, 256 KiB aggregate encoded
input, 128 KiB per response, and 256 KiB retained backend allocation per face.
The realistic PSP profile lets a single response use the same shared 256 KiB
encoded allowance; strict mode keeps the 128 KiB per-response limit. There
are at most eight family identities, six weight faces per family, and two
ordered source alternatives per face; those capacities do not increase the
attempt or byte allowances. Inline `data:` fonts use the same admission
limits and CSP checks. Unsupported or malformed fonts, failed permission
checks, and quota refusals keep fallback text available rather than aborting
the page.

The lab defaults to Smart font policy; `--web-font-mode on|smart|off` selects
the policy and `--no-web-fonts` disables only author fonts, not the built-in
faces. See [Web fonts](HOST_TEST_MODES.md#web-fonts) for same-binary comparison
switches, memory accounting, and focused tests.

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
  ([Input script reference](INPUT_SCRIPT_REFERENCE.md#reading-the-trace)). Shipping PSP builds
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
  identical vectors.
- `style.cache_hits`/`style.cache_misses` are keyed by node address, so their
  split can move by one when allocation interleaving differs; compare
  `style.resolutions` and pass `--ignore style.cache_` to `--check-equal`.
  An extra miss can be one more full resolution, so `style.rule_queries`
  moves in the same direction by at most the miss difference, and
  `style.rule_candidates`/`style.selector_*` move with it. The determinism
  test checks those against the miss difference.
- Page script that reads the clock can change behaviour between runs; use
  `--deterministic-replay-seed` (seeded clock and random) for such pages.
  Without it, `js.float64_boxes` can differ by a few (a timestamp that
  happens to be integral is not boxed).
- `--psp-profile strict|realistic` time-slices each advance (16 ms of host
  time), so a slower or faster build (an op-count build, a code change)
  splits tasks across turns differently and a busy page's vector moves by a
  few percent even with a seed. For A/B across builds, omit `--psp-profile` (the lab
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
`validation_js_bench=N` (PSP validation build; see
[Validation records](VALIDATION_RECORDS.md#engine-micro-benchmark-before-boot)) run the same synthetic kernels, collection, and compile of a trace's
JavaScript records, each timed on the platform clock with the engine's work
counters, and print `tilefinch-js-bench:` lines. `tools/js_bench_compare.py
host=A ppsspp=B device=C` joins reports into ratio tables of elapsed time.
They are not instruction counts or cycles per instruction: each host runs
different machine code, and PPSSPP does not model the PSP's caches. Use them
to find kernels that cost disproportionately more on one platform, with the
same source, iteration counts, allocator and instrumentation settings on
every side and repeated runs kept; they do not predict a whole-page device
speedup. The host tool's `--only a,b`, `--scale N`, `--repeat R` and
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
(768 KiB encoded, 256 KiB each, 1.5 MiB decoded). The image count bounds
network work: icon-sized inline-SVG rasters (64x64 or less) and data: URLs of
at most 4 KiB do not spend it, but still pay the decoded and encoded byte
quotas and the 128-node tracking limit. `<link rel=preload
as=style>` responses use a separate lane (the same count, half the stylesheet
byte total), so preloads can never refuse an active stylesheet; an active link
for the same URL, mode and credentials replays the preloaded body and takes
over its charge. Where the per-file cap is 512 KiB or more (large-sheet
profiles, the PSP app's included), it is a floor rather than a limit: one
response may grow to 2 MiB while the page Budget can stage about six times
its size beside the 2 MiB layout reserve and the requests in flight, and a
sheet that memory no longer parses keeps its complete rules up to the cap.
Such sheets also keep up to 2,304 DOM-relevant rules past their 768-rule
allowance while the Budget has 512 bytes per rule to spare. A stylesheet that
reaches its per-file cap is applied up to
its last complete top-level rule: the rule, declaration, string, comment or
unclosed `@media`/`@supports` block the cap cut through is dropped, the cut
response is kept only in the document's ledger (never the HTTP cache), and the
`resources` line reports `css-truncated=SHEETS/APPLIED/RECEIVED` bytes (the PSP
validation log prints the same field). Resource counts and bytes are printed in the final status. Page-owned assets are destroyed on
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
and frame rendering. `page-zoom PERCENT` relays the page out at a Page zoom
step in place, the way the PSP setting does, and prints a `loop-page-zoom`
line with the result, CSS width and scale; `--page-zoom PERCENT` loads the
first page at that zoom. `reader-on`/`reader-off` and `basic-on` switch to the
extracted views the way Page tools does (`basic-on` prints a `loop-basic` line
with the visited and emitted nodes, truncation and preparation time). The
`census` command prints the site-census report of the live page: the
first-paint timebase, the share of draw commands and boxes below the first
and second screen, each image's decoded and painted size, and the Reader
analysis (see [tools/site-census/README.md](../../tools/site-census/README.md)). PSP
aliases include `dpad-up`, `dpad-down`,
`dpad-left`, `dpad-right`, `cross`, `circle`, `ltrigger`, and `rtrigger`.
`--parser-script-time-limit-ms N` scales the parser-stage script circuit
breaker (20 s on the PSP) down to the host: a host replay runs scripts about
a hundred times faster, so roughly 150 ms reproduces where PPSSPP at
111 MHz trips it.
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
It also prints the page's `heavy-page` line (class none, content, shell or
over; script bytes; estimated PSP start time; waiting scripts; planned and
reachable heap; visible text measured at commit; see
`include/tilefinch/script_admission.h`), which the site census records.
Its first line's `memory-rescue=yes` says the page's realm ran out of
memory, its server-rendered body was put back and the realm retired
(the census records it as `heavy.memory_rescue`).
`--heavy-policy run|ask|refuse` sets the heavy-page policy (run by default,
so a lab run never waits); with `ask`, `heavy-answer run` or
`heavy-answer stop` answers the scripts that wait, and `stop-scripts`
retires the page's realms as the PSP's Stop page scripts does.
`refresh-status` prints the live document's declarative refresh (`loop-refresh
state=none|pending|due|followed|cancelled|stopped|refused|offered`, delay,
elapsed page-clock time, whether it came from the `Refresh` header, whether
input has touched the page, target URL) and the session's loop-guard run and counters. The lab follows a due refresh inside the
next `tick`, like a page-script navigation; input commands (scrolling, focus,
`type`, `activate`, `go`, `back`, ...) restart the loop guard as a PSP button
press does, mark the page touched (a same-URL reload is then `offered`, not
followed), and `type`/`backspace` cancel a pending refresh.
`--module-bytecode-cache-kb N` overrides the in-memory module bytecode ceiling
(0 disables it) and `--classic-bytecode-cache-kb N` the classic-script one;
`--reload 1` loads the page twice through the engine, which is how a
revisit's restores are measured. Classic bytecode is stored by page-idle
work, which the `tick` command does not run: `idle N` runs up to N idle
turns (stopping at the first that leaves no work) without moving the clock,
as the PSP loop does while a page is quiet.
A page with a script runtime (any classic script, with or without
`--fetch-scripts`) does not load its parser `<img>` elements during the
commit. They wait in the post-commit image queue, which the PSP loop runs in
its page-idle and deferred-image slots and the lab runs in `pump` and
`drain`. `tick` does not run it. `idle N` runs it but does not wait for the
network, so its images may not have arrived when it returns. A `tick`-only
run of such a page therefore ends with
`images=0/0`. The `image-queue` report line gives the targets still queued,
the queue's batches so far, and whether the queue is still running
(`pending=no` with targets still queued means they are `loading=lazy` images
waiting for the viewport). Measure image loading with `pump` or `drain`.
`--script-cache-dir DIR` (or the older `--module-cache-dir`) adds the
persistent compiled-script tier, classic scripts and modules: a script
missing from the RAM tables is restored from the pack in `DIR` that its
response and top-level site name, read whole and verified at most once per
load and copied into RAM. `--script-cache-write` (or `--module-cache-write`)
lets idle work write packs there, 16 KiB per idle turn, so a writing run
needs idle turns (`idle N`) before it quits. The two runs of a cold/warm
comparison use the same `DIR`; `tools/site-census/census.py replay
--restart` does exactly that per page. The `javascript-script-cache-disk`
report line gives this load's disk hits and the synchronous read time
(`load-us`: index, pack reads, checksum and copies into RAM), packs and
bytes read, records promoted, lookups the index did not name, and for the
session's writes the packs and bytes written (index included), write time,
removals, evictions and the directory's files and bytes. A host lab refuses
a device path (`ms0:`, `host0:`).
`--no-progressive-first-paint` provides a same-build control for measuring the
provisional first-viewport tradeoff; final rendering and resource policy are
unchanged.

The host PSP frontend simulator installs the same asset, button-input, and
RGB565 presentation callbacks expected from a future PSP frontend. Its
accelerated session advances 30 minutes of logical browser time and enforces a
five-minute wall-clock ceiling:

```sh
./benchmarks/run-platform-session.sh build-preset-release /tmp/platform-session
```

This deliberately is not a CTest, development-test, or port-readiness member;
run it only when explicitly qualifying the simulator. The deterministic fault
recovery and selected neutral Web-platform checks are separate and fast:

```sh
./benchmarks/run-failure-recovery.sh build-preset-release /tmp/failure-recovery
./benchmarks/run-web-platform-correctness.sh build-preset-release /tmp/web-platform
```

The fault run proves timeout, TLS, truncated-response, cancellation, and
allocator recovery in one process against a loopback fixture. The selected
manifest is intentionally described as a small engine regression suite, not
as an upstream WPT conformance claim.

The upstream Web Platform Tests lanes, which run beside these local
regressions, are described in [WPT.md](../WPT.md).

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

Memory profiles, ownership and pressure behavior are in
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
it is diagnostic and is not used for the final truthful attempt. A passing
uninstrumented run loads the official API and frame, completes the normal
challenge, receives the documented dummy token, submits the form, and gets
`TURNSTILE PASS ERRORS none` from Siteverify using the documented test
secret.
