# Bounded streaming navigation

Top-level navigation consumes a transport-independent response stream. It
feeds Lexbor incrementally, pauses for parser-blocking scripts, discovers
resources, and may publish a provisional viewport before EOF. Buffered entry
points are compatibility wrappers around the same candidate-load machinery.

## Public lifecycle

`NavigationLoad` is the cooperative boundary:

- `navigation_load_begin_url()`, `navigation_load_begin_request()`, and
  `navigation_load_begin_sectioned()` create an opaque candidate;
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

Remote compressed-section callbacks are bound to the incumbent until the
candidate commits. A candidate never inherits callbacks into the previous
document backing.

## Explicit low-memory replacement

`NAVIGATION_REPLACEMENT_LOW_MEMORY` is an opt-in peak-memory tradeoff, never a
profile default. It applies only to a streamed, non-sectioned URL replacing a
live page.

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

`document.write()` is intentionally unsupported. It is absent from the DOM
bridge, so author use gets an isolated JavaScript error rather than parser
re-entry or document corruption.

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

Stylesheet suffixes preserve compiler/index state when the input order remains
valid. `@font-face` discovery occurs during the CSS parse. Preview layout is
skipped when the parsed prefix cannot affect visible pixels, and otherwise
limits work to the viewport-relevant region before authoritative layout.

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
- `document.write()` is unsupported.
- The compressed-section representation is opt-in and has its narrower
  replacement guarantee.
- Live-site challenge clearance is not part of navigation.
- Desktop replay timing is not PSP network or CPU timing.

Run the retained private streaming corpus with:

```sh
./benchmarks/run-streaming-corpus.sh \
  build-preset-release \
  /path/to/private/streaming-corpus \
  /tmp/tilefinch-streaming-corpus
```
