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
`c530305c0f` (balance symbol coercion when a property key is a built-in atom),
and `813636e7af` (retire external ArrayBuffer cleanup ownership after detach
or transfer). Transfers also preserve borrowed buffers with a NULL callback;
`tilefinch-quickjs-array-buffer-owner-tests` covers release order and refusal.
The engine-bounds layers backport `2519774403` (checked array allocation
and growth) and `a6da4a30d3` (bounded lexical scopes and parser unwind),
retaining the device-specific growth policy. Module restore publishes GC-visible
counts only after their storage exists. `tilefinch-quickjs-oom-tests` covers
word-size boundaries, bounded parser refusal and subsequent ordinary parsing.

The allocation-safety layer carries the applicable ownership part of
`094306131a` (`Promise.withResolvers` cleanup), transactional array appends
from `b25da59246` adapted to compact storage, and the proxy-key length guard
from `e25861ca4c` with an additional 32-bit byte-overflow bound.
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
| `platform-receiver` | baseline | `JS_MarkPlatformInterfaces`, `JS_MarkPlatformInterface`, `JS_SetPlatformMember`: two object flags mark Web IDL interface prototype objects and their member functions; calling a marked member with a receiver that does not have the member's interface prototype on its chain throws "Illegal invocation" (a 10-bit interface id per prototype and member, taken from the weak reference count's word in the `interface-snapshot` layer's patch, which leaves a 14-bit count with a checked limit; a member several interfaces share takes the nearest interface they inherit from, `JS_SetPlatformMemberOf` marks one for a given interface; the embedder's own code keeps the any-interface check; `JS_MarkPlatformLayer` marks an embedder object as a platform layer, with one more bit from the same word, and a receiver under one passes, call by call, for a member the layer held when marked and still holds). The marking pass also defines each interface prototype's `@@toStringTag`. `JS_SnapshotPlatformInterfaces` records what the interfaces hold, so that a later pass marks only what embedder script added since. | `tilefinch-foundation-tests`, `tilefinch-module-bytecode-tests` |
| `property-creation` | baseline | Closures get their final shape directly; array spread, ordinary class fields and ordinary descriptor objects take direct paths; function creation fails on a refused `length`, `name` or `prototype`. | `tilefinch-quickjs-property-creation-tests` |
| `native-calls` | baseline | Calls from bytecode enter a C function without a bytecode frame; integer `charCodeAt`, one-unit `fromCharCode` and `Math.imul` skip general conversions. | `tilefinch-quickjs-native-call-tests` |
| `add-in-place` | baseline | `OP_add` appends in place to a flat string whose only other reference is the local the next opcode overwrites. | `quickjs-oom` |
| `locale-date-shape` | baseline | `toLocale*String` on dates produce browser en-US shapes, matching the default `Intl.DateTimeFormat`. | `tilefinch-web-conformance-tests` |
| `required-arguments` | baseline (the field shares the `interface-snapshot` layer's word) | `JS_SetRequiredArguments`: a function's own `length` becomes the fewest arguments a call or construction needs; fewer throws Chrome's Web IDL TypeError ("Failed to execute 'f' on 'Interface': N argument(s) required, but only M present.") after the receiver check, before the function runs. A 4-bit field taken from the weak reference count's word. `JS_SetPromiseOperation` (one more bit from that word): a promise-returning operation's failed receiver check, where an undefined or null receiver fails too, and argument count return a promise rejected with the TypeError instead; a marked async function gets the receiver check. | `tilefinch-web-platform-conformance-tests` |
| `platform-methods` | baseline | `JS_READ_OBJ_PLATFORM_METHODS`: lowercase plain functions in embedder bytecode get no `prototype` and are not constructors, like WebIDL operations. | `tilefinch-web-conformance-tests` |
| `reference-proxy-has` | baseline | A global variable lookup that reaches a Proxy on the global object's prototype chain asks its `has` trap first, as an object environment's HasBinding does, and throws ReferenceError when the name is absent instead of returning the `get` trap's value. | `tilefinch-web-platform-conformance-tests` |
| `pinned-atoms` | baseline | `JS_PinAtomTable` interns an embedder atom table at `JS_ATOM_END` and up, first in a runtime and for its lifetime; `JS_WRITE_OBJ_PINNED_ATOMS` names those atoms by index (a marked version byte with the table's count and hash), so `JS_READ_OBJ_ROM_DATA` bytecode with no other atoms runs in place instead of copying its code to relocate atoms. A runtime without the same table refuses such bytecode. | `tilefinch-quickjs-pinned-atoms-tests` |
| `cleanup-jobs` | baseline | FinalizationRegistry callbacks use a runtime-owned job list drained by `JS_ExecutePendingCleanupJob`; retired realms never run them. | `tilefinch-browser-engine-tests` |
| `proxy-brand` | baseline | Trap-free `JS_IsProxy`, used by structured clone to reject a Proxy before reflecting over it. | — |
| `stack-active` | baseline | `JS_GetStackTop` and `JS_IsStackActive`, so the host runs a microtask checkpoint only with an empty script stack. | — |
| `stack-frame-info` | baseline | Allocation-free `JS_GetStackFrameInfo` for the interrupt-time sampling profiler. | — |
| `stack-frame-origin` | baseline | `JS_GetStackFrameOrigin` classifies a frame without decoding the line table. | — |
| `gc-hook` | baseline | `JS_SetGCHook` brackets every collection except the final one. | — |
| `gc-pacing-hook` | baseline | `JS_SetGCPacingHook`, `JS_GetGCThreshold` and `JS_GetGCCause` let the embedder choose where collection re-arms. | — |
| `lazy-compile-hook` | baseline | `JS_SetLazyCompileHook` brackets every first-call compile of a lazy body. | — |
| `weak-targets` | baseline | `JS_NewWeakTarget`, `JS_WeakTargetDeref` and `JS_FreeWeakTarget`: a weak reference the embedder stores without a WeakRef object (the DOM handle table). | `tilefinch-js-responsiveness-tests` |
| `weak-target-state` | baseline | `JS_WeakTargetCarriesState`: whether another weak holder or an own property outside the embedder's list makes a weak target's death visible to script, without allocating (the DOM handle table's collection hold). | `tilefinch-js-responsiveness-tests --stateful-wrappers-only` |
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
| `allocation-safety` | Promise result publication retains ownership through refusal; array append publishes length only after storage commits; proxy key tables reject lengths that cannot fit a 32-bit allocation. The noncompact lab variant uses the equivalent `allocation-safety-ordinary` patch. | `tilefinch-quickjs-read-refusal-tests` |
| `compact-line-tables` | Each function's bytecode-to-source table (pc2line) uses a denser lossless entry encoding: same-line steps in one or two bytes, a step to the next line with its column. Reported lines and columns are those of upstream's encoding; tables take about 73% of upstream's bytes on minified page code. | `tilefinch-quickjs-line-table-tests` |
| `interface-snapshot` | `JS_SnapshotInterfaces` marks the interface objects (class prototypes, global functions, their prototypes and methods; `JS_SnapshotObject` for a namespace). The first change by code other than the embedder's copies a marked object's own properties and prototype to a twin, in which embedder code (bytecode read with `JS_READ_OBJ_PLATFORM_METHODS`, or compiled with `JS_EVAL_FLAG_TRUSTED`, which covers the functions the script defines and not code it compiles at run time) continues property reads, inherited-setter checks, `in`, for-in and own-property reflection; its own changes reach both, except to a member or the prototype author code has changed since, or a new member or prototype of an object author code made non-extensible, which are made in the twin alone. Global variables holding an object are pinned the same way: author code that names, assigns, declares, redefines or deletes one gets a variable reference of its own, and embedder closures keep theirs; an accessor global embedder code defined that author code redefines or deletes is kept for embedder code (`JS_PlanInterfaceSnapshot` before any embedder code is created in the realm; `JS_LeaveGlobalToPage` exempts one). A function author code puts on an object or defines in a subclass does not shadow a member embedder code defined further up the prototype chain when embedder code reads it (ECMAScript's own prototypes excepted). An embedder write to an ECMAScript prototype is the embedder's own only inside `JS_EnterBrowserCode` or outside any script; otherwise it is copied on like author code's. `JS_MarkBrowserNative` and `JS_EnterBrowserCode` make an embedder native embedder code whoever calls it; `JS_MarkBrowserFunction` (test builds) does the same for a script function; `JS_SetEmbedderPageView` gives frameless embedder code the page's view. A promise job's frameless work, and an async function's or async generator's work outside its frame, act for the code they run for, while a realm of the runtime plans a snapshot. | `tilefinch-quickjs-interface-snapshot-tests` |
| `class-field-position` | A class's field-initializer function starts at the token inside the class that first needs it, not at offset 0, so its line table no longer walks the line/column cache back to the start of the file and forward again for each class with fields. Reported positions are unchanged. | `tilefinch-quickjs-class-field-position-tests` |
| `backtrace-pending-exception` | Building an error's stack text keeps the pending exception and its uncatchable mark: a refused allocation there throws, and it used to replace an interrupt or a lazy compile refused for memory with a catchable "out of memory", so page code that caught it and tried again ran on at the heap wall. A refused text leaves `stack` null. | `quickjs-oom --backtrace-refusal-only` |
| `weak-reference-cap` | Refuses a new weak reference to an object at the existing 14-bit limit, without growing the object header. Existing references always release normally, and a failed map insertion or finalization registration leaves no partial reference. | `tilefinch-quickjs-interface-snapshot-tests` |
| `map-computed-insert` | If a `getOrInsertComputed` callback inserts the same key in a Map or WeakMap, update that record in place, preserving iteration order and avoiding a redundant allocation. | `tilefinch-quickjs-interface-snapshot-tests` |
| `author-code-running` | `JS_IsAuthorCodeRunning` tells an embedder native whether author code's frames are anywhere on the stack, whatever browser code, `JS_EnterBrowserCode` bracket (the JS profiler's trampolines) or job view lies between, so the browser can tell a page's own read from its own work. | `tilefinch-quickjs-interface-snapshot-tests` |
| `size-regions` | In the PSP build, where the engine is otherwise `-O2`, rarely run built-ins compile for size (`TILEFINCH_SIZE_REGION_BEGIN`/`_END`, GCC's `optimize("Os")` pragma, active only in an optimizing PSP compile that is not already `-Os`; other compilers, Clang host builds among them, are unaffected): Iterator (the constructor, `Iterator.concat` and `Iterator.from`, the wrapper and helper objects, the prototype methods, and `Iterator.prototype`'s `[Symbol.iterator]` and `toStringTag`); the URI functions, `escape` and `unescape`; Date (the constructor, methods, parsing, formatting and `JS_NewDate`); BigInt (the constructor, `toString`, `valueOf`, `asIntN` and `asUintN`); Atomics, WeakRef and FinalizationRegistry. The interpreter, the parser, the other built-ins (Proxy and Reflect among them, which reactive frameworks run on every property read and write), the weak-target calls the DOM bindings make, and every `JS_AddIntrinsic*` function (they run for each new context: frames, workers) keep `-O2`. Code only: behaviour and the bytecode format are unchanged. | — |
| `atomics-no-block` | No agent may suspend: `Atomics.wait` checks and converts its arguments, then throws "cannot block in this thread" (the TypeError the specification gives when AgentCanSuspend() is false), and `Atomics.notify`, with nobody waiting, returns 0 after its own checks. Upstream's waiter list, its mutex and condition variables, and `JS_SetCanBlock`, which nothing called, are gone. | `tilefinch-quickjs-atomics-tests` |
| `bootstrap-unused-locals` | `JS_WRITE_OBJ_STRIP_UNUSED_LOCALS`, which only the bootstrap generator passes: a function whose arguments and locals nothing captures (`var_ref_count` 0: no closure, mapped `arguments` or reference object reads them through a variable reference) and whose table names none of them (compiled with `JS_STRIP_DEBUG`, so without direct eval) is written with no variable table, as a function with no locals is. Nothing reads such a table: the closure, `OP_close_loc`, mapped-arguments and reference paths need a captured local, direct eval needs the names, and the lexical-variable error, the memory census and the free path already accept a missing table. The writer also scans the function's code and keeps the table if any opcode that reaches one of those readers appears (`close_loc`, `make_loc_ref`, `make_arg_ref`, `eval`, `apply_eval`, or `special_object` building mapped `arguments` for declared arguments), so a later engine path that sets one of them without a captured local cannot strip a table it needs. Each realm then holds 12 bytes less per argument and local of such a function; its frame slots are unchanged. The reader refuses ("invalid function variables") a function record whose table length is neither zero nor its argument and local count, or that has no table while `var_ref_count` says a local is captured. Compilation and every other writer are unchanged, so page scripts and their cached bytecode are as before. | `tilefinch-quickjs-unused-locals-tests` |

## Code compiled for size

In the PSP build, which compiles the engine at `-O2`, the code pages seldom
run compiles at `-Os`. The pragmas below act only in an optimizing PSP
compile that is not already `-Os` (a `-O0` build stays unoptimized), and
`PSP_BROWSER_QUICKJS_OPT_FLAGS` does not override them or `libunicode.c`'s
option. Other compilers, Clang host builds among them, are unaffected.

- In `quickjs.c`, the regions the `size-regions` layer marks (above).
- In `libregexp.c`, the compiler (parsing, character classes and code
  emission, from `dbuf_insert` through `lre_compile`), behind GCC's
  `optimize("Os")` pragma. The matcher, `lre_exec`, keeps the engine's flags. The
  pragma is part of the bytecode-source fingerprint, so it moved the
  engine ID.
- `libunicode.c` as a whole, by a per-source compile option in
  `cmake/TilefinchDependencies.cmake`. Case conversion and the `/i`
  matcher call into it for every character (`lre_case_conv`,
  `lre_canonicalize`); the ASCII branch at the top of each compiles to the
  same few instructions at either level, and the tables behind the
  non-ASCII paths are where `-Os` saves.

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
