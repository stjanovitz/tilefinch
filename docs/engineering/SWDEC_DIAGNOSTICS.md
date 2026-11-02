# Software-decoder diagnostics

For maintainers: how to check, profile, and compare the optional PSP
software decoder add-on. Building and installing it is in
[Development](../DEVELOPMENT.md#optional-psp-software-decoder); the device
loop is in [PSPLink device development](PSPLINK_DEV_LOOP.md).

## Two library variants, one switch

`SWDEC_DIAGNOSTICS` (`src/swdec/swdec_diag.h`) separates profiling builds from
acceptance builds. `scripts/prepare-swdec-ffmpeg.sh` builds the FFmpeg
libraries twice:

| Libraries | Contents | Used by |
|---|---|---|
| `libs/asm22` | no diagnostics | every tree except validation; the release component |
| `libs/asm22-diag` | per-stage clock, stage marks, experimental and no-op deblocking kernels and their test hooks, the reference kernel variants below | a validation tree's component (`SWDEC_DIAGNOSTICS=1`) |

A validation tree's diagnostic component adds logging, Media Engine liveness
and stack reports, fault injection, CPU audio cross-checks, and the stage
report. The finer flags (`SWDEC_STAGE_DIAG`, `SWDEC_ME_DIAG`,
`SWDEC_ME_DIAG2`, `SWDEC_SPIN_DIAG`, `SWDEC_ME_OFF`, `SWDEC_POISON`, and in
FFmpeg `SWDEC_STAGE_TIMING` and `SWDEC_PIPE_VALIDATE`) fail the build without
it, and a component linked against the other variant's libraries fails to
link. The preparation script accepts only a pristine or exactly patched
tree, so prepare a fresh output directory whenever the committed patch
changes.

A validation tree builds two add-ons:

| Add-on directory | Target | Use |
|---|---|---|
| `tilefinch-swdec-addon` | `tilefinch-swdec-bundle` | diagnostics, for profiling |
| `tilefinch-swdec-addon-release` | `tilefinch-swdec-release-bundle` | no diagnostics, for acceptance runs |

## Bit-exactness gate

`tools/swdec-bench/crc-gate.sh` checks the decoder bit for bit against host
FFmpeg under PPSSPP. The clip set covers CAVLC and CABAC with five reference
frames at 416x234 and 426x240, a 16-reference High clip at 432x240, and clips
with 4x4 partitions, several slices per picture, and weighted prediction.

Every clip is decoded four ways:

1. as is;
2. with chroma predicted by the parser, as on the Media Engine;
3. with the parser stopping part-way through rows, so reconstruction predicts
   the rest;
4. as 3, with the decoder closed mid-stream and reopened after 37 and 61
   access units, as a seek's reset and a cancel do.

The gate also checks that each clip's decoder-arena peak fits the arena the
player sizes for it. CTest runs it as `tilefinch-swdec-crc-tests` when
configured with `-DTILEFINCH_SWDEC_CRC_GATE=ON` and
`-DTILEFINCH_SWDEC_CRC_SOURCE_DIR=` a prepared workspace. Prepare a fresh
workspace whenever the committed FFmpeg patch changes.

## Boot-configuration masks

A validation browser reads these `boot.cfg` keys for same-binary A/B runs.
All default to 0 (shipping behavior), and the browser refuses values outside
the ranges shown.

### `validation_swdec_reference` (0–2047)

Bits 1–128 go to a validation add-on in its longer start block (same
component ABI; the browser logs `tilefinch-swdec: validation flags`). Bits
256–1024 act in the browser. Definitions:
`include/tilefinch/swdec_component.h` and `include/tilefinch/media_backend.h`.

| Bit | Selects | Bench `decode` key |
|---:|---|---|
| 1 | the reference motion-compensation kernels instead of the shared-pass hv family (`swdec_mc_reference`) | `mc=ref` |
| 2 | every macroblock on FFmpeg's generic reconstruction path (`swdec_mb_generic`) | `mb=generic` |
| 4 | fixed 21x21 edge-emulation regions (`swdec_edge_full`) | `edge=full` |
| 8 | jobs that carry the whole parsed caches (`swdec_job_full`) | `job=full` |
| 16 | chroma motion compensation one plane per call (`swdec_chroma_split`) | `chroma=split` |
| 32 | the no-op prefetch calls made (`swdec_prefetch_calls`) | `prefetch=calls` |
| 64 | chroma prediction in reconstruction instead of on the Media Engine | |
| 128 | the chroma qualification replay (below) | |
| 256 | the accepted-block audio clock and pre-feed frame-selection timestamp | |
| 512 | refresh-only waits, internal-job redraws, and routine HLS delivery ahead of presentation | |
| 1024 | at most nine usable picture slots | |

Bit 1024 is applied after ordinary allocation and recovery; unused allocated
slots stay owned until teardown, and `allocated` reports physical ownership.
Use 1536 against 1024 for a capacity-matched presentation comparison. Reject
a run unless its window identity reports exactly nine usable slots and
matching memory-bank placement, and keep the memory ceilings unchanged.

### `validation_playback_ab` (0–31)

| Bit | Selects |
|---:|---|
| 1 | software video at fixed full quality (no reduced decodes), so runs compare identical work |
| 2 | page chrome state refreshed every frame while a player covers the page |
| 4 | the picture ring may take slots above 0x0A000000, which the Media Engine and graphics engine cannot read, so the main CPU converts and scales those pictures |
| 8 | reduced-quality rungs drop non-reference pictures (one in two or one in four) instead of only skipping their deblocking |
| 16 | the alternative software-video pacing with its one-frame-ahead non-reference discard rule |

Compare 17 against 1 for a same-binary, full-quality pacing comparison.
`event=working-set-placed` gives `ring-high=high/placed/slots`.

### `validation_swdec_present` (0–3)

The browser draws the decoder's 16-bit (5650) pictures with the graphics
engine straight into the 16-bit page buffer, in 64-texel column slices, and
collects the draw just before the buffer is latched; the slot stays borrowed
until then. Frames with visible chrome, slots above 0x0A000000, geometry
without quads, and Sharp scaling use the software scaler.

| Value | Path |
|---:|---|
| 0 | graphics engine, sliced, collected at latch (shipping) |
| 1 | software scaler |
| 2 | graphics engine, unsliced (one sprite per quad) |
| 3 | graphics engine, synced straight after the submit |

The browser logs `tilefinch-swdec-present:` every 100 pictures: engine and
fallback counts, submit and residual wait time, displayed rate, and the CPU
of the presenting thread and the decode worker over the same window.

## Chroma on the Media Engine

While the Media Engine parses, it also predicts each inter macroblock's chroma
(both lists, averaging, explicit and implicit weights, edge extension) with
scalar kernels whenever it waits for the main core: for a free row buffer
(returning to parsing as soon as one is free) and while draining a slice. It
works through published rows macroblock by macroblock, writes each prediction
into the macroblock's job through the uncached alias, and publishes how many
of the row are done.

Reconstruction copies a done prediction before adding the residual and skips
its own chroma motion compensation. Otherwise it claims the rest of the row
and predicts itself, waiting (bounded) only for a macroblock the Media Engine
is part-way through. Without the Media Engine, reconstruction predicts.

A validation add-on logs `swdec-chroma-me:` when a stream closes: inter
macroblocks predicted by each core, macroblocks waited for and wait-loop
iterations, and rows the main core claimed.

With mask bit 128 the add-on also runs the qualification replay
(`src/swdec/swdec_chroma_replay.c`). One macroblock row in four is recorded
as the chroma prediction operations reconstruction performed, then replayed
through the main core's kernels and through private scalar kernels on the
Media Engine (from its idle hook), and both are compared with the decoder's
own prediction. `swdec-chroma-replay:` reports rows, mismatched macroblocks,
and per-row cost on each side; on the bench, with no Media Engine, the scalar
kernels run on the main core. The replay records only rows whose chroma
reconstruction predicts, so run it as 192 (bits 64 and 128).

## Close reports

A validation add-on (`TILEFINCH_PSP_VALIDATION_LOG=ON`) reports when each
stream closes. Shipping components have none of these lines.

| Line | Contents |
|---|---|
| `swdec-recon-cpu:`, `swdec-recon-kernels:` | the decode thread's time per output picture; one macroblock in eight has its kernels timed by the system clock (wall time, so read kernel buckets as shares) |
| `swdec-recon-qpel:` | luma motion-compensation calls by block size and quarter-sample position |
| `swdec-stages:` | decode thread totals in microseconds: decode wall and run time, the Media Engine slice, sleeps waiting for parsed rows, row stages, frame tail, colour conversion |
| `swdec-stages-me:` | Media Engine CP0 Count ticks by exclusive phase (shares add up to its running time; 64-bit totals), each beside the sampler's share in brackets with how many agree within the sampler's interval; `clock=none` when the boot probe saw Count stand still, `probe=` its three attempts |
| `swdec-stages-occupancy:` | needs no ME clock: the ME publishes its phase (idle, parse, deblock, wait, csc, audio, drain, off) and a main-CPU kernel alarm samples it at random 0.5–2.5 ms intervals; each share with its 95 % interval, the sample count, and the alarm handler's own time |
| `swdec-stages-wait:` | the decode thread's waits for parsed rows: how often a wake found a new row, how long after the ME published it the thread ran (ME Count put on the CPU clock by a ping before each slice), and how often other threads and interrupts preempted it |
| `swdec-stages-threads:` | for every eighth access unit, each thread's mean run time during the decode with a 95 % interval, samples left out because a thread clock could not be read, and the time no thread accounts for |

## Steady-playback window

Performance is scored on a window fixed in advance
(`src/swdec/swdec_window.h`), so startup is reported apart and no window is
chosen after the fact. The window opens on the first unique picture published
5 s after the first one and closes on the first published 30 s after it
opened. A picture counts once, when its buffer is published.

`tilefinch-swdec-realtime:` counts `displayed` pictures once each; `repeats`
are redraws of a picture already shown, and fps-x10 uses distinct pictures
only. For the last window of a video a validation browser prints:

| Line | Contents |
|---|---|
| `tilefinch-swdec-window-id:` | geometry, picture slots placed and above 0x0A000000, slot size, quality switches, free budget, Media Engine pool |
| `tilefinch-swdec-window:` | unique pictures, redraws, picture interval median/p95/max in ms, intervals held over 1.5 frame periods, the three longest with the picture time that ended each, audio clock advance, buffering, seeks; `complete=0` when playback stopped first |
| `tilefinch-swdec-window-work:` | access units submitted, pictures decoded and displayed, drops by reason, reduced-quality decodes, main, decode-worker and audio thread CPU, the worker's decode and conversion wall time, its idle gaps, audio blocks, underruns, output errors and gaps |

All are differences over the window; nothing is derived. With the diagnostic
add-on the component reports its stage figures for the same window between
`swdec-window: when=start` and `when=end` lines, and its close report then
covers only what followed.

Drops are disjoint. Units dropped before dispatch (late, skipped to a
keyframe, queue full) never count as submitted. Every submitted picture that
was not displayed has exactly one reason: dropped late by the worker, no
picture from the decoder, conversion refused, superseded by the presenter, or
still ready at the close. The remainder is work in flight at the window
edges; a remainder beyond four pictures, or a negative reason, is an
accounting error.

## Paired runs

`tools/swdec-bench/window-pairs.sh` runs alternating baseline and candidate
pairs of one recorded page through a device run wrapper with one add-on and
records the binaries and lanes. `window-pairs.py` scores the last window
record of each log:

- fps from unique published pictures over the window's own span;
- frame-interval figures and holds;
- CPU per decoded picture and per second, and the worker CPU the stage report
  leaves unexplained;
- where each lost picture went;
- the acceptance checks: complete window, no reduced decodes, seeks,
  buffering, underruns or output errors, and the audio clock at normal speed.

Lane medians are shown beside the runs, never sums or ratios across runs.

Scoring is strict by default. A run counts only with all three window
records, a closed window, passing checks, a closed ledger, a zero wrapper
status, unchanged binaries after the run, and a log newer than the run start
(`NAME-runs.txt` records each run). Invalid runs are listed with their
reasons and left out of the medians. The script exits nonzero when any run is
invalid, no run is valid, or either lane has fewer valid runs than requested.

| Setting | Effect |
|---|---|
| `BASE_ADDON_DIR`, `CAND_ADDON_DIR` | install a different add-on bundle before each lane, to compare two add-on builds on one browser binary; the identity file records both lanes' sha1s, and each run must use the shared browser and its own lane's add-on |
| `LOOSE=1` (`--loose`) | score whatever left a record, for profiling |
| `ALLOW_REDUCED=1` (`--allow-reduced`) | accept reduced decodes, for adaptive-mode comparisons |
