# Changelog

Newest first. Each release section is finalized from the Unreleased section
at cut time: the `## Unreleased` heading is renamed to
`## <version> — <date>` and a fresh empty `## Unreleased` section is added
above it. [docs/RELEASE_PROCESS.md](docs/RELEASE_PROCESS.md) describes the
mechanics.

## Unreleased

## 0.1.13 — 2026-08-26

- Added selectable Midnight, light, and custom RGB565 themes, with a visual
  theme designer and matching launcher artwork.
- Improved YouTube audio and subtitle switching, caption rendering and style
  controls, and user-friendly track language names.
- Fixed video and subtitle flicker, partial loading frames, and startup A/V
  skew while keeping playback responsive when track menus are open.
- Improved direct YouTube playback and search submission, and expanded the
  included Prism Break 3D game's keyboard and controller support.

## 0.1.12 — 2026-08-24

- YouTube live-stream and premiere support.
- Added audio and subtitle track selection plus preferred, alternate, original,
  and PSP-system language ranking. Captions remain off until selected.
- Added page-control mode and the Gamepad API, expanded Canvas 2D and Web
  Audio support, and started WebGL support. The source tree now includes the
  installable Prism Break 3D game.
- Added installable offline web apps with size previews, update and reinstall
  status, uninstall controls, icons, and bounded manifest presentation.

## 0.1.11 — 2026-08-23

- Added an optional ARK-4 XMB redirect that opens Tilefinch from Sony's
  Internet Browser icon, with an L-trigger bypass and fail-open behavior.
- Added isolated Wi-Fi sign-in for captive portals without weakening normal
  HTTPS verification or sharing ordinary browsing cookies and storage.
- Added YouTube live-stream and premiere support, improved native page audio
  and video routing, and hardened playback, seeking, buffering, and player
  presentation across changing network and media conditions.
- Expanded Canvas 2D for charts, controls, and simple games with native text,
  clipping, paths, sprites, animation batching, shadows, and safer bounded
  raster work.
- Added bounded bidirectional page text, Arabic-family shaping, and optional
  Arabic and Hebrew glyph-pack support while preserving the fast LTR path.
- Improved Reader mode, long-page rendering, script-failure degradation,
  responsive layouts, and static fallbacks for animation-heavy pages.
- Added clearer contextual failure recovery, richer site information, and a
  resumable download manager with bounded Memory Stick writes.
- Improved PSP chrome composition and made HOME-button exit responsive even
  while the browser is cancelling foreground work.

## 0.1.10 — 2026-08-22

- Improved time to playback from YouTube results: Play now opens the native
  player directly and can reuse preparation performed while a result is
  selected; Details remains an ordinary watch page.
- Made YouTube result pages interactive before thumbnails arrive, then load
  and reveal visible thumbnails progressively without stale placeholders.
- Prioritized the focused result's decoded thumbnail before speculative video
  resolution, and let a focus change redirect that work to the new row.
- Made video-resolution progress advance through its real preparation stages
  instead of appearing stuck near the start.
- Fixed confirmed seeks so playback resumes automatically when the video was
  playing, while seeking from Pause continues to preserve the paused state.
- Kept the player timeline stable while a committed seek is loading instead
  of briefly replacing the footer, prevented moving video from bleeding into
  the opaque control strip, and restored one-press Play activation on YouTube
  results when a late relayout invalidates the autofocus region.
- Kept result-page controls responsive while JPEG thumbnails decode and
  arrive, preserving focus and deferred-image order across relayouts.
- Made video opening retry stalled CDN ranges on bounded fresh connections
  before discarding an otherwise valid resolved stream.
- Reduced startup and first-navigation delay by presenting native Home before
  deferred site-data restoration and staging page fonts around network work.
- Expanded HTTPS compatibility and made certificate failures more actionable
  with cause-specific guidance and richer diagnostics.

## 0.1.9 — 2026-08-21

- Fixed the YouTube info control so it opens the watch page without starting
  playback; the result card and watch-page thumbnail remain explicit play
  controls.

## 0.1.8 — 2026-08-21

- Made in-app updates faster while retaining complete package and file
  verification.
- Improved YouTube startup when thumbnails or abandoned page requests are
  still using the shared network worker.
- Improved deferred image loading and recovery after transient failures,
  with less browser-thread work while a page is open.
- Improved playback recovery from transient PSP audio-decoder refusals.

## 0.1.7 — 2026-08-21

- Improved mobile page fidelity across responsive headers, forms, flex and
  grid layouts, inline SVG, layered backgrounds, and script-staged static
  content.
- Added generic structured-audio discovery and native playback for authored
  page audio without requiring full application hydration.
- Improved script compatibility with inline event handlers, cursor hover
  events, correct `nomodule` handling, and soft degradation when optional
  scripts cannot run.
- Hardened flex arithmetic, media-candidate lifetimes, audio-only range
  requests, and media lifecycle transitions under PSP memory limits.
- Added reproducible mobile-viewport capture tools and release checks that
  prevent local build paths from leaking into PSP artifacts.

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
