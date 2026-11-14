# PSP code layout

Audience: engineers who compare PSP device timings between two builds,
change the PSP link, or try a link-order experiment.

## Why placement changes device timings

The Allegrex has a 16 KiB instruction cache and a 16 KiB data cache. Each is
two-way set associative with 64-byte lines, so each way is 8 KiB and an
address modulo 8 KiB picks its set. PSPSDK's `cpu_icache_inv_all` walks
`4096 << Config.IC` bytes per way in 64-byte steps and issues the index
operation twice per line, once for each way.

A page load's hot code is many times the size of the cache. Which hot lines
share a set decides how many conflict misses a journey takes. When code ahead
of a hot function grows or shrinks, that function moves to other sets. The
instructions run are the same, but the device time changes. PPSSPP has no
cache model, so its timings do not move. A placement-only change therefore
looks like a regression or a win on the device and like nothing under
PPSSPP.

Only a change in the middle of the link order changes which hot lines share
a set. Growing all of `.text` from its start moves every function by the
same amount, so every pair of hot lines keeps its relative sets; it changes
only lines that straddle a 64-byte boundary (for growth that is not a
multiple of 64) and where data and the heap land.

## Comparing device runs

Validation builds print `tilefinch-code-layout:` once at boot, after the
heap report. It gives the text base, the image end and the heap base, each
with its offset within an 8 KiB way (`text-set`, `heap-set`). Two runs place
code identically only if their `text-set` values match. PSPLink loads the
dev PRX at the same address in every run and the EBOOT always loads at the
same address, so a given build keeps its sets from run to run; the heap
starts right after the image.

Device runs that replay a trace over host0 also pay for reading the trace
from the Mac. That time grows when the Mac is busy and lands inside the
measured windows. The replay prints a cumulative `tilefinch-trace-replay-io:`
line with the time spent. Subtract it before comparing windows, and take
device timings while no build or emulator run loads the host.

Some journey windows are bimodal from run to run whatever the layout: the
same build lands in one of two times several seconds apart. Repeat each
variant at least twice, alternate the builds, and compare a window only when
its repeats agree.

A device A/B of a small change mixes the change with the placement shift it
causes. To separate them, add a placement control: a build that inserts the
same number of bytes at the same place in the link order without the change.
For a change in the middle of `.text`, that means a retained section of N
bytes in the changed source file, added for the measurement only:

```c
__asm__(".pushsection .text.tf_measure_pad,\"axR\",@progbits\n"
        ".space 1024\n.popsection");
```

`R` (`SHF_GNU_RETAIN`) keeps `--gc-sections` from dropping the unreferenced
section. Never commit such a pad. For a change to data only, an ordering file
with `.rodata : { . += N; }` (see below) moves data and the heap without
moving code.

## The hot-function order experiment

`PSP_BROWSER_PSP_HOT_ORDER` (off by default) links `cmake/PspHotOrder.ld`, a
GNU ld `--section-ordering-file`, into the PSP browser EBOOT and the PSPLink
dev PRX. Every PSP object is compiled with `-ffunction-sections`, so each
function is its own input section. The file places profiled hot functions
at the start of `.text`:

1. Tier 0: code Tilefinch does not edit, such as newlib, libgcc, the SDK,
   QuickJS, lexbor and the header-only vendored libraries (stb_image,
   stb_truetype and nanosvg, which compile into Tilefinch objects and are
   recognized by their `stbi_`, `stbtt_` and `nsvg` name prefixes).
2. `. = ALIGN(8192)`, so tier 1 starts at a fixed offset within a cache way.
3. Tier 1: Tilefinch's own hot functions.
4. `. = ALIGN(8192)`, so the rest of `.text` starts at a fixed offset within
   a cache way.
5. `__tilefinch_hot_order_end`, a symbol that marks the end of the ordered
   block for the post-link check.

Within a tier, functions that call each other are clustered (C3 call-chain
clustering over the static call graph, at most 16 KiB per cluster), and the
clusters are ordered hottest per byte first. A size change in a function
outside the list then moves no listed function, and a size change in a
tier-1 function moves only the tier-1 functions after it.

It is off because on a PSP the ordered layout ran the archive.org search
journey slower than the natural link order, with every padding control
tried, and it did not make timings less sensitive to code changes. An
earlier experiment that ordered the hot set by heat alone was rejected for
the same reason. The heat comes from PPSSPP program-counter samples, which
count instructions, not cache misses, and the hot set is many times the
I-cache, so ordering by heat reshuffles conflicts rather than removing them.
The private performance ledger has the measurements.

The order is link-only. Objects and instructions are unchanged, and so are
frame hashes and PPSSPP timings. With the option on, the two alignments
cost up to 16 KiB of `.text` and count toward the `.text` limits.

Each entry matches its function under any GCC clone suffix
(`.text.foo .text.foo.*` also matches `foo.constprop.0` and `foo.isra.0`),
because the shipping and validation builds clone different functions. GNU
ld silently ignores a pattern that matches nothing, so a renamed function,
a moved source file or a function that is now inlined everywhere would
quietly lose its place. Two checks catch that:

- With the option on, `tools/check_psp_hot_order.py` runs after each PSP
  link of the EBOOT and the dev PRX. It reads the ELF's symbols and fails
  the build when less than 95% of the listed heat that is present in the
  image lies before `__tilefinch_hot_order_end`, and it prints the hottest
  misses. Entries for functions absent from the image (validation-only code
  in a shipping link, for example) are reported and not charged.
- `tilefinch-psp-hot-order-tests` checks the file's shape and fails when a
  Tilefinch or QuickJS entry names a function that no longer exists in the
  sources.

### Regenerating the list

1. Build a validation tree with `-DPSP_BROWSER_PSP_LINK_MAP=ON`. The order
   may be on or off; the ELF and the map must come from the same link.
2. Profile under PPSSPP with `tools/ppsspp-pc-profile.sh` (see
   [Program-counter profiles under PPSSPP](VALIDATION_RECORDS.md#program-counter-profiles-under-ppsspp)):
   - three browsing journeys at 111 MHz: the `layout-reuse-study` input
     script on the archive.org search trace, on the Reddit census capture
     and on the CNN census capture;
   - two game workloads, because browsing journeys never run the game
     audio mixer or WebGL: a three.js page with lit materials at 111 MHz
     (the committed list used three.js r128's lambert-phong demo served
     from 127.0.0.1, with an input script that waits for the first frame
     and then 300 frames), and the `webaudio-program-probe` scenario at
     333 MHz.
3. Report each profile with
   `tools/pc_profile_report.py --elf BUILD/psp-browser-script.unstripped
   --mhz MHZ --json-out NAME.json NAME.tsv`.
4. Write the order:

   ```sh
   python3 tools/code_layout_order.py --stable --coverage 0.98 \
       --command "tools/code_layout_order.py --stable --coverage 0.98 (...)" \
       --elf BUILD/psp-browser-script.unstripped \
       --map BUILD/psp-browser-script.map \
       --pin game_audio_lane_render --pin game_audio_mix_active_voice \
       --pin game_audio_oscillator_step --pin game_audio_curve_value \
       --pin webgl_ge_lit_build \
       --out cmake/PspHotOrder.ld ia.json reddit.json cnn.json \
       three.json=0.5 audio.json=0.5
   ```

   Each profile is normalized to weight 1, or to the weight after `=`; the
   game profiles count half as much as a browsing journey. `--coverage
   0.98` keeps the functions that make up 98% of the combined heat (about
   320 functions, 540 KB); functions with fewer than three samples are
   dropped. `--pin` adds a function that the profiles under-sample but that
   runs on every audio block or lit draw; a pinned function gets the heat
   of the coldest listed one, so it clusters with its callers. `--command`
   records in the file header how the file was made, including which build
   and profiles it came from.
5. Run `ctest -R tilefinch-psp-hot-order-tests`, rebuild both PSP presets
   with the option on, and check frame hashes with PPSSPP journeys. Then
   time the new list on a PSP against the natural link order, each with
   placement controls, before turning it on anywhere.

## Other ordering files

`PSP_BROWSER_PSP_SECTION_ORDERING_FILE` links any other ordering file in
place of the hot order, and the configure step prints a warning. Never set
it in a shipping or baseline build. Uses:

- a control layout: `tools/code_layout_order.py --shuffle SEED` writes the
  same hot set in a seeded random order;
- a data placement control: `.text : { }` followed by
  `.rodata : { . += N; }` moves data and the heap by N bytes and leaves
  code where it is;
- a uniform shift: `. += N;` inside the `.text` block moves all code by N
  bytes (after the list, only unlisted code), which, as above, keeps every
  hot pair's relative sets.

`tools/icache_conflicts.py` compares layouts statically from a profile. It is
a proxy for conflict misses, not a miss rate.
