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
| `TILEFINCH_TRACE_CONSOLE` | 1 | `src/js_runtime/host_primitives.inc` | always | Print page console output (first 64 messages). |
| `TILEFINCH_TRACE_COOKIE` | 3 | `src/fetch/transport.inc` | always | Cookie store decisions. |
| `TILEFINCH_TRACE_CURL_POOL` | 1 | `src/fetch/response_stream.inc` | host | libcurl concurrent pool metrics. |
| `TILEFINCH_TRACE_DOM` | 1 | `src/interactive_main.c` | always | Dump the DOM after the interactive lab tick loop. |
| `TILEFINCH_TRACE_DPU` | 2 | `src/diagnostic_trace.h` | always | Evaluate the DPU bootstrap diagnostic module. |
| `TILEFINCH_TRACE_EVAL_SOURCE` | 1 | `src/diagnostic_trace.h` | host | Install the eval-source tracing hook in the page. |
| `TILEFINCH_TRACE_EVAL_SOURCE_DIR` | 1 | `src/js_runtime/runtime_creation.inc` | host | Directory where the eval-source hook writes evaluated sources (16 max). |
| `TILEFINCH_TRACE_FINAL_DOM` | 1 | `src/interactive_main.c` | always | Dump the DOM at the end of the interactive lab run. |
| `TILEFINCH_TRACE_FLEX_CLASS` | 1 | `src/layout.c` | host | Class name whose flex layout is traced. |
| `TILEFINCH_TRACE_FLEX_TRANSLATE` | 1 | `src/layout.c` | host | Flex translation tracing. |
| `TILEFINCH_TRACE_FRAME_CONTROLS` | 1 | `src/diagnostic_trace.h` | host | Frame control activation phases. |
| `TILEFINCH_TRACE_FRAME_MESSAGES` | 4 | `src/diagnostic_trace.h` | host | Cross-frame message queue events. |
| `TILEFINCH_TRACE_FRAME_MESSAGE_DROPS` | 2 | `src/navigation/page_lifecycle.inc` | host | Only dropped/rejected cross-frame messages. |
| `TILEFINCH_TRACE_GEOMETRY` | 1 | `src/js_runtime/host_primitives.inc` | host | Layout box geometry lookups from script. |
| `TILEFINCH_TRACE_IMAGES` | 3 | `src/image.c` | always | Image resource events. |
| `TILEFINCH_TRACE_IMAGE_PROFILE` | 1 | `src/image.c` | always | Image decode timing profile. |
| `TILEFINCH_TRACE_INTERACTION` | 1 | `src/interactive/diagnostics.inc` | always | Interaction state probe in the interactive lab. |
| `TILEFINCH_TRACE_JS_INTERRUPTS` | 1 | `src/diagnostic_trace.h` | host | QuickJS interrupt and cooperate decisions. |
| `TILEFINCH_TRACE_JS_PROPERTY_FAULTS` | 1 | `src/js_runtime/runtime_creation.inc` | profile builds | Property-fault tracing (property-fault-trace builds only). |
| `TILEFINCH_TRACE_JS_REJECT_STACK` | 1 | `src/budget.c` | host | Backtrace at each refused QuickJS pool request. |
| `TILEFINCH_TRACE_JS_ROOTS` | 4 | `src/interactive_main.c` | always | Script runtime root reports at frame failure and teardown. |
| `TILEFINCH_TRACE_JS_STARTUP` | 1 | `src/diagnostic_trace.h` | host | Per-bootstrap-module timing and the per-module heap census. |
| `TILEFINCH_TRACE_LAYOUT` | 2 | `src/layout.c` | host | Layout tracing. |
| `TILEFINCH_TRACE_LAYOUT_CLASS` | 1 | `src/layout.c` | host | Class name whose layout is traced. |
| `TILEFINCH_TRACE_LAYOUT_PROFILE` | 1 | `src/layout.c` | host | Layout phase profile, including exclusive style/pseudo/intrinsic/margin/text/cooperation flow costs. |
| `TILEFINCH_TRACE_LAYOUT_SLICES` | 1 | `src/layout.c` | host | Resumable layout slice boundaries. |
| `TILEFINCH_TRACE_LAZY_SCRIPTS` | 1 | `src/diagnostic_trace.h` | host | Lazy webpack plan memory and factory events. |
| `TILEFINCH_TRACE_MODULE_ORDER` | 1 | `src/js_module_loader.c` | always | ES module load order. |
| `TILEFINCH_TRACE_MUTATION_JOURNAL` | 2 | `src/navigation/history_runtime.inc` | host | Print each script mutation journal record (kind, element, attribute, exact changed-token count) as layout reuse invalidation consumes it. |
| `TILEFINCH_TRACE_MUTATION_POLICY` | 1 | `src/diagnostic_trace.h` | host | Mutation classification (resource rebuild, image scan) per DOM change. |
| `TILEFINCH_TRACE_NAVIGATION_REQUESTS` | 1 | `src/js_runtime/host_primitives.inc` | host | Script-initiated navigations. |
| `TILEFINCH_TRACE_NETWORK_RESPONSES` | 2 | `src/js_fetch_cors.c` | host | Page network responses (first 64 bytes). |
| `TILEFINCH_TRACE_PAINT` | 2 | `src/layout.c` | host | Paint tracing. |
| `TILEFINCH_TRACE_PREVIEW_LAYOUT` | 1 | `src/navigation/progressive_preview.inc` | host | Progressive preview layout attribution. |
| `TILEFINCH_TRACE_PROMISE_REJECTIONS` | 1 | `src/diagnostic_trace.h` | host | Unhandled promise rejections with counts. |
| `TILEFINCH_TRACE_PROVIDER` | 1 | `src/youtube_lite.c` | host | Video provider route timing. |
| `TILEFINCH_TRACE_PSEUDO_CLASS` | 1 | `src/layout.c` | host | Class name whose pseudo-element layout is traced. |
| `TILEFINCH_TRACE_RANGE_CLASS` | 1 | `src/layout.c` | host | Class name whose range layout is traced. |
| `TILEFINCH_TRACE_RAW_COOKIES` | 2 | `src/fetch/trace_session.inc` | always | Refused by trace acquisition; guards against capturing raw cookies. |
| `TILEFINCH_TRACE_REACT_ERROR` | 1 | `src/script_loader.c` | always | Inject a React error diagnostic into matching bundles. |
| `TILEFINCH_TRACE_REMOTE_MUTATIONS` | 1 | `src/interactive/experimental.inc` | always | Remote (experimental) mutation application. |
| `TILEFINCH_TRACE_REPLAY_DIAGNOSTICS` | 1 | `src/fetch/trace_replay.inc` | always | HTTP trace replay decisions (always on in validation builds). |
| `TILEFINCH_TRACE_REQUEST_BODY` | 1 | `src/js_fetch_cors.c` | always | Small page request bodies. |
| `TILEFINCH_TRACE_RUNTIME_STEPS` | 1 | `src/diagnostic_trace.h` | host | Per-step timing inside a runtime advance. |
| `TILEFINCH_TRACE_SCRIPT_ATTEMPTS` | 2 | `src/script_loader.c` | always | Every script load attempt by URL. |
| `TILEFINCH_TRACE_SCRIPT_FAILURES` | 24 | `src/diagnostic_trace.h` | always | Script admission, compile, and execution failures (29 sites). |
| `TILEFINCH_TRACE_SCRIPT_RESIDENCY` | 1 | `src/js_runtime/evaluation.inc` | always | Script residency phases; the value selects the mode. |
| `TILEFINCH_TRACE_SCROLL` | 1 | `src/diagnostic_trace.h` | host | Script-requested scrolls. |
| `TILEFINCH_TRACE_SCROLL_WIDTH` | 1 | `src/layout.c` | host | Scroll width computation. |
| `TILEFINCH_TRACE_SENTINEL` | 2 | `src/script_loader.c` | always | Inject sentinel/submit diagnostics into matching bundles. |
| `TILEFINCH_TRACE_STARTUP_FAILURE` | 1 | `src/js_module_loader.c` | always | Expose a startup-recovery failure to the page for diagnosis. |
| `TILEFINCH_TRACE_STYLESHEETS` | 14 | `src/resources.c` | host | Stylesheet compile, fragment, and cache decisions (14 sites). |
| `TILEFINCH_TRACE_TASKS` | 1 | `src/diagnostic_trace.h` | host | Timer and task counters at teardown. |
| `TILEFINCH_TRACE_WASM` | 1 | `src/js_wasm_bridge.c` | always | WebAssembly export calls and memory growth. |
| `TILEFINCH_TRACE_WORKER_MESSAGES` | 1 | `src/js_runtime/runtime_creation.inc` | host | Worker messages; the value filters by direction. |
| `TILEFINCH_TRACE_WORKER_SOURCE` | 1 | `src/diagnostic_trace.h` | host | Worker script sources (first 16 KiB). |

## Dump

| Switch | Sites | First site | Device | Purpose |
|---|---|---|---|---|
| `TILEFINCH_DUMP_BUDGET` | 1 | `src/interactive_main.c` | always | Print Budget categories and large resource allocations after the interactive lab tick loop. |
| `TILEFINCH_DUMP_FRAME_MEMORY` | 2 | `src/interactive_main.c` | always | Print per-frame script runtime memory reports in the interactive lab. |
| `TILEFINCH_DUMP_JS_MEMORY` | 1 | `src/diagnostic_trace.h` | host | Print QuickJS memory usage at boot-window advances and runtime teardown. |
| `TILEFINCH_DUMP_JS_POOL` | 1 | `src/diagnostic_trace.h` | host | Print the QuickJS pool class report at runtime teardown. |
| `TILEFINCH_DUMP_JS_POOL_AT_PEAK` | 1 | `src/budget.c` | always | Print the QuickJS pool report when its peak is reached. |
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
| `TILEFINCH_DISABLE_DISCOVERY_GATE` | 1 | `src/js_dom_bindings.c` | host | Refresh image discovery for every class/id change instead of only those a display/visibility/image rule depends on. |
| `TILEFINCH_DISABLE_FIXED_CACHE` | 1 | `src/render.c` | host | Disable the fixed-position paint cache in the tile renderer. |
| `TILEFINCH_DISABLE_IMAGE_PREFILTER` | 1 | `src/image.c` | host | Resolve every element's ::before/::after styles during image discovery instead of skipping elements whose pseudo candidates cannot supply an image. |
| `TILEFINCH_DISABLE_INSERT_SCOPED_REUSE` | 1 | `src/navigation/history_runtime.inc` | host | Reset the layout reuse cache for every child-list insertion instead of scoping detached-subtree insertions to their parent. |
| `TILEFINCH_DISABLE_LAZY_WEBPACK` | 2 | `src/js_fetch_cors.c` | always | Load large webpack bundles eagerly instead of through the lazy factory plan. |
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
| `TILEFINCH_DISABLE_VARIABLE_CACHE` | 1 | `src/style_sheet/declarations.inc` | host | Disable the custom-property resolution cache. |

## Tuning

| Switch | Sites | First site | Device | Purpose |
|---|---|---|---|---|
| `TILEFINCH_JS_BOOT_WINDOW_KB` | 3 | `src/js_runtime/runtime_creation.inc` | always | Extra heap admitted during the page boot window, in KiB. |
| `TILEFINCH_JS_GC_GROWTH_PCT` | 4 | `src/js_runtime/runtime_loop.inc` | always | Automatic GC threshold growth percentage after a collection. |
| `TILEFINCH_JS_GC_MIN_SLACK_KB` | 1 | `src/js_runtime/runtime_loop.inc` | always | Minimum growth window re-armed after a full collection, in KiB. |
| `TILEFINCH_JS_GC_RESERVE_KB` | 1 | `src/js_runtime/runtime_loop.inc` | always | Pre-limit reserve kept below the hard heap limit, in KiB. |
| `TILEFINCH_JS_GC_SLACK_KB` | 2 | `src/js_runtime/runtime_loop.inc` | always | Fixed slack added to the GC threshold, in KiB. |
| `TILEFINCH_JS_POOL_TRIM_RETRY` | 1 | `src/budget.c` | always | Set to 0 to disable the trim-and-retry on a refused QuickJS pool request. |
| `TILEFINCH_JS_STACK_KB` | 1 | `src/js_runtime/runtime_creation.inc` | always | QuickJS native stack limit, in KiB. |
| `TILEFINCH_SELECTOR_COOPERATE_VISITS` | 1 | `src/style_sheet/declarations.inc` | host | Selector matching visits between cooperative checkpoints. |
| `TILEFINCH_VARIABLE_CACHE_ENTRIES` | 1 | `src/style_sheet/declarations.inc` | host | Custom-property cache capacity. |

## Fault

| Switch | Sites | First site | Device | Purpose |
|---|---|---|---|---|
| `TILEFINCH_BUDGET_TRAP_SEQ` | 1 | `src/budget.c` | host | Abort at the Budget allocation with this sequence number, to catch a leak or overrun at its source. |
| `TILEFINCH_JS_POOL_FAIL_OVER_KB` | 1 | `src/budget.c` | host | Fail QuickJS pool requests at or above this size, in KiB. |
| `TILEFINCH_JS_TRAP_ALLOC_SIZE` | 2 | `src/budget.c` | always | Abort on a QuickJS allocation at or above this size, to find the caller. |
| `TILEFINCH_TEST_BOOTSTRAP_BYTECODE_UNAVAILABLE` | 1 | `src/js_runtime/runtime_creation.inc` | host | Pretend bytecode restore failed so the source fallback path runs. |

## Lab

| Switch | Sites | First site | Device | Purpose |
|---|---|---|---|---|
| `TILEFINCH_DIAGNOSTIC_MOBILE_SAFARI` | 4 | `src/fetch/scheduler.inc` | always | Present a mobile Safari identity (viewport, UA hints) to a page for behaviour comparison; host-only diagnostic. |
| `TILEFINCH_ENABLE_IDLE_GLYPH_WARM` | 1 | `src/render.c` | always | Enable idle-time glyph cache warming (off by default). |
| `TILEFINCH_EXPERIMENTAL_BACKGROUND_IMAGES` | 1 | `src/navigation/commit_transaction.inc` | host | Enable the experimental background-image loading path after paint. Not exercised by the mutable image queue; see review notes. |
| `TILEFINCH_FORCE_REBUILDS` | 1 | `src/interactive_main.c` | always | Force this many extra style rebuilds in the interactive lab. |
| `TILEFINCH_HIBERNATE_RESUME_DIR` | 1 | `src/interactive_main.c` | host | Directory for the forked hibernate/resume spike in the interactive lab. |
| `TILEFINCH_HIBERNATE_SPIKE_TICK` | 1 | `src/interactive_main.c` | host | Tick at which the interactive lab forks the hibernate/resume spike. |
| `TILEFINCH_PROBE_EVAL_FILE` | 1 | `src/interactive_main.c` | always | Script file evaluated in the page context after the interactive lab tick loop. |
