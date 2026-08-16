# Engineering manuals

These pages are current subsystem contracts and reproducible workflows,
organized by the part of the system an engineer is changing. Investigation
logs and raw evidence live outside the public repository. Top-level contracts
remain authoritative when a focused manual is narrower.

## PSP constraints and qualification

- [PSPLINK_DEV_LOOP.md](PSPLINK_DEV_LOOP.md) — bounded build/flash/run/log workflow for a real PSP.
- [DEVICE_QUALIFICATION.md](DEVICE_QUALIFICATION.md) — which claims host, PPSSPP, and hardware can prove.
- [PSP_ENVELOPE.md](PSP_ENVELOPE.md) — memory, CPU-slice, executable-size, and storage budgets.

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
