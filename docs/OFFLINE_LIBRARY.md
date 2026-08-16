# Offline library

Tilefinch has a small, explicit offline library for Reader articles and videos
from its built-in YouTube provider. Nothing is scanned or opened during boot.
The first **Library → View offline library** action lazily reads the index from
`data/offline/`.

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

## YouTube downloads

The lightweight watch page includes **Save video offline**. Tilefinch resolves
that video through the same provider boundary as native playback, honors the
current 240p/360p quality option, and downloads the selected direct MP4 video
and (when needed) audio tracks. Ciphered, DRM-protected, live, unavailable, and
unsupported formats are refused rather than saved incorrectly.

Only one download is active at a time. Transfers use exact 256 KiB ranged
chunks, expose at most 32 KiB/2 ms of body work to one UI pump, and write
directly to `.part` files; a complete stream is renamed into place. Circle
pauses a resolving or downloading item. **Resume**, **Pause**, **Play**, and
**Delete** are available from the offline library. Restarting after an app or
power interruption converts an in-progress record to Paused and reconciles
bounded file sizes, so Resume obtains a fresh expiring YouTube URL and
continues from the retained byte offset.

**Options → Resume saves** is off by default. When enabled, Tilefinch lazily
opens the offline index after the initial page is ready and resumes the first
paused or queued video; successful completion then advances through the queue.
Keeping it off preserves the default no-library-I/O boot path.

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
