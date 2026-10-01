# PSP resource envelope

Tilefinch treats memory, uninterrupted CPU time, executable size, and storage
traffic as separate budgets. A page must fit all four; spare capacity in one
does not excuse exceeding another.

## Memory profiles

`browser_config_apply_psp_memory_profile()` installs the canonical profiles:

| Policy | Strict | Realistic |
|---|---:|---:|
| page content budget | 16 MiB | 24 MiB |
| retained navigation entries | 8 | 16 |
| session response cache | 512 KiB | 1 MiB |
| QuickJS heap | 4 MiB | 5 MiB |
| total admitted script source | at most 1 MiB | at most 2 MiB |
| decoded tile capacity | up to 24 (8 reserved) | up to 24 (8 reserved) |

Tiles are 128x128 RGB565 (32 KiB). A 480x272 screen touches twelve to
sixteen; the first eight are reserved when a page's render shell is built.
The remaining slots hold the visible remainder and two rows of
paint-ahead below (or above) the screen: they are allocated on first use
only while the page keeps 2 MiB of budget headroom, and optional-memory
reclaim frees them first. On the PlayStation Portable article on a PSP-3000
this raised the page's budget peak by about 200 KiB and cut Page Down to a
complete frame from p50 117 ms to 83 ms.

Both reserve at least 8 MiB outside the page budget for process control,
network/TLS internals, firmware modules, media, native chrome, stacks, and
library bookkeeping. The PSP's measured free heap is never treated as
additional page authority.

The realistic profile is the shipping default. Strict mode is an engineering
pressure profile: behavior which remains useful there is preferred, but it may
reject pages the ordinary profile admits.

## What the page budget counts

Every owned page allocation carries one category:

```text
dom, javascript, style, resource, layout, render,
session, navigation, uncategorized
```

Lexbor and QuickJS use budget adapters. Display lists, node boxes, resources,
decoded images, glyphs, tiles, response bodies, histories, and cache records
use the same ledger. Fixed concurrent pools and inline tables enter through
reservations so their capacity reduces page admission without allocating a
duplicate payload.

At stable-page and teardown checkpoints:

- category current bytes must sum to the global current total;
- active allocation counts must reconcile;
- uncategorized ownership should be zero;
- complete teardown must return every page category to zero.

Process-lifetime state is separately bounded and exposed in engine metrics.
The CA bundle, TLS library internals, PSP network stack, firmware modules,
thread stacks, and the engine control block are not misreported as page-owned
bytes.

## Pressure behavior

Pressure responses are generic and ordered. Depending on the request and
profile, the engine may:

- decline speculative work;
- evict a response, stylesheet-fragment, glyph, image, or tile cache entry;
- reduce retained navigation or inactive-tab state;
- stop admitting more page scripts or resources;
- keep the incumbent page when a candidate cannot complete;
- reject an oversized page before layout;
- retire the page realm after a fatal script-heap exhaustion while preserving
  static content when safe.

No pressure path selects by hostname, selector, or page content. Every cache
has a fixed capacity and reports hit, miss, eviction, and admission behavior.

## Time budgets and responsiveness

The browser loop gives resumable subsystems explicit work quotas. Layout
cooperates during tree construction and between finalization phases. Resource
discovery, image decode, scripting, screenshots, offline saves, and updater
installation likewise return continuations rather than owning an unbounded
frame.

`BrowserConfig` supplies a small idle-work time and unit budget. Device-facing
frontends may grant larger bounded slices to foreground navigation or
suspend-time teardown, but input and presentation remain between slices.

Native and JavaScript callback boundaries must either poll the watchdog or
prove a small fixed upper bound. A dependency call which cannot be interrupted
is named in diagnostics rather than being counted as cooperative work.

### Boot timeline

PPSSPP ordinary-launcher validation measured browser-main to HOME at
362.6 ms and interactive-ready at 396.3 ms once the synthetic clock probe
(about 319.6 ms) was removed and splash presentation waits were shortened.
This does not establish physical PSP release boot time:
validation logging and asset diagnostics remain, and emulator storage is not
a Memory Stick. The validation-only `tilefinch-boot-input` line separately
records the first actual controller sample and its delay from interactive
loop entry (30.2 ms without an input script). Input-script
file loading adds harness overhead and must not be treated as shipping work.

## 32-bit hot-path discipline

The PSP has expensive software helpers for 64-bit division and modulo. Normal
viewport, color, glyph, and timestamp cases use exact 32-bit or incremental
paths where ranges permit, retaining checked 64-bit fallbacks for hostile or
large inputs. Target-object audits watch calls to helpers such as `__divdi3`
and `__moddi3` in hot functions.

Large transient stack objects receive the same scrutiny. The PSP build records
stack usage for critical translation units, and temporary tables move to
bounded owner storage only when that shortens a genuinely live frame rather
than hiding the same memory elsewhere.

## Executable and hot-symbol ratchets

HTTP capture/replay is controlled by `PSP_BROWSER_ENABLE_FETCH_TRACE`.
Its default is OFF for ordinary live-network PSP builds and ON for host,
PSP validation, and hermetic `psp-replay` builds. Set it explicitly when
reconfiguring an existing build directory: CMake preserves cached choices.
Disabling it keeps the shared transport and its security gates, but trace
entry points refuse without accessing files or changing session state.

An isolated MinSizeRel comparison against `5b9ed80f` (same Allegrex
toolchain, networking and feature settings; no PGO change) measured:

| Ordinary browser ELF | Capture/replay present | Capture/replay absent |
|---|---:|---:|
| `.text` | 4,677,268 B | 4,635,220 B |
| `.rodata` | 2,115,336 B | 2,109,832 B |
| `.bss` | 775,028 B | 771,956 B |

The 42,048-byte code saving comes from removing unreachable lab machinery,
not changing live-request behavior, compiler optimization, TLS policy, or
page APIs. The host suite retains the capture/replay implementation and
also compiles a trace-disabled transport regression. That regression must
refuse all trace activation and leave an empty test directory untouched.

Ordinary trace-free builds also omit the dormant QuickJS stack sampler and
its interrupt-path checks. Validation keeps them. This reduced the same
ordinary ELF by a further 3,504 bytes, to 4,631,716 bytes of `.text`;
`.rodata` and `.bss` were unchanged. The post-link hot-symbol gate now
requires disabled sampler and HTTP capture/replay implementations to be
absent, including GCC-cloned variants, instead of relying only on the
global size ceiling.

Rejected size experiment: replacing the four-float inline-SVG viewBox scan
with `strtof` and routing integer-only core/NanoSVG scans to newlib
`siscanf` added 36 bytes, rather than removing the floating scanner. The
SDK's `__getTlsStructFromThread` still calls `sscanf`; timezone parsing
independently keeps `siscanf`. Both scanner engines therefore remained.
The experiment was reverted. Do not retry these source changes alone
unless that SDK dependency changes; do not replace thread/TLS glue or
interpose libc solely to claim the expected scanner saving.

The next isolated size pass shares the browser's already-required zlib with
FreeType's WOFF1 decoder instead of retaining a private inflater. FreeType
continues installing its Budget-backed allocation callbacks on the zlib
stream. Its sfnt/TrueType/smooth module set and font admission limits are
unchanged. This removed 9,764 B of `.text` and 3,616 B of `.rodata` from the
ordinary PSP EBOOT. The post-link gate rejects a local `inflate`,
`inflateInit2_`, or `inflate_table` implementation in the vendored-font build;
the regression gate fails on the preceding ELF and passes on the shared build.

Compressed-WOFF coverage now compares glyph pixels and metrics against the
raw sfnt and uncompressed WOFF, checks truncation, and sweeps injected Budget
allocation failures through the inflater. An optimized arm64 host experiment
(seven runs of 1,000 compressed-font loads plus glyph rasterization, after
20 warm-up loads) measured median 442.292 us with the private inflater and
223.167 us with shared zlib, with identical glyph hashes, 169,716 B peak
Budget ownership, and zero retained bytes. This is a host cost comparison,
not a measured PSP speedup; physical font-load timing remains unqualified.

Compile-time removal of leftover media-response/playlist diagnostics, the
bootstrap census branch, and the allocation-trap stack callback saved a
further 1,164 B of code and 480 B of read-only data. Ordinary allocation
statistics and runtime/watchdog behavior remain intact; validation and host
builds retain the diagnostics. The allocation trap is not a release feature.

| Ordinary release | Before this pass (`d1098c43`) | After |
|---|---:|---:|
| `.text` | 4,631,716 B | 4,620,788 B |
| `.rodata` | 2,109,832 B | 2,105,736 B |
| `.bss` | 771,956 B | 771,956 B |
| EBOOT.PBP | 6,770,543 B | 6,755,503 B |

Named ordinary and validation PSP targets and post-link gates passed; the
validation ELF retains the sampler, replay, resolver-log, and allocation-trap
symbols. The optimized host
build succeeded and the full enabled suite ran; two local-server tests
(`tilefinch-local-update-server-tests` and
`tilefinch-fetch-stream-scheduler-tests`) cannot pass in this sandbox because
loopback bind is refused with EPERM. The other tests passed or recorded their
normal prerequisite skips. Run the two blocked tests in an ordinary host
environment before integration; the sandbox result is not a full green gate.

An isolated duplicate-code pass starting at `c69f19d6` shares the structured
audio/video DOM script walk without changing their separate capacities,
candidate order, malformed/truncated diagnostics, or allocation-free scan.
Ordinary PSP `.text` fell 360 B (4,620,788 to 4,620,428 B); EBOOT fell 368 B.
Seven alternating optimized-host runs over 120 mixed data scripts measured
49.367 / 49.537 us median per audio+video scan before/after (0.34%, below a
material difference); candidates, counters, and Budget ownership matched.
Physical PSP timing is not qualified. The media tests pin asymmetric candidate
limits, overflow, strict versus assignment-script diagnostics, text truncation,
invalid-input clearing, and zero teardown ownership.

The same pass consolidates four settings reload tails, keeping the security
setting's conditional cancellation and each caller's distinct success/failure
messages. It removes another 428 B of ordinary PSP code and 416 B of EBOOT
(now 4,620,000 B `.text`, 6,754,719 B EBOOT). A host-compiled fake PSP boundary
executes the production helper across 48 combinations and checks navigation,
cancel, clock, timestamp, and status outcomes; two additional cases preserve
the policy-action frame snapshot versus the security action's live frame field.
Seven alternating million-call host batches after warm-up measured
45.464 / 45.801 ns median before/after (0.74%, 0.337 ns). This cold menu path
adds no per-frame work or syscalls; physical PSP menu timing is unqualified.

Extracting the two identical inline border-paint blocks removes another 160 B
of ordinary PSP code/EBOOT (4,619,840 B `.text`, 6,754,559 B EBOOT). The helper
preserves top/bottom/left/right command order, corner overlaps, coordinates,
alpha, and first-refusal propagation without scratch allocation. A 360-item
bordered input/image/textarea fixture produces the same 2,040-command hash and
2,895,624 B Budget peak. Two sets of 15 same-process alternating library
comparisons were noisy; the second measured 1,097.670 / 1,079.495 us median
layout time before/after. No host slowdown was observed, but this is not a
speedup or physical PSP performance claim. Tests pin translucent border
commands for both paths and sweep 48 allocation-refusal points with ownership
returning to baseline. The parity tests also pass against the original code;
these are behavior-preserving extractions, not fixes for a rendering defect.

The final optimized host build succeeds. Of 183 enabled tests, 177 pass and
four record prerequisite skips; the same two loopback-server tests remain
blocked by sandbox socket refusal. A navigation stage-deadline test failed in
one busy parallel run, then passed against both baseline and updated libraries
alone and passed in the final full parallel run. Named ordinary PSP targets
and text/hot-function ratchets pass; no new hardware timing claim is made.

The PSP link gate reads the final ELF with PSP binutils:

- total `.text` has separate ordinary and validation limits;
- `.rodata` is reported rather than charged to the `.text` limit;
- `main` is a cold-growth tripwire;
- the interactive frame is measured as what each kind of frame runs through:
  `psp_loop_frame` plus the steps every page frame (or media frame) calls,
  summed against one ceiling per kind, with `psp_loop_frame` itself held to a
  tighter tripwire so new work arrives as a named step; rare steps (suspend,
  update, a navigation's end) are outside those sums;
- browser/media compositors, `layout_block_impl`, `rasterize_command`, and
  action dispatch have individual size ceilings tied to recurring
  instruction-cache cost;
- raw frontend engine-view access is ratcheted at zero.

Raising a ceiling requires a measured target-side reason. Moving code out of a
ratcheted function without reducing the recurring call graph is not considered
an optimization.

## Device-cost gate

`tilefinch-device-cost-tests` is registered but disabled by default because it
boots PPSSPP. When enabled, it runs a hermetic start-page scenario against
`tests/psp-device-cost-baseline.tsv` and compares budget categories, engine
memory, render-job work, JavaScript retention, presentation, and cadence.

Counters are classified as:

- **exact** for deterministic ownership and work;
- **banded** for small scheduling-dependent variation with an explicit
  tolerance;
- **masked** for wall-clock values which must remain present but cannot be
  meaningfully equal across emulator runs.

Baseline generation requires repeated runs to agree under those treatments.
The gate complements, but does not replace, physical PSP measurement.

## Storage budget

The frame loop, scrolling, focus, layout, raster, and playback paths do not
read or write the Memory Stick. Validation normally streams one aggregate log
through PSPLink. Persistent profile changes are debounced, and large file work
is chunked. The complete file and frequency contract is in
[Memory Stick storage](../STORAGE.md).

## Qualification questions

Any feature which changes the envelope should answer:

1. What is its maximum resident and transient memory?
2. Which ledger or process reserve owns it?
3. What is evicted or refused when full?
4. What is the longest uninterrupted browser-thread work unit?
5. Does the PSP object introduce software 64-bit arithmetic or a large frame?
6. Does `.text`, `.rodata`, or a hot symbol grow?
7. Does it add a Memory Stick read or write, and at what frequency?
8. Which host, PPSSPP, and hardware observations validate the result?
