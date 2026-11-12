# Bounded streaming navigation

Top-level navigation consumes a transport-independent response stream. It
feeds Lexbor incrementally, pauses for parser-blocking scripts, discovers
resources, and may publish a provisional viewport before EOF. Buffered entry
points are compatibility wrappers around the same candidate-load machinery.

## Public lifecycle

`NavigationLoad` is the cooperative boundary:

- `navigation_load_begin_url()` and `navigation_load_begin_request()` create
  an opaque candidate;
- `navigation_load_pump()` advances bounded transport, parser, resource,
  script, style, and layout work;
- `navigation_load_finish()` adopts a completely prepared candidate;
- cancel and destroy retire every unfinished continuation.

Blocking convenience functions pump this same object to a terminal result;
they do not implement a second navigation path.

## Streaming bounds

- The decoded inbound parser buffer is capped at 64 KiB; ordinary delivery
  uses smaller chunks.
- Transactional navigation does not retain a second complete top-level HTML
  body. Capture mode tees chunks directly to the private trace writer.
- Pump quotas bound callbacks, bytes, and elapsed time delivered to the
  browser thread.
- Replay can split identical bytes at arbitrary UTF-8, token, raw-text, table,
  and script boundaries and inject stalls, cancellation, or truncation.
- Progressive checkpoints are byte/time coalesced; the parser does not trigger
  layout per token.
- `DOMContentLoaded`, `load`, and final history adoption occur only after EOF
  and successful finalization.

## Transactional replacement

The default mode keeps the incumbent DOM, runtime, controller, layout, tiles,
URL, and history active while the candidate is fallible. Candidate resource,
script, stylesheet, layout, and frame state is separately owned. Final commit
only moves already-prepared state and cannot allocate.

Cancellation, transport failure, parser rejection, script failure, resource
failure, or allocation refusal therefore leaves the incumbent page usable and
returns candidate ownership to the budget.

## Explicit low-memory replacement

`NAVIGATION_REPLACEMENT_LOW_MEMORY` is an opt-in peak-memory tradeoff, never a
profile default. It applies only to a streamed URL replacing a live page.

A bounded incumbent with a small author-script state may complete transport
first, freeze an already-provisioned RGB565 frame, retire its DOM/runtime, and
then build the candidate. Larger or more complex incumbents fall back to the
transactional path. The candidate body remains subject to the ordinary
document byte ceiling.

Before retirement the normal rollback guarantee applies. After retirement,
the old DOM cannot be reconstructed: URL, title, scroll, history, presentation
metadata, and the frozen frame remain, and a preallocated failure/retry state
is shown. Retry starts a fresh load from that retained navigation record.

Telemetry distinguishes this deliberate `rollback=unavailable` condition and
records bytes released by incumbent retirement.

## Parser, scripts, and resources

Inline and same-origin classic parser-blocking scripts execute when their
closing element is parsed, after blocking stylesheets available at that point.
They see the prefix DOM but not later elements. Deferred, asynchronous, and
module work enters the bounded final lifecycle.

`document.write()` from a script the parser is running (including that
script's microtasks and the handlers it dispatches) feeds the parser at the
insertion point, as HTML specifies and Chromium does. The parser stops right
after each `</script>` and runs the script between Lexbor calls, so the
written markup continues the document's input just after that end tag; a
written inline script runs inside the `write()` call; a written
parser-blocking external script runs after the writing script returns and
before any markup written after it, which waits (at most 256 KiB of it;
beyond that `write()` throws `RangeError`). A `write()` nested more than 21
deep is ignored, Chromium's limit, and a written script shares its writer's
watchdog deadline. Written scripts pass the same CSP, content-blocker,
admission and circuit-breaker checks as the response's own. When one of
them trips the breaker or a parser refusal, the writing script finishes
first (later written scripts in it are skipped) and the page's JavaScript
stops once it returns.

Outside such a script (timers, events, asynchronous, deferred and module
scripts, after load) `write()` has no insertion point: it appends the markup
to the end of the body in a microtask, and a script in it does not run.
Chromium instead ignores writes from asynchronous scripts and otherwise
replaces the document.

The parser progressively resolves:

- the first valid base URL and viewport declaration;
- response and document referrer policy;
- linked stylesheets and scripts;
- responsive image sources and immediately visible images;
- preload candidates which fit the bounded scheduler policy.

Stylesheets and eligible scripts are fetched as soon as their URLs become
known. Immediate CSS, parser-blocking scripts, and visible images outrank
speculation. Unsupported or lower-value preloads remain ordinary later
resource work.

The parser does not do that stylesheet work inside the pump that feeds a
script's end tag. It stops feeding just before the end tag of an open
parser-blocking script whose realm or sheet is not ready, then prepares both
over the following pumps, each bounded by the parser's time quota
(`src/navigation/style_prepare.inc`):

1. It creates the script realm.
2. It polls the render-blocking style preloads; it never waits on them.
3. It builds the sheet. An external sheet of 16 KB or more is parsed in
   slices that stop at rule boundaries (`StyleCssSlicedParse`,
   `StylesheetParseDeferral`).

The end tag is then fed, and the checkpoint reuses the sheet by fingerprint.
A streaming preview with a stale sheet starts the same preparation. Anything
unexpected abandons the preparation and the checkpoint builds as before.
`blocking-stylesheets ... prepared=` (lab) and `style-prepare=` (PSP
validation log) count preparations, abandoned ones, pumps, and the longest
step.

Stylesheet suffixes preserve compiler/index state when the input order remains
valid. `@font-face` discovery occurs during the CSS parse. Preview layout is
skipped when the parsed prefix cannot affect visible pixels, and otherwise
limits work to the viewport-relevant region before authoritative layout. A
laid-out preview paints when its first screen shows text or a decoded image,
or an unfilled image slot of at least 16 CSS px a side together with other
visible paint that is not the page background and visible text laid out
below the screen (a masthead of inline-SVG logos above the first headline).
A preview that would be blank until its images arrive is dropped, and the
next attempt waits for more of the document. The PSP compositor declines a
first preview that rasters to one colour, as such a masthead can in night
mode; a declined slots-only preview backs off the same way.

A painted preview whose current screen shows no text or decoded image while
the preview holds some further down sets the provisional viewport's
`reading_hint`. The PSP app then shows **Preview ready  R to read** in the
loading toast. The browser thread sets that toast in its own UI and, under
the presentation fence, in the supervisor's copy; the supervisor never
reads the engine. The hint ends for that load at the first screen that
shows content (a rasterized logo included), at the first page press, at
cancellation and when the load ends. The job records when it showed and
ended (`reading_hint_shown_us`, `reading_hint_cleared_us`; validation
`reading-hint=`).

## Resumable finalization

Final parse/style/layout remains transactional but is not one unbroken browser
thread call. `LayoutBuildJob` owns all partial output and advances explicit
phases under a time/unit quota. Its inputs are immutable for the job lifetime,
and its completed display list is byte-equivalent to the synchronous layout
entry point.

Quota overruns are diagnostics; they never authorize publication of a partial
page. Failure or cancellation destroys the job and keeps the incumbent.

## Telemetry

Navigation metrics include:

- received bytes/chunks and peak inbound buffering;
- transport, parser, stylesheet, script, resource, and layout stages;
- parser pauses and preload activity;
- first DOM, first eligible preview, first paint, and completion time;
- first content and first text: when a presented, fully rasterized screen at
  the reader's position first holds non-space text or a decoded image, and
  text (`first_content_us`, `first_text_us`; first paint counts a blank
  masthead frame too);
- partial layout/paint counts and skipped no-pixel previews;
- pump/yield counts, work units, maximum slice, and finalization time;
- cancellation, timeout, truncation, replacement mode, and rollback outcome.

Validation uses aggregate records. The navigation hot path does not perform
per-chunk Memory Stick logging.

## Correctness gates

The focused suite covers:

- buffered/streamed equivalence across token and UTF-8 splits;
- parser-blocking script visibility and stylesheet ordering;
- defer/module/async completion ordering;
- timeout, TLS, redirect, truncation, and cancellation followed by same-process
  recovery;
- cancellation before and after useful provisional DOM;
- allocation failure across parser callbacks, scripts, styles, resources,
  layout, paint, finalization, and commit;
- candidate teardown with zero owned bytes;
- low-memory replacement before and after incumbent retirement;
- response-keyed and strict replay under varied chunking and retained delays;
- full-scroll nonblank output and exact top-frame reconstruction.

The pinned Lexbor build includes a guarded partial-document destructor patch
because parser rollback must be correct even when the dependency has only
constructed part of its document.

## Deliberate limits

- Enormous documents whose final DOM/layout exceed the content budget are
  rejected even though their response body can be streamed.
- `document.write()` without an insertion point appends to the body
  instead of opening a new document.
- Live-site challenge clearance is not part of navigation.
- Desktop replay timing is not PSP network or CPU timing.

Run the retained private streaming corpus with:

```sh
./benchmarks/run-streaming-corpus.sh \
  build-preset-release \
  /path/to/private/streaming-corpus \
  /tmp/tilefinch-streaming-corpus
```
