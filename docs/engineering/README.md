# Engineering manuals

These pages are current subsystem contracts and reproducible workflows,
organized by the part of the system an engineer is changing. Investigation
logs and raw evidence live outside the public repository. Top-level contracts
remain authoritative when a focused manual is narrower.

## PSP constraints and qualification

- [PSPLINK_DEV_LOOP.md](PSPLINK_DEV_LOOP.md) — bounded build/flash/run/log workflow for a real PSP.
- [DEVICE_QUALIFICATION.md](DEVICE_QUALIFICATION.md) — which claims host, PPSSPP, and hardware can prove.
- [PSP_ENVELOPE.md](PSP_ENVELOPE.md) — memory, CPU-slice, executable-size, and storage budgets.
- [MEMORY_EXPERIMENTS.md](MEMORY_EXPERIMENTS.md) — accepted and rejected memory experiments, with explicit conditions for revisiting them.
- [PERF_JOURNEYS.md](PERF_JOURNEYS.md) — deterministic PPSSPP performance journeys (trace replay at 111 MHz) and their lower-only baselines.
- [PERFORMANCE_LEDGER.md](PERFORMANCE_LEDGER.md) — selected host and device measurements, accepted improvements, rejected approaches and measurement limits.
- [BUILD_SPEED_EXPERIMENT.md](BUILD_SPEED_EXPERIMENT.md) — the measured targeted host incremental build-speed experiment (base `2ad0b27c`).
- [NATIVE_TIER_INVESTIGATION.md](NATIVE_TIER_INVESTIGATION.md) — native-tier feasibility findings, the decision to park the research and conditions for revisiting it; not a shipping feature.

## Labs and acceptance

- [LAB_USAGE.md](LAB_USAGE.md) — static and interactive desktop frontend options.
- [CANDIDATE_ACCEPTANCE.md](CANDIDATE_ACCEPTANCE.md) — interactive acceptance scenarios and gates.
- [INPUT_SCRIPT_HARNESS.md](INPUT_SCRIPT_HARNESS.md) — deterministic input scripts and the device loop.
- [REPLAY_LAB.md](REPLAY_LAB.md) — strict and response-keyed replay, reference capture, acquisition safety, and frame comparison.
- [YOUTUBE_VIDEO_LAB.md](YOUTUBE_VIDEO_LAB.md) — host-side media and PSP playback seams.

## Retained subsystem contracts

- [STREAMING_NAVIGATION.md](STREAMING_NAVIGATION.md) — bounded transport, parsing, progressive paint, and rollback.
- [PSP_MEDIA_SESSION_STATE.md](PSP_MEDIA_SESSION_STATE.md) — authoritative media-session state machine and validation contract.
- [PSP_NETWORK_SUPERVISOR.md](PSP_NETWORK_SUPERVISOR.md) — network target reconciler and lease-based teardown.
- [PSP_TRANSPORT.md](PSP_TRANSPORT.md) — owned curl/Mbed TLS/nghttp2 transport, worker boundary, and TLS acceleration.
