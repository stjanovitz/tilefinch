# Media pipeline

This page covers the media path from source to screen: the YouTube provider
adapter, the range and HLS sources, the portable demuxer, both PSP decoders
(firmware and optional software) and how a stream chooses between them, the
native player, and the tests and logs that qualify each seam.

Tilefinch treats YouTube as a bounded site adapter, not as a special case in
the HTML, CSS, layout, or paint engines. The adapter turns public mobile
YouTube responses into a small script-free document, and the native player
turns a supported watch URL into ordinary MP4 range sources.

## End-to-end shape

```text
mobile URL
  -> bounded provider fetch and parser
  -> ordinary Tilefinch HTML page
  -> watch activation
  -> bounded player-response resolver
  -> progressive MP4, separate AVC/AAC MP4, AAC alone, or live HLS
  -> HTTP range reader or rolling HLS playlist/segment source
  -> portable MP4 or MPEG-TS demuxer
  -> host or PSP codec backend
  -> native player surface
```

The browser engine never contains YouTube selectors or layout exceptions.
The provider boundary is also where a future provider can be replaced without
changing navigation, rendering, media ownership, or player controls.

The optional audio-only setting selects the adaptive AAC representation at
route open. It does not request video bytes and does not construct the video
range, H.264 demux/decoder, or picture surfaces; it reuses the same bounded
audio transport, buffering, pause, and seek machinery. The setting is sampled
once per new YouTube route so changing Settings cannot mutate a live pipeline.
Page `<video>` and saved media are deliberately unaffected by it.

## Provider document

Home, search, channel, playlist, history, and watch routes use the registered
site-adapter boundary (`youtube_lite_route()`). A Shorts link opens as an
ordinary watch page. Any other path on a YouTube host opens the native home
route, so the full YouTube application never runs. Account controls are
drawn by the native provider; signing in uses Google's account page, which
then hands back to the provider.

The adapter reads mobile-page data without evaluating it, extracts a bounded
set of renderer records, releases the source buffers, and commits generated
HTML through the normal navigation transaction. The resulting page uses the
same forms, links, CSS, images, focus model, history, and scrolling as every
other document.

Playback always requires an explicit native-player activation. The primary
result card opens media directly; its adjacent info control opens the watch
document without playing. The watch document's thumbnail is the corresponding
explicit play control. Entering, restoring, or traversing history to a watch
URL never starts media merely because of the URL shape.

| Resource | Bound |
| --- | ---: |
| Source response | 2 MiB |
| Generated HTML | 96 KiB |
| Search, playlist and history entries per page | 12 |
| Watch recommendations | 12 |
| Full description | 8 KiB |
| Comments response | 256 KiB |
| Rendered comments | 8 |

A response that holds more entries than a page is shown a page at a time:
**Load more** continues inside the same response (refetching it) before it
follows YouTube's continuation token.

Provider work runs through the ordinary one-slot fetch scheduler. Parsing and
HTML generation advance in bounded slices, remain cancellable, and preserve
the incumbent page until the replacement commits. A validated mobile API
identity occupies one fixed 1.4 KiB session slot for at most 30 minutes. It
is bound to the signed-in account and the visitor cookie, so it is dropped
when either changes or cookies are cleared, but not when YouTube's preference
cookies or other sites' cookies change; the player resolver starts from it
on every system language. Once the signed-in feed, a rating or an account
action has read the mobile home page and found the account signed in, the
slot also records that, so later feeds, ratings and account actions send
their API request at once. If that first answer refuses the request, the job
reads the home page once and tries again.

### Provider caches

The provider keeps at most two generated Home or first-search documents for
two minutes in the session Budget. Keys include the exact URL, the
presentation variant, and the cookie authority visible to the request;
eviction is least-recently-used. This cache is optional session memory: it
takes part in ordinary cache reclaim, **Clear HTTP caches**, and site-data
clearing, so a generated provider page cannot outlive the user's clear action
or crowd out an authoritative navigation under pressure. Raw provider
responses and decoded images are not kept. Result cards commit with fixed
thumbnail geometry, and their explicitly lazy images join the ordinary
post-paint resource queue.

Details and their expansions share this cache for a parsed snapshot of watch
facts (at most 28 KiB per entry, never the raw response). The key is the
mapped watch URL, and the request-cookie fingerprint, language, date format,
two-minute age limit, and captive-portal exclusion must all match. Returning
to details can reuse these facts; an explicit Reload fetches fresh data.
Description can reuse the parsed description with no network job, while
Comments fetches only its fresh API response. The comments token is
discovered in bounded steps alongside the original build; unfinished
discovery never delays that build and instead sends a later Comments request
down the ordinary path, as does a cache or allocation miss. A rejected cached
continuation invalidates only its matching facts entry and uses the existing
single full-watch retry. Jobs copy facts before yielding and never borrow a
cache entry across a pump or keep DOM pointers.

## Resolver and admission policy

The resolver calls a fixed, signed table of client profiles. It parses
`playabilityStatus` before format selection so age, login, region, and
not-yet-started premiere restrictions produce useful errors rather than a
generic malformed inventory message. An active live stream or premiere
supplies an HLS manifest and enters the ordinary native-player lifecycle;
Retry re-resolves a scheduled event after it begins.

The final direct/enriched profile is VisionOS. The resolver does not use the
Android VR identity because its media URLs may stop serving after a small
prefix; smaller windows and reconnects cannot recover a URL-wide serving cap.
Tilefinch's production range windows are already below 1 MiB and can be
reissued after ordinary request failures. It does not generate YouTube
Proof-of-Origin (PO) tokens. If YouTube requires a player or media PO token for
a particular video or client policy, that route remains unsupported rather
than fabricating attestation state.

Only unciphered HTTPS media URLs are admitted. The resolver prefers a
progressive AVC/AAC MP4 within the configured height, then separate AVC MP4
and AAC MP4 streams. Active live delivery selects the bounded AVC MPEG-TS HLS
variant at or below 240p. Baseline/Main plus AAC-LC normally uses the PSP
firmware backend; High profile, and Baseline/Main streams the firmware would
refuse, use the optional software decoder (see
[Route selection](#route-selection)). Live playback begins three
segments behind the advertised edge for radio-loss tolerance and has no seek
operation because its zero and duration move with the playlist. The shipping
preference for finite video is 360p; users can select the
smaller 240p path, and the PSP session can retry once at 240p after a
wide-profile admission or decoder failure. HE-AAC is rejected because the PSP
audio path is dimensioned for AAC-LC.

Provider-resolved URLs must remain on HTTPS port 443 at `googlevideo.com` or
one of its subdomains. Redirects are disabled for those sources. Expiring URLs
are rejected before playback when their remaining lifetime is inadequate.
That lifetime is the one the player response states (`expiresInSeconds`),
counted on the monotonic clock from when the URL was resolved, so a PSP clock
set wrong does not reject fresh URLs; only a response without it is judged by
the URL's `expire=` against the device clock. The bounded mid-playback refresh ladder may re-resolve and seek to the current
position after an expiry-related failure. Live refresh reopens at the new edge
instead of attempting to seek into an expired rolling window.

Failures emit a bounded `tilefinch-youtube:` record containing the stage,
HTTP status, selected client, itag, attempt count, and remaining lifetime.
Signed URLs and response bodies are never logged.

## Range transport

`media_http.c` reads media directly into RAM. It never writes a playback cache
to the Memory Stick.

- Googlevideo reads use the bounded `range=first-last` query form.
- A `206` response must carry an exact `Content-Range`.
- A `200` response is accepted only when the requested body length and the
  already-authorized complete length match exactly.
- Each source owns one active window and one look-ahead window, each at most
  256 KiB in the PSP player.
- A demanded window which stops making byte progress is re-issued on at most
  three fresh connections. Slow but progressing bytes are retained. The
  logical window has one absolute deadline, after which it becomes terminal
  rather than remaining simultaneously failed and pending; selecting a new
  window starts a fresh incident.
- Malformed, short, oversized, or ambiguous responses fail closed.
- The shared transport worker performs network progress without blocking the
  browser thread. Request ownership remains singular and cancellation retires
  the slot by generation.

Sequential look-ahead starts earlier after a real buffering event. A sample or
fragment header spanning two adjacent windows is assembled from those already
owned buffers rather than downloaded again.

## Portable MP4 demuxer

`media_mp4.c` is independent of FFmpeg, the Memory Stick, and PSP firmware.

- Progressive files retain bounded `moov` sample tables.
- Indexed fragmented files retain the flat `sidx` and only the active
  fragment's sample tables.
- A fragmented single-track source can use the lazy sliding window. A
  multi-track fragmented source uses the eager compatibility path so tracks
  cannot share an invalid global fragment cursor.
- Seeks use the segment index when present, snap video to a keyframe, and
  align audio inside the selected window.
- AVC and AAC samples are emitted in decode-time order through one bounded
  packet buffer.

Every range-read failure remains distinguishable from structural MP4 failure;
logs include the failing offset and transport status rather than collapsing
both classes into “fragment plan failed.”

## HLS sources

HLS masters are parsed exactly, and large live media playlists
incrementally, keeping only the newest twelve complete segment records
rather than a DVR-sized response: a live playlist is compacted that way when
its text passes 64 KiB or it lists more than 256 segments. A finite (VOD or
ended) playlist is kept whole, so it may list at most 2,048 segments in at
most 128 KiB of text: about 68 minutes at 2-second segments or 3.4 hours at
6-second ones, and about 400 segments when each segment line is a 300-byte
signed URL. A longer one fails as an unsupported format ("PSP CAN'T PLAY THIS
VIDEO FORMAT", "Video playlist too long") and the page's next source, if any,
is tried. The bounds are a memory choice. A segment record is 16 bytes (an
offset into the retained playlist text, its start, duration and flags; its
sequence number is the playlist's first plus its index), so the table is
32 KiB, and the text is kept as an exact-size copy. While a playlist is
parsed the stream's retained text and that copy are both held. A master
lists at most 256 variants and audio renditions, in the same table.
Demuxed masters keep at most one video and one audio source, and each source
streams one MPEG-TS segment at a time through the shared worker.

A live source starts from the last three advertised segments, refreshes the
rolling media playlist at half its target duration, maps media-sequence
changes onto one continuous local clock, and records any skipped window as a
discontinuity. With a separate audio rendition the two playlists number
their segments independently and are fetched one after the other, so the
server may publish a segment in between; segment numbers say nothing about
which segments hold the same moment. Once both sources have primed, the
audio is lined up with the video by the first timestamp each one read (the
MPEG-TS PTS, or for packed audio the timestamp in the segment's ID3 tag).
When the audio starts a segment or more away it moves once to the segment
that holds the video's first picture and primes again; the remainder is
trimmed, dropping sound from before the first picture, or kept as a late
start of up to 50 ms. Audio whose timestamp lies outside the audio
playlist (unrelated clocks) is left where it started. Packed audio must
carry its timestamp in the segment's ID3 PRIV frame (RFC 8216, Packed
Audio); without one, the only placement the standard leaves is the
segment's position on its playlist timeline, so the two starts are matched
by their distance from their playlists' ends, both of which are the live
edge. A segment published between the two playlist fetches then shows up
as a whole-segment offset that nothing in the playlists reveals.

A live source that falls behind its window skips to the window's first
segment, and its clock goes on without the skipped content. Once a
separate audio rendition is lined up, the two share their skips, so a skip
by either removes the same content from both clocks however far the other
one skipped. The skipped duration comes from the timestamps: the new
segment's first one against where the segment before the skip would put
it. When there is none (packed audio without the ID3 timestamp), or it
disagrees with the skip's EXTINF and sequence-number estimate by more than
twice that estimate, the estimate is used. An estimate longer than the
skipped segments the old playlist lists plus the new playlist's whole
window is the numbers jumping (a renumbered playlist), not content: the
timestamp is then checked against that length instead (or must match the
estimate within an eighth, a long stall), and a skip neither measures
stays the skipping rendition's own and unlinks the pair, which then play
apart by the skipped content as unlinked renditions do. The rendition that
did not skip drops a shared skip's content too and does not fetch segments
wholly inside it; each segment it passes counts as progress for the
60-second stall limit.
The sound output is the clock and counts what it played, so sound
already queued still plays and a picture skip removes only what follows
it: the picture holds until the sound reaches it. Pictures dropped this
way resume at the next keyframe. One of the two request slots
stays free for a near-edge playlist refresh. Stale snapshots, temporary refresh failures, signed-URL
replacement, and an eventual `ENDLIST` are all bounded transitions, and a
playlist that makes no progress for 60 seconds fails visibly instead of
spinning.

HLS queues at most 64 samples and 576 KiB of payload, and it rejects
encrypted, fragmented-MP4, byte-range, and explicitly non-AVC variants.
A media playlist opened directly is played as muxed video and must carry AVC
video: a program with no video stream, or packed-audio segments, fails as an
unsupported format ("No supported codec"). Audio-only HLS is not supported.
Audio that arrives before a muxed segment's first SPS is trimmed to the
newest 48 frames instead of filling the queue.
Neither MP4 nor HLS writes media bytes to the Memory Stick. A finite HLS seek
resets only the selected segment and preserves an untouched in-flight request
across repeated cooperative readiness probes; rolling live playback
deliberately has no scrubber. Video-only variants may start priming as soon as
their PMT and first video sample are known instead of filling the whole first
segment.

## MP3 page audio

Page audio whose probe answers MPEG audio (an `audio/mpeg`-family type, or a
body starting with an ID3v2 tag or two consistent Layer III frames) opens
through `MediaMp3Source` (`src/media_mp3.c`) instead of the MP4 demuxer:

- **Probe.** The page-audio probe is the ordinary 1 KiB range probe with an
  `audio/mpeg` Accept and ICY status lines accepted as HTTP/1.0 200
  (`FetchRequest.icy_status_line`, a libcurl 200 alias). A 206 is a seekable
  file, a whole small 200 is a file of that length, and a body that
  overflows the 32 KiB sniff window is a live stream.
  `media_http_probe_route_audio` makes the call.
- **SHOUTcast root addresses.** A SHOUTcast DNAS server (v1 and v2) picks
  what its root address answers from the User-Agent: players get the
  stream, and web browsers (any agent naming Mozilla, Tilefinch's
  included) get a `302` to its status page, `index.html?sid=N`. Accept,
  Range, `Icy-MetaData` and `Accept-Encoding` do not change that answer.
  Every DNAS serves its first stream at `/;` to any client, and that is the
  address its own `listen.pls` and `listen.m3u` give. When page audio at a
  server root ends at that status page (same host and port, either
  scheme), `media_shoutcast_stream_url` names `/;` on the same server and
  the probe runs once more there, upgraded or not exactly as the first one
  was. Nothing else is rewritten: another path, server or answer is
  reported as it came.
- **Transport.** The stream is one long worker response (`psp_media_hls.c`
  in audio mode, a 6-hour transfer deadline, no `Icy-MetaData`), admitted
  under the page's policy on every hop as `TILEFINCH_DESTINATION_AUDIO`.
  A seekable file resumes or seeks with `Range: bytes=N-`.
- **Source.** A 128 KiB compressed ring holds about 8 s of a 128 kbit/s
  station, so an hour of radio uses the same memory as a second. Sync needs
  two consistent headers; a damaged frame drops back into a resync bounded
  at 64 KiB. Leading ID3 tags are skipped (by range when seekable), a Xing,
  Info or VBRI frame supplies a VBR file's length and seek table and is not
  decoded, and a dropped live stream reconnects with 0.5-4 s backoff, at
  most four times without a delivered frame.
- **Station text.** The stream's `icy-name` and `icy-genre` response
  headers (headers only; the player never asks for in-band metadata) are
  kept per response, and a reconnect's non-empty answer replaces them.
  `media_mp3_station_text` reads each as UTF-8 when the whole value is
  well-formed and as Latin-1 otherwise, drops C0 and C1 controls, collapses
  whitespace and cuts on a character boundary at 96 bytes. The name becomes
  the player title, ahead of the element's `title` attribute and the page
  title; the genre is the line under the listening state. A live reconnect
  reports `reconnecting` in the stats from the break until the new
  response's first bytes.
- **Decode.** `sceAudiocodec` in MP3 mode decodes one frame per call into a
  1152-sample buffer that is re-blocked into the audio queue's 1024-sample
  stereo blocks. MPEG-1, 2 and 2.5 at every Layer III rate, CBR or VBR,
  mono or stereo. The decoder is skipped when the optional software decoder
  owns the Media Engine.

MP3 inside MP4 or HLS is not supported, and the host lab has no MP3 backend:
host tests cover the parser, source and probe (`tilefinch-media-mp3-tests`,
`tilefinch-media-mp3-stream-tests`), and decode is checked under PPSSPP and on
the device.

## PSP decode and presentation

Two decoders feed one native player. The media-session state machine owns
opening, priming, playing, paused, buffering, seeking, recovery, and quiescing
for both; see [PSP media session state](PSP_MEDIA_SESSION_STATE.md). It is the
only authority that may decide when a pipeline can be replaced or destroyed.

### Route selection

The stream's first SPS decides the backend before any decoder is built
(`psp_media_apply_demux_decoder_route` in `src/psp_media_open.c`). The
software decoder is required when any of these holds:

- the stream is High profile;
- the firmware would refuse a Baseline or Main stream: more than three
  reference frames, a declared B-frame pyramid (`max_num_reorder_frames`
  above 1), or a level above 3.0;
- the software decoder already owns the Media Engine.

Every other Baseline/Main stream within PSP bounds goes to the firmware
path. The software route admits only progressive 8-bit 4:2:0 pictures of at
most 432×240. When a firmware refusal is only a hint (a declared pyramid in
provider or offline media) and the software route cannot take the stream,
firmware stays the judge; a declared pyramid in page media is refused so the
page's next rendition is tried. Any other stream that neither route can take
fails admission with the refusal text. In-band parameter-set changes are
rejected unless they preserve the admitted configuration. WebM and
unsupported HLS forms fail as unsupported formats rather than being
misreported as decoder failures.

### Firmware path

The firmware backend uses the AVC raw-NAL bridge and the AAC-LC decoder. The
codec worker converts each access unit to its bounded length-prefixed input
and owns a single job stream, while two decoded-surface slots allow the Media
Engine to convert the next picture after the prior picture has been staged.

Each firmware surface follows an explicit ownership protocol:

```text
FREE -> ME_WRITING -> READY -> READING -> FREE
```

Slots carry session epochs and picture identities. Codec and DMA completions
carry the same tuple; stale completions after seek, reset, quarantine, or
teardown are discarded. A slot never returns to `FREE` until its real reader
has completed; a timed-out reader quarantines its surface rather than
returning memory to a writer. Video is staged in EDRAM and scaled by the GE.
Pixel signatures and canary rows make silent substitution and short writes
visible in validation builds.

The firmware 240p path uses a 512-pixel CSC stride. The 360p path keeps
firmware decode at its admitted geometry and uses the wide CSC/presentation
path to produce a 480x270 display image.

### Firmware-path presentation

On the firmware path, fullscreen video keeps the decoder's 8888 scanout rather than narrowing the whole picture to the page
renderer's RGB565 format. The native player projects
its overlay into at most 16 clipped, disjoint rectangles. Opaque title and
control bars start from cleared RGB565 scratch and so never import the moving
picture; only translucent badges, previews, and panels import their bounded
backdrop before the ordinary chrome compositor runs. The result is widened
only inside those rectangles, and every pixel outside them is the decoder's
original output, byte for byte. Fixed player shapes use precomputed coverage
from the same bounded antialiasing sampler as the generic path.

### Software path

H.264 High, and firmware-refused Baseline/Main streams, at up to 432×240 use
a replaceable, user-built software-decoder PRX. Official releases contain the
loader but no custom H.264 or AAC decoder binary. The component lives outside
the A/B slots, carries a small ABI record, and survives ordinary app updates;
if it is missing or incompatible, this route fails with an actionable player
message while firmware-compatible video keeps working. Building and installing
it is described in
[Development](../DEVELOPMENT.md#optional-psp-software-decoder).

The decoder keeps FFmpeg's allocations inside one admitted arena, runs CABAC,
deblocking, AAC, and most RGB565 conversion on the Media Engine, reconstructs
rows on the CPU, and publishes into a 25-slot generation-tagged ring. The
display clock slips forward instead of racing when a frame is more than 8 ms
late, quality rungs depend only on ring occupancy, and audio may not run more
than 80 ms ahead of the displayed picture. Once this path owns the Media
Engine, every later AVC or AAC route stays on it until the browser exits.
Suspend detaches the component and restores the firmware's ME state before
the power transition; resume reattaches it before the next software decode.

Every buffer shared with the Media Engine sits on an isolated 64-byte cache
line with tail padding, so range cache maintenance cannot touch Budget
metadata or a neighboring object. CPU-written AAC packets are written back
before submission, completed RGB surfaces are invalidated before the browser
reads them, and reordered pictures recover their presentation timestamps from
the source-AU mapping rather than the decoder's completion order. Each Annex-B
access unit revalidates any in-band SPS against the admitted geometry, the
decoder independently bounds the pictures it returns, and color conversion
receives the exact byte capacity of its destination slot. AAC output is
admitted only when its channel count fits the component's fixed two-plane
ABI. After a software takeover, later Baseline and Main streams must also fit
the 432×240 bound; larger streams fail admission instead of reaching an
incompatible backend.

## Buffering and player behavior

Startup buffering is enabled by default. Mid-stream starvation must persist
before the UI changes to `Buffering…`, which prevents momentary range-window
handoffs from flashing the player chrome. While buffering, audio and the
presentation clock are held; network and decode work continue. Playback
resumes only after both streams have a stable reserve. Repeated stalls request
a larger reserve within the same fixed windows.

The native player is provider-neutral:

- Cross toggles play and pause.
- Left and Right seek ten seconds; pointer input can seek on the progress bar.
- Scrubbing moves a target marker over the last displayed frame without
  touching the decoder or network. Cross commits one seek; playback resumes
  only after both split source heads have been prepared.
- Circle closes the native surface immediately from the user's perspective;
  bounded teardown continues through the session machine.
- Controls auto-hide during playback and are composited without sampling a
  mutable decoded surface after its lease ends.

## Media found in page data

Visible posterless videos inside same-origin HTTPS article links may obtain
the linked article's declared Open Graph/Twitter image during idle work.
This is an article preview, not a decoded video frame. The page permits two
lookups, each limited to 512 KiB of streamed head data, 4 KiB tags, and a
five-second overall job lifetime. Raw script/style text is discarded and no
linked document is installed or executed. Metadata requests omit credentials;
normal request policy, CSP, redirect rules, image limits and cancellation still
apply. Authored posters win, and hidden/offscreen videos are not looked up.
This path does not add a decoder or change native playback admission.

When a server-rendered `<video>` has no `src`, activating it may perform one
bounded, allocation-free scan of retained data scripts and generic media
attributes for direct MP4, HLS, or WebM references. Candidate selection
ignores hostnames, prefers the lowest direct MP4 quality at or above 240p, and
still passes URL resolution, CSP, request authority, and media probing. The
same retained-data seam recognizes schema-scoped `AudioObject` and
`MusicRecording.audio` sources, indexing at most twelve compact spans rather
than copying a JSON object graph. An otherwise inert Play or Preview button
may open the audio-only player only when one candidate exists, or when nearby
title or URL metadata identifies exactly one. Authored media elements,
JavaScript handlers, navigation, cancellation, and ambiguous matches always
take precedence.

## Host qualification

The release suite covers MP4 parsing, range semantics, playback timing,
promotion policy, DOM integration, PSP ownership, and the media state reducer:

```sh
ctest --test-dir build-preset-release -R \
  'tilefinch-(media|host-media|psp-media)'
```

Probe a local file through the portable demuxer:

```sh
build-preset-release/psp-browser-media-probe /path/to/input.mp4
```

The optional FFmpeg/SDL lab supplies a watchable host backend without becoming
a device oracle:

```sh
cmake --preset release -B build-media-lab \
  -DTILEFINCH_ALLOW_BUILD_DIR=ON -DPSP_BROWSER_BUILD_HOST_MEDIA_LAB=ON
cmake --build build-media-lab --target psp-browser-interactive-lab -j8
benchmarks/run-youtube-video-lab.sh \
  build-media-lab /path/to/input.mp4 /tmp/tilefinch-playback.mp4
```

Media captures and provider responses are local test inputs and are not
committed. Acquisition is explicit and reviewable; no test silently contacts
YouTube.

## PPSSPP and hardware qualification

Validation builds embed short 240p Baseline and 360p Main-profile AVC/AAC
fixtures. `validation_media_fixture_auto=1` exercises the real demux and PSP
backend without a network dependency. The fixtures can be regenerated only
with `tools/generate_psp_media_fixtures.sh`.

PPSSPP does not emulate the firmware raw-NAL Media Engine path completely. It can
qualify demux, audio, clocks, transport, UI, and lifecycle behavior, but an
AV-module/raw-NAL stop is `PARTIAL`, not a video-decode pass. Physical PSP
qualification must cover:

- changing, spatially nonuniform pixels at 240p and 360p;
- audio progress and bounded A/V skew;
- seek, close, reopen, and destroy/recreate;
- no ownership, signature, canary, DMA, or quarantine faults;
- real range-window progress over Wi-Fi;
- sustained playback at the source cadence;
- on the software path: a High-profile stream and a firmware-refused
  Baseline/Main stream both play through the software decoder;
- after a software takeover of the Media Engine, a later firmware-compatible
  stream stays on the software route, and one larger than 432×240 is refused
  at admission with the player's refusal message;
- suspend and resume during software playback: the component detaches and
  restores the firmware ME state, then reattaches before the next decode;
- a rewind of at least 30 seconds on each route reopens the backend and
  resumes at the target.

Use the zero-Memory-Stick PSPLink workflow in
[PSPLink device development](PSPLINK_DEV_LOOP.md) so instrumentation does not
perturb playback with storage I/O.

## Reading a soak log

Soak runs use the PSPLink loop and the boot keys in
[PSPLink device development](PSPLINK_DEV_LOOP.md#boot-overridescfg-reference-soak-shape).
Software-decoder diagnostics are in
[SWDEC_DIAGNOSTICS.md](SWDEC_DIAGNOSTICS.md).


- Lifecycle: `tilefinch-media-stability: event=started / event=seek /
  event=complete` (the 120s summary) or `event=failed reason=...`;
  `tilefinch-validation: outcome=` at exit. During playback, health and A/V
  skew are sampled into bounded RAM counters every 250ms without writing to
  host0. This is intentional: a USB-host write can pause the browser thread
  and manufacture the cadence hitch being measured.
- Stability verdicts: `pipeline-skips` are late, catch-up, prerequisite, or
  terminal pictures discarded before a presentation claim. `display-drops`
  are claimed identities that did not reach scanout, with `quiesce-drops` as
  the intentional-close subset. `max-av-skew` includes startup and seek
  discontinuities; `steady-max-av-skew` includes only samples after two
  uninterrupted seconds in `Playing`, and `steady-skew-samples` proves that
  bucket was actually observed.
- Cadence + pipeline: `tilefinch-media-feed:`,
  `tilefinch-media-transport:`, `tilefinch-media-present:`, and
  `tilefinch-media-video-cadence:` lines are emitted once at teardown; their
  fields are cumulative over the complete run. `tilefinch-video-scanout:` is
  the bounded histogram of wall intervals between distinct decoded pictures
  that actually reached scanout; repeated chrome-only presentation of one
  picture is excluded. The records are separate so every one fits the PSP
  logger's bounded line buffer. Per-picture ring traces:
  `tilefinch-media-picture:` only in builds configured with
  `TILEFINCH_PSP_MEDIA_PICTURE_TRACE=ON`.
- Occupancy: `tilefinch-media-slots:` and `tilefinch-media-worker:`, printed
  from the same teardown snapshot with the same `phase=` word, so the lines
  read together.
  These are **durations**, where the cadence line is counts: per-slot dwell in
  FREE/ME_WRITING/READY/READING, `no-free` vs `free-idle` (a writable slot
  with nothing being written — the direct measure of the worker not running
  ahead), the batch fill, the CSC itself, which frame phase each conversion
  landed in, and the worker's dispatch/prologue/firmware/collect ladder
  either side of the one shared job slot. Read the guide in the comment above
  `psp_media_report_slots` in `src/psp_media_telemetry.c` before drawing a
  conclusion from any single field.
- Never-taken paths, which should read zero: `dma-quarantine=` and
  `surface-quarantines=` on the `tilefinch-media-present:` line, plus
  `tilefinch-media-present: event=dma-quarantine*`. A quarantine without a
  matching late completion is a staging transfer that never came back, and
  the session that posted it ends on it deliberately, once
  (`event=dma-quarantine-expired ... owned=1`). Retry and later sessions play
  without the staging copy until the transfer is seen to end.
- Firmware events: `tilefinch-media-decoder: event=...` records setup,
  refusals, recoveries, and failures. Steady job timing is aggregated in
  `tilefinch-media-worker:` rather than logged per job; watchdog failures name
  the exact stalled stage.
- The backend stats reader refuses while a codec job runs — a sample line
  showing `decoded=0 audio-packets=0` mid-playback is that artifact, not a
  stall.

### Transport scheduling attribution

The transport worker normally runs one priority step above the browser so its
bounded service and 10 ms poll cannot be starved. Connection setup is a
different workload: DNS, TCP, and TLS can remain inside one
`curl_multi_perform` call long enough to delay input and media collection.
Until response headers arrive, the worker therefore runs below the browser;
steady body transfer restores the ordinary higher priority.

Validation builds report setup and steady perform distributions as
`tilefinch-background-transport-worker*`. Read those beside the browser frame,
presentation, codec-collection, and input-latency histograms. The intended
signature is:

- setup calls may be long but do not create matching browser-thread stalls;
- steady body performs remain bounded closely enough to feed range windows;
- network work does not starve codec collection or input dispatch;
- a lifecycle run can quiesce, destroy, revalidate, reopen, and resume without
  transport, ownership, signature, or quarantine faults.

Run both an instrumented PRX and an uninstrumented release PRX when changing
this priority policy. The latter proves that validation logging is not shaping
the cadence being measured.

## Provider contingency

If ordinary direct media URLs cease to be available, an optional
user-configured proxy provider may be added behind the same resolver seam. It
must be explicit, HTTPS-only, credential-free, independently disableable, and
subject to the same origin, length, redirect, and range-response checks. No
proxy is enabled by default.
