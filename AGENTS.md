# Conventions for agents working in Tilefinch

This file is for AI coding agents (and is equally useful to new human
contributors). It records the conventions that this repository actually
enforces — most of them through CMake, CTest, or a committed ratchet file — so
that a change either follows them or fails a gate.

Start with [README.md](README.md) for what the project is, and
[docs/README.md](docs/README.md) for the documentation map.

## Build and test

The canonical gate is the optimized host build and its test suite:

```sh
cmake --build build-preset-release -j8
ctest --test-dir build-preset-release -j8 --output-on-failure
```

All enabled tests must pass. Registration varies with optional host tooling,
and only these may legitimately not run:

- `tilefinch-device-cost-tests` is registered but disabled by default;
- `tilefinch-swdec-crc-tests` is disabled unless configured with
  `-DTILEFINCH_SWDEC_CRC_GATE=ON` and a prepared
  `TILEFINCH_SWDEC_CRC_SOURCE_DIR`;
- the localhost redirect test reports `Skipped` in a sandbox that forbids
  loopback sockets;
- the update-root proof skips when its external prerequisite is absent;
- the `tilefinch-treadline-*` Node tests report `Skipped` when `node` was not
  found at configure time;
- the `tilefinch-reference-*-browser-tests` report `Skipped` when Node or
  its `playwright` package is missing, and
  `tilefinch-local-update-server-tests` when `openssl` is;
- `tilefinch-fidelity-floor-tests` skips when the ignored `fidelity/` corpus
  is absent.

A skip is not a pass: run the gate in an ordinary host environment before
claiming it.

Private investigation tests and fuzz reproducers live under the ignored
`.private-investigations/tests/`, never as inline public test inputs; see
[Optional private tests](docs/DEVELOPMENT.md#optional-private-tests).
Maintainers with local suites enable them with
`-DTILEFINCH_ENABLE_PRIVATE_TESTS=ON` before the full gate.

The edit loop is `./scripts/dev.sh` (`dev.sh unit <filter>` for aggregate
unit filters, `dev.sh test` for the registered suite in its `build-dev`
tree); it reconfigures its tree when CMake inputs change. The `dev` CMake
preset (`build-preset-dev`) is not the edit loop: an existing tree goes stale
and its CTest can run stale binaries, so use it only after a fresh
`cmake --preset dev`. Compare test counts only within one tree; they
legitimately differ between configurations.

Sanitizers (`sanitize`), the hostile-input parser harness (`hostile`),
link-time optimization (`release-lto`), and hermetic PSP replay
(`psp-replay`) are explicit presets, not part of the edit loop.
[Presets and build trees](docs/DEVELOPMENT.md#presets-and-build-trees) lists
every preset and tree.

## The PSP cross-build is a separate gate

It is never part of the test suite and never runs implicitly:

```sh
PSPDEV=/path/to/pspdev cmake --preset psp
cmake --build build-preset-psp \
  --target tilefinch_core psp-browser-fixture psp-browser-script
```

Build named PSP targets only. The host lab and input-latency executables are
not device targets, and the bare cross-build `all` target is not the PSP gate.

Two things only this build enforces:

- **A 5,500,000-byte ordinary `.text` ratchet** (5,900,000 bytes when
  validation logging is compiled in). `cmake/CheckPspTextSize.cmake` reads the
  actual ELF `.text` sections with `psp-objdump` after every link, reports
  `.rodata` separately, and fails the build above the appropriate limit.
  The aggregate `psp-size` `text` column is not used because it includes
  read-only data and does not isolate executable code.
  Raising either ratchet requires a re-measurement and a device-cost
  justification, not a bigger number.
- **The real Allegrex toolchain.** Host-only assumptions (64-bit pointers,
  `%u` with `uint32_t`, glibc-isms) surface here and nowhere else.

Run it after any change that adds code paths, dependencies, or data tables,
and record the result. `./scripts/dev.sh psp` wraps the same two commands.

**On macOS, never invoke `PPSSPPSDL` directly from a non-GUI automation shell.**
That process context can abort in Cocoa's `_RegisterApplication` before any
PSP code runs. Use `scripts/launch-ppsspp-safe.sh` for manual runs or
`scripts/run-ppsspp-network.sh` for the automated smoke; both launch the app
through LaunchServices and repair only a disposable copy when Homebrew's app
bundle seal is invalid.

**Device-facing code is shared, not copied.** The browser EBOOT
(`src/psp_script_main.c`) and the qualification fixture (`src/psp_main.c`) must go
through the same modules for anything the hardware sees — scanout lives in
`src/psp_display.c` and the chrome in `src/psp_ui.c`, each behind a header in
`include/tilefinch/`. A seam that lets the host substitute a fake (see
`PspDisplayBackend`) makes device logic testable in CTest; prefer it to a
second implementation.

The two PSP executables also write their EBOOTs to different directories on
purpose: the browser to `build-preset-psp/`, which is what every script and doc
means, and the fixture to `build-preset-psp/fixture/`. Never merge these output
paths: validation must not mistake the fixture for the browser.

## Regenerating the JavaScript bootstrap

After editing `src/bootstrap/*.js`, run
`cmake --build build-preset-release --target regenerate_tilefinch_bootstrap`
and commit both generated C files and `src/bootstrap/generated.sha256` with
the change; never edit the manifest by hand. The rule and its checks are in
[src/bootstrap/README.md](src/bootstrap/README.md#regenerating).

## The fidelity scoreboard and its floors

Visual fidelity is scored against Chrome device-emulation references at
480x272. The committed floors are `tests/fidelity-baselines.tsv`; the workflow
and the corpus rules are in [docs/FIDELITY.md](docs/FIDELITY.md).

Run the scoreboard directly:

```sh
python3 benchmarks/run-fidelity-scoreboard.py \
  --manifest benchmarks/fidelity-scenarios.tsv \
  --trace-root fidelity/captures --reference-root fidelity/references \
  --work-dir fidelity/scoreboard --output fidelity/scoreboard.tsv
```

The gate is `tilefinch-fidelity-floor-tests`, registered only in Release
configurations (the oracle is defined on the optimized lab). It re-renders the
corpus and fails when SSIM, MS-SSIM, or edge F1 falls more than 0.02 below the
baseline. It skips cleanly when the git-ignored `fidelity/` corpus is absent.

**The hard rule: a regression is never re-baselined away.** A floor moves only
when the engine is genuinely more faithful, in the same commit as the
improvement that earned it, with the reasoning in that commit's
message and the CHANGELOG. Lowering a floor to make a red gate green is not an option
even when the number looks small.

All committed fidelity rows are expected to pass. Exact floors live only in
`tests/fidelity-baselines.tsv`; do not duplicate or lower them in prose.

## PSP engineering constraints

The engine runs inside a 16 MiB (`strict`) or 32 MiB (`realistic`, the PSP
app's own, shared with voice) ceiling on a 32-bit in-order MIPS core with no
virtual memory. In engine code:

- **Allocate through the `Budget` allocator** (`include/tilefinch/budget.h`).
  Every byte the page owns — DOM, JavaScript heap, styles, resources, layout,
  tiles, session state — is charged to one ceiling and must be returned at
  teardown. Tests assert zero owned bytes after teardown; a raw `malloc` in
  engine code both escapes the ceiling and breaks that assertion.
- **No unbounded allocation.** Every buffer, table, cache, and quota has a
  fixed maximum. Growth is admitted by the budget, and a refusal is a normal,
  recoverable outcome that must roll back cleanly rather than abort the page.
- **Every loop is bounded.** Parser, layout, script, network, and event loops
  carry explicit iteration or byte ceilings so hostile or merely enormous input
  cannot take over the device.
- **Compare integer size math by subtraction.** Write
  `if (charge > limit - current)`, never `if (current + charge > limit)`: on a
  32-bit target the addition wraps and the check silently passes. `src/budget.c`
  is the reference for the idiom.
- Prefer the `BrowserEngine` facade (`include/tilefinch/browser_engine.h`) for new
  frontend work; see [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for ownership
  and lifecycle boundaries.

## The PSP source-text contract test is frozen

`tests/test_psp_sdk_contracts.py` pins C source text (needles, orderings,
slices of PSP-only files) rather than behaviour, so it breaks on harmless
renames and passes when behaviour is wrong but spelled the same. Do not add
checks to it. Put a new PSP invariant in a host-compiled behavioural test,
behind a seam such as `PspDisplayBackend` if needed. When a refactor breaks
one of its checks, update only that check, minimally, or replace it with a
behavioural test; deleting a check that a named behavioural test covers is
welcome.

## Verification discipline

These rules keep verification evidence trustworthy:

- **Read the private ledgers before an experiment.** Maintainers keep measured
  results and memory experiments in the ignored
  `.private-investigations/MEMORY_EXPERIMENTS.md`; public clones do not
  include it, so ask a maintainer for prior outcomes. Do not retry a rejected
  approach unless its stated premise changed, and record the new evidence and
  outcome, including rejections. Public docs keep no measured figures.
- **Judge performance on the device, with one binary.** Follow the
  [evidence gates](docs/DEVELOPMENT.md#evidence-gates): a runtime switch over
  alternating runs, never two builds.
- **`cmake --build ... | tail -1` hides whether anything recompiled.** A
  truncated log looks the same whether the compiler ran or the tree was up to
  date. Read enough output to see the compile and link steps.
- **To compare against another commit, check out sources and rebuild fully.**
  Use `git checkout <sha> -- src/ include/` and a complete rebuild. Do **not**
  use `git stash`: it moves unrelated files, is easy to leave half-applied,
  and has silently poisoned comparisons here before.
- **Confirm a new regression test fails against the pre-fix code.** Build the
  old sources, watch the test fail, then restore the fix and watch it pass.
- **Distrust a surprising result from a shared tree.** A patch applied in one
  build tree can reach binaries built from another; reconfigure a clean tree
  before believing it.
- **Check what the firmware returned, not that the call was made.** PSP
  syscalls report failure in their result; a counter that records attempts
  can certify a broken build.
- **The emulator is more permissive than the hardware.** PPSSPP accepts VRAM
  aliases and argument combinations a PSP-3000 does not, so a green emulator
  run is necessary, not sufficient. When the device disagrees with a green
  run, read the emulator's HLE log before theorising about engine internals:
  `scripts/run-ppsspp-network.sh` honours a `PPSSPP=` variable, so a wrapper
  that adds `-d` gets a full syscall trace in
  `build-preset-psp/ppsspp-network-latest/ppsspp.log`.

## Commits

Commit at each passed gate rather than batching a session's work into one
commit. A gate is a green `ctest --test-dir build-preset-release`, a clean PSP
cross-build, or a passing fidelity floor run. Scratch directories and
uncommitted edits are not durable; a passed gate that is not committed can be
lost.

Use an imperative subject line and a body that explains why the change was
made, not just what changed.
