# Development and test workflow

The short edit loop and the qualification loop are intentionally separate.
Use `scripts/dev.sh` for the fastest targeted rebuild, or the checked-in CMake
presets when reproducible configure/build/test directories are more useful.

## Fast edit loop

```sh
./scripts/dev.sh                         # interactive frontend only
./scripts/dev.sh unit foundation         # one aggregate unit-suite filter
./scripts/dev.sh unit                    # core: foundation + runtime + layout
./scripts/dev.sh run --fixture fixtures/interactive.html --ticks 2
./scripts/dev.sh test                    # build registered test binaries + CTest
```

The helper's `build-dev` tree uses `Debug` with `-O0 -g0` and disables the
unrelated JavaScriptCore spike. Debug, Release, and sanitizer helper trees all
use pinned upstream Bellard QuickJS, matching the presets and the
correctness-qualified PSP-facing engine configuration. The checked-in VM patch
is an opt-in diagnostic experiment via
`-DPSP_BROWSER_APPLY_BELLARD_QUICKJS_PATCH=ON`; it is not enabled by the helper
or presets. `unit` builds only `tilefinch_core` and the aggregate
`tilefinch-tests` executable before running the requested filter; it is the
shortest loop for subsystem edits. `test` deliberately builds every
executable-backed registered CTest, plus the frontends used by script-backed
tests, before invoking CTest. This keeps a clean tree from producing `Not Run`
results while leaving the default build command small. Release, sanitizer,
network/replay, and long-session qualification remain explicit. Use a separate
raw CMake tree for an intentional experimental-patch comparison.

The portable Bellard configuration enables one source-neutral interpreter
shortcut by default: a two-instruction function that only returns one captured
value can return that value without constructing an otherwise unobservable
interpreter frame. It adds no fields or caches to QuickJS objects. Configure a
fresh comparison tree with
`-DPSP_BROWSER_QUICKJS_CAPTURE_GETTER_FASTPATH=OFF` when an exact upstream
dispatch control is needed; CMake refuses to relabel an already patched source
tree as that control.

### Property-fault and frame-message diagnostics

The portable Bellard baseline has an opt-in lab-only property-read diagnostic.
It is compiled out of ordinary builds, adds no fields to their QuickJS
objects, and cannot be combined with the experimental VM patch:

```sh
cmake -S . -B build-property-trace -DCMAKE_BUILD_TYPE=Debug \
  -DPSP_BROWSER_JS_PROPERTY_FAULT_TRACE=ON \
  -DPSP_BROWSER_APPLY_BELLARD_QUICKJS_PATCH=OFF
cmake --build build-property-trace -j8
TILEFINCH_TRACE_JS_PROPERTY_FAULTS=32 \
  ./build-property-trace/psp-browser-interactive-lab ...
```

The numeric environment value is bounded to 1--128 faults. Each record gives
the source position, function, bytecode position/opcode, base and key types,
property preview, arguments, locals, and nearby bytecode. The diagnostic is
for reducing a failure to the first invalid value; it must not be used to
rewrite third-party source or manufacture browser capabilities.

`TILEFINCH_TRACE_FRAME_MESSAGES=2048` independently records bounded frame
`postMessage` JSON (256--65,536 characters). It is available in trace-enabled
lab builds and compiled out by `PSP_BROWSER_DISABLE_TRACE`. Frame payloads can
contain transient tokens, so keep their logs out of Git and redact them before
sharing.

## CMake presets

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev

cmake --preset release
cmake --build --preset release
ctest --preset release

cmake --preset sanitize
cmake --build --preset sanitize
./scripts/run-sanitizer-tests.sh
```

The presets create `build-preset-dev`, `build-preset-release`, and
`build-preset-sanitize`. `dev` uses `RelWithDebInfo`; `release` is an optimized
host build; `sanitize` enables
AddressSanitizer and UndefinedBehaviorSanitizer with frame pointers. It keeps
the FFmpeg media backend in the gate but disables SDL audio output, avoiding
SDL's Cocoa/LaunchServices bootstrap in dozens of parallel command-line tests.
A Release
configure does not silently select the PSP runtime policy: invoke a frontend
with `--psp-profile` when that behavior is the subject of a lab run. All
presets use the pinned Bellard QuickJS configuration, enable tests, disable the
unrelated JavaScriptCore spike, and use a compiler cache when available.

Host tools and tests load one shared `tilefinch_core` image. Consequently an
engine implementation edit rebuilds that image without relinking every test
executable; the logical build dependency still guarantees that the new image
is complete before any test starts. PSP configurations continue to link a
static core and retain the same `.text` and hot-symbol ratchets. The Release
fidelity gate reserves four CTest processor slots and renders its independent
scenarios concurrently, while keeping scoreboard rows in manifest order.

On macOS, `scripts/run-sanitizer-tests.sh` runs the suite in parallel with
symbolization disabled, then reruns only failed tests serially with normal
symbols while preserving the failed status. This keeps the green path parallel
and prevents several tests exposing the same defect from overwhelming
CoreSymbolication/LaunchServices and hiding the first useful report. Override
the default two workers on macOS (eight elsewhere) with
`TILEFINCH_SANITIZER_JOBS`.

The hostile-input parser harness is opt-in:

```sh
cmake --preset hostile
cmake --build --preset hostile
ctest --preset hostile
```

It runs under the sanitizer configuration and is not a replacement for a
general-purpose fuzzer. Its build preset requests only the hostile-parser
harness; the matching test preset runs only that test.

Host builds make `tilefinch_core` depend on
`check_tilefinch_bootstrap_generated`. Editing `src/bootstrap/*.js` therefore
fails the next core build if the checked-in source or bytecode image is stale.
Regenerate both deterministic artifacts explicitly with:

```sh
cmake --build build-preset-release --target regenerate_tilefinch_bootstrap
```

That target regenerates both embedded C files and derives
`src/bootstrap/generated.sha256` from the exact authored/generated tree.
Do not edit the manifest by hand. Host checks verify the same manifest that
PSP-only builds consume.

The PSP cross-build consumes those checked-in artifacts and does not run a
host QuickJS generator through the PSP toolchain.

### Canvas and WebGL game qualification

The public [WebGL authoring guide](WEBGL.md) defines the bounded PSP profile
that this lane qualifies and the patterns page authors should use.

The focused game-compatibility lane exercises a bounded Canvas paddle/ball
loop, indexed WebGL, point/line/triangle depth parity, uniform snapshots,
packed-wire boundary refusal, page-visibility suspension, Gamepad sampling,
drawing-buffer limits, pixel readback, object deletion, and invalid
buffer/index ranges. It also runs the complete installable
`examples/prism-break-3d/` game through deterministic physics, collision,
level-transition, multiball, power-up, particle, and dynamic-mesh scenarios.
The same lane runs `examples/treadline-arena/` through its PSP-realistic
startup, offline manifest, fixed-step tread movement, firing, pause, one-bounce
shells, destructible barrier swaps, three armor/speed classes, class-specific
secondary weapons, directional armor, the opt-in Command meter, all five
gadgets, Survival, Team Control, Convoy Escort, objective-aware bot movement,
saved setup preferences, bounded entity pools, and teardown ownership.

```sh
cmake --build build-preset-release \
  --target tilefinch-canvas-webgl-conformance-tests
ctest --test-dir build-preset-release \
  -R '^tilefinch-canvas-webgl-conformance-tests$' --output-on-failure
```

The ordinary lane stays short. A bounded 600-frame allocation soak is opt-in:

```sh
TILEFINCH_WEBGL_SOAK_FRAMES=600 \
  build-preset-release/tilefinch-canvas-webgl-conformance-tests
```

It reports the QuickJS heap before, after, and at its high-water mark together
with the number of charged allocations. The retained heap must remain within
256 KiB of its pre-soak value after collection.

The synthetic pages live under
`tests/fixtures/canvas-webgl-games/`. They are suitable for the host lab and
the isolated PPSSPP localhost harness; they contain no captured third-party
game code. PPSSPP proves lifecycle, rendering, and cleanup behavior, but its
software-renderer timing is not a substitute for the real-PSP performance
qualification.

Physical-device WebGL cadence runs use
`tests/input-scripts/webgl-game-cadence.txt` with Prism's explicit
`?qualification=ordinary`, `?qualification=heavy`, and
`?qualification=profile` URLs. The first mark resets validation-only counters
after warm-up. The profile mode also splits the example's JavaScript cost into
update, geometry construction, upload, command setup, and HUD work. The
terminal report separates author callback time from synchronous native WebGL
time and records canvas-presentation median, p95, maximum, and 60/30 Hz
deadline counts. These probes are absent from shipping builds and do not expose
a higher-resolution clock to arbitrary page JavaScript.

Treadline Arena also provides `?qualification=long-soak`. It keeps the player
alive, drives the player with the game's bounded target/avoidance planner,
aims and fires through the ordinary cooldown and projectile paths, cycles
arenas, and repeats collisions, particles, smoke, and destruction. The run
therefore proves real movement and combat instead of tracing a blind route
that can park against scenery or settling into an idle title or victory
screen. Use
`tests/input-scripts/treadline-long-soak.txt`: its 2,700-frame measured window
is roughly ninety seconds at the PSP's intended 30 Hz presentation cadence,
with marks bracketing the interval. The validation writer
persists one marked frame per run, so archive a gameplay frame at the first
mark and use the final active-game summary plus near-zero HTML-overlay time to
prove that the run did not fall back to a title panel. Use a second paired run
or a PSPLink screenshot when a visual end-frame comparison is required.
Validation retains percentile samples for the first 1,024 presented pipelines
and separately counts every over-34-ms pipeline and the global worst pipeline
for the complete run; use the latter two values to judge the long tail. Compare
the pre-interaction and controlled budget/category reports as well as the
QuickJS retained heap so a smooth run cannot hide gradual growth.

The long soak intentionally calls the game's bounded combat planner directly;
it is a simulation/render stress test, not an input-routing test. Before a
release, also run `tests/input-scripts/treadline-offline-controls.txt` from
native Home with the current Treadline package installed as Saved row one
(the device fixture keeps Prism Break in row zero).
That scenario opens **Library → Saved**, launches the sealed offline resource
pack, activates Deploy, exits and re-enters Page controls with Start+Select,
then drives nub, face, shoulder, gadget, and command inputs. Its terminal
`tilefinch-input-gamepad:` line is the receiver proof: connected, button, and
analog frame counts must all be nonzero, the connection count must cover the
automatic entry and manual re-entry, and `buttons` must include the exercised
standard Gamepad slots. Use its canvas cadence report to assess manual-input
action frames. A direct qualification URL cannot substitute for this run.

For a deterministic PPSSPP pressure bracket, set
`TILEFINCH_PPSSPP_CPU_MHZ=166` or `111` when invoking
`scripts/run-ppsspp-input-script.sh`. The harness writes PPSSPP 1.20's
canonical `[CPU] CPUSpeed` setting and defaults to `0` (normal emulation).
`TREADLINE_CPU_MHZ` remains a compatibility alias for older local runners.
Always inspect the isolated run's generated `ppsspp.ini` before trusting a
new clock experiment: the obsolete `LockedCPUSpeed` spelling is silently
ignored by current PPSSPP builds.

#### Staging games on a PSP without stale assets

An example directory is the only authored source of its game. A PSP test can
nevertheless involve two other copies: a temporary HTTP staging tree and an
installed, integrity-protected offline snapshot. Rebuilding Tilefinch,
regenerating its JavaScript bootstrap, or loading a new browser PRX does not
update either copy. In particular, changing only an `index.html` query does
not invalidate separately cached JavaScript, CSS, or images.

Run the focused host lane above first. For a direct network-backed device run,
use the content-addressed staging helper. It hashes the complete authored tree,
copies it to an immutable directory, verifies an existing directory before
reuse, and writes the exact digest-bearing document URL to the validation
`boot.cfg`. Because the digest is part of the directory, every subresource gets
a fresh URL when any authored byte changes:

```sh
scripts/stage-psp-game.sh \
  --source examples/treadline-arena \
  --stage-root /tmp/tilefinch-game-stage \
  --base-url http://HOST_LAN_IP:8770 \
  --boot-config build-preset-psp-validation/boot.cfg \
  --name treadline \
  --query 'qualification=long-soak&profile=ordinary-cadence'
python3 -m http.server 8770 --bind HOST_LAN_IP \
  --directory /tmp/tilefinch-game-stage
```

The helper prints the full tree digest and final URL. Preserve that output with
the device log: it is the revision identity for the run. It also selects the
Game Profile's bounded 384 KiB per-script envelope in the validation boot
configuration, so a direct device run and an installed offline game admit the
same authored package. Never edit a staged
directory. If its contents no longer match the marker, the helper refuses it
instead of silently testing a mixed revision. It also records a managed binding
beside `boot.cfg`; `scripts/psplink-device.sh` rehashes and restages the current
source automatically before every subsequent memory or slot load. Pointing
`boot.cfg` at another URL disables that binding without rewriting the new task.
Leave `input_script=` empty for a manual run, exit any resident Tilefinch
instance normally with HOME, and then use `scripts/psplink-device.sh memory`.
Do not force-unload a live browser module.

Use the same helper for Prism Break, Treadline Arena, and every other packaged
game. Do not stage with an ordinary `cp`, reuse a stable game directory, or
claim a revision from the EBOOT alone. An end capture showing a title, pause,
Deploy, or completion panel invalidates a gameplay soak even if the WebGL
counters look healthy; it measured the overlay, not sustained gameplay.

Prove the loaded revision instead of relying on the screen alone:

- the HTTP server must record PSP requests for `index.html`, `game.css`, and
  `game.js` from the device address;
- the requested directory digest must match the helper's `stage-digest` output;
- `tilefinch-validation.txt` must report one discovered and loaded game script,
  no JavaScript error, and an interactive-ready page;
- capture a PSPLink screenshot before input, then press Play and verify the
  Page-controls notice and Start+Select exit;
- when measuring cadence, use the committed input script and its explicit
  qualification URL rather than the manual staging configuration.

An installed copy in **Library → Offline apps** is a sealed snapshot. Source or
staging changes do not mutate it. Open the current network-backed page, choose
**Page tools → Install offline app**, confirm that the preview says Update or
Reinstall and lists the expected resources, and complete that operation before
testing the library entry. An integrity error means the stored document and
resource pack no longer agree; never repair it by replacing individual files.
Uninstall and reinstall the whole snapshot. Finally, verify offline launch with
the network unavailable—the launch itself must not associate Wi-Fi.

### Optional ARK-4 XMB redirect

The XMB redirect is a small kernel PRX with no dependency on the browser
engine. Build it explicitly with the PSP toolchain:

```sh
PSPDEV=/path/to/pspdev cmake --preset psp
cmake --build build-preset-psp --target tilefinch-xmb-redirect
```

The result is
`build-preset-psp/xmb-redirect/tilefinch_xmb.prx`. The release install-tree
target stages the same file under `TILEFINCH/OPTIONAL/`; it is never placed in
an A/B application slot or an in-app update package. Its module-start hook is
name-based (`htmlviewer_plugin_module`) rather than firmware-offset-based,
chains the previously registered ARK handler, and signals an already-waiting
worker rather than allocating inside ARK's pre-start callback. The worker
requires an XMB confirmation press, honors the L-trigger bypass, and waits a
bounded interval for Sony's module start to finish before LoadExec. Incidental
HTML-viewer loads during XMB startup, a missing launcher, controller-read
failure, worker setup failure, or LoadExec failure all leave Sony's browser
available.

### Optional PSP software decoder

H.264 High-profile playback uses a replaceable, user-built PRX so the official
EBOOT and release archives remain independent of FFmpeg decoder binaries.
Baseline/Main MP4 and MPEG-TS HLS with AAC-LC use the PSP firmware backend,
including compatible active YouTube live streams and premieres.
Prepare the narrow LGPL n8.1.2 build once, then point an opt-in PSP configure
at that ignored workspace:

```sh
PSPDEV=/path/to/pspdev \
  ./scripts/prepare-swdec-ffmpeg.sh build-swdec
PSPDEV=/path/to/pspdev cmake --preset psp \
  -DTILEFINCH_PSP_ENABLE_SWDEC_COMPONENT=ON \
  -DTILEFINCH_SWDEC_SOURCE_DIR="$PWD/build-swdec"
cmake --build build-preset-psp --target tilefinch-swdec-bundle
```

The preparation script checks out the exact upstream tag, applies the
committed patch, enables only H.264, `aac_fixed`, and the H.264 parser, and
can optionally consume device-trained profiles by passing their directory as
the second argument. Without them the component is compatible but may be
materially slower near the PSP's real-time limit.

The target writes a three-file add-on directory at
`build-preset-psp/tilefinch-swdec-addon/`: `tilefinch-swdec.prx`, the resident
`swdec-meload.prx` helper, and `component-info.txt`. Copy all three into
`PSP/GAME/TILEFINCH/components/swdec/`; do not put them in `slot-a` or
`slot-b`. The ordinary browser EBOOT always contains the bounded loader, so
it does not need to be replaced with a custom EBOOT. Signed app updates leave
the shared component directory untouched.

`component-info.txt` records the loader ABI. Every browser update manifest
also carries the ABI it expects. The update UI warns **Decoder rebuild
needed** when an installed add-on differs; rebuild the bundle with the new
source and replace all three files together. No rebuild is needed merely
because the Tilefinch version changes.
The player reserves the component's resident memory before loading it and
keeps all stream buffering in RAM.

The live-network PSP build also consumes project-owned, hash-pinned curl,
Mbed TLS, and optional nghttp2 archives. Populate the ignored offline cache
once before configuring a fresh PSP tree:

```sh
./scripts/fetch-psp-transport-deps.sh
cmake --preset psp
cmake --build --preset psp
```

`psp-http1` is the current-stack HTTP/1.1 comparison build;
`psp-legacy-transport` is an explicit non-release SDK-stack escape hatch.
See `docs/engineering/PSP_TRANSPORT.md` for provenance and validation.

### PSP compiler-hardening candidate

The real GCC 15.2.0/newlib PSP toolchain supports both
`-fstack-protector-strong` and `_FORTIFY_SOURCE=2`. The candidate preset keeps
those flags separate from the shipping preset until their device cost is
qualified:

```sh
cmake --preset psp-hardened
cmake --build build-preset-psp-hardening \
  --target tilefinch_core psp-browser-fixture psp-browser-script
```

The cross-build measurement used the same source revision and MinSizeRel
configuration for every row:

| PSP C hardening | `.text` | Delta from baseline | `.rodata` |
| --- | ---: | ---: | ---: |
| none | 3,648,612 B | — | 1,840,932 B |
| fortify level 2 | 3,649,688 B | +1,076 B (+0.03%) | 1,841,204 B |
| stack protector strong | 3,729,684 B | +81,072 B (+2.22%) | 1,841,188 B |
| combined candidate | 3,730,744 B | +82,132 B (+2.25%) | 1,841,252 B |

The combined image has roughly 2,012 protected return sites and remains below
the ordinary 4,480,000-byte `.text` ratchet. The five hot-function size
ratchets also pass. Newlib initializes one process-wide fixed stack guard
rather than a random per-process guard, so this is useful corruption detection
and exploit friction, not a desktop-grade randomized canary.

Do not turn the candidate on in the `psp` preset from build evidence alone.
Promotion requires a real PSP boot/navigation/media smoke, a before/after
input and frame-cadence comparison, and the normal named-target cross-build.
This avoids charging every protected call on the 333 MHz CPU without measuring
the effect. The `psp-hardened` preset exists so that device comparison is
reproducible rather than dependent on hand-edited C flags.

## Focused suites

The aggregate `tilefinch-tests` executable is available for an explicit
whole-program qualification run. Its test program has three
registered filters, and the layout suite lives in its own executable next to
the layout translation units it exercises:

```sh
./build-preset-dev/tilefinch-tests --list
./build-preset-dev/tilefinch-tests --filter foundation
./build-preset-dev/tilefinch-tests --filter web-runtime
./build-preset-dev/tilefinch-tests --filter sections
./build-preset-dev/tilefinch-layout-tests
```

`core` is an alias for foundation and web-runtime (`scripts/dev.sh unit core`
also runs the layout executable). CTest exposes the same suites as
`tilefinch-foundation-tests`, `tilefinch-web-runtime-tests`,
`tilefinch-layout-tests`, and `tilefinch-section-tests`, plus focused executables for
URL/security, session security, fetch streaming, the engine facade, JavaScript
responsiveness, pumpable navigation, and supported loopback redirect behavior.
For example:

```sh
ctest --test-dir build-preset-dev -R 'tilefinch-(url|session-security)-tests' \
  --output-on-failure
ctest --test-dir build-preset-dev -R tilefinch-navigation-load-tests \
  --output-on-failure
ctest --test-dir build-preset-dev -L security --output-on-failure
```

Use the narrowest relevant filter while editing, then run all registered
Release and sanitizer gates plus the aggregate executable before treating a
change as ready. Network/replay
acceptance scripts and the long platform simulator are separate qualification
tools; they should not be hidden in every local build or unit-test invocation.

The one-hour media cadence check advances a virtual clock in a tight loop and
usually finishes in under 10 ms, but it is still an occasional policy check.
It is neither registered with CTest nor included in a default build target:

```sh
cmake --build build-preset-release \
  --target tilefinch-occasional-media-timing
```

Media delivery also has an explicit real-time resilience gate. It generates a
temporary, video-only fragmented MP4 and serves it through deterministic local
fault profiles; no third-party media or network service is required. The
profiles cover healthy burst delivery, per-request setup delay, truncated
responses, slow trickles, asymmetric audio/video supply, a persistent
30-second path outage followed by recovery, buffering hysteresis, and the
PSP's publication quantum:

```sh
./scripts/run-media-resilience-gate.sh build-preset-release
```

This is intentionally outside CTest because it preserves wall-clock delivery
cadence and the longest profile takes tens of seconds. It complements rather
than replaces a hardware playback soak: host curl cannot reproduce PSP WLAN,
firmware decoder, DMA, or thread-priority timing.

## Source and target boundaries

`include/tilefinch/` contains the exposed Tilefinch contracts. Most subsystem
headers have a same-named implementation in `src/`; `remote_selector.h` and
`budget_quickjs.h` are intentionally header-only shared types/adapters. New
frontend code should prefer `browser_engine.h`; direct subsystem headers remain
available primarily for tests and the compatibility seams described in
`ARCHITECTURE.md`.

`tilefinch_core` contains engine/subsystem implementations only. The entry points
`main.c`, `interactive_main.c`, and `failure_recovery_main.c` are separate
frontend or diagnostic targets. They and the host-portable `psp_ui.c` support
module are included in the generated portability-source manifest. Keep
`TILEFINCH_CORE_SOURCES` in `cmake/TilefinchCore.cmake` as the canonical core
inventory so normal linking and portability auditing do not silently diverge.
The root `CMakeLists.txt` establishes project-wide policy and includes focused
modules for dependencies, core sources, product targets, and tests. PSP
SDK-only entry points are checked by the cross-build rather than the host
compile audit.

Large, tightly coupled owners may be divided into private implementation seams
under `src/<owner>/`. These `.inc` files are included exactly once by the
same-named `.c` owner, in dependency order; they are not standalone translation
units and must not be added to `TILEFINCH_CORE_SOURCES`. This keeps private state
and PSP code generation unchanged while making policy, transport, lifecycle,
and transaction responsibilities reviewable in isolation. Aggregate test
executables use the same pattern under `tests/suites/` so allocator and
process-lifetime semantics remain unchanged.

The current high-level maps are visible directly in their owner files:
`src/js_runtime.c` orders bridge state, host primitives, evaluation, dynamic
scripts, document state, event-loop work, result snapshots, runtime creation,
document evaluation, runtime advancement, dispatch, and teardown;
`src/style_properties.c` orders visual basics, typography, box model, layout,
positioning/visual effects, and property dispatch. The redirect, navigation,
and dynamic-script test owners similarly include scenario-family units from
`tests/suites/`. Start in the owner file and open only the relevant unit.

## Dependency integrity

Configuration requires CMake, a C11 compiler, `patch`, and Git. Git applies
the pinned Bellard QuickJS OOM lifetime patch with zero-context semantics;
complete preimage and postimage hashes are checked before and after that
operation, so an unknown or partially modified source is never patched.

The first configure downloads pinned Lexbor, QuickJS, stb, NanoSVG, and DejaVu
archives and verifies their hashes. `PSP_BROWSER_VENDOR_DIR` can supply the
prepared dependencies supported by its documented layout; individual selected
FetchContent sources can also be supplied with
`FETCHCONTENT_SOURCE_DIR_<NAME>`. Treat a checksum mismatch as a dependency or
configuration failure; do not bypass it to make a build proceed.

The development helper follows explicit FetchContent source overrides already
recorded in a reusable build tree instead of assuming every dependency lives in
that tree's `_deps` directory. For Bellard QuickJS it accepts only the exact
pinned source or the exact postimage of the mandatory OOM lifetime fix. An
experimental VM-patched, partial, or otherwise altered source is not shared
with the portable Debug, Release, or sanitizer configurations.

## Configuring and building

This longer build reference includes dependency acquisition and vendored-source
overrides in addition to the preset and fast-edit-loop sections above.

The first configure downloads hash-pinned Lexbor, the selected QuickJS engine,
stb, NanoSVG, and DejaVu sources. Raw CMake defaults to QuickJS-NG; the checked-in
presets and `scripts/dev.sh` select pinned Bellard QuickJS. Existing prepared
sources can be supplied through `PSP_BROWSER_VENDOR_DIR` (for its supported
dependency layout) or CMake's `FETCHCONTENT_SOURCE_DIR_<NAME>` overrides. The
build also requires libcurl development headers and a TLS-capable libcurl.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Equivalent checked-in presets provide isolated development, Release,
ASan/UBSan, and opt-in hostile-input trees:

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

See the preset and focused-suite sections above for all presets, the four focused
unit-suite filters, security/network/architecture tests, and the boundary
between short iteration and explicit qualification runs.

For the normal Bellard QuickJS development loop, use the targeted helper
instead. It selects the pinned upstream engine used by the presets, detects the
host's parallel job count, disables the unrelated JavaScriptCore spike, and
builds only the binary needed for the current edit. The experimental VM patch
remains available with `-DPSP_BROWSER_APPLY_BELLARD_QUICKJS_PATCH=ON`, but is
not a correctness-qualified production default. The default tree uses `-O0`
for short compile latency; Release is kept in a second tree for live-site,
performance, and final validation:

```sh
./scripts/dev.sh                 # interactive lab only
./scripts/dev.sh run --fixture fixtures/interactive.html --ticks 2
./scripts/dev.sh static          # static renderer only
./scripts/dev.sh unit foundation # fastest focused aggregate unit suite
./scripts/dev.sh test            # build registered binaries and run CTest
./scripts/dev.sh release         # optimized interactive lab
./scripts/dev.sh test-release    # optimized local test gate
./scripts/dev.sh verify          # optimized tests plus three network/replay gates
./scripts/dev.sh sanitize        # separate ASan/UBSan tree and tests
```

The portable Bellard default includes a small, allocation-free fast path for a
general closure getter that only returns one captured value. It can be disabled
in a fresh control tree with
`-DPSP_BROWSER_QUICKJS_CAPTURE_GETTER_FASTPATH=OFF`; it does not inspect source
text, URLs, or call sites.

The first command creates `build-dev`; optimized commands use `build-release`.
The release and sanitizer configurations reuse dependency source trees from
`build-dev` instead of downloading and extracting them again. Override the
trees or job count with `PSP_BROWSER_BUILD_DIR`,
`PSP_BROWSER_RELEASE_BUILD_DIR`, `PSP_BROWSER_SANITIZE_BUILD_DIR`, and
`PSP_BROWSER_JOBS`. Set `PSP_BROWSER_DEP_SOURCE_BUILD` when dependency sources
live in another build tree. CMake automatically uses `ccache` or `sccache` when
one is installed. Full optimized tests and sanitizer checks remain explicit
gates instead of running during every edit/build cycle.

The helper deliberately keeps its Bellard feature configuration aligned across
Debug, Release, and sanitizer trees. This ensures `test-release`, `verify`, and
the consolidated pre-cross-build readiness workflow qualify the same portable
VM behavior intended for the eventual PSP build. Use a separate raw CMake tree
when an upstream, unpatched Bellard comparison is required.

To use existing dependency checkouts:

```sh
cmake -S . -B build \
  -DPSP_BROWSER_VENDOR_DIR=/path/to/vendor \
  -DCMAKE_BUILD_TYPE=Release
```

Running the built frontends is documented separately in
[LAB_USAGE.md](engineering/LAB_USAGE.md).
