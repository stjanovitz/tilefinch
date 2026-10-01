# Diagnostic switches

Every `TILEFINCH_*` environment variable the engine reads, generated from
the `getenv` sites in `src/` and checked by `tests/test_diagnostic_switches.py`,
which fails when a switch is read that this file does not list or listed
here but no longer read. Add a row when adding a switch.

"Device" says whether the read survives into a `TILEFINCH_NO_TRACE` device
build: *host* means it is compiled out there, *always* means the device
reads it too (a PSP build has no environment, so the default applies, but
the code and its strings ship).

## Trace

| Switch | Sites | First site | Device | Purpose |
|---|---|---|---|---|
| `TILEFINCH_TRACE_BASE64` | 1 | `src/diagnostic_trace.h` | host | Expose base64 tracing to the bootstrap. |
| `TILEFINCH_TRACE_CALLBACK_SOURCE` | 1 | `src/diagnostic_trace.h` | host | Capture the source context of the last uncaught frame callback error. |
| `TILEFINCH_TRACE_CLIP` | 1 | `src/layout.c` | host | Layout clip tracing. |
| `TILEFINCH_TRACE_CONSOLE` | 1 | `src/diagnostic_trace.h` | host | Print page console output (first 64 messages). |
| `TILEFINCH_TRACE_COOKIE` | 1 | `src/diagnostic_trace.h` | host | Cookie store decisions; value `summary` omits URLs, byte counts, and cookie contents. |
| `TILEFINCH_TRACE_CURL_POOL` | 1 | `src/diagnostic_trace.h` | host | libcurl concurrent pool metrics. |
| `TILEFINCH_TRACE_DOM` | 1 | `src/interactive_main.c` | always | Dump the DOM after the interactive lab tick loop. |
| `TILEFINCH_TRACE_DPU` | 1 | `src/diagnostic_trace.h` | host | Evaluate the DPU bootstrap diagnostic module. |
| `TILEFINCH_TRACE_EVAL_SOURCE` | 1 | `src/diagnostic_trace.h` | host | Install the eval-source tracing hook in the page. |
| `TILEFINCH_TRACE_EVAL_SOURCE_DIR` | 1 | `src/js_runtime/runtime_creation.inc` | host | Directory where the eval-source hook writes evaluated sources (16 max). |
| `TILEFINCH_TRACE_FINAL_DOM` | 1 | `src/interactive_main.c` | always | Dump the DOM at the end of the interactive lab run. |
| `TILEFINCH_TRACE_FRAME_DOM` | 1 | `src/interactive_main.c` | always | Dump each loaded child frame's DOM (with style, SVG sizing and control attributes) after the interactive lab tick loop. |
| `TILEFINCH_TRACE_FLEX_CLASS` | 1 | `src/layout.c` | host | Class name whose flex layout is traced. |
| `TILEFINCH_TRACE_FLEX_TRANSLATE` | 1 | `src/layout.c` | host | Flex translation tracing. |
| `TILEFINCH_TRACE_FRAME_CONTROLS` | 1 | `src/diagnostic_trace.h` | host | Frame control activation phases. |
| `TILEFINCH_TRACE_FRAME_MESSAGES` | 4 | `src/diagnostic_trace.h` | host | Cross-frame message queue events. |
| `TILEFINCH_TRACE_FRAME_MESSAGE_DROPS` | 2 | `src/navigation/page_lifecycle.inc` | host | Only dropped/rejected cross-frame messages. |
| `TILEFINCH_TRACE_GEOMETRY` | 1 | `src/js_runtime/host_primitives.inc` | host | Layout box geometry lookups from script. |
| `TILEFINCH_TRACE_IMAGES` | 1 | `src/diagnostic_trace.h` | host | Image resource events. |
| `TILEFINCH_TRACE_IMAGE_PROFILE` | 1 | `src/diagnostic_trace.h` | host | Image decode timing profile. |
| `TILEFINCH_TRACE_INTERACTION` | 1 | `src/interactive/diagnostics.inc` | always | Interaction state probe in the interactive lab. |
| `TILEFINCH_TRACE_JS_INTERRUPTS` | 1 | `src/diagnostic_trace.h` | host | QuickJS interrupt and cooperate decisions. |
| `TILEFINCH_TRACE_JS_PROPERTY_FAULTS` | 1 | `src/diagnostic_trace.h` | host | Bounded property tracing; numeric values trace all events, `absent` traces failed `in`, and `fault` traces null/undefined bases (property-fault-trace builds only). |
| `TILEFINCH_TRACE_JS_REJECT_STACK` | 1 | `src/budget.c` | host | Backtrace at each refused QuickJS pool request. |
| `TILEFINCH_TRACE_JS_ROOTS` | 2 | `src/diagnostic_trace.h` | always | Script runtime root reports at frame failure and teardown. |
| `TILEFINCH_TRACE_JS_STARTUP` | 1 | `src/diagnostic_trace.h` | host | Per-bootstrap-module timing and the per-module heap census. |
| `TILEFINCH_TRACE_LAYOUT` | 2 | `src/layout.c` | host | Layout tracing. |
| `TILEFINCH_TRACE_LAYOUT_CLASS` | 1 | `src/layout.c` | host | Class name whose layout is traced. |
| `TILEFINCH_TRACE_LAYOUT_PROFILE` | 1 | `src/layout.c` | host | Layout phase profile, including exclusive style/pseudo/intrinsic/margin/text/cooperation flow costs. |
| `TILEFINCH_TRACE_LAYOUT_SLICES` | 1 | `src/layout.c` | host | Resumable layout slice boundaries. |
| `TILEFINCH_TRACE_LAZY_SCRIPTS` | 1 | `src/diagnostic_trace.h` | host | Lazy webpack plan memory and factory events. |
| `TILEFINCH_TRACE_MODULE_ORDER` | 1 | `src/diagnostic_trace.h` | host | ES module load order. |
| `TILEFINCH_TRACE_MUTATION_JOURNAL` | 8 | `src/navigation/history_runtime.inc` | host | Print each script mutation journal record (kind, element, attribute, exact changed-token count) as layout reuse invalidation consumes it, the resets and scoped `:has()` invalidation it caused, and each build's reuse-cache style hits and misses. |
| `TILEFINCH_TRACE_MUTATION_POLICY` | 1 | `src/diagnostic_trace.h` | host | Mutation classification (resource rebuild, image scan) per DOM change. |
| `TILEFINCH_TRACE_NAVIGATION_REQUESTS` | 1 | `src/js_runtime/host_primitives.inc` | host | Script-initiated navigations. |
| `TILEFINCH_TRACE_NETWORK_RESPONSES` | 2 | `src/js_fetch_cors.c` | host | Page network responses (first 64 bytes). |
| `TILEFINCH_TRACE_PAINT` | 2 | `src/layout.c` | host | Paint tracing. |
| `TILEFINCH_TRACE_PREVIEW_LAYOUT` | 1 | `src/navigation/progressive_preview.inc` | host | Progressive preview layout attribution. |
| `TILEFINCH_TRACE_PROMISE_REJECTIONS` | 1 | `src/diagnostic_trace.h` | host | Unhandled promise rejections with counts. |
| `TILEFINCH_TRACE_PROVIDER` | 1 | `src/youtube_lite.c` | host | Video provider route timing. |
| `TILEFINCH_TRACE_PSEUDO_CLASS` | 1 | `src/layout.c` | host | Class name whose pseudo-element layout is traced. |
| `TILEFINCH_TRACE_RANGE_CLASS` | 1 | `src/layout.c` | host | Class name whose range layout is traced. |
| `TILEFINCH_TRACE_RAW_COOKIES` | 2 | `src/diagnostic_trace.h` | always | Refused by trace acquisition; guards against capturing raw cookies. |
| `TILEFINCH_TRACE_REACT_ERROR` | 1 | `src/diagnostic_trace.h` | host | Inject a React error diagnostic into matching bundles. |
| `TILEFINCH_TRACE_REMOTE_MUTATIONS` | 1 | `src/interactive/experimental.inc` | always | Remote (experimental) mutation application. |
| `TILEFINCH_TRACE_JS_PROFILE` | 1 | `src/js_runtime/evaluation.inc` | host | Interrupt-time sampling profile of page script (self time by line, inclusive by function, both with the function's definition line `fn=` so anonymous functions stay apart; promise-job time by call site with its outermost frames and inclusive/self tables; per-function call counts in `PSP_BROWSER_JS_CALL_COUNTS=ON` builds); the lab's `profile [LABEL]` command prints and clears it. `tools/js_profile_report.py LABEL RUN...` joins runs per bootstrap function (use `TILEFINCH_DISABLE_BOOTSTRAP_BYTECODE=1` for bootstrap lines). On by default in PSP validation builds, reported at each input-script mark. While it runs, the `tilefinch-work` record also counts `js.native_calls` and the top `js.native.<name>` natives. |
| `TILEFINCH_JS_OPCODE_HISTOGRAM` | 1 | `src/work_vector.c` | host / PSP validation | After every `tilefinch-work` record, the page realm's cumulative per-opcode dispatch counts as `tilefinch-opcodes: label=L op=NAME count=N` lines, most frequent first (op-count engines, `PSP_BROWSER_JS_OP_COUNTS=ON`; other builds print nothing). Subtract consecutive labels for a window's opcode mix. |
| `TILEFINCH_JS_POOL_HISTOGRAM` | 1 | `src/work_vector.c` | host / PSP validation | After every `tilefinch-work` record, the page realm's QuickJS pool traffic per class as `tilefinch-js-pool: label=L class=C allocs=N fresh=N live=N live-requested=N live-capacity=N cached=N` (cumulative blocks handed out and fresh Budget allocations; `class=0` is the direct class), then one line with reserved and `js-malloc` totals and peaks. Classes 16-512 carry no traffic: QuickJS carves small objects from its own 4 KiB arenas, which reach the pool as the 4096 class. |
| `TILEFINCH_VALIDATION_JS_PROFILE` | 2 | `src/js_runtime/evaluation.inc` | PSP validation | Set internally from `boot.cfg`'s `validation_js_profile` (default `1`); `0` disables the JS sampler and call wrappers for a same-binary device comparison. Confirm the `tilefinch-js-profiler` line reports `enabled=0 set-status=0` before trusting a profiler-off timing. |
| `TILEFINCH_SCRIPT_SPLIT` | 1 | `src/js_runtime/runtime_creation.inc` | host / PSP validation | The script split (`include/tilefinch/script_split.h`) is on by default in host and PSP validation builds. `0` turns it off; `1` keeps the timed bridge kinds but skips the per-poll frame sample (no page/bootstrap/native attribution). For measuring the split's own cost in one binary. On PSP validation builds, `boot.cfg`'s `validation_script_split=0\|1\|2` (default `2`) sets it, and a `tilefinch-script-split: mode=` line confirms a non-default mode. |
| `TILEFINCH_JS_PROFILE_RANKS` | 1 | `src/js_runtime/evaluation.inc` | host | Rows per profile table (up to 1024 on the host, 256 on the PSP) for a deeper look at a flat profile; PSP builds print 12. |
| `TILEFINCH_JS_PROFILE_OUTLIER_US` | 1 | `src/js_runtime/evaluation.inc` | host / PSP validation | With the profiler on, prints `tilefinch-js-outlier` (stack with definition locations, phase, site, promise-job identity and last timed native) for sample intervals at least this long. Matching `tilefinch-promise-job` records separate sampled, native, GC, cooperation and profiler time, with lazy-compilation counts/bytes; unframed time is part of sampled time. Intervals over 250 ms are reported as unassigned `long-gap-us`, not charged to a sampled frame. Root samples explicitly mark depth truncation. `tools/promise_job_report.py` joins records without double-counting nested work. Opt-in only; low thresholds can produce large private logs and increase profiling overhead. On PSP validation, `boot.cfg`'s `validation_js_outlier_us` sets it. |
| `TILEFINCH_DISABLE_FAST_DOM` | 1 | `src/js_dom_bindings.c` | host | Keep the script versions of the native node getters and methods (parentNode, parentElement, tagName, nodeType, getAttribute), for A/B timing and debugging. |
| `TILEFINCH_TRACE_REPLAY_DIAGNOSTICS` | 1 | `src/diagnostic_trace.h` | host | HTTP trace replay decisions (always on in validation builds). |
| `TILEFINCH_REPLAY_IGNORE_REQUEST_BODY` | 1 | `src/fetch/trace_replay.inc` | host / PSP validation | Lab replay matches recorded requests without their body length/hash, for POSTs that carry freshly minted tokens. On PSP validation builds, `boot.cfg`'s `trace_ignore_request_body=1` sets it. |
| `TILEFINCH_REPLAY_PUMP_US` | 1 | `src/fetch/trace_replay.inc` | host / PSP validation | Replay holds each response for its recorded `async-delay-pumps` times this many microseconds of wall time (plus at least one pump) instead of for that many scheduler pumps, so a loop change that pumps more often does not also make the recorded network answer sooner (A/B of frame pacing). Read when replay begins; `stage=replay-admit` records print the `release-us`. On PSP validation builds, `boot.cfg`'s `trace_replay_pump_us=N` sets it. |
| `TILEFINCH_REPLAY_VOLATILE_UUIDS` | 1 | `src/fetch/trace_replay.inc` | host / PSP validation | Lab replay treats client-minted UUIDs as volatile: a UUID-shaped query value matches any UUID (all other URL bytes still match exactly), and the served headers and body echo the live request's UUIDs in place of the recorded request's (paired by field from the URL and the retained `NNNN.request` body). For pages that key a streamed response to identifiers minted per send, such as chatgpt.com's `operationId`. Read when replay begins; decisions print as `HTTP replay volatile-uuid`. On PSP validation builds, `boot.cfg`'s `trace_volatile_uuids=1` sets it. |
| `TILEFINCH_TRACE_REQUEST_BODY` | 1 | `src/diagnostic_trace.h` | host | Small page request bodies. |
| `TILEFINCH_TRACE_RUNTIME_STEPS` | 1 | `src/diagnostic_trace.h` | host | Per-step timing inside a runtime advance. |
| `TILEFINCH_TRACE_SCRIPT_ATTEMPTS` | 1 | `src/diagnostic_trace.h` | host | Every script load attempt by URL. |
| `TILEFINCH_TRACE_SCRIPT_FAILURES` | 2 | `src/diagnostic_trace.h` | always | Script admission, compile, and execution failures (29 sites). |
| `TILEFINCH_TRACE_SCRIPT_RESIDENCY` | 1 | `src/diagnostic_trace.h` | host | Script residency phases; the value selects the mode. |
| `TILEFINCH_TRACE_SCROLL` | 1 | `src/diagnostic_trace.h` | host | Script-requested scrolls. |
| `TILEFINCH_TRACE_SCROLL_WIDTH` | 1 | `src/layout.c` | host | Scroll width computation. |
| `TILEFINCH_TRACE_SENTINEL` | 1 | `src/diagnostic_trace.h` | host | Inject sentinel/submit diagnostics into matching bundles. |
| `TILEFINCH_TRACE_STARTUP_FAILURE` | 1 | `src/diagnostic_trace.h` | host | Expose a startup-recovery failure to the page for diagnosis. |
| `TILEFINCH_TRACE_STYLESHEETS` | 14 | `src/resources.c` | host | Stylesheet compile, fragment, and cache decisions (14 sites). |
| `TILEFINCH_TRACE_TASKS` | 1 | `src/diagnostic_trace.h` | host | Timer and task counters at teardown. |
| `TILEFINCH_TRACE_WASM` | 1 | `src/diagnostic_trace.h` | host | WebAssembly export calls and memory growth. |
| `TILEFINCH_TRACE_WORKER_MESSAGES` | 1 | `src/js_runtime/runtime_creation.inc` | host | Worker messages; the value filters by direction. |
| `TILEFINCH_TRACE_WORKER_SOURCE` | 1 | `src/diagnostic_trace.h` | host | Worker script sources (first 16 KiB). |

## Dump

| Switch | Sites | First site | Device | Purpose |
|---|---|---|---|---|
| `TILEFINCH_DEBUG_BOX_CLASS` | 1 | `src/interactive_main.c` | always | Print the final layout boxes (geometry, content size, command range) of up to 16 elements whose class contains this text. |
| `TILEFINCH_DUMP_BUDGET` | 1 | `src/interactive_main.c` | always | Print Budget categories and large resource allocations after the interactive lab tick loop. |
| `TILEFINCH_DUMP_FRAME_MEMORY` | 2 | `src/interactive_main.c` | always | Print per-frame script runtime memory reports in the interactive lab. |
| `TILEFINCH_DUMP_JS_MEMORY` | 1 | `src/diagnostic_trace.h` | host | Print QuickJS memory usage at boot-window checks (with the check's GC and census times) and runtime teardown. |
| `TILEFINCH_DUMP_JS_POOL` | 2 | `src/diagnostic_trace.h` | host | Print the QuickJS pool class report at runtime teardown. |
| `TILEFINCH_DUMP_JS_POOL_AT_PEAK` | 1 | `src/diagnostic_trace.h` | host | Print the QuickJS pool report when its peak is reached. |
| `TILEFINCH_DUMP_JS_PROFILE` | 1 | `src/diagnostic_trace.h` | host | Dump the QuickJS execution profile at teardown (profile builds only). |

## A/B

| Switch | Sites | First site | Device | Purpose |
|---|---|---|---|---|
| `TILEFINCH_DISABLE_ANCESTOR_CACHE` | 1 | `src/style_sheet/declarations.inc` | host | Disable the layout-scoped selector and ancestor-bloom caches for timing comparison. |
| `TILEFINCH_DISABLE_BOOTSTRAP_BYTECODE` | 1 | `src/js_runtime/runtime_creation.inc` | always | Compile the embedded bootstrap from source instead of restoring bytecode; gives bootstrap stack traces line numbers. |
| `TILEFINCH_DISABLE_BOUNDED_LAYOUT_PREVIEW` | 1 | `src/navigation/commit_transaction.inc` | host | Skip the bounded progressive layout preview policy. |
| `TILEFINCH_DISABLE_COMPILED_COMPLEX_SELECTORS` | 1 | `src/style_selector_program.c` | host | Match complex compound selectors through the interpreter instead of the compiled program. |
| `TILEFINCH_DISABLE_COMPILED_DEFERRED` | 1 | `src/style_properties/dispatch.inc` | host | Disable compiled deferred-property dispatch in the style property table. |
| `TILEFINCH_DISABLE_COMPILED_SELECTORS` | 1 | `src/style_selector_program.c` | host | Never build the compiled selector program for a stylesheet. |
| `TILEFINCH_DISABLE_COMPILED_STYLESHEET_CACHE` | 2 | `src/navigation/page_lifecycle.inc` | host | Do not reuse a compiled stylesheet across navigations. |
| `TILEFINCH_DISABLE_COMPRESSION_STREAMS` | 1 | `src/js_runtime/runtime_creation.inc` | always | Do not install the native CompressionStream/DecompressionStream primitive. |
| `TILEFINCH_DISABLE_DISCOVERY_GATE` | 1 | `src/js_dom_bindings.c` | host | Refresh image discovery for every class/id change instead of only those a display/visibility/image rule (or, with inline SVG rasters, a colour/size/presentation/custom-property rule) depends on. |
| `TILEFINCH_DISABLE_FIXED_CACHE` | 1 | `src/render.c` | host | Disable the fixed-position paint cache in the tile renderer. |
| `TILEFINCH_DISABLE_IMAGE_PREFILTER` | 1 | `src/image.c` | host | Resolve every element's ::before/::after styles during image discovery instead of skipping elements whose pseudo candidates cannot supply an image. |
| `TILEFINCH_DISABLE_INSERT_SCOPED_REUSE` | 1 | `src/navigation/history_runtime.inc` | host | Reset the layout reuse cache for every child-list insertion instead of scoping detached-subtree insertions to their parent. |
| `TILEFINCH_DISABLE_NODE_RETIREMENT` | 1 | `src/navigation/history_runtime.inc` | host | Do not bind the runtime's freed-subtree callback to the page reuse cache, so removals, moves and innerHTML replacements reset the cache again. |
| `TILEFINCH_DISABLE_LAZY_WEBPACK` | 2 | `src/js_fetch_cors.c` | always | Load large webpack bundles eagerly instead of through the lazy factory plan. |
| `TILEFINCH_DISABLE_HAS_SCOPED_INVALIDATION` | 2 | `src/js_dom_bindings.c` | host | Reset the layout reuse cache and getComputedStyle's retained styles for every change a `:has()` rule may observe instead of invalidating only the elements whose `:has()`-dependent matches can move (for A/B comparison). |
| `TILEFINCH_DISABLE_RETAINED_MATCHES` | 1 | `src/layout.c` | host | Disable the page layout reuse cache's retained per-element matched-rule lists for timing and equivalence comparison. |
| `TILEFINCH_DISABLE_SELECTOR_APPEND_REUSE` | 1 | `src/style_sheet.c` | host | Rebuild the selector program on every stylesheet append instead of preserving it. |
| `TILEFINCH_DISABLE_STYLE_APPEND` | 1 | `src/navigation/history_runtime.inc` | host | Rebuild the page stylesheet for every script-inserted `<style>` element instead of appending it in place. |
| `TILEFINCH_DISABLE_STYLESHEET_CONTINUATION` | 1 | `src/navigation/stylesheet_checkpoint.inc` | host | Disable resumable stylesheet parsing across checkpoints. |
| `TILEFINCH_DISABLE_STYLESHEET_FRAGMENT_CACHE` | 1 | `src/resources.c` | host | Do not cache compiled stylesheet fragments in the shared body store. |
| `TILEFINCH_DISABLE_STYLESHEET_PARSED_IR` | 1 | `src/resources.c` | host | Do not cache the parsed stylesheet IR in the shared body store. |
| `TILEFINCH_DISABLE_STYLESHEET_RULE_BATCH` | 1 | `src/resources.c` | host | Insert stylesheet rules one at a time instead of batching. |
| `TILEFINCH_DISABLE_STYLESHEET_SUFFIX_PRELOAD_SETTLE` | 1 | `src/navigation/stylesheet_checkpoint.inc` | host | Skip settling suffix preloads before the stylesheet checkpoint. |
| `TILEFINCH_DISABLE_STYLE_INDEX` | 1 | `src/style_sheet.c` | host | Never build the rule index for a stylesheet. |
| `TILEFINCH_DISABLE_STYLE_RULE_FILTER` | 1 | `src/style_sheet.c` | host | Never build per-rule quick-reject filters. |
| `TILEFINCH_DISABLE_SVG_REFRESH_REUSE` | 2 | `src/image.c` | host | Rasterize every inline SVG a mutation-driven image refresh or image-only rebuild revisits instead of reusing the outgoing table's identical raster. |
| `TILEFINCH_DISABLE_VARIABLE_CACHE` | 1 | `src/style_sheet/declarations.inc` | host | Disable the custom-property resolution cache. |

## Tuning

| Switch | Sites | First site | Device | Purpose |
|---|---|---|---|---|
| `TILEFINCH_JS_BOOT_WINDOW_KB` | 3 | `src/js_runtime/runtime_creation.inc` | always | Extra heap admitted during the page boot window, in KiB. |
| `TILEFINCH_JS_GC_GROWTH_PCT` | 4 | `src/js_runtime/runtime_loop.inc` | always | Automatic GC threshold growth percentage after a collection. |
| `TILEFINCH_JS_GC_MIN_SLACK_KB` | 1 | `src/js_runtime/runtime_loop.inc` | always | Minimum growth window re-armed after a full collection, in KiB. |
| `TILEFINCH_JS_GC_RESERVE_KB` | 1 | `src/js_runtime/runtime_loop.inc` | always | Pre-limit reserve kept below the hard heap limit, in KiB. |
| `TILEFINCH_JS_GC_SLACK_KB` | 2 | `src/js_runtime/runtime_loop.inc` | always | Fixed slack added to the GC threshold, in KiB. |
| `TILEFINCH_JS_LAZY_FUNCTIONS` | 1 | `src/js_runtime/evaluation.inc` | always | Minimum source bytes of a page function compiled lazily on first call; 0 compiles every function eagerly. |
| `TILEFINCH_JS_PREPARSE` | 1 | `src/js_runtime/evaluation.inc` | always | `0` parses the body of every lazily compiled function in full when its script compiles, instead of validating it with the preparser (preparsing). |
| `TILEFINCH_JS_POOL_TRIM_RETRY` | 1 | `src/budget.c` | always | Set to 0 to disable the trim-and-retry on a refused QuickJS pool request. |
| `TILEFINCH_JS_STACK_KB` | 1 | `src/js_runtime/runtime_creation.inc` | always | QuickJS native stack limit, in KiB. |
| `TILEFINCH_SELECTOR_COOPERATE_VISITS` | 1 | `src/style_sheet/declarations.inc` | host | Selector matching visits between cooperative checkpoints. |
| `TILEFINCH_VARIABLE_CACHE_ENTRIES` | 1 | `src/style_sheet/declarations.inc` | host | Custom-property cache capacity. |

## Fault

| Switch | Sites | First site | Device | Purpose |
|---|---|---|---|---|
| `TILEFINCH_BUDGET_TRAP_SEQ` | 1 | `src/budget.c` | host | Abort at the Budget allocation with this sequence number, to catch a leak or overrun at its source. |
| `TILEFINCH_JS_POOL_FAIL_OVER_KB` | 1 | `src/budget.c` | host | Fail QuickJS pool requests at or above this size, in KiB. |
| `TILEFINCH_JS_TRAP_ALLOC_SIZE` | 2 | `src/budget.c` | host/validation | Dump the stack once on a QuickJS allocation at or above this size, to find the caller; compiled out of trace-free releases. |
| `TILEFINCH_TEST_BOOTSTRAP_BYTECODE_UNAVAILABLE` | 1 | `src/js_runtime/runtime_creation.inc` | host | Pretend bytecode restore failed so the source fallback path runs. |

## Lab

| Switch | Sites | First site | Device | Purpose |
|---|---|---|---|---|
| `TILEFINCH_DIAGNOSTIC_MOBILE_SAFARI` | 2 | `src/fetch.c` | always | Present a mobile Safari identity (viewport, UA hints) to a page for behaviour comparison; host-only diagnostic. |
| `TILEFINCH_ENABLE_IDLE_GLYPH_WARM` | 1 | `src/render.c` | always | Enable idle-time glyph cache warming (off by default). |
| `TILEFINCH_EXPERIMENTAL_BACKGROUND_IMAGES` | 1 | `src/navigation/commit_transaction.inc` | host | Enable the experimental background-image loading path after paint. Not exercised by the mutable image queue; see review notes. |
| `TILEFINCH_FORCE_REBUILDS` | 1 | `src/interactive_main.c` | always | Force this many extra style rebuilds in the interactive lab. |
| `TILEFINCH_HIBERNATE_RESUME_DIR` | 1 | `src/interactive_main.c` | host | Directory for the forked hibernate/resume spike in the interactive lab. |
| `TILEFINCH_HIBERNATE_SPIKE_TICK` | 1 | `src/interactive_main.c` | host | Tick at which the interactive lab forks the hibernate/resume spike. |
| `TILEFINCH_PROBE_EVAL_FILE` | 1 | `src/interactive_main.c` | always | Script file evaluated in the page context after the interactive lab tick loop. |
| `TILEFINCH_EXECUTION_CENSUS` | 1 | `src/script_census.c` | host + PSP validation | 1: independent owner-CPU opcode/native census; 2: bounded function/job/path attribution. Off by default; shipping PSP omits instrumentation. Compare sampler-off runs; reference-count builds are count-only. |
