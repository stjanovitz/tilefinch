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

Exported linear-memory views are page-Budget-owned ArrayBuffer backings rather
than QuickJS-heap allocations. Tilefinch currently mirrors WAMR's relocatable
memory and synchronizes it at every exported call and grow boundary; this keeps
the standard JavaScript buffer lifetime and detachment behavior while ensuring
a bounded one-page-to-sixteen-page module cannot exhaust an otherwise healthy
realm merely by exposing its memory.

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
`Budget` and released when no module or instance remains.

Host builds link the same pinned WAMR interpreter directly for sanitizer and
conformance testing. The PSP acceptance fixture
`tests/fixtures/wasm-bounded-runtime.html` additionally proves lazy component
loading, a JavaScript function import, exported-memory coherence, standard
fixed-width SIMD, live exported globals, and teardown through the normal
browser EBOOT.
