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
wait-ready [MAX_FRAMES]
wait-screen SCREEN [MAX_FRAMES]
wait-menu MODE [MAX_FRAMES]
wait-busy [MAX_FRAMES]
wait-idle [MAX_FRAMES]
press BUTTONS [FRAMES]
release [FRAMES]
mark NAME
capture NAME
end
```

`BUTTONS` joins names with `+`: `up`, `down`, `left`, `right`, `cross`,
`circle`, `triangle`, `square`, `ltrigger`, `rtrigger`, `start`, and `select`.
The parser accepts at most 256 steps, 20 characters per mark, and 8 KiB per
file. The boot key accepts only a leaf filename—no separators or `..`.

`press` is frame-counted, not time-counted. Avoid using it to qualify the
Triangle hold shortcut, whose meaning is intentionally millisecond-based.

The useful wait distinction is:

- `wait-busy` proves the operation actually entered an asynchronous state;
- `wait-idle` proves the later stable state;
- `wait-ready` waits for ordinary UI readiness and is not appropriate while
  sustained media playback intentionally remains active.

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
```

The runner creates an isolated HOME directory, writes the boot configuration,
waits for the clean terminal record, and extracts only
`tilefinch-input-script:` lines. Artifacts are placed under
`build-preset-psp-validation/ppsspp-input-script-latest/`.

Use `--debug-log` when the question is which PSP call PPSSPP accepted or
rejected. The wrapper launches PPSSPP through the macOS-safe path described in
`AGENTS.md`.

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

Every new scenario must bound each wait, state whether its evidence is
hermetic or external, and identify the receiver output that proves success.
