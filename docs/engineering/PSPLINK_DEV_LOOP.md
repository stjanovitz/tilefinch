# The PSPLink device dev loop

How to build, flash, run, and read a Tilefinch validation session on the real
PSP-3000, hands-free. It is written so an engineer with no local context can
run the loop safely and understand each constraint.

## One-time setup (usually already true)

- Device: PSP-3000 with ARK-4 CFW, PSPLink EBOOT installed at
  `ms0:/PSP/GAME/PSPLINK/`, USB cable connected. ARK-4 USB charging enabled
  (`always, usbcharge, on`) so the device stays powered indefinitely.
- Host: pspdev SDK. Every shell needs `PSPDEV` set to the SDK root and
  `export PATH="$PSPDEV/bin:$PATH"`; PSP cmake reconfigures additionally
  need `PSPDEV` exported (builds fail at reconfigure without it).
- The loader shim `tfexec.prx` is built into `build-preset-psp-validation/`
  (source: `tools/psplink-loop/`). It exists because ARK-4 rejects a direct
  `ld`/`modexec` of the browser EBOOT; tfexec LoadExecs it instead.

## The host bridge (automatic)

The PSPLink screen on the device is only half of the connection. On the Mac,
`usbhostfs_pc` must also be alive: it owns the USB connection, serves `host0:`,
and exposes the socket used by `pspsh`. A Codex task restart, terminal exit, or
host reboot can kill that process while the PSP continues to display PSPLink.
The resulting `connect: Connection refused` is therefore a missing **host
bridge**, not a reason to power-cycle the PSP.

Use the repository wrapper instead of raw `pspsh` for ad-hoc work:

```sh
PSPDEV=/path/to/pspdev scripts/psplink-shell.sh ready
PSPDEV=/path/to/pspdev scripts/psplink-shell.sh exec 'ver'
PSPDEV=/path/to/pspdev scripts/psplink-shell.sh exec 'ls host0:/'
```

It first performs a bounded probe. If the host bridge is absent, it starts
`usbhostfs_pc` detached, records its PID, served root, and log in one shared
temporary state directory, and waits a bounded 12 seconds for PSPLink. Keeping
the ownership record independent of `host0:` is important: changing from an
SDK tree to a validation build now restarts the one recorded bridge instead of
starting a competing server or falsely reporting the old root as the new one.
It never kills an unknown bridge or spins indefinitely. If a bridge was started
outside this wrapper, stop it once and rerun `psplink-shell.sh ready` so the
wrapper can establish tracked ownership. `scripts/psplink-device.sh` and the
YouTube UX harness invoke this check automatically, so their documented
commands have no manual file-server prerequisite.

`usbhostfs_pc` binds a local host socket. Sandboxed agent shells may deny that
operation even while the PSP visibly has PSPLink open. The wrapper detects the
specific `bind: Operation not permitted` failure, terminates the unusable host
process, and asks for host-socket permission; rerun the same command with that
permission instead of relaunching PSPLink. Do not leave the failed process
around and then mistake the resulting untracked bridge warning for a device
failure.

The special two-root installed-EBOOT workflow later in this document remains
manual because it deliberately exposes both `host0:` and `host1:`.

`host0:` paths resolve ONLY under the served directory. `host0:/../foo` does
not escape it (error 0x80010002). To land a pulled file elsewhere, pull to
`host0:/name` then `mv` locally.

## Link health

**Do not issue stock `scrshot`/`ss` while media is active.** PSPLink v3.2.1
uses partition 4 (`0x88300000`) for its bitmap scratch buffer, overlapping
Media Engine firmware. This caused delayed AVC/AAC watchdog timeouts in
otherwise healthy playback tests. The wrapper now requests `scrshot-user`,
which fails closed on old device builds. Build/install the
[media-safe HOME-exit PSPLink](../../tools/psplink-home-exit/README.md) before
using it. Until then, use Tilefinch's own input-script captures, not raw
`pspsh scrshot`. Do not attribute a capture-corrupted run to browser decoding.

```
PSPDEV=/path/to/pspdev scripts/psplink-shell.sh exec 'ver'
```

Healthy prints `PSPLink v3.2.1`. Never call `pspsh` bare in automation: a
missing device can hang instead of returning an error. `psplink-shell.sh`
provides the canonical bounded command runner.

## One cycle

### Preferred zero-Memory-Stick cycle

For ordinary iteration, build and load the browser PRX directly from `host0:`:

```sh
PSPDEV=/path/to/pspdev scripts/psplink-device.sh memory
```

The wrapper builds only `psp-browser-script-dev-prx`, bounds PSPLink commands
so a disconnected device cannot hang the shell, and starts the fresh PRX.
Exit a running Tilefinch instance normally with HOME first. The wrapper
deliberately refuses to force-stop it: PSPLink can remove a live module before
its browser, transport, or codec threads have unwound, leaving too little
clean user memory for the next load. The equivalent manual commands remain:

```sh
cmake --build build-preset-psp-validation --target psp-browser-script-dev-prx -j8
pspsh -e "ld host0:/psp-browser-script-dev.prx"
```

The PRX is a second link of the same target objects as the validation EBOOT;
it is not packaged for users. `argv[0]` makes `host0:` both the program and
data directory, so these files remain in `build-preset-psp-validation/` on the
Mac while the program runs:

| Purpose | Host file |
| --- | --- |
| configuration | `boot.cfg` |
| active/previous validation report | `tilefinch-validation.txt`, `tilefinch-validation.previous.txt` |
| active/previous crash record | `tilefinch-crash.txt`, `tilefinch-crash.previous.txt` |
| scripted input | the leaf named by `input_script=` |
| captured frames | `frame-*.ppm`, `frame-mark-*.ppm` |
| on-demand screenshots | `screenshots/` |

Fonts, roots, and development voice assets are staged beside the PRX by the
build. The optional software decoder is a separate
`tilefinch-swdec-addon/` bundle installed under `components/swdec/`, matching
the release EBOOT's runtime path.
The PSP still supplies the real network stack and codec firmware, but the run
performs **zero Memory Stick writes** unless the scenario explicitly invokes
an offline save, screenshot, download, cache, or update action. Logging also
avoids `sceIoSync("ms0:")` when its derived paths are on `host0:`. Loading the
module and its assets over USB changes startup timing, so compare host0 runs
only with host0 runs.

`psp-browser-script-dev-prx` uses PSPSDK's PRX link/fixup recipe and reuses the
EBOOT object files verbatim. It deliberately has no `PARAM.SFO`, package step,
install entry, or shipping ELF ratchet. Those gates remain attached to the
actual EBOOT.

Use the slotted EBOOT cycle below only for launcher/install-path validation.
It is unnecessary wear and timing noise for media iteration.

### Slotted EBOOT cycle

1. **Build and deploy** (never `build-preset-dev`):
   `PSPDEV=/path/to/pspdev scripts/psplink-device.sh slot`
   After any source change, rebuild before flashing. When a change adds
   telemetry, verify it is really in
   the binary: `strings build-preset-psp-validation/EBOOT.PBP | grep <field>`.
   The host preflights the EBOOT, `tilefinch-wasm.prx`, and, when enabled in
   that build's CMake cache, `tilefinch-voice.prx`. It stages that exact set,
   deploys the components first, and promotes the EBOOT last. Do not copy only
   the EBOOT after a component ABI change: wasm and voice correctly fail closed
   when paired with an incompatible PRX. A stale voice file in a voice-disabled
   validation build is not deployed merely because it exists.

   The development-only `tfdeploy.prx` accepts only `wasm`, `voice`, or `eboot`
   artifact names, never arbitrary paths. It reads each file from `host0:` in
   1 MiB units, writes its `.new` file in at most eight Memory Stick payload
   writes **per artifact**, checks its header and length, and then promotes it
   over slot A with `.previous` recovery. The running browser and component
   modules must be absent. This avoids PSPLink's 2 KiB `cp` loop and its in-place
   partial-file failure mode. The helper never ships in a release.

   Promotion is per-file, **not an atomic multi-file bundle**. After any error
   or interrupted deployment, do not launch the slot: rerun the complete
   command to reconcile all components and the EBOOT. Successful component
   promotions are not rolled back if a later artifact fails. No bootloader,
   voice-model data, settings, or offline-library files are changed by this
   command; install those separately when their contents change.
   The helper reserves only 64 KiB of newlib heap in addition to its fixed
   1 MiB transfer buffer, and retires the runtime before self-unloading.
   A helper that bypasses runtime cleanup can strand most of user RAM even
   after its module disappears; `meminfo` then shows a small maximum free
   block and the next load fails with `0x800200D9`. Restart PSPLink to recover
   an already-stranded allocation; never force-unload a running browser.
   Module-list checks normalize PSPLink's CRLF output so a resident helper
   cannot be missed by an end-of-line name match.
   Launch the deployed build normally from the XMB. Do not chain a shipping
   EBOOT through `tfexec.prx`: its ordinary `sceKernelExitGame` return can
   leave the XMB in a transient invalid state because it was not entered by
   the normal launcher lifecycle.
2. **Config** — slotted installs read `DATA/`, not the EBOOT's directory:
   `pspsh -e "cp host0:/boot-overrides.cfg ms0:/PSP/GAME/TILEFINCH/DATA/boot-overrides.cfg"`
3. **Reset the resume state when a scripted run requires it**:
   `pspsh -e "cp host0:/profile-clean.cfg ms0:/PSP/GAME/TILEFINCH/DATA/profile.cfg"`
   Why: the profile's `R <video-id> <position-us> <duration-us>` lines persist
   the resume position at exit — including a failed run's seek target. Without
   this reset, the next run resumes at a far cold offset and dies in ways that
   look like new bugs. `profile-clean.cfg` is the checked-in-to-the-build-dir
   copy with all `R` positions zeroed.
4. **Launch**: use the XMB for ordinary/manual slotted tests. The separate
   `tfexec.prx` loop is reserved for validation EBOOTs whose checked
   `exit_to=` handoff returns directly to PSPLink; never use it with a
   shipping build that exits to the XMB.
5. **Wait**: a full 120s stability soak ≈ 35s boot/open + 120s + exit ≈ 165s.
   Sleep ~150s, then run the bounded probe every 15s until PSPLink answers
   (the app's `exit_to` config returns to PSPLink automatically on any clean
   exit, including harness-declared failures).
6. **Pull the log**:
   `pspsh -e "cp ms0:/PSP/GAME/TILEFINCH/DATA/tilefinch-validation.txt host0:/run-<name>.txt"`
   then `mv build-preset-psp-validation/run-<name>.txt device-runs/` —
   **immediately**. Files left in `build-*` are deleted by rebuilds; two soak
   logs were lost that way. `device-runs/` (repo root, untracked) is the
   archive.
7. **AU dump hygiene** (only when `validation_media_au_dump=1`): the dump is
   buffered and written once to `host0:/tilefinch-au-dump.bin`. It has no
   `ms0:` fallback, so even this failure diagnostic performs zero Memory Stick
   I/O. Move it out of the served build directory immediately. Format TFAU v1
   (`build-preset-psp-validation/tfau-diff.py` parses/diffs).

### Installed-EBOOT remote cycle

Questions about the launcher, slotted data paths, `PARAM.SFO`, or update trials
must use the installed EBOOT. Build `tfexec.prx`, serve it as an additional
host directory, and use it to LoadExec the browser:

```sh
PSPDEV=/path/to/pspdev PATH="$PSPDEV/bin:$PATH" \
  make -C tools/psplink-loop
usbhostfs_pc build-preset-psp-validation tools/psplink-loop
```

```text
ld host1:/tfexec.prx
```

Set `exit_to=ms0:/PSP/GAME/PSPLINK/EBOOT.PBP` in the validation boot override
so a controlled exit returns to PSPLink. This two-hop path is slower than the
direct PRX and should be reserved for behavior that depends on the installed
layout.

## boot-overrides.cfg reference (soak shape)

```
url=https://m.youtube.com/watch?v=<id>
interactive_validation_ticks=12000        # dead-man exit cap
exit_to=ms0:/PSP/GAME/PSPLINK/EBOOT.PBP   # return to PSPLink on exit
validation_media_play=1                   # auto-press play at open-settle
validation_media_stability_auto=1         # the 2-minute soak harness (2 seeks)
validation_media_stability_seconds=120    # explicit 10..900s; long gates opt in
validation_media_seek_permille=667        # soak seek target; 0 disables seeks
validation_media_lifecycle_auto=0         # 1 injects logical quiesce/recover at 30s
validation_media_reset_mode=2             # 0 in-place / 1 recreate / 2 no-touch
validation_media_refusal_reset=1          # reposition-recovery on AU refusals
validation_media_au_dump=0                # per-AU dump (256KiB cap, truncate/run)
validation_latch_probe=0                  # invasive full-frame flicker hashes; opt in only
```

An `input_script=` key arms the scripted-input harness instead; note its
stall detector treats media playback as not-ready — do not use plain `wait`
around video (see INPUT_SCRIPT_HARNESS.md).
Use `mark-live` and `tap-live circle` as well: a readiness-gated marker or
close action can strand the harness while the player is legitimately busy.
Archive the selected URL from `tilefinch-input-focus-target`; live search
ordering can change between runs, so an identical query/input sequence does
not prove an identical video was replayed.

An `event=codec-watchdog` line means a firmware job did not return within the
bounded window, not that firmware returned the following synthetic BUSY
status. Validation builds record the worker status, wait type/ID and run
clocks at that verdict. Keep those fields and the later quarantine outcome
together; do not weaken quarantine to turn a stalled firmware owner into a
successful retry.

`validation_media_lifecycle_auto=1` publishes a paired suspend/resume notice
and lets the production main-loop state machine order quiesce before recovery.
It validates media teardown/reopen, display rearm, and network revalidation,
including a codec job that may be in flight. It deliberately does **not**
claim to emulate firmware sleep: user-mode PSP code has no timed wake API, so
use a physical power-switch cycle when a change specifically touches firmware
suspend/resume behavior. It is not a general release prerequisite.

Two additional validation modes isolate device boundaries:

- `validation_media_range_probe=1` resolves the configured media URL and
  crosses two fragment windows without invoking the decoder. Its outcome is
  network-dependent and diagnostic, while the loopback range test is the
  deterministic gate.
- `validation_csc_order_probe=1` decodes the embedded fixture and sweeps
  firmware color-conversion modes. It needs a physical Media Engine and has no
  meaningful PPSSPP pass.

The scripted scenarios `youtube-watch-live.txt` and
`youtube-results-live.txt` drive sustained playback and result activation.
They intentionally have no device golden because CDN and wireless behavior
are external inputs. See [Input script harness](INPUT_SCRIPT_HARNESS.md) for
the script language and deterministic frontend golden.

### Memory headroom

Run `meminfo` before loading the PRX and compare partition 2 `MAXFREE` with the
browser's `free-mem`, `max-free`, and `heap-capacity-lower-bound` boot lines.
The last value is the bounded admission sample taken before the aligned Media
Engine pool is reserved; validation deliberately does not exhaust the now-
fragmented heap a second time. PSPLink's
resident modules are kernel modules when `psplink.ini` keeps `pluser=0`, so
the user partition should retain the expanded PSP-3000 heap apart from the PRX
image itself. If it does not, check `pluser`, remove user modules from
`modload.ini`, or lower that validation run's `limit=`; do not change the
shipping heap declaration to accommodate a developer environment.

## Reading a soak log

- Lifecycle: `tilefinch-media-stability: event=started / event=seek /
  event=complete` (the 120s summary) or `event=failed reason=...`;
  `tilefinch-validation: outcome=` at exit. During playback, health and A/V
  skew are sampled into bounded RAM counters every 250ms without writing to
  host0. This is intentional: a USB-host write can pause the browser thread
  and manufacture the cadence hitch being measured.
- Stability verdicts: `pipeline-skips` are late, catch-up, prerequisite, or
  terminal pictures discarded before a presentation claim. `display-drops`
  are claimed identities that did not reach scanout, with `quiesce-drops` as
  the intentional-close subset. `max-av-skew` includes startup and seek
  discontinuities; `steady-max-av-skew` includes only samples after two
  uninterrupted seconds in `Playing`, and `steady-skew-samples` proves that
  bucket was actually observed.
- Cadence + pipeline: `tilefinch-media-feed:`,
  `tilefinch-media-transport:`, `tilefinch-media-present:`, and
  `tilefinch-media-video-cadence:` lines are emitted once at teardown; their
  fields are cumulative over the complete run. `tilefinch-video-scanout:` is
  the bounded histogram of wall intervals between distinct decoded pictures
  that actually reached scanout; repeated chrome-only presentation of one
  picture is excluded. The records are separate so every one fits the PSP
  logger's bounded line buffer. Per-picture ring traces:
  `tilefinch-media-picture:` only in builds configured with
  `TILEFINCH_PSP_MEDIA_PICTURE_TRACE=ON`.
- Occupancy: `tilefinch-media-slots:` and `tilefinch-media-worker:`, printed
  from the same teardown snapshot with the same `phase=` word, so the lines
  read together.
  These are **durations**, where the cadence line is counts: per-slot dwell in
  FREE/ME_WRITING/READY/READING, `no-free` vs `free-idle` (a writable slot
  with nothing being written — the direct measure of the worker not running
  ahead), the batch fill, the CSC itself, which frame phase each conversion
  landed in, and the worker's dispatch/prologue/firmware/collect ladder
  either side of the one shared job slot. Read the guide in the comment above
  `psp_media_report_slots` in `src/psp_media_telemetry.c` before drawing a
  conclusion from any single field.
- Never-taken paths, which should read zero: `dma-quarantine=` and
  `surface-quarantines=` on the `tilefinch-media-present:` line, plus
  `tilefinch-media-present: event=dma-quarantine*`. A quarantine without a
  matching late completion is a staging transfer that never came back, and
  the session ends on it deliberately.
- Firmware events: `tilefinch-media-decoder: event=...` records setup,
  refusals, recoveries, and failures. Steady job timing is aggregated in
  `tilefinch-media-worker:` rather than logged per job; watchdog failures name
  the exact stalled stage.
- The backend stats reader refuses while a codec job runs — a sample line
  showing `decoded=0 audio-packets=0` mid-playback is that artifact, not a
  stall.

### Transport scheduling attribution

The transport worker normally runs one priority step above the browser so its
bounded service and 10 ms poll cannot be starved. Connection setup is a
different workload: DNS, TCP, and TLS can remain inside one
`curl_multi_perform` call long enough to delay input and media collection.
Until response headers arrive, the worker therefore runs below the browser;
steady body transfer restores the ordinary higher priority.

Validation builds report setup and steady perform distributions as
`tilefinch-background-transport-worker*`. Read those beside the browser frame,
presentation, codec-collection, and input-latency histograms. The intended
signature is:

- setup calls may be long but do not create matching browser-thread stalls;
- steady body performs remain bounded closely enough to feed range windows;
- network work does not starve codec collection or input dispatch;
- a lifecycle run can quiesce, destroy, revalidate, reopen, and resume without
  transport, ownership, signature, or quarantine faults.

Run both an instrumented PRX and an uninstrumented release PRX when changing
this priority policy. The latter proves that validation logging is not shaping
the cadence being measured.

## Failure modes and remediation

- **App hard-freeze** (screen stuck, probe never answers): only the user can
  fix — power-cycle the PSP, relaunch PSPLink. The log survives on the card
  through explicit setup/failure checkpoints and its tail names the last
  committed phase. Steady-state telemetry is buffered so Memory Stick writes
  do not perturb playback cadence. Repeated network sessions can leave PSP
  firmware state unhealthy; the network supervisor must unwind or retain it
  safely rather than terminating beneath a live transport lease.
- **Host reboot / task/session death**: run `psplink-shell.sh ready` (the normal
  deploy and validation scripts do this themselves). If it reports that
  `usbhostfs_pc` is running but PSPLink is unreachable, replug USB or relaunch
  PSPLink on the device. The ignored `.tilefinch-usbhostfs.log` beside the
  build records whether the bridge found the PSP or failed before connecting.
  If it instead reports that the host environment denied the local socket,
  grant that host command permission and retry; the device is already ready.
- **ge-present-probe stalls (PPSSPP)**: the harness launches the GUI
  emulator with a hardcoded OpenGL backend; on macOS it intermittently goes
  "application not responding" during pre-boot (window-server/context
  starvation, worse under host load or from non-GUI shells — a no-context
  failure logs `FailedGraphicsBackends=OPENGL`). Zero PSP instructions run
  in that state, so a timeout is environmental, not a build verdict: retry
  up to twice on a quiet host, then defer to the device soak, which is the
  stronger gate. Durable fix if it keeps costing time: the headless PPSSPP
  binary (tooling backlog).
- **CDN velocity limiting**: after many runs in a day against one video,
  googlevideo 403s range requests (even on freshly resolved URLs) or serves
  at a trickle. Rotate test video IDs, space runs, treat late-run range
  failures on a worn video as environmental.
- **`(eval):1: ... not found` in zsh**: `=WORD` and `--include=*.c` style
  arguments hit zsh globbing — quote them.
- **Crash to XMB**: relaunch PSPLink manually; check
  `ms0:/PSP/GAME/TILEFINCH/DATA/tilefinch-crash.txt` (512-byte placeholder =
  no crash record).

## Iteration gates vs sign-off

While iterating on device behavior: host suites touched + both PSP presets
under hot-symbol ratchets + ge-present-probe. Goldens, menu-tour,
device-cost, and full ctest run once at sign-off, not per cycle.
