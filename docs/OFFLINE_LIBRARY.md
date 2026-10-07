# Offline library

Tilefinch has a small, explicit offline library for Reader articles, bounded
web-app snapshots, and videos from its built-in YouTube provider. Nothing in
it is scanned or opened during boot; the first **Library** action reads the
index from `data/offline/`.

## Reader articles

**Page tools → Save article** writes a compact Reader snapshot of the current
committed document. Headings, paragraphs, lists, block quotes, and
preformatted regions stay semantic, reflowable HTML; scripts, author styles,
form state, cookies, event handlers, and the live JavaScript realm are not
kept. The snapshot is self-contained and links to the original URL, so opening
it makes no network request.

- An article is capped at 1 MiB on disk, and the published file's length and
  checksum are verified before it is parsed again.
- Saving the same source URL again replaces its library entry instead of
  taking another slot.
- The writer checks free space first, records the save date, writes bounded
  chunks, and polls Circle between them, so a slow Memory Stick does not make
  the browser chrome unresponsive.
- At the 12-item combined cap, the library marks its oldest dated item as a
  suggestion to delete rather than choosing one for you.

This format deliberately archives no page images or author CSS. That keeps a
saved article predictable and useful within the PSP's memory, but it is not a
pixel-exact web archive.

## Offline web apps

**Page tools → Install offline app** is available for a committed HTTPS page
with a same-origin Web App Manifest. Tilefinch reads the manifest's bounded
name, short name, start URL, scope, icon metadata, theme color, and display
mode. Theme colors use the engine's bounded CSS color parser. Display is
limited to `browser`, `minimal-ui`, `standalone`, and `fullscreen`; unknown
values use `browser`. Tilefinch stores a 16×16 decoded icon for the native
Saved list, serializes the current live document, and
packages the same-origin resource bodies that the current page has already
loaded. Reopening the item restores those bodies with their original typed
resource or module authority before parsing the saved document under its
original origin. A cached script therefore does not become executable merely
because it was written to disk.

“Already loaded” includes same-origin assets requested dynamically by page
script, including game images prepared through `decode()` or
`createImageBitmap()`, when their response is still present in the bounded HTTP
cache at preview time. Installation does not inspect JavaScript objects or
crawl guessed URLs; the response cache remains the single authority and size
boundary.

Installation is a two-step user action. The first pass builds and immediately
discards a bounded candidate snapshot, then shows its estimated payload size,
captured resource count, known unavailable-resource count, display mode, and
theme color. It labels a new app **Install**, changed content at the same source
**Update**, and byte-identical content **Reinstall**. No payload is written until
the user confirms. To measure the package the preview compiles each classic
script that lacks bytecode; it keeps that bytecode (at most 1 MiB, charged to
the page Budget) so the confirming install publishes it instead of compiling
the same scripts again. Cancel releases it with the small manifest/icon
preparation. The confirmed pass repeats every size, budget, and free-space
check before publishing.

Both passes compile at most one script at a time and show progress on the
status line (**PREPARING 2 OF 4 SCRIPTS - CIRCLE STOPS**). Circle stops either
pass between scripts or pack records: a stopped preview keeps nothing, and a
stopped install removes its temporary files and leaves the library unchanged.
On the host, previewing then installing Treadline now compiles its four
scripts once instead of twice; the install's Budget peak fell from 2.07 MB to
37 KB, while the preview peak rose from 2.07 to 2.15 MB because it holds the
335 KB of bytecode it compiled (2.85 to 3.32 MB with 631 KB kept for 1 MiB of
synthetic script).

The snapshot is capped at 1 MiB of document markup, 1.5 MiB of response bodies,
32 resources, up to 1 MiB of optional classic-script bytecode compiled from
at most 1 MiB of source, and one fixed-size icon thumbnail. Only responses the
live memory cache still holds can be captured, so a large game may need a
larger **Memory cache** setting while it is loaded for installation. The
resource pack is streamed to and from the Memory Stick: installing does not
hold a second copy of it in memory, and launching holds one member at a time
beside the restored cache entries. Reinstalling the same source
publishes a new payload generation and updates the index before deleting the
old one. This is intended for small, mostly self-contained games and tools.
It does not crawl links, guess assets that the page never requested, archive
cross-origin dependencies, retain login cookies, or promise that a
server-dependent application will work offline.

### Script source on demand

Launching reads and checksums the whole pack as before, but a classic script
stored with current bytecode is restored as bytecode only: its source is
hashed through a 16 KiB buffer and stays in the pack. The script then runs
from its bytecode without its source ever being in memory. The source is read
from the pack, verified against the hash taken at launch, and restored as an
ordinary cache entry (with its bytecode) only when something needs the
bytes:

- the bytecode fails to restore (for example a damaged artifact); the script
  is then compiled from that source, as before;
- the `<script>` element has `integrity` metadata, which is checked against
  the bytes;
- the page inserts that script dynamically, or any other loader asks the
  cache for the response;
- a cross-origin script of at least 192 KiB needs the static cost check
  (packs hold only same-origin responses, so this does not occur in
  practice);
- the page-capability trace wraps the source;
- the running app is installed again (the capture reads every source).

What page script observes is unchanged: offline bytecode was already
compiled without source text, so `Function.prototype.toString()` returns the
same `[native code]` body, and error positions come from the same line
tables. The only difference is a browser diagnostic: a script that throws
while running from bytecode does not record the surrounding source lines in
the failure trace. If the pack was removed or damaged after launch, the read
fails its length or hash check, the request misses like any absent response
(the script's `error` event fires), and `offline-app: deferred script source
unavailable` is logged. A Budget refusal while reading leaves the script
deferred for a later attempt. The deferred bytecode is cache: it goes with
the session cache or the next app launch, and under memory pressure only the
bytecode is released; the script stays a hit and compiles from its source,
read on demand, instead of missing to the network.

Measured on the host (`tilefinch-offline-app-limits-tests`, realistic 24 MiB
budget), the launch's Budget peak and what it leaves resident fell from
1.42/0.90 MB to 0.37/0.35 MB for Treadline and from 2.07/1.66 MB to
0.65/0.63 MB for 1 MiB of synthetic script. Those Treadline figures were for
an older five-script model of the game; the test now installs the shipped
module set read from `index.html` (seven deferred scripts, 675 KiB of
source), whose launch peak and resident bytes are 0.48/0.45 MB.

### After a browser update

Stored bytecode is valid only for the compiler that produced it. When an
update changes it, **Library → Saved** marks the app **RECOMPILE** and its
detail line says **Needs recompile for this browser**; the hint bar says
**X RECOMPILE**. The check is a comparison against a fingerprint kept in the
library index (which compiler ABI produced the bytecode, and for how many
scripts), so showing the list reads nothing else. An entry saved by an older
build has no fingerprint yet; its pack's record headers are read once,
skipping every body, and the result is kept with the index.

X on that row opens **Recompile offline app**: the saved size, how many
scripts will be recompiled, and **X Recompile**, **Square Open anyway**,
**O Back**. Recompile reads the stored package record by record, compiles
each classic script from its stored source (the same bounds as installing:
up to eight scripts and 1 MiB of source and of bytecode), shows **PREPARING
2 OF 4 SCRIPTS - CIRCLE STOPS**, and writes a new generation through the
same temporary-file and index-first transaction as an install: the markup
and icon are copied after their checksums are verified, the new pack is
written beside the old one, the whole old pack must pass its checksum before
anything is published, and the index changes before the old generation is
removed. Circle, a full Memory Stick, a write failure, a damaged old pack or
a Budget refusal leaves the old generation and the index exactly as they
were (a script whose compile alone is refused is published source-only, as
in an install). The recompiled app then opens. **Open anyway** opens the app
as before: every script is compiled from its stored source at launch, which
is slower.

The fingerprint lives in a field web-app entries did not use, so the index
format is unchanged and a build from before this change, after an A/B
rollback, still reads it. The library page used by the host lab shows the
same state with **Recompile** and **Open anyway** links.

This is deliberately not a Service Worker implementation. Tilefinch does not
run background fetch, push, periodic sync, install events, or a page-authored
offline request router. The saved app is an explicit user snapshot using the
browser's existing loader and security policy, not a new privileged runtime.

`examples/prism-break-3d/` is the repository's reference offline app. Its
manifest, stylesheet, and game script are same-origin and self-contained, so
an HTTPS-hosted copy can be previewed, installed, reopened without a network,
updated, and uninstalled through the same user-facing path as another small
web game. `examples/treadline-arena/` follows the same packaging contract with
a self-contained WebGL and Gamepad tank game whose Survival, Team Control, and
Convoy Escort modes, tank classes, and optional Command rules all reopen
without network access. Its optional two-player Team Control mode requests the
network only after the player chooses Host, Find LAN, or Enter code; opening
the installed game itself remains offline. The channel and discovery contract
is documented in [Direct multiplayer](MULTIPLAYER.md).

Installed apps appear in **Library → Saved**. Square says **Uninstall** for an
app and requires a second press on the same row; Circle cancels the armed
action. The index is made durable before the app payload generation is removed.

An installed app whose captured manifest requests `display: fullscreen` enters
an immersive page presentation as part of the trusted Library handoff, so the
browser bars do not cover its first menu frame. This does not grant controller
capture or DOM fullscreen to page script: Triangle restores Tilefinch chrome,
and Page-controls still requires a trusted activation or the Start+Select
chord. Browser, minimal-UI, and standalone snapshots keep the ordinary browser
presentation for now.

## YouTube downloads

The lightweight watch page includes **Save video offline**. Tilefinch resolves
the video through the same provider boundary as native playback, honors the
**Settings → Video → YouTube** quality (240p or 360p), and downloads the
selected direct MP4 video and, when needed, audio tracks. Ciphered,
DRM-protected, live, unavailable, and unsupported formats are refused rather
than saved incorrectly.

Only one download runs at a time, and the **Downloads** section manages them:
X pauses or resumes an incomplete item and opens a finished one in the native
player, and **Resume**, **Pause**, **Play**, and **Delete** are all available
there. The active row shows percent complete, measured transfer speed, and
the most recently sampled free-space reserve; paused and failed rows keep
their bounded failure reason across restarts.

Transfers use exact 256 KiB ranged chunks, give one UI pump at most 32 KiB or
2 ms of body work, and write straight to `.part` files, renaming a complete
stream into place. After an app or power interruption, restarting converts an
in-progress record to Paused and reconciles the bounded file sizes, so Resume
fetches a fresh, expiring YouTube URL and continues from the saved byte
offset.

**Settings → Video → Resume saves** is off by default. When it is on,
Tilefinch opens the offline index after the first page is ready and resumes
the first paused or queued video, moving through the queue as each one
finishes. Leaving it off keeps boot free of library I/O.

The fixed range size and per-frame pump limits bound Memory Stick and
browser-thread work. You control when writes happen, by pausing individual
items and through the off-by-default **Resume saves**; just viewing Downloads
writes nothing.

Each video and audio stream is capped at 512 MiB, the library holds at most 12
combined items, and a download refuses to begin unless it can retain at least
8 MiB of free Memory Stick space after the remaining bytes. The library does
not run concurrently with native video playback. Local playback reuses the
ordinary bounded MP4 demuxer and PSP decoder through a size-checked file-range
reader; downloaded files are not copied into the page heap.

## Storage and privacy

The index is bounded to 32 KiB and written through a temporary file. A prior
complete generation remains in `library.bin.bak`; startup validates the
primary, temporary, and backup generations in that order, then republishes a
recovered copy without first destroying the surviving generation. Replaced
articles retain the matching previous payload until the new index is durable,
and article reads accept that backup only when its recorded length and checksum
match the active index. Article payloads are corruption-checked; media chunks
require an exact HTTPS response length and completed files must match their
provider metadata length before opening. These files are not encrypted.
Anyone with the Memory Stick can read saved articles, titles, source URLs, and
video files. Deleting an item removes its published, backup, temporary, and
partial payloads after the index update succeeds.

Offline saves are user-initiated. Ordinary pages cannot silently invoke the
management routes: YouTube enqueue links are accepted only from a committed
YouTube watch page, and delete/resume/open actions only from the internal
offline library origin.
