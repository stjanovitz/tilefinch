# Treadline Arena qualification

For contributors changing Treadline Arena or the browser paths it exercises:
the host fixtures, harness API, PPSSPP and device runs, and visual review that
qualify the game. The shared Canvas/WebGL lane that runs these fixtures is in
[Development](../../docs/DEVELOPMENT.md#canvas-and-webgl-game-qualification).

## Host fixtures

The Canvas/WebGL lane runs Treadline through its PSP-realistic startup,
offline manifest, fixed-step tread movement, charge/release firing, pause,
ricocheting shells, destructible barrier swaps, the three armor/speed classes
and their secondary weapons, directional armor, the opt-in Command meter, all
five gadgets, Survival, Team Control, Convoy Escort, objective-aware bot
movement, saved setup preferences, bounded entity pools, and teardown
ownership. The feature fixture also covers Daily Arena, Billiards, bosses,
hazards, local artillery handoff, paints and palettes, wall
refusal/leading/routes, replay checksums and deterministic results, and the
killcam's return to play.

These fixtures run once per CTest pass, in
`tilefinch-treadline-feature-budget-tests`: a fresh 7 MiB JS realm that
requires zero allocator refusals and zero owned bytes after teardown. The
default conformance test loads the game but does not repeat them.

`tilefinch-canvas-webgl-conformance-tests` modes:

| Mode | Runs |
|---|---|
| `--treadline-features [game-script]` | Game Profile script admission, replay import, and the feature fixtures (features, nearest goal, occupancy, render cache, campaign, campaign HUD, controls, practice, placement, planning, breach, camera, look, smoke, layers) |
| `--treadline-league N [game-script]` | 1–1000 headless seeded matches; `scripts/run-treadline-bot-league.py` summarizes the JSONL results |
| `--treadline-host-profile [game-script]` | six fixed-seed arena cases of 1,200 frames at 30 Hz, for scheduling research |
| `--treadline-probe SCRIPT [N]` | a probe script with the invariant fixture's helpers, optionally after playing its first `N` cases |

The optional `game-script` is a source-root-relative path, for exact
before/after comparisons without changing the packaged game; normal CTest
always loads the packaged game. Alternate sources share the Game Profile's
512 KiB per-script ceiling.

The layers fixture measures every overlapping pair of parallel box faces in
16-bit depth steps at the real camera (and flat tops against the static grid
and contact-shadow layers), and checks that a decal dropped for instance room
stays dropped.

The host profile counts strategy refreshes, bank plans, wall rays, and bot
updates through a test-only lexical probe, and times simulation plus instance
preparation with an external monotonic clock. It does not render, wait for
vblank, run the active audio graph, or predict PSP milliseconds.
`scripts/profile-treadline-host.py --candidate <game-script>` runs three
alternating pairs, requires deterministic work counts, and reports tail
timing and clustering separately. Timing uses `--treadline-host-timing`
without the per-ray and planner counters; separate work runs check
determinism and fairness. Initial and reset warmup is excluded; the raw
maximum is still reported. Pair this screen with the feature and bot-league
checks, and take only surviving changes to the device.

## Harness API

The game ships without its harness API. `examples/treadline-arena/qualification.js`
holds `__treadlineDebug` (the API of the fixtures, companions, and sweeps), the
phase-clock profile behind `__tilefinchStartInputProfile` with its
`TREADLINE-JS-PHASES` report, the soak's action cycle, and the long-soak
driver. The modules' harness surfaces come through it as well:
`__treadlineDebug.campaign` (the menu, the save, missions, and `bridge()`, the
game bindings the campaign attached with), `.practice`, and `.controls`. The
modules hand them over at attach only when the game started for
qualification or a harness, so ordinary play neither builds nor exposes
them. Each harness loads the file explicitly:

- **A qualification URL** (any `?qualification=...`) makes the page fetch it
  after `game.js`. An installed game must carry it, so build qualification
  packages from every entry of `package-files.txt`
  (`awk '!/^#/ && NF { print $1 }'`); ordinary packages leave it out. In
  Tilefinch the script attaches within a frame or two of boot, before any
  companion mark reads `__treadlineDebug`.
- **A harness that evaluates the scripts itself** sets
  `globalThis.__treadlineEnableDebug = true` before `game.js` and evaluates
  `qualification.js` right after it, as `tests/test_canvas_webgl_conformance.c`
  does for the fixtures, the bot league, and the campaign and invariant
  sweeps. The game then does not fetch it.
- **The lab-driven gates** use qualification URLs that change nothing they
  measure: the menu-layout check `?qualification=layout`, the reference
  screenshots `?qualification=references`, and `treadline-visual-play`
  `?qualification=visual` (its companion turns the killcam back on, which
  qualification runs otherwise keep off).

## Long soak

`?qualification=long-soak` keeps the player alive and drives it with the
game's bounded target/avoidance planner, aiming and firing through the
ordinary cooldown and projectile paths, cycling arenas, and repeating
collisions, particles, smoke, and destruction. The run therefore proves real
movement and combat rather than a blind route that parks against scenery or
settles on a title or victory screen.

Use `tests/input-scripts/treadline-long-soak.txt`. The page opens on Quick
Match with Deploy focused; once the runtime is ready the script presses
Cross, a trusted Deploy that claims Page controls, so the match runs full
screen with no browser chrome over the canvas (the soak stays audio-free).
Its companion `treadline-long-soak.mark.js` runs the 180-frame phase-clock
profile and returns its report before the window, after the PAGE CONTROLS ON
toast, then switches the clocks off: clocks and report are attribution work,
not gameplay.

The measured window is 1,350 game frames (45 seconds at 30 Hz; under Page
controls each script frame is one game frame), bracketed by `mark-page` marks
so the measurement reset falls between frames. The PPSSPP harness requires
chrome hidden and clocks off at the window's start and a delivered profile
report. Validation keeps percentile samples for the first 1,024 presented
pipelines and separately counts every pipeline over 34 ms and the worst
pipeline from `webgl-measure-start` to `webgl-measure-end` (the end mark's own
frame is not counted); judge the long tail by those two. Compare the
pre-interaction and controlled budget/category reports and the QuickJS
retained heap, so a smooth run cannot hide gradual growth.

The validation writer keeps one marked frame per run: archive a gameplay
frame at the first mark, and use the final active-game summary and near-zero
HTML-overlay time to show the run did not fall back to a title panel. Use a
second paired run or a PSPLink screenshot when a visual end-frame comparison
is needed.

The long soak calls the combat planner directly, so it is a simulation and
render stress test, not an input-routing test. Before a release also run
`tests/input-scripts/treadline-offline-controls.txt` from native Home with the
current package installed as Saved row one (the device fixture keeps Prism
Break in row zero). It opens **Library → Saved**, launches the sealed offline
pack, activates Deploy, exits and re-enters Page controls with Start+Select,
and drives nub, face, shoulder, gadget, and command inputs. Its terminal
`tilefinch-input-gamepad:` line is the receiver proof: connected, button, and
analog frame counts must be nonzero, the connection count must cover the
automatic entry and the manual re-entry, and `buttons` must include the
exercised standard Gamepad slots. A direct qualification URL cannot
substitute for this run.

Automated `treadline-*` scenarios mute PPSSPP's host output in the disposable
run configuration; emulated audio stays enabled so mixer and audio-syscall
work is exercised. Do not change the game's audio preferences or the user's
emulator or system volume to silence a background test.

## Treadline visual QA: camera track and recorded gameplay

Two tools look at what the player sees rather than at simulation state.

### Camera track in the invariant sweep

`python3 scripts/run-treadline-invariants.py` composes the camera after every
1/30 s step, as a device frame does after its update
(`__treadlineDebug.presentCamera`), and feeds the detector in
`tests/fixtures/treadline-camera-motion.js`: distance, pitch, yaw, and the eye
and look target relative to the player. Per 1.5 s window it flags:

- oscillation (distance, pitch, or yaw reversing three times over);
- re-retraction (snapping in, easing out, and snapping in again);
- jumps no authored motion makes (yaw beyond the follow rate, a look target
  leaping without a retraction, an expansion faster than its ease);
- sustained jerk, and shake that never settles.

Arena changes, the killcam, respawns, Duel turns, and camera-mode switches are
hard cuts; a single retraction, the follow turn, and hit shake stay below the
limits. Bot cases alternate the fixed and follow cameras. The runner prints a
`camera motion:` summary line, and `-v` lists every flagged case with a
distance excerpt. A flagged case fails the sweep. To inspect one case frame by
frame, a `--treadline-probe` script can set
`globalThis.__treadlineCameraTrace = {case, from, to, rows: []}` before that
case begins. `tests/test_treadline_camera_motion.js` (CTest
`tilefinch-treadline-camera-motion-tests`) drives the detector with synthetic
series.

### Recorded PPSSPP gameplay

`scripts/run-ppsspp-input-script.sh --record-video DIR` turns on PPSSPP's
frame dump for that run only: a lossless FFV1 AVI at 480x272 in `DIR/VIDEO`,
the mixed audio in `DIR/AUDIO`, and the validation log, copied out of the
isolated HOME before it is deleted. Host audio goes to SDL's dummy driver, so
the run stays silent without muting the dump. A long soak:

```sh
build-preset-release/tilefinch-offline-library-fixture --web-app OUT \
  'https://game.test/index.html?qualification=long-soak&seed=12345' \
  examples/treadline-arena \
  $(awk '!/^#/ && NF { print $1 }' examples/treadline-arena/package-files.txt)
  # add &mode=onslaught to the URL for Onslaught
scripts/run-ppsspp-input-script.sh --script treadline-long-soak \
  --url 'https://tilefinch.local/offline/app?id=1' --offline-library OUT \
  --script-file-kb 512 --measure --record-video recording
python3 scripts/analyze-game-video.py recording --out analysis
```

The `awk` takes every entry of the canonical package list, including
`qualification.js` (see the file's header for an ordinary package). `OUT`
must not exist yet. Do not point `TMPDIR` under `~/Documents`, or
LaunchServices refuses the launch. The long soak alternates the fixed and
follow cameras every 900 game frames.

PPSSPP writes one AVI frame per presented frame, and the file's 59.94 fps
label is nominal. The analyzer takes the true rate from frames over audio
duration (`--fps` overrides); `TILEFINCH_PPSSPP_CPU_MHZ=111` gives
device-like 1/30 s steps.

The analyzer uses ffmpeg, the standard library, and
`build-preset-release/tilefinch-video-motion` (`tools/video_motion.c`), which
estimates a zoom about the screen centre and a shift per frame (`--no-camera`
skips it). It reports:

- camera oscillation, pumping, jerk, and single jumps;
- single-frame transients (a band such as browser chrome, or a large area,
  shown for one frame);
- brightness flashes and HUD-text dropouts (`--hud-top`/`--hud-bottom` set
  the HUD bands);
- popping; black, blank, and noise frames; frozen runs; the tile
  checkerboard; and partly drawn frames.

Single jumps (a cut, a deliberate retraction, or a glitch: the video alone
cannot tell) and frozen runs (pause screens freeze on purpose) are listed for
review but not counted as problems. `report.txt` gives times and frame
numbers, `frames.tsv` the per-frame measures, and `events.json` the events.
Each event gets an mp4 clip at the true rate and a 4x3 contact sheet of
twelve consecutive frames starting five before its peak. `--skip SECONDS`
ignores boot and menus. CTest `tilefinch-game-video-analyzer-tests` runs the
analyzer on synthetic ffmpeg clips.

### Blinks

The flicker, transient, and popping checks look for large areas and let hit
flashes and smoke through, so the analyzer also runs
`build-preset-release/tilefinch-video-blink` (`tools/video_blink.c`) on the
full-resolution colour frames. A blink is a region that changes for one to
four frames and then returns exactly to what it was (on-off-on or off-on-off)
while its surroundings stay put and the camera is still:

| Kind | Meaning |
|---|---|
| `blink hud` | bright pixels in a HUD band |
| `blink object` | scenery, a tank, an effect, or a HUD indicator in the scene area |
| `shimmer line` | a thin or sparse run: sub-pixel lines such as the floor grid and wall tops dropping in and out as the camera moves; informational, compare counts between reviews |

The report gives each blink's box in native pixels, its length, and whether
it appears or vanishes, and writes `NNN-...-zoom.png`, eight frames cropped
around it and enlarged. Intended blinks exist: the enemy hit flash (white,
0.12 s), the 8 Hz fire-windup pulse, muzzle and bounce flashes, sparks, and
the pickup bob at a screen edge. `--no-blink` skips the check.

PPSSPP's hardware renderer does not show what the browser's CPU draws over a
GE-written frame (chrome, toasts such as PAGE CONTROLS ON), so menus and
overlays are missing from ordinary recordings. Set
`TILEFINCH_PPSSPP_SOFTWARE_RENDERER=1` for the input-script runner to record
with the slower software renderer when overlays matter.

### Reference screenshots

`scripts/check-treadline-references.py` renders key scenes in the interactive
lab and compares them pixel for pixel with `tests/visual/treadline/*.png`: the
title, every main menu screen, the Controls pages, the Practice Range panel,
pause, the HUD in play, and the opening view of each authored arena (the
`*-first-frame` scenes render after two ticks, because the first tick runs the
arena change). The lab is byte-deterministic (fixed 16 ms ticks, a pinned save
with remarks off, `?daily=20261001`), and menu scenes reuse
`check-treadline-menu-layout.py`'s `SCREENS` visits. CTest
`tilefinch-treadline-reference-tests` (label `visual`) runs it. On a mismatch
it fails and writes the render and `<scene>-diff.png` (reference, render,
changed pixels in magenta) into `build-preset-release/treadline-references/`.
When, and only when, the change is intended, approve it after looking at the
diffs:

```sh
python3 scripts/check-treadline-references.py --update-references [--only SCENE,...]
```

Commit the new PNGs with the change that caused them; `--list` names the
scenes. A branch that restyles Treadline's text, HUD, menus, or palette must
re-approve the references in the same change.

### Milestone review

Before merging a Treadline change to visuals, the camera, rendering, the HUD,
or menus (and at each milestone), record the standard scenario set and read
the review folder:

```sh
scripts/visual-review.py --build-dir build-preset-psp-validation --out review
# or: cmake --build build-preset-release --target treadline-visual-review
#     (EBOOT from TILEFINCH_VISUAL_REVIEW_PSP_BUILD, folder
#      build-preset-release/treadline-visual-review)
```

It packages the game from the checkout (so an older validation EBOOT still
reviews the current JavaScript), records each scenario in PPSSPP at the
device-like 111 MHz clock (`--clock normal|both` for PPSSPP's own), analyzes
it, builds an overview sheet (one frame every 2 s), runs the reference check,
and writes `index.html` and `index.md`: a summary table, then per run the
overview and every event with its sheet, zoom, and clip, then a review
checklist.

The standard set is the Quick Match long soak (seed 12345), Onslaught seeds 2
and 7, campaign mission 1-1, and the Practice Range; `--scenarios` adds
`onslaught-12345` and `mission-3-4`. Missions and the range use
`tests/input-scripts/treadline-visual-play.txt`: real pad input after a
trusted Deploy, with the scene chosen by the package URL
(`?visual=mission&mission=N`, `?visual=range`). `--software` adds a
software-renderer run of each, and `--keep-video` keeps the lossless
recordings. It needs PPSSPP, so it is a build target and never part of the
default CTest gate; `--strict` makes analyzer problems or reference changes
fail it.

Review checklist (also in each index):

1. Decide for every event whether it is an intended effect, a known issue, or
   a new problem.
2. Look at every overview sheet for things no detector names.
3. Compare 111 MHz with normal-clock runs: a problem only at 111 MHz is a
   frame-budget effect (deferred HUD work, dropped optional geometry).
4. Approve reference changes only when the diff is the intended one.

## Staging a game on a PSP without stale assets

This applies to every packaged game, including Prism Break. An example
directory is the only authored source of its game, but a PSP test can involve
two other copies: a temporary HTTP staging tree and an installed,
integrity-protected offline snapshot. Rebuilding Tilefinch, regenerating its
bootstrap, or loading a new browser PRX updates neither, and changing only an
`index.html` query does not invalidate separately cached JavaScript, CSS, or
images.

Run the host lane first. For a direct network-backed device run, use the
content-addressed staging helper. It hashes the authored tree, copies it to an
immutable directory, verifies an existing directory before reuse, and writes
the digest-bearing document URL to the validation `boot.cfg`. Because the
digest is part of the directory, every subresource gets a fresh URL when any
authored byte changes:

```sh
scripts/stage-psp-game.sh \
  --source examples/treadline-arena \
  --stage-root /path/to/game-stage \
  --base-url http://HOST_LAN_IP:8770 \
  --boot-config build-preset-psp-validation/boot.cfg \
  --name treadline \
  --query 'qualification=long-soak&profile=ordinary-cadence'
python3 -m http.server 8770 --bind HOST_LAN_IP \
  --directory /path/to/game-stage
```

The helper prints the tree digest and final URL; keep that output with the
device log as the run's revision identity. It also selects the Game Profile's
512 KiB per-script envelope in the boot configuration, so a direct run and an
installed game admit the same package. Never edit a staged directory: if its
contents no longer match the marker, the helper refuses it. It records a
managed binding beside `boot.cfg`, and `scripts/psplink-device.sh` rehashes
and restages the current source before every later memory or slot load;
pointing `boot.cfg` at another URL disables the binding. Leave
`input_script=` empty for a manual run, exit any resident Tilefinch normally
with HOME, then use `scripts/psplink-device.sh memory`. Do not force-unload a
live browser module.

Do not stage with a plain `cp`, reuse a stable game directory, or claim a
revision from the EBOOT alone. An end capture showing a title, pause, Deploy,
or completion panel invalidates a gameplay soak even if the WebGL counters
look healthy. Prove the loaded revision:

- the HTTP server records PSP requests for `index.html`, `game.css`, and
  `game.js` from the device address;
- the requested directory digest matches the helper's `stage-digest` output;
- `tilefinch-validation.txt` reports one discovered and loaded game script,
  no JavaScript error, and an interactive-ready page;
- a PSPLink screenshot before input, then Play, shows the Page-controls
  notice and Start+Select exits;
- cadence runs use the committed input script and its qualification URL, not
  the manual staging configuration.

An installed copy in **Library → Saved** is a sealed snapshot that source or
staging changes do not touch. Open the current network-backed page, choose
**Menu → Page tools → Install offline app**, check that the preview says
Update or Reinstall and lists the expected resources, and finish that before
testing the library entry. An integrity error means the stored document and
resource pack disagree; uninstall and reinstall the whole snapshot rather than
replacing files. Finally, verify offline launch with the network unavailable:
the launch itself must not associate Wi-Fi.
