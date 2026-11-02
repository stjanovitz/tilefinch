# Development and test workflow

For contributors: how to build, test, and qualify a change. Measured results
are kept in maintainers' private ledgers, not here; see
[AGENTS.md](../AGENTS.md#verification-discipline) before repeating an
experiment.

## Quick start

The desktop lab builds and tests the same engine on your computer, at PSP
screen sizes and memory profiles. The first configure downloads hash-pinned
dependencies. Host builds also need libcurl development headers (with a
TLS-capable libcurl) and zlib.

```sh
cmake --preset release
cmake --build build-preset-release -j8
ctest --test-dir build-preset-release -j8 --output-on-failure
```

Running the built frontends is documented in
[LAB_USAGE.md](engineering/LAB_USAGE.md).

## Edit loop

`scripts/dev.sh` is the edit loop. It builds only the binary a command needs
and reconfigures its tree whenever `CMakeLists.txt`, `cmake/`, `patches/`, or
the script itself changes:

```sh
./scripts/dev.sh                 # interactive lab only
./scripts/dev.sh run --fixture fixtures/interactive.html --ticks 2
./scripts/dev.sh static          # static renderer only
./scripts/dev.sh unit foundation # one aggregate unit-suite filter
./scripts/dev.sh unit            # core: foundation + runtime + layout
./scripts/dev.sh test            # build registered test binaries + CTest
./scripts/dev.sh release         # optimized interactive lab
./scripts/dev.sh test-release    # optimized local test gate
./scripts/dev.sh verify          # optimized tests plus three network/replay gates
./scripts/dev.sh sanitize        # separate ASan/UBSan tree and tests
./scripts/dev.sh psp             # the named-target PSP cross-build
```

- Its default tree, `build-dev`, is `Debug` with `-O0 -g0` for short compile
  latency. Optimized commands use `build-release`; live-site, performance,
  and final validation belong there. The release and sanitizer trees reuse
  dependency sources from `build-dev` instead of downloading them again.
- `build-dev` is not the `dev` CMake preset's `build-preset-dev`. "The dev
  tree" in these docs always means `build-dev`.
- `unit` builds only `tilefinch_core` and the aggregate `tilefinch-tests`
  before running the filter; it is the shortest loop for subsystem edits.
  `test` builds every executable-backed registered test plus the frontends
  that script-backed tests use, so a clean tree never reports `Not Run`.
- Every helper tree uses the vendored Bellard QuickJS, so `test-release`,
  `verify`, and the readiness workflow qualify the VM the PSP build ships.
- Override the trees or job count with `PSP_BROWSER_BUILD_DIR`,
  `PSP_BROWSER_RELEASE_BUILD_DIR`, `PSP_BROWSER_SANITIZE_BUILD_DIR`, and
  `PSP_BROWSER_JOBS`; set `PSP_BROWSER_DEP_SOURCE_BUILD` when dependency
  sources live in another tree. CMake uses `ccache` or `sccache` when one is
  installed.

Release, sanitizer, network/replay, and long-session qualification stay
explicit gates rather than part of every edit.

## Presets and build trees

| Configure preset | Directory | Type | Purpose |
|---|---|---|---|
| `release` | `build-preset-release` | Release | the canonical host gate and the tree every example uses |
| `release-lto` | `build-preset-release-lto` | Release + LTO | optimized host build with link-time optimization |
| `sanitize` | `build-preset-sanitize` | Debug, ASan + UBSan | sanitizer gate |
| `hostile` | `build-preset-hostile` | as `sanitize` | the hostile-input parser harness only |
| `dev` | `build-preset-dev` | RelWithDebInfo | not the edit loop: needs a fresh `cmake --preset dev` before use, because an existing tree goes stale and its CTest can run stale binaries |
| `psp` | `build-preset-psp` | MinSizeRel | the shipping PSP cross-build |
| `psp-http1` | `build-preset-psp-http1` | as `psp` | current stack with HTTP/1.1 only, for comparison |
| `psp-hardened` | `build-preset-psp-hardening` | as `psp` | compiler-hardening candidate |
| `psp-legacy-transport` | `build-preset-psp-legacy-transport` | as `psp` | non-release SDK-transport escape hatch |
| `psp-replay` | `build-preset-psp-replay` | as `psp` | hermetic replay: a curl stub replaces the network stack |

| Other tree | Made by | Purpose |
|---|---|---|
| `build-dev`, `build-release`, `build-dev-sanitize` | `scripts/dev.sh` | the edit loop |
| `build-preset-psp-validation` | `cmake --preset psp -B ... -DTILEFINCH_PSP_VALIDATION_LOG=ON` | the PSP validation build every PPSSPP, PSPLink and device script runs ([below](#psp-validation-tree)) |

All presets use the pinned Bellard QuickJS, disable the unrelated
JavaScriptCore spike, and use a compiler cache when available; the host
presets build the tests. `release`, `sanitize`, and `hostile` have matching
build and test presets; the PSP build presets build `psp-browser-script`.

**Validation** has one meaning in these docs: a *validation build* is a PSP
build configured with `TILEFINCH_PSP_VALIDATION_LOG=ON`, normally the
`build-preset-psp-validation` tree. It logs bounded reports, honours the
`validation_*` keys in `boot.cfg`, and raises the `.text` ratchet to its
validation limit; never ship it. The legacy `psp-validation/` directory is an
unrelated standalone diagnostic EBOOT that nothing builds.

Configuring into a directory inside the source tree that `CMakePresets.json`
does not name fails on a fresh configure. `CMakeLists.txt` allowlists the few
non-preset trees that scripts default to; `-DTILEFINCH_ALLOW_BUILD_DIR=ON`
admits a deliberate experiment, and such a tree should be deleted afterwards.
Check dependency-source and download-cache references before pruning any
tree: another tree's cache may point at its sources.

A Release configure does not select the PSP runtime policy: pass
`--psp-profile` to a frontend when that behavior is under test. For
shipping-shaped memory runs also pass `--psp-fonts` to the interactive lab,
which loads the packaged Latin faces instead of the larger desktop fallbacks;
the `runtime-fonts` receipt records the mode and bytes. Keep full-font
visual-oracle runs separate.

### Measuring build time

Measure the complete targeted build command, not individual compiler
invocations, in a disposable worktree nothing else is editing or building:

```sh
python3 benchmarks/measure-incremental-build.py \
  --build build-preset-release --output /path/to/build-timings --reconfigure
```

The benchmark touches source mtimes and restores them, requires compile
evidence for edit samples, and keeps full logs. A no-change configure must
leave the vendored QuickJS sources' contents and mtimes unchanged
(`--reconfigure` asserts this); otherwise every consumer of `quickjs.h`
rebuilds. A compiler cache speeds repeated identical compiles and branch
switches, not a genuinely new edit.

## Test suite

### Suites and filters

The aggregate `tilefinch-tests` executable has two registered filters; the
layout suite is its own executable beside the layout translation units:

```sh
./build-preset-release/tilefinch-tests --list
./build-preset-release/tilefinch-tests --filter foundation
./build-preset-release/tilefinch-tests --filter web-runtime
./build-preset-release/tilefinch-layout-tests
```

`core` is an alias for foundation and web-runtime (`scripts/dev.sh unit core`
also runs the layout executable). CTest exposes the same suites as
`tilefinch-foundation-tests`, `tilefinch-web-runtime-tests`, and
`tilefinch-layout-tests`, plus focused executables for URL and session
security, fetch streaming, the engine facade, JavaScript responsiveness,
pumpable navigation, and loopback redirects:

```sh
ctest --test-dir build-preset-release -R 'tilefinch-(url|session-security)-tests' \
  --output-on-failure
ctest --test-dir build-preset-release -L security --output-on-failure
```

Use the narrowest filter while editing, then run the Release and sanitizer
gates and the aggregate executable before treating a change as ready. The
single-behavior modes of the test executables are listed in
[Host test modes](engineering/HOST_TEST_MODES.md).

Host tools and tests load one shared `tilefinch_core` image, so an engine
edit rebuilds that image without relinking every test; the build dependency
still completes the image before any test starts. PSP configurations link a
static core. The Release fidelity gate reserves four CTest processor slots and
renders its scenarios concurrently, keeping scoreboard rows in manifest order.

### Adding a test binary

Create test executables with `tilefinch_add_test_binary` in
`cmake/TilefinchTests.cmake`, which also registers their build dependency. A
bare `add_executable` there could leave `dev.sh test` running a stale
executable. Release tests that use C `assert` must undefine `NDEBUG` before
including `<assert.h>`.

### Optional private tests

The default configure, build, and CTest suite need only the public tree.
Reproducing inputs and investigation tests belong in the ignored
`.private-investigations/tests/` directory, not in public fixtures or inline
public test cases. To include a local suite:

```sh
cmake --preset release -DTILEFINCH_ENABLE_PRIVATE_TESTS=ON
cmake --build build-preset-release --target tilefinch-test-binaries -j8
ctest --test-dir build-preset-release -L private --output-on-failure
```

`TILEFINCH_PRIVATE_TESTS_DIR` can point at another directory containing
`CMakeLists.txt`. Register its executables with `tilefinch_add_test_binary`
and its tests with `tilefinch_add_private_test(name COMMAND ... LABELS ...
TIMEOUT ...)`; they get the `private` label and join the build-before-test
gate, and full CTest runs include them. An enabled suite whose directory is
missing fails configuration. Select only public tests with
`ctest --test-dir build-preset-release -LE private`. Public tests and shipping
targets never depend on private files.

### Sanitizers, ThreadSanitizer, and the hostile harness

```sh
cmake --preset sanitize
cmake --build --preset sanitize
./scripts/run-sanitizer-tests.sh

cmake --preset hostile
cmake --build --preset hostile
ctest --preset hostile
```

`sanitize` enables AddressSanitizer and UndefinedBehaviorSanitizer with frame
pointers. It builds QuickJS at `-O1`, because unoptimized instrumented
interpreter frames can exhaust the unchanged stack guard on small, finite
component trees; no stack or heap limit changes. It keeps the FFmpeg media
backend but disables SDL audio output, avoiding SDL's Cocoa/LaunchServices
bootstrap in many parallel tests.

On macOS, `scripts/run-sanitizer-tests.sh` runs the suite in parallel with
symbolization off, then reruns only failed tests serially with symbols while
keeping the failed status. Override its workers (two on macOS, eight
elsewhere) with `TILEFINCH_SANITIZER_JOBS`.

ThreadSanitizer cannot share a binary with AddressSanitizer, so it has no
preset. The ordinary host trees build one TSan test,
`tilefinch-psp-media-ownership-tsan-tests` (label `tsan`), whenever the
compiler links `-fsanitize=thread` and no other sanitizer is in
`CMAKE_C_FLAGS`. It is the PSP media slot hand-off stress and fails on the
first race report, so it runs in the normal gate.

The hostile-input parser harness runs under the sanitizer configuration; its
build preset builds only the harness and its test preset runs only that test.
It does not replace a coverage-guided fuzzer ([fuzzing](../tests/fuzz/README.md)).

### Media timing gates

The one-hour media cadence check advances a virtual clock in a tight loop. It
is an occasional policy check, neither registered with CTest nor built by
default:

```sh
cmake --build build-preset-release --target tilefinch-occasional-media-timing
```

The real-time media resilience gate generates a temporary video-only
fragmented MP4 and serves it through deterministic local fault profiles:
healthy bursts, per-request setup delay, truncated responses, slow trickles,
asymmetric audio/video supply, a 30-second outage with recovery, buffering
hysteresis, and the PSP's publication quantum. No third-party media or network
service is involved:

```sh
./scripts/run-media-resilience-gate.sh build-preset-release
```

It stays outside CTest because it preserves wall-clock cadence. It does not
replace a hardware playback soak: host curl cannot reproduce PSP WLAN,
firmware decoder, DMA, or thread-priority timing.

## Evidence gates

Each gate proves a different thing, and a pass on one is never taken for
another ([device qualification](engineering/DEVICE_QUALIFICATION.md) assigns
each claim to an environment):

| Gate | What it proves |
|---|---|
| focused unit and fault-injection tests | bounds, rollback, parser policy, reducers, allocators |
| frame-pump policy proof (`tests/test_frame_pumps.c`) | every workload and completion against every distinct refusal set; the pinned set of pumps able to contend for one resource in a frame |
| ledger owner checks | no thread but its owner mutates a `Budget` (aborting on the host; counted in an opted-in PSP run, where the workers exist) |
| Canvas/WebGL game lane | animation lifecycle, Gamepad sampling, pixel output, indexed geometry and graphics bounds |
| selected upstream WPT | web-platform behavior against unchanged tests |
| response-keyed replay | deterministic network inputs and closed request ledgers |
| Chrome fidelity scoreboard | structural pixel similarity at the PSP viewport |
| PSP cross-build ratchets | 32-bit ABI, actual `.text`, `.rodata`, stack and hot-symbol size |
| work record (`tilefinch-work`) | deterministic operation counts, identical on host, PPSSPP and device, for comparing changes without timing noise |
| PPSSPP | packaged EBOOT, Allegrex execution, deterministic scripted flows |
| offline device replay | a recorded journey replayed on the PSP from `host0:`, with responses released on a fixed clock so a faster build cannot fake a faster network |
| physical PSP | firmware, caches, WLAN, Memory Stick, latency, media and lifecycle truth |

Performance claims come from the device. Any relink moves code placement in
the PSP's small instruction cache and with it single paths and whole
journeys, so a small engine change is judged with **one binary and a runtime
switch over alternating runs**, never two builds. Ideas that do not pay off on
the hardware are recorded as rejected in the private ledgers; this page keeps
no measured figures.

Release builds compile out logging and HTTP capture and replay. Validation
builds aggregate counters in RAM and normally publish one bounded report
through PSPLink, avoiding Memory Stick traffic during a run; per-event
flushing is reserved for localizing crashes.

## PSP cross-build

The PSP build is a separate gate, never run by CTest
([AGENTS.md](../AGENTS.md#the-psp-cross-build-is-a-separate-gate) lists what
only it enforces). Build named targets only:

```sh
PSPDEV=/path/to/pspdev cmake --preset psp
cmake --build build-preset-psp \
  --target tilefinch_core psp-browser-fixture psp-browser-script
```

### PSP transport dependencies

The live-network PSP build consumes project-owned, hash-pinned curl, Mbed
TLS, and optional nghttp2 archives. Populate the ignored offline cache once
before configuring a fresh PSP tree:

```sh
./scripts/fetch-psp-transport-deps.sh
```

[PSP transport](engineering/PSP_TRANSPORT.md) covers provenance, the
`psp-http1` comparison build, and the `psp-legacy-transport` escape hatch.

### PSP validation tree

`build-preset-psp-validation` is the `psp` preset configured into its own
allowlisted directory with validation logging. The PPSSPP input-script,
device-cost, and latency scripts run it:

```sh
export PSPDEV=/path/to/pspdev
cmake --preset psp -B build-preset-psp-validation \
  -DTILEFINCH_PSP_VALIDATION_LOG=ON
cmake --build build-preset-psp-validation --target psp-browser-script
```

Like every `psp` configure it needs the transport cache and, because
`PSP_BROWSER_ENABLE_PSP_VOICE` defaults on, PocketSphinx. In a sandboxed or
offline tree add `-DPSP_BROWSER_ENABLE_PSP_VOICE=OFF` (the EBOOT then has no
voice component) or pass
`-DPSP_BROWSER_POCKETSPHINX_SOURCE_DIR=/path/to/pocketsphinx-5.1.1`; see
[Dependencies](#dependencies).

The validation interpreter compiles in execution-census checks. Setting
`validation_execution_census=0` stops the sampler but not the checks; for a
device timing control, configure the same tree with
`-DPSP_BROWSER_EXECUTION_CENSUS=OFF`, rebuild the named targets and the
PSPLink PRX, keep the boot configuration, input script, and capture
unchanged, and compare completed replies and final pixels. Restore `ON`
before collecting family attribution. Shipping PSP builds never compile the
hooks.

Keep the runner and binary unchanged until PSPLink reports `Load/Start`: the
runner prints the PRX's SHA-256 before loading and rejects a changed artifact
afterward, so save that output with the log. Never edit the shell runner while
it waits, because shells can read its remaining commands lazily. Build the
next variant only after the previous one is resident. The device loop is in
[PSPLink device development](engineering/PSPLINK_DEV_LOOP.md).

### Running under PPSSPP

On macOS every automated PPSSPP run goes through LaunchServices
([AGENTS.md](../AGENTS.md#the-psp-cross-build-is-a-separate-gate)). The
input-script runner, its options, and scenario criteria are in the
[input script reference](engineering/INPUT_SCRIPT_REFERENCE.md). Scenarios
that need local pages serve `tests/fixtures` on loopback:

```sh
python3 -m http.server 8779 --bind 127.0.0.1 --directory tests/fixtures
# In another terminal:
scripts/run-ppsspp-input-script.sh --script runtime-input-live --runs 2 \
  --url http://127.0.0.1:8779/cooperative-runtime-input.html
```

Live-page scenarios and what each must show:

| Scenario | Start URL | Inspect |
|---|---|---|
| `section-disclosure-live` | `http://127.0.0.1:8779/cooperative-disclosure.html` | `before`, `expanded`, `collapsed` captures and `tilefinch-control-activation` while 200 paragraphs open and close with timers pending |
| `wikipedia-article-section-live` | an article with a `#History` fragment | native pointer target `id=History`; the last `section-*` capture shows History prose, not just a focus box; at normal emulator speed, activation at most 1.25 s, flow layout at most 750 ms, and no resource-rebuilding relayout |
| `youtube-results-focus-live` | a provider results page | `result-next`, `result-below-fold`, `results-filled`: the selected card and scroll move before thumbnails arrive and stay put afterward |
| `youtube-detail-poster-live` | a watch page | `poster-ready`: the play card holds the downloaded poster after idle work; compare `tilefinch-adapter-commit` resource time apart from transport time (no video plays) |

```sh
scripts/run-ppsspp-input-script.sh --script wikipedia-article-section-live \
  --url 'https://en.wikipedia.org/wiki/PlayStation_Portable#History' --runs 2
```

A receiver golden proves input delivery and script completion, not visual
correctness, so read the captures. Live layouts change: update a cursor
route only after inspecting the page, and keep raw journey logs outside the
repository. A zero layout-phase counter can mean instrumentation is
unavailable, not that the work was free. The host-only
`TILEFINCH_DISABLE_ANCESTOR_CACHE=1` disables layout-scoped selector caches
for timing comparisons.

`tilefinch-focus-feedback` measures an accepted focus publication from the
frame's input sample, including a repaint that finishes in later render
slices; it requires a newer completed frame, the same navigation generation,
and a successful display publication, and reports a superseded input as
incomplete. A later marked screenshot includes runtime and frame scheduling
and is not that latency.

`TILEFINCH_PPSSPP_CPU_MHZ=166` or `111` gives a deterministic pressure
bracket (the default `0` is normal emulation); the runner writes PPSSPP's
`[CPU] CPUSpeed` setting, so check the run's generated `ppsspp.ini` before
trusting a new clock experiment.

If a background PPSSPP launch hangs before PSP boot on macOS, check PPSSPP's
own log for a fresh `Booted` line before blaming the EBOOT, and retry with
`TILEFINCH_PPSSPP_GRAPHICS=vulkan` (default `opengl`); the OpenGL path can
block while the window is occluded. The runner sets both the command-line
option and `GraphicsBackend` in the appended configuration, so confirm Vulkan
initialization in PPSSPP's log rather than trusting the variable. Keep the
backend identical across A/B captures. This is an emulator-window workaround,
not a PSP rendering change.

### Canvas and WebGL game qualification

The [WebGL authoring guide](WEBGL.md) defines the PSP profile this lane
qualifies. The lane exercises a Canvas paddle/ball loop, indexed WebGL,
point/line/triangle depth parity, uniform snapshots, packed-wire boundary
refusal, page-visibility suspension, Gamepad sampling, drawing-buffer limits,
pixel readback, object deletion, and invalid buffer/index ranges. It runs the
installable `examples/prism-break-3d/` game through physics, collision, level
transition, multiball, power-up, particle, and dynamic-mesh scenarios, and
`examples/treadline-arena/` through the fixtures described in
[Treadline Arena qualification](../examples/treadline-arena/QUALIFICATION.md),
which also covers its harness API, long soak, visual QA, and staging a game on
a PSP.

```sh
cmake --build build-preset-release \
  --target tilefinch-canvas-webgl-conformance-tests
ctest --test-dir build-preset-release \
  -R '^tilefinch-canvas-webgl-conformance-tests$' --output-on-failure
```

A bounded 600-frame allocation soak is opt-in:

```sh
TILEFINCH_WEBGL_SOAK_FRAMES=600 \
  build-preset-release/tilefinch-canvas-webgl-conformance-tests
```

It reports the QuickJS heap before, after, and at its high-water mark with
the number of charged allocations; the retained heap must stay within 256 KiB
of its pre-soak value after collection.

The synthetic pages in `tests/fixtures/canvas-webgl-games/` contain no
third-party game code and run in the host lab and the isolated PPSSPP
localhost harness. PPSSPP proves lifecycle, rendering, and cleanup, but its
software timing does not replace device qualification.

Device WebGL cadence runs use `tests/input-scripts/webgl-game-cadence.txt`
with Prism's `?qualification=ordinary`, `?qualification=heavy`, and
`?qualification=profile` URLs. The first mark resets validation-only counters
after warm-up. The profile mode splits the example's JavaScript cost into
update, geometry, upload, command setup, and HUD work. The terminal report
separates author callback time from native WebGL time and records
canvas-presentation median, p95, maximum, and 60/30 Hz deadline counts. These
probes are absent from shipping builds and expose no finer clock to pages.

#### Treadline visual QA: camera track and recorded gameplay

The camera-track sweep, recorded PPSSPP gameplay, reference screenshots, and
milestone review are in
[Treadline Arena qualification](../examples/treadline-arena/QUALIFICATION.md#treadline-visual-qa-camera-track-and-recorded-gameplay).

### Optional PSP components

**ARK-4 XMB redirect.** A small kernel PRX with no engine dependency:

```sh
cmake --build build-preset-psp --target tilefinch-xmb-redirect
```

It writes `build-preset-psp/xmb-redirect/tilefinch_xmb.prx`; the install tree
stages it under `TILEFINCH/OPTIONAL/`, never in an A/B slot or update
package. Its hook is name-based (`htmlviewer_plugin_module`), chains the
previous ARK handler, requires an XMB confirmation press, honours the
L-trigger bypass, and leaves Sony's browser available on any failure.

**Voice engine.** The ordinary `psp` build stages `tilefinch-voice.prx` beside
the browser EBOOT; PocketSphinx is not linked into the browser. The PRX loads
from the installed slot directory on the first voice request, under its own
768 KiB code/data reservation with allocations forwarded to the browser heap,
and is unloaded with its reservation released when the engine is evicted. A
decoder observed killed is quarantined for the process lifetime: never run
its `module_stop` or stdio cleanup. Updates carry the engine PRX with the
slot, so never deploy only `EBOOT.PBP` when testing it. For real module and
model initialization under PPSSPP or on a PSP:

```sh
cmake --build build-preset-psp --target psp-voice-component-probe -j8
```

Stage `voice-probe/EBOOT.PBP`, `tilefinch-voice.prx`, and `voice-model/`
together in an isolated game directory and launch through
`scripts/launch-ppsspp-safe.sh` with an isolated `HOME`. `voice-probe.txt`
records three load/create/cancel/destroy/unload cycles with Budget ownership
and newlib heap use. Microphone recognition still needs a physical PSP.

### Optional PSP software decoder

H.264 High-profile playback, and Baseline/Main streams the firmware refuses
(more than three reference frames, B-frame pyramids; CABAC or CAVLC,
progressive 8-bit 4:2:0 only), use a replaceable, user-built PRX, so the
official EBOOT and release archives stay independent of FFmpeg decoder
binaries. Other Baseline/Main MP4 and MPEG-TS HLS with AAC-LC use the
firmware backend. Prepare the narrow LGPL FFmpeg build once, then point an
opt-in PSP configure at that ignored workspace:

```sh
PSPDEV=/path/to/pspdev \
  ./scripts/prepare-swdec-ffmpeg.sh build-swdec
PSPDEV=/path/to/pspdev cmake --preset psp \
  -DTILEFINCH_PSP_ENABLE_SWDEC_COMPONENT=ON \
  -DTILEFINCH_SWDEC_SOURCE_DIR="$PWD/build-swdec"
cmake --build build-preset-psp --target tilefinch-swdec-bundle
```

The script checks out the pinned upstream tag, applies the committed patch,
and enables only H.264, `aac_fixed`, and the H.264 parser. A directory of
device-trained profiles as its second argument makes the component faster
near the PSP's real-time limit.

The target writes `build-preset-psp/tilefinch-swdec-addon/`:
`tilefinch-swdec.prx`, the resident `swdec-meload.prx` helper, and
`component-info.txt`. Copy all three into
`PSP/GAME/TILEFINCH/components/swdec/`, not into `slot-a` or `slot-b`. The
ordinary EBOOT already contains the loader, and signed app updates leave the
component directory alone. `component-info.txt` records the loader ABI; when
an update expects a different one, the update UI warns **Decoder rebuild
needed**, and all three files are replaced together. A version change alone
needs no rebuild.

The decoder PRX lives in user-partition memory outside the browser heap. When
the add-on is installed, the browser leaves 4 MiB of the partition outside its
heap instead of 2 MiB at startup (`src/psp_heap_reserve.c`), so the heap is
2 MiB smaller; without it nothing changes. Install or remove the add-on with
the browser closed. The validation boot line reports
`heap-threshold=4096KB swdec-reserve=1`.

Loading the component charges the page nothing. A software-decoded stream's
open charges its working set: the picture arena, sized for the reference
pictures the stream's SPS declares (at least 4, at most 16, and 16 when the
SPS cannot be read; a later SPS declaring more ends playback with a
reference-frame message), the 25-slot picture ring, the audio arena, and a
512 KiB playback packet. When the page holds too much, it gives memory back a
step at a time and the open is retried after each: optional memory first
(staged fonts, a collected script heap, caches, resident tiles), then the
page's scripts. The page keeps its DOM and position and, when the player
closes, says **PAGE SCRIPTS STOPPED: NOT ENOUGH MEMORY**. Stream buffering
stays in RAM.

Checking, profiling, and comparing the decoder are in
[Software-decoder diagnostics](engineering/SWDEC_DIAGNOSTICS.md).

### PSP compiler-hardening candidate

The PSP toolchain supports `-fstack-protector-strong` and
`_FORTIFY_SOURCE=2`. The candidate preset keeps them out of the shipping
preset until their device cost is qualified:

```sh
cmake --preset psp-hardened
cmake --build build-preset-psp-hardening \
  --target tilefinch_core psp-browser-fixture psp-browser-script
```

Its build enforces the same `.text` and hot-function ratchets as `psp`.
Newlib initializes one process-wide fixed stack guard, so this is corruption
detection and exploit friction, not a randomized canary. Do not enable it in
the `psp` preset from build evidence alone: promotion requires a real PSP
boot, navigation, and media smoke, a before/after input and frame-cadence
comparison, and the normal named-target cross-build.

## Dependencies and QuickJS

### Dependencies

The first configure downloads pinned archives and verifies their hashes:

| Dependency | Fetched for |
|---|---|
| Lexbor, stb, NanoSVG, DejaVu fonts, libwebp | every build |
| FreeType, brotli | `PSP_BROWSER_ENABLE_WEB_FONTS` (default on) |
| WAMR | PSP builds, and host builds with `PSP_BROWSER_ENABLE_HOST_WEBASSEMBLY` (default on); `PSP_BROWSER_WAMR_SOURCE_DIR` supplies a local tree |
| QuickJS-NG | only with `PSP_BROWSER_USE_BELLARD_QUICKJS=OFF` |
| PocketSphinx 5.1.1 | the `psp` presets, for the voice component |

The vendored Bellard QuickJS in `third_party/quickjs` is the default engine.
The PSP transport archives are not fetched by CMake; see
[PSP transport dependencies](#psp-transport-dependencies).
`PSP_BROWSER_VENDOR_DIR` can supply prepared dependencies in its documented
layout, and individual FetchContent sources can be supplied with
`FETCHCONTENT_SOURCE_DIR_<NAME>`. A checksum mismatch is a dependency or
configuration failure; never bypass it.

```sh
cmake --preset release -DPSP_BROWSER_VENDOR_DIR=/path/to/vendor
```

For an offline configure, populate each tree's `_deps/*-src` from an earlier
configure and pass `-DFETCHCONTENT_FULLY_DISCONNECTED=ON`. A fresh `psp` tree
without `_deps/pocketsphinx-src` can point
`-DPSP_BROWSER_POCKETSPHINX_SOURCE_DIR=/path/to/pocketsphinx-5.1.1` at an
unpacked source tree (the configure patches it in place), or leave voice out
with `-DPSP_BROWSER_ENABLE_PSP_VOICE=OFF`.

### QuickJS configuration

QuickJS is vendored in `third_party/quickjs` with the repository's changes
applied, and its two engine files are fingerprint-pinned at configure time.
Engine edits go into that tree and update the pins in
`cmake/TilefinchDependencies.cmake`; `third_party/quickjs/README.md` lists the
carried changes and the update procedure. Every configure selects the
vendored engine; `-DPSP_BROWSER_USE_BELLARD_QUICKJS=OFF` builds against
pinned QuickJS-NG instead. Use a separate raw CMake tree for an unpatched
upstream comparison.

The portable configuration enables one source-neutral interpreter shortcut:
a two-instruction function that only returns one captured value returns it
without constructing an unobservable frame. For an exact upstream dispatch
control, configure a fresh tree with
`-DPSP_BROWSER_QUICKJS_CAPTURE_GETTER_FASTPATH=OFF`; that control is built
from a binary-directory copy of the engine with the layer reversed and checked
against a pinned fingerprint, and the vendored tree is never modified.

The JavaScript bootstrap is generated from `src/bootstrap/*.js`; after
editing it, regenerate as described in
[the bootstrap README](../src/bootstrap/README.md#regenerating).

### Property-fault diagnostic

An opt-in, lab-only property-read diagnostic is applied, like the other lab
variants, to a binary-directory copy of the engine; ordinary builds compile
it out. The tree below is not a preset, so it needs the explicit allowance:

```sh
cmake -S . -B build-property-trace -DCMAKE_BUILD_TYPE=Debug \
  -DPSP_BROWSER_JS_PROPERTY_FAULT_TRACE=ON -DTILEFINCH_ALLOW_BUILD_DIR=ON
cmake --build build-property-trace -j8
TILEFINCH_TRACE_JS_PROPERTY_FAULTS=32 \
  ./build-property-trace/psp-browser-interactive-lab ...
```

The value is 1–128 records. Each gives the source position, function,
bytecode position and opcode, base and key types, property preview,
arguments, locals, and nearby bytecode. `absent` records only failed
JavaScript `in` checks; `fault` records only accesses whose base is `null` or
`undefined`, isolating the operation that throws. Use it to reduce a failure
to the first invalid value, never to rewrite third-party source or invent
browser capabilities.

### JavaScript heap composition

Host builds compile a read-only heap walk, `JS_ComputeMemoryDetail`
(`CONFIG_TILEFINCH_HEAP_DETAIL`, set for every non-PSP build in
`cmake/TilefinchDependencies.cmake`; no PSP image compiles it).
`TILEFINCH_TRACE_JS_HEAP_DETAIL=1` prints a census of a realm at the first
safe point after its allocator refuses a request, at every 4 MiB high-water
step (`TILEFINCH_TRACE_JS_HEAP_DETAIL_STEP_KB`), and with each memory report.
It covers allocator arenas and large blocks, every class with its property
arrays and payload, each script's bytecode, lazy stubs, line tables and
retained source, closures, instances per prototype, strings by size band,
atoms, shapes, pending jobs, `Function.prototype.toString` calls, the Budget
categories, and the refused request. `=gc` also collects after an OOM census
and censuses again, answering whether a collection would have freed enough.
Summarise a run directory (one subdirectory per page with `lab.log` and
`lab.err`):

```sh
TILEFINCH_TRACE_JS_HEAP_DETAIL=1 ./build-preset-release/psp-browser-interactive-lab \
  --url URL --psp-profile realistic --replay-http-response-keyed TRACE ... 2> run/page/lab.err
python3 tools/jsmem_report.py run          # composition table, MB
FILES=1 python3 tools/jsmem_report.py run  # plus top scripts and prototypes
```

Sizes are 64-bit host allocations: pointer-heavy rows (objects, shapes,
closures, bytecode headers) are larger than on the PSP; text rows (source,
strings, atoms) are the same.

## Source and target boundaries

`include/tilefinch/` holds the exposed contracts. Most subsystem headers have
a same-named implementation in `src/`; `budget_quickjs.h` is a header-only
shared adapter. New frontend code should prefer `browser_engine.h`; direct
subsystem headers remain mainly for tests and the compatibility seams in
[ARCHITECTURE.md](ARCHITECTURE.md).

`tilefinch_core` contains engine and subsystem implementations only. The
entry points `main.c`, `interactive_main.c`, and `failure_recovery_main.c` are
separate frontend or diagnostic targets; they and the host-portable
`psp_ui.c` are in the generated portability-source manifest.
`TILEFINCH_CORE_SOURCES` in `cmake/TilefinchCore.cmake` is the canonical core
inventory, so linking and portability auditing cannot diverge. The root
`CMakeLists.txt` sets project-wide policy and includes focused modules for
dependencies, core sources, product targets, and tests. PSP SDK-only entry
points are checked by the cross-build, not the host compile audit.

Large, tightly coupled owners may be split into private `.inc` seams under
`src/<owner>/`. Each is included exactly once by its same-named `.c` owner, in
dependency order. They are not translation units and must not be added to
`TILEFINCH_CORE_SOURCES`. This keeps private state and PSP code generation
unchanged while making responsibilities reviewable in isolation. The owner
file's include list is the map: start there and open only the relevant unit.
Aggregate test executables use the same pattern under `tests/suites/`.

## Test fault injection and diagnostic switches

Host tests inject failures through one struct, `TilefinchTestFaults` in
`src/tilefinch_test_faults.h` (`tilefinch_test_faults()` returns it).
Production code reads a field where the real failure would surface, and the
struct does not exist in device builds. Add a field there rather than a new
static in an engine file.

Every `TILEFINCH_*` environment variable the engine reads is listed in
[Diagnostic switches](DIAGNOSTIC_SWITCHES.md), with whether each read survives
a device build. `tests/test_diagnostic_switches.py` fails when a `getenv`
appears without a row or a row outlives its `getenv`; refresh site paths and
counts with `python3 tests/test_diagnostic_switches.py . --refresh-sites`.
