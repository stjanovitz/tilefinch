# Running the desktop labs

This is the complete command-line reference for the two host frontends. Build
them first with [DEVELOPMENT.md](../DEVELOPMENT.md); the PSP EBOOT is covered
by [Device qualification](DEVICE_QUALIFICATION.md).

## Static renderer (`psp-browser-lab`)

```sh
./build/psp-browser-lab \
  --fixture fixtures/demo.html \
  --output-dir frames \
  --limit-mb 48 \
  --js-limit-mb 8
```

The program prints phase-by-phase memory telemetry and tile-cache statistics. It writes PPM frames because that format has no runtime dependency. Most image tools can convert them to PNG.

Use `--skip-js` to measure HTML/CSS/layout/rendering independently when a real page's scripts require browser APIs that Tilefinch does not implement. `--dump-links links.tsv` exports word-level hyperlink hit regions, while `--dump-layout layout.tsv` exports the retained paint list. On a live load, relative and protocol-relative links are made absolute. The final telemetry lines are intended for machine-readable comparisons.

`--challenge-diagnostic` is an explicitly diagnostic navigation policy. It retains an HTTP error page, records `cf-mitigated`, `Accept-CH`, `Critical-CH`, and server headers, runs only the page's initially present inline scripts, and reports external script URLs inserted into the DOM. It does not claim the error page is the requested site and does not fabricate or submit a challenge result.

The interactive lab can make a legitimate managed-challenge attempt with `--url ... --fetch-scripts`. It retains secure/HttpOnly response cookies, retries a safe GET once when `Critical-CH` requests truthful PSP client hints, and sends only the named high-entropy hints to the origin that requested them. A cross-origin retry redirect suppresses those hints for the rest of that redirect chain while truthful low-entropy hints continue normally. The lab loads scripts inserted by the bootstrap under normal quotas and reports challenge/network/clearance state without cookie values. It never copies clearance from another browser. The ordinary compatibility User-Agent carries iPhone/WebKit/Safari routing tokens so large sites choose their bounded mobile document, while also naming `PlayStation Portable` and `Tilefinch`; `navigator.platform` and low-entropy Client Hints remain explicitly PSP/Tilefinch rather than claiming the capabilities of Safari or Chrome.

Live document loads use the same hard allocation budget as parsing and rendering:

```sh
./build/psp-browser-lab \
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

`--fetch-images` separately enables bounded raster/SVG, CSS-background, and CSS-mask loading. Defaults bound the pass to 64 unique attempts, 512 KiB aggregate encoded input, 256 KiB per response, and 512 KiB retained decoded SVG/mask data. Raster files remain compressed and are decoded one at a time through the tile cache; a source decode may use at most four times the output quota, capped at 8 MiB for sub-8-MiB profiles, and is repeatedly halved before painting when its RGBA target exceeds the quota. Larger decompression candidates are skipped before allocation. Responsive `<picture>` and width-descriptor `srcset` select a viewport-sized supported source. Change the limits with `--max-images`, `--max-image-kb`, `--max-image-file-kb`, and `--max-decoded-image-kb`. Duplicate URLs share resources, element and pseudo-element masks and CSS background images use the same quotas, unsupported or failed optional resources do not abort the page, and every retained buffer uses the shared page budget. Background `cover` and `contain` use centered bounded sampling; repetition is supported, and the common two-gradient layered-background form paints in source order. Arbitrary layer counts and independent per-layer URL positioning remain unsupported. External stylesheets may recursively import up to four levels while sharing the same URL-count, byte, timeout, cache, and page-memory limits; conditional imports use the bounded media/supports evaluators. Quoted and single-`attr(name)` pseudo-element content is decoded locally under a 64-string/63-byte-per-string cap; live attribute values reuse bounded DOM bytes. When external resources are enabled and FreeType 2.14.3 or newer is available, page fonts are limited to eight attempts, 256 KiB aggregate encoded input, 96 KiB per response, two regular/bold families, two ordered sources per face, and 256 KiB of backend allocation per face. Unsupported WOFF2, collections, CFF outlines, inline data URLs, failed CORS requests, and quota misses fall back without aborting the page. The interactive lab has a separate quota-controlled external-script pipeline.

A bounded mobile-CSS run is:

```sh
./build/psp-browser-lab \
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
./build/psp-browser-lab \
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

`psp-browser-interactive-lab` exercises the persistent layers together. It retains JavaScript and session state, can advance the bounded timer clock, load quota-controlled same-origin scripts, repeat navigation to exercise HTTP validators and the script cache, drive controller focus/edit/activation, follow GET/POST form actions, and render the resulting page:

`--forced-dark` enables the same role-aware page-color mapping used by the PSP
night mode. It is intended for deterministic visual captures: page surfaces
and text are remapped while image pixels remain untouched.

For URL navigation, the persistent lab owns the same bounded external
stylesheet and image pipeline as the static renderer: at most 6 stylesheets
(768 KiB total, 256 KiB each) and 12 images (1.5 MiB encoded total, 384 KiB
each, 3 MiB decoded) with a 15-second per-resource timeout. Resource counts and
bytes are printed in the final status. Page-owned assets are destroyed on
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
./build/psp-browser-interactive-lab \
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
./build/psp-browser-interactive-lab \
  --fixture fixtures/interactive.html \
  --commands fixtures/lab-loop.commands \
  --loop-output-dir interactive-frames \
  --output interactive-final.ppm

./build/psp-browser-interactive-lab \
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
  build-bellard-clean-current \
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
./build/psp-browser-interactive-lab \
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
