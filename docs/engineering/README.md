# Engineering manuals

These pages are current subsystem contracts and reproducible workflows,
organized by the part of the system an engineer is changing. Investigation
logs, performance and memory experiment ledgers, and raw evidence are kept by
maintainers outside the public repository. Top-level contracts remain
authoritative when a focused manual is narrower.

## Device

- [PSPLINK_DEV_LOOP.md](PSPLINK_DEV_LOOP.md): build, flash, run and collect
  logs on a real PSP over PSPLink, and the soak boot keys.
- [DEVICE_QUALIFICATION.md](DEVICE_QUALIFICATION.md): which claims host,
  PPSSPP and hardware can each prove, and the lifecycle matrix.

## Budgets

- [PSP_ENVELOPE.md](PSP_ENVELOPE.md): memory, CPU-slice, executable-size and
  storage budgets, and the link-time ratchets.

## Input harness

- [INPUT_SCRIPT_REFERENCE.md](INPUT_SCRIPT_REFERENCE.md): the input-script
  language, limits, arming, runners, outcomes and goldens.
- [INPUT_SCENARIOS.md](INPUT_SCENARIOS.md): pass criteria for each committed
  script in `tests/input-scripts/`.
- [VALIDATION_RECORDS.md](VALIDATION_RECORDS.md): what each validation log
  record means, and the attribution tools that read them (load timeline,
  execution census, script split, canvas capture, PC profile, PGO).

## Labs

- [LAB_USAGE.md](LAB_USAGE.md): command-line reference for the static and
  interactive desktop labs.
- [PRIVATE_ACCOUNT_LAB.md](PRIVATE_ACCOUNT_LAB.md): signing the lab into a
  dedicated test account with the private launcher and sign-in keeper.
- [REPLAY_LAB.md](REPLAY_LAB.md): strict and response-keyed replay, reference
  capture, acquisition safety, frame comparison, and the device replay keys.
- [CANDIDATE_ACCEPTANCE.md](CANDIDATE_ACCEPTANCE.md): interactive acceptance
  of candidate sites and reference-frame normalization.
- [PERF_JOURNEYS.md](PERF_JOURNEYS.md): deterministic PPSSPP performance
  journeys (trace replay at 111 MHz) and their lower-only baselines.
- [HOST_TEST_MODES.md](HOST_TEST_MODES.md): per-flag host test modes.

Lab tools with their own pages:
[site census](../../tools/site-census/README.md),
[site survey](../../tools/site-survey/NOTES.md),
[top-sites survey](../../benchmarks/top-sites/README.md),
[challenge-lab probes](../../tests/fixtures/challenge-lab/README.md),
[PSPLink host bridge patch](../../tools/psplink-host/README.md) and
[media-safe HOME-exit PSPLink](../../tools/psplink-home-exit/README.md).

## Media

- [MEDIA_PIPELINE.md](MEDIA_PIPELINE.md): provider adapter, range and HLS
  sources, demuxing, firmware and software decoders and route selection, the
  native player, qualification, and reading a soak log.
- [PSP_MEDIA_SESSION_STATE.md](PSP_MEDIA_SESSION_STATE.md): the media-session
  state machine and its validation contract.
- [SWDEC_DIAGNOSTICS.md](SWDEC_DIAGNOSTICS.md): software-decoder diagnostic
  switches and masks.

## Contracts

- [STREAMING_NAVIGATION.md](STREAMING_NAVIGATION.md): bounded transport,
  parsing, progressive paint and rollback.
- [PSP_NETWORK_SUPERVISOR.md](PSP_NETWORK_SUPERVISOR.md): network target
  reconciler and lease-based teardown.
- [PSP_TRANSPORT.md](PSP_TRANSPORT.md): owned curl/Mbed TLS/nghttp2
  transport, worker boundary, entropy and TLS reuse.
- [VIEWPORT.md](VIEWPORT.md): meta viewport resolution, CSS/device
  conversion, and how Page zoom composes with a page's own viewport.

## Parked directions

- **Native execution tier.** Native-tier and whole-graph ahead-of-time
  compilation are parked and part of no build; the interpreter with cached
  bytecode and PGO is the execution path. Reopen only for a materially
  different hypothesis with measured callback coverage and bounded code and
  metadata memory.
