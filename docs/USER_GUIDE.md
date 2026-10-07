# Using Tilefinch

This guide covers everyday use beyond the basics in the
[README](../README.md): typing, finding text, Reader mode, Basic view and the
offline library, language packs, games and page controls, screenshots, what
happens when a page will not load, faster revisits, and the per-site controls. The button map is in the README's
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
  watch pages. When a page finishes loading and is clearly one of them, it
  switches to Reader without asking and shows a short note: **Select →
  Page tools → Reader mode** returns to the full page (Circle is still
  Back). An article qualifies only with a page title and mostly prose; front
  pages full of headline links, product and store pages, and articles too
  long for Reader to show whole stay as they are. Auto Reader decides page by
  page: a link from an article it switched opens normally unless that page
  qualifies too, while Reader you turned on yourself stays on as you follow
  links.

[The Reader mode reference](READER_MODE.md) describes exactly what Reader mode
does and does not do.

## Reader mode vs Basic view

Both show a simplified copy of the page, without its scripts or site styling.
They solve different problems:

| | Reader mode | Basic view |
|---|---|---|
| Purpose | Nicer reading of a page that works | Rescuing a page that is broken |
| What it keeps | Just the main article, listing or media | Nearly all real content: links, navigation, tables and simple search forms |
| Turns on by itself | Only with **Auto Reader** (off by default), page by page, on clear article, listing or media pages (an article needs a title and mostly prose), even when nothing failed; a short note says so | When scripts failed and the page is blank, hidden or unusable, as set by **Basic view fallback** (Automatic, Ask or Off; Automatic by default) |
| Manual toggle | **Menu → Page tools → Reader mode** | **Menu → Page tools → Basic view** |
| Settings | **Settings → Appearance → Auto Reader**; **Page tools → Site information → Permissions & controls → Always Reader** for one site | **Settings → Browsing & input → Basic view fallback** |

Either view stops the page's scripts; reload the page to run them again. A
page has only one simplified view at a time: when a page fails, Basic view is
tried first, and switching to the other view needs a reload.
[Basic view](BASIC_VIEW.md) describes its bounds and fallback in detail.

**Page tools → Save article** creates a self-contained text snapshot. The
Library's **Saved** and **Downloads** sections open and delete saved
articles and pause, resume, play, or delete YouTube downloads. An active
download shows its progress, speed, the remaining Memory Stick space, and a
lasting failure reason; an interrupted download resumes from its last
verified byte instead of starting over. The library holds at most 12 items
and is not read at boot. After a browser update, an installed game may show
**RECOMPILE** in **Saved**: X offers to recompile it now (Circle stops) or
to open it anyway, which starts more slowly.
[The offline library reference](OFFLINE_LIBRARY.md) has the formats and
limits.

## Other languages and language packs

The built-in fonts cover Latin text, and a small built-in fallback draws
Chinese, Japanese and Korean characters and emoji. Other scripts need an
optional language pack (about 1 MB each, on the Memory Stick): Cyrillic,
Extended Latin (for example Vietnamese), Arabic and Hebrew, plus nicer
Japanese, Chinese and Korean glyphs. Without one, those words show as blank
cells, or as boxes in bold text.

When a page you are reading uses one of these scripts and its pack is not
installed, a notice appears above the bottom bar, for example
**This page uses Cyrillic text. Install the Cyrillic language pack?** It
does not stop you reading or scrolling, and it disappears after about 12
seconds. While it shows:

- **X Install...** opens a short confirmation. Tilefinch first fetches and
  verifies the pack's signed details (**Checking size...**), then asks, for
  example, **Install Cyrillic pack (1.0 MB)?** with the real download size.
  Press X again to download, verify and install it exactly as the menu
  does, with progress on the notice line; O cancels. One press never starts
  a download. When the install finishes, the page redraws with the new
  letters without reloading. If the PSP is offline or the check fails, the
  confirmation shows the reason; if the Memory Stick lacks space, the notice
  says so and nothing is installed.
- **Triangle Don't ask again** stops offers for that pack. The choice is
  saved with your settings.
- **O** is still Back. It also closes the notice.

The notice appears at most once per site each time Tilefinch runs, only on
pages with a meaningful amount of the script (not a stray word or a list of
language names), and only when the fonts really lack those letters. It also
appears in Reader mode and Basic view, which use the same fonts. A button
pressed in its first half second still goes to the page.

You can also manage packs yourself in **Settings → Appearance → Language &
emoji**:

- choose the **Language**, then select **Language pack** and press X
  (**X Download pack**). The menu downloads at once, without the size
  question. After any install the selected language and color-emoji packs
  are attached again straight away, with no restart;
- **Offer language packs** turns the notice off (**Off**) or back on
  (**Ask**, the default);
- **Reset declined offers** forgets every **Don't ask again** answer.

Besides the selected language, Tilefinch attaches up to two other installed
packs when a page needs them.

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

## Pages that move on by themselves

Some pages forward you elsewhere after a moment, or reload themselves every
few minutes, with a refresh instruction rather than JavaScript (a link
redirector's "Continue" page is a common one). Tilefinch follows these with
JavaScript on or off, and the page it leaves is not kept as a step back
(the same goes for a page that forwards you with JavaScript's
`location.replace`). Typing into a box on the page, or opening another page,
stops a pending refresh.

A page that only reloads itself does so while you leave it alone. Once you
have pressed any button on it, Tilefinch does not pull the page out from under
you: the status line shows **PAGE WANTS TO RELOAD  SQUARE RELOADS**, and
Square reloads it when you are ready. A page that sends you somewhere else is
still followed.

A page that keeps refreshing while you do nothing is stopped after five
refreshes and stays on screen, and the status line shows
**AUTO-REFRESH STOPPED**; press any button and the next page may refresh
again.

## When a page will not load

Tilefinch keeps the last usable page on screen and offers the recovery
choices that fit the failure: retry, Basic view, Reader mode, Wi-Fi sign-in,
turning off JavaScript for that site, or lower-bandwidth media settings.
**Show Basic view** appears when the failed page is still loaded;
**Reload in Basic view** appears when the page arrived but could not be built,
and loads it once more without its JavaScript. Neither appears when
**Basic view fallback** is Off. A page whose app is too heavy for the PSP
gets the same sheet titled "Too heavy for the PSP", without Retry (see
[Heavy pages](#heavy-pages)).

## Heavy pages

Some sites are large apps: before showing anything they load megabytes of
script, and the PSP needs about 10 seconds for each megabyte to start it
(sometimes much less, when the site splits its app into pieces it runs as
needed). Tilefinch weighs every page's scripts as they arrive, and when a
page's scripts would take 15 seconds or more to start, what you see
depends on what the site sent (laying out and drawing what they build takes
time on top):

- **The page already shows its text** (news fronts, most articles). Its
  scripts keep running in the background and you can read and scroll at
  once. A status says how much script the page runs and roughly how long
  it takes; while it shows, **X** stops the page's scripts (**O** keeps
  reading). It comes back once more if the page runs low on memory.
  Stopping scripts keeps the page as it is; if that leaves it blank or
  without links, **Basic view fallback** takes over as usual.
  If the scripts run out of memory and wipe the page as they fail,
  Tilefinch puts back the page the site sent, stops its scripts and says
  "Page scripts stopped: not enough memory".
- **The page is an empty shell for an app** (m.vk.ru logged out, Bluesky).
  Basic view cannot help there, so Tilefinch asks first, with the size and
  an estimate: "It needs about 5.1 MB of script. It may take about 51 s to
  start." **X** starts it for this session, **Triangle** always for this
  site, **O** (or Menu) does not start it and leaves the page as it is. The
  app's big scripts wait while you decide; nothing else on the page stops.
  When a shell's scripts all came with the page itself, so there is nothing
  to hold back, you see the status above instead, titled "Large app
  starting".
- **The app cannot fit, even in the best case.** Tilefinch does not start
  it. On an empty app shell it says how much memory the app would need
  against what is free (or that its script is over the 4 MB limit) and
  offers **Show Basic view** (unless **Basic view fallback** is Off),
  **Try Reader mode**, turning off JavaScript for the site, or going back. m.vk.ru's logged-out app is one: its 3 MB
  main script would need about 9.5 MB more than the 8.7 MB a PSP has free
  there; Bluesky's app script is over 4 MB. On a page that already shows
  its content, a status says a large page script was skipped and you keep
  reading.

**Settings → Browsing & input → Heavy pages** chooses **Ask** (the default,
as above), **Run** (never ask; scripts still show their status) or **Basic
view** (never start the big scripts of an app shell or of a page that cannot
fit; the page stays as the server sent it). The sites you chose to always
run are remembered on the Memory Stick with your settings; **Reset
permissions** in a site's **Site information** forgets the choice.

A single script may be up to 4 MB. Whether it runs is decided by the memory
its compile needs, measured on real sites: about twice its size on a typical
script, up to about ten times on some. A script that needs more than the
page has free is refused on its own, and the rest of the page's scripts keep
running. An install whose `boot.cfg` still carries the old default
`file_kb=512` is updated to the new ceiling once, the first time this
version starts; any other value you set is kept (see
[TROUBLESHOOTING.md](../TROUBLESHOOTING.md#a-site-misbehaves-because-of-its-scripts)).

## Faster revisits: Keep compiled scripts

Most of the time a script-heavy page takes to load on the PSP goes into
compiling its JavaScript. Tilefinch keeps compiled scripts in RAM for the
session, so going back to a page is faster until you exit. **Settings →
Device & storage → Site data & storage → Keep compiled scripts** also keeps
them on the Memory Stick, so pages you visited before restarting Tilefinch
start faster too. It is **Off** by default because it uses Memory Stick
space (at most 8 MB, shown on the row while it is on) and writes to the
stick while you read a page. **Clear compiled scripts** below it empties
it, and turning the option off removes its files. **Clear HTTP caches**
empties it too, and **Clear data for this site** removes that site's
compiled scripts. If a site misbehaves after it updates, clear the compiled
scripts or turn the option off ([Troubleshooting](../TROUBLESHOOTING.md#a-site-misbehaves-after-it-updates)).

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
