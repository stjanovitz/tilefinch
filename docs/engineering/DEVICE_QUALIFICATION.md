# Device qualification

Tilefinch has three materially different execution environments. Every result
must name the environment which produced it.

| Evidence tier | Proves | Cannot prove |
|---|---|---|
| host Release/sanitizer tests | parser, layout, policy, demux, reducers, allocation, deterministic pixels | PSP ABI cost, SDK behavior, Media Engine output, cache coherency |
| PSP EBOOT under PPSSPP | Allegrex build, target wiring, native chrome/raster paths, controller and emulated SDK flow | real AVC output, WLAN timing, Memory Stick latency, hardware caches |
| physical PSP-3000 | firmware modules, Media Engine pixels/audio, WLAN, DMA/cache maintenance, fragmentation and cadence | no lower tier may substitute for it |

Use `PASS`, `PARTIAL`, and `UNTESTED` according to that table. A PPSSPP media
run is necessarily `PARTIAL` because the emulator does not implement the
firmware decode path Tilefinch uses.

## Why the tiers remain separate

- Host video uses a substitute backend; the PSP uses `sceMpeg*`,
  `sceAudiocodec`, DMA, and GE presentation.
- PPSSPP accepts some module, pointer, cache, and timing combinations which
  physical firmware rejects.
- A settled screenshot cannot reveal flicker, stale chrome, input latency,
  buffering cadence, or a bad provisional frame.
- Host milliseconds are comparative CPU evidence, not a calibrated 333 MHz
  clock.
- A successful syscall attempt is not a successful outcome; device counters
  record returned status and observed output.

The goal is not to make every edit wait on hardware. It is to assign each
risky boundary the smallest target-native test capable of proving it.

## PSP boundary contracts

### Threads and module loading

Thread state is observed through one PSP wrapper which initializes the SDK
structure size and classifies running, terminal, and query-error results.
Positive timed joins are separate from nonblocking observation; production
code is prevented from calling the fragile forms directly.

Module results are classified per module family. “Already resident” is not the
same as “loaded and owned”: shutdown unloads only modules Tilefinch owns.
Unknown negative results fail closed and retain the exact phase and status in
validation telemetry.

### Network lifecycle

APCTL and the network service rungs are owned by the
[network supervisor](PSP_NETWORK_SUPERVISOR.md). Setup may adopt exact
compatible resident state. Teardown closes transport admission, drains slot
leases, waits for APCTL leave, and unwinds initialized services in reverse
order. It never unloads networking beneath an executing curl or resolver call.

### Media and DMA

Firmware structures have Allegrex-only size and offset assertions. CPU-written
codec inputs receive bounded data-cache writeback before Media Engine access;
DMA/GE readers keep a generation-bearing lease until completion. A timeout
quarantines memory whose reader or writer may still be active.

Codec qualification checks returned module and firmware status, changed output
pixels, audio progress, timestamp ordering, and claimed/staged/displayed frame
identity. An emulator-clean demux/control path is not decoder evidence.

### Memory Stick publication

PSP FAT does not provide POSIX rename-over-existing behavior. Crash-sensitive
writers publish a complete temporary file only after flushing it, and rotate
or remove the explicitly stale generation before rename. A/B records retain
one valid generation through every injected failure point.

The complete write-frequency and recovery contract is in
[Storage](../STORAGE.md).

## Automated target panels

### Media fixture

`validation_media_fixture_auto=1` runs deterministic Baseline 320×240 and Main
640×360 AVC/AAC-LC files through the shipping demux and PSP backend, including
seek, profile change, teardown, and replay. Host tests open the same bytes.

```sh
scripts/run-ppsspp-network.sh --media-fixture-test
```

Only hardware may emit a media hardware pass after decoder pixels and audio
actually progress.

### Raster fixture

`validation_raster_fixture_auto=1` renders an embedded atlas through the PSP
font, tile, and RGB565 path. Assertions cover rounded-edge antialiasing,
palette behavior, fallback glyphs, and synthetic italic continuity.

```sh
scripts/run-ppsspp-network.sh --raster-fixture-test --timeout 30
```

The generated frame is a review artifact; structural checks are the automated
verdict.

### Scripted input during work

The live navigation-cancel scenario starts from native HOME, begins an actual
shared-worker navigation, presses Circle while response/parser work is active,
and captures loading, immediate acknowledgement, and settled cancellation.

```sh
scripts/run-ppsspp-input-script.sh \
  --script navigation-cancel-live --runs 2
```

Temporal snapshots first enter a bounded RAM ring and are written after the
run, so storage latency cannot shape the measured response.

### PSPLink runs

The preferred physical-device loop serves the PRX, configuration, and logs
through `host0:`. This avoids repeated Memory Stick writes and permits rapid
rebuild/run/collect cycles. See [PSPLink development loop](PSPLINK_DEV_LOOP.md).

Release-style storage paths still require a separate packaged-EBOOT check when
the feature under test depends on Memory Stick semantics.

## Required lifecycle scenarios

Device-sensitive changes select from this matrix:

- cold boot and controlled exit;
- network warmup, foreground demand, cancel during association, and reconnect;
- logical suspend/resume and retained-stack validation;
- page navigation while transport is active;
- media open, startup preroll, playback, buffering, forward/backward seek,
  close/reopen, decoder refusal recovery, and teardown;
- 240p and 360p video, 23.976/24 fps and 30 fps sources;
- voice component admission before and after a media lifetime;
- heavy page plus video near the configured memory boundary;
- signed update trial, health confirmation, rollback, and launcher recovery;
- storage publication and recovery for any changed persistent record.

Physical power-switch and externally controlled AP-loss checks are valuable
platform investigations, but are not automatic release prerequisites unless a
release changes those paths.

## Review rule

Before calling a device boundary covered, record:

1. Which production code path did the test execute?
2. Which dependency was substituted, emulated, or absent?
3. Is the verdict based on a structural invariant, an observed output, or an
   attempt counter?
4. What observation remains reserved for physical hardware?
5. Did validation logging or storage traffic perturb the behavior?

A test which cannot answer those questions stays diagnostic and is labelled
accordingly.
