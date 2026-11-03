# Using Tilefinch

This guide is for PSP owners. It covers everyday use beyond the basics in the
[README](../README.md): tabs, bookmarks and history, typing, finding text,
Reader mode and Basic view, saving and downloads, video and YouTube, language
packs, games and page controls, screenshots, what happens when a page will
not load, faster revisits, and the per-site controls. The button map is in
the README's [Controls](../README.md#controls) table.

Menu paths start from the menu that Select opens: **Menu → Page tools → Find
in page** means press Select, choose **Page tools**, then **Find in page**.

## Tabs, bookmarks and history

- **Tabs.** **Menu → Tabs** lists your open tabs, up to five. Choose
  **+ New tab** to open another, press Cross to switch to the selected tab,
  and Square to close it. Switching tabs reloads that tab's page, from the
  cache where possible. **Menu → Settings → Browsing & input → Restore page**
  reopens your tabs after you exit normally, and **Hibernate tab** moves one
  inactive tab's record to the Memory Stick to free RAM; that tab is marked
  `[Z]`.
- **Bookmarks.** **Menu → Page tools → Add/remove bookmark** bookmarks the
  current page, or removes its bookmark, and the status line confirms it.
  **Menu → Library** lists your bookmarks under **Bookmarks**.
- **History.** URL history is off by default. Turn it on with **Menu →
  Settings → Browsing & input → URL history**; Tilefinch then keeps your 100
  most recent addresses, lists them under **Menu → Library → History**, and
  offers them as address-bar suggestions.

The Library (its screen is headed **COLLECTIONS**) has five sections:
**Saved**, **Bookmarks**, **History**, **Downloads** and **Shots**
(screenshots). L/R switches between them.

## Typing

Pressing Start or activating a text field opens the selected keyboard. The
PSP system keyboard is the default; **Menu → Settings → Browsing & input →
Keyboard** switches to the faster Danzeff layout. In Danzeff:

- move the analog stick to one of nine character groups, then press
  Triangle, Square, Cross, or Circle for the character in that direction;
- hold R for uppercase and symbols, and press L to switch between letters
  and numbers;
- Start accepts, Select cancels, and the D-pad moves the text cursor or picks
  a matching bookmark or history entry.

Address completion stays on the PSP. URL history contributes only when you
have turned history on.

## Finding text

**Menu → Page tools → Find in page** takes a search term from the keyboard.
D-pad Up/Down then move between highlighted matches (hold to repeat), L/R
scroll the page, Cross or Start edits the term, and Circle closes Find.
Find highlights at most 256 matches.

## Website fonts

**Menu → Settings → Appearance → Website fonts** has three choices:

- **On:** load fonts used by the page, within the normal download and memory
  limits.
- **Smart** (default): use page fonts while memory allows, then keep using
  built-in fonts for the rest of that page visit if memory gets low.
- **Off:** use built-in fonts without downloading website fonts.

The setting is saved with your other preferences. Changing it affects the
current page without downloading the page again. If memory runs short while
the fonts change, the current page stays readable. Language and emoji packs
are independent.

## Reader mode

**Menu → Page tools → Reader mode** hides an article's surrounding navigation
and sidebars, uses the full screen width, and increases line spacing.
Turning it off restores the original page without fetching it again. Reader
mode stops the page's scripts once the view is shown, so reload to run its
JavaScript again.

- **Menu → Settings → Appearance → Reader font** chooses Sans or Serif, and
  the ordinary page text-size control resizes Reader text.
- **Remember size** keeps that size for up to 16 sites. It is off by default,
  so reading and resizing cause no extra Memory Stick writes.
- **Auto Reader**, also off by default, recognizes articles, media listings,
  and watch pages by the shape of their content, not by site. When a page
  finishes loading and is clearly one of them, it switches to Reader without
  asking and shows a short note: **Menu → Page tools → Reader mode** returns
  to the full page (Circle is still Back). An article qualifies only with a
  page title and mostly prose; front pages full of headline links, product
  and store pages, and articles too long for Reader to show whole stay as
  they are. Auto Reader decides page by page: a link from an article it
  switched opens normally unless that page qualifies too, while Reader you
  turned on yourself stays on as you follow links. Back and Forward check
  the page again.

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
| Settings | **Menu → Settings → Appearance → Auto Reader**; **Menu → Page tools → Site information → Permissions & controls → Always Reader** for one site | **Menu → Settings → Browsing & input → Basic view fallback** |

Either view stops the page's scripts; reload the page to run them again. A
page has only one simplified view at a time: when a page fails, Basic view is
tried first, and switching to the other view needs a reload.
[Basic view](BASIC_VIEW.md) describes how it works and when it turns on.

## Saving and downloads

Everything you save is listed in the Library, which holds at most 12 saved
articles, apps, videos and files in all.

- **Articles.** **Menu → Page tools → Save article** saves a self-contained
  text copy of the page that opens without a network. **Menu → Library →
  Saved** opens and deletes saved articles.
- **Offline apps.** On a page that is a small installable web app, **Menu →
  Page tools → Install offline app** saves it, with its icon and the files it
  has already loaded, so it opens from **Menu → Library → Saved** without a
  network. After a browser update, an installed app may show **RECOMPILE** in
  **Saved**: Cross offers to recompile it now (Circle stops) or to open it
  anyway, which starts more slowly.
- **Files.** **Menu → Page tools → Download focused link** saves an HTTPS
  file to the Memory Stick after you confirm. Common archive links,
  attachment responses and homebrew binary links offer this directly.
  Downloads never install or run automatically.
- **YouTube videos.** **Save video offline** on a video's page downloads it
  at your **Menu → Settings → Video → YouTube** quality.

**Menu → Library → Downloads** manages files and videos. An active download
shows its progress, speed, the remaining Memory Stick space, and a lasting
failure reason; an interrupted download resumes from where it stopped instead
of starting over. Square, then Square again, deletes an item.

A completed homebrew ZIP offers **Details → Install homebrew…**, with a
separate confirmation that shows where it will go. It installs one checked
app folder under `PSP/GAME`, refuses folders that already exist and keeps the
ZIP. Circle cancels and removes the current attempt's temporary files.
**Downloads → Installed homebrew** removes a Tilefinch-installed app even
after its ZIP is gone, with a separate confirmation warning that saved data
inside the app folder is removed too. Other apps are not touched.

While the Library holds a downloaded file, older Tilefinch versions cannot
read it. If you return to an earlier version with safe start or the update
picker, that version does not show your saved items.

[The offline library reference](OFFLINE_LIBRARY.md) has the formats and
limits.

## Native video player

YouTube results and compatible page videos open in the same native player.
Some posterless video cards show the linked article's preview image instead
of a blank box. This does not mean the video format is supported; unsupported
videos still show the player's normal explanation when opened.

- Cross pauses or resumes. D-pad Left/Right previews a seek 10 seconds back
  or forward, L/R a minute, and the analog stick scrubs; Cross confirms the
  new position. Circle cancels a preview or closes playback. In a YouTube
  playlist, L/R change video instead; see
  [Playlists and the queue](#playlists-and-the-queue).
- Square turns captions on or off while a video with captions plays. It
  brings back the track you last used, or the first track, which follows
  your preferred subtitle language.
- Triangle opens the chapter, audio and subtitle picker when available, with
  Chapters first, followed by Audio and Subtitles. Use L/R to change
  category, Up/Down to choose an entry, and Cross to select it; Circle
  dismisses the picker.
- Videos with chapters show chapter ticks on the timeline and the current
  chapter's name above it. Scrubbing previews the chapter name; choosing a
  chapter seeks directly to its start.
- Streamed (HLS) videos that are not live show their length and seek bar and
  seek like any other video. Live streams show **LIVE** instead and cannot be
  seeked.
- A video still opening after about ten seconds says **Still loading**;
  Circle cancels it. If the player falls back to 240p, it says **Retrying at
  240p**. A video that cannot play says why and what to try, with a short
  code underneath to quote in a bug report; see
  [A video won't play](../TROUBLESHOOTING.md#a-video-wont-play).

**Select rotates the picture and player controls** for holding the PSP
vertically. For landscape 240p video it cycles through portrait fit, a central
9:16 crop, and landscape. The crop is useful when vertical footage has black
sidebars baked into a horizontal video; it is never applied automatically.
For 360p landscape video, Select toggles directly between that crop and
landscape, and portrait video toggles between portrait fit and landscape.
Captions, track menus, D-pad directions and stick seeking follow the rotated
view, and rotation always uses Smooth scaling.

A Rotate hint appears for portrait video, for vertical footage with black
sides (checked during the first five seconds of playback), and in an already
rotated view. **Menu → Settings → Video → Rotate hint** offers **Auto** (the
default) or **Always** for videos you prefer to rotate yourself. The setting
only shows the hint; it never rotates automatically.

When a page declares a video that its own player cannot play on the PSP,
Tilefinch adds a Play card at the top of the page, and the first D-pad press
goes to it. Cross on the card plays the page's declared video, or opens the
page's player page and plays the stream it requests; Back then returns to
the page. If nothing playable turns up, Tilefinch says so and goes back.

YouTube Shorts links open in the same player when a compatible stream is
available.

## YouTube

YouTube opens in Tilefinch's own lightweight YouTube pages, not the full
YouTube site, including when another service redirects there. Videos play
in the native player above.

### Home, search and sign-in

The YouTube homepage shows its search box first, without waiting for a feed.
Signed out, the homepage is just the header and the search box.
**Menu → Settings → Video → YouTube home topics** (On by default) then loads
up to eight Explore categories from YouTube; turn it Off to skip that
request. If topics fail to load, the search box still works. Topic names and
video details follow the PSP's system language where YouTube supplies them,
while Tilefinch's own controls use your interface language.

Search results are listed as compact rows by default. **Menu → Settings →
Video → YouTube results** switches between **Compact** and **Detailed**
(larger cards with a description line); the change applies when the page
is reloaded and is kept from then on.

Search results offer a collapsible **Filters** control, one short line under
the heading, for upload date, duration and relevance/popularity ordering. Select a row to cycle its
choices, then **Apply filters**. The collapsed heading summarizes the active
choices; **Filters: Off** clears them while keeping the search text, and
**Load more results** keeps them.

Search results, channel pages and feeds mark a live stream with a red
**LIVE** label in place of its length, with the number watching, and a
scheduled stream or premiere with YouTube's **Upcoming** label and its start
time. Selecting an upcoming stream opens its page rather than playing it.

**Sign in** in the homepage header opens Google's sign-in page. Once signed
in, recommendations replace the generic topics, and **Subscriptions** opens
your subscription feed. A status line shows when suggestions are reloading.
If you have moved down into the page, new results wait behind **Refresh
suggestions** rather than changing beneath your selection; otherwise they
appear automatically. When your most recent YouTube history entry is not
already suggested, a **Recently watched** card shows it, with a resume link
when YouTube supplies the position.

If signing in ends on a Google page instead of returning to YouTube, choose
**Menu → Page tools → Continue to YouTube**. It finishes the sign-in without
sending your password again, and YouTube's homepage then checks the login.
If sign-in still seems to do nothing, see
[YouTube sign-in does nothing](../TROUBLESHOOTING.md#youtube-sign-in-does-nothing).

In the signed-in header, **Account** opens a small panel for feed navigation,
switching accounts, **Watch Later**, **History**, **Playlists** and **Sign
out**; select **Account** again to close it. History lists recently watched videos with
their resume positions, a page at a time with **Load more**.

**Sign out** first asks whether to sign out of YouTube and your Google
account on this PSP, with **Cancel** selected; **Cancel** returns to the
YouTube homepage and Circle to the page you came from. Choosing **Sign out**
removes this browser's YouTube cookies and personalized pages, and any saved
YouTube login on the Memory Stick, then returns to the search homepage and
says **SIGNED OUT**. It also ends the Google account sign-in
that YouTube's sign-in left in this browser, so the next **Sign in** asks
for the account again. It does not delete other site data or sign the
account out on other devices.

### Videos and channels

A video's page plays it from its thumbnail. Below it, the action row starts
with the channel name, which opens the channel, and always includes **Save
video offline**. Signed in, it also offers the subscription bell, Like,
Watch Later and **Save to playlist**, and its **Account** menu holds
**Unsubscribe**, Unlike and **Remove from Watch Later**; signed out, it
offers **Sign in**. The bottom hint bar names the focused icon's action;
when the bars have hidden themselves, moving to another icon brings the hint
bar back until the controls go idle again.

In the player, Cross pauses, D-pad Left/Right seeks 10 seconds, L/R a
minute, Square turns captions on or off, Triangle opens chapters, audio and
subtitles, and Circle closes the video. The player's hints list the same
buttons; see [Native video player](#native-video-player).

When YouTube declares chapters, a collapsible **Chapters** list starts the
video at the selected timestamp. **Up next** lists up to 12 suggestions,
without ads, Shorts shelves or mixes; a suggested playlist appears as a
playlist card.

A channel page shows the channel's banner, picture, name, subscriber count
and a line of its description. **Menu → Settings → Video → Channel banners**
(**Show** by default) set to **Hide** leaves the banner out: it is not
downloaded, which saves several hundred KiB of memory and brings the tabs
onto the first screen. The change applies from the next channel page you
open. Selecting the description line opens the full
description with the channel's links, view count and join date; **Back to
channel** returns. **Home**, **Videos**, **Shorts** and **Playlists** tabs sit
under the header. Home shows the shelves the channel arranges, each under its
own heading with **See all** where the shelf has one, named after the shelf; Videos lists the latest
uploads, with **More videos** for the next page.

Signed in, the bell beside the channel name subscribes. On a channel page a
lit bell means you are subscribed, and selecting it unsubscribes; when
YouTube does not say whether you are subscribed, the bell subscribes and
**Unsubscribe** is in the **Account** menu beside it. Signed out, a channel
page has no subscribe control.

Subscribing, Watch Later and playlist edits show success only after YouTube
confirms them; a failed confirmation offers a manual retry. Navigating away
cancels pending edits.

### Playlists and the queue

Playlist pages load their entries a page at a time with **Load more**;
playlists you can edit also offer **Remove**. Choose **Play all** to start
at the beginning, or select a video to play from that entry, keeping the
playlist order (including repeated videos).

**While a playlist plays, L/R select the previous and next video** instead
of seeking; the stick and Left/Right (10 seconds a press) still seek within
a video, and Circle closes playback. Triangle
opens **Queue**, which shows the playing video and its playlist position
above the next five entries, with the next video selected. Square turns
**Automatic next** on or off (it starts Off). L/R switch to the other player
menus, and Circle returns to playback. After the final video, Queue offers
replaying it, returning to the previous video, or going back to the
playlist.

A video that fails offers **Retry**, R to **Skip**, or Circle to **Cancel**
when those actions are available; Tilefinch never skips a failed video by
itself. Circle returns to the playlist page as you left it, with its scroll,
focus and entered text. No queue or playlist position is saved to the Memory
Stick.

### Saving your login

After a new login is confirmed, a prompt asks whether to save it and whether
to ask next time. **Not now; ask next time** is selected by default.
**Save; ask next time** saves this login only; **Always save; don't ask**
also remembers that preference. **Never save; don't ask** keeps logins in
RAM and removes an existing saved login. Change the choice under **Menu →
Settings → Video → Save YouTube login**. Saving requires **Cookies &
storage** to be enabled.

A saved login is YouTube's session cookies, not your password, stored
unencrypted in `data/youtube-login.bin`; anyone with the Memory Stick may be
able to use it. Save only on a Memory Stick you trust. If saving fails, the
current session keeps working. When YouTube no longer accepts a saved login,
the browser removes it and says **SIGNED OUT BY YOUTUBE**.

## Other languages and language packs

For the browser interface, open **Menu → Settings → Appearance → Language &
emoji → Interface language**. Choose with Left/Right and press Cross to
install; press Cross again while it installs to cancel. English, Spanish,
French, German, Japanese, Russian, Ukrainian, Simplified Chinese, Korean,
Hindi, Arabic, Portuguese, Bengali and Assamese are available. Languages
that need extra glyphs also download their font pack. Restart to apply. This
is separate from page fonts and preferred audio/subtitle languages; it does
not translate websites or games. Advanced messages not yet translated stay
in English. Translations and signed glyph packs are downloaded from
[tilefinch-models](https://github.com/stjanovitz/tilefinch-models).

If the selected language's file or font pack is missing or damaged, the menus
fall back to English. Once per start, an English notice names the language
and what it needs; Cross opens Interface language on that language, where one
more Cross downloads it, and Circle dismisses the notice until the next
start.

The built-in fonts cover Latin text, and a small built-in fallback draws
Chinese, Japanese and Korean characters and emoji. Other scripts need an
optional language pack (up to about 1 MB each, on the Memory Stick):
Cyrillic, Extended Latin (for example Vietnamese), Arabic, Hebrew, Devanagari
(Hindi) and Bengali–Assamese, plus nicer Japanese, Simplified Chinese,
Traditional Chinese and Korean glyphs. A color-emoji pack (about 5 MB) is
also available. Without a pack, those words show as blank cells, or as boxes
in bold text. Hindi, Bengali and Assamese use pre-shaped common syllables;
unusual conjuncts outside the pack's inventory may use separate fallback
cells. Arabic and Hebrew pages are laid out right to left.

When a page you are reading uses one of these scripts and its pack is not
installed, a notice appears above the bottom bar, for example
**Cyrillic: This page needs this language pack. Install it?** It
does not stop you reading or scrolling, and it disappears after about 12
seconds. While it shows:

- **X Install...** opens a short confirmation. Tilefinch first fetches and
  verifies the pack's signed details (**Checking size...**), then shows the
  pack and its real download size, for example **Cyrillic (1.0 MB)**, and
  asks **Install this font pack?**
  Press Cross again to download, verify and install it exactly as the menu
  does, with progress on the notice line; Circle cancels. One press never
  starts a download. When the install finishes, the page redraws with the
  new letters without reloading. If the PSP is offline or the check fails,
  the confirmation shows the reason; if the Memory Stick lacks space, the
  notice says so and nothing is installed.
- **Triangle Don't ask again** stops offers for that pack. The choice is
  saved with your settings.
- Circle is still Back. It also closes the notice.

The notice appears at most once per site each time Tilefinch runs, only on
pages with a meaningful amount of the script (not a stray word or a list of
language names), and only when the fonts really lack those letters. It also
appears in Reader mode and Basic view, which use the same fonts. A button
pressed in its first half second still goes to the page.

You can also manage packs yourself in **Menu → Settings → Appearance →
Language & emoji**:

- choose the **Language**, then select **Language pack** and press Cross
  (**X Download pack**). The menu downloads at once, without the size
  question. After any install the selected language and color-emoji packs
  are attached again straight away, with no restart;
- Square removes an installed pack;
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
**Menu → Settings → Browsing & input → Game buttons** chooses Cross or Circle
as the primary face button.

- A page can enter fullscreen only after a button press. Triangle or the
  Start + Select chord brings Tilefinch's chrome back.
- Game audio supports WAV sound effects and simple generated tones, with
  volume, stereo panning, looping, and short scheduling: up to eight loaded
  sounds and four playing at a time. It does not decode compressed audio.
- Installed games can use Tilefinch's direct-multiplayer API, shaped like
  `RTCDataChannel`. [Direct multiplayer](MULTIPLAYER.md) covers LAN
  discovery, numeric invites, NAT traversal, page limits, and the networks
  that cannot connect without a relay.

Game authors should start with
[Tilefinch Game Profile v1](GAME_PROFILE.md), the complete API,
resource, lifecycle, packaging, and qualification contract, and then read the
[WebGL guide](WEBGL.md) for renderer details.

## Screenshots

**Menu → Page tools → Save screenshot** saves what is on screen as a PNG in
`data/screenshots/`. It is written a little at a time, so saving does not
freeze navigation, and the completion message names the new file. **Menu →
Library → Shots** lists the newest 32 with their sizes. Delete old ones over
USB when space runs low.

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
script, and Tilefinch estimates that the PSP needs about 10 seconds for each
megabyte to start it (often less, when the site splits its app into pieces
it runs as needed). Tilefinch weighs every page's scripts as they arrive, and
when a page's scripts would take 15 seconds or more to start, what you see
depends on what the site sent (laying out and drawing what they build takes
time on top):

- **The page already shows its text**, as news front pages and most
  articles do. Its scripts keep running in the background and you can read
  and scroll at once. A status says how much script the page runs and
  roughly how long it takes; while it shows, Cross stops the page's scripts
  (Circle keeps reading). It comes back once more if the page runs low on
  memory. Stopping scripts keeps the page as it is; if that leaves it blank
  or without links, **Basic view fallback** takes over as usual. If the
  scripts run out of memory and wipe the page as they fail, Tilefinch puts
  back the page the site sent, stops its scripts and says "Page scripts
  stopped: not enough memory".
- **The page is an empty shell for an app**, as some social sites send when
  you are logged out. Basic view cannot help there, so Tilefinch asks first,
  with the size and an estimate, for example "It needs about 5.1 MB of
  script. It may take about 51 s to start." Cross starts it for this
  session, Triangle always for this site, and Circle (or Select) does not
  start it and leaves the page as it is. The app's big scripts wait while
  you decide; nothing else on the page stops. When a shell's scripts all
  came with the page itself, so there is nothing to hold back, you see the
  status above instead, titled "Large app starting".
- **The app cannot fit, even in the best case.** Tilefinch does not start
  it. On an empty app shell it says how much memory the app would need
  against what is free (or that its script is over the 4 MB limit) and
  offers **Show Basic view** (unless **Basic view fallback** is Off),
  **Try Reader mode**, turning off JavaScript for the site, or going back.
  On a page that already shows its content, a status says a large page
  script was skipped and you keep reading.

**Menu → Settings → Browsing & input → Heavy pages** chooses **Ask** (the
default, as above), **Run** (never ask; scripts still show their status) or
**Basic view** (never start the big scripts of an app shell or of a page that
cannot fit; the page stays as the server sent it). The sites you chose to
always run are remembered on the Memory Stick with your settings; **Reset
permissions** in a site's **Site information** forgets the choice.

A single script may be up to 4 MB. Whether it runs depends on the memory its
compile needs, usually about twice its size and sometimes much more. A script
that needs more than the page has free is refused on its own, and the rest of
the page's scripts keep running. If large sites stay empty with a script
error, see
[A site misbehaves because of its scripts](../TROUBLESHOOTING.md#a-site-misbehaves-because-of-its-scripts).

## Faster revisits: Keep compiled scripts

Most of the time a script-heavy page takes to load on the PSP goes into
compiling its JavaScript. Tilefinch keeps compiled scripts in RAM for the
session, so going back to a page is faster until you exit. **Menu → Settings
→ Device & storage → Site data & storage → Keep compiled scripts** also keeps
them on the Memory Stick, so pages you visited before restarting Tilefinch
start faster too. It is **Off** by default because it uses Memory Stick
space (at most 8 MB, shown on the row while it is on) and writes to the
stick while you read a page. **Clear compiled scripts** below it empties
it, and turning the option off removes its files. **Clear HTTP caches**
empties it too, and **Clear data for this site** removes that site's
compiled scripts. If a site misbehaves after it updates, clear the compiled
scripts or turn the option off
([Troubleshooting](../TROUBLESHOOTING.md#a-site-misbehaves-after-it-updates)).

## Per-site controls

**Menu → Page tools → Site information** shows the current connection, its
certificate issuer and the site's cookie and storage use. From there you can
open the site's **Site storage**, which shows whether that storage is in RAM
or on the Memory Stick, clear the site's data, or reset all of its
exceptions, and **Permissions & controls** holds the switches below. A switch
that cannot change on the current page says why when you press Cross.

- **JavaScript.** Wikipedia opens with page JavaScript off by default because
  browsing is more responsive that way; search, article links, and scrolling
  work without it. Turning it on under **Permissions & controls →
  JavaScript** applies to Wikipedia's language subdomains too, and **Reset
  permissions** restores the default. Other sites are unaffected.
- **Ad blocking.** Basic blocking and conservative cosmetic hiding are on by
  default. **Menu → Settings → Privacy & security → Content blocker** chooses
  Off, Basic, or Custom, and **Hide page ads** controls only the cosmetic
  hiding. **Permissions & controls → Content blocking** turns both off for
  the current site. Basic reads no list from the Memory Stick; Custom reads
  `data/adblock.txt`, and **Load allowlist** imports extra site exceptions
  from `data/adblock-allow.txt`, up to 32 sites in all.
  [The content-blocking reference](CONTENT_BLOCKING.md) lists the built-in
  hosts and selectors, the accepted custom syntax, and the limits.
- **Cookie notices.** Common consent overlays are hidden by default without
  clicking Accept or writing consent cookies. **Permissions & controls →
  Cookie notices** shows them again for the current site, independently of
  ad blocking.
