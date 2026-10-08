# Native execution tier: findings and decision

As of 2026-09-30, native-tier and whole-graph ahead-of-time (AOT) compilation
research is parked. Neither is enabled in shipping builds. The interpreter
remains the execution path; cached QuickJS bytecode is distinct from native
machine code.

This page summarizes the conclusions. Detailed investigations, development
branches, captures, binaries and profiles are retained locally and in the
private development archive.

## What was measured

An isolated Allegrex emitter executed bounded bytecode regions on a physical
PSP-3000. Differential probes covered real interpreter frames, ownership,
allocation refusal, exceptions, predicates and interruption. Correct execution
of those probes did not establish a browser performance benefit.

Three matched device runs per helper interface showed 5–11% savings on
own-property kernels, roughly neutral to 5% savings on inherited-property
kernels, 3–8% savings on getter kernels, and a 25% proxy regression. Generic
reference-batch variants were 2.3–4.7 times slower than the interpreter.
Host whole-page controls showed approximately no gain or a small regression.
These results reject promotion of the current native tier.

The main limitations were short supported regions, frequent interpreter exits,
value/frame handoff costs, and substantial called-body work outside the
accelerated regions. A narrow borrowed-property lowering covered only about
1.2% of the dominant callback. Compact plan storage reduced metadata but
regressed the getter fixture; memory savings alone did not justify it.

Whole-graph AOT produced an Allegrex object, but lacked a qualified runtime
and browser-binding integration. It has no execution or speedup result.
Hand-written native kernels and host gains cannot substitute for that result.

The PSP's 16 KiB instruction cache makes code placement material: even
unchanged code moved between builds can change individual kernel timings
substantially. A sampled code footprint is not a hardware cache-miss census,
and device wall time divided by a host opcode count is not cycles per opcode.

## Proven alternatives and measurement limits

Profile-guided optimization, validated lazy compilation, cached module
bytecode, and narrower style invalidation have produced useful improvements
without a native execution tier.

One matched device profile comparison reduced Send to the DOM-answer
predicate from 43.472 to 40.469 seconds median (6.9%). Eager cached bytecode
later reached that predicate at 38.715 seconds median. These are distinct
experiments and must not be combined into a single causal comparison.

Earlier device answer-image claims were withdrawn: requested marks exceeded
the capture queue, so a reused output file was stale. Those timings establish
DOM readiness, not first-visible-answer latency or device pixel parity.
Independent host pixel comparisons and synthetic device probes remain valid.

## Conditions for revisiting

Reopen only for a materially different, execution-ready hypothesis with
substantial measured callback coverage and bounded code/metadata memory.
Use the [canonical comparison procedure](INPUT_SCRIPT_HARNESS.md#canonical-reply-performance-comparisons).
A short investigation should first establish at least a 20% improvement on
the real callback in three or more alternating or counterbalanced device
runs per mode, preferably using one binary with a runtime switch. Include
compilation, exits and fallbacks; prove identical fresh answer pixels and
unchanged required work. Check startup, other jobs and memory peaks too.

That threshold permits further research; it does not meet the larger goal.
Reducing the measured 26.418 seconds of script work by 20% would still leave
about 33.431 seconds of the 38.715-second DOM-answer interval. Holding other
work fixed requires roughly 52% less script time to reach 25 seconds.

## Required execution and ownership contract

- Keep interpreter fallback and exact bytecode PC, stack, exception and value
  ownership at every exit. Do not repeat earlier side effects.
- Guard property kind, shape changes and lifetime. Retain receiver identity,
  coercion, getters/proxies, TDZ, const writes, NaN and negative zero semantics.
- Preserve interrupt accounting, watchdogs and realm retirement. Unsupported
  operations must refuse optimization cleanly.
- Charge all code, metadata, staging and alignment through Budget and JS-heap
  admission. Bound compilation and treat refusal as optional optimization.
- Invalidate plans when lazy compilation replaces bytecode. Root values across
  re-entrant calls and wait for active invocations before releasing code.
- On hosts, publish separately owned writable pages as executable only after
  successful protection changes. On PSP, qualify the actual Allegrex ABI and
  data/instruction cache maintenance; the device provides no memory protection.
- Never persist native addresses or emitted code in the module bytecode cache.
  Research uses an isolated worktree and artifacts; promotion requires host,
  sanitizer, named PSP and size gates plus physical-device measurements.

Do not repeat rejected prototypes without a changed premise. Full experiment
records remain available privately for that decision.
