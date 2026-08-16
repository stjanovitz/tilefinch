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

Release currently registers **126 tests: 125 enabled plus the opt-in
`tilefinch-device-cost-tests`, which is registered but disabled by default**.
All enabled tests must pass. The localhost
redirect test may report `Skipped` in a sandbox that forbids loopback sockets;
the update-root proof can likewise skip when its external prerequisite is not
available. A skip is not a passing substitute for running either gate in an
ordinary host environment.

For a faster edit loop, configure rather than trusting a previously-created
development tree:

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

The development preset currently registers **123 tests**; Release adds
`tilefinch-fidelity-floor-tests`. The legacy `./scripts/dev.sh unit` command
remains useful for aggregate unit filters. Test counts legitimately differ
between configurations, so compare a tree against itself, not against another
tree.

Sanitizers (`cmake --preset sanitize`) and the hostile-input parser harness
(`cmake --preset hostile`) are explicit gates, not part of the edit loop.
Details are in [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md).

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

- **A 4,435,000-byte ordinary `.text` ratchet** (4,460,000 bytes when
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

`src/bootstrap/*.js` is compiled into two committed C files. If you edit any
of those `.js` sources you must regenerate **both** artifacts and the manifest:

```sh
cmake --build build-preset-release --target regenerate_tilefinch_bootstrap
```

That target rewrites `src/generated/js_bootstrap.c` and `src/generated/js_bootstrap_bytecode.c`
and derives `src/bootstrap/generated.sha256` from the exact authored and
generated tree. Never edit `generated.sha256` by hand.

Both host and PSP builds make `tilefinch_core` depend on
`check_tilefinch_bootstrap_generated`, so a stale artifact fails the *next* core
build either way. The difference is what each can check: a host build re-runs
the bytecode generator in `--check` mode *and* verifies the manifest, while the
PSP build cannot run a host QuickJS generator through the cross toolchain and
therefore verifies the manifest only. A hand-edited manifest would pass the
PSP gate and is exactly what the host gate exists to catch.

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
improvement that earned it, with the reasoning recorded in
`docs/FIDELITY.md`. Lowering a floor to make a red gate green is not an option
even when the number looks small.

All committed fidelity rows are expected to pass. Exact floors live only in
`tests/fidelity-baselines.tsv`; do not duplicate or lower them in prose.

## PSP engineering constraints

The engine runs inside a 16 MiB (`strict`) or 24 MiB (`realistic`) shared
ceiling on a 32-bit in-order MIPS core with no virtual memory. In engine code:

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

## Verification discipline

These rules keep verification evidence trustworthy:

- **`cmake --build ... | tail -1` hides whether anything recompiled.** A
  truncated build log looks identical whether the compiler ran or the tree was
  already up to date, so it can "prove" a fix that was never built. Read enough
  of the output to see the compile and link steps.
- **To compare against another commit, check out sources and rebuild fully.**
  Use `git checkout <sha> -- src/ include/` followed by a complete rebuild.
  Do **not** use `git stash`: it moves unrelated files, is easy to leave
  half-applied, and has silently poisoned comparisons here before.
- **Confirm a new regression test actually fails against the pre-fix code.** A
  test that passes both before and after proves nothing. Build the old sources,
  watch the test fail, then restore the fix and watch it pass.
- Beware shared dependency source trees: a patch applied in one build tree can
  reach binaries built from another. If a result is surprising, reconfigure a
  clean tree before believing it.
- **Check what the firmware returned, not that the call was made.** PSP
  syscalls report failure in their result. A counter that records an attempt
  rather than a successful outcome can certify a broken build.
- **The emulator is more permissive than the hardware.** PPSSPP accepts VRAM
  aliases and argument combinations a PSP-3000 does not, so a green emulator
  run is necessary and not sufficient. When device behaviour disagrees with a
  green validation run, read the emulator's own HLE log before theorising about
  engine internals — `scripts/run-ppsspp-network.sh` honours a `PPSSPP=` env
  var, so pointing it at a wrapper that adds `-d` gets a full syscall trace in
  `build-preset-psp/ppsspp-network-latest/ppsspp.log`.

## Commits

Commit at each passed gate rather than batching a session's work into one
commit. A gate is a green `ctest --test-dir build-preset-release`, a clean PSP
cross-build, or a passing fidelity floor run. Scratch directories and
uncommitted edits are not durable; a passed gate that is not committed can be
lost.

Use an imperative subject line and a body that explains why the change was
made, not just what changed.
