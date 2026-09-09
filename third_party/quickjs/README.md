# Vendored QuickJS

Upstream: Bellard QuickJS, commit `04be246001599f5995fa2f2d8c91a0f198d3f34c`
(the 2026-06-04 release line). Only the files the engine build needs are
kept; upstream's tests, tools, and documentation are not.

The tree already carries this repository's engine changes. It replaces the
earlier configure-time flow that fetched the pristine tarball and applied
seventeen layered patches with marker detection and reverse-apply steps.
That flow failed three times in one week (a shared source directory let an
experimental patch reach release binaries, a fix had to be shipped as a
patch on a patch, and a validation rewrite rejected already-patched trees),
and every engine change was reviewable only as a patch file. Now an engine
change is an ordinary diff.

`cmake/TilefinchDependencies.cmake` pins the SHA-256 of `quickjs.c` and
`quickjs.h`; a mismatch fails configure. An intentional engine change
updates the pins in the same commit (the error prints the new values).

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
   `PSP_BROWSER_QUICKJS_CAPTURE_GETTER_FASTPATH`, default ON).
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
11. `single-char-string-buffer` — single-code-unit `StringBuffer` results
    share the immutable one-character cache.
12. `compact-char-array` and `compact-char-array-cow` — pack dense arrays
    of one-character Latin-1 strings, with copy-on-write (option
    `PSP_BROWSER_QUICKJS_COMPACT_CHAR_ARRAY`, default ON).

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

## Lab variants

Turning `PSP_BROWSER_QUICKJS_CAPTURE_GETTER_FASTPATH` or
`PSP_BROWSER_QUICKJS_COMPACT_CHAR_ARRAY` off, or turning
`PSP_BROWSER_JS_PROPERTY_FAULT_TRACE` on, copies `quickjs.c` and
`quickjs.h` into the binary directory, reverses layers 4 to 12 down to the
shared bounded baseline (fingerprint checked), re-applies them with the
chosen options, and checks the result against a fingerprint pinned for that
combination. The vendored tree is never modified. The experimental VM patch
(`vm-profile`) and the `latin1-string-vm` and `array-growth-128k-migration`
patches are history only; configure refuses
`PSP_BROWSER_APPLY_BELLARD_QUICKJS_PATCH=ON`.

## Updating upstream

Fetch the new upstream tree, re-apply the changes above (or port them),
copy the engine files here, and update both pins. Keep this README's list
in step with what the tree actually contains.
