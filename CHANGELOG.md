# Changelog

Newest first. Each release section is finalized from the Unreleased section
at cut time: the `## Unreleased` heading is renamed to
`## <version> — <date>` and a fresh empty `## Unreleased` section is added
above it. [docs/RELEASE_PROCESS.md](docs/RELEASE_PROCESS.md) describes the
mechanics.

## Unreleased

## 0.1.29 — 2026-10-07

### Before you update

- **This version edits your `boot.cfg` once.** The largest single script
  Tilefinch accepts is now 4096 KiB (it was 512). If your `boot.cfg`,
  `data/boot-overrides.cfg` or an old `boot-live.cfg` still says exactly
  `file_kb=512`, the first start rewrites that line to `file_kb=4096` and
  adds a `#file_kb-migrated-from=512` comment, so it never does so again.
  Any other value you chose, including 512 set again after that comment,
  is left alone, as is a `boot.cfg` written by `scripts/stage-psp-game.sh`
  for a staged game, which keeps its 512 KiB Game Profile limit (see
  [Troubleshooting](TROUBLESHOOTING.md#a-site-misbehaves-because-of-its-scripts)).

### Large sites and memory

- Scripts are admitted by the memory their compile needs instead of a fixed
  size cap. A single script may now be up to 4 MB (was 512 KB) and a page's
  scripts up to 16 MB in total (was 2 MB), so the app bundles of many large
  sites run instead of being refused. A script that does not fit fails on
  its own; the page's other scripts keep running. Scripts inside frames get
  the same allowance.
- Stylesheets are admitted by what they cost once parsed rather than by
  three times their size. A sheet larger than the 768 KB per-file limit may
  grow to 2 MB when memory allows, and otherwise is applied up to its last
  complete rule instead of being thrown away, so such pages are no longer
  left unstyled.
- Pages up to 8 MB can be opened (was 4 MB).
- **Heavy pages.** Tilefinch now estimates how long a page's scripts will
  take to start. When a page that is only an empty app shell would need
  15 seconds or more, it asks first, with the size and an estimate: X
  starts it, Triangle always starts it on that site, O leaves the page as
  the server sent it. A page that already shows its text keeps loading in
  the background and says what it costs, and X stops its scripts. An app
  too large for the PSP even in the best case is not started; Tilefinch
  says how much memory it would need and offers Basic view, Reader mode or
  turning off the site's JavaScript. **Settings → Browsing & input → Heavy
  pages** chooses Ask (the default), Run or Basic view.
- If a page's scripts run out of memory and wipe the page as they fail,
  Tilefinch puts back the page the server sent and says "Page scripts
  stopped: not enough memory".
- **Basic view fallback** (**Settings → Browsing & input**) chooses what
  happens when a page's scripts fail or stop and leave it blank or
  unusable: Automatic (the default, as before) switches to Basic view, Ask
  offers **Show Basic view**, Off never switches. When a page failed to
  load after its document arrived, the recovery sheet can also **Reload in
  Basic view** with JavaScript off for that one load. Basic view now works
  on very large pages (it shows a shortened view instead of refusing), and
  Basic view and Reader mode show more of a page when memory allows.
- Images are decoded at the size the page shows them instead of at full
  size and then shrunk, and after layout, pictures and SVG icons that are
  drawn smaller than they were decoded are reduced to their painted size.
  On one image-heavy test page the peak memory fell from 29.6 MB to
  21.0 MB.

### Faster

- Revisited script-heavy pages compile far less. Compiled scripts now have
  their own 3 MB cache; in 0.1.28 they shared the 640 KB page cache with
  images and stylesheets and were almost always pushed out before a
  revisit. Compiled scripts are stored while the page is idle, not during
  the load, and the cache gives its memory back before a page runs short.
- **Keep compiled scripts** (**Settings → Device & storage → Site data &
  storage**) also keeps compiled scripts on the Memory Stick (at most 8 MB),
  so pages you visited before restarting Tilefinch start faster too. It is
  **Off** by default. **Clear compiled scripts** empties it.
- Sites that ship large webpack bundles (such as The Washington Post, XE
  and VK) start faster on the first visit: their bundles' functions are no
  longer all compiled up front just to check their syntax (on The
  Washington Post's home page this was about 8 seconds of PSP time).
  A function with a syntax error now reports it when it first runs. A
  revisit reuses the bundle's factory plan instead of working it out again.
- JavaScript garbage collection no longer thrashes when a page's memory is
  nearly full: in the emulator, one page's longest task spent 25.5 seconds
  collecting and now spends 4.3.
- Pages built with utility CSS frameworks such as Tailwind find their
  images much sooner: in the emulator, XE loads in 80 s instead of 116,
  weather.com in 74 s instead of 131 and Reddit in 26 s instead of 80.
- Large news pages relayout much faster: styles are kept across relayouts,
  container-query pages are laid out in one pass, and changes inside hidden
  content no longer cause a relayout.
- Pages that use only one of the bundled bold fonts load only that one.

### Pages that work better

- Pages that move on with `<meta http-equiv="refresh">` or a `Refresh`
  response header now do. An automatic reload of a page you have started
  using is offered instead ("Page wants to reload", Square reloads), and a
  chain of more than five refreshes without your input is stopped.
  `location.replace()` now replaces the history entry, so Back skips
  redirect pages.
- Documents in legacy single-byte encodings (such as windows-1251 and
  KOI8-R) and in UTF-16 now display correctly, and so do their scripts.
  Chinese, Japanese and Korean legacy encodings are not supported yet.
- Web components' styles stay inside their shadow trees, `:host` and
  `::slotted()` work, and slotted content is laid out where its slot is.
- CSS custom properties may be up to 1 KB long (was 95 bytes), `@property`
  registrations are honoured, and a gradient direction may be combined with
  an interpolation method, so Tailwind 4 gradients and shadows paint.
- Content Security Policy Level 3: nonces, script hashes, `'strict-dynamic'`
  and the separate element and attribute directives are honoured, so sites
  with modern policies (such as Reddit and weather.gov) run their scripts.
- `<noscript>` content shows when JavaScript is off for the page.
- Pages that check for `document.fonts`, `document.all`, printing or Event
  Timing now find them, and permission checks for sensors and other
  features the PSP lacks answer "denied" instead of failing. Also added or
  fixed: `document.links`, `document.anchors`, `document.scrollingElement`,
  rendered `innerText`, `window.length` and `window[i]` for frames,
  `window.origin`, `Range.createContextualFragment()`, `document.domain`
  relaxation, `navigator.sendBeacon()` (bodies were dropped), cross-origin
  `no-cors` fetches, many more `IntersectionObserver`s, and `Intl`'s default
  time zone.
- Layout: wide grids and grid placement, floats and clearfix, `clip-path`,
  sticky bars (no more doubled copies), backgrounds of inline boxes,
  baseline alignment of inline blocks, tables without explicit rows,
  `var()` in inline SVG colours and font stacks, and empty `@media` blocks.
  Page Down no longer scrolls text under bars pinned to the screen edges.
- Pages in Cyrillic and other scripts the built-in fonts lack now offer to
  install the matching language pack, showing its signed size before
  asking again. Installed packs take effect without a restart.
  **Settings → Appearance → Language & emoji → Offer language packs** turns
  the offer off.
- When a site's bot protection blocks Tilefinch, the page now says so
  instead of staying blank.
- **Auto Reader** now recognizes clear articles, decides page by page and
  shows a short note when it switches.
- The search compatibility page can retry regular scripted search with an
  honest PSP identity.

### Installed apps and games

- After a Tilefinch update that changes the compiled-script format, an
  installed app is marked RECOMPILE in **Library → Saved** and X offers to
  recompile it before it opens (or to open it anyway), instead of silently
  compiling every script on each launch.
- Installed apps may precompile up to 1 MB of script (was 512 KB), get a
  larger JavaScript heap (`app_heap_mb` in `boot.cfg`, 9 MB by default),
  keep their script source on the Memory Stick instead of in RAM, and show
  install progress (Circle stops).
- Full-screen WebGL games draw straight through the PSP's graphics chip,
  game audio is scheduled natively, the browser bars are hidden before a
  game's first full-screen frame, and the Page controls notice lasts 5
  seconds. Shader logic the PSP cannot run is reported instead of drawn
  wrongly.
- Treadline Arena gains a campaign (The Line), a Practice Range, daily runs
  and replays, adaptive music, Gunner controls with their own Controls menu,
  an optional aim guide (off by default), larger menu text and a cleaner
  HUD font, with many gameplay and AI fixes.

### Fixes

- JavaScript engine crashes fixed: a function compile that ran out of
  memory could corrupt memory (a crash on GitLab), a regular expression
  compile that ran out of memory could write past its buffer (a crash on
  Mastodon), a page that kept retrying a function the engine had no memory
  to compile could loop until the watchdog stopped it, and running out of
  memory while parsing was reported as a syntax error in the page.
- A paused video rewound to 0:00 resumes again, and the "Opening video"
  notice no longer stays up during playback.
- One stalled script request no longer holds up the page's other scripts,
  and webpack bundles that start with a byte order mark are now split and
  loaded lazily like any other.
- `location` keeps the current URL until a navigation actually commits.
- Pages with more than 8,192 elements no longer exhaust the DOM handle
  table.
- Hardening against hostile pages in the SVG renderer, CSS `calc()` and
  `var()` handling, table layout and the HTML parser.

### Fixes from the pre-release audit

- Pages that call `location.replace()`, or refresh themselves, through a
  built-in site adapter now replace the history entry, so Back skips the
  redirecting page. A `Refresh` header whose address is too long is ignored
  instead of being followed to a cut-off address.
- A site's "always run" or "this session" heavy-page choice applies from its
  first scripts and no longer carries over to the next site. The "Too heavy
  for the PSP" sheet no longer offers Basic view when Basic view fallback is
  Off.
- `querySelectorAll`, `children`, `childNodes`, `getElementsByTagName` and
  `document.all` no longer stop silently at 4,096 results on large pages.
- `fetch()` in `no-cors` mode drops headers it may not send and sends the
  request, as other browsers do, instead of failing (common on analytics
  scripts). `sendBeacon` reports `false` instead of silently dropping a
  beacon when the network queue is full.
- Large lazily loaded webpack modules are no longer refused with "out of
  memory" on first use when the page had room for them.
- "Clear data for this site" also drops that site's compiled scripts that
  were still waiting to be saved, so they are not written back afterwards.
- Inline SVG icons coloured through a CSS variable that holds a modern
  colour (`oklch()`, `color-mix()`, `light-dark()`) paint in that colour
  instead of grey. Long CSS variable values (transition lists, filter
  chains, grid templates and similar) now apply instead of being dropped,
  and `color-mix()` accepts every CSS Color 4 colour space.
- `TextDecoder` decodes every legacy single-byte encoding (windows-125x,
  KOI8, ISO-8859-x, IBM866, Mac and x-user-defined), not only windows-1252.
- `document.all` is much faster in old-style loops and keeps working when a
  page replaces DOM methods; pages without frames no longer walk the whole
  document before every script.
- Constructed stylesheets split their rules the same way `<link>`
  stylesheets do, `document.fonts.check()` uses the real font grammar, and a
  disabled form-associated custom element can no longer be focused.
- Treadline Arena: the boss can no longer clip into a wall or gate corner
  when it shoves or spawns, and menus read the gamepad once per step.
- Treadline Arena: after backing out of Enter code in Multiplayer, the
  Garage's Paint row and the Camera & Display and Audio settings no longer
  disappear, and Multiplayer keeps its heading after you cancel an online
  attempt. The title menus no longer show a gadget bar that never changed;
  the in-game HUD still shows the gadget and Command meters.
- The compiled-script cache kept by **Keep compiled scripts** is discarded
  once after updating, because the engine fingerprint now also covers the
  regular-expression and Unicode tables.
- Debug, test and experiment code (an alternative Web Audio
  implementation, test bridges in Treadline Arena, and leftover
  measurement switches) is no longer part of the shipped build.
- Low-memory style builds preserve ordinary and document-adopted styles
  without letting shadow-confined rules escape their roots. Constructed
  sheets beyond the tracked-source limit are declined safely, and dense
  cascades have an additional bounds check.
- A near-full stylesheet ending in ignored text stays editable, and an
  unreadable compiled-script pack is tried only once per navigation instead
  of repeatedly reading the Memory Stick.
- Refreshed the public suffix data used for cookie and site-boundary checks.

## 0.1.28 — 2026-10-01

- Faster loading and responses on JavaScript-heavy sites.
- More responsive dynamic styling, page updates, and images.
- Improved navigation reliability and memory handling.
- Refreshed security data and fixed a JavaScript engine error-path leak.

## 0.1.27 — 2026-09-23

- Made long pages usable sooner, with more responsive focus, scrolling, and menus while loading continues.
- Improved layout, text, and WebGL rendering on complex pages and games.
- Added per-site storage controls with optional Memory Stick persistence and safer recovery.
- Reduced browsing and playback overhead, with additional stability fixes.

## 0.1.26 — 2026-09-21

- Improved page layout and computed-style compatibility on complex sites.
- Improved PSP cursor responsiveness, UI rendering, and frame scheduling.
- Hardened JavaScript memory handling and runtime diagnostics.

## 0.1.25 — 2026-09-19

- Fixed dynamic styling and motion handling on large pages.
- Improved Worker and origin-private storage performance and memory use.

## 0.1.24 — 2026-09-19

- Improved compatibility for Workers, embedded frames, Web Crypto, storage,
  and other modern web APIs.
- Reduced JavaScript memory use and improved stability on script-heavy pages.
- Fixed additional frame updates, session-data clearing, and platform API
  behavior.

## 0.1.23 — 2026-09-13

- Improved Worker and browser event ordering on complex pages.
- Additional compatibility and stability fixes.

## 0.1.22 — 2026-09-13

- Improved compatibility and reliability for Workers, embedded frames, and
  browser events.
- Updated the PSP network stack with the latest security fixes.

## 0.1.21 — 2026-09-13

- Faster, more memory-efficient web response handling.
- Improved reliability for Fetch, Streams, text decoding, and IndexedDB.

## 0.1.20 — 2026-09-12

- Improved compatibility and reliability for Workers, Fetch, IndexedDB,
  embedded frames, and other dynamic Web APIs.
- Reduced routine Memory Stick writes and fixed additional lifecycle, memory,
  and stability issues.

## 0.1.19 — 2026-09-09

- Improved Wikipedia search forms and results layout.
- Playback-position saving is now off by default, with options to enable it or clear saved positions.

## 0.1.18 — 2026-09-09

- More responsive browsing and improved network reliability.
- Faster video descriptions and comments, with corrected scroll positioning.
- More reliable Wikipedia browsing.
- Weekly automatic update checks enabled by default.
- Additional memory and stability fixes.

## 0.1.17 — 2026-09-08

- Faster, more responsive page loading and navigation.
- Reduced memory use and improved reliability on complex pages.
- Fixed dynamic styling, embedded-frame, and viewport handling.
- Additional playback and stability fixes.

## 0.1.16 — 2026-09-06

- Faster startup and more responsive page loading, scrolling, and focus navigation.
- Improved video loading and playback.
- Improved compatibility and reliability for Workers, WebAssembly, embedded frames,
  and dynamic styles.
- Additional memory and stability fixes.

## 0.1.15 — 2026-09-02

- Added bounded WebAssembly and Web Worker support for more modern sites.
- Improved complex-page reliability with better fallback, Reader, and history
  recovery.
- Added native playback cards for pages that declare playable video.
- Improved JavaScript memory use and hardened browsing, media, and offline-game
  lifecycles.

## 0.1.14 — 2026-08-30

- Added Treadline Arena, an installable 3D tank game with Arcade and Classic
  controls, aim assist, Onslaught mode, and shareable generated arenas.
- Added direct multiplayer support for compatible games, with PSP LAN
  discovery and numeric invites plus service-free pairing in ordinary browsers.
- Improved WebGL and Canvas game speed, input response, frame pacing, audio,
  visual stability, and installed-game startup.
- Improved complex and long-page browsing by preserving useful page content
  when scripts fail and offering contextual recovery through Basic view.
- Made Reader mode more reliable and useful for text, links, images, captions,
  tables, and code, with better position preservation when switching views.

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
