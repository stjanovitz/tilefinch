# Targeted host build-speed experiment — 2026-09-05

Base: `2ad0b27c`, in an isolated worktree with separately copied dependency
sources. The active main checkout, build directory, and its unfinished changes
were not used as mutation targets. AppleClang 21, optimized Release, Unix
Makefiles, compiler cache disabled, eight build jobs. No PSP code or bootstrap
payload changed.

## Complete command, not just the compiler

The measured command is:

```sh
cmake --build build-preset-release \
  --target tilefinch-browser-engine-tests -j8
```

Three warm samples per case; values below are median wall time. The edit cases
touch source mtimes without changing contents and require the full build log to
show the expected compile. Waiting for Make's timestamp resolution is outside
the timed interval. This is an incremental-build experiment, not a clean-build
or test-execution speed claim.

| Case | Before | Split journey tests + content-verified bootstrap memo | Reduction |
|---|---:|---:|---:|
| No-op build | 0.554 s | 0.384 s | 31% |
| Journey-test edit | 1.315 s | 0.547 s | 58% |
| `browser_engine.c` edit | 1.158 s | 1.005 s | 13% |

Splitting the journey test alone measured 0.805 s for its edit/build cycle.
The main executable and command-line selection remain the same; only the
compilation unit changes. The other browser-engine suites remain together.

A final three-sample confirmation after scratch patch validation measured
0.314 s no-op, 0.492 s journey edit, and 0.908 s engine edit. A further no-change
configure/build took 2.740 + 0.682 s and passed the byte/mtime stability check.
Host load was not pinned; use the roughly 0.49–0.55 s journey result rather
than treating one millisecond value as a portable guarantee.

The preliminary isolated component medians were 639 ms for the monolithic test
compilation, 415 ms for `browser_engine.c`, 38 ms for executable linking, 225 ms
for bootstrap regeneration/comparison, and 29 ms for manifest verification.
Recursive Make dry-run traversal was another 275 ms, excluding executed CMake
dependency scans. These component timings cannot be summed as an exact build
prediction because dependencies and parallel scheduling differ.

## Why some invocations take much longer

A no-change CMake configure reversed and reapplied the QuickJS patch stack to
the actual compiled source tree. Both final files were byte-identical, but
their mtimes changed. That rebuilt QuickJS, the bootstrap generator, and many
engine consumers of `quickjs.h`.

| One no-change configure + subsequent build | Before | Scratch patch validation |
|---|---:|---:|
| Configure | 3.206 s | 3.322 s |
| Subsequent targeted build | 9.465 s | 0.812 s |
| Combined | 12.671 s | 4.135 s |

The experiment now performs the same exact fingerprint checks and patch
operations on scratch copies, then publishes only different validated bytes.
Both `quickjs.c` and `quickjs.h` preserved their SHA-256 and nanosecond mtimes
across the improved run. The build portion fell 91%; combined time fell 67%.
This explains one reproducible long-build path, not every delay observed in
the main task.

## Verification contracts

- All source/artifact hashes are still checked on every build.
- Bootstrap comparison reuse includes the generator executable and both
  verification scripts, not just source mtimes.
- A changed source with preserved mtime fails; a manually updated manifest
  cannot hide incorrect bytecode from the full comparison.
- Failed comparisons do not publish a success memo. Concurrent checks use
  a build-local lock and publish one successful result.
- The existing full bootstrap CTest always regenerates and compares.
- Test splitting preserves the aggregate executable, CLI, and CTest coverage.
- The `--reconfigure` benchmark asserts unchanged QuickJS contents and mtimes.

The complete optimized host build passed. CTest passed 148 tests with no
failures; the update-root proof and absent fidelity corpus skipped, and the
device-cost lane remained disabled (151 registered, 150 enabled). Localhost
tests required the ordinary socket-capable host run rather than the sandbox.
No physical PSP run or PSP cross-build was performed for this build-only
experiment; copied dependency source hashes were checked instead.

## Remaining opportunities

1. Configuration itself still costs about three seconds; patch validation
   deliberately still runs. Do not skip its accepted-state checks merely to
   make configuration faster.
2. The unconfigured update-root header is rewritten during configure and
   causes a small extra compile/link. Content-stable generation is a separate
   low-risk follow-up; it was not changed here.
3. Split additional test suites when edit frequency justifies it. The 58%
   result applies to journey edits, not every test or engine source.
4. Compare Ninja in another tree when available. No runnable Ninja installation
   was found here, and no tool was installed or preset changed for this
   experiment. Do not claim a measured Ninja improvement.
5. Compiler caching helps repeated identical compilations and branch switching,
   but should not be credited with making a genuinely new source edit free.

Reproduce with `benchmarks/measure-incremental-build.py` in an exclusively owned
worktree. It retains raw build logs and JSON samples, rather than relying on a
truncated final build line.
