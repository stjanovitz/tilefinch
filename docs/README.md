# Documentation map

Tilefinch's documentation has three layers:

1. **Release-facing pages at the repository root** — for PSP owners:
   [README](../README.md) (what it is, install, controls),
   [TROUBLESHOOTING](../TROUBLESHOOTING.md), [SECURITY](../SECURITY.md),
   [CHANGELOG](../CHANGELOG.md), [CONTRIBUTING](../CONTRIBUTING.md), and
   [THIRD_PARTY_NOTICES](../THIRD_PARTY_NOTICES.md).
2. **Current reference contracts in this directory** — the authoritative
   architecture, security, storage, update, and feature behavior.
3. **Engineering manuals and lab procedures in
   [engineering/](engineering/README.md)** — focused subsystem contracts and
   reproducible workflows.

## Current contracts and workflows

- [Architecture](ARCHITECTURE.md): the concise system guide—ownership,
  transactional navigation, bounded work, rendering, transport, lifecycle
  machines, media, frontend, storage, updates, and validation.
- [Development](DEVELOPMENT.md): fast edit loop, presets, focused tests,
  generated-source workflow, source/target boundaries, and dependency
  acquisition.
- [Security model](SECURITY_MODEL.md): canonical URLs, request context,
  redirects, cookies, cache identity, trace handling, and explicit
  omissions.
- [Content blocking](CONTENT_BLOCKING.md): built-in and custom-list modes,
  supported uBlock/EasyList network syntax, limits, and per-site allowlisting.
- [Reader mode](READER_MODE.md): reversible presentation transform,
  typography controls, site profiles, and explicit limits.
- [Offline library](OFFLINE_LIBRARY.md): bounded Reader snapshots and
  resumable YouTube downloads, including storage and trust boundaries.
- [Memory Stick storage](STORAGE.md): the complete on-stick layout map,
  per-file size bounds and eviction, crash-safety disciplines, and
  free-space requirements.
- [Secure in-app updates](SECURE_UPDATES.md): signed Stable/Beta records,
  the explicit unsigned Developer path, producer workflows, A/B installation,
  stable launcher, rollback, and remaining production-enablement gates.
- [Agent conventions](../AGENTS.md): the gates, ratchets, and engineering
  rules a change has to satisfy.

## Active qualification guides

- [Web Platform Tests](WPT.md): repository-owned, compact upstream, and
  expanded exploratory HTML/CSS lanes.
- [Fidelity workflow](FIDELITY.md): the visual-fidelity scoreboard, corpus
  rules, floors, and the never-rebaseline-a-regression policy.
- [Device qualification](engineering/DEVICE_QUALIFICATION.md): the physical
  PSP checks required before a release claim, including media, networking,
  memory pressure, lifecycle, and updater coverage.

## Engineering manuals

The focused index for PSP resource constraints, media and network state
machines, transport, deterministic replay, scripted input, and acceptance is
[engineering/README.md](engineering/README.md). Raw investigations and one-off
evidence are kept outside the public repository; these pages describe the
current system and how to reproduce its claims.
