# Unified PSP browser hardware validation

This is one diagnostic EBOOT, not the browser engine. Copy it once and run it once. It automatically executes the hardware-sensitive checks the desktop lab cannot answer and writes one versioned `validation.txt` log.

The v3 run is fixed at the selected 333 MHz operating target and includes:

- Firmware, CPU/bus clock, power, and battery readings.
- Total/free heap readings and a touched largest-contiguous-allocation probe while the 16-tile cache is resident.
- Sixteen rounds of varied small-allocation pattern verification (1,536 allocations in the reference run).
- A deliberate 48-block, 11.25 MiB fragmentation pattern followed by largest-block recovery measurement.
- 4-, 8-, 12-, and 16-entry RGB565 tile-cache suites, each sampled three times.
- Deterministic top/middle checksums after eviction and backward scrolling.
- A ChatGPT-shaped rounded composer and fixed-overlay test at three scroll offsets, with pixel samples, invariant checksums, and separate base/overlay timings.
- A checked 256 KiB Memory Stick write/read/delete round trip.
- A synthetic `validation.ppm` framebuffer export for optional corruption inspection.

Build with an installed PSPDEV SDK:

```sh
export PSPDEV=/path/to/pspdev
export PATH="$PSPDEV/bin:$PATH"
make -C psp-validation
```

Run `EBOOT.PBP` in PPSSPP or from `ms0:/PSP/GAME/PSPBVAL/EBOOT.PBP`. It displays progress, writes `validation.txt` and `validation.ppm` beside the executable, shows a summary for five seconds, and exits automatically.

Return only `validation.txt` unless the PSP screen looked corrupted. A complete log begins with `schema=psp-browser-validation-v3`, contains `run_complete=yes`, and ends with `run_end=1`. A truncated or missing marker means the run did not finish.

The overall failure mask covers cache allocation, the 24 MiB contiguous-memory gate, allocator integrity, fragmentation recovery, tile checksums, timing limits, rounded/fixed primitive correctness, and Memory Stick I/O. Clock comparison is intentionally disabled; the app records and runs at 333 MHz. Emulator success verifies packaging and deterministic execution; only a physical PSP-3000 supplies final memory and performance evidence.
