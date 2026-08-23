# PSP transport contract

Tilefinch owns the complete HTTPS chain used by the PSP release build:

- curl 8.21.0;
- Mbed TLS 3.6.7 LTS;
- nghttp2 1.69.0 when HTTP/2 is enabled;
- PSPDEV zlib 1.3.1.

`third_party/psp_transport/dependencies.lock` records official archive URLs and
SHA-256 digests. `scripts/fetch-psp-transport-deps.sh` populates the ignored
local cache; later cross-builds are offline and verify every archive before
extraction. The owned libraries include only narrow PSP portability changes.
Runtime initialization also checks the linked curl, TLS, and nghttp2
provenance against the versions compiled into Tilefinch.

## Build modes

The shipping `psp` preset selects the owned stack with HTTP/2 enabled:

```text
TILEFINCH_PSP_TRANSPORT_MODE=OWNED
TILEFINCH_PSP_HTTP2=ON
```

HTTP/2 negotiates through ALPN and falls back to HTTP/1.1. Two diagnostic
presets isolate the protocol or dependency choice:

```sh
cmake --preset psp-http1
cmake --build --preset psp-http1

cmake --preset psp-legacy-transport
cmake --build --preset psp-legacy-transport
```

The legacy SDK stack is an escape build, not a release-qualified transport.
HTTP/3 is out of scope: it would require a QUIC implementation, a materially
larger memory and code budget, and a separate device validation program.

## Execution boundary

One process transport worker owns curl handles and the platform calls which
may block in DNS, TCP, or TLS. It exposes bounded generation-bearing request
slots:

- ordinary page and resource lanes allocate 64 KiB response buffers lazily;
- media range requests use two 256 KiB windows;
- HOME preconnect owns a bodyless descriptor;
- update traffic uses the same worker but retains updater-owned verification
  and installation state.

The worker is a byte mover, not a browser-policy authority. It never touches
the DOM, JavaScript runtime, page allocator, cookie jar, UI, profile, logger,
or filesystem. The browser thread prepares and authorizes each request hop,
then consumes immutable chunks through the ordinary fetch scheduler.

Redirect chains are intentionally sequential: only one already-authorized hop
from a request is executing at a time. Independent requests still progress
concurrently, and same-origin requests may multiplex over HTTP/2. This keeps
redirect, cookie, CORS, CSP, mixed-content, Private Network Access, and content
blocking decisions on the browser thread without serializing the whole page.

Starting a replacement navigation cancels unfinished resource and script
requests owned by the incumbent page before the candidate document is queued.
Only optional network progress is retired: the incumbent document and scanout
remain the rollback surface until the candidate commits. This prevents a page
of thumbnails or fonts from consuming every response descriptor ahead of the
user's next navigation. A failed or cancelled candidate does not reissue those
retired requests; the intact page remains interactive, but optional images or
author fetches which had not completed stay failed. A future priority lane can
preempt this work without cancelling it.

Cancellation closes admission and marks slots atomically. Network teardown
waits for every lease to retire before unloading PSP networking. A timeout
retains the stack rather than calling `sceNetInetTerm` beneath a live curl
operation; the [network supervisor](PSP_NETWORK_SUPERVISOR.md) owns that
ordering.

## TLS policy and performance

Handshake acceleration does not weaken certificate, hostname, protocol, or
cipher validation.

### Key exchange

Mbed TLS advertises x25519 before P-256. The PSP build enables Project
Everest's verified Curve25519 implementation through
`MBEDTLS_ECDH_VARIANT_EVEREST_ENABLED`. The standalone crypto self-test checks
generic and Everest results against RFC 7748 vectors and requires both paths
to agree.

### Connection and session reuse

curl's shared connection and process-local TLS session caches are enabled for
ordinary requests and media ranges. By default, a bounded cross-boot store
extends resumption across process launches:

- at most 16 hosts and two sessions per host;
- at most 4 KiB per session and 64 KiB for the complete file;
- versioned, checksummed records;
- exact same-site keys and bounded future validity;
- cleared together with HTTP cache data.

The global **TLS ticket saving** preference disables cross-boot import/export
and removes the durable generation without disabling live connection reuse.

The store contains bearer material. It remains on the Memory Stick, is never
exposed to pages, and is offered only to its recorded site.

### HOME preconnect

Holding focus on the built-in YouTube HOME destination for 300 ms may queue one
bodyless `HEAD` request with the same pinned mobile User-Agent as an ordinary
page request. It resolves and establishes transport/TLS but sends no cookies,
credentials, referrer, or response body. Moving focus, navigating
elsewhere, or suspending cancels it. User bookmarks, page-provided links, and
arbitrary typed destinations do not receive speculative preconnect authority.

Completing an ordinary HTTP request returns the socket to curl's shared live
connection cache. The following provider request can therefore reuse TCP and
TLS rather than merely resuming a TLS session. Media bytes served by a
different CDN origin necessarily use that origin's own connection.

### Focused-result pre-resolution

On a generated YouTube results page, a Play card held in focus for 700 ms may
own one speculative player-metadata resolver. Admission waits until page
transport is idle and the shared page budget has 2 MiB of working space plus a
1 MiB reserve. Its watch/player responses are independently capped at 1.5 MiB
and 512 KiB so optional work cannot grow to the ordinary foreground resolver's
larger limits. Input and rendering take precedence over browser-thread resolver
pumps. Moving focus, navigating, opening a modal, voice-memory reclaim, or
suspend cancels the single job.

Each focused result follows one explicit bounded state sequence: thumbnail,
dwell, resolving, then ready (or failed/consumed). A resolver cannot enter its
admission state until the focused card's thumbnail has decoded or the image
pipeline has classified that thumbnail as terminal. Moving focus retires the
old resolver before the page's next idle-image slice; if the requested row is
outside an active two-image batch, only that batch's uncommitted suffix is
cancelled and any thumbnail already published through layout is preserved.

The resolver may obtain signed media descriptors from YouTube, but it never
opens the media CDN before activation. Pressing Play transfers the same job—
complete or still in flight—to the media session, so no request is repeated.
Results age out after 90 seconds, and one selected result is not speculated
again until focus leaves it. A pending resolver older than 15 seconds is not
transferred: explicit Play receives a fresh full deadline instead. Details
remains an ordinary page navigation.

### Measurement

`FetchResult` records handshake attribution with explicit presence bits:
connect time, TLS time, connection reuse, negotiated TLS version, and HTTP
version. A measured zero is distinct from an unavailable measurement.
Validation builds aggregate these counters; release builds do not emit
per-request logs.

Transport performance is evaluated with three separate quantities:

1. code and read-only-data cost from the PSP ELF ratchets;
2. connection/handshake behavior from transport counters;
3. user-visible navigation and input latency on hardware.

Improving one is not accepted as proof of another. In particular, host and
PPSSPP handshake timing is useful for regression comparison but is not a PSP
latency claim.

## Security invariants

- The system clock must be valid before certificate verification.
- CA and hostname verification fail closed.
- TLS resumption never bypasses ordinary peer verification.
- A preconnect carries only the pinned User-Agent: no page credentials,
  cookies, referrer, or body.
- HTTP status codes, TLS-policy failures, and CDN throttling are not evidence
  that the PSP network stack regressed.
- Transport slots cannot grant request authority; they accept only a prepared
  request or an explicitly context-free native-service request.
- Teardown cannot unload networking while a slot may still execute inside it.

The broader origin, cookie, response-header, and cache-provenance rules are in
[the security model](../SECURITY_MODEL.md).
