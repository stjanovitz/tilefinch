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
      the loaded page's screen does not show it either.
    - `reading_hint_ms` (`reading-hint=`) is when the "preview ready" hint
      showed, when it did.
  - `scroll.*`: press-to-first-visible and press-to-complete-frame, at p50,
    p95 and max (`tilefinch-scroll-feedback`).
    - Each press is timed from when it was made. That includes a press the
      supervisor queued during page work and replayed afterwards.
    - A press that a later one replaced before its frame completed is
      reported too (`superseded=1`), when its replacement completes.
      `visible-source=press` means that press had its own recorded first
      frame; `visible-source=replacement` means the delay is to the
      replacement's first frame instead. Both feed the historical
      `scroll.first_visible_*` and `scroll.complete_*` metrics unchanged.
      `scroll.direct_first_visible_*` and
      `scroll.replacement_first_visible_*` report the two kinds separately.
      Old superseded records without the source count only in the historical
      metrics because they don't distinguish the two kinds.
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
Every commit that moves a baseline lists its rows old → new in its message,
so `git log -p tests/perf-baselines.tsv` is each row's history.
`--record` rewrites the baselines of the journeys it ran. Use it for a new
journey, or for a deliberate reset explained in its commit message.
A metric with no baseline row is printed (`no baseline`) but not gated, so a
new metric gates a journey only once it has a row: `--record-missing PREFIX`
adds rows for the metrics starting with PREFIX and compares the rest as usual.

Tolerances are set when a baseline is recorded. Load and layout times repeat
exactly under trace replay, so they get 2-5%. Frame composition and latency
tails move a few percent with emulated-thread interleaving, so they get
10-15%, with a small absolute floor.

A few scroll tails repeat exactly for one build but move by whole frames
whenever unrelated code shifts the emulated threads' interleaving: every
`scroll.*` row of `article-scroll-during-load` and `www-reddit-load`, and
`press-during-preview`'s `scroll.complete_p95_ms`. Those rows keep their
value and carry a wider tolerance instead: the largest value measured on any
build from 0.1.53 to the re-baseline, less the baseline, plus one 60 Hz frame
(16.7 ms). A rise inside that band is the known spread; one past it is a
regression like any other.

The journeys run offline (see
[the PPSSPP runner](INPUT_SCRIPT_REFERENCE.md#ppsspp)): every run records
PPSSPP's `-d` log for the network check. That log does not change a
journey's metrics, which repeat exactly with or without it.

### Rows set by hand

The last reset set these rows by hand. Each had drifted above its limit over
several releases with no single change to fix, so the value is the one the
validation build of `main` measures, twice and identically, and the
tolerance follows the recording rule. The scroll tails above keep their
value and widen their tolerance. `ia-search`'s `sheets.top.full_relayouts`
is an exact count: its second whole-sheet rebuild is part of the
baseline, so a third fails.

| Journey | Metric | Old value | Old tolerance | New value | New tolerance |
|---|---|---|---|---|---|
| `article-scroll-during-load` | `scroll.complete_max_ms` | 560.8 | 56.1 | 560.8 | 340.1 |
| `article-scroll-during-load` | `scroll.complete_p95_ms` | 186.4 | 18.6 | 186.4 | 165.5 |
| `article-scroll-during-load` | `scroll.first_visible_max_ms` | 410.7 | 41.1 | 410.7 | 173.2 |
| `bbc-home-load` | `load.initial.first_content_ms` | 2793 | 55.9 | 2926 | 58.5 |
| `bbc-home-load` | `load.initial.first_present_ms` | 2529 | 50.6 | 2663 | 53.3 |
| `bbc-home-load` | `load.initial.preview_reach_ms` | 3631 | 72.6 | 3784 | 75.7 |
| `bbc-home-load` | `load.initial.reading_hint_ms` | 2529 | 50.6 | 2663 | 53.3 |
| `bbc-home-load-night` | `load.initial.first_content_ms` | 2876 | 57.5 | 2993 | 59.9 |
| `bbc-home-load-night` | `load.initial.first_present_ms` | 2608 | 52.2 | 2735 | 54.7 |
| `bbc-home-load-night` | `load.initial.preview_reach_ms` | 3735 | 74.7 | 3856 | 77.1 |
| `chatgpt-ask` | `control.activation_max_ms` | 950.2 | 95 | 1141.8 | 114.2 |
| `ia-search` | `load.initial.first_content_ms` | 5087 | 101.7 | 5228 | 104.6 |
| `ia-search` | `load.initial.first_present_ms` | 4511 | 90.2 | 4873 | 97.5 |
| `ia-search` | `load.initial.loaded_ms` | 5087 | 101.7 | 5228 | 104.6 |
| `ia-search` | `load.initial.preview_reach_ms` | 4511 | 90.2 | 4873 | 97.5 |
| `ia-search` | `load.phase.script_ms` | 3875 | 77.5 | 4197 | 83.9 |
| `ia-search` | `scroll.complete_max_ms` | 166.8 | 16.7 | 450.6 | 45.1 |
| `ia-search` | `scroll.complete_p95_ms` | 166.8 | 16.7 | 450.6 | 45.1 |
| `ia-search` | `scroll.first_visible_p50_ms` | 33.4 | 5 | 50.2 | 5.0 |
| `ia-search` | `sheets.top.full_relayouts` | 1 | 0 | 2 | 0.0 |
| `press-during-preview` | `load.phase.parse_ms` | 894 | 20 | 919 | 20.0 |
| `press-during-preview` | `scroll.complete_p95_ms` | 152.3 | 15.2 | 152.3 | 88.3 |
| `press-during-preview` | `scroll.first_visible_p95_ms` | 35.5 | 5 | 47.8 | 5.0 |
| `press-during-preview` | `scroll.preview_first_visible_max_ms` | 24.8 | 5 | 32.7 | 5.0 |
| `press-during-preview` | `scroll.preview_first_visible_p50_ms` | 22.9 | 5 | 32.7 | 5.0 |
| `press-during-preview` | `scroll.preview_first_visible_p95_ms` | 24.8 | 5 | 32.7 | 5.0 |
| `wiki-revisit` | `load.phase.parse_ms` | 890 | 20 | 916 | 20.0 |
| `wiki-revisit-night` | `load.phase.parse_ms` | 890 | 20 | 916 | 20.0 |
| `www-reddit-load` | `load.initial.first_content_ms` | 1000 | 20 | 1145 | 22.9 |
| `www-reddit-load` | `load.initial.first_present_ms` | 862 | 20 | 1026 | 20.5 |
| `www-reddit-load` | `load.initial.loaded_ms` | 1000 | 20 | 1145 | 22.9 |
| `www-reddit-load` | `load.initial.preview_reach_ms` | 862 | 20 | 1026 | 20.5 |
| `www-reddit-load` | `load.phase.script_ms` | 66 | 20 | 101 | 20.0 |
| `www-reddit-load` | `scroll.complete_max_ms` | 50.1 | 5 | 50.1 | 83.4 |
| `www-reddit-load` | `scroll.complete_p50_ms` | 33.4 | 5 | 33.4 | 50.1 |
| `www-reddit-load` | `scroll.complete_p95_ms` | 50.1 | 5 | 50.1 | 83.4 |
| `www-reddit-load` | `scroll.first_visible_max_ms` | 50.1 | 5 | 50.1 | 33.4 |
| `www-reddit-load` | `scroll.first_visible_p50_ms` | 33.4 | 5 | 33.4 | 25.1 |
| `www-reddit-load` | `scroll.first_visible_p95_ms` | 50.1 | 5 | 50.1 | 33.4 |

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

The traces are private and are not in any clone. Maintainers keep the source
copies outside the repository, in their private trace archive; copy one into
`perf/traces/NAME` (or prepare it there) and the manifest's SHA-256 checks it.
The census traces start from the site census corpus
(`CENSUS/corpus/NAME/capture`, see
[tools/site-census](../../tools/site-census/README.md)) and are prepared with
`--prepare-trace`.

| Trace | Journeys | What it holds |
| --- | --- | --- |
| `wikipedia-article` | `article-*` | a PSP capture of `scroll-during-load-live` from the search URL: the article, its reload, its stylesheets, icons and the images the scrolled screens asked for |
| `wikipedia-article-psp` | `press-during-preview` | a PSP capture of the PlayStation Portable article and its first-screen images |
| `wiki-abc` | `wiki-revisit*` | the same article URL served three times, as three articles that share their `<head>` stylesheets |
| `ia-search-thumbs` | `ia-search` | Internet Archive search results for "psp", with stand-in thumbnails |
| `bbc-home`, `www-reddit` | `bbc-home-load*`, `www-reddit-load` | census captures |
| `youtube-bbb` | `youtube-bbb` | see below |
| `chatgpt-ask-send76` | `chatgpt-ask` | a PSP capture of the chatgpt.com journey |

Re-capturing a trace is a new baseline (`--record`), not a ratchet.

The `wiki-revisit*` journeys reload one URL; with keyed replay each reload
is served the next of the trace's three document records. A fourth load of
the URL finds none.
- `wiki-revisit` pins the third article's load from the `<head>` sheet of
  the first (`sheets.c-settled.head_takes` 1, `sheet_compiles` 2).
- `wiki-revisit-dark` runs at 22:00, so all three articles load dark.
- `wiki-revisit-night` turns Night mode to Dark through the menu after the
  first article, and its frames must equal `wiki-revisit-dark`'s. The only
  cached sheet is then the first article's light one. Compiled-sheet keys
  carry night mode, so `sheets.c-settled.head_takes` is pinned at 0; a key
  without it would let the third article take that light `<head>`. The pin,
  not the frame comparison, guards the key: on this trace the frames match
  even with a light sheet, because the article gates its dark rules on a
  class its scripts set, and with JavaScript off only the forced-dark
  transform applies.

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
and watch pages). Playback is not part of it: PPSSPP stops at the firmware
decoder boundary, so video is measured on hardware.

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
  three reference frames, because the PSP-3000 Media Engine refuses the
  four-reference stream libx264 writes by default (the generator's header
  has the details). Encoders are not byte-reproducible across versions, so
  the generator prints the digests.
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
