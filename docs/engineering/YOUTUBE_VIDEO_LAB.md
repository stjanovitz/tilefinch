# YouTube and media lab

Tilefinch treats YouTube as a bounded site adapter, not as a special case in
the HTML, CSS, layout, or paint engines. The adapter turns public mobile
YouTube responses into a small script-free document, and the native player
turns a supported watch URL into ordinary MP4 range sources. This document
defines those seams and the tests that qualify them.

## End-to-end shape

```text
mobile URL
  -> bounded provider fetch and parser
  -> ordinary Tilefinch HTML page
  -> watch activation
  -> bounded player-response resolver
  -> progressive MP4, separate AVC/AAC MP4 sources, or AAC alone
  -> HTTP range reader
  -> portable MP4 demuxer
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
once per new YouTube route so changing Options cannot mutate a live pipeline.

## Provider document

Public home, search, channel, and supported watch routes use the registered
site-adapter boundary. Account, Shorts, Studio, and unsupported routes remain
ordinary browser navigations.

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
| Search results | 12 |
| Watch recommendations | 6 |
| Full description | 8 KiB |
| Comments response | 256 KiB |
| Rendered comments | 8 |

Provider work runs through the ordinary one-slot fetch scheduler. Parsing and
HTML generation advance in bounded slices, remain cancellable, and preserve
the incumbent page until the replacement commits. A validated mobile API
identity occupies one fixed 1.4 KiB session slot for at most 30 minutes and is
invalidated when the cookie jar changes.

## Resolver and admission policy

The resolver calls a fixed, signed table of client profiles. It parses
`playabilityStatus` before format selection so age, login, region, live, and
premiere restrictions produce useful errors rather than a generic malformed
inventory message.

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
and AAC MP4 streams. The shipping preference is 360p; users can select the
smaller 240p path, and the PSP session can retry once at 240p after a
wide-profile admission or decoder failure. HE-AAC is rejected because the PSP
audio path is dimensioned for AAC-LC.

Provider-resolved URLs must remain on HTTPS port 443 at `googlevideo.com` or
one of its subdomains. Redirects are disabled for those sources. Expiring URLs
are rejected before playback when their remaining lifetime is inadequate;
one bounded mid-playback refresh may re-resolve and seek to the current
position after an expiry-related failure.

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

## PSP decode and presentation

The PSP backend uses the firmware AVC raw-NAL bridge and AAC-LC decoder. The
codec worker owns a single job stream, while two decoded-surface slots allow
the Media Engine to convert the next picture after the prior picture has been
staged.

Each surface follows an explicit ownership protocol:

```text
FREE -> ME_WRITING -> READY -> READING -> FREE
```

Slots carry session epochs and picture identities. Codec and DMA completions
carry the same tuple; stale completions after seek, reset, quarantine, or
teardown are discarded. A slot never returns to `FREE` until its real reader
has completed. Pixel signatures and canary rows make silent substitution and
short writes visible in validation builds.

The 240p path uses a 512-pixel CSC stride. The 360p path keeps firmware decode
at its admitted geometry and uses the wide CSC/presentation path to produce a
480x270 display image. Both paths use the same player and ownership model.

The media-session state machine owns opening, priming, playing, paused,
buffering, seeking, recovery, and quiescing. See
[PSP media session state](PSP_MEDIA_SESSION_STATE.md). It is the only
authority that may decide when a pipeline can be replaced or destroyed.

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
cmake -S . -B build-media-lab -DPSP_BROWSER_BUILD_HOST_MEDIA_LAB=ON
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

PPSSPP does not emulate the raw-NAL Media Engine path completely. It can
qualify demux, audio, clocks, transport, UI, and lifecycle behavior, but an
AV-module/raw-NAL stop is `PARTIAL`, not a video-decode pass. Physical PSP
qualification must cover:

- changing, spatially nonuniform pixels at 240p and 360p;
- audio progress and bounded A/V skew;
- seek, close, reopen, and destroy/recreate;
- no ownership, signature, canary, DMA, or quarantine faults;
- real range-window progress over Wi-Fi;
- sustained playback at the source cadence.

Use the zero-Memory-Stick PSPLink workflow in
[PSPLink device development](PSPLINK_DEV_LOOP.md) so instrumentation does not
perturb playback with storage I/O.

## Provider contingency

If ordinary direct media URLs cease to be available, an optional
user-configured proxy provider may be added behind the same resolver seam. It
must be explicit, HTTPS-only, credential-free, independently disableable, and
subject to the same origin, length, redirect, and range-response checks. No
proxy is enabled by default.
