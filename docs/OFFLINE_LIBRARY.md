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

## File downloads

Opening a common archive or binary link (such as a ZIP or PBP) offers
**Save file?**. Ordinary GET navigation also recognizes attachment headers and
supported binary content types, including extensionless download endpoints,
before parsing the response body. For another file type, focus its
link and choose **Page tools → Download focused link**. Same-origin HTML
`download` links also offer saving. The native confirmation stays over the
current page, so cancelling does not reload it or lose its scroll and focus.
It shows the source origin, not a potentially sensitive query string.

After confirmation, the existing **Downloads** section provides Pause,
Resume/Retry, Details, and Delete. Each file receives an ID-prefixed safe
filename under `data/offline/` on the Memory Stick. The server's
Content-Disposition filename takes precedence over the URL basename; path
components and unsafe FAT characters are removed. Non-ASCII filename bytes
currently become underscores. Downloaded bytes are not changed or executed.
Use USB storage to move a file, or open a completed ZIP's Details to request
the separately confirmed installation described below.

Transfers accept HTTPS only, including every redirect. They stream directly
to a partial file, using at most 32 KiB of body callbacks or 2 ms of owner work
per pump. A file is limited to 512 MiB and shares the existing 12-item library
and one-active-download limit. At least 8 MiB of free space is reserved.
The transport ring is bounded independently of file size. A completed stream
is flushed and its completed length recorded before publication.

Resume requires a matching strong ETag or Last-Modified validator and an
exact Content-Range. A server that returns a fresh complete response starts
over safely rather than appending it. Without a validator, Retry restarts the
file. HTML responses without an explicit attachment disposition, HTTP errors,
unverifiable partial responses, over-limit
bodies, and failed writes leave a retryable item rather than a completed file.
Existing destination files are not intentionally overwritten.

No save occurs before confirmation. Retrying persists the original download
URL and validator, which can themselves contain private access tokens; these
are local, unencrypted files, not diagnostic logs. Cookies and passwords are
not saved in download metadata, and persisted failure messages omit transport
text that could contain a signed URL. Delete removes the record, partial,
metadata, and completed file. The index format is upgraded when this feature
is used; older app versions cannot read the new format.

They do not bypass a site's authentication, API, or JavaScript requirements.

### Install homebrew from ZIP

A completed ZIP offers **Install homebrew…** in Details, including when its
filename has no ZIP extension. This signature check is only a UI hint. A
read-only validation pass must find exactly one `EBOOT.PBP` and an unambiguous
app folder before showing the native confirmation with the destination,
expanded size and file count. Nothing is installed until **X Install**.

Installation targets `ms0:/PSP/GAME/<app>` (or `ef0:` for an internal-storage
library). Bare EBOOT archives receive a folder derived from the archive name.
Existing destinations are refused, including links; this is not an updater.
Only install software you trust: ZIP checksums detect corruption, not malicious
programs or publisher identity. No game is launched automatically.

The first implementation supports single-disk stored/DEFLATE ZIPs, at most
512 entries, eight path components, 128 MiB per file and 256 MiB of installed
data, with an 8 MiB free-space reserve. Names must be portable ASCII/FAT names.
It refuses traversal and absolute paths, case aliases, duplicate/conflicting
names, symbolic/special files, encryption, ZIP64 and ambiguous multi-app
packages. Explicit directory entries must be empty and stored. Root-level
documentation and desktop resource-fork metadata stay in the original ZIP.
Other payloads outside the selected app folder make the archive ambiguous.

Extraction uses bounded Budget memory and cancellable 32 KiB/2 ms owner slices.
Every installed file must match its declared length and CRC; the EBOOT header
is checked too. Files are written beneath a newly created private staging
folder and published by one final directory rename. Cancellation or a normal
failure attempts to remove only this attempt's staging files and retains the ZIP. A power
cut may leave `.tilefinch-unpack-<id>` under `PSP/GAME`; it is deliberately not
deleted automatically. Remove that unfinished staging folder through USB
before retrying the same item. Deleting the download does not uninstall a game.

**Downloads → Installed homebrew** lists the folders installed by Tilefinch,
even after their source ZIPs have been deleted. Choose an app, review its native
**Remove homebrew?** confirmation and press X to remove the folder. This also
removes saved data inside that folder; downloads and other app folders are not
touched. Cancel before confirmation writes nothing. Stopping a removal after
it begins cannot restore files already deleted; its receipt remains so the
remaining contents can be inventoried and removed on a retry.

Each new installation includes an 80-byte, corruption-checked
`.tilefinch-homebrew` ownership receipt containing only its folder name and
local installation ID—not its source URL or credentials. ZIPs cannot supply
that reserved root file. Manual/older installations without a valid receipt
are never offered for removal. The manager supports at most 64 installations
and examines at most 512 GAME directory entries. Removal inventories at most
1,024 files/directories, eight levels and 256 MiB before confirmation; links,
special files, changed types and larger/unsupported trees require USB cleanup.
Deletion is pumped in small, cooperative batches rather than one recursive
blocking call. A filesystem failure retains the receipt when possible and
reports an incomplete removal instead of claiming success.

The host `tilefinch-file-download-tests` runs synthetic streaming, recovery,
filename, confirmation, and allocation-refusal checks without network access.
Its optional `--fetch-url HTTPS_URL EXPECTED_SHA256` mode performs one public
download into a temporary host directory, checks its complete SHA-256, reports
bytes and Budget peak, then deletes it. `--source-journey PAGE_URL FILE_URL
EXPECTED_SHA256` additionally checks actual rendered-link activation, native
confirmation and cancellation continuity. `tilefinch-zip-install-tests` covers
staging, publication, corrupt/unsafe archives, cancellation and allocation
refusals; `--qualify-archive LOCAL_ZIP` extracts a public package only into a
temporary host folder and removes that test installation. Do not pass account-bearing URLs to
this command or include them in shared logs.

The optional `tilefinch-file-download-tests --profile` reports host owner-pump
times and simulated focus/scroll-to-frame readiness on a warm synthetic page
while an 8 MiB response is streamed. The simulated press is due immediately
before a pump, so its timing includes that pump's blocking work. This is not
physical display latency or a prediction of Memory Stick speed.
`tilefinch-zip-install-tests --profile-archive LOCAL_ZIP` reports preview and
extraction-pump timing using the same bounded installer.

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
