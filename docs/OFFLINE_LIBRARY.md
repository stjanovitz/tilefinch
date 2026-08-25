# Offline library

Tilefinch has a small, explicit offline library for Reader articles, bounded
web-app snapshots, and videos from its built-in YouTube provider. Nothing is
scanned or opened during boot. The first **Library** action lazily reads the
index from `data/offline/`.

## Reader articles

**Library → Save article for later** takes the current committed document and
writes a compact Reader snapshot. Headings, paragraphs, lists, block quotes,
and preformatted regions remain semantic, reflowable HTML; script, author
style, form state, cookies, event
handlers, and the live JavaScript realm are not retained. The snapshot has a
link to the original URL and uses self-contained, reflowable HTML, so opening
it performs no network request.

An article is capped at 1 MiB on disk. The published file is length- and
checksum-verified before it is parsed again. Saving the same source URL
replaces its existing library entry instead of consuming another slot. The
writer checks free space first, records the save date, emits bounded chunks, and cooperatively polls
Circle so a slow Memory Stick does not make the browser chrome unresponsive.
At the 12-item combined cap, the library marks its oldest dated item as a
deletion suggestion instead of silently choosing one for the user.

This first format intentionally does not archive page images or author CSS.
That keeps a saved article predictable and useful under the PSP memory limit,
but it is not a pixel-exact Web archive.

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
the user confirms. Cancel releases the small retained manifest/icon
preparation. The confirmed pass repeats every size, budget, and free-space
check before publishing.

The snapshot is capped at 1 MiB of document markup, 1 MiB of response bodies,
32 resources, and one fixed-size icon thumbnail. Reinstalling the same source
publishes a new payload generation and updates the index before deleting the
old one. This is intended for small, mostly self-contained games and tools.
It does not crawl links, guess assets that the page never requested, archive
cross-origin dependencies, retain login cookies, or promise that a
server-dependent application will work offline.

This is deliberately not a Service Worker implementation. Tilefinch does not
run background fetch, push, periodic sync, install events, or a page-authored
offline request router. The saved app is an explicit user snapshot using the
browser's existing loader and security policy, not a new privileged runtime.

`examples/prism-break-3d/` is the repository's reference offline app. Its
manifest, stylesheet, and game script are same-origin and self-contained, so
an HTTPS-hosted copy can be previewed, installed, reopened without a network,
updated, and uninstalled through the same user-facing path as another small
web game.

Installed apps appear in **Library → Saved**. Square says **Uninstall** for an
app and requires a second press on the same row; Circle cancels the armed
action. The index is made durable before the app payload generation is removed.

## YouTube downloads

The lightweight watch page includes **Save video offline**. Tilefinch resolves
that video through the same provider boundary as native playback, honors the
current 240p/360p quality option, and downloads the selected direct MP4 video
and (when needed) audio tracks. Ciphered, DRM-protected, live, unavailable, and
unsupported formats are refused rather than saved incorrectly.

Only one download is active at a time. The Downloads section is the manager:
X pauses or resumes an incomplete item and opens a completed item in the
native player. Its active row reports percent complete, measured transfer
speed, and the last sampled free-space reserve; paused and failed rows retain
the bounded failure reason across restarts.

Transfers use exact 256 KiB ranged
chunks, expose at most 32 KiB/2 ms of body work to one UI pump, and write
directly to `.part` files; a complete stream is renamed into place. **Resume**,
**Pause**, **Play**, and **Delete** are available from the offline library.
Restarting after an app or
power interruption converts an in-progress record to Paused and reconciles
bounded file sizes, so Resume obtains a fresh expiring YouTube URL and
continues from the retained byte offset.

**Settings → Device & storage → Resume saves** is off by default. When enabled, Tilefinch lazily
opens the offline index after the initial page is ready and resumes the first
paused or queued video; successful completion then advances through the queue.
Keeping it off preserves the default no-library-I/O boot path.

The fixed range and per-frame pump limits bound Memory Stick and browser-thread
work. Users control when writes occur by pausing individual items and by the
off-by-default Resume saves preference; merely viewing Downloads performs no
payload write.

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
