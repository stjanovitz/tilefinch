# JavaScript bootstrap sources

These JavaScript files are the authored sources for the browser runtime
bootstrap. Edit them directly; do not hand-edit `../generated/js_bootstrap.c` or
`../generated/js_bootstrap_bytecode.c`.

`sources.def` is the shared manifest for source filenames, exported C symbols,
QuickJS diagnostic names, and bytecode symbols. Runtime evaluation order and
conditional deterministic-mode behavior remain explicit in
`../js_runtime/runtime_creation.inc`.

The generated C files are checked in because a PSP cross-build cannot execute
the host QuickJS generator. They keep the runtime filesystem-free. Host and
development builds preserve source fallback when embedded bytecode cannot be
restored; PSP builds default to `PSP_BROWSER_EMBED_BOOTSTRAP_SOURCE_FALLBACK=OFF`
and fail a realm cleanly if embedded bytecode cannot be restored. The
generated-manifest gate verifies that bytecode against the authored sources at
build time; there is no redundant runtime integrity check on device.
This avoids retaining a second representation of every bootstrap program in
the device's executable and memory image.

Generated QuickJS bytecode is stored uncompressed in read-only C arrays and
deserialized directly with QuickJS's ROM-data mode. The arrays outlive each
runtime, so this avoids a per-realm inflate buffer and permits QuickJS to
reference the serialized data in place. The bytecode omits its duplicate
source text. In host builds, the filesystem-free fallback is embedded
separately in `../generated/js_bootstrap.c`; PSP section garbage collection drops those
unreferenced arrays. QuickJS line tables remain present so bootstrap exceptions
can still report useful source locations.

From an existing host build directory, regenerate both files with:

```sh
cmake --build build-preset-release --target regenerate_tilefinch_bootstrap
```

Normal test builds run `tilefinch-bootstrap-generated-check`, which regenerates
temporary copies and fails if either checked-in artifact is stale. The
generator also records the exact source length and FNV-1a hash beside every
bytecode program, so runtime bytecode admission is tied to the authored source
bytes.

The current files intentionally retain the bootstrap's established logical
boundaries. Moving APIs between modules or changing runtime order should be a
separate, behavior-reviewed change.

Canvas, IndexedDB, CSS motion, and bounded Streams are ROM-backed on-demand
modules. Their standards-visible globals and Canvas prototype entries begin
as configurable accessors; the first read, write, Canvas dimension operation,
or bounded CSS animation hint synchronously evaluates the corresponding
bytecode, replaces the accessors with the normal implementation, and
continues the original operation. This keeps feature detection honest without
charging every page realm for uncommon stateful APIs or the CSS-keyframe
scanner.
`TILEFINCH_TRACE_JS_STARTUP=1` reports heap bytes after each eager or lazy
module. Regression tests require an ordinary realm to start with zero lazy
module loads, then verify that each first-use boundary activates only its
requested module and preserves the relevant API behavior.

The Streams boundary is intentionally below `Request`, `Response`, `Blob`, and
the fetch pipeline: those eager API definitions mention stream constructors
only inside callable methods. The first actual stream body or direct Streams
constructor access activates `streams.js`; pages which use neither retain only
the small configurable accessors. The bounded stream behavior is tested through
both the unloaded and first-use paths.

`motion.js` is evaluated after the scheduler because stylesheet keyframes,
inline transitions, and `Element.animate()` deliberately share one bounded
frame queue. It retains only the common single-animation path; keep new timing
or composition semantics within its documented PSP caps.

The authored JavaScript was mechanically formatted once with the default
Prettier 3.6.2 rules. Prettier is not a project dependency or a build step;
future edits should preserve the surrounding readable style without requiring
a formatter run for every change.
