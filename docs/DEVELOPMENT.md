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
host build and disables optional GIF support; `sanitize` enables
AddressSanitizer and UndefinedBehaviorSanitizer with frame pointers. It keeps
the FFmpeg media backend in the gate but disables SDL audio output, avoiding
SDL's Cocoa/LaunchServices bootstrap in dozens of parallel command-line tests.
A Release
configure does not silently select the PSP runtime policy: invoke a frontend
with `--psp-profile` when that behavior is the subject of a lab run. All
presets use the pinned Bellard QuickJS configuration, enable tests, disable the
unrelated JavaScriptCore spike, and use a compiler cache when available.

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

### Optional PSP software decoder

H.264 High-profile playback is a replaceable, user-built PRX so the official
EBOOT and release archives remain independent of FFmpeg decoder binaries.
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
the ordinary 4,435,000-byte `.text` ratchet. The five hot-function size
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
