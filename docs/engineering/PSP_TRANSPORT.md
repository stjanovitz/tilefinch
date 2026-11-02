# PSP transport contract

Tilefinch owns the complete HTTPS chain used by the PSP release build:

- curl 8.22.0;
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

### Client-only footprint

The owned PSP stack is client-only. What it compiles out, and why each removal
is safe:

| Removed | Why |
| --- | --- |
| Mbed TLS server role | the browser is only a TLS client |
| Mbed TLS debug formatters and feature-name table | numeric version reporting and certificate diagnostics remain |
| RIPEMD-160, SHA-3, NIST key wrap, DES, PKCS#5/#7/#12, CRL parsing, persistent PSA key storage, DTLS, CFB/OFB/XTS cipher modes | reached only through generic dispatch tables |
| TLS 1.2 static-PSK key exchanges | never offered without a configured PSK |
| RSA key generation (`GENPRIME`), deterministic ECDSA and HMAC_DRBG, CMAC, non-PKCS#7 cipher paddings | no client path uses them |
| `PK_WRITE` | only curl's public-key pinning used it |
| `FS_IO` | the CA bundle is loaded from memory |
| curl proxy, AWS SigV4, MIME/form API, terminal progress meter, option-name reflection | Tilefinch serializes its own request bodies and configures no proxy; private-network socket/prerequisite checks stay active |
| curl HTTP authentication | the browser never holds a credential, and `tilefinch_url_parse` rejects URLs with userinfo |
| curl date parser | only `TIMECONDITION`, `FILETIME`, cookies, HSTS, alt-svc and the unread `CURLINFO_RETRY_AFTER` used it |
| curl local-address binding | no client path binds a local address |
| curl verbose strings (`CURL_DISABLE_VERBOSE_STRINGS`) | the browser never sets `CURLOPT_VERBOSE` or a debug callback; the PSP curl patch keeps the `curl_*_strerror()` tables under `TILEFINCH_CURL_KEEP_STRERROR`, and `failf()` messages are unaffected |
| curl client certificate and private-key loaders (`TILEFINCH_CURL_NO_CLIENT_CERT`), cipher-list parser (`TILEFINCH_CURL_NO_CIPHER_LIST`) | setting either option fails as not built in; the ClientHello still offers the default `mbedtls_ssl_list_ciphersuites()` |

It retains TLS 1.2/1.3 clients, certificate verification, HTTP/2, WebSockets,
and the transfer callbacks used for cancellation and the browser's progress
UI. These removals are wire-neutral: the ClientHello, the offered groups,
signature algorithms and cipher suites, certificate verification and the
serialized-session header are unchanged by them.

The same `MBEDTLS_USER_CONFIG_FILE` reaches the TLS library, curl and native
consumers because role switches can change public structure layouts. Both
external projects track that header as a configure dependency. Changing
`patches/curl-8.22.0-psp.patch` re-extracts the curl archive in an existing
build tree, because ExternalProject reruns a patch step only when its command
line changes.

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
- two optional WebSocket lanes retain one 64 KiB receive message and one
  64 KiB outbound message each, with one browser-visible event of backpressure;
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

### Attributing long worker calls on hardware

Validation EBOOTs and their PSPLink developer PRXs include bounded linker
probes; ordinary shipping builds do not. `tilefinch-transport-call` reports
elapsed and worker **run-clock** time for a slow `curl_multi_perform` or
`curl_multi_poll` (`op=perform|poll`), followed
by scheduling counts and inclusive DNS/connect/select/recv/send/semaphore/delay
timings. `release=0` means the worker did not voluntarily block during that
call. Primitive times can include preemption and nested calls; do not add them
as disjoint CPU categories. Records are snapshotted before the diagnostic
logger takes its own semaphore. This logging is a validation-only exception
to the worker's no-logger contract, not shipping telemetry.

After curl successfully enables nonblocking mode, the probe reads back
`SO_NONBLOCK` through **libc `getsockopt`**. Curl has a libc descriptor, not a
firmware socket number. `CURLOPT_SOCKOPTFUNCTION` runs before curl enables
nonblocking mode, so reading there would produce a misleading failure. The
probe never changes socket mode, scheduling, or request ownership, and preserves
the wrapped call's result/errno. It also records zero-timeout `select` separately.
The `net-select` primitive measures the SDK's underlying `sceNetInetSelect`;
its final diagnostic argument is the requested timeout in microseconds
(zero is exactly zero, -1 means no timeout), not a socket descriptor.

### Worker stall decisions

Long `curl_multi_perform`/`curl_multi_poll` wall times on hardware are mostly
descheduling: firmware network services that run below the browser thread
need CPU to finish a socket probe, alongside genuine TLS CPU cost. Three
decisions follow:

- **Do not replace select with poll.** A zero-timeout `sceNetInetPoll` before
  the select moves the stall into the extra poll; it does not remove it.
- **Do not raise the two `sceNetInit` service priorities.** Raised priorities
  were verified on device and the stalls remained; the original priorities
  are kept. Raising the worker's own priority wholesale would instead let a
  multi-second TLS CPU burst block browser work.
- **The owner CPU-donation lease is the retained fix.** At a browser-owner
  checkpoint, after input and presentation and only without cancellation,
  the owner sleeps a bounded 2 ms slice at most once per 4 ms when the worker
  has runnable setup work or holds the poll lease it publishes around its
  existing poll. Setup eligibility is published before the worker lowers its
  priority and cleared after restoring it. No extra poll is issued, the
  checkpoint changes no thread priority, and no request deadline or
  cancellation rule changes. With no active setup or poll, the checkpoint
  returns without a clock read or sleep; an idle worker waits on its event
  flag.

A setup-time reclassification (`CURLINFO_PRETRANSFER_TIME_T`) is not a stall
fix; do not retry it without a fixture that isolates header-wait starvation.

Handshake acceleration does not weaken certificate, hostname, protocol, or
cipher validation.

### Key exchange

Mbed TLS advertises x25519 before P-256. The PSP build enables Project
Everest's verified Curve25519 implementation through
`MBEDTLS_ECDH_VARIANT_EVEREST_ENABLED`. The standalone crypto self-test checks
generic and Everest results against RFC 7748 vectors and requires both paths
to agree.

### Entropy

Every TLS secret on the PSP — PSA's DRBG seed, curl's CTR_DRBG seed and its
prediction-resistance reseeds, ECDHE private keys, client randoms — comes from
mbed TLS's entropy module, so its strong source decides whether a recorded
session can be decrypted later.

**Why not libcglue's `getentropy()`.** It calls `time()`, seeds
`sceKernelUtilsMt19937Init` with it and reads MT19937. Its output is fully
determined by the wall-clock second of the call, so a passive observer who
knew roughly when a handshake happened could enumerate the seeds.

**What the PSP offers user mode.** Nothing better than timing:

- KIRK's PRNG (command 0xE via `sceUtilsBufferCopyWithRange`) is exported by
  the kernel `semaphore` library; pspsdk ships only a kernel stub
  (`libpspsemaphore_660.a`) and a user-mode EBOOT cannot import it. Reaching it
  through a kernel PRX, kubridge or the savedata `sceChnnlsv` path would tie
  TLS to custom firmware or undocumented module loading.
- `sceKernelUtilsMt19937*` is a deterministic PRNG; the OpenPSID, MAC address
  and battery/analog readings are constant or slow and observable.
- CP0 Count and the timer registers are kernel-only. The finest clock is the
  1 MHz system timer, read by syscall.

**The source** (`src/psp_entropy.c`, pure half `src/psp_entropy_pool.c`)
measures, with that timer, two things a remote observer cannot reconstruct:
how long 32 read-modify-writes through the *uncached* alias of a 4 KiB buffer
take (DDR arbitration against the Media Engine, GE and DMA, interrupt entry,
tick phase), and how long the shortest `sceKernelDelayThread` actually sleeps
(timer-interrupt latency plus whatever else ran). Batches of 256 work samples
and 64 sleep samples alternate. Every sample is hashed into a SHA-256 pool;
credit is assigned per batch:

1. Estimate the per-sample min-entropy as `-log2(p)`, where `p` is the 99%
   upper confidence bound (z = 2.576, SP 800-90B style) on the hit rate of the
   best of several predictors: most-common value, repeat at lags 1..8, and a
   linear-trend guess. Constants, alternations, short cycles and drifting
   phase between two clocks all score near zero.
2. Cap that at 1 bit per sample, then divide by 4, rounding down. The samples
   are not independent and an unanticipated predictor could beat ours; the
   divisor is the margin for both. A batch therefore earns at most 64 bits
   (work) or 16 bits (sleep).
3. Release output only once 256 bits are credited. From then on the pool is a
   hash generator with a one-way key ratchet after every output, fed the clock
   on every call; mbed TLS counts its bytes 1:1, as `getrandom()` output is
   counted after the kernel pool initializes.

Nothing else is credited. The boot context (clock, wall time, free memory,
thread id, stack address) and the persisted seed file are mixed in only: a
copied or restored Memory Stick replays them. libcglue's `getentropy()` is not
used at all.

**Wiring.** `cmake/psp_transport/mbedtls_user_config.h` defines
`MBEDTLS_NO_PLATFORM_ENTROPY` and `MBEDTLS_ENTROPY_HARDWARE_ALT` (and refuses
NV seed, an external PSA RNG or disabled default sources), so
`mbedtls_entropy_init()` registers `mbedtls_hardware_poll`
(`src/psp_entropy_mbedtls.c`) as the only strong source, for curl's legacy
context and for PSA's alike. `cmake/PspOwnedTransport.cmake` links the hook and
the source immediately after `libmbedcrypto.a` on both the transport and the
crypto interfaces, so the browser, the launcher and the crypto selftest all
resolve to it. `tests/test_psp_sdk_contracts.py` pins all of this.

**Release policy.** `psp_entropy_configure()` picks one per process:

| Process | Policy | Why |
|---|---|---|
| Shipping browser | `REQUIRE_CREDIT` | TLS keys; no output before 256 bits |
| Validation browser | `PERMIT_UNCREDITED` | runs under PPSSPP, whose instruction-counted clock may earn nothing |
| Launcher | `PERMIT_UNCREDITED` | `psa_crypto_init()` seeds the RNG, but signature verification never draws from it |
| Crypto selftest | `PERMIT_UNCREDITED` | known-answer vectors; runs under PPSSPP |

Under `REQUIRE_CREDIT`, one attempt gathers for at most 0.75 s (plus one batch)
or 128 batches. If it falls short, the poll fails, `psa_crypto_init()` fails and
curl's global initialization reports failure; the credit already earned is
kept, so the next request's attempt continues from there. Under
`PERMIT_UNCREDITED` the pool is released after one attempt either way and the
report says which.

**Seed file.** `entropy-seed.bin` in the install's data directory: `"TFES"`,
version 1, 32 seed bytes, then 8 bytes of SHA-256 over the rest (torn-write
detection only). It is read when the pool first seeds, never at boot, and a
successor drawn from the pool before any caller output is written back right
after seeding, so it carries each boot's entropy forward without any boot
reusing it as output. Missing, wrong-length, damaged or all-zero files are
ignored.

**DNS query IDs** use `psp_entropy_fill_best_effort()`: the same pool, without
waiting for credit (the first lookup precedes the first handshake), plus one
timing sample per call. That is enough for an off-path guess; it is never used
for keys.

**Cost.** The first Home paint and input sample do not wait for seeding.
After paint, association wait frames optionally mix the existing seed,
gather one complete estimator batch per admitted slice, and perform the
ordinary credited release in its own slice. An immediate navigation's
foreground connection wait uses the same preparation owner after checking
controls. Optional warm-up never releases uncredited output, even with the
validation policy; if it refuses, ordinary demand retains its original
bounded attempt and configured policy. It adds no network requests and
moves, rather than duplicates, the existing seed-file read and write.

Without completed preparation, the remaining first seeding runs inside
curl's global initialization on the browser thread: at least 4 work and 3 sleep batches
(1,216 samples) when every batch earns the maximum, which should take tens of
milliseconds, plus one small Memory Stick read and a 48-byte write. After
release, each mbed TLS entropy poll costs about ten SHA-256 compressions for
its 128 bytes. The actual sample times and estimates are only known from a device
run:

```text
tilefinch-entropy: event=seeded mode=credited credited-bits=... target=256 attempts=1 elapsed-us=... seed-file=loaded work-batches=... work-mbits=MIN..MAX work-sample-us=... sleep-batches=... sleep-mbits=MIN..MAX sleep-sample-us=...
tilefinch-entropy: seed-file=written
```

`mode=uncredited` on hardware means the jitter estimate fell short within one
attempt; `event=unseeded` is the shipping build refusing. Either must be
understood before a release.

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

googlevideo media ranges negotiate TLS 1.2. The media servers send no TLS 1.3
tickets on `/videoplayback`, so a TLS 1.3 media reconnect is always a full
handshake (0.5-1.5 s on hardware). A TLS 1.2 ticket arrives inside the
handshake itself, and a resumed TLS 1.2 handshake does no key exchange and
verifies no certificate: 70-200 ms on hardware, across launches too. The
ClientHello hook re-enables the TLS 1.2 tickets libcurl turns off, and every
googlevideo requester (player ranges and the range probe) asks for TLS 1.2,
because curl reuses a connection only for requests with the same TLS settings.
The cost is that a resumed TLS 1.2 connection reuses the ticket's master
secret instead of a fresh key exchange; certificate and hostname checks on
the full handshake are unchanged.

The browser thread and the transport worker share one curl share, so it
carries a single recursive lock for every category (the PSP has one core;
finer locks buy nothing and one lock cannot deadlock on category order).
Suspend and exit export the live share into the store before writing it, and
an unchanged cache neither duplicates entries nor rewrites the file.

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
- The shipping build's TLS RNG seeds only from credited timing jitter
  ([Entropy](#entropy)); nothing time-seeded is ever counted as entropy.
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
