# Changelog

Newest first. Each release section is finalized from the Unreleased section
at cut time: the `## Unreleased` heading is renamed to
`## <version> — <date>` and a fresh empty `## Unreleased` section is added
above it. [docs/RELEASE_PROCESS.md](docs/RELEASE_PROCESS.md) describes the
mechanics.

## Unreleased

## 0.1.6 — 2026-08-20

- Improved YouTube playback, seeking, navigation, buffering recovery, and
  thumbnail performance; added audio-only playback and compact results.
- Added generic page-video and bounded HLS support, plus an optional build-time
  software-decoder add-on that remains separate from official releases.
- Replaced site-specific Reader profiles with bounded content analysis and
  added lazy-image sourcing for server-rendered pages.
- Reorganized menus around page tools, library, settings, and diagnostics;
  added signed installation of up to eight recent releases.
- Expanded mobile CSS, typography, compositing, Cyrillic, and Extended Latin
  support while preserving Wikipedia performance and fidelity.
- Hardened native media geometry, AAC channel bounds, script degradation, and
  aggregate backdrop-filter work.

## 0.1.5 — 2026-08-18

- Improved YouTube startup and recovery when the shared transport worker is
  busy or a delivery candidate stalls.
- Added photographed diagnostic reports with exact, bounded QR rendering and
  complete multipart recovery for large logs.

## 0.1.4 — 2026-08-17

- Improved player-control redraw stability during interactive seeking.
- Localized YouTube result and watch metadata from the PSP language and date
  settings without delaying native startup.
- Improved navigation responsiveness by promptly retiring abandoned page
  requests before the next page starts.
- Accepted larger real-world response cookie sets while preserving bounded,
  fail-closed redirect handling.
- Reduced cooperative YouTube parsing work on PSP.

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
