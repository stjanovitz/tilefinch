# Tilefinch

*An experimental web browser for the Sony PSP (333 MHz, 64 MB): a
from-scratch layout engine, JavaScript, and hardware-assisted video, all
running on the handheld itself.*

| Wikipedia at PSP resolution | 360p video playback |
|---|---|
| ![The English Wikipedia PlayStation Portable article rendered by Tilefinch](docs/wikipedia-on-psp.png) | ![Tilefinch's video overlay—Big Buck Bunny title, pause control, scrubber, and timestamps—over a Big Buck Bunny frame](docs/big-buck-bunny-player.png) |

| Reader mode | Dark mode |
|---|---|
| ![The Wikipedia PlayStation Portable article reflowed in Tilefinch Reader mode](docs/wikipedia-reader-mode.png) | ![The Wikipedia PlayStation Portable article rendered with Tilefinch forced dark mode](docs/wikipedia-dark-mode.png) |
| Five-tab switcher | Danzeff text entry |
| ![Tilefinch's native tab switcher showing five open pages](docs/native-tabs.png) | ![Tilefinch's Danzeff radial keyboard with bookmark and history suggestions](docs/danzeff-text-entry.png) |

## Why

I picked up my PSP for the first time in about a decade and wanted an excuse
to find out three things: how far the machine could be pushed, how much of
the modern web could run in a setting this constrained, and what today's
coding agents are capable of. A web browser tests all three.

## What it does

| Feature | Support |
|---|---|
| **Web browsing** | Real HTTPS pages with JavaScript, bounded WebAssembly and WebSockets, cookies, images, mobile layout, and TrueType text. No proxy or companion computer is involved. |
| **Native media** | YouTube (through its lightweight provider) and compatible HTML `<video>`/`<audio>` elements open in one native player. Official builds use the PSP's firmware decoder for 240p/360p Baseline/Main MP4 and 240p HLS with AAC-LC, including YouTube live streams and premieres. Seeking, buffering status, preferred audio and subtitle languages, track selection, resumable downloads, and audio-only playback are built in. An optional, separately built decoder adds 240p H.264 High. |
| **Tabs and navigation** | Five tabs, bookmarks, history, address and search suggestions, find in page, optional session restore, and optional one-tab hibernation. |
| **Ad blocking** | Conservative request blocking and cosmetic hiding are on by default. Custom uBlock/EasyList-style rules and per-site exceptions are supported. |
| **Cookie notices** | Common consent banners are hidden without clicking Accept or creating consent cookies; individual sites can be exempted. |
| **Reader and offline** | Reflow and save articles. Small manifest-backed web apps can be installed for offline use with their icon and the same-origin resources already loaded, then updated, reinstalled, or removed. |
| **Text entry** | The PSP system keyboard or the faster Danzeff radial keyboard, with completion from local bookmarks and history. |
| **Games** | Bounded Canvas 2D and WebGL 1 for charts and modest games, with `ImageBitmap` asset preparation, user-started PCM game audio, page fullscreen, and the standard Gamepad API mapped to the PSP's controls. Two example games are included: [Prism Break 3D](examples/prism-break-3d/) and the two-player [Treadline Arena](examples/treadline-arena/), which pairs PSPs through LAN discovery or numeric invites and ordinary browsers through manual, service-free WebRTC. |
| **Appearance** | Automatic or forced page dark mode, dark and light browser chrome, downloadable color themes, page text scaling, bounded mixed right-to-left and left-to-right layout with Arabic-family shaping, and optional Japanese, Chinese, Korean, Cyrillic, Extended Latin, Arabic, Hebrew, and color-emoji glyph packs. |
| **Native PSP interface** | An immediate home screen, Collections, clock, battery and Wi-Fi status, contextual controls, PNG screenshots, and diagnostics shown as photographable QR codes. |
| **Optional XMB redirect** | With ARK-4, Sony's Internet Browser icon can launch Tilefinch; holding L opens the original browser instead. |
| **Updates** | Signed in-app updates with A/B slots, a trial boot, automatic rollback, explicit approval, and an optional picker for signed earlier versions. |
| **Experimental voice search** | An optional downloadable model and on-demand speech engine. Off by default; currently slow and inaccurate. |

### At a glance

| | |
|---|---|
| **Hardware target** | PSP-3000: 333 MHz MIPS, 64 MiB RAM (about 43 MiB of measured heap, with pages and voice sharing a 32 MiB envelope), 480×272 display using RGB565 surfaces for pages and chrome and 32-bit scanout for fullscreen video |
| **Execution model** | Networking, HTML, CSS, JavaScript, layout, and rendering all run on the PSP |
| **Browser engine** | Lexbor HTML parser, Tilefinch's own style, layout, and rendering, QuickJS runtime |
| **Presentation** | Incremental retained display list with a bounded RGB565 tile cache |
| **Networking** | HTTPS with certificate and hostname verification, cookies, redirects, caching, and an isolated Wi-Fi sign-in flow for captive portals |
| **License** | MIT, with separately licensed third-party components |

### Engineering highlights

| Area | What makes it interesting |
|---|---|
| **A browser, not a port** | The style, layout, paint, policy, media, and native interface layers are original C11. Lexbor, QuickJS, curl, Mbed TLS, nghttp2, and FreeType are vendored and hash-pinned ([notices](THIRD_PARTY_NOTICES.md)). |
| **One strict memory ledger** | DOM, JavaScript, CSS, resources, layout, and tiles share one 24 MiB page budget. Every table and loop is bounded, and a refused allocation is a tested, recoverable path rather than an exception. |
| **Useful pixels before the download ends** | Streaming HTML and CSS, preload discovery, resumable layout, priority for visible resources, lazily loaded bootstrap modules, and bounded caches of compiled styles and scripts all aim at first paint on a 333 MHz in-order core. |
| **Deterministic rendering** | Host and PSP share the same layout and rasterizer, down to ordered RGB565 dithering. Fidelity floors measured against Chrome references may only move upward ([fidelity workflow](docs/FIDELITY.md)). |
| **Firmware video acceleration** | A raw-NAL bridge and a bounded H.264 recovery-point rewriter feed the PSP's Media Engine, and two guarded decode surfaces overlap color conversion with presentation ([media state machine](docs/engineering/PSP_MEDIA_SESSION_STATE.md)). |
| **Optional two-core video** | A user-built add-on decodes 240p H.264 High by giving CABAC, deblocking, audio, and color conversion to the Media Engine while the CPU reconstructs rows. Official binaries deliberately leave it out. |
| **360p on a 480×272 screen** | 640×360 frames pass through EDRAM as two guarded strips, and the GE scales them bilinearly with no per-pixel CPU pass ([device envelope](docs/engineering/PSP_ENVELOPE.md)). |
| **Explicit lifecycle ownership** | Media and networking use pure reducers, epoch-tokened services, consumer leases, pumped teardown, and quarantine, so memory is never freed beneath live firmware or worker activity ([architecture](docs/ARCHITECTURE.md)). |
| **Security without pretending to sandbox** | HTTPS-first navigation, CORS, CSP, SRI, private-network protection, partition-aware resource authority, cookie controls, and signed A/B updates are enforced within a documented shared-process model ([security model](docs/SECURITY_MODEL.md)). |
| **Hardware-aware gates** | The optimized host suite is backed by sanitizer, hostile-input, WPT, fidelity, and PSP cross-build gates, plus ratchets on executable size and hot-function size ([engineering guide](AGENTS.md)). |

## What you need

- A PSP that runs homebrew (custom firmware) and has the 64 MB memory mode:
  - **PSP-3000**: the tested model. Everything claimed on this page ran on it.
  - **PSP-2000 and PSP Go**: same memory mode, expected to work, not yet
    qualified.
  - **PSP-1000**: not supported. It has 32 MB of RAM, and most sites need
    more.
  - **PSP-E1000 (Street)**: has the memory but no Wi-Fi, so only the offline
    library would work. Not a sensible target.
- About 20 MB free on the Memory Stick for everyday browsing and updates.
  - Optional glyph packs take about 1 MB each; the color-emoji pack takes
    about 5 MB. Tilefinch shows the signed download size before installing.
    Besides the selected language pack, it can attach up to two other
    installed packs when a page uses their scripts.
  - Installing the optional voice model needs about 19 MB more while the
    verified download and the new copy coexist. Allow 40 MB free if you want
    voice recognition.
- A Wi-Fi network the PSP can join. With the latest ARK-4 custom firmware,
  WPA2 works; otherwise the PSP joins only WPA (TKIP) or open networks, a
  limit of its hardware era. Most routers can offer a WPA/TKIP guest network.
  Save the connection in the PSP's own Network Settings first.

## Install

1. Download the PSP install `.zip` from the
   [latest Tilefinch release](https://github.com/stjanovitz/tilefinch/releases/latest).
2. Extract it and copy the whole `TILEFINCH` folder to `PSP/GAME/` on the
   Memory Stick. The launcher should end up at
   `PSP/GAME/TILEFINCH/EBOOT.PBP`, with `slot-a`, `slot-b`, `data`, and
   `NOTICES` beside it.
3. Safely disconnect the PSP, then start **Tilefinch** from
   **Game → Memory Stick**.
4. If you have not already, save a Wi-Fi connection in the PSP's Network
   Settings. Tilefinch uses connection profile 1 unless you choose another
   in **Settings → Device & storage**.

The `.tfum` and `.tfup` files on the release page are for Tilefinch's signed
in-app updater, not for manual installation. Optional language, emoji, and
voice components are installed from their Settings screens.

### Optional ARK-4 XMB redirect

ARK-4 users can make **Network → Internet Browser** open Tilefinch. This is
an optional VSH plugin: it does not modify the PSP firmware or `flash0`, polls
nothing in the background, and writes nothing to the Memory Stick.

1. Install Tilefinch normally at `PSP/GAME/TILEFINCH/`.
2. Copy `TILEFINCH/OPTIONAL/tilefinch_xmb.prx` from the install archive to
   `ms0:/SEPLUGINS/tilefinch_xmb.prx`.
3. In ARK's Custom Launcher or Plugin Manager, install or enable the plugin
   for the **VSH (XMB)** runlevel.
4. Restart VSH or reboot the PSP.

The equivalent manual line in ARK's `PLUGINS.TXT` is:

```text
vsh, ms0:/SEPLUGINS/tilefinch_xmb.prx, on
```

On a PSP Go using internal storage, use `ef0:` in both paths.

- Hold **L** while opening the Internet Browser icon to get Sony's browser.
  Because the redirect uses L for this, Tilefinch's previous-version safe
  start is available only when you launch Tilefinch from
  **Game → Memory Stick**.
- If Tilefinch is missing or the handoff fails, Sony's browser stays open.
- To restore the original behavior, disable the plugin in ARK's Plugin
  Manager and restart VSH.

Tilefinch still runs as an ordinary homebrew application, not as code inside
VSH, so leaving it reloads the XMB and may show the usual Sony or ARK startup
screen. After replacing the plugin file itself, power the PSP off completely
and cold-boot it: Restart VSH can keep the previous `NO_STOP` handler in
memory. The redirect starts Tilefinch's stable root launcher, so signed
updates, trial boots, and automatic rollback work as usual. ARK-4's
[plugin documentation](https://github.com/PSP-Archive/ARK-4/wiki/Plugins)
describes its Plugin Manager and `PLUGINS.TXT` formats.

### Optional H.264 High decoder

Official releases do not redistribute the custom H.264 and AAC decoder.
Compatible Baseline/Main MP4 and HLS, live streams included, keep using the
PSP's firmware. To add the 240p High-profile path, build the add-on from its
pinned upstream source by following
[Optional PSP software decoder](docs/DEVELOPMENT.md#optional-psp-software-decoder),
then copy its three files to `PSP/GAME/TILEFINCH/components/swdec/`.

That directory sits outside both app slots, so signed updates keep the
add-on. Update metadata records the decoder ABI: if a future update changes
it, the update screen says **Decoder rebuild needed**. When a page needs the
add-on and it is missing or incompatible, the player points to these build
instructions rather than reporting a generic video failure.

## Controls

| Button | Action |
|---|---|
| D-pad | Move focus between links and controls (hold to repeat) |
| Analog stick | Move the page cursor; hold it against the top or bottom edge to scroll (Settings can switch to direct scrolling) |
| X | Activate the focused item, or click under the cursor |
| Circle | Back, or cancel the current load |
| Square | Reload (starts voice input on a text field when Experimental Voice is on) |
| Triangle | Show or hide the browser chrome |
| Start | Address and search bar |
| Select | Menu: Home, Tabs, Page tools, Library, Settings, Help & diagnostics, Exit |
| Start + Select (hold together) | Give the controls to the page's JavaScript; hold again to take them back |
| L / R | Page up / page down |
| L held at boot | Safe start: boot the previous version (once an update has been installed) |

Type a URL in the address bar to go there, or anything else to search.

## Using Tilefinch

### Typing

Pressing Start or activating a text field opens the selected keyboard. The
PSP system keyboard is the default; **Settings → Browsing & input →
Keyboard** switches to the faster Danzeff layout. In Danzeff:

- move the analog stick to one of nine character groups, then press
  Triangle, Square, X, or Circle for the character in that direction;
- hold R for uppercase and symbols, and press L to switch between letters
  and numbers;
- Start accepts, Select cancels, and the D-pad moves the text cursor or picks
  a matching bookmark or history entry.

Address completion stays on the PSP. URL history contributes only when you
have turned history on.

### Finding text

**Menu → Page tools → Find in page** takes a search term from the keyboard.
D-pad Up/Down or L/R then move between highlighted matches (hold to repeat),
X or Start edits the term, and Circle closes Find. Results stop at 256 so a
very repetitive page cannot use unbounded memory.

### Reader mode and the offline library

**Menu → Page tools → Reader mode** hides an article's surrounding navigation
and sidebars, uses the full screen width, and increases line spacing.
Turning it off restores the original page without fetching it again. Reader
mode retires the page's scripts once the view is shown, so reload to run its
JavaScript again.

- **Settings → Appearance → Reader font** chooses Sans or Serif, and the
  ordinary page text-size control resizes Reader text.
- **Remember size** keeps that size for up to 16 sites. It is off by default,
  so reading and resizing cause no extra Memory Stick writes.
- **Auto Reader**, also off by default, uses a bounded content-shape
  classifier (no hostname rules) to recognize articles, media listings, and
  watch pages.

[docs/READER_MODE.md](docs/READER_MODE.md) describes exactly what Reader mode
does and does not do.

**Page tools → Save article** creates a self-contained text snapshot. The
Library's **Saved** and **Downloads** sections open and delete saved
articles and pause, resume, play, or delete YouTube downloads. An active
download shows its progress, speed, the remaining Memory Stick space, and a
lasting failure reason; an interrupted download resumes from its last
verified byte instead of starting over. The library holds at most 12 items
and is not read at boot. [docs/OFFLINE_LIBRARY.md](docs/OFFLINE_LIBRARY.md)
has the formats and limits.

### Games and page controls

For a Canvas game, hold **Start + Select** together until **Page controls
on** appears. The D-pad, analog stick, face buttons, shoulder buttons, Start,
and Select then reach the page through the standard browser Gamepad mapping
instead of moving Tilefinch's focus or chrome. Hold the same chord to leave.
The PSP HOME button always remains a way out, and page control ends by itself
when the document navigates, native media opens, or the PSP suspends.
**Settings → Browsing & input → Game buttons** chooses X or Circle as the
primary face button.

- A page can enter fullscreen only after a button press. Triangle or the
  Start + Select chord brings Tilefinch's chrome back.
- Game audio is a bounded Web Audio subset for decoded PCM WAV effects and
  simple generated waveforms, with gain, stereo panning, loop points, and
  short scheduling: one context, eight decoded buffers, and four voices at a
  time. It does not decode compressed audio or run an arbitrary audio graph.
- Installed games can use Tilefinch's bounded direct-multiplayer API, shaped
  like `RTCDataChannel`. [Direct multiplayer](docs/MULTIPLAYER.md) covers LAN
  discovery, numeric invites, NAT traversal, page limits, and the networks
  that cannot connect without a relay.

Game authors should start with
[Tilefinch Game Profile v1](docs/GAME_PROFILE.md), the complete API,
resource, lifecycle, packaging, and qualification contract, and then read the
[WebGL guide](docs/WEBGL.md) for renderer details.

### Screenshots

Screenshots are written to `data/screenshots/` a little at a time, so saving
one does not freeze navigation. The completion message names the new file,
and **Library → Screenshots** lists the newest 32 with their sizes (without
scanning the folder at boot).

### When a page will not load

Tilefinch keeps the last usable page on screen and offers the recovery
choices that fit the failure: retry, Reader mode, Wi-Fi sign-in, turning off
JavaScript for that site, or lower-bandwidth media settings.

### Per-site controls

**Page tools → Site information** shows the current connection, its
certificate issuer, the site's bounded cookie and storage use, and whether
that storage is in RAM or on the Memory Stick. From there you can open the
site's **Site storage**, clear the site's data, or reset all of its
exceptions, and **Permissions & controls** holds the switches below. A switch
that cannot change on the current page says why when you press X.

- **JavaScript.** Wikipedia opens with page JavaScript off by default because
  browsing is more responsive that way; search, article links, and scrolling
  work without it. Turning it on under **Permissions & controls →
  JavaScript** applies to Wikipedia's language subdomains too, and **Reset
  permissions** restores the default. Other sites are unaffected.
- **Ad blocking.** Basic blocking and conservative cosmetic hiding are on by
  default. **Settings → Privacy & security → Content blocker** chooses Off,
  Basic, or Custom, and **Hide page ads** controls only the cosmetic hiding.
  **Permissions & controls → Content blocking** turns both off for the page's
  registrable site. Basic reads no list from the Memory Stick; Custom reads
  `data/adblock.txt`, and **Load allowlist** imports extra site exceptions
  from `data/adblock-allow.txt` into a set of at most 32 sites.
  [docs/CONTENT_BLOCKING.md](docs/CONTENT_BLOCKING.md) lists the built-in
  hosts and selectors, the accepted custom syntax, and the limits.
- **Cookie notices.** Common consent overlays are hidden by default without
  clicking Accept or writing consent cookies. **Permissions & controls →
  Cookie notices** shows them again for the current site, independently of
  ad blocking.

## Updating

Check for updates from **Settings → Updates**. An optional background check
looks for new release information at most once a week and can be turned off;
Tilefinch never downloads or installs an update unless you ask.

- Stable and Beta releases are cryptographically signed and verified before
  installation. The Developer channel, which you must select explicitly, is
  unsigned and trusts the endpoint you configure.
- A new version installs into a second slot and boots as a trial. If it
  fails to start properly, the launcher returns to the version that worked,
  and holding L at boot always starts the previous version.
- On Stable, press Square on the update page to choose an older signed
  release. It installs into the inactive slot and must pass the same trial
  boot.

[docs/SECURE_UPDATES.md](docs/SECURE_UPDATES.md) and [SECURITY.md](SECURITY.md)
have the details.

## Privacy

- No telemetry, analytics, or accounts. Tilefinch sends nothing about you or
  your browsing anywhere. Requests to YouTube carry the configured video
  language (the PSP's language by default) so results and audio tracks can be
  localized.
- Everything the browser keeps stays on the PSP. Cookies live only in memory
  and are gone at power-off; history (off by default), the cache, saved pages
  and videos, screenshots, and settings are on your Memory Stick.
  **Settings → Device & storage → Site data & storage** lists the sites
  holding data and clears one of them, or HTTP caches, cookies, local
  storage, and session storage for every site.
- Site storage (`localStorage`, `sessionStorage`, and the origin-private file
  system) lives in RAM and is not written to the Memory Stick unless you
  choose. When a site outgrows RAM, Tilefinch asks before keeping that site's
  data on the Memory Stick, for this session or always, and shows how much
  it needs. **Permissions & controls → Site storage** sets the choice per
  site, and Site data & storage sets it for any listed site.
- To reconnect faster, Tilefinch normally keeps the short-lived TLS
  resumption tickets that servers issue, in `data/tls-sessions.bin`. They act
  as credentials, so they stay on your Memory Stick, are sent only back to
  the site that issued them, and are erased by **Site data & storage → Clear
  HTTP caches**. **Settings → Privacy & security → TLS ticket saving** stops
  keeping them between boots without disabling connection reuse within a
  session.
- The PSP contacts only the sites you visit, plus the GitHub releases API at
  most once a week if the update check is on. That check compares version
  numbers, sends nothing identifying beyond an ordinary HTTPS request, and
  can be turned off.
- **Help & diagnostics → Check Wi-Fi sign-in** makes one cookie-free HTTP
  request to `connectivitycheck.gstatic.com`, only when you ask for it or
  accept a sign-in suggestion. A detected sign-in page opens with temporary
  cookies and storage that are deleted when it closes.
- On the start page, resting on the built-in YouTube tile for about a third
  of a second opens a connection to YouTube in the background, so the site
  is ready when you press X. This happens only for that built-in tile, never
  for a bookmark or an address a page supplied. It sends one bodyless request
  with no cookies, credentials, or referrer and fetches no content until you
  open the site. At most one such connection exists, and it closes as soon as
  you move away or the PSP suspends.
- Voice search runs entirely on the PSP; audio never leaves it.

## Security

[The security model](docs/SECURITY_MODEL.md) documents Tilefinch's security
defaults, compatibility controls, threat model, and known isolation limits.

## Known limits

- Many sites render imperfectly and some heavy sites do not load at all;
  Tilefinch does not yet implement every web standard or API. A page that
  exceeds the memory budget degrades or stops loading instead of crashing.
- YouTube support depends on YouTube not changing; it breaks occasionally
  and updates fix it. Tilefinch does not generate YouTube Proof-of-Origin
  (PO) tokens, so videos that require one may stay unavailable even when
  browsing and search work.
- Voice search is experimental: slow, often wrong, and English only.
- Tabs keep a bounded record of each page's history, URL, focus, and scroll
  position rather than five complete pages, so switching tabs reloads the
  page through the shared cache. The switcher keeps one 60×34 preview per tab
  (about 20 KiB in total), taken from a frame that was already rendered.
  Optional **Hibernate tab**, off by default, moves one inactive tab's
  snapshot (not its page memory) to the Memory Stick and marks it `[Z]`.
- Sites behind Cloudflare managed challenges do not work.
- WebGL is a bounded subset of the PSP's GE for conventional vertex-color and
  textured shaders, not a complete programmable GPU. Complex shaders,
  transparent drawing buffers, offscreen framebuffers, and large scenes fail
  softly instead of consuming unbounded memory or frame time.
- There is no desktop-grade process sandbox and no complete CSP. Tilefinch
  enforces a bounded subset of response-header CSP, resource, and framing
  policy, Subresource Integrity for scripts and stylesheets, and restrictive
  iframe sandbox and origin boundaries (see
  [the security model](docs/SECURITY_MODEL.md)), but it does not isolate
  origins in separate processes. Do not use it for sensitive accounts or
  transactions.

### Reporting problems

If something misbehaves, start with [TROUBLESHOOTING.md](TROUBLESHOOTING.md).
A useful bug report includes:

- the PSP model and custom firmware (for example "PSP-3000, ARK-4");
- the Tilefinch version from **Help & diagnostics → Version & system**;
- the site or URL, and what you pressed;
- after a crash, `PSP/GAME/TILEFINCH/data/tilefinch-crash.txt` from the
  Memory Stick (a 512-byte file of zeros means no crash was recorded).

Without easy Memory Stick access, **Help & diagnostics → Diagnostic QR** shows
the same bounded logs as QR pages you can photograph. For suspected security
problems, [SECURITY.md](SECURITY.md) explains what the project protects and
how to report.

## Building from source

The desktop lab builds and tests the same engine on your computer, at PSP
screen sizes and memory profiles. The first configure downloads hash-pinned
dependencies; host builds need libcurl development headers and zlib.

```sh
cmake --preset release
cmake --build build-preset-release
ctest --test-dir build-preset-release
```

[docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) has the full workflow; read
[CONTRIBUTING.md](CONTRIBUTING.md) before sending changes.

## How it works

One engine runs everywhere. Parsing, styling, layout, scripting, and
tile-based rendering are bounded and charged to a memory budget, and are
designed so that a refused allocation, a malformed page, or a cancellation is
a normal event rather than a crash.
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) has the system diagrams, memory
contracts, and lifecycle boundaries, and [docs/README.md](docs/README.md)
maps all of the documentation, including the engineering manuals in
[docs/engineering/](docs/engineering/README.md).

**Evaluating the engineering?** Read in this order:

1. [ARCHITECTURE](docs/ARCHITECTURE.md): the system and its ownership rules.
2. [SECURITY_MODEL](docs/SECURITY_MODEL.md): what fails closed, and which
   guarantees are absent.
3. The two device lifecycle contracts, for
   [media](docs/engineering/PSP_MEDIA_SESSION_STATE.md) and
   [networking](docs/engineering/PSP_NETWORK_SUPERVISOR.md).
4. The [device qualification strategy](docs/engineering/DEVICE_QUALIFICATION.md):
   which claims the host, the emulator, and the hardware can each prove.
5. [RELEASE_PROCESS](docs/RELEASE_PROCESS.md): gates, ratchets, and the
   offline signing ceremony.

## Contributing

Issues and pull requests are welcome. Tilefinch is a spare-time project, so
responses and reviews may take a while. [CONTRIBUTING.md](CONTRIBUTING.md)
describes the build and test expectations.

## License and independence

Tilefinch is MIT licensed; see [LICENSE](LICENSE). Third-party components
keep their own licenses and notices, listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). The security policy and
vulnerability reporting are in [SECURITY.md](SECURITY.md).

The browser download contains no acoustic model or speech dictionary. If you
request the optional voice component in the app, its signed package carries
the Alpha Cephei and CMUdict license files alongside the model; the browser
bundle itself carries the PocketSphinx license that its linked decoder
requires.

The optional Japanese, Chinese, Korean, Cyrillic, Extended Latin, Arabic,
Hebrew, and color-emoji packs are likewise not part of the download. They are
generated from Noto fonts, carry the complete SIL Open Font License notice
and a digest of the source font, and are fetched only when you ask for one.
Tilefinch does not read the PSP's proprietary firmware fonts.

Tilefinch is an independent, unofficial homebrew project. It is not
affiliated with, sponsored by, or endorsed by Sony Interactive Entertainment.
PlayStation, PSP, and related marks belong to their respective owners. The
project contains no proprietary Sony SDK material, firmware, artwork, or
branding.

The Wikipedia screenshots show the English article
["PlayStation Portable"](https://en.wikipedia.org/wiki/PlayStation_Portable).
Wikipedia text is licensed
[CC BY-SA 4.0](https://creativecommons.org/licenses/by-sa/4.0/). Wikipedia
and its marks are trademarks of the Wikimedia Foundation, shown only to
identify browser compatibility; Tilefinch is not affiliated with the Wikimedia
Foundation.

The video screenshot shows a frame from *Big Buck Bunny*, copyright 2008
Blender Foundation, licensed
[CC BY 3.0](https://creativecommons.org/licenses/by/3.0/). The film and its
credits are available from the
[Blender open-movie project](https://studio.blender.org/projects/big-buck-bunny/).
