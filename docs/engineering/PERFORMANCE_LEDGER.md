# Performance findings and qualification summary

This page records selected conclusions and comparable measurements through
2026-09-30. Detailed session journals, private branch references and raw
captures are retained locally and in the private development archive.
Read the [development workflow](../DEVELOPMENT.md),
[memory experiment ledger](MEMORY_EXPERIMENTS.md), and
[performance journeys](PERF_JOURNEYS.md) before repeating an experiment.

## Measurement rules

Distinguish optimized host, PPSSPP and physical-PSP results. Host timings
cannot price firmware calls or the device's caches. Attribute nested native
and layout work without double-counting it, and separate DOM readiness from
successful presentation of visible content.

Use the [canonical reply comparison](INPUT_SCRIPT_HARNESS.md#canonical-reply-performance-comparisons):
fixed-clock offline response release, explicit PGO and instrumentation settings,
and at least three alternating runs per mode where possible. Pump-count replay
can make scheduling changes appear to accelerate the network. A rebuild can
also change code placement enough to confound a small engine improvement.

Capture evidence must show admission, successful write and fresh output.
Some historical device answer-pixel comparisons were withdrawn because too
many requested marks filled the capture queue and left an old answer file.
The affected timings measure the DOM-answer predicate, not first visible ink.

## JavaScript execution and reply performance

The measured callback cost includes interpreter work, nested calls/getters,
and synchronous DOM/style operations. No single dispatch optimization has
demonstrated the requested 50% callback reduction on the device.

A matched physical-PSP comparison with three runs per mode found:

| Metric | Baseline median | Fresh engine PGO median |
|---|---:|---:|
| Send to DOM-answer predicate | 43.472 s | 40.469 s |
| First usable input | 42.455 s | 40.037 s |
| Send-window script, including input handlers | 31.437 s | 27.586 s |
| Longest promise job | 9.311 s | 8.296 s |

The 6.9% Send-to-answer improvement is smaller than the script-only gain.
Phase medians are not additive. A separate eager-bytecode-cache experiment
reached the DOM-answer predicate at 38.715 seconds median; it did not establish
device answer-pixel parity or a native-code benefit.

An earlier merged-engine PGO comparison reduced first usable input from
39.38–39.40 to 37.37–37.50 seconds and the answer milestone from 97.8–99.6
to 93.2–93.7 seconds after process start. Its configuration differs from
the later canonical timing contract. Profiles are not portable artifacts:
retrain after engine/compiler changes and retain the fingerprint check.

The validating preparser and bounded lazy compilation reduce repeated parsing
and first-call work. Persistent module bytecode avoids recompiling unchanged
assets. These wins do not remove synchronous callback, resource or layout cost.

Native-region experiments are parked: small synthetic gains did not translate
into a qualified real-reply speedup, and several helper/proxy paths regressed.
See [native-tier findings](NATIVE_TIER_INVESTIGATION.md) for rejection reasons
and the conditions for reopening that work.

## Style, DOM and resource refresh

Scoped selector invalidation, retained computed styles and complete attribute
dependency tracking avoid repeating cascades on unrelated mutations. Cache
validity still includes document, stylesheet, inheritance, container and font
dependencies; shadow/adopted sheets and pseudo queries require the same care.
No-op DOM adoption and form-state walks can be skipped only with evidence that
their observable inputs are unchanged.

For a selected six-icon SVG refresh, sharing the mutation-invalidated layout
style cache reduced optimized-host refresh from 1.533/1.608 to 0.743/0.604 ms
(51.5–62.4%). Reply text and fresh host RGB565 pixels matched. Other additive
resource batches still required full discovery. A single device pair reduced
the targeted compound preparation batch from 468 to 292 ms, but it was not an
interleaved end-to-end comparison and its answer-pixel claim was withdrawn.

Keep style reuse bounded and owned. Tests cover inherited color, hidden/reveal
behavior, interrupted refresh, allocation refusal and zero-owned teardown.
Subtree-only image discovery was rejected because inherited/background resource
dependencies require a wider walk.

## Long-article layout cost and live input evidence (from 2026-09-04)

Resumable layout and checkpoints within long text runs let cursor, menu and
cancel input interrupt page work. On a 32 KiB single-text-node host regression,
the median largest cooperative gap fell from 1.418 to 0.184 ms with identical
text commands, fingerprint and height. This bounds loop progress; it does not
bound one indivisible shaping/font call or promise a PSP latency.

Font publication retains font-independent styles and recomputes sizing and
font-metric dependencies. Generation-preserving cache rebinding avoids a cold
pass after navigation promotion. A matched 30-sample host comparison reduced
ordinary-style time from 2.193 to 1.622 ms and total publication from 7.808 to
7.488 ms. Pseudo absence checks, ASCII proof reuse and fewer repeated glyph
lookups yielded additional measured savings while preserving raster checksums.
These are separate experiments; their percentages must not be added.

A device article comparison reduced resource traversal from 14.42 to 9.77
seconds, but full flow remained about 18 seconds. With a bounded one-minute
navigation deadline, the tested article completed in 35.887 seconds with a
4.080-second first preview. Cursor and browser controls worked during loading.
The longer deadline prevents premature cancellation; it does not resolve all
remaining layout cost. The associated section-focus script missed its intended
target, so that run did not qualify section expansion.

## Scheduling and rejected approaches

- A 2 ms page-task yield saved about half a second after Send in a matched
  device replay but added 7–10 seconds before interactivity in two of three
  runs by starving lower-priority I/O. It remains opt-in.
- Native reference batches, broad frame fast paths and local TDZ-elision
  experiments did not establish device gains. Compact native metadata saved
  memory but regressed getters. Their full evidence is retained privately.
- Host or emulator CPU ratios are not hardware cycles-per-instruction or a
  cache-miss census. Synthetic working-set kernels identify hypotheses, not
  percentages of whole-page savings.
- Extra memoization and broad resource shortcuts are declined when ownership,
  invalidation or fallback costs exceed the measured benefit.

Future work should attribute the critical request/job/layout timeline first,
then optimize measured repeated work. Use unchanged replies, fresh pixels,
complete-work counters and teardown ownership as acceptance evidence, alongside
timings. Consult private experiment records before reopening a rejected path.
