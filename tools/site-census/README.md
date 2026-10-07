# Site census and replay harness

Tools for the popular-site compatibility census and for measuring a branch
against a fixed, offline corpus of those sites. Captured pages are private
third-party data: the corpus lives outside the repository (by default
`../census/` next to the worktree, override with `CENSUS_DIR` or
`--census`) and is never committed.

| File | Purpose |
| --- | --- |
| `sites.tsv` | The page list: name, tags, URL. Tags select subsets. |
| `census.py` | Driver: `capture`, `replay`, `extract`, `sheet`. |
| `replay.sh OUT [opts] [SELECTOR...]` | Parallel offline replay of the corpus (wraps `census.py replay`; `LAB=` selects another build's lab, `CORPUS=` another corpus generation such as `../census/corpus-v2`, `SITES=` a page list other than `sites.tsv`). |
| `extract.py RUN_DIR` | One lab log to one JSON record. |
| `compare.py BASE.jsonl NEW.jsonl` | Per-metric and per-page deltas; also the noise check. |
| `report.py METRICS.jsonl` | Markdown tables for the census report. |
| `factory_cache.py pages\|pairs RUN_DIR ...` | Lazy webpack bundle costs per page and a two-visit session simulation of caching compiled factories, from a replay run with `TILEFINCH_TRACE_FACTORY_CACHE=1` (see `docs/DIAGNOSTIC_SWITCHES.md`). |
| `classify.py` | Hand-maintained script classifier (first-party, framework, analytics, ads, tag manager, consent, A/B testing, chat, social). |
| `api-probe.js` | Lab-only missing-API probe, loaded by `--probe` runs. |
| `sheet.py` | 480x272 screenshot grids without Pillow. |
| `reader-labels.tsv`, `reader_eval.py` | Hand labels (article, listing, app, other) and automatic Reader precision and recall for one or more `census.jsonl` runs. |

## What a run records

Every page is loaded by `psp-browser-interactive-lab` with the shipping
defaults: Tilefinch's honest default UA, `--psp-profile realistic` (the PSP
app's memory, script and document caps), basic content blocking, cookie
notices hidden (never clicked), 480x272, a seeded clock and random source
(`--deterministic-replay-seed 1`). The command script ticks the page for 150
x 33 ms, renders the top and two scrolled screens, runs the `census` lab
command (first-paint timebase, off-screen layout share, decoded vs. painted
size of every image, Reader analysis), records a few page facts and prints
the script heap report.

There are four replay passes, each writing its own JSONL:

- the metrics pass (default): what the census and `compare.py` read. Each
  page runs `--repeat` times (default 3) and every timing is the minimum
  over the repeats; memory, work counts and errors come from the first run.
  A page whose first run takes over 20 s (the 60 s script watchdog) is not
  repeated.
- `--probe`, the diagnostic pass: adds the API probe, script-refusal tracing
  (`TILEFINCH_TRACE_SCRIPT_FAILURES`: which script hit the per-script cap,
  the total cap, the count cap or compile admission) and switches to Basic
  view at the end (emitted nodes, truncation). All three perturb the page or
  its timing, so they never run in the metrics pass. `report.py` merges the
  probe pass's Basic and refusal fields into the metrics records.
- `--revisit`: loads each page twice in one session (`--reload 1`); the
  script ledger and JS totals are for the second visit, which measures what
  the existing in-memory bytecode caches already save. The reload follows
  the first load at once, so the first visit never runs its ticks: async and
  dynamic scripts load only on the second visit and cannot hit a cache.
- `--revisit-ticked`: the first visit also runs the 150 ticks and up to 300
  owner idle turns (`idle 300`), then the page URL is loaded again
  (`go URL`) and measured. This is the closer model of a reader returning to
  a page; it takes about twice as long. The idle turns matter: the lab's
  `tick` advances only the page runtime, while the PSP loop also runs the
  engine's idle work whenever the page is quiet, and classic bytecode is
  stored there (a page left with stores still queued stores at most 256 KiB
  of them as it closes and drops the rest).
- `--restart`: a browser restart between the visits. The first visit
  (with its ticks and idle turns) runs in its own lab process with a fresh
  persistent compiled-script directory (`--script-cache-dir RUN/script-cache
  --script-cache-write`), which writes the tier at idle; the measured visit
  is a new process on the same directory. `lab.first.log` is the first
  process's log; `bytecode.py` reads it as visit 1. The record's
  `js.script_disk_*` fields come from the `javascript-script-cache-disk`
  line (disk hits, synchronous read time, packs and bytes read and written).

`--idle N` adds up to N owner idle turns to the measured visit, after its
ticks and before it is rendered (the lab's `idle` command, which stops at
the first turn that leaves no work). The default pass runs none, so the
idle work the PSP loop does between frames only appears in an `--idle`
pass: background image and font continuations, display retargeting
(`images.retarget`: decoded rasters reduced to their painted size after
layout, SVG markup rasterized again at it, counted in `rasters`) and the
classic-bytecode stores (`js.bytecode_cache_idle_store_us`,
`..._deferred`, `..._deferred_dropped`). Compare `--idle` passes only with
each other. The first visit of `--revisit-ticked` and `--restart` runs its
idle turns whatever `--idle` says.

`CENSUS_LAB_ARGS` appends lab options to every run (an experiment knob, for
example `CENSUS_LAB_ARGS="--classic-bytecode-cache-kb 2048"`).

`census-bytecode-*` and `census-visit` ledger lines classify every classic
external-script bytecode lookup (hit, no cached entry, ineligible, ...), each
store attempt and each dropped entry, split by visit.

Two lab-only switches feed the record. Both are compiled out of
`TILEFINCH_NO_TRACE` (PSP) builds and listed in `docs/DIAGNOSTIC_SWITCHES.md`:

- `TILEFINCH_TRACE_CENSUS=1`: a stdout ledger. `census-js` per page script
  compile and top-level execution (kind, bytes, microseconds, monotonic start
  time, name), `census-exception` per uncaught exception (message and
  throwing frame), `census-rejection` per promise rejected without a
  handler (`census-rejection-handled` when one is attached later), and
  `census-csp-refusal` per refused request, inline script/style, attribute
  or `eval`.
- `TILEFINCH_LAB_INIT_SCRIPT=FILE`: evaluate `FILE` in each top-level page
  realm after the bootstrap and before author code. `--probe` runs use it
  for `api-probe.js`, which splices transparent Proxy sentinels above the
  platform prototypes, the namespace objects and the global object and
  records every property a page reads or tests and does not find (for
  example `window.ResizeObserver`, `HTMLElement.animate`,
  `static:Promise.withResolvers`). The probe slows inherited lookups and
  turns a bare read of an undeclared global into `undefined`, so probe runs
  are a separate pass and never the timing pass.

`extract.py` turns the log into one JSON record per page (field names are
append-only): `timing` (first paint, loaded, relayouts, longest blocking
step), `memory` (peak and per-category peaks), `document`, `layout`
(retained bytes, share of draw commands below the first and second screen),
`images` (decoded bytes, savings from decoding at painted size, never-painted
and below-the-fold bytes), `styles`, `scripts` (loader counters),
`script_list` (per script: bytes, compile and execute time, start time,
whether it started before first paint, category), `js` (totals, pre-paint
and non-essential pre-paint time, cacheable compile time, heap peak), `errors`,
`limits` (document cap, script quota, memory pressure, stylesheet cap, DOM
handles, watchdog, compile cap, JS heap), `fallback` (Reader kind and
confidence, extraction size, Basic view nodes, visible text and truncation,
whether the page is blank), `work` (the deterministic `tilefinch-work` vector),
`api_probe` (probe runs) and `outcome_auto` (a first-pass outcome; the report
applies screenshot judgements on top).

## Capturing (live network)

```sh
python3 tools/site-census/census.py capture --retry            # every page
python3 tools/site-census/census.py capture --retry news       # by tag
```

One capture per page, at most one retry. Never solve challenges or log in: a
challenge or block is a finding, recorded as the page's outcome. The trace
lands in `CENSUS/corpus/NAME/capture`.

## Replaying a branch against the corpus

Build the branch's lab (optimized), then replay and compare with a baseline:

```sh
cmake --build build-preset-release --target psp-browser-interactive-lab -j8
tools/site-census/replay.sh ../census/runs/base                 # main's lab
LAB=/path/to/branch/build-preset-release/psp-browser-interactive-lab \
  tools/site-census/replay.sh ../census/runs/branch
python3 tools/site-census/compare.py ../census/runs/base/census.jsonl \
  ../census/runs/branch/census.jsonl --pages
```

`replay.sh OUT -j N news reference` replays a subset by name or tag. Replays
run in parallel (default: cores - 4, leaving the efficiency cores alone) and
print their wall time: the whole corpus takes about 60 s on an M-series Mac
(three repeats per page; one page sits in the 60 s script watchdog), the
probe pass about 70 s and the revisit pass about 30 s. For a branch that
changes Basic view or script admission, compare the `--probe` passes too.

`compare.py` groups metrics by planned change: A (JavaScript cost: compile,
execute, pre-paint and non-essential pre-paint time, heap), B (memory: peaks
by category, decoded images, retained layout), C (Reader and Basic
extraction), T (timings) and W (the deterministic work vector). It prints the
corpus totals, the per-page median and 90th-percentile absolute change, the
pages whose outcome, errors, limits or Reader classification changed, and
with `--pages` every metric that moved by more than `--threshold` percent.

## Noise

Replays are offline and seeded; memory, decoded images, retained layout,
Reader extraction and the deterministic work vector (`W` rows) repeat
exactly or within 1% between runs of one build. Timings are host wall time
and move with machine load. Measured on the October 2026 corpus (55 pages,
6 jobs, repeat 3), two runs of the same lab:

- quiet machine: corpus totals within 1% (relayout, first paint, loaded, JS
  compile and execute); per-page p90 absolute change 3-7%.
- typical (other agents building on the same Mac): totals within 2-11%,
  per-page median 2-3%, p90 13-26%.
- busy: totals up to 12% apart, per-page p90 up to 29%.

The first of the three repeats of a page is usually the slowest (cold
caches), which is why the minimum is taken. Basic view statistics are zero
in the metrics pass by design; compare `--probe` runs for change C.

So for a timing claim, run base and branch back to back (ABAB) on a quiet
machine, check the A/A pair first (`compare.py` of two base runs), and only
trust per-page deltas well above that pair's p90 column. Work counts and
memory need no such care.
