# Performance journeys

A performance journey is one [scripted-input scenario](INPUT_SCRIPT_HARNESS.md)
measured under PPSSPP with every page served from a recorded HTTP trace. The
emulated PSP runs at a fixed 111 MHz and the trace replaces the network, so a
journey's load, scroll and layout timings depend on the build rather than on
host scheduling or network timing. That makes them usable as ratchets, the way
[fidelity floors](../FIDELITY.md) are for visual output.

These are emulator measurements: they predict device behaviour but do not
replace it. At the fixed 111 MHz, PPSSPP is slower than a PSP-3000 on CPU-bound
phases. On the Wikipedia article, device times were about 0.56x the journey
numbers for page loads, 0.9x for layout completion, and 0.55x for frame
composition. Press latencies measured alike on both. (At PPSSPP's normal clock
the emulator is instead faster than the device.) Record device results in the
[performance ledger](PERFORMANCE_LEDGER.md).

## Running

```sh
python3 benchmarks/run-perf-journeys.py              # all journeys vs baselines
python3 benchmarks/run-perf-journeys.py --journey article-page-down-fast
python3 benchmarks/run-perf-journeys.py --ratchet     # lower beaten baselines
```

It needs the validation build (`build-preset-psp-validation`) and PPSSPP,
exactly as `scripts/run-ppsspp-input-script.sh` does. Each run's artifacts
are kept in `build-preset-psp-validation/perf-journeys-latest/<journey>/`.

- `benchmarks/perf-journeys.tsv` lists the journeys: the scenario, start URL,
  trace (pinned by SHA-256) and data seed.
- `tests/perf-baselines.tsv` holds one row per journey and metric: the value
  and its tolerance. **Every metric is lower-is-better.** A value above
  baseline plus tolerance fails the run.
- Metrics come from the validation log:
  - `load.<scope>.*`: first paint, fully loaded and preview reach
    (`tilefinch-load-experience`), plus presses that did not move the page
    during a load. A journey with several navigations numbers the repeats:
    the second `follow` load is `follow2`.
  - `scroll.*`: press-to-first-visible and press-to-complete-frame, at p50,
    p95 and max (`tilefinch-scroll-feedback`).
    - Each press is timed from when it was made. That includes a press the
      supervisor queued during page work and replayed afterwards.
    - A press that a later one replaced before its frame completed is
      reported too (`superseded=1`), when its replacement completes.
  - `layout.*`: steps and total time building a provisional page's remaining
    layout (`tilefinch-layout-completion`).
  - `ui.*`: frame composition cadence at exit.
  - `control.*`: activation latency.

## Baselines only get lower

`--ratchet` lowers every baseline this run beat by more than its tolerance
and never raises one. A regression is fixed. If a slower number is the right
trade, raise that row by hand in the same commit, and record the reason in the
performance ledger. This is the same rule the fidelity floors follow.
`--record` rewrites the baselines of the journeys it ran. Use it for a new
journey, or for a deliberate reset recorded in the ledger.

Tolerances are set when a baseline is recorded. Load and layout times repeat
exactly under trace replay, so they get 2-5%. Frame composition and latency
tails move a few percent with emulated-thread interleaving, so they get
10-15%, with a small absolute floor.

## Traces

Recorded traces are page content and are not committed. They live under
`perf/traces/` (git-ignored), and a journey whose trace is absent is skipped.
To make one, record the PSP's own live session under PPSSPP. The requests are
then exactly the ones replay will see, including images the lab does not load:

```sh
scripts/run-ppsspp-input-script.sh --build-dir build-preset-psp-validation \
    --script SCENARIO --url URL --measure --capture-trace /path/to/capture
python3 benchmarks/run-perf-journeys.py --prepare-trace /path/to/capture NAME
```

`--capture-trace` sets the validation-only boot key `trace_capture`. The lab can
also record a trace (`psp-browser-interactive-lab --url URL --capture-http DIR
--commands FILE`), but it never pumps deferred images, so its captures lack
them.

While a trace records or replays, the YouTube stream resolver and HLS stay off.
They use the background transport, which neither records nor replays, so a
journey never reaches the live network. Speculative pre-resolve reports
`start-refused`, and pressing play fails the resolve.

`--prepare-trace` copies the capture into the corpus. It then adds a record
under the final URL of every redirected document, so a reload of that page
finds its response. It prints the SHA-256 to pin in the manifest. The PSP
replays a journey trace with `trace_keyed=1`: responses are matched by method
and URL (as the lab's `--replay-http-response-keyed` does), not in strict
capture order, because a page's image and stylesheet requests race.

The `wikipedia-article` trace is a lab capture of the Main Page, a search, and
the PlayStation Portable article (955 KB), with its stylesheets.

The `youtube-bbb` trace is a PSP capture of the `youtube-bbb` scenario. It has
14 records:
- the provider's results for "big buck bunny" (the official Blender short,
  `aqz-KE-bpKQ`, is first);
- that watch page (672 KB of provider source);
- eleven thumbnails;
- a second copy of the results page, because the live session re-fetched the
  list after Circle.

Replay restores the list from history instead, and asks for four of the first
list's thumbnails that the capture never saw. They fail the same way on
every run. Playback is not part of it: PPSSPP stops at the firmware decoder
boundary, so video is measured on hardware.

## Adding a journey

1. Write or pick a scenario in `tests/input-scripts/`. Journeys run it with
   `--measure`, so it needs no golden; a golden that exists is still enforced.
2. Add a manifest row.
3. Run `run-perf-journeys.py --journey NAME --runs 2 --record`. The runner
   fails if the two runs' traces differ, which catches a scenario that
   depends on something other than the build.
4. Commit the manifest and baseline rows together.

## Offline YouTube journey (hardware)

Playback needs a real PSP, and live YouTube runs depend on the network. This
device journey plays a downloaded-style YouTube video with no network at all,
so player work can be repeated without it:

```sh
cmake --build build-preset-release --target tilefinch-offline-library-fixture
tools/make-offline-youtube-fixture.sh        # once; writes perf/fixtures/offline-youtube
scripts/run-psplink-offline-youtube.sh       # PSPLink loop; host0 only
```

- The fixture is a one-item offline library holding a synthetic 60 s clip
  (a frame counter over a test pattern, plus a tone). It is shaped like the
  split streams the resolver picks: 640x360 H.264 Main level 3.0 (itag 134)
  and AAC-LC 128 kbit/s (itag 140), both DASH-fragmented. It uses at most
  three reference frames. libx264's default preset writes a four-reference
  SPS, which the PSP-3000 Media Engine refuses on the first access unit
  with 0x80628002 (logged as `wide-program-rejected`). Encoders are not
  byte-reproducible across versions, so the generator prints the digests.
- The run starts at `https://tilefinch.local/offline`. Wi-Fi is deferred for
  an offline start URL. `tests/input-scripts/offline-youtube-live.txt` then:
  1. focuses Play and opens the video;
  2. previews and cancels while playing;
  3. commits a seek about 30 s ahead;
  4. rewinds to 0, far enough to rebuild the decoder, and presses Right,
     Right, Cross while it reopens;
  5. pauses, then previews and cancels while paused;
  6. closes the player.
- `scripts/verify-psplink-offline-youtube.py` checks each step, and that the
  run had no decoder failures, stalls or lifecycle mismatches.
- The runner moves aside `boot.cfg`, the profile, site data and any
  `offline/` directory on host0, and restores them afterwards. Nothing is
  written to the Memory Stick.

A live-network variant of the same flow is
`tests/input-scripts/youtube-seek-rewind-play.txt` (and
`youtube-subtitles-seek-play.txt` for the caption menu). Run either through
the device loop with `url=https://m.youtube.com/watch?v=Qtl8lJwbd4g`, which has
English captions and six dubbed audio tracks.
