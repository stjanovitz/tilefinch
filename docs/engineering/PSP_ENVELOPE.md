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
| decoded tile capacity | up to 8 | up to 8 |

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

The PSP link gate reads the final ELF with PSP binutils:

- total `.text` has separate ordinary and validation limits;
- `.rodata` is reported rather than charged to the `.text` limit;
- `main` is a cold-growth tripwire;
- `psp_app_run_interactive`, browser/media compositors,
  `layout_block_impl`, `rasterize_command`, and action dispatch have individual
  size ceilings tied to recurring instruction-cache cost;
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
