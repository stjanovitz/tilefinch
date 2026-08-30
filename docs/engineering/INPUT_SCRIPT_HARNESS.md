# PSP input script harness

The validation build can replace physical controller input with a deterministic
script interpreted inside the PSP application. The script crosses the same
`psp_ui_update()` and action-dispatch boundary as real buttons, so it qualifies
menu routing, focus, state projection, navigation cancellation, and application
lifecycle without depending on an emulator automation API.

Shipping EBOOTs compile the harness out. A release build handed an
`input_script=` key ignores it and boots normally.

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

## Script format

The language is line-oriented. Blank lines and `#` comments are ignored.

```text
wait COUNT
tap BUTTONS
hold COUNT BUTTONS
press COUNT BUTTONS
stick COUNT DIRECTION
mark NAME
end
```

Append `-live` to `wait`, `tap`, `hold`, `press`, `stick`, or `mark` when
that step must continue while the application reports asynchronous work. For
example, `wait-live 30` advances for 30 presented frames during a navigation
or sustained media session. Without the suffix, a step pauses until the
ordinary browser loop is ready to accept scripted input.

`BUTTONS` joins names with `+`: `up`, `down`, `left`, `right`, `cross`,
`circle`, `triangle`, `square`, `ltrigger`, `rtrigger`, `start`, and `select`.
The parser accepts at most 256 steps, 20 characters per mark, and 8 KiB per
file. The boot key accepts only a leaf filename—no separators or `..`.

`hold` holds one chord continuously for the requested frame count. `press`
emits that many distinct press/release pairs. `stick` accepts `up`, `down`,
`left`, or `right`. Counts are frame-counted, not time-counted; avoid using
them to qualify shortcuts whose meaning is intentionally millisecond-based.

The script does not infer semantic readiness from pixels. Use ordinary steps
when input must wait for the browser's ready boundary, `-live` steps when the
scenario deliberately overlaps ongoing work, and correlate `mark` records
with the operation journal to prove the state the scenario reached.

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

Outcomes are:

| Outcome | Meaning |
| --- | --- |
| `complete` | reached `end` |
| `exit-action` | the script intentionally exited first |
| `interrupted` | an external exit stopped the scenario |
| `stalled` | a bounded wait expired |
| `idle` | no script remained active |

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
  --heap-mb 7 --script-file-kb 384
```

The optional directory is copied into the run's disposable Memory Stick. It
must contain `library.bin` and the referenced payload files; the runner never
falls back to the network to manufacture a missing installed app.

Every new scenario must bound each wait, state whether its evidence is
hermetic or external, and identify the receiver output that proves success.
