# PSP input script harness

The validation build can replace physical controller input with a deterministic
script interpreted inside the PSP application. The script crosses the same
`psp_ui_update()` and action-dispatch boundary as real buttons, so it qualifies
menu routing, focus, state projection, navigation cancellation, and application
lifecycle without depending on an emulator automation API.

Shipping EBOOTs compile the harness out. A release build handed an
`input_script=` key ignores it and boots normally.

### Fixed-pixel canvas setup probe

Validation mark `canvas-setup-probe` (after `canvas-vfpu-check`, outside the
timing window) compares baseline and setup-once dispatch in one binary.
The probe alternates three batches per variant for both packed and padded
320×180 input, scaled to 480×272 with padded output. It preserves the existing
VFPU row converter and duplicate-row copy. Full output/padding and borrowed
VFPU state must match; actual VFPU row count must be 180, not merely enabled.
Each batch reports owner CPU, wall-clock bounds and clock-query overhead.

This bounded Budget-owned **RAM** corpus is not an EDRAM bandwidth or FPS
measurement. It leaves framebuffer ownership unchanged and releases every
allocation. `canvas-setup-on` / `canvas-setup-off` select the research path
for subsequent gameplay; baseline is the default. None of these paths ships
in ordinary builds. A gameplay comparison must still use identical recorded
input/dt/state and distinguish profiling overhead from deadline misses.

## Why input is injected inside the application

PPSSPP input injection cannot reliably answer when a frame consumed an edge,
whether the app was busy, or which action receiver accepted it. The in-app
stepper owns those facts and produces a deterministic trace:

- one scripted action per eligible frame;
- explicit wait predicates and bounded stall detection;
- semantic marks tied to a completed presentation;
- receiver and setting coverage counts;
- a terminal outcome independent of wall-clock time.

Visual marks enter a bounded RAM capture queue and are flushed only after the
scenario ends. Device timing therefore does not include per-frame Memory Stick
writes.

The current mark name capacity is 20 bytes including the terminator, and only
three distinct mark images are retained. Validate generated names against
`PSP_INPUT_SCRIPT_MARK_CAPACITY` before a launch. Extra diagnostic marks can
consume image slots: require the matching `tilefinch-input-capture:` record
and a successful current-run write before inspecting a named file. A file
left by a previous run is not evidence. Keep expensive pixel comparisons in
warm-up or after the measured window, not in startup or timed gameplay.

Qualification-only boots should have an empty `input_script`: otherwise the
report waiter correctly demands completion of a script the qualification
runner never executes. Read the qualification outcome separately from an
ordinary scripted browsing/gameplay outcome.

For small-text or emoji inspection, the validation build also honors
`dump_frame=1` after the interactive scenario finishes. It writes
`frame-device-final.ppm` from the current 480×272 RGB565 front buffer before
teardown, preserving full spatial resolution rather than the mark queue's
240×136 RGB332 samples. This opt-in write happens after the measured sequence;
use a host0 launch to keep it off the Memory Stick. It does not capture the
separate RGB8888 video surface and reports `final-frame-dumped=0` when that
surface owns scanout. Ordinary release builds do not contain this final dump.

## Script format

The language is line-oriented. Blank lines and `#` comments are ignored.

```text
wait COUNT
tap BUTTONS
hold COUNT BUTTONS
press COUNT BUTTONS
stick COUNT DIRECTION [BUTTONS]
mark NAME
end
```

Append `-live` to `wait`, `tap`, `hold`, `press`, `stick`, or `mark` when
that step must continue while the application reports asynchronous work. For
example, `wait-live 30` advances for 30 sampled frames during a navigation
or sustained media session. Without the suffix, a step pauses until the
ordinary browser loop is ready to accept scripted input.

Use plain `mark` for page/runtime profiling boundaries. A `mark-live` can be
consumed by the input supervisor while page work is still running: its UI
capture is valid, but the main-loop page observer may not emit the JavaScript
profile or the `.mark.js` diagnostic. Verify those records exist before using
a mark as a timing/report boundary; a captured image alone does not prove it.
`mark-page` also runs in the main loop and is useful for an animated game.
Probe names replace the input script extension: `game.txt` uses
`game.mark.js` and `game.until.js`, not `game.txt.mark.js`.
`psplink-device.sh memory` stages these companions with a committed scenario,
so an updated diagnostic cannot silently run with an older copy on host0.

`treadline-action-budget.txt` exercises Deploy, all nub directions, face-button
aim, cannon, secondary, gadget, command, and the escape chord in an Onslaught
boss scene. Its companion requires successful gameplay and killcam history,
and disables game phase clocks during the displayed-frame measurement. Use the
USB-only installed-package route in [PSPLink dev loop](PSPLINK_DEV_LOOP.md),
with `qualification=input&bridge-profile=off&profile-report=off&seed=12345`
in the package source URL. The `configure`, `heavy-setup`, and `post-play`
capture slots cover the menu, heavy scene, and post-action scene. Keep attribution
runs separate: phase clocks add work
and must not be mistaken for ordinary gameplay timings.

For attribution, leave input profiling enabled after `webgl-measure-start`
and collect `__treadlineDebug.slowProfile()` at the end. Input qualification
retains up to 540 samples, covering the full button workload rather than only
its first 180 frames. The ranked `rows` have ten words per frame: update,
prelude, bots, movement, projectiles, world, scene build, HUD, commands, total.
The first three ranks have matching `aiRows`; use `aiRowWords` for their stride
(currently 31). Words 0–10 are target selection, goal, route, steering, aim,
fire, BFS, aim preparation, visibility, bank planning, and aim angle. Words
11–14 are movement drive/traction, collision/recovery, travel/height/turret,
and timers/actions. Words 15–17 split the route helper into goal remapping,
field lookup/build, and waypoint selection/visibility. Word 18 counts strategy
refreshes; 19–20 are aim-preview time and count. Words 21–24 are cannon,
secondary, gadget, and command attempt times; 26–29 are their attempt counts
(not successful shots). Words 25 and 30 are gain-envelope scheduling time and
count. Action times are nested in movement timers/actions; envelopes can also
run in other phases. Nested phases must not be added to their parent totals.
Movement, action and envelope deltas reuse existing clock readings;
the finer route clocks run only during attribution. Run host CPU-heavy gates
separately from USB timing runs so host0 servicing is not a confounder.

For silent physical-device game tests, set `validation_game_audio_mute=1`
in the validation build's `boot.cfg`. Voices, envelope processing and PCM
mixing still run; only hardware output volume is zero. The default is 0,
and shipping configuration scrubbing removes the setting. For Treadline
cadence runs, `profile-report=off` suppresses the large formatted in-game
report; collect its bounded `__treadlineDebug.aiProfile()` totals in a mark
probe instead. A report-building pause is not a gameplay optimization target.

Use `-page` on the same commands to wait for a committed, painted page but
not for background resources to finish. These steps run only in the main
input loop, never inside a runtime cooperation callback. They are useful for
checking focus and disclosure controls during page startup. Neither suffix
turns the earlier provisional navigation preview into an interactive document.

`until COUNT` idles like `wait COUNT` but ends early once the page is ready:
when a `<script>.until.js` probe sits beside the script, the main loop
evaluates it as a function body about twice a second while the step runs, and
a truthy result ends the step on the next frame (logged as `until-met`).
Readiness probes use the bounded synchronous probe evaluator: they do not
implicitly drain queued page jobs, refresh the document, force collection or
take a full runtime snapshot. This keeps author continuations on the normal
runtime path. Probe source is still trusted diagnostic JavaScript and can
explicitly call author functions or mutate state; write observation-only
predicates. Ordinary `.mark.js` diagnostics retain their checkpoint behavior.
For frame-timing runs, the entire `.until.js` file may instead contain
`@global predicateName` (an ASCII identifier of at most 96 bytes, followed only
by whitespace). Define that function before the measured window. The harness
calls it directly with no arguments and the page global as its receiver, under
the same watchdog. This avoids repeated source compilation and clipboard
bookkeeping; it still does not drain jobs. A missing, throwing, or non-callable
predicate cannot satisfy the condition. Malformed `@global` directives fail
the check rather than executing as JavaScript.
`COUNT` stays the upper bound, so a probe that never holds costs the same as
the fixed wait it replaces. The probe never runs inside page script. Like
`<script>.mark.js`, which is evaluated at every mark and logged, it is a
validation-only device diagnostic.

### Send/interaction wall-time accounting

Scripted validation runs emit `tilefinch-loop-timing` for every owner-loop
invocation, including short frames and early returns. Its ordered phase
durations sum exactly to `end-us - begin-us`: wait/setup, input (including
pointer dispatch), action, diagnostic probe, navigation, runtime/recovery,
idle resources/UI sync, raster, presentation, deferred images, and tail
pumps. These are exclusive wall intervals, not CPU utilization. Background
transport can overlap them. Log formatting between invocations remains a
gap; the analysis tool leaves gaps unassigned rather than naming them network
time. The counters and logging are absent from ordinary builds.

Use explicit monotonic endpoints from the input edge and the desired
condition/publication records:

```sh
python3 scripts/analyze-input-timeline.py validation.log \
  --start-us 1000000 --end-us 21000000
```

The tool clips partially overlapping phase intervals, rejects malformed or
overlapping records, and reports coverage. Never add the nested JS/native
profiles or curl durations to its totals. `tilefinch-input-probe` records
evaluation start/end; `until-met at-us` is the completed check, not a pixel
publication. `tilefinch-page-publication` records successful/failed present
results and the layout text fingerprint; `tilefinch-input-capture` records
when the named front-buffer snapshot was actually copied. Inspect pixels
before claiming the desired content was visible.

Named device captures have a fixed three-entry queue. Reserve a slot for the
answer rather than requesting more marks and later trusting an old file. For
each claimed image, require both `tilefinch-input-capture: mark=... at-us=...`
and the matching `capture=... written=1` completion, and verify that the artifact
was newly written by that run. Prefer a previously absent per-run output path;
otherwise remove only owned capture outputs before launch. Verify the complete
image parses and record its hash. File existence or a matching hash alone is
not capture evidence; inode change is not required because truncating an
existing file may preserve its inode. Named marks are 240x136 RGB332 samples,
not full-resolution RGB565 parity. A strict pixel gate needs a fresh 480x272
`frame-device-final.ppm` with `final-frame-dumped=1` and matching answer state,
or another full-resolution capture. A final dump is a separate endpoint, not
automatically the first-answer frame.

### Canonical reply performance comparisons

For fixed-delay execution replay, use one captured response set and input/probe
set with these explicit settings:

```ini
trace_keyed=1
trace_ignore_request_body=1
trace_volatile_uuids=1
trace_replay_pump_us=2000
page_task_yield=0
validation_js_profile=0
validation_script_split=0
validation_execution_census=0
```

Confirm each key against the current boot-config parser before running. Fix and
record eager/lazy compilation policy, module-cache state and write policy, PGO
on/off and profile identity, immutable binary hash, capture/input/probe hashes,
compiled instrumentation, PSP clocks, and transport/storage path. Separate
attribution runs may enable profiling/splitting; their totals are not timing
baseline substitutes. Use at least three runs per mode, alternating or
counterbalanced with a same-binary runtime switch when available.

The fixed delay is admission time plus `max(1, recorded_pumps) * 2000` microseconds;
delivery still requires scheduler service. This does not reconstruct captured
wire latency. Re-baselining from pump-count timing is a configuration change,
not an optimization gain. Start at the actual Send input edge and end at verified
assistant content plus successful publication and a fresh admitted capture;
keep the endpoint convention unchanged across comparisons. DOM-only readiness
and inspected first-visible-answer latency must be labeled separately.

`tilefinch-fetch-timeline` links scheduler-local request IDs and diagnostic
item tokens to generation-tagged background IDs, without URLs, headers or
body text. Enqueue is the end of admission; submission/worker activity can
begin within it. Worker start, complete headers, first accepted nonredirect
body bytes, wire completion, scheduler take, and JS delivery bracket distinct
boundaries. Foreground `first-body-callback` records arrival before quota or
delivery acceptance; it does not prove that those bytes were retained.
Redirects/retries can produce multiple worker records. Ordinary
Fetch currently delivers after the whole buffered body, so first wire bytes
are not first JavaScript-visible bytes. A response trace's numeric sequence
allows private correlation; none of these tokens is an authorization identity.

Full HTTP trace capture currently disables the scheduler's background worker
path. Use captured responses to reproduce page work, but do not treat a capture
run's network scheduling as ordinary browsing. For end-to-end device timing,
repeat with `trace_capture` unset, retain the validation timeline, and verify
nonzero background IDs at submission (completion clears the live worker ID).
Record this transport-mode difference when comparing runs.

### Load milestone timeline

`tools/load_timeline.py` reads whole validation logs (device or PPSSPP) and
splits boot -> interactive-ready -> first usable input (the first `until-met`)
-> send (the last input edge before `mark=sent`) -> first answer (the next
`until-met`; if the until timed out, the `answer` mark closes the window and
the report says NOT MET) into exclusive owner-thread categories, each naming
its evidence: `busy.*` (script, relayout, raster, pointer dispatch, and for the
load the `tilefinch-navigation-phases` totals), `wait.*` (the frame loop's
wait phase, split by whether a response sat ready untaken or a checkpoint left
jobs pending), `harness.probe`, and `unattributed`. Nested records (runtime
checkpoints, JS samples, fetch timelines) are listed as the window's longest
items, never added. Each window ends with what its records cannot
distinguish.

```sh
python3 tools/load_timeline.py device.txt              # one report
python3 tools/load_timeline.py run1.txt run2.txt       # + milestone table
python3 tools/load_timeline.py --compare ppsspp.txt device.txt   # B/A
python3 tools/load_timeline.py --json device.txt
```

`at-us`, `begin-us`/`end-us` and checkpoint `heartbeat-ms` are system time
since the PSP booted; `tilefinch-boot-timing elapsed` counts from main(). Over
PSPLink the two differ by the loader time (about 17 s); under PPSSPP by
almost nothing. The tool reports every milestone from main(). Never subtract
a boot-timing time from an at-us time: that is how a 13.4 s device
interactive -> usable window was once read as 31 s.

Offline replay's own file reads (`tilefinch-trace-replay-io`, cumulative at
marks) happen inside `busy.*` work. They are shown per window as
`harness.replay-io` bounds -- the I/O between the marks bracketing the window,
between marks inside it, and that bracket pro-rated by responses taken --
beside the categories, not subtracted from any one of them.

### Independent execution census

For attribution below the script split, host and validation builds support
`TILEFINCH_EXECUTION_CENSUS=1` (PSP boot key
`validation_execution_census=1`, default off). A separate jittered sampler
reads the browser owner's thread CPU counter roughly every 1–1.5 ms and
records the current VM opcode/helper or native callback address. It does
not depend on VM branch/call interrupt polls. Scoped helper tags restore
across nested calls and exceptions; native tags include CFunctionData
getters that ordinary native wrappers miss. No clock reads or allocations
occur at opcode dispatch sites. Shipping PSP builds omit the tags and sampler.

The sampler has fixed tables (512 native addresses; no eviction), a
Budget-reserved stack, and deterministic teardown. Its milestone records
report sample failures, CPU gaps over 20 ms left unassigned, outside-script
CPU, and native-table overflow. All rows are emitted, not just a top twelve.
Keep the measured ELF beside each log: callback addresses are relocated
using that binary's `JS_RunGC` anchor, not a newer build.

```sh
python3 tools/execution_census_report.py private-run.txt \
  --from typed --to answer --binary measured-psp-browser-script
```

Use `validation_js_profile=0` and `validation_script_split=1` for this
capture, then join the result to `load_timeline.py` and the per-checkpoint
script-split records. CPU bins, exact script wall time, and end-to-end wall
time have different denominators; never add their nested views together.
Mode `2` adds function identities (8,192), source labels (256), a bounded
joint function/family table (8,192), property/call path counts and records
for promise jobs lasting at least 100 ms. Identities restore across nested
calls and exceptions; lazy functions register after compilation. Tables
never evict and explicitly report overflow and unknown CPU. The fixed
diagnostic storage and sampler stack are Budget reserved; this is roughly
1 MiB in detailed mode. Job output happens after its exact wall timer closes
and is charged to diagnostic host bookkeeping, not the callback.

The `call-frame-*` bins separate ordinary bytecode-frame bookkeeping,
argument copying, local/reference-slot initialization and cleanup from
executing the callee. Cleanup includes its final-release helpers, but a
reentrant call gets its own execution attribution. These bins are parts of
the existing CPU total, not extra timings to add to the call family. The
matching path counts describe phase entries; `call-frame-simple` counts
normal functions with no argument copy or captured-local reference slots.
Counts do not establish their CPU cost or prove that initialization can be
omitted. Generator resumes reuse their saved frames and are not new setups.
Phase scopes themselves add diagnostic work, even around empty initialization
loops. Treat sampled frame shares as instrumented estimates, not exact release
cycle costs; use a sampler-off control for end-to-end timing.

Detailed mode also joins each sampled native body to its current bytecode
caller, both for cumulative marks and long jobs. The fixed 4,096-pair table
adds about 128 KiB of diagnostic storage; it never evicts and reports
`function-native-overflow-us`. Reports expose `native_functions` and
`native_unassigned_cpu_us` on each source-function row. These native times
are already included in the caller's native-family and CPU totals: do not
add them again or infer a missing caller from the job-wide native list.
Old logs without the join retain an explicit unassigned-native total.
`native_without_family_cpu_us` preserves discrepancies when the independent
function/family table loses samples that the native join retained.

For bootstrap source locations, generate private artifacts using
`TILEFINCH_BOOTSTRAP_KEEP_LINES=1` with the host bytecode generator. Configure
`TILEFINCH_CENSUS_BOOTSTRAP_BYTECODE=/absolute/private/bytecode.c` to use that
artifact on host or PSP validation only. Shipping PSP refuses this override;
the canonical bootstrap check still verifies the committed artifacts. Never
commit the private output. Keep a matching sampler-off comparison: line
tables change retained memory, so this is an attribution variant, not a
shipping-performance benchmark.

`PSP_BROWSER_JS_REFCOUNT_CENSUS=ON` is a separate count-only build. It counts
refcounted value API retains, releases and final releases per function and
long job; it does not count direct internal atom references. The additional
fixed counter storage is also Budget reserved. Do not interpret its sampled
CPU or wall time as the cost of reference counting. Restore this option to
OFF after collecting counts. Counts describe frequency, not time saved.

A sampled helper interval includes any untagged work inside it. The census
does not identify instruction/data-cache misses. Those need controlled
device comparisons, not relabeling samples. Raw captures stay outside the
public repository.

The shared `tilefinch-js-bench` kernels include matched `own_hot_int`,
`own_ring_int`, `own_hot_ref`, `own_ring_ref`,
`own_ring_ref_scattered`, `binding_move_int` and `binding_move_ref` probes.
The hot/ring probes execute the same four own-field reads with the same
array indexing, over either one receiver or 1,024 receivers; reference
values are either shared or scattered. Setup runs outside the timed
function, and each function checks its final result. The first timed call
can still include lazy function compilation; do not interpret the delta as
an exact cache-miss or refcount cost. Nothing allocates in the timed loops.
Use host `--only` or validation boot `js_bench_only=<comma-separated names>`
to isolate these kernels. The PSP filter is cleared from ordinary builds,
and the log prints the selected names. Match source, flags, iteration count,
loaded PRX hash and successful outcome; repeat hardware runs before drawing
conclusions. A kernel win is not a whole-page speed claim.
Explicit filters refuse unknown names, empty comma-separated entries and
more than 512 bytes before any kernel runs; `compile_trace` also requires a
trace directory. Duplicate names select a kernel once. This prevents an
empty or partially misspelled short run from becoming false timing evidence.

Always run the same binary and replay with the census off as well as on.
The tags themselves cost work on Allegrex: the initial hardware comparison
added about 12–15% to the reply window, versus about 2% on the host. Do not
call census-on timing shipping performance. For scheduling A/Bs, use a
wall-paced replay (`trace_replay_pump_us`) rather than changing the recorded
response delay by pumping faster; pump-paced captures cannot measure live
network latency.

### Where a frame's time goes

Validation builds also print one `tilefinch-loop-work` record per
`tilefinch-loop-timing` frame (same `begin-us`). It answers, for every
frame and not only for advances over 100 ms, how much of each phase was
the page and how much the browser:

- `PHASE=S/N/L/Z` for every phase that saw any: exclusive microseconds of
  script (JavaScript: page tasks, promise jobs, event handlers, compile and
  evaluate, the bootstrap glue they run), bridge natives called from script
  (only when the JS profiler wraps natives, `validation_js_profile=1`;
  otherwise inside script), layout (`navigation_relayout` and what it
  runs), and sleep (vblank and delay waits, transport polls). What the
  phase does not claim is browser bookkeeping: the event loop and
  scheduler, dispatch plumbing, hit testing, DOM refresh, cooperative
  checkpoints and their presentations (`src/work_ledger.c`, called
  `work_ledger_enter`/`leave` at those sites; nested kinds are exclusive,
  so a relayout forced by script is layout).
- `step=INDEX:KIND`: the input-script step the frame served. A fixed-length
  step (`wait`, `stick`, `press`, `hold`, `mark`) paces frames by count
  whatever the page does.
- The page realm's runnable state sampled as the frame began, before its
  wait: `timers`, `due-us` (earliest timer's due time minus now; negative
  is overdue) and `due-kind` (`frame` for requestAnimationFrame and the
  observer frames, which run at a rendering opportunity; `task` otherwise),
  `task-due-us` (the earliest timer that is not a frame callback, `none`
  without one; a due frame at the head cannot hide it). These are in the
  scheduler's time, which on the PSP is a virtual clock: each runtime
  advance moves it by the wall time since the previous advance, at most one
  16 ms tick (include/tilefinch/runtime_clock.h), so a busy frame still
  moves page timers by one tick only,
  `jobs` (a microtask checkpoint left pending), `ready` (complete responses
  not yet delivered), `flight` (requests in transport), `cont` (a segmented
  classic script's next slice). `task-yield=1` marks a frame whose wait
  was the short page-work yield instead of a vblank.
- The frame's runtime advances: `advances`, `advance` (wall), `realm` (the
  page realm's `script_runtime_advance`), its `prelude`, `timer-prepare`,
  `tasks=COUNT/US` (timer tasks and frame callbacks), `microtasks`,
  `network` (transport pump and deliveries), `refresh`; then `child`
  (child-frame realms), `setup`, `messages`, `relayout`, `damage`;
  `late=SUM/MAX/COUNT` (how late timer tasks ran after their due time, and
  how many a whole frame late) and `frame-callbacks=COUNT/LATE_US`;
  `deferred=RENDER/OTHER` (advances skipped behind a pending render or a
  handoff).

Offline replay also prints `tilefinch-fetch-timeline: stage=replay-admit ...
delay-pumps=N release-us=T` when it admits a request: the response is held
for the N scheduler pumps (one per runtime advance) its capture took, which
models the recorded server and transport time in frames, not in seconds. A
loop that pumps more often therefore also makes the recorded network answer
sooner. For an A/B of frame pacing, set `trace_replay_pump_us=16667` in
boot.cfg (host lab: `TILEFINCH_REPLAY_PUMP_US`): each response is then held
for N x 16.667 ms of wall time (`release-us`) and at least one pump.

With these records `tools/load_timeline.py` splits each window's runtime and
input phases into `busy.script`, `busy.native`, `busy.relayout`,
`wait.runtime-sleep`, `busy.runtime-other` (bookkeeping) and their
`busy.input-*` counterparts, and each frame's wait by what would have ended
it sooner: `wait.work-due` (a task, response, job or continuation was already
runnable, or a task came due during the wait: the loop's own delay),
`wait.network` (requests in transport, nothing due), `wait.timer` (the
page's next timer or frame callback), `harness.scripted-wait` (nothing
runnable while a fixed script step counted frames) and `wait.idle`. It also
prints an owners line (page script, layout, rendering, our bookkeeping, our
avoidable sleep, page timers, network, harness), the runtime-advance
breakdown, timer lateness, the waits by reason, and the requests replay held
for their recorded pumps.

### Causal canvas deadline capture

In validation builds, `webgl-measure-start` begins an additional bounded
capture and `webgl-measure-end` ends its window: presentation counts, the
over-34-ms tally and the worst pipeline stop there too (the end mark's own
frame carries that mark's diagnostics and is not counted). The counters
reset at the main loop's next frame after `webgl-measure-start`, so the
start mark's own diagnostics (page report, `.mark.js`) stay outside the
window too; the page's `__tilefinchStartInputProfile` hook still runs on the
mark frame, before any `.mark.js` for that mark. Use `mark-page` for both
marks in timing runs. Scripts with `until` steps keep their `until-met` line
in goldens without its `at-us` clock. The final report contains
one `tilefinch-canvas-frame` row for every stored presentation in that
window (up to 1,024), not merely the sixteen longest pipelines. Rows carry
absolute runtime-start, framebuffer-ready and post-vblank timestamps,
the corresponding display vcounts, and the previous canvas publication.
Compare the vcount delta with the desired two-vblank cadence; do not infer
a missed deadline from `present` time, which includes waiting. Join the
runtime-start timestamp to `tilefinch-loop-timing` to account for input,
dispatch, idle and post-present work. Runtime contains callbacks and
microtasks; native WebGL is nested inside microtasks, not additive to them.
Loop-timing rows inside this window are buffered with their presentation,
not printed live: USB log flushing can otherwise delay the next frame by
several milliseconds. Exclusive `loop-work` text is suppressed in this
window; the joined native/game phases provide the attribution instead.
Routine runtime-checkpoint tracing and per-frame publication text are also
disabled in this window. Other log records are retained whole in a 32 KiB
validation-only buffer; heartbeat flushes wait until measurement ends.
`tilefinch-timing-log` reports retained bytes, refused records and explicit
flushes. Require `dropped=0 forced-flushes=0` for a clean timing capture.
Crash/watchdog records and explicit failure/checkpoint flushes remain enabled;
such a flush contaminates the timing run rather than being silently hidden.
`canvas-trace-live` before the start mark restores the former live tracing
for a same-binary overhead control; `canvas-trace-quiet` restores the default.
The buffer still formats occasional input diagnostics and uses the log lock,
so this removes I/O and routine trace work, not every validation instruction.
Each presentation also has a `tilefinch-canvas-cpu` row with the runtime's
thread-run microseconds, successful sample-pair count, interrupt/thread
preemptions and voluntary releases. These are firmware thread counters,
not instruction-cycle counts. Require `samples` to equal the frame's
runtime `turns` before interpreting wall-minus-CPU time; a failed firmware
query is unavailable evidence, not zero CPU work. The two boundary queries
add measurement overhead. `tilefinch-canvas-raster` separately records
successful raster CPU sample pairs and per-frame deltas of the fast canvas
conversion and overlay counters. Require its `samples` to match `slices`;
conversion and overlays are nested in `render`, not additional frame work.
The residual also includes fast-path preparation or a non-fast render slice.
`tilefinch-canvas-copy` records successful CPU accounting for base-frame
copy/clear, plus the actual byte count. Require `valid=1`; its CPU time is
nested in `base`, and wall-minus-CPU includes preemption and query overhead.
It does not cover publication or the vblank wait.
`tilefinch-canvas-conversion` identifies the last successful fast conversion's
actual source address/stride, dimensions and kernel/general/reused row counts.
Require `samples=1` before treating those row counts as a whole-frame account.
This distinguishes a slow conversion kernel from falling off its fast path.
Ordinary Allegrex builds use the pixel-exact VFPU color-packing kernel on
16-byte-aligned rows, retaining the scalar path for other source alignment.
Validation builds use the same kernel by default. `canvas-vfpu-off` selects
the scalar kernel in the same binary for an A/B run, and `canvas-vfpu-on`
restores the VFPU kernel. `canvas-vfpu-check` runs the bounded
pixel/canary/register/prefix probe outside the timing window: require
`available=1` and zero pixel and state mismatches. A failed check turns the
VFPU kernel off (and `canvas-vfpu-on` refuses it) until a later check passes.
Verify `vfpu` row counts in the conversion records: for an aligned 320x180
canvas using the 3:2 fast path, require 180 VFPU rows per applicable
single-conversion frame.

The base-frame copy follows the same rule. Validation builds copy with the
VFPU on its owner thread, as shipping builds do. `copy-vfpu-off` selects the
scalar copy and `copy-vfpu-on` restores the VFPU copy. `copy-vfpu-check`
compares every VFPU copy against the scalar copy and turns the VFPU copy off at
the first mismatch. `copy-vfpu-stat` prints `tilefinch-copy-vfpu: copies=
checks= errors=`.
Both retain the canonical RAM
frame and back-buffer-only publication. Ordinary builds contain neither the
probe nor these controls. This saves about 0.28-0.30 ms of conversion CPU in
three same-binary device pairs; it does not eliminate gameplay deadline misses.
For intrusive audio attribution, mark `canvas-audio-on` before
`webgl-measure-start` (`canvas-audio-off` is the default). Each
`tilefinch-canvas-audio` row records calls and inclusive boundary time for one
native command. This includes argument collection and service, not the
JavaScript graph/scheduling work surrounding it. Reentrant native calls belong
to their outer boundary and are reported as `nested`; do not double-count.
These per-call clocks deliberately add overhead. Use a clocks-off control
for deadline incidence, and compare composition only in the intrusive lane.
Do not charge preempted wall time to the JavaScript or conversion phase
that happened to span it.

Audio CPU overlapping a runtime turn is not the cost of one output block.
For block accounting, use `audio-mix-start` / `audio-mix-stat` outside the
start/end measurement marks. `tilefinch-audio-block` reports completed
512-sample blocks, mixing and output-submission thread CPU separately,
maximum block CPU, firmware-query overhead, failed samples and dropped
records. Work rows partition actual voice-samples into silent/audible and
constant/automated; fast-forwarded samples are a subset of silent samples.
The CPU histogram uses 100 us bins (the last includes overflow). These
intrusive counters are off by default and absent from ordinary builds.
Do not subtract the entire observer duration from either CPU interval.
`tilefinch-audio-block-records` reports the bounded record count and overflow;
each `tilefinch-audio-block-record` retains before/after brackets for the three
CPU queries, absolute audio-thread CPU counters, output sample position, work
counts, and the actual firmware output result. Join these to frame timestamps,
report blocks crossing boundaries separately, and never charge the entire CPU
of a crossing block to both frames. Overflow, unavailable clocks, or dropped
records invalidates detailed attribution. Records are flushed after measurement,
not while the mixer runs. No individual sample has a timing query.

`tilefinch-canvas-webgl` copies existing translator counters at runtime batch
boundaries without sorting samples. It splits command wall time into decode,
cache, vertex, state, emission, AA, writeback and finalization, with draw/vertex/
cache/state/list counts. Matrix and tint clocks are nested inside vertex work;
native WebGL is nested inside runtime/microtask work. These are wall clocks,
not owner CPU, and may include audio preemption. `invalid` must be zero.

`audio-mix-probe` runs outside gameplay with no hardware output. It compares
each PCM block and every voice state on a fixed four-voice command corpus,
then alternates reference/optimized mixing six times with work counters off.
Setup, command scheduling and PCM comparison are outside the timed mixer
intervals. `audio-mix-off` / `audio-mix-on` select the same-binary paths;
ordinary builds contain no switch. Record corpus CPU per block separately
from gameplay deadline incidence, and require zero PCM/state mismatches.

`audio-const-probe` isolates constant-state block specialization from silent
oscillator fast-forwarding: the latter stays enabled for both variants.
It uses the same per-block PCM and complete voice-state checks, then six
alternating 256-block runs with work counters disabled. `audio-const-off` /
`audio-const-on` select that specialization in validation builds (on by
default, as shipped); ordinary builds use it without a switch. Admission excludes blocks
containing starts, stops, unfinished automation or nonlooping completion.
Looping-buffer wrap, voice order, integer rounding and final clipping remain
unchanged. A corpus CPU saving is not a gameplay deadline claim; qualify the
recorded-frame workload separately, with output muted but mixing active.

`audio-auto-probe` is the same comparison for the native automation path
(gain and pan curves), and `audio-auto-off` / `audio-auto-on` select that path
in validation builds.

Presentation and WebGL A/B marks select the same binary's previous path for
a device comparison. Validation builds start on the shipping path, and
ordinary builds have no switch:

- `canvas-ge-off` (or `canvas-cpu`) publishes canvas frames through the CPU
  copy instead of straight from the graphics engine, and `canvas-ge-on`
  restores GE-direct publication. `canvas-ge-verify` publishes GE-direct
  while comparing every published frame with the CPU copy.
  `canvas-ge-stat` prints `tilefinch-canvas-ge:` with presents, refusals,
  materializations and the verify counts.
- `webgl-tint-off` gives every colored instance its own vertex slice instead
  of sharing one per tint, and `webgl-tint-on` restores sharing.
  `webgl-direct-off` turns off direct geometry draws, and `webgl-direct-on`
  restores them. `webgl-tint-stat` prints `tilefinch-webgl-tint:` with
  instances, slices, direct draws and retired geometry.

For intrusive competing-thread attribution, issue `canvas-thread-on` before
the measurement window (`canvas-thread-off` disables it). Discovery checks
firmware results and reports the thread inventory, selected slots and omissions;
at most eight browser, callback/watchdog and USB peers are sampled. A missing
or retired thread is unavailable, not zero CPU. `tilefinch-canvas-peer-cpu`
joins each valid peer delta to its presentation ordinal.

The runtime wall timer excludes peer sampling. Owner CPU queries carry matching
wall bounds: `wall-min`/`wall-max` contain the actual firmware counter interval,
and `observer` is their uncertainty width. Compute off-CPU time from those
bounds, not a differently bracketed runtime timer. Peer `span` reports sampling
duration; `extra` bounds the peer window outside the owner queries. Neither is
a blanket correction to subtract from off-CPU time. Report the remaining
unexplained interval and use a clocks-off control for actual deadlines.
`causal-loops-without-sample` exposes loops that had no retained canvas row.
The first 64 such loops also emit `tilefinch-canvas-loop-gap`, retaining
their timer head/due time, fast-followup decision, callback count and wait
duration. Join those timestamps between consecutive publications to
distinguish a work overrun from an empty rAF turn followed by an extra wait.

A private instrumented page may publish one preallocated 96-word 32-bit
typed-array packet as `__tilefinchValidationFrameMetrics`. The native
observer copies the own data property after publication, without executing
getters, proxies or JavaScript. Short/detached views are refused. The
report pairs `tilefinch-canvas-game` with the same presentation ordinal and
the page's `performance.now()` origin. Packet schema and phase labels
belong to the capture; reject unknown versions, stale frame IDs or timestamps
outside the corresponding runtime/publication window. The first 512
packets are retained in fixed validation-only storage (192 KiB); overflow
never wraps onto old frames. Records are formatted after measurement,
not inside the deadline-critical path. `packet-read` reports observer cost.

Always bracket a deeply instrumented capture with clocks-off runs using
the same binary, pack, seed and real input sequence. Keep gameplay and
killcam/menu intervals separate. Intrusive phase clocks can turn otherwise
successful frames into misses: use controls for real lateness and detailed
packets for work composition, not the intrusive miss rate as product FPS.
Use fixed-step operation counts to screen host candidates; host microsecond
timings are not a PSP deadline oracle. A counter-only packet avoids inner
phase clocks but still costs instrumentation and packet construction, so
it is not a replacement for the clocks-off control.
An offline fixture must preserve the qualification query on its original
source URL; a completed input script without successful setup markers is
not a valid performance run.

Validate generated marker JavaScript syntax on the host before a device launch,
and reject a capture if any `tilefinch-input-script-mark-js` row reports `ok=0`.
Successful script completion or gameplay alone does not prove profiler switches,
heavy-scene setup, or measurement start/end controls ran. Verify the loaded
artifact hash after the wrapper's build as well as before it; a rebuild between
hashing and launch must invalidate the comparison.

For finer game attribution, record cache misses by both goal and completed
geometry revision (including warm-up), split field initialization from BFS
traversal, and separate bank-shot arithmetic from exact wall/smoke checks.
Record audio lookup hits and parameter scheduling spans alongside the native
command boundaries. Keep an explicit residual for each parent rather than
assuming a phase name describes everything inside it: the game prelude may
include sound and particle updates. Observer packing and callback tail costs
are separate from game phases. Check simulation/work-count identity before
using an instrumented source, and retain a clocks-off control of that exact
pack when observer source differs from the ordinary light packet. Never
subtract differently evolved live-run medians as a precise saved-time estimate.
For matched gameplay comparisons, record the admitted per-frame simulation
step, canonical input and time-dependent events, then replay from identical
initial state and check each frame's simulation result. The same seed and
physical button script alone do not guarantee identical work when elapsed
time or rendering cadence changes.

### Native hierarchical game phase records

Validation builds expose `__tilefinchValidationPhase(op, id, ...)` for a
private instrumented game. This is not a browser API and is absent from
ordinary PSP builds. Configuration allocates one Budget-owned capture;
there are no per-frame allocations or log writes. It retains at most 512
frames, 32 numeric phase IDs, 32 counters and 12 nested scopes. Any overflow,
invalid clock or unbalanced scope invalidates the entire capture instead of
discarding inconvenient frames.

Operations are configure (`0`, mode), begin frame (`1`, sequence), enter
(`2`, phase), leave (`3`, phase), and end frame (`4`, checksum A, checksum B,
preallocated `Uint32Array` counters). Modes are coarse frame clocks (`1`),
counters without clock reads (`2`), and hierarchical clocks (`3`). The
`phase-stat` mark formats records after the measured window. Each timed scope
reports inclusive wall/owner-thread CPU, exclusive CPU after subtracting its
children, calls and clock-query uncertainty. Observer durations are reported,
not subtracted wholesale; uninstrumented and counters-only comparisons are
still required. Missing native/render phases remain unattributed work, not
zero-cost work.

Host and validation builds also expose `__tilefinchValidationHashBytes(view,
byteCount, outputUint32Array)` for intrusive parity checks. It hashes at most
512 KiB without JS byte loops or intermediate allocations. Hashing and
`readPixels` belong to a separate correctness run, never a deadline run.
Require changing rendered output and per-frame parity, not merely matching
hashes of an unchanged canvas. Record HUD admission and other time-dependent
decisions when replaying identical work. A host report timeout is not device
completion; do not restore served assets while a proof is still running.
On PSP, `__tilefinchValidationRenderHistory(restoreBoolean)` snapshots or
restores the current realm/canvas's temporal antialiasing matrix for that
proof. It does not restore EDRAM ownership or an old display epoch. Restore
CPU staging arrays and separately published HUD/mesh buffers as well: a
partially rebuilt HUD is not equivalent to its last uploaded buffer. Keep
these setup/proof operations outside the timed window.
Hold the simulation at a completed replay until the final check is read;
otherwise live frames between the completion predicate and the mark can
invalidate that check. `canvas-stat` dumps the current publication/deadline
window before the next reset, while `phase-stat` dumps the corresponding
hierarchical records. Measurement marks follow the current frontend session,
including an initial navigation committed after the boot handoff.

For an identical-work replay, begin with clocks and detailed counters disabled.
`phase-select-misses` selects the actually late publication sequence numbers
through the private game's `fixedSelectFrame` hook after that window. The hook
may include nearby ordinary controls; it must not evaluate script in a timed
frame. Enable fine scopes only on those selected sequences. Export recorded
gameplay/killcam/transition labels through the numeric capture after the window,
not through a rate-limited console log. Cold geometry initialization is a separate
category. An empty, disabled game-phase capture is not a timed sample.

`runtime-cpu-on` / `runtime-cpu-off` control a second, batch-only owner-thread
CPU tree. `tilefinch-canvas-runtime-cpu` reports inclusive/exclusive/call triples
for advance, event preparation, callback dispatch, jobs, native WebGL and DOM
refresh. Native WebGL flushes nested inside jobs are children, not extra siblings.
Game scopes use the same thread clock. Reconcile their exclusive sum plus the
game remainder with the callback; keep dispatch outside the game explicit.
Reconcile the native tree with script advance and retain the difference between
script advance and the outer browser runtime as DOM/layout/damage plus an
unattributed remainder. Never obtain JavaScript CPU by subtracting WebGL wall
time. Report bracket uncertainty and matched observer-mode deltas separately.

`audio-work-off` disables per-voice/sample counting while retaining audio block
CPU clocks; `audio-work-on` enables those counts. Change this switch only outside
an active audio measurement. A zero counter in clocks-only mode means unmeasured,
not silent or absent work. `audio-count-probe` alternates counting off/on over
identical commands, checks exact PCM and subsequent voice state, and prices that
observer independently of gameplay. For consecutive valid block records, add
the CPU gap from the previous submission to the next block's beginning to mix
and output CPU. This full span must reconcile with the block thread clock.
Bound blocks crossing frame edges rather than charging each full block twice,
and compare the coverage with independently sampled audio-thread CPU. Native
audio state is not rewound merely by restoring the game's simulation.

For each genuine missed sequence, compare a nearby ordinary sequence with the
same mode, live tanks and wave. Report differences in actual candidates, exact
tests, navigation cells, uploads, glyphs and cache paths alongside exclusive CPU
differences. Retain ambiguous legacy counters as ambiguous; a combined sound/
route counter is not evidence of a route search. If a broad unexplained branch
dominates, split that branch next rather than instrumenting every small helper.
Detailed-run deadline misses are observer effects until confirmed by the minimal
run. Audio can also disturb the owner's caches; subtracting its independently
measured CPU does not account for that secondary effect.

Validation marks `canvas-followup-off` and `canvas-followup-on` compare the
canvas timer-remainder wait in one binary. Ordinary builds always use the
wait: after a successful canvas publication, a fast followup whose virtual
frame timer is not due sleeps only the remainder needed for the next
admitted clock tick. It does not consume the clock while predicting, catch
up missed ticks, spin, or alter NEXTFRAME publication and buffer ownership.
Timers beyond one tick and non-frame timer heads keep the existing policy;
native menus, navigation and media do not take this canvas shortcut.

### What script time is made of

"Script" above includes the synchronous host work page script asks for.
Validation builds (and host builds) split it without the JS profiler and
without its native trampoline (`include/tilefinch/script_split.h`): a few
bridge natives time themselves through a plain C wrapper installed in
place of the native, so calls keep the engine's direct native path.
Exclusive kinds: `style` (getComputedStyle and the transition snapshot),
`query` (querySelector/All, closest, matches), `mutate` (setAttribute,
removeAttribute, insertBefore, append, remove, style writes, innerHTML),
`fetch` (request setup in `__tilefinchFetchAsync`, which in offline replay
includes reading the trace record), `layout` (a synchronous layout a native
forced), `compile` (lazily defined function bodies compiled on first call,
through the engine's lazy-compile hook, and compiles script starts),
`gc`, `host` (UI and input service inside VM polls) and `js`: the
interpreter, built-ins and every untimed native. `js` is further divided
at each VM poll (about every 10,000 work units) by the innermost bytecode
frame: `bootstrap` (stripped bootstrap functions), `native` (a built-in
polling from its own loop, such as a regexp) and `page`; that part is a
sampled estimate, the timed kinds are exact.

Each input-script mark (live ones included) and each lab `work` command
logs the cumulative totals as `tilefinch-script-split: label=L at-us=T
js=... page=... bootstrap=...`, and every `runtime-checkpoint` record ends
with `split={...}` for the checkpoint and `longest-split={...}` for its
longest promise job. `tools/script_split_report.py LOG --from typed --to
answer` prints a window's split and its longest checkpoints; in the lab set
`TILEFINCH_TRACE_RUNTIME_STEPS=1` for the checkpoint records. Built-ins
inside `js` are not timed (a clock read per native call would cost more
than many of them); the PPSSPP PC sampler's self time splits them from the
interpreter.

The split's own cost is measurable in one binary: `boot.cfg`'s
`validation_script_split=1` keeps the timed kinds without the per-poll
frame sample, `=0` turns the split off (`TILEFINCH_SCRIPT_SPLIT` in the
lab). On the chatgpt-ask PPSSPP replay (2026-09-29) the three modes' mark
times and script totals are recorded in the performance ledger.

### Fast buffered reply experiments on the host

Keep response captures and their request-ID adaptation commands outside Git.
`scripts/run-buffered-reply-replay.py` builds the optimized interactive lab,
repeats a supplied private journey, and refuses a result unless the last JS
diagnostic returns a JSON object with a nonempty `answer` containing the
expected text, the journey succeeds, and teardown returns ownership to zero.
Repeated final framebuffers must match. For example:

```sh
python3 scripts/run-buffered-reply-replay.py \
  --responses /private/path/responses --commands /private/path/send.commands \
  --url https://fixture.example/ --expect-text 'Expected reply' \
  --output-dir /private/path/new-experiment --runs 3
```

The private command file's final diagnostic should read the actual reply DOM
and return `JSON.stringify({answer: reply.textContent})`; do not substitute
captured text as the assertion. A success exit alone can hide a mismatched
capture or an empty assistant node. The runner records capture/command hashes,
Git state, pixel hashes and timing summaries. Every invocation rebuilds; its
output directory must be new so prior evidence is not overwritten. This is a
48 MiB/10 MiB JS host-throughput fixture, not a PSP memory or transport test.
Changing session/request identifiers in the private replay adapter is solely
fixture normalization; live requests and browser policy must remain unchanged.

#### Fast screening versus hardware confirmation

Use buffered host replay for the inner edit loop: responses are already
available, page time advances without sleeping, and a batch builds once then
runs several fresh realms. Set `TILEFINCH_REPLAY_PUMP_US=0` explicitly for this
throughput mode. Do not combine fixed-wall response delays with thousands of
accelerated timer ticks: the page can time out before the response is released.
Keep the fixed-wall replay configuration for the separate PSP confirmation.
The runner defaults script-split timing off and records an explicit override;
keep sampling and execution census off for timing, too. Use separate runs for
attribution rather than comparing a profiled host run with an unprofiled PSP.

Place `work send-begin` **before** the activating click, and `work answer` after
the reply completes. Add these options to the command above:

```sh
--window-labels send-begin answer \
--require-work js.module_compiles=EXPECTED_COUNT \
--require-work js.module_restores=EXPECTED_COUNT
```

Replace the expected counts with the captured workload's actual values. Match
eager/lazy compilation, bootstrap mode, module-cache state and PGO explicitly;
for eager compilation use `TILEFINCH_JS_LAZY_FUNCTIONS=0`. The manifest records
process wall time, the buffered command window, executable/core-library hashes,
QuickJS flags and work-vector differences. Nested runtime/relayout timers are
not added to the command wall total. Required work counters fail closed;
non-required differences remain visible and prevent an identical-work claim.
The actual answer, final pixels, author-error diagnostic (when supplied) and
zero-owned teardown remain mandatory checks.

Qualified completion-bounded host reply batches took about 1.2–1.6 s per run,
with 0.38–0.53 s in the activation-through-reply processing window. A same-binary wrapper-reuse A/B
was flat/worse on both host and PSP, so the fast loop could have rejected that
candidate without six more full hardware journeys. This is one negative
calibration case, **not** proof of a universal speed ratio or percentage-gain
predictor. ARM64/64-bit heap sizes, hardware caches, soft-float, executable
placement, PSP scanout and real networking differ. Host memory limits in this
fixture are intentionally larger and do not qualify PSP admission/refusal.

Reject broken or clearly slower candidates on the host; retain the raw result.
Take promising changes to a matched, sampler-off, same-binary PSP A/B before
claiming device gains. Accumulate positive and negative calibration cases by
subsystem; one matching DOM result does not validate interpreter or GE changes.
Do not repeatedly rebuild/re-baseline the fixed PSP control for host edits.

An existing host or PSP `tilefinch-work:` log can supply an explicit endpoint
contract instead of copying its counter values into the command line:

```sh
--reference-log /private/path/reference.log --reference-label answer \
--match-work js.module_compiles --match-work js.module_restores \
--match-work dom.mutations --match-work dom.mutation_records
```

The reference must have one unique endpoint record and each named field must
be an available integer. Missing/duplicate records, `n/a` and mismatches fail
before accepting a run; an incomplete reference fails before building. Every
run records all other endpoint differences as well. These are **cumulative
counts at completion**, not Send-window deltas or evidence that all executed
bytecodes match. Keep the reference build/configuration manifest alongside its
log; the runner records the log hash but cannot infer its hardware provenance.
Do not silently normalize missing device counters or call a partial match full
host/PSP equivalence.

Match page preferences as well as the compiler configuration. The runner
accepts `--content-blocker basic` and `--user-css /private/path/presentation.css`
and records the blocker mode and stylesheet content hash. For example, a
device profile using 125% text must not be compared silently with the lab's
100% default; use the same font-scale and stock cosmetic/consent rules. CSS is
bounded to 64 KiB. These options retain the lab's existing behavior: its user
stylesheet is applied after initial load, whereas the PSP frontend installs
presentation CSS before navigation. They bring reply rendering closer but do
not qualify initial-load parity. Recheck pointer coordinates after changing
text scale; a missed Send button must fail completion rather than become an
apparently fast sample.

For a qualified PSP capture, add `--reference-frame /private/path/answer.ppm`
and `--match-region X Y W H` to require exact RGB pixels in a shared-page
region. Identify that region from the real capture and include the visible
answer, not an empty background; native chrome and its clock may be outside
it. Missing/truncated/oversized PPMs, out-of-bounds regions and pixel mismatches
fail closed. The manifest retains the reference image hash, region and region
hash. This is an additional visual contract, not a substitute for full-reply
DOM checks or proof of identical VM work. Do not widen exclusions to conceal a
rendering regression.

Use a three-level loop: short operation-family probes to reject a mechanism,
the completion-bounded host journey to check real-page effects, then physical
PSP confirmation only for surviving candidates. Host semantic/bridge work is
shared with PSP; cache-sensitive VM/code-layout changes, soft-float, memory
admission and scanout need hardware evidence even when the endpoint contract
matches. One-time paired calibration is reusable until its source/configuration
premise changes; it is not a universal seconds-to-seconds conversion.

For a closer completion boundary, replace the post-Send fixed `tick` tail
with `until MAX-TURNS MS EXPRESSION` in the private lab commands. For example,
`until 6000 16 document.getElementById('reply').textContent.length > 0`.
Use the same actual-DOM predicate as the device journey, not a predetermined
tick count or copied response string. The lab checks before advancing, then
after each runtime turn, and stops on the first match. The normal command
tail paints that state. A throwing predicate, missing runtime, or exhausted
turn cap fails the journey. Bounds are 1–10,000 turns and 0–60,000 ms/turn.
First nonempty text and complete reply are different milestones in a streamed
response. Keep a separate full-reply diagnostic; if the experiment measures
completion, its predicate must wait for the complete expected DOM text or a
real completion signal. Early pixels need not match a later settled frame.
Like the device probe, this uses the synchronous diagnostic path without an
extra microtask checkpoint. Predicate work is still observation overhead;
keep it small and identical across A/B runs. Timer cadence, memory admission
and scanout remain separate hardware checks even when reply work matches.

The manifest also records whole-process child CPU time where the host supports
it (`process_cpu_us`; otherwise null). This includes load and teardown, not just
the Send window. Compare it alongside process wall time to distinguish host
scheduling noise from a throughput change; never substitute it for window time
or for device CPU attribution.

Use this loop as a screening tier: build once, run alternating fresh-realm
controls/candidates, and require the same answer, pixels and admission counters.
Retain the settled-work check separately from first/complete reply. Confirm a
positive result in an independent batch before spending a hardware run. A neutral
host result does not reject an explicitly PSP-specific cache or soft-float
hypothesis. Shared engine code and matching output establish functional
confidence, not a universal ARM64-to-Allegrex timing ratio; physical confirmation
is still required before promoting a claimed device speedup.

Validation device logs now split pointer (all phases) and submit dispatch into
handler, promise-checkpoint and document-refresh time. Timed checkpoint records
include count/total/maximum job duration and pending state; device records below
1 ms are omitted. `analyze-input-timeline.py` reports these under
`nested_not_additive`: they overlap outer input/action/runtime totals. Partial
window crossings are flagged, not charged in full. These records distinguish
many short jobs from a single long job without altering checkpoint ordering.

For callback-level host attribution, run the same private replay with
`TILEFINCH_TRACE_JS_PROFILE=1 TILEFINCH_JS_PROFILE_OUTLIER_US=1`.
`TILEFINCH_DISABLE_BOOTSTRAP_BYTECODE=1` additionally gives bootstrap source
locations; compare against normal bytecode too, rather than treating the
source-mode run as a device timing. Then join the records:

```sh
python3 tools/promise_job_report.py /private/path/run-1.log \
  --after 'label=after-click samples=' --before 'label=advance-1 samples='
```

Every job has its own monotonic identity and wall/sampled/native/GC/cooperation/
profiler totals. Unframed time is a **subset** of sampled time; lazy compilation
counts and bytes are not additional time buckets. The report preserves the
unassigned residual and printed-sample coverage. Root samples whose 64-frame
walk was exhausted are explicitly marked incomplete and never named as a
callback. A stack can include first-call compilation charged to its caller;
function samples alone do not distinguish compilation from execution.
Low-threshold tracing adds measurable overhead and private log volume: use
the reported profiler time and uninstrumented matched runs for speed claims.

Use `-text` for keystrokes meant for the on-screen keyboard. Those steps
advance only while the keyboard samples input; main-loop frames hold them.
On slow pages the keyboard opens seconds after the activating press, and a
fixed wait lets the first keystroke reach the page as a second activation.

`BUTTONS` joins names with `+`: `up`, `down`, `left`, `right`, `cross`,
`circle`, `triangle`, `square`, `ltrigger`, `rtrigger`, `start`, and `select`.
The parser accepts at most 256 steps, 19 characters per mark, and 8 KiB per
file. The boot key accepts only a leaf filename—no separators or `..`.

`hold` holds one chord continuously for the requested frame count. `press`
emits that many distinct press/release pairs. `stick` accepts `up`, `down`,
`left`, `right`, `up-left`, `up-right`, `down-left`, or `down-right`.
An optional button chord combines nub position and face buttons for Danzeff
entry. Validation builds feed the same script into the modal Danzeff input
loop, including its initial release gate; ordinary builds still read only
the physical controller. For example, `stick 1 right cross`, `wait 1`,
`stick 1 down-left cross` types `ps`. Finish with `tap rtrigger+start` to
exercise real form submission rather than injecting a URL.
Counts are frame-counted, not time-counted; avoid using
them to qualify shortcuts whose meaning is intentionally millisecond-based.

`wikipedia-native-priority-live.txt` exercises Select, Settings navigation,
Triangle, and nub motion during a committed page's reflow. Require
`busy-menu` to show `screen=menu`, `busy-settings` to show `screen=options`,
and `toolbar-hidden` to report `visible=0`; a completed script alone is not
success. The old queued path completed while dropping six of these inputs.
Presentation-only navigation runs on the owner thread's retained-frame
supervisor; page-changing actions and chords remain with the main receiver.
Native menus defer new page runtime/resource/raster work. An in-flight
operation still reaches a safe checkpoint before input can be serviced.
Correlate `max-ack` with `max-gap` and `tilefinch-cooperate-slow-gap`: the
former measures presentation after sampling, not physical press-to-display
latency. Live script steps are themselves checkpoint-driven.

The script does not infer semantic readiness from pixels. Use ordinary steps
when input must wait for the browser's ready boundary, `-page` for a painted
document with pending work, `-live` for input within ongoing work, and correlate `mark` records
with the operation journal to prove the state the scenario reached.

`youtube-comments-play-live.txt` starts on a provider watch URL, expands
Comments with D-pad input, returns focus to the video card, and activates it.
It is a live diagnostic, not a network-dependent golden. Check that
`comments-link` targets `tilefinch_view=comments`, `comments-open` reports that
committed URL, and `play-target` identifies `class=watch-target` with the
canonical watch URL. The `played` and `progressed` marks alone do not prove
playback: require decoded frames and an advancing media clock in the media
log, with zero state-machine mismatches. PPSSPP can reach the known missing
`flash0:/kd/mpeg_vsh.prx` boundary; this qualifies the input/handoff only,
not firmware playback. Do not bless that failure as a playback golden.
The hermetic `--provider-navigation-only` host test independently covers
Play from both expanded Description and Comments documents, including the
canonical URL, native-player action, and unchanged backing generation.

Two records close the timeline's former attribution gaps (validation
builds): `fetch-wait=` on `tilefinch-page-slow-advance` and
`tilefinch-navigation-phases` is the owner's time blocked in synchronous
fetch waits (module imports, byte-pool capacity), reported as
`wait.fetch-sync`; `tilefinch-relayout-full-phases` splits each full
relayout into prepare, stylesheet, external-css, css-events, images, fonts,
layout and attach, with the flags that forced it (style-rebuild,
resource-rebuild, overflow, conservative, append-tried).

### Engine micro-benchmark before boot

`validation_js_bench=N` runs the JavaScript engine micro-benchmark
(src/js_bench.c, the host's `tilefinch-js-bench`) before the first HOME
frame at iteration scale N, then boots normally so any input script (the
minimal `js-bench` scenario) ends the run. `js_bench_dir=DIR` names an HTTP
trace beside the EBOOT whose JavaScript records the bench compiles; it
defaults to the replay trace when one is configured. The report is the
run's `tilefinch-js-bench:` lines (one per kernel, then `outcome=complete`).
`scripts/js-bench-device.sh` stages the boot.cfg for a host0 device run;
under PPSSPP pass `--script js-bench --measure --boot validation_js_bench=1
--trace DIR` at `TILEFINCH_PPSSPP_CPU_MHZ=333` (the runner's native-HOME
boot gate fails afterwards because the bench key is a validation key; the
lines are in the run's tilefinch-validation.txt). Shipping builds ignore
both keys.
Each bench line ends in `at-ms=`, the monotonic clock when it was printed,
so a kernel ran between the previous line's stamp and its own.

### Program-counter profiles under PPSSPP

`tools/ppsspp-pc-profile.sh OUT.tsv -- RUNNER-ARGS` runs one scenario with
PPSSPP's WebSocket debugger (`TILEFINCH_PPSSPP_DEBUGGER_PORT`, default
45123) and samples the emulated program counter (`tools/ppsspp_pc_sampler.py`,
about 50-60 samples per second). PPSSPP serves its debugger only after a
blocking discovery registration (about a minute without outbound network),
so the first boot idles on native HOME with the scenario's `input_script`,
`url` and `validation_js_bench` keys held back; once connected, the sampler
installs the real boot.cfg, removes the partial run's profile and cache
files and reboots the game with the CPU held, so samples start at the first
instruction. `tools/pc_profile_report.py --elf psp-browser-script.unstripped
--timeline-log OUT.tsv.log OUT.tsv` splits samples at the load-timeline
milestones (`--bench-log` splits a js-bench run per kernel) and prints the
hot functions and hot-set sizes. PPSSPP services a break at the next JIT
block boundary, so a break requested during an HLE call lands on its return
site weighted by host time; the report drops those samples, the idle thread
and validation display waits. Each sample is weighted by the emulated cycles
since the previous one (`--weight ticks`, the default; at most 3x the median
step), so samples taken while the emulated clock stood still count for
nothing: one chatgpt-ask run froze PPSSPP for 128 samples at a single
program counter (the entry of JS_CreateProperty, a JIT block boundary),
which unweighted counting (`--weight samples`) reported as 11% of the load.
`tools/code_layout_order.py` turns report
JSON into a link order (`PSP_BROWSER_PSP_SECTION_ORDERING_FILE`);
`tools/icache_conflicts.py` compares layouts statically. `--lines FN`
reports how many 64-byte I-cache lines hold a function's samples.

### Engine profile collection (PGO) under PPSSPP

A validation tree configured with `-DPSP_BROWSER_QUICKJS_PGO_GENERATE=ON`
(and `-DPSP_BROWSER_PSP_TEXT_LIMIT_OVERRIDE=9000000`: the counters add about
1.45 MB of code) instruments only the QuickJS objects. Its clean exit writes
libgcov's `.gcda` counters to `pgo/` beside the EBOOT, and the runner copies
any `.gcda` a run wrote into `run-N/pgo/`.
`scripts/train-quickjs-pgo.sh BUILD OUT_DIR perf/traces [--bench-weight W]`
replays chatgpt-ask (and optionally the js-bench kernels, merged at weight
W with `psp-gcov-tool`) and writes `OUT_DIR` with a `PROVENANCE` file
holding the engine build's fingerprint (sources, headers, compiler, flags);
`-DPSP_BROWSER_QUICKJS_PGO_USE=OUT_DIR` builds with it and refuses a profile
trained on any other engine build. Training runs under PPSSPP only, never
on a device. PPSSPP replays the journey deterministically: the same build
and trace give identical counters, and the PGO build is byte-identical.

## Arming a run

Validation builds read a script beside the executable:

```ini
input_script=input-script.txt
```

This changes only the input source. With an empty/default URL, the browser
takes the shipping entrance and presents native HOME before the first scripted
press. Scenarios which require a document or media can set the corresponding
boot URL and validation mode.

## PPSSPP

`cursor-latency` qualifies native HOME at an explicit 333 MHz (also when the
clock override is unset/zero). Continuous movement segments exclude the
script's deliberate idle gaps and cursor-edge clamps. Both sample cadence and
sample-to-accepted-presentation latency must average at most 20 ms and peak at
most 34 ms; every sampled move must receive a presentation with no coalescing.
Explicit other clocks, such as `TILEFINCH_PPSSPP_CPU_MHZ=111`, are pressure
profiles: timings are reported without claiming stock-clock qualification,
while presentation accounting and the receiver golden remain mandatory.
`tilefinch-cursor-work` splits composition from publication (including vblank)
to diagnose missed refreshes without weakening the stock-clock gate. Each run
keeps its requested emulator configuration beside its log.

```sh
PSPDEV=/path/to/pspdev cmake --preset psp \
  -B build-preset-psp-validation \
  -DTILEFINCH_PSP_VALIDATION_LOG=ON
PSPDEV=/path/to/pspdev cmake --build build-preset-psp-validation \
  --target psp-browser-script

scripts/run-ppsspp-input-script.sh
scripts/run-ppsspp-input-script.sh --runs 2
scripts/run-ppsspp-input-script.sh --update-golden
scripts/run-ppsspp-input-script.sh --url 'https://example.test/' \
  --script live-page-scenario
```

The runner creates an isolated HOME directory, writes the boot configuration,
waits for the clean terminal record, and extracts only
`tilefinch-input-script:` lines. Artifacts are placed under
`build-preset-psp-validation/ppsspp-input-script-latest/`.
Without `--url` the native-HOME entrance remains mandatory. An explicit HTTPS
URL is intended for live page/media scenarios and is verified against the
direct-URL boot record before the run is accepted.

`--trace DIR --trace-keyed` serves every request from a recorded HTTP trace
(staged beside the EBOOT, replayed by method and URL through the
`trace_keyed=1` boot setting) instead of the network, so a page load takes the
same emulated time on every run. `--measure` accepts a scenario with no golden
(one that exists is still enforced). Both exist for the
[performance journeys](PERF_JOURNEYS.md).

`--record-video DIR` dumps the run's presented frames (lossless FFV1 AVI at
480x272) and audio into `DIR` for `scripts/analyze-game-video.py`; see
[Treadline visual QA](../DEVELOPMENT.md#treadline-visual-qa-camera-track-and-recorded-gameplay).

Use `--debug-log` when the question is which PSP call PPSSPP accepted or
rejected. The wrapper launches PPSSPP through the macOS-safe path described in
`AGENTS.md`.

On macOS every automated PPSSPP harness uses LaunchServices, including the
input, network, crypto, and device-cost runners. They never fall back to direct
`PPSSPPSDL` execution: when GUI registration is unavailable, the harness stops
with an actionable error before PPSSPP starts. Direct execution from an
automation shell can abort in Cocoa before any PSP code runs, so
`PPSSPP_LAUNCHSERVICES=0` is deliberately rejected. Grant the LaunchServices
operation and rerun instead; an emulator abort before a Tilefinch log is not a
browser result.

LaunchServices is invoked with `open -g`, so automated PPSSPP windows remain
in the background and do not activate over the user's current application.
Do not remove that flag to make a test easier to watch; inspect the captured
frames and logs, or explicitly bring PPSSPP forward for a manual run.

## Physical PSP

Copy the script beside an installed validation EBOOT and name it in
`data/boot-overrides.cfg`, or keep the executable, script, configuration, and
logs on the host with the preferred zero-Memory-Stick PSPLink workflow:

[PSPLink device development](PSPLINK_DEV_LOOP.md)

The PSPLink manual owns the PRX build, host0 paths, memory check, installed
EBOOT handoff, and live media scenarios. This document owns only the input
language and deterministic trace.

## Host qualification

```sh
ctest --test-dir build-preset-release \
  -R tilefinch-psp-input-script-tests
build-preset-release/tilefinch-psp-input-script-tests --update
```

The host executable checks the parser and stepper, runs the menu tour twice
through `psp_ui_update()`, and compares both traces with
`tests/input-scripts/menu-tour.host-trace.txt`. It qualifies the frontend half
of dispatch; PSP-only action receivers require PPSSPP or hardware evidence.

## Current golden coverage

`tests/input-scripts/menu-tour.txt` and its device golden begin on native HOME,
exercise both launch-tile directions, create a local document tab, traverse
the principal surfaces, and exit through the menu.

Covered action receivers include focus and page movement, reload, Reader mode,
tab creation/switching, collections, back, bookmark, home, and exit. The tour
also covers UI scale, page font scale, Reader font and site scale, color mode,
chrome theme, and video scaling.

Text-entry modals, live navigation, site data, screenshots, and seeded
collection deletion use separate scenarios because they need external state or
clock-derived output.

`site-restore` idles on native HOME and must be run with
`--data-dir tests/fixtures/ppsspp-site-data`, which seeds a profile with
local-storage persistence and a 1 MB disk cache enabled, one local-storage
snapshot, and a 120 KiB cache snapshot beside the EBOOT. Its golden is
trivial; the gate is the validation log, which must show both deferred
restores completing through the frame-pump policy, the baseline font read
yielding to a restore slice, and no order or wiring violations. The seed is
written by `tilefinch-site-data-fixture --write` with the shipping writers,
and the `tilefinch-site-data-fixture-tests` host test reloads it with the
shipping readers, so a persistence-format change fails on the host instead of
turning the emulator run into a restore with nothing to read.

`runtime-input-live` and `runtime-cancel-live` use the bounded cooperative
runtime fixture. Their goldens preserve action order/counts and captures but
omit asynchronous cursor positions; the raw trace remains available. Separate
gates require runtime-specific queue, successful presentation, and cancellation
evidence. `wikipedia-navigation-live` uses `-page` input and additionally checks
distinct focus geometry, an authored or browser focus indicator, pending tasks,
and captures for visual inspection. Menu focus now has a separate 150 ms
emulator gate; menu activation is timed but still requires full relayout.
Its golden ignores asynchronous action-cursor annotations, not receiver
order or captures. A passing action golden alone is not a latency budget.
See [the performance ledger](PERFORMANCE_LEDGER.md#long-article-layout-cost-and-live-input-evidence-from-2026-09-04).

`wikipedia-search-keyboard-live` starts at the English Main Page, hides
the toolbar, opens its responsive search icon, then moves the nub to the
separate form's input, enters `psp` through Danzeff, and submits with R+Start.
Check that `search-loaded` is loaded, its URL contains the nonempty `search=psp`
query, and its capture shows search results. Optional resources may still be
pending; a keyboard-close, empty-query page, or action acknowledgement alone
is not success. Coordinates are tied to the live 480px layout: inspect the
`search-field` capture and focus-target log when the site changes. This is a
live smoke, not a stable page-layout golden.
`wikipedia-font-cursor-live` alternates nub movement and idle time during
optional-font publication. Check the font adoption/rollback and owner-thread
input-acknowledgement records together with the captures. The old fallback
layout must survive preemption, and font adoption must remain retryable.
Both scenarios use the normal receiver; neither injects a search-result URL.

## Reading the trace

```text
tilefinch-input-script: armed script=... steps=139 stall-limit=1800
tilefinch-input-script: mark=scrolled step=12 screen=page
tilefinch-input-script: step=13 action=reload setting=none screen=page ...
tilefinch-input-script: covered action=reload count=1
tilefinch-input-script: outcome=exit-action steps=136/139 frames=725 ...
```

The trace records intent delivery, not success by itself. Match dispatched
actions with the `tilefinch-operation:` begin/end journal and the relevant
receiver records (`tilefinch-tabs:`, `tilefinch-profile:`,
`tilefinch-content-blocker:`, and so on). A receiver-specific success condition
must corroborate every behavior the scenario claims.

At each page-observed `mark` (plain marks; see `mark-live` above) the
validation build also prints the deterministic work record the host lab
prints:

```text
tilefinch-work: label=scrolled js.work_units=... style.resolutions=... raster.tiles=...
```

It is the same record, fields and reset semantics as the lab's `work` command
([LAB_USAGE.md](LAB_USAGE.md#work-vector-tilefinch-work)): integer counts
cumulative for the current document, `n/a` for counters the build does not
compile, no timings. Compare a PPSSPP or device run with a lab run of the
same journey through `tools/work_vector_report.py`, or subtract consecutive
marks with `--steps`. On PPSSPP and the device the frame-cadence fields
(`raster.*`, relayouts) follow emulated or real speed; the JavaScript, style
and parse fields are the ones that should match the lab.

Outcomes are:

| Outcome | Meaning |
| --- | --- |
| `complete` | reached `end` |
| `exit-action` | the script intentionally exited first |
| `interrupted` | an external exit stopped the scenario |
| `stalled` | a bounded wait expired |
| `idle` | no script remained active |

## Adding coverage

PSP validation realms expose `__tilefinchValidationThreadCpuUs()` for two
samples around a bounded, isolated kernel. It returns the firmware thread's
microsecond run counter, not CPU cycles, and throws if the firmware query
fails. It is absent from ordinary and host builds. Keep such sampling outside
frame timing, and collect operation counts separately: counter writes can
disproportionately penalize the reference implementation. Restore any mutable
game state used by an identical-input replay before resuming gameplay.

Prefer one focused script over extending the hermetic menu tour with network or
persistent-state prerequisites. Useful independent panels include:

- address/find entry and the Danzeff keyboard;
- Browsing, Privacy, Experimental, and System settings;
- collection activation/deletion against a seeded profile;
- live HOME activation and cancellation;
- five-tab pressure, close, and hibernation;
- screenshots with an explicit filename-normalization rule.

Game input needs an additional distinction: a qualification page can mutate
its simulation directly without ever exercising PSP controls. The committed
`treadline-offline-controls.txt` scenario instead enters through native Home
and Library and sends its authored `PspUiInput` through Page controls and the
standard Gamepad publication path. Validation reports the downstream evidence
as `tilefinch-input-gamepad:`; the PPSSPP runner requires every scripted face,
shoulder, D-pad, and analog extreme to appear there. Do not infer delivery
merely from `held-frames`. Seed the isolated emulator with the same sealed
library layout used on-device:

```sh
scripts/run-ppsspp-input-script.sh \
  --script treadline-offline-controls \
  --offline-library build-preset-psp-validation/offline \
  --heap-mb 7 --script-file-kb 512
```

The optional directory is copied into the run's disposable Memory Stick. It
must contain `library.bin` and the referenced payload files; the runner never
falls back to the network to manufacture a missing installed app.

`treadline-menu-focus-probe.txt` takes the same installed-package route to
check D-pad focus geometry in the menus: from the title it moves to Quick
Match, down through the setup screen's bottom row and back up to Deploy,
then deploys with Cross. Its marks (`deploy-focus`, `bottom-code-focus`,
`deploy-return-focus`, `deploy-result`) and its device golden
(`treadline-menu-focus-probe.device-golden.txt`) record that every move
lands where a person expects; run it with the same `--offline-library`
options as above. Menu focus timing has its own, longer scripted loop
(PERFORMANCE_LEDGER.md, "Focus moves and small updates over live
canvases").

Every new scenario must bound each wait, state whether its evidence is
hermetic or external, and identify the receiver output that proves success.
