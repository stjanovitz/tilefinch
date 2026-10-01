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

The owned PSP stack disables Mbed TLS's server role, and curl's proxy, AWS
SigV4, MIME/form API, terminal progress meter and option-name reflection API.
Mbed TLS debug formatters and its unused feature-name table are also omitted;
numeric version reporting and certificate diagnostics remain available.
It retains TLS 1.2/1.3
clients, certificate verification, HTTP/2, WebSockets, and transfer callbacks
used for cancellation and the browser's progress UI. Tilefinch serializes its
own request bodies. Private-network socket/prerequisite checks remain active;
only the unavailable proxy setter is omitted in this configuration.

The same `MBEDTLS_USER_CONFIG_FILE` reaches the TLS library, curl and native
consumers because role switches can change public structure layouts. Both
external projects track that header as a configure dependency.

Ordinary EBOOT measurements when this footprint landed (actual `.text`,
unchanged 4,480,000-byte ceiling): original 4,482,252 bytes; client-only TLS 4,446,228;
unused curl subsystems removed 4,380,568. These are linked measurements, not
archive-size estimates. Speech extraction is a separate runtime-component
change, described in the development guide.

After speech extraction, removing those unused debug/reflection facilities
reduced ordinary `.text` from 4,127,224 to 4,091,068 bytes and `.rodata` from
1,932,060 to 1,904,756 bytes (including the small counter-index layout fix).
Both ordinary and validation named cross-build targets and all 149 enabled
host tests passed. These are linked ELF measurements, not estimates of
archive members that may already have been discarded by the linker.

Post-change PPSSPP qualification passed a small public HTTPS page (HTTP 200,
verified TLS, page load and clean teardown). The live Wikipedia article run
received HTTPS data but exceeded its timeout in CSS selector matching; it is
not counted as a passing page qualification or attributed to DNS. Physical
PSP networking/media checks remain separate release gates.

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

Physical-PSP measurements reproduced the reported long wall calls:

| Call | Elapsed | Worker CPU | Relevant evidence |
| --- | ---: | ---: | --- |
| Pre-fix diagnostic run | 15,582 ms | 1.18 ms | No DNS/connect/select; recv 26 us, send 377 us |
| Repeat with wait probes | 8,895 ms | 2,066 ms | Priority 0x21; release=0; semaphore total 758 us; no delay calls |
| Narrowed setup-classifier experiment | 8,941 ms | 2,037 ms | Same pattern; TLS connection completion dominated actual CPU work |

Observed nonblocking readbacks succeeded. These initial perform samples do
**not** support changing socket mode or replacing the multi API: their long
wall time is largely descheduling, with separate genuine TLS CPU cost. A reused
request's zero DNS/connect/TLS timers cannot identify what another connection
did inside the shared multi call.

Subsequent poll attribution isolated a second mechanism: a 6,397 ms poll used
only 318 us of worker CPU and spent 6,396 ms inside select, with two voluntary
wait releases. Direct firmware-select instrumentation then measured zero-timeout
calls taking 2,662 and 3,746 ms. A temporary zero-timeout `sceNetInetPoll` before
the select moved the stall into that extra poll (6,520 ms); it did not fix it.
The extra call was removed. Do not replace select with poll on this evidence.

Thread dumps showed firmware network service runnable below the browser.
Raising the two `sceNetInit` service priorities from 42 to 30 was also rejected:
the priorities were verified on device, but a completed repeat still recorded
a 3,755 ms poll. Other firmware dependencies remain; adjusting those two
priorities alone does not resolve the wait. Their original priorities are kept.

Rejected experiment: use `CURLINFO_PRETRANSFER_TIME_T` to stop classifying a
connected request waiting for headers as setup. Setup-call counts fell, but the
repeat's worst call was unchanged (8.895 vs 8.941 seconds); the change was removed.
It does not solve CPU sharing during actual TLS work. Do not retry as a claimed
stall fix without a fixture isolating header-wait starvation. Raising the worker
priority wholesale would instead let the roughly two-second TLS CPU burst block
browser work.

Bounded owner CPU-sharing experiments request a 2 ms sleep with at least 4 ms
between completed donations, after input/presentation and only without
cancellation. Setup eligibility is published before lowering worker priority
and cleared after restoring it. The owner checks whether setup work is runnable
only at the donation cadence, not at every layout checkpoint. Completed
setup-only runs reduced worst perform to 1,178–2,309 ms from the 8,895 ms
baseline. Page loads remained about 34–35 seconds; this is transport progress,
not a claim of faster navigation. Settled cursor feedback stayed about 7.2 ms
average / 16.7 ms maximum, 240/240 samples presented and none over 33 ms.
Requested sleeps can run longer through preemption (observed maximum 7.7 ms).

The retained fix also publishes a lease around the existing worker poll. While
that lease is active, the owner may donate even if the worker is waiting:
firmware services below both threads need that CPU window to complete a socket
probe. No extra poll is issued, no thread priority is changed by the checkpoint,
and no request deadline or cancellation rule changes. With no active setup/poll,
the checkpoint returns without a clock read or sleep. The poll lease is used
only in the existing branch with transport work; an idle worker waits on its
event flag as before.

Three final physical-PSP runs with the original firmware thread priorities:

| Scenario | Page load | Images loaded | Worst perform | Worst poll | Exit |
| --- | ---: | ---: | ---: | ---: | --- |
| Settled cursor, first | 37,110 ms | 17 | 2,688 ms | 88 ms | clean |
| Settled cursor, repeat | 36,825 ms | 22 | 2,299 ms | 59 ms | clean |
| Native menu/cursor | 37,242 ms | 17 | 2,671 ms | 127 ms | clean |

The settled tests each presented all 240 cursor samples, average 7.2 ms,
maximum 16.7 ms, none over 33 ms. The native-menu scenario completed its
15 action/wait steps and terminal `end`; its 12 cursor samples likewise stayed
under 16.7 ms. It began after commit with no pending resources, so it does not
prove interruption during initial resource loading. Across these runs the
owner donated 3.61–3.74 seconds total; the longest observed individual donation
including preemption was 13.5 ms. Image completion counts vary, so these live
loads are not a controlled throughput comparison. Multi-second TLS CPU and
page style/resource cost remain separate work.

Two earlier diagnostic runs returned to PSPLink before a report completed,
without user input. Their last persisted record was initial navigation, not
a recorded HOME request or clean exit; the cause remains unresolved. One used
the extra poll experiment and one did not, so removing that probe is **not**
proof of an exit fix. Keep the incomplete logs and do not count either as a
passing journey. The three final runs above completed, but do not qualify a
release or the script-enabled dynamic-request case.

All five baseline journeys completed the cursor/menu script and exited. They
used a script-free article response (`scripts discovered=0`); they reproduce
shared transport contention but do **not** qualify the dynamic auto-login script
path. Raw logs belong in private device artifacts, not the public repository.

Baseline gates: 150 host tests passed, external update-root proof skipped, optional
device-cost test disabled. Ordinary and validation named PSP targets passed;
ELF `.text` was 4,125,856/4,480,000 and 4,242,608/4,500,000 bytes respectively.
The ordinary ELF has no probe wrapper symbols. No release or XMB deployment was
performed for this investigation.

CPU-sharing fix gates: all 150 enabled, available host tests passed (external
update-root proof skipped, optional device-cost test disabled). Ordinary and
validation named PSP targets passed at 4,126,324 and 4,243,776 bytes of `.text`,
respectively, within unchanged ceilings. The new source-contract test failed
when the owner donation call was removed and passed when restored; bounded
cadence tests cover inactive work, spacing, clock reversal and integer limits.

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

**What was wrong.** The mbed TLS patch originally routed
`mbedtls_platform_entropy_poll` to libcglue's `getentropy()`, which calls
`time()`, seeds `sceKernelUtilsMt19937Init` with it and reads MT19937. Its
output was fully determined by the wall-clock second of the call, so a passive
observer who knew roughly when a handshake happened could enumerate the seeds.

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

**Cost.** Boot pays nothing. The first seeding runs inside curl's global
initialization on the browser thread: at least 4 work and 3 sleep batches
(1,216 samples) when every batch earns the maximum, which should take tens of
milliseconds, plus one small Memory Stick read and a 48-byte write. After
release, each mbed TLS entropy poll costs about ten SHA-256 compressions for
its 128 bytes. Adds about 5 KB of `.text` to the browser and 6 KB to the
launcher. The actual sample times and estimates are only known from a device
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
