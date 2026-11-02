# Input script reference

The validation build can replace physical controller input with a deterministic
script interpreted inside the PSP application. The script crosses the same
`psp_ui_update()` and action-dispatch boundary as real buttons, so it qualifies
menu routing, focus, state projection, navigation cancellation, and application
lifecycle without depending on an emulator automation API.

Shipping EBOOTs compile the harness out. A release build handed an
`input_script=` key ignores it and boots normally.

Scenario pass criteria are in [Input scenarios](INPUT_SCENARIOS.md); the
meaning of each validation log record, and the tools that read them, are in
[Validation records](VALIDATION_RECORDS.md).

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

The mark name capacity is 20 bytes including the terminator, and only
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

## Arming a run

Validation builds read a script beside the executable:

```ini
input_script=input-script.txt
```

This changes only the input source. With an empty/default URL, the browser
takes the shipping entrance and presents native HOME before the first scripted
press. Scenarios which require a document or media can set the corresponding
boot URL and validation mode.

Companion probes sit beside the script and replace its extension:
`game.txt` uses `game.mark.js` and `game.until.js`, not `game.txt.mark.js`.
`psplink-device.sh memory` stages these companions with a committed scenario,
so an updated diagnostic cannot silently run with an older copy on host0.

Other `boot.cfg` keys commonly used with scripts:

- `dump_frame=1` writes a full-resolution final frame (see
  [Captures](#captures)).
- `validation_game_audio_mute=1` silences physical-device game tests. Voices,
  envelope processing and PCM mixing still run; only hardware output volume
  is zero. The default is 0, and shipping configuration scrubbing removes the
  setting.

## Script format

The language is line-oriented. Blank lines and `#` comments are ignored.

```text
wait COUNT
until COUNT
tap BUTTONS
hold COUNT BUTTONS
press COUNT BUTTONS
stick COUNT DIRECTION [BUTTONS]
tap-after-live MILLISECONDS BUTTONS
mark NAME
focus-page-media
end
```

Append `-live` to `wait`, `until`, `tap`, `hold`, `press`, `stick`, or `mark` when
that step must continue while the application reports asynchronous work. For
example, `wait-live 30` advances for 30 sampled frames during a navigation
or sustained media session. Without the suffix, a step pauses until the
ordinary browser loop is ready to accept scripted input.

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

`until COUNT` and `focus-page-media` are described below.

### Timed presses

`tap-after-live MILLISECONDS BUTTONS` schedules a press independently of
browser readiness (1–65535 ms after the step is armed). It retains the due
time when the next input poll is late. Validation logs therefore include
polling delay in press-to-visible timing, rather than measuring only from
when busy work finally samples the button. A timed step following another
step is armed when that preceding step finishes, before returning to owner
work—not at the next poll after that work. Select and Triangle report an
accepted-frame `tilefinch-input-response`; focus and scrolling use their
existing accepted-frame feedback records. Missing or superseded responses
are not successful samples. Use identical captured responses and an explicit
emulator clock for A/Bs; emulator timings are not physical-device latency
claims. EOF is finalized only in the owner loop after queued presses drain.

### Input during page work

Read-only selector preparation, cache preparation and tile raster keep the last completed page framebuffer
immutable and use owner-thread checkpoints for native controls. Composition
suppresses those presents until its framebuffer writes finish; a completion
publication retains the supervisor's latest menu state. Queued page input
defers additional author tasks only between tasks, after their microtask
checkpoint, and defers further tile units without cancelling completed work.
Optional image admission has the same resumable boundary before each request:
waiting page input keeps the job pending before the first request, or retains
the first request and leaves the optional second target in the page queue.
Replay metadata polls between bounded line groups without skipping its full
integrity validation. Native feedback still uses immutable incumbent pixels;
no DOM input dispatch occurs inside request preparation.
Visible, in-viewport targets can emit `tilefinch-focus-first-visible` before
author styling completes; an offscreen target must use the later complete
frame. A `visible=0` completion is not a successful focus-feedback sample.

### Marks

Use plain `mark` for page/runtime profiling boundaries. A `mark-live` can be
consumed by the input supervisor while page work is still running: its UI
capture is valid, but the main-loop page observer may not emit the JavaScript
profile or the `.mark.js` diagnostic. Verify those records exist before using
a mark as a timing/report boundary; a captured image alone does not prove it.
`mark-page` also runs in the main loop and is useful for an animated game.

An explicit mark whose name starts with `targets-` records up to 64 link and
64 control hit-test rectangles, with their indices, IDs and control types.
It does not record field values, page text or link destinations. Use these
validation-only records to check a journey's actual targets when layout or
font changes make an old cursor-coordinate sequence miss a field.
Every observed page mark also records numeric provider feed, account-action
and like states. Require a confirmed `CURRENT` state after the intended
action; an activation receipt or completed script alone is not confirmation.

### Page and text suffixes

Use `-page` on the same commands to wait for a committed, painted page but
not for background resources to finish. These steps run only in the main
input loop, never inside a runtime cooperation callback. They are useful for
checking focus and disclosure controls during page startup. Neither suffix
turns the earlier provisional navigation preview into an interactive document.

Use `-text` for keystrokes meant for the on-screen keyboard. Those steps
advance only while the keyboard samples input; main-loop frames hold them.
On slow pages the keyboard opens seconds after the activating press, and a
fixed wait lets the first keystroke reach the page as a second activation.

The script does not infer semantic readiness from pixels. Use ordinary steps
when input must wait for the browser's ready boundary, `-page` for a painted
document with pending work, `-live` for input within ongoing work, and correlate `mark` records
with the operation journal to prove the state the scenario reached.

### Focusing page media

`focus-page-media` is one idle frame on which the harness moves d-pad focus
to whatever Play would act on, so the d-pad path, which differs per site and
between the lab and the device, does not decide what a following `tap cross`
plays. It prefers the declared media card when the page has one, then the
first author `<video>` or `<audio>` in document order with a source page media
can open (a non-empty, non-`blob:` `src` or `<source>`: the same
`browser_engine_author_media_openable` rule the card uses to decide that a page
has its own video), then the first other author media element, whose
activation builds the same declared or discovered candidates as the page's
Play. An element that is not itself a focus target (hidden or covered by its
player's chrome) is reached through the first focus target of its nearest
enclosing element that has one, usually the player's play button, whose Cross
runs the page's own play. It logs `tilefinch-input-script: step=N
action=focus-page-media
target=card|author|author-player|element|element-player|none` (`none` leaves
focus where it was). It
accepts the `-page` and `-live` suffixes. Cross then plays through the normal
user path. Like the rest of the harness it is compiled out of shipping builds
(`src/psp_input_page_media.c` is a validation-only source).

### Readiness probes

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

## Captures

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

For small-text or emoji inspection, the validation build also honors
`dump_frame=1` after the interactive scenario finishes. It writes
`frame-device-final.ppm` from the current 480×272 RGB565 front buffer before
teardown, preserving full spatial resolution rather than the mark queue's
240×136 RGB332 samples. This opt-in write happens after the measured sequence;
use a host0 launch to keep it off the Memory Stick. It does not capture the
separate RGB8888 video surface and reports `final-frame-dumped=0` when that
surface owns scanout. Ordinary release builds do not contain this final dump.

## Runners

### PPSSPP

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

### Physical PSP

Copy the script beside an installed validation EBOOT and name it in
`data/boot-overrides.cfg`, or keep the executable, script, configuration, and
logs on the host with the preferred zero-Memory-Stick PSPLink workflow:

[PSPLink device development](PSPLINK_DEV_LOOP.md)

The PSPLink manual owns the PRX build, host0 paths, memory check, installed
EBOOT handoff, and live media scenarios.

### Host qualification

```sh
ctest --test-dir build-preset-release \
  -R tilefinch-psp-input-script-tests
build-preset-release/tilefinch-psp-input-script-tests --update
```

The host executable checks the parser and stepper, runs the menu tour twice
through `psp_ui_update()`, and compares both traces with
`tests/input-scripts/menu-tour.host-trace.txt`. It qualifies the frontend half
of dispatch; PSP-only action receivers require PPSSPP or hardware evidence.

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

## Outcomes

Outcomes are:

| Outcome | Meaning |
| --- | --- |
| `complete` | reached `end` |
| `exit-action` | the script intentionally exited first |
| `interrupted` | an external exit stopped the scenario |
| `stalled` | a bounded wait expired |
| `idle` | no script remained active |

## Goldens

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

## Adding coverage

Prefer one focused script over extending the hermetic menu tour with network or
persistent-state prerequisites. Useful independent panels include:

- address/find entry and the Danzeff keyboard;
- Browsing, Privacy, Experimental, and System settings;
- collection activation/deletion against a seeded profile;
- live HOME activation and cancellation;
- five-tab pressure, close, and hibernation;
- screenshots with an explicit filename-normalization rule.

Game input needs an additional distinction: a qualification page can mutate
its simulation directly without ever exercising PSP controls. A game scenario
must instead enter through native Home and Library and send its authored
`PspUiInput` through Page controls and the standard Gamepad publication path;
[`treadline-offline-controls`](INPUT_SCENARIOS.md#treadline-offline-controls)
is the model.

Every new scenario must bound each wait, state whether its evidence is
hermetic or external, and identify the receiver output that proves success.
