# Documentation map

This map is for everyone: PSP owners, web and game developers, contributors
and maintainers. Tilefinch's documentation has three layers:

1. **Release-facing pages** — for PSP owners: at the repository root,
   [README](../README.md) (what it is, install, controls),
   [TROUBLESHOOTING](../TROUBLESHOOTING.md), [SECURITY](../SECURITY.md),
   [CHANGELOG](../CHANGELOG.md), [CONTRIBUTING](../CONTRIBUTING.md), and
   [THIRD_PARTY_NOTICES](../THIRD_PARTY_NOTICES.md); in this directory, the
   [user guide](USER_GUIDE.md) (tabs, typing, Reader mode, saving, YouTube,
   language packs, games, per-site controls) and
   [licensing, attribution, and independence](LICENSING.md).
2. **Current reference contracts in this directory** — the authoritative
   architecture, security, storage, update, and feature behavior.
3. **Engineering manuals and lab procedures in
   [engineering/](engineering/README.md)** — focused subsystem contracts and
   reproducible workflows.

## Current contracts and workflows

- [Architecture](ARCHITECTURE.md): the concise system guide—ownership,
  navigation that commits only when it succeeds, work limits, rendering,
  transport, lifecycle machines, media, frontend, storage, updates, and
  validation.
- Detailed state-machine audit notes and investigation evidence are maintained
  privately; current lifecycle contracts remain in the architecture reference.
- [Development](DEVELOPMENT.md): fast edit loop, presets, focused tests,
  generated-source workflow, source/target boundaries, and dependency
  acquisition.
- [Security model](SECURITY_MODEL.md): canonical URLs, request context,
  redirects, cookies, cache identity, trace handling, and explicit
  omissions.
- [Content blocking](CONTENT_BLOCKING.md): built-in and custom-list modes,
  supported uBlock/EasyList network syntax, limits, and per-site allowlisting.
- [Reader mode](READER_MODE.md): how pages are classified and simplified,
  typography controls, and limits.
- [Basic view](BASIC_VIEW.md): the script-free fallback that keeps a broken
  page's links, tables and search forms working.
- [Bidirectional text](TEXT_BIDI.md): UAX #9 page layout, Arabic-family
  shaping, logical/visual mappings, optional glyph packs and their in-page
  install offer, and degradation.
- [Chrome themes](THEMES.md): built-in palettes, downloaded themes, and the
  interactive color designer.
- [Game Profile v1](GAME_PROFILE.md): the developer and coding-agent contract
  for PSP-sized Canvas/WebGL games, including APIs, hard ceilings, lifecycle,
  offline packaging, fallbacks, and qualification. Its
  [JSON companion](tilefinch-game-profile-v1.json) is machine-readable.
- [WebGL authoring](WEBGL.md): the PSP-efficient shader, resource, animation,
  input, and offline-packaging profile for small games and charts.
- [WebAssembly](WEBASSEMBLY.md): the standards-facing JavaScript API,
  PSP ceilings, lazy runtime component, and deliberately unsupported
  proposals/import kinds.
- [Direct multiplayer](MULTIPLAYER.md): the RTCDataChannel-shaped
  installed-game API, LAN discovery, numeric invites, UDP hole punching,
  bounds, and limits.
- [Offline library](OFFLINE_LIBRARY.md): saved Reader articles, offline
  apps, and resumable file/video downloads, including storage and trust
  boundaries.
- [Memory Stick storage](STORAGE.md): the complete on-stick layout map,
  per-file size bounds and eviction, crash-safety disciplines, and
  free-space requirements.
- [Diagnostic QR reports](DIAGNOSTIC_QR.md): on-screen log transport,
  exact QR geometry, chunk and bundle formats, and desktop recovery.
- [Secure in-app updates](SECURE_UPDATES.md): signed Stable/Beta records,
  the explicit unsigned Developer path, producer workflows, A/B installation,
  stable launcher, rollback, and remaining production-enablement gates.
- [Release process](RELEASE_PROCESS.md): how a binary release is cut, what
  `scripts/cut-release.sh` automates, and the manual credentialed steps.
- [Diagnostic switches](DIAGNOSTIC_SWITCHES.md): every `TILEFINCH_*`
  environment variable the engine reads, generated from the source and
  checked by a test.
- [Agent conventions](../AGENTS.md): the gates, size limits, and engineering
  rules a change has to satisfy.
- [Interface translations](../translations/README.md): the translation
  catalog, its file format, and how new versions are published.

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

## Component READMEs

These pages sit beside the code or data they describe:

- [JavaScript bootstrap sources](../src/bootstrap/README.md): the built-in
  JavaScript and how its generated C files are regenerated.
- [Vendored QuickJS](../third_party/quickjs/README.md): the pinned engine
  and Tilefinch's patches to it.
- [Public Suffix List snapshot](../third_party/public_suffix/README.md) and
  [QR Code generator](../third_party/qrcodegen/README.md): smaller vendored
  components.
- [PSP TLS trust bundle](../certs/README.md) and
  [release trust root](../trust/README.md): the certificate roots and the
  update-signing keys.
- [Offline voice model](../psp-assets/voice-model/README.md): the optional
  voice-search model.
- [Treadline Arena](../examples/treadline-arena/README.md) and
  [Prism Break 3D](../examples/prism-break-3d/README.md): the example
  offline games.
- [PSP hardware validation](../psp-validation/README.md): the qualification
  EBOOT.
- [Top-sites census](../benchmarks/top-sites/README.md) and
  [site census and replay harness](../tools/site-census/README.md): the
  compatibility census tools.
- [libFuzzer targets](../tests/fuzz/README.md): the fuzzing harnesses.
- [PSPLink host bridge](../tools/psplink-host/README.md) and
  [PSPLink HOME-exit build](../tools/psplink-home-exit/README.md): tools for
  development on a PSP over USB.
