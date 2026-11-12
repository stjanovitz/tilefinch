# Performance journeys

## Host continuity and interruption gates

`tilefinch-search-video-journey-tests` exercises Search → scroll → Details →
Comments → native-player handoff → seek/close intents → Back. It checks the
exact selected video, expanded-section visibility, restored result focus and
scroll, and an edited search value. Transactions use bounded synthetic response
fixtures, including a transport timeout, cancellation and a successful retry;
failures must retain the incumbent page and expose a nonempty cause. The
history, control values and provider reuse remain in RAM. No persistent
cache or playback-position store is configured. The native-player part checks
real control receivers, not decoding: the offline hardware journey remains the
decode/seek gate.

The browser-engine interruption lane measures **request → changed pixels** for
Select, cursor, Triangle, scroll and focus during font publication, image work,
section layout and a running script. It also starts with queued page input:
native presentation must not wait behind it, and opening a menu discards older
page presses rather than replaying them into menu rows. Focus/scroll wait until
the document borrow ends. Host timings are diagnostics, not device estimates;
the gates assert admission and actual changed pixels without flaky time limits.

Targeted checks:

```sh
build-preset-release/tilefinch-browser-engine-tests --search-video-journey-only
build-preset-release/tilefinch-browser-engine-tests --background-interruption-only
build-preset-release/tilefinch-browser-engine-tests --text-memory-only
```

## Recorded performance journeys

A performance journey is one [scripted-input scenario](INPUT_SCRIPT_REFERENCE.md)
measured under PPSSPP with every page served from a recorded HTTP trace. The
emulated PSP runs at a fixed 111 MHz and the trace replaces the network, so a
journey's load, scroll and layout timings depend on the build rather than on
host scheduling or network timing. That makes them usable as ratchets, the way
[fidelity floors](../FIDELITY.md) are for visual output.

These are emulator measurements: they predict device behaviour but do not
replace it. At the fixed 111 MHz, PPSSPP is slower than a PSP-3000 on CPU-bound
phases; at PPSSPP's normal clock it is faster. A journey ratchet is a
regression gate, not a device timing.

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
  trace (pinned by SHA-256) and data seed. Optional columns:
  - `boot`: extra validation `boot.cfg` keys, separated by `;`.
  - `cpu_mhz`: a clock other than 111 MHz, for a page whose own wall-clock
    limits trip at 111 MHz.
  - `clock`: the PSP's local time when the run starts, `HH:MM`. The
    default is 12:00, and `host` keeps the host's clock. Night mode's
    Auto setting follows the local clock (dark from 19:00 to 07:00), so a
    journey would otherwise be day or night depending on when it ran. The
    runner gives PPSSPP a time zone that puts the local clock at that time
    (`TILEFINCH_PPSSPP_TZ`). `22:00` makes a night run.
  - `sheet_marks`: the marks whose stylesheet counters become `sheets.*`
    metrics.
  - `frames`: frame marks that must equal another journey's,
    `MARK=JOURNEY:MARK`, separated by `,`. The other journey must come
    earlier in the manifest.
- `tests/perf-baselines.tsv` holds one row per journey and metric: the value
  and its tolerance. **Every metric is lower-is-better.** A value above
  baseline plus tolerance fails the run.
- Metrics come from the validation log:
  - `load.<scope>.*`: first paint, fully loaded and preview reach
    (`tilefinch-load-experience`), plus presses that did not move the page
    during a load. A journey with several navigations numbers the repeats:
    the second `follow` load is `follow2`.
    - `first_present_ms` (`first-present=`) is the first preview frame,
      even one whose screen is blank, such as a masthead logo that is not
      drawn yet.
    - `first_content_ms` (`first-content=`) is the first time the screen
      the reader is looking at shows visible text or a decoded image.
      `first_text_ms` (`first-text=`) counts text only. Each is the load's
      end when the preview never showed it, and `never` (no metric) when
      the loaded page's screen does not show it either. The journeys
      recorded with `--record` since these fields existed have baseline
      rows for them; the older ones do not.
    - `reading_hint_ms` (`reading-hint=`) is when the "preview ready" hint
      showed, when it did.
  - `scroll.*`: press-to-first-visible and press-to-complete-frame, at p50,
    p95 and max (`tilefinch-scroll-feedback`).
    - Each press is timed from when it was made. That includes a press the
      supervisor queued during page work and replayed afterwards.
    - A press that a later one replaced before its frame completed is
      reported too (`superseded=1`), when its replacement completes.
    - A press that cannot move the page (at its edge, or on a page that
      does not scroll) owes no frame and draws none. It is reported at
      once (`noop=1`), timed from the press to that report. These stay out
      of the percentiles above; their worst time is `scroll.noop_max_ms`.
    - A press made during a load is drawn first on the load preview
      (`action=preview`). That frame is not the press's complete frame, so
      it has its own `scroll.preview_first_visible_*` p50, p95 and max.
    - A feedback line of any other shape fails that journey's run instead
      of dropping out of a percentile; `tests/test_perf_journey_metrics.py`
      pins every shape the app prints.
  - `layout.*`: steps and total time building a provisional page's remaining
    layout (`tilefinch-layout-completion`).
  - `ui.*`: frame composition cadence at exit.
  - `control.*`: activation latency.
  - `relayout.prepare_ms` and `relayout.build_ms`: total time of the
    relayouts after a load (`tilefinch-relayout-phases`). Prepare is the
    style side: sheet appends or a whole-sheet rebuild, and the style and
    image passes. Build is the layout pass.
  - `memory.peak_kib`: the session's budget peak (`tilefinch-budget`
    at exit).
  - `sheets.MARK.*`: where the page's stylesheet came from at each mark
    named in `sheet_marks` (`tilefinch-input-stylesheets`).
    - `sheet_compiles`: the session's loads so far that compiled their
      sheet from scratch. A load served from the compiled-sheet cache,
      whole or cut back to its `<head>`, does not count.
    - `unrestored`: 1 when this mark's sheet was not restored from the
      cache.
    - `head_takes`: loads served by a cached sheet cut back to its
      `<head>`. A journey where none may be taken (across a Night mode
      change) pins it at 0; a take that should happen and does not shows
      in `sheet_compiles`.
    - `head_refusals`: cached sheets the cache refused to cut back to
      their `<head>`.
    - `full_relayouts`: whole-sheet rebuilds so far.
  - `frame.MARK.differs`: 1 when the mark's frame capture differs from the
    one named in `frames`.
  The `sheets.*` and `frame.*` values repeat exactly under replay and have
  no tolerance.

## Baselines only get lower

`--ratchet` lowers every baseline this run beat by more than its tolerance
and never raises one. A regression is fixed. If a slower number is the right
trade, raise that row by hand in the same commit, and give the reason in the
commit message. This is the same rule the fidelity floors follow.
`--record` rewrites the baselines of the journeys it ran. Use it for a new
journey, or for a deliberate reset explained in its commit message.

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
--commands FILE`), but its `tick` command does not run the deferred image
queue, so a capture driven only by `tick` lacks those images. `pump` and
`drain` run the queue (see [LAB_USAGE.md](LAB_USAGE.md)).

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

The traces are private and are not in any clone. The maintainers' source
copies sit outside the repository, in the workspace's `work/` tree; copy one
into `perf/traces/NAME` (or prepare it there) and the manifest's SHA-256
checks it.

| Trace | Journeys | Source copy (workspace `work/`) | What it holds |
| --- | --- | --- | --- |
| `wikipedia-article` | `article-*` | lost; no copy matches its pin | a lab capture of a main page, a search and one long article |
| `wikipedia-article-psp` | `press-during-preview` | `review-1109/perf-1110/wiki-archive-data/traces/` | a PSP capture of the PlayStation Portable article and its first-screen images |
| `wiki-abc` | `wiki-revisit*` | `merge-tools/traces/` | the same article URL served three times, as three articles that share their `<head>` stylesheets |
| `ia-search-thumbs` | `ia-search` | `review-1109/perf-1110/wiki-archive-data/traces/` | Internet Archive search results for "psp", with stand-in thumbnails |
| `bbc-home`, `www-reddit` | `bbc-home-load*`, `www-reddit-load` | `census/corpus/NAME/capture`, prepared with `--prepare-trace` | census captures (2026-10-06) |
| `youtube-bbb` | `youtube-bbb` | the main checkout's `perf/traces/` | see below |
| `chatgpt-ask-send76` | `chatgpt-ask` | the main checkout's `build-preset-psp-validation/census-replay-trace` | a PSP capture of the chatgpt.com journey |

The `article-*` journeys are skipped until their trace is re-captured and
re-pinned. A new trace is a new baseline (`--record`), not a ratchet.

The `wiki-revisit*` journeys reload one URL; with keyed replay each reload
is served the next of the trace's three document records. A fourth load of
the URL finds none.
- `wiki-revisit` pins the third article's load from the `<head>` sheet of
  the first (`sheets.c-settled.head_takes` 1, `sheet_compiles` 2).
- `wiki-revisit-dark` runs at 22:00, so all three articles load dark.
- `wiki-revisit-night` turns Night mode to Dark through the menu after the
  first article. The only cached sheet is then the first article's light
  one, and the compiled-sheet keys carry night mode, so
  `sheets.c-settled.head_takes` is pinned at 0. Without the key, the
  third article takes that light `<head>`. Its frames must equal
  `wiki-revisit-dark`'s. On this trace the frames match even with a light
  sheet: Wikipedia gates its dark rules on a class its scripts set, and
  with JavaScript off only the forced-dark transform applies.

`bbc-home-load` and `bbc-home-load-night` are the same load at 12:00 and
22:00. `www-reddit-load`'s initial load is the site's challenge page; the
page itself is the `follow` load. `ia-search` runs the page's scripts at
111 MHz and pins its whole-sheet rebuilds (`sheets.p3.full_relayouts`).
`press-during-preview` is `scroll-during-load-live` on the PSP capture:
R presses every 0.6 s through a reload, drawn first on the load preview.

The `youtube-bbb` trace is a PSP capture of the `youtube-bbb` scenario. It has
19 records:
- the provider's results for the scenario's query;
- the first result's watch page;
- fourteen result thumbnails, the watch page's large thumbnail and one
  channel avatar;
- a second copy of the results page, because the live session re-fetched the
  list after Circle.

Replay restores the list from history instead, and asks for four of the first
list's thumbnails that the capture never saw. They fail the same way on
every run. The list restored by Back (`load.follow2`) takes its sheet from
the compiled-sheet cache and compiles none: the journey pins
`sheets.returned.unrestored` at 0 and `sheet_compiles` at 2 (the results
and watch pages). Playback is not part of it: PPSSPP stops at the firmware decoder
boundary, so video is measured on hardware.

## Adding a journey

1. Write or pick a scenario in `tests/input-scripts/`. Journeys run it with
   `--measure`, so it needs no golden; a golden that exists is still enforced.
2. Add a manifest row.
3. Run `run-perf-journeys.py --journey NAME --runs 2 --record`. The runner
   fails if the two runs' traces differ, which catches a scenario that
   depends on something other than the build. Before committing, run the
   journey three separate times and check that every recorded value
   repeats; `--from-artifacts --record` then records the kept run without
   another emulator pass.
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
the device loop with `url=` set to a provider watch URL for a video that has
captions and several audio tracks.
