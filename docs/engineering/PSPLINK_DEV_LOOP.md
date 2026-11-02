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
and exposes the socket used by `pspsh`. A task restart, terminal exit, or
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
SDK tree to a validation build restarts the one recorded bridge instead of
starting a competing server or falsely reporting the old root as the new one.
It never kills an unknown bridge or spins indefinitely. If a bridge was started
outside this wrapper, stop it once and rerun `psplink-shell.sh ready` so the
wrapper can establish tracked ownership. `scripts/psplink-device.sh` and the
YouTube UX harness invoke this check automatically, so their documented
commands have no manual file-server prerequisite.

For automation, keep a foreground bridge session open for the entire app
lifecycle. A detached process is not a persistent-session guarantee under an
automation runner, and bridge disappearance alone does not distinguish runner
cleanup from a bridge crash:

```sh
PSPDEV=/path/to/pspdev HOST_ROOT=/absolute/stage scripts/psplink-shell.sh hold
```

Wait for `PSPLink hold ready` before launching. Keep that command's terminal or
execution session alive while running other commands with the **same exact
`HOST_ROOT`**. `hold` watches its recorded bridge PID and served root; loss or
replacement exits nonzero without silently creating another bridge or killing
one. It is a host-process lifetime check, not continuous USB/device health
verification. After normal app retirement and the usual module/thread/memory
checks, interrupt the hold session. Interrupting it does not itself terminate
the bridge. Never switch roots or restart the bridge while the app is live:
its open `host0:` handles, including buffered validation logs, may become
stale even if a later `ver` succeeds. A lost hold invalidates the run's host-I/O
continuity; do not resume qualification by silently reconnecting it.

### Host bridge crash during reconnect

A stock `usbhostfs_pc` can crash in `libusb_submit_transfer` when its filesystem
thread closes the shared USB handle while the async command thread is writing.
That is a host transport failure, not evidence that Tilefinch crashed. The
[host lifetime patch](../../tools/psplink-host/README.md) protects that handle
without serializing the blocking read and write paths:

```sh
scripts/build-psplink-host.sh --test /path/to/psplinkusb /path/to/host-tools
PSPDEV=/path/to/pspdev USBHOSTFS=/path/to/host-tools/usbhostfs_pc \
  HOST_ROOT=/absolute/stage scripts/psplink-shell.sh hold
```

The builder uses a pinned Git archive, ignores dirty checkout files, runs
mocked concurrency checks with `--test`, and never installs into the SDK or
contacts the device. Select it only after normal app exit and retirement
checks, with the old tracked bridge stopped. `USBHOSTFS` does not automatically
replace a running bridge. A bridge loss under a live app invalidates host-I/O
continuity; do not hide it with automatic replacement or reuse a partial
report as a pass. Preserve the host crash report separately from device logs.

`usbhostfs_pc` binds a local host socket. Sandboxed shells may deny that
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
Media Engine firmware, which causes delayed AVC/AAC watchdog timeouts in
otherwise healthy playback. The wrapper requests `scrshot-user`,
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

**Do not type `quit` or `exit` to detach a persistent host console.** Those
commands are forwarded to PSPLink on the device and return it to XMB; they
are not host-only disconnects. Close only the host `pspsh` process (for
example, with a bounded launcher's SIGTERM), after capturing the terminal
receipt and checking module/thread/memory retirement. Keep the USB bridge
alive until those checks and any clock restoration are complete.

## One cycle

The JavaScript-profiler and replay boot keys used with this loop are in
[Replay and reference lab](REPLAY_LAB.md#device-profiler-and-replay-keys).

### Preferred zero-Memory-Stick cycle

For ordinary iteration, build and load the browser PRX directly from `host0:`:

```sh
PSPDEV=/path/to/pspdev scripts/psplink-device.sh memory
```

The wrapper builds only `psp-browser-script-dev-prx`, bounds PSPLink commands
so a disconnected device cannot hang the shell, and starts the fresh PRX.
That command confirms **launch**, not completion: the validation file is
buffered and can end at `interactive-ready` while the device is still running.
For a self-terminating `input_script=` or `interactive_validation_ticks=` run,
use `scripts/psplink-device.sh memory --wait`. It waits for a newly rotated
report with `tilefinch-log: finish outcome=clean-exit healthy=1` and, when a
script is configured, its `complete` or `exit-action` outcome. The default
limit is 300 seconds (`PSPLINK_REPORT_TIMEOUT_SECONDS` overrides it); a
partial report is never a pass or proof of a crash. If the host0 report does
not finish, check the live module and PSPLink exception state before
attributing the result to Tilefinch. An isolated on-card validation directory
can distinguish a browser failure from a host0 logging failure.
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
| persistent compiled scripts (opt-in) | the directory named by `module_cache_dir=`, or `script-cache/` when **Keep compiled scripts** is on |

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

### USB-only installed-game tests

Game tests do not require an HTTP server or Wi-Fi. Build a local fixture with
the same authenticated offline-package writer used by installs:

```sh
cmake --build build-preset-release --target tilefinch-offline-library-fixture
build-preset-release/tilefinch-offline-library-fixture --web-app \
  /absolute/scratch/offline \
  'https://game.test/index.html?qualification=input&seed=12345' \
  examples/treadline-arena \
  $(awk '!/^#/ && NF { print $1 }' examples/treadline-arena/package-files.txt)
```

The list is the game's canonical package list,
`examples/treadline-arena/package-files.txt`. A `?qualification=` URL needs
the `qualification.js` it marks (the page fetches it); an ordinary package
uses `awk '!/^#/ && NF == 1'` instead.

The output directory must not already exist. Only the explicitly listed flat
files are packaged; symlinks and parent paths are refused. The tool reads the
package back into a fresh session and checks the resource grants—not just the
bytes—so style/script consumers can use it without falling through to a network
request. No game files are fetched while creating the fixture.

Preserve any existing `offline/` directory in the served build directory, then
stage this fixture there as `host0:/offline/`. Set `url=` to the app URL printed
by the tool, use the ordinary button-input script, set
`validation_game_audio_mute=1`, and launch with `psplink-device.sh memory --wait`.
The offline startup route skips network association. Require the log's
`network: deferred for local startup`, successful CSS/script loading, a rendered
menu, and a gameplay marker before accepting performance measurements. Compare
rendered frames with the same game revision and settings; a static HTML shell
with missing resources is a failed harness run, not fast gameplay.

Validation builds use the same native audio slots as shipping. For an A/B
comparison, `validation_game_audio_reference=1` selects the JS Web Audio
reference (`game-audio.js`, compiled only into host and validation builds)
before runtime creation. The default is `0`; only these exact two values are
accepted. Selection is
fixed for each runtime and inherited by subsequent page/frame runtimes.
Shipping builds clear this validation setting, and saving user settings never
persists it. Keep output muted with `validation_game_audio_mute=1`; this does
not turn off the mixer. Compare fresh-process runs of one binary, using the
same recorded inputs and simulation times. A selected flag alone is not proof
that the lazy audio backend installed successfully.
Use `audio-slots-pre`, `audio-slots-post`, and `audio-slots-end` input-script
marks to request status. These queue bounded requests; the next owner-loop
checkpoint reports runtime identity, navigation generation, selection, lazy
installation, and failure state. Require each `queued=1` request and its
matching deferred receipt, with the requested/selected lane equal to the
sealed test configuration and post/end `ready=1`. Missing receipts are a
failed comparison, not permission to inspect a busy runtime from a supervisor.
Validation builds start with the shipping mixer: `audio-mix`, `audio-const`
and `audio-auto` all on. For a complete mixer OFF/ON comparison, set all three
marks (`audio-mix-off/on`, `audio-const-off/on`, `audio-auto-off/on`)
explicitly before the measurement window.

Keep baseline and candidate packages separately, restore the original `offline/`
directory and `boot.cfg` afterwards, and never stage these test packages to
`ms0:`. Installed-package loading and HTTP loading have different startup costs;
only compare matched USB-only runs when evaluating a gameplay change.

The persistent compiled-script tier (classic scripts and modules) is off
unless **Keep compiled scripts** is on or `boot.cfg` names
`module_cache_dir=`, which takes precedence. A host0 launch keeps the data
directory on the Mac, so the option's `script-cache/` lands there; point
`module_cache_dir=` at `host0:/...` for a separate directory.
`module_cache_write=1` lets that directory be written (idle work only), and
`0` (the default) only reads what a previous run wrote. A cold/warm
comparison runs twice against the same directory, the first run writing.
A writing run into a full directory removes its least recently used packs
during idle work. Never point a test run at `ms0:`.
`scripts/script-cache-device.sh [BUILD_DIR] [PAGE]` stages exactly that for
a census page (default `mdn`; not Wikipedia, whose page JavaScript is off by
default on the PSP, so nothing compiles) and runs the unmodified
`psplink-device.sh memory --wait` cold and then warm. A validation build
logs `tilefinch-script-cache: configured ...` at boot and, at every input
script mark, a `tilefinch-script-cache:` line with the page's compiles and
compile time, classic hits, misses, queued and idle-time stores, and the
persistent tier's disk hits, synchronous load time, packs and bytes read
and written, rejects and evictions.

### A script ending inside the keyboard is a failed run

Modal text entry advances the input script outside the browser's frame loop.
If its submit/cancel sequence misses, it can consume the remaining steps while
the keyboard is still open. Validation cancels that modal and reports
`event=keyboard-input-exhausted` with a `stalled` outcome, then uses normal
cleanup. Completion in a cooperative receiver likewise reaches the main-loop
report path. A physical-input handoff still leaves control with the user.

Do not count `interactive-ready` or an exhausted script cursor as a search
pass: require the expected destination URL, a completed scenario, and clean
exit. The validation-only `tilefinch-text-input:` records identify keyboard
readiness and submit/cancel key handling without logging the entered text.

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
   look like new bugs. `profile-clean.cfg` is a copy of `profile.cfg` with
   every `R` position zeroed that you stage in the served build directory
   yourself; the build does not generate it.
4. **Launch**: use the XMB for ordinary/manual slotted tests. The separate
   `tfexec.prx` loop is reserved for validation EBOOTs whose checked
   `exit_to=` handoff returns directly to PSPLink; never use it with a
   shipping build that exits to the XMB.
5. **Wait**: a stability soak takes its configured duration plus boot, open
   and exit. Wait at least that long, then run the bounded probe every 15 s
   until PSPLink answers; the app's `exit_to` config returns to PSPLink on any
   clean exit, including harness-declared failures.
6. **Pull the log**:
   `pspsh -e "cp ms0:/PSP/GAME/TILEFINCH/DATA/tilefinch-validation.txt host0:/run-<name>.txt"`
   then move `build-preset-psp-validation/run-<name>.txt` out of the build
   tree **immediately**: rebuilds delete files left in `build-*`.
7. **AU dump hygiene** (only when `validation_media_au_dump=1`): the dump is
   buffered and written once to `host0:/tilefinch-au-dump.bin`. It has no
   `ms0:` fallback, so even this failure diagnostic performs zero Memory Stick
   I/O. Move it out of the served build directory immediately. Format TFAU v1
   (magic `PSP_MEDIA_AU_DUMP_MAGIC` in `src/media_backend_psp.c`).

### Installed-EBOOT remote cycle

Validation launchers pass a bounded, timestamp-only argv receipt to the browser.
The `tilefinch-boot-handoff` record reports launcher entry to handoff,
handoff to browser `main`, browser entry to first scanout, and handoff to first
scanout. No timestamp file or additional Memory Stick write is used. A missing,
malformed, stale, or non-monotonic receipt reports `valid=0`, never a fabricated
duration. The standard-loader fallback refreshes its handoff timestamp after
a failed CFW call.

Cross-process stamps use `sceRtcGetCurrentTick`, with its 1 MHz resolution
verified, and report `clock=rtc-us`. Kernel uptime resets during LoadExec:
subtracting kernel timestamps from separate processes can produce plausible
but wrong durations, or become negative for a small executable. Older handoff
records without `clock=rtc-us` are invalid. Within-process phase stamps still
use the kernel clock. A clock adjustment during measurement invalidates the
comparison; keep the RTC unchanged across matched runs.

Use `source=memory-stick` runs to assess installed-EBOOT loading. USB runs are
labelled separately and are not equivalent. The handoff interval includes
process replacement, loader I/O and C runtime startup, not just storage reads.
It does not measure XMB selection to launcher entry; that needs an external
observation or instrumentation before the launcher. Keep boot configuration,
storage, clock policy and update-trial state matched across comparisons, and
report repeated runs rather than calling an emulator/USB timing a hardware
Memory Stick result.

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

A media soak's `boot-overrides.cfg` sets the start URL, the exit path and the
soak harness:

```ini
url=https://m.youtube.com/watch?v=<id>
interactive_validation_ticks=12000
exit_to=ms0:/PSP/GAME/PSPLINK/EBOOT.PBP
validation_media_play=1
validation_media_stability_auto=1
validation_media_stability_seconds=120
validation_media_seek_permille=667
validation_media_reset_mode=2
validation_media_refusal_reset=1
```

| Key | Default | Meaning |
| --- | --- | --- |
| `interactive_validation_ticks` | 0 | dead-man exit cap, in frames |
| `exit_to` | none | executable to return to on exit (here, PSPLink) |
| `validation_media_play` | 0 | `1` presses Play once the open settles |
| `validation_media_stability_auto` | 0 | `1` runs the soak harness (two seeks) |
| `validation_media_stability_seconds` | 120 | soak length, 10 to 900 s; long gates opt in |
| `validation_media_seek_permille` | 667 | soak seek target as a fraction of duration; `0` disables seeks |
| `validation_media_lifecycle_auto` | 0 | `1` injects a logical quiesce/recover 30 s in (below) |
| `validation_media_reset_mode` | 0 | firmware reset on refusal: `0` in place, `1` recreate, `2` no touch |
| `validation_media_refusal_reset` | 0 | `1` enables reposition recovery on access-unit refusals |
| `validation_media_au_dump` | 0 | `1` dumps every access unit (256 KiB cap, truncated per run) |
| `validation_latch_probe` | 0 | `1` hashes full frames for flicker; invasive, opt in only |
| `validation_swdec_audio_scan` | 0 | `1` scans every software-decoded audio block for peaks |
| `validation_swdec_reference` | 0 | software-decoder reference-path mask ([SWDEC_DIAGNOSTICS.md](SWDEC_DIAGNOSTICS.md)) |
| `validation_playback_ab` | 0 | playback A/B mask ([SWDEC_DIAGNOSTICS.md](SWDEC_DIAGNOSTICS.md)) |
| `validation_swdec_present` | 0 | software-decoder presentation: `0` GE async (shipping), `1` CPU scaler, `2` GE unsliced, `3` GE synced at once |

An `input_script=` key arms the scripted-input harness instead; note its
stall detector treats media playback as not-ready — do not use plain `wait`
around video (see [Input script reference](INPUT_SCRIPT_REFERENCE.md)).
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
are external inputs. See [Input script reference](INPUT_SCRIPT_REFERENCE.md)
for the script language and deterministic frontend golden.

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
  stronger gate.
- **Media CDN velocity limiting**: after many runs in a day against one
  video, the media CDN answers range requests with 403 (even on freshly
  resolved URLs) or serves at a trickle. Rotate test video IDs, space runs, treat late-run range
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
