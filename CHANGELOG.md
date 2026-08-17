# Changelog

Newest first. Each release section is finalized from the Unreleased section
at cut time: the `## Unreleased` heading is renamed to
`## <version> — <date>` and a fresh empty `## Unreleased` section is added
above it. [docs/RELEASE_PROCESS.md](docs/RELEASE_PROCESS.md) describes the
mechanics.

## Unreleased

## 0.1.3 — 2026-08-17

- Improved YouTube playback recovery when a delivery URL serves only an
  unusable prefix or rejects later byte ranges.
- Added a bounded 360p-to-240p fallback and more complete last-error details
  for prolonged buffering and terminal media failures.

## 0.1.2 — 2026-08-16

- Expanded the curated PSP TLS trust bundle for more widely used certificate
  authorities and added a native-Mbed-TLS site audit to the release process.

## 0.1.1 — 2026-08-16

- Fixed GitHub certificate verification and expanded device network-error
  diagnostics.
- Added automatic recovery from a missing saved connection and a Wi-Fi
  profile selector under System options, including each saved profile's SSID.

## 0.1.0 — 2026-08-12

- Initial release.
