#!/usr/bin/env python3
"""Estimate I-cache set pressure of a sampled hot set under two link layouts.

  tools/icache_conflicts.py --profile-elf ELF samples.tsv [...] \\
      [--timeline-log LOG | --bench-log LOG] LAYOUT_ELF [LAYOUT_ELF ...]

Samples (tools/ppsspp_pc_sampler.py) are symbolized against --profile-elf,
the link they were taken from, as (function, offset). Each LAYOUT_ELF (same
objects, another link order) places those functions elsewhere; a sampled
pc and the 64-byte line after it are taken as hot lines. For every window
the script reports the hot lines, how many of the 128 sets of one 8 KiB way
they occupy, and the "overflow": the sample weight of lines that rank third
or lower in their set, which a 2-way cache cannot hold alongside the two
hottest. Overflow is a static proxy for conflict misses (it ignores timing
and capacity), for comparing layouts, not a miss rate.
"""
import argparse
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pc_profile_report as ppr  # noqa: E402

LINE = 64
SETS = 128


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--profile-elf", required=True)
    ap.add_argument("--nm", default="psp-nm")
    ap.add_argument("--objdump", default="psp-objdump")
    ap.add_argument("--timeline-log")
    ap.add_argument("--bench-log")
    ap.add_argument("--mhz", type=float, default=333.0)
    ap.add_argument("--min-window", type=int, default=30)
    ap.add_argument("inputs", nargs="+")
    args = ap.parse_args()
    samples = [p for p in args.inputs if p.endswith(".tsv")]
    layouts = [p for p in args.inputs if not p.endswith(".tsv")]

    psyms = ppr.load_symbols(args.profile_elf, args.nm)
    psym = ppr.Symbolizer(psyms)
    paddr = {n: a for a, _, n in psyms}
    code = ppr.Code(args.profile_elf, args.objdump)
    jedges = ppr.timeline_edges(args.timeline_log) if args.timeline_log else None
    bedges = ppr.bench_edges(args.bench_log) if args.bench_log else None

    def window_of(seconds):
        if jedges:
            name = jedges[0][0]
            for n, at in jedges:
                if seconds >= at:
                    name = n
            return name
        if bedges:
            for n, lo, hi in bedges:
                if lo < seconds <= hi:
                    return n
            return None
        return "all"

    hits = collections.defaultdict(collections.Counter)  # window -> (fn, off)
    base = {}
    for path, row in ppr.read_samples(samples):
        try:
            pc = int(row["pc"], 16)
            ticks = int(row.get("ticks", "0") or "0")
        except ValueError:
            continue
        base.setdefault(path, ticks)
        fn = psym.lookup(pc)
        if fn is None or fn == "psp_validation_display_wait_vblank":
            continue
        if code.syscall_return(pc, psym.bounds):
            continue
        win = window_of((ticks - base[path]) / (args.mhz * 1e6))
        if win is None:
            continue
        key = (fn, pc - paddr[fn])
        hits[win][key] += 1
        if win != "all":
            hits["all"][key] += 1

    layout_addr = []
    for elf in layouts:
        syms = ppr.load_symbols(elf, args.nm)
        layout_addr.append({n: a for a, _, n in syms})

    print("%-28s %6s %6s" % ("window", "samples", "lines")
          + "".join("  %-26s" % os.path.basename(e)[:26] for e in layouts))
    for win, c in hits.items():
        total = sum(c.values())
        if total < args.min_window:
            continue
        cols = []
        for addr_of in layout_addr:
            lines = collections.Counter()
            for (fn, off), n in c.items():
                if fn not in addr_of:
                    continue
                a = addr_of[fn] + off
                lines[a // LINE] += n
                lines[a // LINE + 1] += n / 2.0
            per_set = collections.defaultdict(list)
            for line, w in lines.items():
                per_set[line % SETS].append(w)
            overflow = 0.0
            for ws in per_set.values():
                ws.sort(reverse=True)
                overflow += sum(ws[2:])
            weight = sum(lines.values())
            cols.append("sets %3d over %5.1f%%" % (
                len(per_set), 100.0 * overflow / weight if weight else 0))
        nlines = len({(fn, off // LINE) for fn, off in c})
        print("%-28s %6d %6d" % (win[:28], total, nlines)
              + "".join("  %-26s" % col for col in cols))


if __name__ == "__main__":
    main()
