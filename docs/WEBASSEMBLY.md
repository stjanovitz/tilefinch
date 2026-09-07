# Bounded WebAssembly profile

Tilefinch implements the standard JavaScript WebAssembly API for small,
self-contained modules. The PSP loads the interpreter only when a page first
accesses `WebAssembly`, so ordinary browsing and boot do not pay its code or
initialization cost. The component is installed and updated as part of the
same signed Tilefinch slot as the browser.

This is a bounded compatibility profile, not an alternate module format.
Modules use the WebAssembly binary format, ordinary `application/wasm`
streaming responses, standard numeric conversions, and the usual JavaScript
error classes. Work that exceeds a Tilefinch ceiling fails through the API;
it is never allowed to grow in proportion to an untrusted module.

Within the admitted numeric/module subset, namespace enumerability,
constructor brands, `Instance.prototype.exports`, immutable export-object
descriptors, asynchronous result shapes, and live exported-global semantics
match the WebAssembly JavaScript API. Unsupported proposal features or import
kinds fail during validation or linking; Tilefinch does not publish a partial
instance with silently missing exports.

The base JavaScript surface is checked with a curated, PSP-bounded lane derived
from the Web Platform Tests for the WebAssembly JS API. It covers interface
descriptors, Web IDL dictionary access and coercion order, constructor and
promise timing, numeric conversions, reflection, imports, exports, memory,
tables, globals, and error behavior. The lane intentionally excludes proposals
and import kinds listed under Deliberate omissions; passing it is a conformance
claim for the admitted profile, not a claim that Tilefinch implements every
WebAssembly proposal or every desktop resource limit.

The compatibility surface also preserves the less-visible JS API contracts
used by feature and integrity probes: exported functions retain their core
function index as `name`, expose their declared parameter arity, and share one
object when the same function is exported under multiple names. Error
constructors work with or without `new` and use the standard prototype
descriptors. Numeric coercions distinguish `BigInt` from Number as required:
`i64` accepts and wraps BigInt-compatible values, while the other numeric
types reject BigInt. Address conversions reject `BigInt`, NaN, infinities, and
negative values before applying Tilefinch's smaller resource ceilings.
`externref` and `anyfunc` table/global defaults follow their standard
`undefined`/`null` semantics, including the distinction between an omitted
`anyfunc` table value and an explicitly supplied `undefined`. Descriptor
members are read in Web IDL order, so observable getters behave consistently
with ordinary browsers. Memory and table descriptors accept the standard
omitted or explicit `address: "i32"`; `address: "i64"` fails explicitly as part
of the documented memory64 omission.

## Supported surface

- `WebAssembly.compile()`, `instantiate()`, `validate()`,
  `compileStreaming()`, and `instantiateStreaming()`;
- `Module`, `Instance`, `Memory`, `Table`, and `Global` constructors;
- `Module.imports()`, `Module.exports()`, and `Module.customSections()`;
- `CompileError`, `LinkError`, and `RuntimeError`;
- synchronous constructors and the standard promise/result shapes of the
  asynchronous helpers;
- `i32`, `i64`/JavaScript `BigInt`, `f32`, and `f64` function values;
- standard fixed-width SIMD (`v128`) instructions, interpreted through a
  portable bounded implementation on PSP;
- bounded multi-value results, bulk-memory operations, exported linear
  memory, coherent `Memory.buffer`, and buffer detachment after `grow()`;
- exported numeric `Global` objects, including live mutable values shared
  between JavaScript and module execution;
- JavaScript function imports, including exact propagation of exceptions
  thrown by the imported function and imported start functions that execute
  exactly once during instantiation;
- immutable compiled `Module` values in `structuredClone()` and messages to a
  bounded Tilefinch Dedicated Worker. Instances, memories, tables, and globals
  remain non-cloneable.

Streaming compilation consumes the bounded `Response.arrayBuffer()` path and
requires an actual `Response` object, a successful status, and a MIME essence
of `application/wasm`. A lookalike object cannot enter the streaming path.
Tilefinch does not expose a second, less constrained streaming loader.

## Loading and calling

A `Module` is validated by one interpreter load, and that load also records
the module's import and export descriptors, so `Module.imports()` and
`Module.exports()` answer from the `Module` without loading again. Each
`Instance` costs exactly one further load: the module is loaded unlinked,
its function imports are bound from the import object, and the same loaded
module is then linked in place and instantiated. The interpreter's load is
the expensive step on the PSP (it validates and pre-decodes every function
body), so a `compile()`/`instantiate()` pair performs two loads and
`instantiate(bytes)` performs two, never three or four.

Every function export is resolved once at instantiation into an
instance-owned table holding the interpreter function handle and its
signature; an export call indexes that table rather than looking the function
up by name and re-reading its types. The interpreter's contract is that a
module's byte buffer is writable and referenced until unload, so every load
works on a private copy rather than sharing bytes between instances or
borrowing a JavaScript `ArrayBuffer`; an instantiated module releases that
copy as soon as the interpreter confirms it no longer references it (the
fast interpreter pre-decodes function bodies and the loader clones data
segments), so a live instance does not hold its binary for its lifetime.
Exported memory is aliased, not copied:
`Memory.buffer` is an external `ArrayBuffer` over the interpreter's linear
memory, refreshed at every wasm/JavaScript transition so growth (which moves
the memory) detaches the stale buffer as the standard requires.

## PSP ceilings

| Resource | Limit |
| --- | ---: |
| Module binary | 512 KiB |
| Interpreter pool, charged to the page budget | 4 MiB |
| Execution stack per instance | 64 KiB |
| Linear memory | 16 pages / 1 MiB |
| Imports per module | 32 |
| Aggregate import-name storage | 4 KiB |
| Exports per module | 64 |
| Function arguments | 16 |
| Function results | 4 |
| Matching custom sections per query | 32 |
| JavaScript-created table | 1,024 elements |
| Native instructions per exported call | 50,000,000 |

The instruction limit is a responsiveness boundary because native
interpretation runs outside QuickJS's ordinary bytecode interrupt handler. A
module that exhausts it traps with `WebAssembly.RuntimeError`; the page realm
remains usable. Fixed-width SIMD is scalar-emulated on Allegrex. It is useful
for compatibility and bounded feature probes, but large throughput benchmarks
can be much slower than on desktop SIMD hardware.

Exported ArrayBuffers directly alias WAMR's Budget-owned linear memory; no
per-call memory copies are made. Growth detaches old views before allocating
a replacement, including on allocation refusal. Retained views keep their
native owner alive, and detach/finalization release each alias exactly once.
The host task watchdog is shared across instances and calls, rather than
granting a new wall-clock allowance to each export. Instantiation, imports,
and export entry/return check that shared deadline. The interpreter's finite
instruction ceiling still bounds individual native calls.

## Deliberate omissions

The PSP profile does not provide WASI, Emscripten libc shims, JIT or AOT
execution, threads/shared memory, relaxed SIMD, reference types,
garbage-collected WebAssembly, memory64, or multiple memories. It currently
links native modules through JavaScript **function** imports. JavaScript-created
Memory, Table, and Global objects follow their bounded standalone API, but
importing those objects into a native module and exposing a module's native
Table are not yet supported.

Sites should feature-detect proposals and optional import kinds in the usual
way. A rejected unsupported module must not retire the JavaScript realm or
weaken the page's network, CSP, origin, or memory policy.

## PSP component lifecycle

The boot EBOOT contains only a small versioned adapter. On first namespace
activation it loads `tilefinch-wasm.prx` from the active signed slot and
verifies the component ABI before publishing `WebAssembly`. A missing,
incompatible, or refused component leaves the namespace unavailable rather
than partially installing it. The runtime pool is allocated lazily through
`Budget`. It is released once no module operation or instance remains, at
the next task boundary, memory-pressure collection, or realm teardown rather
than immediately after each operation, so a `validate()`, `compile()`, and
`instantiate()` sequence within one task initializes the runtime once.

Host builds link the same pinned WAMR interpreter directly for sanitizer and
conformance testing. The PSP acceptance fixture
`tests/fixtures/wasm-bounded-runtime.html` additionally proves lazy component
loading, a JavaScript function import, exported-memory coherence, standard
fixed-width SIMD, live exported globals, and teardown through the normal
browser EBOOT.
