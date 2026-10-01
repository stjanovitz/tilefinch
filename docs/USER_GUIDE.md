# Using Tilefinch

This guide covers everyday use beyond the basics in the
[README](../README.md): typing, finding text, Reader mode and the offline
library, games and page controls, screenshots, what happens when a page will
not load, and the per-site controls. The button map is in the README's
[Controls](../README.md#controls) table.

## Typing

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

## Finding text

**Menu → Page tools → Find in page** takes a search term from the keyboard.
D-pad Up/Down or L/R then move between highlighted matches (hold to repeat),
X or Start edits the term, and Circle closes Find. Results stop at 256 so a
very repetitive page cannot use unbounded memory.

## Reader mode and the offline library

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

[The Reader mode reference](READER_MODE.md) describes exactly what Reader mode
does and does not do.

**Page tools → Save article** creates a self-contained text snapshot. The
Library's **Saved** and **Downloads** sections open and delete saved
articles and pause, resume, play, or delete YouTube downloads. An active
download shows its progress, speed, the remaining Memory Stick space, and a
lasting failure reason; an interrupted download resumes from its last
verified byte instead of starting over. The library holds at most 12 items
and is not read at boot. [The offline library reference](OFFLINE_LIBRARY.md)
has the formats and limits.

## Games and page controls

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
  like `RTCDataChannel`. [Direct multiplayer](MULTIPLAYER.md) covers LAN
  discovery, numeric invites, NAT traversal, page limits, and the networks
  that cannot connect without a relay.

Game authors should start with
[Tilefinch Game Profile v1](GAME_PROFILE.md), the complete API,
resource, lifecycle, packaging, and qualification contract, and then read the
[WebGL guide](WEBGL.md) for renderer details.

## Screenshots

Screenshots are written to `data/screenshots/` a little at a time, so saving
one does not freeze navigation. The completion message names the new file,
and **Library → Screenshots** lists the newest 32 with their sizes (without
scanning the folder at boot).

## When a page will not load

Tilefinch keeps the last usable page on screen and offers the recovery
choices that fit the failure: retry, Reader mode, Wi-Fi sign-in, turning off
JavaScript for that site, or lower-bandwidth media settings.

## Per-site controls

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
  [The content-blocking reference](CONTENT_BLOCKING.md) lists the built-in
  hosts and selectors, the accepted custom syntax, and the limits.
- **Cookie notices.** Common consent overlays are hidden by default without
  clicking Accept or writing consent cookies. **Permissions & controls →
  Cookie notices** shows them again for the current site, independently of
  ad blocking.
