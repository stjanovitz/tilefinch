# PSP resource envelope

Tilefinch treats memory, uninterrupted CPU time, executable size, and storage
traffic as separate budgets. A page must fit all four; spare capacity in one
does not excuse exceeding another.

## Memory profiles

`browser_config_apply_psp_memory_profile()` installs the canonical profiles.
Realistic is the PSP application's own configuration
(`browser_config_apply_psp_app_defaults()`, from the `BROWSER_PSP_APP_*`
constants that the `boot.cfg` defaults also use); strict is an engineering
pressure profile:

| Policy | Strict | Realistic (the PSP app) |
|---|---:|---:|
| page content budget | 16 MiB | 32 MiB |
| retained navigation entries | 8 | 4 |
| session response cache | 512 KiB | 640 KiB |
| QuickJS heap | 4 MiB | 5 MiB |
| installed-app QuickJS heap floor | 6 MiB | 9 MiB |
| total admitted script source | at most 1 MiB | memory-based (16 MiB ceiling) |
| one script | at most 256 KiB | memory-based (4 MiB ceiling) |
| decoded tile capacity | up to 24 (8 reserved) | up to 24 (8 reserved) |

The PSP application admits a page script by the memory its compile needs,
not by its size: a compile unit (a whole script, or the largest factory or
statement of a bundle the browser splits) is admitted when twice its source
plus 256 KiB fits the QuickJS heap the realm can still grow to while the page
keeps a 3 MiB presentation reserve. A script may be up to 4 MiB
(`BROWSER_PSP_APP_SCRIPT_FILE_KB`), a page's scripts up to 16 MiB in all
(`BROWSER_PSP_APP_SCRIPT_TOTAL_MB`); both are sanity ceilings for hostile
input. A compile that needs more than planned is stopped by the heap limit
and refused alone. "The heap the realm can still grow to" is the heap a
compile can really reach: the heap left under its limit and the growth the
free page Budget can back, which come out of the same free Budget and are
not added (after the Budget has given an inline compile its copy of the
text, when it needs one). Inline, external and module scripts, module
imports and scripts a page inserts are all admitted this way. One that does
not fit is refused before it compiles: a document script is counted as
memory pressure (the census tag `script-memory-pressure`), an inserted one
fails with an `error` event. Once the text the parser has so far of an
inline script could not be admitted, the rest is dropped as it streams, so
the element reads as empty and the page carries on without it. The
admission contract is in
`include/tilefinch/script_admission.h`. Experiment measurements are kept in a
private maintainer ledger outside the repository.

Tiles are 128x128 RGB565 (32 KiB). A 480x272 screen touches twelve to
sixteen; the first eight are reserved when a page's render shell is built.
The remaining slots hold the visible remainder and two rows of
paint-ahead below (or above) the screen: they are allocated on first use
only while the page keeps 2 MiB of budget headroom, and optional-memory
reclaim frees them first. Paint-ahead runs in idle turns. One tile takes
longer than the 500 µs idle budget on the PSP, so tiles may hold a turn for
up to one frame period (16.7 ms). A press during that turn is still served at
the raster's cooperation checkpoints.

Beneath the profiles, PSPSDK's grow-to-largest heap takes what is left after
2 MiB is held back; what remains after the executable, stacks, modules, and
system reserves is the process heap. That heap does not decide what a page
may use; the profile ceiling does.

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

### Boot order

HOME and Collections are native chrome, so boot does only what their first
frame needs. Profile settings, bookmarks, and HOME destinations load before
that frame, because they define the native UI. An ordinary boot shows one
static splash until HOME is ready rather than waiting on scanout for
intermediate startup messages. The timing markers `platform-ready` and
`runtime-ready` describe native callbacks, input, clocks, and service setup,
**not** a page JavaScript runtime. The Media Engine pool still reserves its
addressable memory before the browser heap grows; that is an ownership
requirement, not deferrable page work. Synthetic clock benchmarking runs only
in the explicit `validation_power_test_auto` mode. HOME's first input sample
skips the redundant post-presentation vblank wait and duplicate Home
publication, while later frames keep their usual cadence.

The native HOME placeholder is committed through the initial empty-document
path, so it owns neither an unused QuickJS realm nor a resource
scheduler/HTTP worker. Its temporary resource and script policy is restored
on success or refusal; the configured global and per-site policy applies from
the first real navigation.

Wi-Fi warm-up starts after HOME's first paint, and the highlighted built-in
provider can preconnect once association completes. Secure entropy, the
bounded TLS-session store, the trust bundle (4 KiB reads), and the shared
HTTP worker are prepared in separate post-paint units during association.
The same owner continues those units if an immediate first navigation adopts
the foreground connection wait, including a native provider open. Optional
provider work yields until this preparation completes or refuses; ordinary
demand remains the fallback. No extra host is contacted. The provider's local
YouTube search screen can open before association finishes; explicit
searches, topic requests, and sign-in handoffs still require networking. The
saved Wi-Fi profile name is queried only when Options needs it; the
already-loaded profile number remains usable for navigation before that
query.

Local adapter entry surfaces retain a deferred resource-scheduler view:
creating or pumping an empty view does not acquire curl or start its worker.
The first accepted resource request initializes that view, with ordinary
retryable refusal and teardown. Ordinary document admission keeps its
network reservation, so font and resource pressure decisions do not change.

Activating a HOME tile publishes `OPENING...` through the shared
acknowledgement path before any network or page preparation, so navigation
never has to hide input receipt while background warm-up finishes.

Page fonts follow the same staged boundary, with one presentation rule: the
regular sans face used by native HOME is loaded before HOME's first frame, so
its labels never switch from the bitmap fallback after they are visible. That
is the only page-font read before the first frame. Its metric-only
counterpart loads during association wait frames. On the frame networking
becomes ready, the highlighted built-in provider's preconnect takes precedence
over any remaining font read, and a navigation that outruns warm-up completes
this two-face baseline before measuring text. Serif, italic, and bold faces
are requested from the committed page's bounded census of draw commands and
load one at a time in 16 KiB idle slices, which navigation and rendering can
pre-empt. Bold text requests only the bold face its family draws with
(TilefinchSans Bold for Arial/Helvetica stacks, DejaVu Sans Bold for other
sans families, DejaVu Serif Bold for serif), and a web family is counted as
its built-in fallback. A newly arrived metric face triggers one transactional
relayout before its first repaint, so fallback measurements never survive
under the real glyph advances.

Optional disk-cache and local-storage restoration starts only after HOME is
interactive; [Storage](../STORAGE.md) describes the transactional reader and
its ordering against the first navigation.

### Boot timeline

The validation-only `tilefinch-boot-input` line records the first actual
controller sample and its delay from interactive loop entry. Emulator boot
times do not establish physical PSP release boot time: validation logging and
asset diagnostics remain, and emulator storage is not a Memory Stick.
Input-script file loading adds harness overhead and must not be treated as
shipping work.

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

### Fast paths

PSP-specific fast paths never change results:

- checked 32-bit arithmetic for ordinary viewport geometry, with 64-bit
  fallbacks only for values that need them, and fixed-point font advances
  and kerning in hot measurement paths;
- selector programs that compare interned identifiers and reuse node-local
  attribute facts, and compiled stylesheet fragments reused from bounded RAM
  caches (never written to the Memory Stick), and the previous page's whole
  compiled sheet reused when the next page, or Back to one, has the same
  style inputs;
- preview layout limited to geometry that can affect the viewport;
- on simple script-free pages, visible images loaded before publication and
  offscreen images pumped from idle work one viewport-ranked request and one
  decode at a time, kept in the bounded RAM cache for repeat navigations;
- native image slots retained before decoding for script-free, fixed-size
  raster images, so a matching DOM/cascade generation permits paint-only
  publication that keeps unrelated tiles and the display list (intrinsic
  sizing, author runtimes, fixed/sticky layers, and expanding filters keep the
  full-layout fallback);
- image discovery that carries the computed parent style instead of
  rebuilding the cascade per image, and bounded caches with measured eviction
  for repeated gradients, glyphs, and tiles.

## Executable and hot-symbol ratchets

HTTP capture/replay is controlled by `PSP_BROWSER_ENABLE_FETCH_TRACE`.
Its default is OFF for ordinary live-network PSP builds and ON for host,
PSP validation, and hermetic `psp-replay` builds. Set it explicitly when
reconfiguring an existing build directory: CMake preserves cached choices.
Disabling it keeps the shared transport and its security gates, but trace
entry points refuse without accessing files or changing session state.

Ordinary trace-free builds also omit the dormant QuickJS stack sampler and
its interrupt-path checks; validation keeps them. The post-link hot-symbol
gate requires the disabled sampler and HTTP capture/replay implementations to
be absent, including GCC-cloned variants, instead of relying only on the
global size ceiling. The host suite retains the capture/replay implementation
and also compiles a trace-disabled transport regression, which must refuse
all trace activation and leave an empty test directory untouched.

FreeType's WOFF1 decoder shares the browser's zlib instead of a private
inflater, with its Budget-backed allocation callbacks installed on the zlib
stream. The post-link gate rejects a local `inflate`, `inflateInit2_`, or
`inflate_table` implementation in the vendored-font build.

Ordinary builds compile out the media-response/playlist diagnostics, the
bootstrap census branch, and the allocation-trap stack callback; validation
and host builds retain them. The allocation trap is not a release feature.

Do not retry routing the inline-SVG viewBox and NanoSVG integer scans through
`strtof`/`siscanf` to drop the floating scanner: the SDK's
`__getTlsStructFromThread` still calls `sscanf`, so both scanner engines stay.

The PSP link gate reads the final ELF with PSP binutils:

- total `.text` has separate ordinary and validation limits, set in
  `cmake/TilefinchTargets.cmake` and checked by `cmake/CheckPspTextSize.cmake`;
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
