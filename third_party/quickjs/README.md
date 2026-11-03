# Vendored QuickJS

For contributors changing the JavaScript engine. Upstream is Bellard QuickJS,
commit `04be246001599f5995fa2f2d8c91a0f198d3f34c` (the 2026-06-04 release
line). Only the files the engine build needs are kept; upstream's tests,
tools, and documentation are not.

The tree already carries every change listed below, so an ordinary build
fetches and patches nothing and an engine change is an ordinary diff.
`cmake/TilefinchDependencies.cmake` pins the SHA-256 of `quickjs.c` and
`quickjs.h` and one combined fingerprint of the other sources whose change can
alter compiled bytecode (`quickjs-atom.h`, `quickjs-opcode.h`,
`libregexp.c`/`.h`, `libregexp-opcode.h`, `libunicode.c`/`.h`,
`libunicode-table.h`, `dtoa.c`/`.h`). A mismatch fails configure; an
intentional change updates the pins in the same commit (the error prints the
new values). `TILEFINCH_QUICKJS_ENGINE_ID`, which keys the persistent
compiled-script tier and lazy bundle records, hashes the compiled `quickjs.c`
with that fingerprint, so any engine change rebuilds the tier. Retrain the PSP
QuickJS PGO profile before releasing an engine change.

Upstream fixes carried individually: `c8ef88981b` (`Atomics.store`
out-of-bounds value release), `817509a87c` (Unicode script-range allocation
refusal), `79f58c104c` (truncated BigInt limb release), `5febaaa49f`
(inherited-property write releases its value when lazy initialization is
refused), `ef5c6e356c` (signed BigInt decrement on 64-bit limbs),
`e8b69194b3` (bound captured-variable indices and propagate compiler failures),
and `c530305c0f` (balance symbol coercion when a property key is a built-in atom).
`tilefinch-quickjs-read-refusal-tests` covers the two read-path fixes.

## Changes carried

Patch files live in the repository-root `patches/` directory as
`bellard-quickjs-04be246-<id>.patch`. They are the inputs of the
[lab variants](#lab-variants); keep them while the variants use them. "Test"
names the CTest target; "—" means the change is covered only through the
browser's own suites. `quickjs-oom` stands for `tilefinch-quickjs-oom-tests`,
whose `--…-only` modes are named where they exist.

### Bounded baseline

These changes sit below the patch layers: reversing every layer reaches the
pinned baseline fingerprint, which already contains them.

| Id | Patch file | Behaviour | Test |
|---|---|---|---|
| `oom-backtrace` | yes | Roots the current exception while its backtrace is built, so an out-of-memory during stack capture cannot free it. | `quickjs-oom` |
| `compile-interrupt` | yes | The watchdog interrupt handler can abort an oversized compile at a bounded token cadence. | `tilefinch-quickjs-lazy-function-tests` |
| `bounded-array-growth` | yes | Caps dense-array spare capacity at 128 KiB once the value buffer reaches 512 KiB. | `tilefinch-fast-array-growth-tests` |
| `module-bytecode-restore` | baseline | A module whose bytecode read or import resolution fails is left unregistered, as a failed source compile is, so the module cache can fall back to source. | `quickjs-oom` |
| `lazy-functions` | baseline | `JS_SetLazyFunctionThreshold`: long inner functions are parsed for early errors and closure variables but compiled at first call; a first call refused for memory fails uncatchably and stays lazy, so a retry loop cannot recompile forever. | `tilefinch-quickjs-lazy-function-tests` |
| `preparse` | baseline | `JS_SetLazyFunctionPreparse` (default on): a lazy body is validated by a preparser that builds nothing, and anything it does not model falls back to the parser, so every early error is still reported at compile time. | `tilefinch-quickjs-lazy-function-tests` |
| `set-prototype-refusal` | baseline | A refused copy-on-write shape releases the proposed prototype and leaves the old one. | `quickjs-oom --set-prototype-only` |
| `autoinit-retry` | baseline | A lazily initialized property keeps its autoinit record when initialization is refused, so a later access retries instead of reading `undefined`. | `quickjs-oom --global-autoinit-only`, `--inherited-autoinit-only` |
| `global-autoinit-release` | baseline | A refused global-variable reference releases the newly created built-in. | `quickjs-oom --global-autoinit-only` |
| `single-capture-closure` | baseline | A closure with exactly one capture and no home object stores it in the home-object slot. | `quickjs-oom --capture-refusal-only` |
| `function-metadata-cache` | baseline | Common `length`/`name` pairs share immutable per-realm property tables; author changes detach first. | `quickjs-oom --metadata-refusal-only` |
| `small-source-copy` | baseline | Function source spans of at most 256 bytes keep a private copy rather than pinning a shared backing. | `quickjs-oom --small-source-only` |
| `shared-source-spans` | baseline | Nested functions share one refcounted immutable source backing; bytecode stores offsets into the enclosing function's source. | `quickjs-oom` |
| `bound-function-refusal` | baseline | Marking and finalizing a bound function tolerate its not-yet-allocated arguments. | `quickjs-oom --bind-refusal-only` |
| `context-refusal` | baseline | A context reaches the runtime lists only after its tables exist; a refused global variable table returns cleanly. | `quickjs-oom --context-refusal-only` |
| `regexp-refusal` | baseline | The regexp compiler stops at a refused bytecode growth instead of patching a jump past the buffer. | `quickjs-oom` |
| `parse-oom-reporting` | baseline | A parse whose bytecode buffer lost an allocation, or that has thrown out-of-memory, reports "out of memory" rather than a SyntaxError. | `quickjs-oom` |
| `compile-refusal` | baseline | Function creation, label resolution, scopes, constant pools and the atom table stop cleanly at the first refused allocation. | `quickjs-oom` |
| `near-limit-gc-rearm` | baseline | Automatic GC re-arms at half the remaining headroom near a finite limit; large `toString()` snapshots check it before copying. | `quickjs-oom` |
| `source-snapshot-ropes` | baseline | Whole-owner `toString()` snapshots share their backing, and large non-ASCII ones are built as codepoint-aligned ropes. | `quickjs-oom` |
| `dense-join-presize` | baseline | `join` of a complete dense array of flat strings is pre-sized; up to 64 string or rope entries use rope concatenation. | `quickjs-oom` |
| `ascii-identifier-intern` | baseline | Plain ASCII identifiers are interned straight from their source span. | `tilefinch-quickjs-lazy-function-tests` |
| `property-creation` | baseline | Closures get their final shape directly; array spread, ordinary class fields and ordinary descriptor objects take direct paths; function creation fails on a refused `length`, `name` or `prototype`. | `tilefinch-quickjs-property-creation-tests` |
| `native-calls` | baseline | Calls from bytecode enter a C function without a bytecode frame; integer `charCodeAt`, one-unit `fromCharCode` and `Math.imul` skip general conversions. | `tilefinch-quickjs-native-call-tests` |
| `add-in-place` | baseline | `OP_add` appends in place to a flat string whose only other reference is the local the next opcode overwrites. | `quickjs-oom` |
| `locale-date-shape` | baseline | `toLocale*String` on dates produce browser en-US shapes, matching the default `Intl.DateTimeFormat`. | `tilefinch-web-conformance-tests` |
| `platform-methods` | baseline | `JS_READ_OBJ_PLATFORM_METHODS`: lowercase plain functions in embedder bytecode get no `prototype` and are not constructors, like WebIDL operations. | `tilefinch-web-conformance-tests` |
| `cleanup-jobs` | baseline | FinalizationRegistry callbacks use a runtime-owned job list drained by `JS_ExecutePendingCleanupJob`; retired realms never run them. | `tilefinch-browser-engine-tests` |
| `proxy-brand` | baseline | Trap-free `JS_IsProxy`, used by structured clone to reject a Proxy before reflecting over it. | — |
| `stack-active` | baseline | `JS_GetStackTop` and `JS_IsStackActive`, so the host runs a microtask checkpoint only with an empty script stack. | — |
| `stack-frame-info` | baseline | Allocation-free `JS_GetStackFrameInfo` for the interrupt-time sampling profiler. | — |
| `stack-frame-origin` | baseline | `JS_GetStackFrameOrigin` classifies a frame without decoding the line table. | — |
| `gc-hook` | baseline | `JS_SetGCHook` brackets every collection except the final one. | — |
| `gc-pacing-hook` | baseline | `JS_SetGCPacingHook`, `JS_GetGCThreshold` and `JS_GetGCCause` let the embedder choose where collection re-arms. | — |
| `lazy-compile-hook` | baseline | `JS_SetLazyCompileHook` brackets every first-call compile of a lazy body. | — |
| `weakref-deref` | baseline | `JS_WeakRefDeref` reads a WeakRef's target from C. | — |
| `work-counters` | baseline | `JS_GetWorkCounters`: interrupt budget, collections and lazy-compile totals; opt-in builds add entry and opcode counts. | `tilefinch-quickjs-native-call-tests` |
| `opcode-histogram` | baseline | `JS_GetOpcodeCounts` in a `CONFIG_TILEFINCH_OP_COUNTS` engine; the default build returns none. | — |
| `execution-census` | baseline | `CONFIG_TILEFINCH_EXECUTION_CENSUS` (host and validation builds) tags opcodes, helpers and natives for a sampling thread; ordinary PSP builds omit it. | — |
| `arena-census` | baseline | The explicit memory census reports arena capacity and occupied block bytes. | — |
| `test-refuse-allocation` | baseline | Host-only `JS_TestRefuseAllocation(n)` refuses the Nth allocation, reaching the engine's own arenas. | `tilefinch-js-responsiveness-tests` |

### Patch layers, in application order

| Id | Behaviour | Test |
|---|---|---|
| `property-fault-trace` | Lab only (`PSP_BROWSER_JS_PROPERTY_FAULT_TRACE`): records the first property-access faults with function and line. Applied first, and only when that option is on. | `tilefinch-psp-sdk-quickjs-variant-tests` |
| `capture-getter-fastpath` | A trivial closure getter returns its captured value without an interpreter frame (`PSP_BROWSER_QUICKJS_CAPTURE_GETTER_FASTPATH`, default ON). | `tilefinch-quickjs-variant-control-tests` |
| `dynamic-code-policy` | `JS_SetDynamicCodeEnabled`, the engine gate for CSP `unsafe-eval`. | `tilefinch-content-security-policy-tests` |
| `realm-retirement` | `JS_RetireContext` and `JS_SetGlobalThis` for retired worker and frame realms and the WindowProxy receiver. | `quickjs-oom` |
| `retired-async-state` | Resuming a retired async function or generator throws instead of returning through a frame never entered. | `quickjs-oom` |
| `scope-oom` | A scope resolution refused for memory is not treated as a bytecode cursor. | `quickjs-oom` |
| `latin1-string` | The bounded Latin-1 string constructors the host's decoders use. | — |
| `repeat-rope-eval` | Large `String.prototype.repeat` results are ropes, and eval compacts a rope prefix. | `quickjs-oom` |
| `single-char-string-buffer` | A one-code-unit `StringBuffer` result shares the one-character cache. | `quickjs-oom` |
| `compact-char-array` | Dense arrays of one-character Latin-1 strings are packed (`PSP_BROWSER_QUICKJS_COMPACT_CHAR_ARRAY`, default ON). | `quickjs-oom` |
| `compact-char-array-cow` | Copy-on-write for packed character arrays (same option). | `quickjs-oom` |
| `repeat-rope-eval-scan` | Memoized prefix scanning keeps repeated-rope eval proportional to rope depth. | `quickjs-oom` |
| `host-get-code-for-eval` | `JS_SetHostGetCodeForEval`, the HTML HostGetCodeForEval hook. | `tilefinch-tests` |
| `near-limit-array-growth` | Realloc-only array growth follows GC pacing, and near the limit keeps a bounded share of headroom instead of failing. | `quickjs-oom` |
| `array-length-shrink` | Reducing a sparse Array's `length` cannot leave configurable indexed properties behind. | `quickjs-oom` |
| `compact-byte-array` | Dense arrays of byte or 16-bit integers are packed (compact option). | `quickjs-oom` |
| `compact-array-repack` | Stores into packed arrays stay packed, and a deoptimized numeric array is repacked at power-of-two lengths (compact option). | `quickjs-oom` |
| `native-string-gc` | Native Latin-1 producers and string-buffer growth get the paced pre-allocation collection; large UTF-8 is sized before allocation. | `quickjs-oom --string-buffer-growth-only` |
| `float-array-indexed-store` | Numeric writes to in-bounds Float32Array/Float64Array run in the interpreter, int32-to-float32 without a double. | `quickjs-oom` |
| `heap-detail` | Host-only `JS_ComputeMemoryDetail` (`CONFIG_TILEFINCH_HEAP_DETAIL`) heap walk for the lab's heap census. | — |
| `json-direct` | `JSON.parse` tokenizes the string in place instead of a UTF-8 copy, with identical results and errors. | `tilefinch-quickjs-json-parse-tests` |
| `external-source` | `JS_SetExternalSourceHooks`: a stripped script's functions record offsets into embedder-held text for `toString()` and lazy compiles. | `tilefinch-quickjs-external-source-tests` |

## Lab variants

Turning `PSP_BROWSER_QUICKJS_CAPTURE_GETTER_FASTPATH` or
`PSP_BROWSER_QUICKJS_COMPACT_CHAR_ARRAY` off, or
`PSP_BROWSER_JS_PROPERTY_FAULT_TRACE` on, copies `quickjs.c` and `quickjs.h`
into the binary directory, reverses the patch layers down to the bounded
baseline (fingerprint checked), re-applies them with the chosen options in
the order above, and checks the result against the fingerprint pinned for
that combination. The vendored tree is never modified. Variant pins cover only
`quickjs.c`, so they move whenever `quickjs.c` or a layer does; a change to the
baseline moves the baseline pin and every variant pin.

## Updating upstream

Fetch the new upstream tree, port the changes above, copy the engine files
here, regenerate the patch layers against the new baseline, and update every
pin. Keep both tables in step with what the tree and `cmake/` contain.
