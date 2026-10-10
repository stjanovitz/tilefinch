# Running the desktop labs

This is the complete command-line reference for the two host frontends. Build
them first with [DEVELOPMENT.md](../DEVELOPMENT.md); the PSP EBOOT is covered
by [Device qualification](DEVICE_QUALIFICATION.md).

## Private local account input

Use a dedicated disposable test account, never your main account. Do not put
passwords, verification codes, session cookies, or authenticated URLs in chat,
command scripts, shell arguments, or captures. For direct host-browser testing:

```sh
python3 scripts/run-private-interactive-lab.py --url https://example.com/login
```

Run this yourself in a local interactive terminal after building the release
host lab. The launcher refuses piped input, discards the child's stdout and
stderr, removes inherited tracing/cache settings, disables core dumps, automatic
screenshots, HTTP captures, and persistent bytecode caching, and uses a private
temporary directory. Its separate native status pipe contains numeric
page/focus/task information and the parsed top-level origin, never DOM text,
URL paths, or query parameters. Check that origin before entering a value;
non-HTTPS credential entry is refused. Cookies and browser
session data remain in RAM for this process.

Use `view` to explicitly open a temporary 480×272 image, then `focus-id ID`,
`focus-next`, or `tap X Y` to focus the desired field. Use `secret` to replace
that field with locally masked input; it preserves spaces and Unicode. Use it
for account names and verification codes too. `activate` presses the focused
button; `pump 32 16` advances normal browser turns, including background
resources and cache maintenance. `tick` deliberately advances runtime/media
only and is reserved for isolated execution experiments; using it alone for
a live sign-in can strand optional-resource maintenance and constrain later
script staging. Ordinary `type`, script
evaluation, and diagnostic commands are unavailable. `quit` tears down the
browser and deletes the temporary files. An abrupt OS/process failure may
leave that directory behind; delete it manually in that case.

For the dedicated Google test account, use `signin` in this console, or add
`--signin` when launching with `--url https://accounts.google.com/ServiceLogin`.
On macOS it opens native, masked account/password dialogs. The local driver
finds unambiguous rendered username/password fields, checks the exact accounts
HTTPS origin before filling, submits each step once, and advances page work.
It pauses on a missing password step, changed origin, or refused action; it
does not solve verification prompts or claim authentication succeeded. The
requested sign-in opens a temporary PNG page view locally after completion or
pause; `view` refreshes that snapshot. Keep the process open to retain its
RAM-only browser session. No account values are passed as shell arguments or
saved as credential files.

### Keep credentials through browser restarts

For repeated host sign-in debugging, start the RAM-only keeper yourself:

```sh
python3 scripts/run-private-signin-session.py
```

Enter the dedicated account once in the local masked dialogs. The helper
starts the same private browser and attempts the sign-in flow. It never opens
a preview automatically; use `view` explicitly. Starting it authorizes separately
requested attempts until stopped, not an automatic retry loop. Optional
`--confirm-first-attempt` and `--confirm-each-attempt` enable local confirmation;
`--password-attempt-limit 0` removes the conservative three-attempt cap.
Its printed control socket is in an owner-only temporary
directory outside the repository. A test driver can use that socket without
receiving credentials or accessing page screenshots:

```sh
python3 scripts/run-private-signin-session.py \
  --control /private/tmp/tf-auth-EXAMPLE/control.sock --action status
```

Replace the example path with the one printed by the keeper. Only `status`,
`attempt`, `restart`, `advance`, `view`, `view-below`, `view-above`, `inspect`,
`youtube-home`, `youtube-subscriptions`, and `stop` are accepted. Status
contains a fixed phase name, numeric page/focus/task information, and the
origin, never form values, cookies, DOM text, URL paths, or error strings.
Actions run serially; status remains available while browser work is running.
The keeper's preparation, field waits and response waits use `pump`, matching
the browser's sliced resource/runtime schedule rather than runtime-only ticks.
An `aria-invalid` response on the entered account field stops the flow with
`username_rejected`, without sending the password. Its associated feedback is
scrolled into the local preview; message text is not returned over the socket.
`view` opens an image on the user's computer; it does not return its contents
over the control channel. No network listener is opened.

For a YouTube session, start with `--service youtube`. This follows YouTube's
ordinary sign-in entry into the accounts page; credentials are still filled
only on the exact accounts HTTPS origin. After sign-in, `youtube-home` and
`youtube-subscriptions` open the lightweight provider UI in that same browser
session: complete ordinary service-entry navigation once, then use bounded
bootstrap metadata and the browse API. Once navigation has reached YouTube,
feed checks do not repeat sign-in or reload its heavyweight page. The provider
builds bounded script-free cards and loads feeds cooperatively in idle turns.
Request signatures are derived natively from the same session's
policy-eligible cookies and are never included in diagnostics.
If memory pressure discarded the sign-in continuation's scripts while its
incumbent was resident, the first feed action can reload that static page
through at most two explicit normal reloads per browser process. Advance
normal browser turns, then request
the feed check again. This never submits a password or bypasses verification.
No cookies/tokens are exported, no arbitrary
URL or request headers are accepted through the socket, and no preview opens.
`inspect` includes numeric provider state and row counts. Raw personal titles,
payloads and screenshots stay local and must not be exported as diagnostics.
An authenticated feed is published only after the API confirms signed-in
status; an empty account can legitimately publish an empty feed. These host
checks do not qualify the PSP sign-in journey.
The targeted synthetic UI gate is
`build-preset-release/tilefinch-browser-engine-tests --youtube-home-only`;
it covers background loading, held publication, refresh, subscriptions and
transactional allocation refusal without an account or external requests.
The host-only private navigation command reuses normal authority/cookie policy
and does not add a history entry or persist the session. Changing a running
keeper's available actions requires restarting it; opaque credentials and
cookies must never be migrated to a replacement process.

Views now capture the rendered page vertically (up to 4,352 pixels), not just
the first handheld screen. The diagnostic visual copy removes text belonging
to input, textarea and editable controls before painting or writing files;
live field values and focus are unchanged. Matching retained field values in
other text commands are also removed. Raw DOM, attributes, scripts, cookies,
and response bodies are not exported. This is a diagnostic for the dedicated
test account, not a guarantee against an arbitrary page deliberately reflecting
credentials through images or fragmented text.

With the user's permission, `inspect` produces `redacted.png` and
`redacted.ppm.txt` beside the socket for a developer to inspect. Only those
redacted artifacts may be shared; do not inspect an older `view.png` or a raw
browser dump. Wait for `busy=false` and `inspection_ready=true` before reading
them. Files remain owner-only, outside the repository, and are removed when
the keeper exits. The text report states the captured and complete page
heights; oversized pages are explicitly bounded rather than called complete.
The text report also includes fixed runtime counters and exception categories
(such as `type` or `none`), never exception messages or stacks. This distinguishes
script admission failures and pending requests without exposing author data.
An opt-in promise-outcome histogram keeps only fixed exception categories,
including rejections later handled by the page. It records no reason text or
stack and does not change normal rejection handling.
Response byte/text lengths, timer and pending-work counts, and allocator
refusals help distinguish an incomplete delivery from a stalled continuation;
response contents are never included.
Dynamic-script fetch failures are partitioned into response-size, transport
(including HTTP errors), shared-memory, authority/MIME, and source-admission
counts. These describe receiving a script, not later author evaluation errors;
they expose no request or response text.
The private console also opts into a 32-entry RAM-only XHR history: numeric
send sequence, status, byte/text lengths, response phase, getter-read counts,
and event counts. Overflow is reported explicitly. Phases are 1 (sent),
2 (headers), 3 (loading), 4 (done), 5 (loadend complete), 6 (failure), and
7 (abort). No URL, body, headers, handler identity or field value is retained
by this history. Ordinary browsing does not allocate it. Scheduler flags
separate future timers from runnable promise jobs/checkpoint continuations
and a pending navigation; a successful HTTP status alone is not sign-in success.
Navigation boundary counters distinguish accepted, refused and consumed host
requests without recording their targets or metadata. They share the private
observer's opt-in lifetime and do not allocate in ordinary browsing.
The redacted snapshot also reports numeric navigation HTTP/transport status,
TLS/timeout/error-present flags and commit count. Browser-owned failure prefixes
map to fixed construction-phase categories; unknown errors map to `other`.
Neither error text nor author exceptions are exported.
The private test also counts calls and failures through an opt-in `JSON.parse`
observer, including caught failures. It retains no parser input, result or
exception text. This observer preserves arguments, receivers, revivers, values
and exceptions, but changes function identity; aliases cached by the page before
enablement are not covered. Treat its counts as diagnostic evidence, not a
complete parser census or a claim that a particular response was accepted.
With separate user approval, a maintainer may supply an owner-only (0600),
non-symlink `outcome-classifier.js` in the helper's disposable working directory
before restarting the browser. It is a trusted local function expression, at
most 16 KiB, receiving `(value, originalParse, event)`: event 0 observes a parse,
event 1 observes a promise rejection (with no original parser). For event 1,
only a bounded numeric stage ID 0–12 is reported, never exception contents.
Snapshot sampling uses events 2 and 3 for entered/failed stage masks; only
13-bit integer masks are accepted. Keep these diagnostic counters independent
of live inputs and clear them at the specific public-code boundary being probed.
Keep account-specific
filters in private investigation storage. Only numeric categories 0–12 are
reported: unrecognized, rejected, verification required, redirect, credential
transition, protocol error, challenge update, then credential-handoff
navigation, form POST, native account, close, prerequisite, and other.
These classify the declared next action, not its completion. Invalid returns and filter
exceptions become unrecognized; filter exceptions never replace the page's
parse result or escape into diagnostics. Values and response contents are not
retained by the native recorder. This observes only parses through the wrapper,
and a credential transition is not proof that authentication completed.
The keeper's `--outcome-classifier /private/path/filter.js` option safely copies
that explicitly requested owner-only filter into a new session before starting
the browser, avoiding a separate installation/restart. No filter is selected
by default, and the file must contain diagnostic code, never account inputs.
Button submission uses normal controller focus and activation, including blur
notifications, not a selector-only click. Invalid password feedback is revealed
locally without another submission.

`restart` discards browser cookies and images, not retained account input.
Use `attempt` afterward. The keeper conservatively counts a possible password
submission before filling the password field, including failed/uncertain
delivery. Starting the keeper locally authorizes requested attempts until it
stops; no additional approval dialogs or image previews appear by default.
Use `view` to open a preview, or `--confirm-each-attempt` to opt into a local
confirmation dialog (Cancel by default) for every attempt. At most three
possible password submissions are allowed per keeper session by default.
For explicitly authorized extended debugging, `--password-attempt-limit 0`
removes that lifetime cap. It does not retry automatically: after the initial
startup attempt, each attempt remains a separate control action. Credentials
stay in RAM until Ctrl-C or `stop`; uncapped mode does not export or migrate
them. Repeated failures can trigger account lockouts, so inspect the result
and change the tested implementation before retrying an unchanged failure.
A browser failure does not request the credentials again.
For a single explicitly approved diagnostic attempt, start a fresh keeper with
`--password-attempt-limit 1 --confirm-first-attempt`. The first attempt then
requires the same local confirmation, and restarting the browser cannot permit
a second password submission. These startup options cannot change a running
keeper's allowance or transfer its retained credentials.
Verification and unexpected pages remain under user control. Ctrl-C or `stop`
terminates the browser, clears the retained bytearrays, and deletes the local
temporary directory. The keeper itself must stay running to avoid re-entry.

The owner-only socket is not a security boundary against other processes
running as the same OS user. Do not share it publicly. This helper never
transfers cookies from another browser, and browser restarts do not retain an
authenticated session. Credentials remain available only to the local helper
and the intended website during its ordinary sign-in flow.

This is not a hardened credential vault: the experimental browser and the
intended website necessarily hold submitted values in memory. Python does not
guarantee erasure of string copies; OS swap, privileged inspection, and image
viewer caches are outside the launcher's protection. Explicit views can contain
account details. After testing, sign out or revoke the test session from the
account's security settings. This path does not transfer another browser's
session or bypass an identity provider's checks.

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

The interactive lab's closing summary reports `bot-wall detected=yes site=... vendor=... status=...` when the committed top-level document is a bot-protection wall: a refusal status (403, 405, 429 or 503, or AWS WAF's 202) with a vendor signature in its headers or title (DataDome, Cloudflare challenge, Imperva, AWS WAF, Kasada, HUMAN, Akamai's block page), or a refusal-status page that paints nothing after its script was refused. The PSP chrome raises the same two-line notice ("<site> blocked Tilefinch / Bot protection: this site may not work") instead of leaving a silent blank page. Detection is generic, reads only the response and the committed page, and never attempts the challenge.

The interactive lab can make a legitimate managed-challenge attempt with `--url ... --fetch-scripts`. It retains secure/HttpOnly response cookies, retries a safe GET once when `Critical-CH` requests truthful PSP client hints, and sends only the named high-entropy hints to the origin that requested them. A cross-origin retry redirect suppresses those hints for the rest of that redirect chain while truthful low-entropy hints continue normally. The lab loads scripts inserted by the bootstrap under normal quotas and reports challenge/network/clearance state without cookie values. It never copies clearance from another browser. The ordinary compatibility User-Agent carries iPhone/WebKit/Safari routing tokens so large sites choose their bounded mobile document, while also naming `PlayStation Portable` and `Tilefinch`; `navigator.platform` and low-entropy Client Hints remain explicitly PSP/Tilefinch rather than claiming the capabilities of Safari or Chrome. Google is the one per-site exception (`src/site_identity.c`): with the compatibility User-Agent it serves an XHTML-MP "Update your browser" page, so Google searches open a local compatibility page. Its "Try Google anyway" button opts the session in until the browser restarts; Google top-level documents then use the honest `Mozilla/5.0 (PlayStation Portable) Tilefinch/0.1` for their navigation, every request they make, and `navigator.userAgent`, and a 429, `/sorry/`, XHTML-MP, or failed search clears the opt-in and returns to the compatibility page. The opt-in marker on that button's URL is stripped before any request is built.

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

`--psp-profile strict` selects a 16 MiB content ceiling, 4 MiB JavaScript ceiling, 1 MiB cumulative script-source allowance, and eight tiles. `--psp-profile realistic` is the PSP application's own configuration, taken from the `BROWSER_PSP_APP_*` constants that `src/psp_boot_config.c` and the PSP app use (`browser_config_apply_psp_app_defaults()`): a 32 MiB page envelope, a 5 MiB JavaScript heap with the app's 4 MiB boot window, GC growth and array-cap engine knobs (an explicit environment value wins), 256 scripts, memory-based script admission with a 16 MiB source ceiling and 4 MiB per file, a 60-second script timeout, an 8 MiB document cap, four history entries, a 640 KiB session cache, 24 tiles, and the app's stylesheet and image limits. A lab replay therefore admits what the device admits without explicit limit flags. The VM value is a ceiling rather than an up-front reservation. The public `BrowserEngine` profile additionally records an 8 MiB minimum non-page reserve for UI, stacks, TLS/backend state, sockets, libc metadata, and fragmentation; that reserve is deliberately unavailable to page content. `browser_config_apply_psp_memory_profile()` applies the same strict or realistic policy to embedders (realistic there is `browser_config_apply_psp_app_defaults()` too). Both profiles enable generic allocation-pressure adaptation: as owned memory approaches guarded stage reserves, the loader may skip JavaScript, reduce stylesheet and image quotas, or retain four rather than eight tiles. Each decision is logged, depends only on remaining budget, and can be selected or disabled explicitly with `--adaptive-resources` or `--no-adaptive-resources`. Explicit `--limit-mb`, `--js-limit-mb`, or `--tile-count` values override the selected profile. `--navigation-stress N` repeatedly replaces a page through the bounded navigation owner and fails if teardown does not return to the pre-stress allocation level. `--resource-stage-ms` bounds each complete external CSS or image phase (100–60000 ms); requests inside the phase remain four-way concurrent and preserve document order when applied. CSS telemetry reports batches, the usable first batch, deadline cancellation, and total elapsed time.

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
  An extra miss can be one more full resolution, so `style.rule_queries`
  moves in the same direction by at most the miss difference, and
  `style.rule_candidates`/`style.selector_*` move with it (about one run in
  300 when each run's fixture sits in a fresh temporary directory). The
  determinism test checks those against the miss difference.
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
and frame rendering. `reader-on`/`reader-off` and `basic-on` switch to the
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
