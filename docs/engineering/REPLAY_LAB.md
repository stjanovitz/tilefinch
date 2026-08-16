# Replay and reference lab

Tilefinch uses retained HTTP traces to make network-dependent behavior
reproducible without turning tests into live-site probes. The same trace can
drive the native engine and an offline Chromium reference, allowing request
closure, script behavior, layout, and pixels to be compared against identical
response bytes.

Trace content is private test material and is not committed. The repository
contains the formats, selectors, runners, and digest checks required to inspect
and reproduce the workflow.

## Two replay modes

### Strict replay

Strict replay consumes complete captured request identities in record order.
It is the right mode for transport, cookies, redirects, CORS, CSP, timing,
cancellation, and security tests because a changed request shape fails rather
than receiving a nearby response.

Use the lab's `--replay-http TRACE` option or the focused replay scripts under
`benchmarks/`.

### Response-keyed visual replay

Visual comparison needs both engines to receive the same content even when
their incidental client-hint or header shapes differ. The opt-in
`--replay-http-response-keyed TRACE` mode selects by method and normalized,
fragmentless URL using the versioned algorithm named by
`FETCH_TRACE_RESPONSE_KEY_ROUTE_SELECTION_VERSION`.

For each method/URL key:

1. a successful HTTP record outranks a captured HTTP failure;
2. a captured HTTP failure outranks a status-zero transport failure;
3. one highest-rank record is reusable;
4. multiple highest-rank records form an ordered occurrence sequence;
5. exhaustion, ambiguity, malformed metadata, or integrity failure fails
   closed;
6. a lower-rank response is never used after the selected rank is exhausted.

Selected records are still parsed by the authoritative trace parser. The
selector does not bypass response hashes, quotas, retained delays, CORS,
cookies, or cancellation semantics.

## Closure ledger

The native and Chromium runners emit a bounded ledger with:

- total requests;
- matched, served, and policy-rejected requests;
- unmatched, conflicting, and invalid requests;
- reusable and occurrence claims;
- occurrence exhaustion;
- a compact range of claimed record IDs.

A native response-keyed run closes only when:

```text
matched == served + rejected
requests == matched + unmatched + conflicts + invalid
```

and unmatched, conflicting, invalid, and occurrence-exhausted counts are zero.
At least one response must be served. A Chromium fidelity reference is
stricter: every request must match and be fulfilled, so policy rejection is
also zero.

The ledger is part of visual evidence. A plausible screenshot from an
unclosed replay is not a valid reference.

## Loopback reference server

`benchmarks/trace_replay_server.py` exposes retained decoded responses to a
desktop browser over IPv4 loopback only:

```sh
python3 benchmarks/trace_replay_server.py \
  /path/to/private/trace --port 0
```

The first output line gives the selected local URL. The server:

- matches the exact raw path, query, and method;
- writes retained decoded body bytes without rewriting content;
- strips hop-by-hop fields and stale Content-Encoding/Content-Length values;
- rejects missing or length-mismatched bodies before listening;
- reports missing routes, method mismatches, retained transport failures, and
  conflicting responses explicitly;
- cannot bind to a non-loopback interface.

It is not an HTTPS interception proxy. Absolute subresources still point at
their original origins unless the reference-capture layer fulfills them from
the trace. Reference qualification therefore depends on the full closure
ledger, not on the loopback page alone.

## Cookies and replay time

Response cookies are stored as separate numbered fields, not rejoined from a
generic header snapshot. Reference replay validates syntax, origin/domain/path
scope, supported attributes, prefixes, and redaction before Chromium starts.
Persistent lifetime is reconstructed against the trace's fixed origin time:
`Max-Age` remains relative, and `Expires` retains its captured TTL.

Request credentials and live session secrets must not enter a distributable
trace. Private corpora remain access-controlled even when cookie fields are
redacted because page bodies and URLs may still contain copyrighted or
sensitive material.

## Acquiring missing responses

Live acquisition is a separate maintenance operation and requires explicit
authorization. It never runs as part of build, test, replay, or scoring.

`benchmarks/acquire-trace-plan.py` accepts only a digest-bound plan projected
from a qualified reference diagnostic. The plan fixes:

- source trace digest and record count;
- route-selection version and replay origin;
- exact GET/HEAD method and URL occurrences;
- allowed origins and resource types;
- response, request-time, and final-trace bounds.

The recorder performs one canonical request per authorized key. It does not
follow redirects or send a body, cookies, Authorization, client certificates,
custom headers, referrer, Origin, client hints, proxy credentials, or netrc
state. A redirect is a terminal captured response.

Each response first lands in an isolated one-record trace. The orchestrator
checks metadata/body pairing, lengths, hashes, capture completion, method,
URL, and policy fields before adding it to a private staging copy. It then
re-inspects the complete trace, verifies old routes are unchanged, flushes the
staging tree, and publishes with a no-replace rename. Failure removes only
tool-owned staging paths.

No stale or hand-authored plan is live authority. A new acquisition requires a
fresh eligible diagnostic and explicit approval of the exact origin and route
set.

## Visual comparison

The fidelity scoreboard is described in [Visual fidelity](../FIDELITY.md). For
one pair of already-qualified frames, use:

```sh
python3 benchmarks/compare-reference-frame.py \
  candidate.ppm reference.png
```

The comparator operates on canonical PNG references and binary P6 PPM
candidates. Its JSON output includes RGB565 error, SSIM, multi-scale SSIM,
edge metrics, foreground coverage, optional geometry anchors, settings, and
qualification failures. File contents must match their extensions;
`benchmarks/normalize-reference-frame.py` canonicalizes references.

For text-specific investigation:

```sh
psp-browser-interactive-lab ... \
  --dump-text-metrics /tmp/candidate-text.json

node benchmarks/capture-reference.js ... \
  --text-metrics-output /tmp/reference-text.json

python3 benchmarks/compare-text-metrics.py \
  /tmp/candidate-text.json /tmp/reference-text.json
```

Both exporters are bounded. The comparison reports retained 26.6 advances,
positions, line boxes, font sizes, and line-break agreement, which helps
separate font metrics from broader layout shifts.

## Trust boundary

- Replay is never a live-network fallback for the PSP.
- Response-keyed selection is used only for visual/reference work.
- Strict replay remains authoritative for request-security behavior.
- Traces and generated reference images stay outside public Git history.
- A trace digest proves bytes, not permission to redistribute those bytes.
- A host reference proves browser behavior under retained inputs, not live-site
  availability or device timing.
