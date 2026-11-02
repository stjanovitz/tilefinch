# JavaScript bootstrap sources

For contributors: these JavaScript files are the authored sources for the
browser runtime bootstrap. Edit them directly; do not hand-edit `../generated/js_bootstrap.c` or
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
reference the serialized data in place. The bytecode omits its source text,
line tables, and local-variable names, because QuickJS would copy them out of
ROM into every realm's heap on restore. Bootstrap exceptions still name the
module and function; for line numbers, run the host with
`TILEFINCH_DISABLE_BOOTSTRAP_BYTECODE=1`, which compiles the embedded source
with full debug information. Setting `TILEFINCH_BOOTSTRAP_KEEP_LINES=1` while
regenerating keeps the line tables for a profiling build; never commit that
output. In host builds, the filesystem-free fallback is embedded separately in
`../generated/js_bootstrap.c`; PSP section garbage collection drops those
unreferenced arrays.

## Regenerating

This README is the home of the regeneration rule. After editing any
`src/bootstrap/*.js` file or `sources.def`, regenerate **both** generated C
files and the manifest from a host build tree:

```sh
cmake --build build-preset-release --target regenerate_tilefinch_bootstrap
```

That target rewrites `../generated/js_bootstrap.c` and
`../generated/js_bootstrap_bytecode.c` and derives `generated.sha256` from the
exact authored and generated tree. Never edit `generated.sha256` by hand, and
commit the three files with the source change.

The generator embeds a compacted copy of each source (indentation, trailing
whitespace, and comments removed; newlines kept) and refuses to emit unless
the compacted text compiles to the same bytecode as the authored file. It
records that compacted text's length and FNV-1a hash beside every bytecode
program, so runtime bytecode admission is tied to the embedded source bytes.

Host and PSP builds both make `tilefinch_core` depend on
`check_tilefinch_bootstrap_generated`, so a stale artifact fails the next core
build either way. They check different things:

| Build | Check |
|---|---|
| host | the generator's `--check` mode compares the bytecode, and the manifest is verified |
| PSP | the manifest only: the cross toolchain cannot run the host QuickJS generator |

A hand-edited manifest would pass the PSP check; the host check exists to
catch it. The host build hashes the complete source and artifact set on every
invocation, and reuses a successful bytecode comparison only while the
manifest contents, the generator executable, and the verification
implementation are unchanged, so C-only edits do not recompile JavaScript.
Never replace this content identity with timestamp-only stamps. The
`tilefinch-bootstrap-generated-check` CTest always regenerates temporary
copies and compares them, without the memo.

## Module boundaries

The current files intentionally retain the bootstrap's established logical
boundaries. Moving APIs between modules or changing runtime order should be a
separate, behavior-reviewed change.

Canvas, IndexedDB, CSS motion, bounded Streams, bounded capability probes,
and DOM traversal (NodeFilter, TreeWalker, NodeIterator and the Document
factories for them) are ROM-backed on-demand modules. The capability module exposes the Web Speech
shape with an empty engine and a Media Source shape that advertises no
supported byte-stream types; it does not pretend the PSP implements either
pipeline. Their standards-visible globals and Canvas prototype entries begin
as configurable accessors; the first read, write, Canvas dimension operation,
or bounded CSS animation hint synchronously evaluates the corresponding
bytecode, replaces the accessors with the normal implementation, and continues
the original operation. This keeps feature detection honest without charging
every page realm for uncommon stateful APIs or the CSS-keyframe scanner.
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
