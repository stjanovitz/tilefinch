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
