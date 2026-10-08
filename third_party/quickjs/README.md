# Vendored QuickJS

Upstream: Bellard QuickJS, commit `04be246001599f5995fa2f2d8c91a0f198d3f34c`
(the 2026-06-04 release line). Only the files the engine build needs are
kept; upstream's tests, tools, and documentation are not.

The 2026-10-01 release sweep also carries upstream commit `c8ef88981b`'s
`Atomics.store` out-of-bounds error-path value release. Other subsequent
upstream changes remain under review rather than being imported wholesale
into the bounded PSP engine.

The tree already carries this repository's engine changes. It replaces the
earlier configure-time flow that fetched the pristine tarball and applied
seventeen layered patches with marker detection and reverse-apply steps.
That flow failed three times in one week (a shared source directory let an
experimental patch reach release binaries, a fix had to be shipped as a
patch on a patch, and a validation rewrite rejected already-patched trees),
and every engine change was reviewable only as a patch file. Now an engine
change is an ordinary diff.

`cmake/TilefinchDependencies.cmake` pins the SHA-256 of `quickjs.c` and
`quickjs.h`, and one combined fingerprint of the other sources whose
change can alter compiled bytecode (`quickjs-atom.h`, `quickjs-opcode.h`,
`libregexp.c`/`.h`, `libregexp-opcode.h`, `libunicode.c`/`.h`,
`libunicode-table.h`, `dtoa.c`/`.h`: regexp literals are compiled at parse
time and their programs are serialized into cached bytecode). A mismatch
fails configure. An intentional engine change updates the pins in the same
commit (the error prints the new values). `TILEFINCH_QUICKJS_ENGINE_ID`,
which keys the persistent compiled-script tier and lazy bundle records,
hashes the compiled `quickjs.c` together with that combined fingerprint.
The lab variants' pins cover only their `quickjs.c` (the other sources are
copied unchanged), so they move only when `quickjs.c` or a patch does.

Host and validation builds can also enable an independent execution census
(`CONFIG_TILEFINCH_EXECUTION_CENSUS`). Its C-only API enables per-runtime
opcode/helper/native-address tags; scoped helper tags restore through
reentry and exceptions. The embedder samples those tags on a separate thread,
not from VM interrupt polls. Detailed mode retains function identities across
nested calls and exceptions and classifies property/call paths. Diagnostic
function fields are not serialized and do not change the bytecode ABI.
An additional, default-off `CONFIG_TILEFINCH_REFCOUNT_CENSUS` build counts
value API retains/releases; its timings are intentionally not comparable.
Ordinary PSP builds omit all tags and counters entirely.
Frame census bins distinguish bytecode setup, argument copying, local
initialization and teardown; teardown owns its release helpers while nested
calls retain independent attribution. The additional simple-frame counter
is an eligibility count, not a timing or a skipped lifetime check.
The [harness guide](../../docs/engineering/INPUT_SCRIPT_HARNESS.md#independent-execution-census)
defines the CPU/wall-time denominators, refusal/overflow reporting and the
mandatory sampler-off comparison: the validation tags have a measurable
Allegrex cost. This is diagnostic evidence, not a new author-visible API or
an optimization of the interpreter.

Bound-function finalization and marking tolerate a not-yet-created payload:
the object enters the runtime before its separately allocated bound arguments.
An allocation refusal or collection in that interval must not dereference NULL.
`test_quickjs_oom --bind-refusal-only` refuses each allocation in turn, checks
the exception, and verifies that the first successful bind remains callable.

Numeric primitive indexed writes to in-bounds Float32Array/Float64Array views
run directly in the interpreter. Int32-to-float32 writes avoid an unnecessary
intermediate double conversion on soft-float targets. Non-numeric values and
out-of-bounds indices retain the full property path, including coercion before
the bounds check; proxies are never admitted. Detached and resized views use
the engine's existing synchronously updated element count. The optimized-host
parity fixture covers byte representations, reentrant coercion, detach/resize,
fixed out-of-bounds views, non-extensible views, and proxies.
The lab variant reconstruction carries this path as an unconditional layer;
its reverse/apply hashes are checked for all seven control combinations.

## Changes carried, in application order over upstream

The original patch files stay under `patches/` as history and as inputs to
the lab variants below. Do not delete those inputs until the variants no
longer depend on them.

1. `oom-backtrace` — root the current exception while building its backtrace
   so an out-of-memory during stack capture cannot free it.
2. `compile-interrupt` — let the watchdog interrupt handler abort oversized
   compiles at a bounded token cadence.
3. `bounded-array-growth` — cap dense-array spare capacity at 128 KiB once
   the value buffer reaches 512 KiB.
4. `capture-getter-fastpath` — return a trivial closure getter's captured
   value without an interpreter frame (option
   `PSP_BROWSER_QUICKJS_CAPTURE_GETTER_FASTPATH`, default ON). The vendored
   path also admits C API calls with `COPY_ARGV`: the two-opcode getter
   never reads or modifies arguments. Constructor and generator flags retain
   the ordinary path, and the entry interrupt/realm checks still run.
   Interpreter field reads call an already-found accessor without repeating
   its prototype lookup; the getter is retained across self-deletion and
   receives the original receiver. Exotic lookup and lazy property
   initialization continue through the general property path.
5. `dynamic-code-policy` — `JS_SetDynamicCodeEnabled`, the engine-level
   CSP `unsafe-eval` gate.
6. `realm-retirement` — `JS_RetireContext` and `JS_SetGlobalThis` for
   retired worker and frame realms and the WindowProxy receiver.
7. `retired-async-state` — retired async and generator resumes throw
   instead of returning through a frame that was never entered.
8. `scope-oom` — a failed scope resolution under memory pressure is not a
   bytecode cursor.
9. `latin1-string` — the bounded Latin-1 string constructor used by the
   host's decoders.
10. `repeat-rope-eval` — large `String.prototype.repeat` results and
    rope-aware eval prefix compaction.
11. `repeat-rope-eval-scan` — memoized horizontal-prefix scanning and direct
    suffix seeking keep repeated-rope eval proportional to rope depth while
    retaining the logical scan and interrupt bounds.
12. `single-char-string-buffer` — single-code-unit `StringBuffer` results
    share the immutable one-character cache.
13. `compact-char-array`, `compact-char-array-cow`, and `compact-byte-array`
    — pack dense arrays of one-character Latin-1 strings, signed/unsigned
    byte integers, or signed 16-bit integers, with copy-on-write for character
    arrays (option
    `PSP_BROWSER_QUICKJS_COMPACT_CHAR_ARRAY`, default ON).
14. `array-length-shrink` — keep examining the current property-table slot
    when deletion compacts a sparse Array's shape, so reducing `length`
    cannot leave configurable indexed properties behind.
15. `native-string-gc` — give bounded native Latin-1 producers the same
    pre-allocation cycle-collection opportunity as the engine's other large
    string builders.
16. `module-bytecode-restore` (not a patch layer; part of the bounded
    baseline) — a module read from bytecode no longer outlives a failure.
    `JS_ReadObject` released only one of the two references a half-read
    module holds, leaving it registered under its name (with element counts
    whose tables were never allocated, which the GC mark and finalizer walk).
    `JS_ResolveModule` left a restored module whose import could not load
    registered as resolved with a missing dependency. Both now leave the
    module where a failed source compile does: unregistered, so a later
    import loads it again. The browser's in-memory module bytecode cache
    depends on this for its fallback to source.

17. `lazy-functions` (not a patch layer) — `JS_SetLazyFunctionThreshold`:
    while it is non-zero, a script or module keeps inner functions whose
    source text is at least that long without bytecode until their first
    call. The whole script is still parsed, so early errors are reported at
    compile time, and the kept function's variables (and those of the
    functions nested in it) are resolved as usual, which decides its closure
    variables and what the enclosing functions capture; only the later
    passes and the bytecode object are skipped. The stub keeps its closure
    variables, name, length, flags, definition position and source text
    (`Function.prototype.toString()` text, or a private copy when the source
    is stripped; a stripped function whose text is longer than its
    unresolved bytecode stays eager). On the first call the body is parsed
    again alone under a synthetic parent that reproduces the enclosing
    parser state and offers the stub's closure variables by name (the
    direct-eval mechanism), so it compiles to the same closure layout and is
    moved into the stub. A first-call compile stopped by the native stack
    limit or the interrupt handler fails the call as execution would (stack
    overflow, uncatchable interrupt) and leaves the function lazy. One
    refused for lack of memory (`JS_ThrowOutOfMemory` was reached during the
    compile) also fails uncatchably, as "out of memory", and leaves the
    function lazy: the page sees only a call fail, and a page that catches
    and retries it (React's render loop retries a unit of work that threw)
    would otherwise recompile the body on every turn, each attempt refused
    again, until the script watchdog stopped it. Counted in
    `JSLazyFunctionStats.memory_failures`; the lab-variant baseline and
    variant pins moved with this change. Functions
    near a direct eval or inside `with`, class constructors, field
    initializers and static blocks are always compiled eagerly. Two
    resolution details keep the captured set exact: assigning an enclosing
    constant captures it (the error is thrown from the body later), and a
    captured private name records its kind. Serialized bytecode keeps stubs
    as stubs (a flag bit in the function tag, so format 6 is unchanged for
    everything compiled eagerly). A stub reuses its empty `byte_code_buf`
    for its lazy state, so eagerly compiled functions are no larger.
    `tests/test_quickjs_lazy_functions.c` runs every case lazily and
    eagerly.

The regexp compiler (`libregexp.c`) stops at a refused bytecode growth
before patching a jump offset. Upstream emitted a split or goto, then wrote
its offset back at the returned position without checking the buffer's
error flag. After a refused realloc that position is at or past the end
of the buffer, and the string-list emitter's goto chain was never written,
so the patch overran the heap or followed garbage links. A
class with properties of strings (`\p{RGI_Emoji}` under `v`) reached it on
a saved Mastodon page at the PSP script-heap ceiling. The string-list
emitter, the alternation patch and the quantifier scan now raise "out of
memory" instead. `tests/test_quickjs_oom.c` sweeps every refusal point of
that compile under a guarded allocator.

The bytecode emitter keeps going after its buffer refuses a growth (the
buffer's error flag is checked when the function is created), so a parse
can read back a lost opcode first: `get_lvalue` then took it for an
invalid assignment target and the compile failed with "SyntaxError:
invalid assignment left-hand side", blaming the page's code for a full
heap (seen on the saved bbc-sport census page at the script-heap ceiling).
`js_parse_error_v` now reports "out of memory" whenever the current
function's bytecode buffer has lost an allocation.
`tests/test_quickjs_oom.c` sweeps a parse of many assignments through its
refusal points and accepts no SyntaxError. The lab-variant baseline and
variant pins moved with this change.

Compiling a function now fails cleanly with "out of memory" (uncatchably
for a lazy body's first call) wherever an allocation is refused, instead
of carrying on with state that no longer matches its buffers. A saved
GitLab census page lazily compiling a large function at a 26-32 MiB page
limit died with SIGBUS in `memmove` under `js_create_function`:
`resolve_labels` keeps writing after its output buffer refuses a growth,
so the addresses it had recorded for relocations and short jumps pointed
past the end, and the short-jump pass moved the code by a negative
length. Its failure path also freed the output and kept the input,
releasing the atoms of instructions the optimizer had already dropped a
second time. It now stops at the first refused growth, keeps the output
and releases the unconsumed input's atoms, and checks the line table. The
same refusal sweep found more of the pattern, all present upstream:
`push_scope` failures (unchecked by nearly every caller) let the matching
`pop_scope` leave the parent scope, so scope -1 was emitted as 65535 and
`has_with_scope` looped forever over memory outside the scope array; a
failed push now unwinds with its pops and marks the bytecode lost. A child
function whose constant-pool slot could not be allocated was stored at
index -1; the variable passes record closure-variable index -1 (as 65535)
when one cannot be added, so `js_create_function`, and the lazy stub's
resolution, now fail on any "out of memory" thrown while they run. The
class constructor, private-brand and `switch` default patches skip a
buffer that lost a write; a label that could not be allocated is not
indexed. An atom table that cannot grow raises "out of memory" (upstream
`JS_NewAtomStr` returned the null atom silently, "XXX: should generate an
exception"), and `js_parse_error_v` reports "out of memory" for any parse
that has thrown it: the `for (...)` lookahead scan ignores a token it
could not make and misread a three-part loop as a for-in/of.
`tests/test_quickjs_oom.c` sweeps every refusal point of a lazy
first-call compile and of an eager script compile of a large function
(every later request refused, or only one; all sizes, or only requests of
256 or 4096 bytes and more) and of loop heads with new names, under the
guarded allocator, and requires an honest "out of memory" or a correct
result, intact guards and no leaks. The lab-variant baseline and variant
pins moved with this change.

Plain, unescaped ASCII identifiers are interned directly from their source
span. An existing atom needs neither a temporary identifier copy nor a second
ASCII scan. Escaped, non-ASCII and private names keep the original parser in
a cold helper; the fast scanner consumes nothing before falling back. This
does not retain source pointers or change bytecode, lazy-function or cache
semantics. The lazy-function lane covers Unicode continuations, escapes,
private names, early errors, long names and identifiers ending at EOF.

The embedder also exposes a trap-free `JS_IsProxy` brand query. HTML
structured clone uses it to reject Proxy values before reflecting over their
prototype or keys; it is not exposed to page JavaScript.

Nested function compilation also shares immutable, overlapping UTF-8 source
spans. A separate refcounted backing (not a parent-function reference) keeps
`Function.prototype.toString()` exact when parents or children die first.
Optional owner-allocation refusal retains the original independent copies.
Bytecode format 6 preserves those spans: a child with the same source owner
stores a checked byte offset into its enclosing function instead of another
source copy. Reading restores a source-only reference, never a function,
context or input-buffer reference. Unrelated sources remain independent;
owner-allocation refusal falls back to a normal admitted copy. This uses one
parent pointer per reader/writer, not a growing source table or substring scan.
Tilefinch's Bellard cache ABI is correspondingly revised; old offline caches
are ignored and their retained author source is recompiled, without reinstall.

Automatic GC also re-arms below a finite allocator limit: once the usual
50% growth would exceed the limit, the next collection uses half the remaining
headroom. This keeps collectible cycles from exhausting a long callback before
the embedder can adjust the threshold at its next safe point. It does not
increase the memory limit or force collections in allocation-free frame loops.
Large `Function.prototype.toString()` snapshots also check this threshold
before copying their source, with the receiver and its immutable backing rooted
by the native call. This avoids refusing a source copy while reclaimable cycles
occupy its headroom; low-level allocation routines do not initiate collection.
ASCII source owners adopt their allocation into an immutable string before
publishing child offsets, so a whole-owner `toString()` snapshot shares its
backing. The owner keeps a reference even if the snapshot becomes a property
key; freeing either the function or snapshot first remains safe. Serialized
bytecode still contains exact source spans, never owner pointers.
Large non-ASCII snapshots use decoded, codepoint-aligned chunks and bounded
rope concatenation rather than widening an entire mostly-ASCII function for
one non-Latin character. Each chunk is sized before allocation. `String(value)`
preserves a returned primitive rope; the boxed constructor and native APIs
that need contiguous storage retain their ordinary flattening paths. Source
contents, coercion order and malformed UTF-8 replacement stay unchanged.

Large joins also pre-size complete dense arrays of flat strings (at most 4,096
entries, at least 8 KiB output). This avoids incremental buffer-growth peaks
and checks GC headroom while the array and separator are rooted. Up to 64
string or rope entries use the existing bounded rope concatenation machinery
instead, sharing immutable text. Accessors, coercible objects, sparse arrays,
locale joins, and larger collections stay on the ordinary path, preserving
their observable evaluation order.

The explicit memory census also reports small-allocation arena capacity and
occupied block bytes. These distinguish arena slack from live allocations;
they do not change the allocator or the enforced heap limit. The general
object census remains approximate (in particular for shared string ropes),
so its difference from malloc usage must not be described as reclaimable memory.

18. `stack-active` (not a patch layer) — `JS_GetStackTop` returns an
    identity for the innermost frame (the profiler uses it to tell a native's
    own checkpoint from script the native called back into), and
    `JS_IsStackActive` reports whether
    any JavaScript or native-function frame is active on the runtime. HTML
    performs a microtask checkpoint only when the JavaScript execution
    context stack is empty; the host checks this before draining jobs, so a
    host path reached from script (an event dispatched by `click()`, a
    module loaded for `import()`) leaves queued microtasks for the outer
    checkpoint instead of running them inside the caller.

19. `stack-frame-info` (not a patch layer) — `JS_GetStackFrameInfo`
    returns a frame's function-name and file atoms and current line and
    column (line -1 marks a native C function frame) without
    allocating, for the host's interrupt-time sampling profiler (validation
    builds log the hottest functions).

20. `gc-hook` (not a patch layer) — `JS_SetGCHook` calls an embedder hook
    before and after every collection (automatic or explicit; not the
    final one in `JS_FreeRuntime`), so the
    profiler can report collection count and time separately instead of
    charging a collection to whichever frame's allocation triggered it.

21. `weakref-deref` (not a patch layer) — `JS_WeakRefDeref` reads a WeakRef's
    target from C without a call, so native DOM getters can return a node's
    cached wrapper.

22. `work-counters` (not a patch layer) — `JS_GetWorkCounters` returns the
    deterministic work of a runtime for the host's `tilefinch-work` record:
    the interrupt budget consumed over every context (each context keeps
    its counter's value at the last reset, so a poll or `JS_FreeContext`
    folds the exact consumption into the runtime), poll and `JS_RunGC`
    collection counts, and the lazy-compile totals. Opt-in engine builds add
    bytecode entry counts (`CONFIG_TILEFINCH_CALL_COUNTS` or
    `CONFIG_TILEFINCH_OP_COUNTS`) and, with `CONFIG_TILEFINCH_OP_COUNTS`
    (`PSP_BROWSER_JS_OP_COUNTS`), an opcode counter in the interpreter
    dispatch and a count of float64-tagged values created by the engine
    (the header's float constructors are wrapped inside `quickjs.c` only).
    The default build compiles neither; interrupt timing is unchanged.

23. `add-in-place` (not a patch layer) — `OP_add` appends to a flat,
    non-atom string in place when its only other reference is the local that
    the next opcode (`put_loc`/`set_loc`, checked and short forms) overwrites
    with the result. Upstream appends in place only through `OP_add_loc`,
    which the compiler emits for `s += constant/local/argument`; a checked
    `let` binding or any other right-hand side (`s += f(x)`) went through
    `OP_add` with the string referenced twice, so each append below the
    8,192-character rope threshold allocated and copied the whole string.
    It relies on the allocator reporting real slack as the usable size
    (Tilefinch's pool reports its size-class capacity).

24. `opcode-histogram` (not a patch layer) — `JS_GetOpcodeCounts` returns
    the per-opcode dispatch counts (with opcode names) of a
    `CONFIG_TILEFINCH_OP_COUNTS` engine; the default build compiles neither
    the 256-entry table nor the names, and returns 0 entries.

25. `preparse` (not a patch layer) — `JS_SetLazyFunctionPreparse` (default
    on): the body of a function that will be kept lazy (change 17) is
    validated, not parsed, when its script compiles
    (`js_preparse_function_body`), by a recursive-descent preparser that
    builds nothing (no atoms for property names, no literal values, no
    bytecode, no nested function definitions). It follows the parser's own
    decisions: token classification by context (strict mode, generator,
    async, class bodies), the `js_parse_skip_parens_token` look-ahead with
    its regexp heuristic (arrows, destructuring, for heads),
    `simple_next_token` peeks, the statement and expression structure, the
    last-opcode rule for assignment targets, and the early-error rules
    (labels and break targets, `??` mixing, `yield`/`await`/`super`/
    `new.target`/`import.meta`/`arguments` contexts, redeclarations,
    accessor arity, class member names, `use strict` with non-simple
    parameters, ...). Regexp literals are compiled, numbers converted and
    escaped or non-ASCII string and template text decoded by the parser's
    functions. It accepts a body only if the parser would: on anything it
    does not model (`eval`, `with`, private names, escapes or non-ASCII
    outside literals, `yield`/`await`/`let`/`static` and other conditionally
    reserved words used as names, sloppy-mode function declarations in
    blocks, repeated parameters, declarations it cannot tell apart from a
    redeclaration, a nested `use strict`, legacy octal escapes, deep
    nesting) or anything invalid, it gives up and the parser parses the
    body, reporting any error itself - so every early error is reported
    when the script compiles, before any of it runs. Declarations are
    tracked with their scopes (functions, parameters with expressions,
    blocks, for heads, catch clauses, classes); the names a body uses but
    does not declare (plus `this`, `arguments`, `new.target` and `super` as
    the parser's pseudo variables unless a nested non-arrow function binds
    them) become one `OP_scope_get_var` each, so resolving the stub gives it
    the closure variables a parse would; a name missing from them could not
    go unnoticed (the first call would fail with "lazy function compiled
    differently" and is retried without skipping). In a first-call compile,
    a name that a nested skipped body records but the lazy function does not
    have is known not to be used and gets no closure. Bodies shorter than
    the threshold are parsed, and under `JS_STRIP_SOURCE` bodies longer than
    a lower bound of their bytecode (so the eligibility rule's decisions are
    unchanged; functions nested in a lazy candidate being parsed are parsed
    too, since the candidate's decision includes their bytecode). A direct
    eval found after a skipped body compiles the script again without
    skipping. Stats: `preparsed`, `preparsed_source_bytes`,
    `preparse_fallbacks`, `preparse_restarts`. Host builds
    (`CONFIG_TILEFINCH_LAZY_TOOLS`) add `JS_CompileLazyFunctions`, which
    compiles every deferred body of a script for verification.
    `tests/test_quickjs_lazy_functions.c` runs every case eagerly, lazily
    with full parses and lazily preparsed, compiles every deferred body,
    and requires each early error to reject its script at compile time in
    all three modes.

27. `property-creation` (not a patch layer; numbered after the TDZ
    elision's expected 26, renumber at merge if needed) — three
    shortcuts for properties whose definition always ends the same way.
    (a) A new closure (`js_closure`: plain and async functions; generators
    keep the old path) is created with its final shape, kept per realm in
    `JSContext.closure_shape[]` and built on first use exactly as
    `js_function_set_properties()` and `JS_DefineAutoInitProperty()`
    would leave it ('length' and 'name' configurable, then a constructor's
    writable auto-initialized 'prototype'), and its slots are filled
    directly. Before, each closure cloned an empty shape for 'length',
    found the rest by transitions and reallocated its property array
    once per property. The shapes are hashed, so native functions that
    reach the same properties by transitions share them; the realm holds
    one reference, so they are never modified in place. (b) `OP_append`
    (array spread) stores each element with `add_fast_array_element()`
    when the literal is an extensible fast array whose end is the next
    index, the only case the literal can be in, instead of an index atom,
    `JS_DefineProperty()` and `JS_CreateProperty()`. (c) `OP_define_field`
    adds a missing field of an ordinary (`JS_CLASS_OBJECT`, not exotic)
    extensible object with `add_property()`; class fields on other
    receivers (Proxy, arrays, non-extensible objects) and redefinitions
    take the general path. (d) `js_obj_to_desc()` (Object.defineProperty,
    defineProperties, Object.create, Reflect.defineProperty) reads the
    fields of an ordinary descriptor object from their slots when the
    object and its prototype chain are ordinary, the chain adds none of
    the six fields and the object's own fields are plain data properties
    (reading them then runs no code), instead of a JS_HasProperty() and
    JS_GetProperty() pair per field. Accessor fields, inherited fields,
    Proxy and other exotic descriptors keep the ordered general reads.
    A closure whose shape cannot be allocated fails like any other
    allocation failure. Function creation also no longer ignores a failed
    'length', 'name' or 'prototype' definition (upstream returned the
    function with the out-of-memory exception left pending): closures,
    `JS_NewCFunction3`, `JS_NewCFunctionData`, promise resolving
    functions and `bind` now fail the creation instead.
    `tests/test_quickjs_property_creation.c` pins the defined results
    (key order, attributes, prototypes, constructor bits, receivers,
    descriptor fields and their read order) and the refusal paths, with
    one refused allocation at a time at each allocation of the script's
    closure, literal, spread and descriptor paths.

28. `native-calls` (not a patch layer) — the interpreter's OP_call* and
    OP_call_method enter a native (`JS_CLASS_C_FUNCTION`) callee through
    `js_call_from_bytecode()`: the interrupt poll JS_CallInternal() makes
    on entry, then `js_call_c_function()`, without JS_CallInternal()'s
    bytecode frame setup (about fifty Allegrex instructions per call). A
    scoped `JS_CallInternal` macro makes the redirection, so those call
    lines stay as the lab-variant patches expect. With integer arguments,
    `String.prototype.charCodeAt` on a flat string, one-unit
    `String.fromCharCode` (the shared one-character string for Latin-1,
    no string buffer) and `Math.imul` skip the general conversions; every
    other argument shape takes the general path.
    `tests/test_quickjs_native_calls.c` pins results and interrupt-budget
    consumption.

29. `lazy-compile-hook` (not a patch layer) — `JS_SetLazyCompileHook`
    calls an embedder hook before and after every first-call compile of a
    lazy function body, so the host's script split (script_split.h) can
    time lazy compilation inside page script without the profiler.

30. `stack-frame-origin` (not a patch layer) — `JS_GetStackFrameOrigin`
    tells a frame's origin (native, bytecode with a file name, bytecode
    without one) without decoding the line table, so the script split's
    per-poll sample costs a few loads instead of a pc-to-line walk.

31. `locale-date-shape` (not a patch layer) — `Date.prototype.toLocaleString`,
    `toLocaleDateString` and `toLocaleTimeString` produce browser en-US
    shapes (`10/1/2026, 9:05:00 AM`, no zero padding of month, day or
    hour), the same text the Intl polyfill's default `DateTimeFormat`
    formats, so `date.toLocaleDateString() === new
    Intl.DateTimeFormat().format(date)` holds as it does in browsers. The
    lab-variant baseline and variant pins moved with this change.

32. `gc-pacing-hook` (not a patch layer) — `JS_SetGCPacingHook` lets the
    embedding choose the threshold automatic collection re-arms at after
    each collection the allocation threshold runs: the hook gets the heap
    that collection left, the memory limit and QuickJS's own next threshold
    (live plus half of it, or half the remaining headroom near the limit)
    and returns the one to use. `JS_GetGCThreshold` reads the threshold and
    `JS_GetGCCause` tells a `JS_SetGCHook` observer whether the running
    collection is the allocation threshold's (`JS_GC_CAUSE_THRESHOLD`) or
    realloc-only array exhaustion (`JS_GC_CAUSE_ARRAY_GROWTH`), or
    any other `JS_RunGC` (`JS_GC_CAUSE_EXPLICIT`). The array rescue uses the
    same threshold/hook rather than collecting on every refused append.
    Without a hook the
    re-arm is unchanged. Tilefinch's pacing (`js_rt_gc_pacing`) uses it to
    stop near-limit collection thrash. The lab-variant baseline and variant
    pins moved with this change.

FinalizationRegistry callbacks use a separate runtime-owned job list. The
host drains it through `JS_ExecutePendingCleanupJob` as bounded tasks after
promise checkpoints, not as promise reactions. Both queues retain their
callback arguments and realms under the QuickJS allocator and are released
at teardown; retired realms never execute queued callbacks. The native API
refuses cleanup while a script or promise checkpoint is active.

## Lab variants

Turning `PSP_BROWSER_QUICKJS_CAPTURE_GETTER_FASTPATH` or
`PSP_BROWSER_QUICKJS_COMPACT_CHAR_ARRAY` off, or turning
`PSP_BROWSER_JS_PROPERTY_FAULT_TRACE` on, copies `quickjs.c` and
`quickjs.h` into the binary directory, reverses the optional/default layers down to the
shared bounded baseline (fingerprint checked), re-applies them with the
chosen options, and checks the result against a fingerprint pinned for that
combination. The vendored tree is never modified. The experimental VM patch
(`vm-profile`) and the `latin1-string-vm` and `array-growth-128k-migration`
patches are history only; configure refuses
`PSP_BROWSER_APPLY_BELLARD_QUICKJS_PATCH=ON`.

## Updating upstream

Fetch the new upstream tree, re-apply the changes above (or port them),
copy the engine files here, and update every pin. Keep this README's list
in step with what the tree actually contains.
