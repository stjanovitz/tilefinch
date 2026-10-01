#!/usr/bin/env python3
"""Symbolize program-counter samples from tools/ppsspp_pc_sampler.py.

  tools/pc_profile_report.py --elf ELF samples.tsv [more.tsv ...]
      [--timeline-log LOG | --bench-log LOG] [--top 40] [--json-out FILE]

ELF must carry symbols (psp-browser-script.unstripped, or the dev PRX's
psp-browser-script-dev.elf with --prx-base when the samples came from a PRX).
For every window it prints the functions by share of samples, the text bytes
needed to cover 50/80/90/95/99% of samples (the hot set against the 16 KiB,
2-way, 8 KiB-per-way Allegrex I-cache), and the hot functions' address
spread modulo 8 KiB. `--json-out` feeds tools/code_layout_order.py, which
writes the link order for PSP_BROWSER_PSP_SECTION_ORDERING_FILE.

Sample hygiene: PPSSPP services a debugger break at the next JIT block
boundary, so a break requested while the emulator runs an HLE system call
lands on the call's return site, weighted by host time. Samples there
(`jal` to a .sceStub.text stub, or to a function that tail-jumps to one),
in the HLE idle thread (outside .text) and in --drop-fn waits are dropped.
"""
import argparse
import bisect
import collections
import json
import os
import re
import subprocess
import sys

WAY = 8192


def load_symbols(elf, nm):
    out = subprocess.run([nm, "-n", "-S", "--radix=d", elf],
                         capture_output=True, text=True, check=True).stdout
    syms = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 4 and parts[2] in ("T", "t", "W", "w"):
            addr, size, _, name = int(parts[0]), int(parts[1]), parts[2], parts[3]
            if size > 0:
                syms.append((addr, size, name))
    syms.sort()
    return syms


class Code:
    """Instruction words of .text and the .sceStub.text range, for spotting
    samples that sit at the return site of a system call."""

    def __init__(self, elf, objdump):
        out = subprocess.run([objdump, "-h", elf], capture_output=True,
                             text=True, check=True).stdout
        self.sections = {}
        for line in out.splitlines():
            parts = line.split()
            if len(parts) >= 6 and parts[1] in (".text", ".sceStub.text"):
                self.sections[parts[1]] = (int(parts[3], 16), int(parts[2], 16),
                                           int(parts[5], 16))
        with open(elf, "rb") as fh:
            vma, size, off = self.sections[".text"]
            fh.seek(off)
            self.text = fh.read(size)
        self.text_vma = self.sections[".text"][0]
        svma, ssize, _ = self.sections.get(".sceStub.text", (0, 0, 0))
        self.stub = (svma, svma + ssize)
        self._stubby = {}

    def word(self, addr):
        i = addr - self.text_vma
        if 0 <= i <= len(self.text) - 4:
            return int.from_bytes(self.text[i:i + 4], "little")
        return None

    def in_stub(self, addr):
        return self.stub[0] <= addr < self.stub[1]

    def stubby(self, target, sym_bounds, depth=0):
        """A system-call stub, or a function that tail-jumps (j) to one."""
        if self.in_stub(target):
            return True
        if depth > 1 or target in self._stubby:
            return self._stubby.get(target, False)
        self._stubby[target] = False
        bounds = sym_bounds(target)
        result = False
        if bounds and bounds[1] - bounds[0] <= 256:
            for a in range(bounds[0], bounds[1], 4):
                w = self.word(a)
                if w is not None and (w >> 26) == 2:
                    t = ((a + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
                    if self.stubby(t, sym_bounds, depth + 1):
                        result = True
                        break
        self._stubby[target] = result
        return result

    def syscall_return(self, pc, sym_bounds):
        """pc directly follows `jal stub` (and its delay slot)."""
        w = self.word(pc - 8)
        if w is None:
            return False
        if (w >> 26) == 3:
            target = ((pc - 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
            return self.stubby(target, sym_bounds)
        return False


class Symbolizer:
    def __init__(self, syms, base=0):
        self.syms = syms
        self.addrs = [s[0] for s in syms]
        self.base = base

    def bounds(self, pc):
        i = bisect.bisect_right(self.addrs, pc) - 1
        if i >= 0:
            addr, size, name = self.syms[i]
            if pc < addr + size:
                return addr, addr + size
        return None

    def lookup(self, pc):
        pc -= self.base
        i = bisect.bisect_right(self.addrs, pc) - 1
        if i >= 0:
            addr, size, name = self.syms[i]
            if pc < addr + size:
                return name
        return None


def timeline_edges(log):
    """Window edges in seconds from main(), from tools/load_timeline.py."""
    here = os.path.dirname(os.path.abspath(__file__))
    out = subprocess.run([sys.executable, os.path.join(here, "load_timeline.py"),
                          log], capture_output=True, text=True).stdout
    edges = [("load", 0.0)]
    names = {"first usable input": "usable->send",
             "send press": "send->answer",
             "first answer": "after-answer"}
    for line in out.splitlines():
        m = re.match(r"\s+(first usable input|send press|first answer)\s+"
                     r"([0-9.]+) s", line)
        if m:
            edges.append((names[m.group(1)], float(m.group(2))))
    return edges


def bench_edges(log):
    """Kernel windows from the at-ms stamps of tilefinch-js-bench lines: a
    kernel ran between the previous line and its own."""
    edges = []
    prev_ms = None
    with open(log, errors="replace") as fh:
        for line in fh:
            m = re.search(r"tilefinch-js-bench: (?:kernel=(\S+)|(begin|end))"
                          r".* at-ms=(\d+)", line)
            if not m:
                continue
            at = int(m.group(3)) / 1000.0
            name = m.group(1) or m.group(2)
            alloc = re.search(r" alloc=(\S+)", line)
            if alloc and m.group(1):
                name += "/" + alloc.group(1)
            if prev_ms is not None and name != "begin":
                edges.append((name, prev_ms, at))
            prev_ms = at
    return edges


def read_samples(paths):
    for path in paths:
        with open(path) as fh:
            header = fh.readline().rstrip("\n").split("\t")
            for line in fh:
                row = dict(zip(header, line.rstrip("\n").split("\t")))
                yield path, row


def line_report(target, pcs, syms):
    """How many 64-byte I-cache lines hold a function's samples: the lines
    covering 50/85/95% of them, and the bytes those lines span."""
    pieces = [(a, sz, n) for a, sz, n in syms
              if n == target or n.startswith(target + ".")]
    total = sum(pcs.values())
    print("== lines in %s: %s; %d samples" % (
        target, ", ".join("%s %d B at %x" % (n, sz, a) for a, sz, n in pieces),
        total))
    if not total:
        return
    lines = collections.Counter()
    for pc, n in pcs.items():
        lines[pc // 64] += n
    ranked = lines.most_common()
    start = min(a for a, _, _ in pieces) // 64 if pieces else 0
    print("   %d lines sampled of %d" % (
        len(ranked), sum((sz + 63) // 64 for _, sz, _ in pieces)))
    for t in (0.5, 0.85, 0.95, 1.0):
        cum = 0
        chosen = []
        for line, n in ranked:
            cum += n
            chosen.append(line)
            if cum >= t * total:
                break
        print("   %3d%% of samples: %d lines (%d B), within the first %d B "
              "of the function's pieces" % (
                  t * 100, len(chosen), 64 * len(chosen),
                  64 * (max(chosen) - start + 1)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("samples", nargs="+")
    ap.add_argument("--elf", required=True)
    ap.add_argument("--nm", default="psp-nm")
    ap.add_argument("--objdump", default="psp-objdump")
    ap.add_argument("--keep-syscall-returns", action="store_true",
                    help="keep samples at system-call return sites (host "
                    "time spent in PPSSPP's HLE, not guest instructions)")
    ap.add_argument("--drop-fn", default="psp_validation_display_wait_vblank",
                    help="comma list of functions whose samples are waits "
                    "(validation display pacing)")
    ap.add_argument("--drop-pc", default="",
                    help="comma list of hex program counters to drop "
                    "(return sites of indirect calls into system calls)")
    ap.add_argument("--prx-base", type=lambda v: int(v, 0), default=0)
    ap.add_argument("--timeline-log",
                    help="journey validation log: windows at its milestones")
    ap.add_argument("--bench-log",
                    help="js-bench validation log: one window per kernel")
    ap.add_argument("--mhz", type=float, default=333.0,
                    help="emulated clock, to turn cycles into seconds")
    ap.add_argument("--weight", default="ticks", choices=["ticks", "samples"],
                    help="ticks (default): weight a sample by the emulated "
                    "cycles since the previous one, so samples taken while "
                    "the emulated clock stood still (HLE calls, emulator "
                    "stalls) carry no weight; samples: count every sample")
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--json-out")
    ap.add_argument("--lines", default="",
                    help="comma list of functions: sample density per 64 B "
                    "I-cache line inside each (with its .cold/.part pieces)")
    args = ap.parse_args()

    syms = load_symbols(args.elf, args.nm)
    sizes = {}
    addr_of = {}
    for addr, size, name in syms:
        sizes[name] = size
        addr_of[name] = addr
    sym = Symbolizer(syms, args.prx_base)
    jedges = timeline_edges(args.timeline_log) if args.timeline_log else None
    bedges = bench_edges(args.bench_log) if args.bench_log else None

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
            return "outside-bench"
        return "all"

    rows = []
    for path, row in read_samples(args.samples):
        try:
            rows.append((path, int(row["pc"], 16),
                         int(row.get("ra", "0") or "0", 16),
                         int(row.get("ticks", "0") or "0")))
        except ValueError:
            continue
    # Weight: emulated cycles since the previous sample, capped at 3x the
    # median step so an idle stretch does not land on one program counter.
    weights = []
    base_ticks = {}
    prev = {}
    for path, pc, ra, ticks in rows:
        if path not in base_ticks and ticks:
            base_ticks[path] = ticks
        d = ticks - prev.get(path, ticks) if ticks else 0
        prev[path] = ticks
        weights.append(max(0, d))
    positive = sorted(w for w in weights if w > 0)
    cap = 3 * positive[len(positive) // 2] if positive else 1
    counts = collections.defaultdict(collections.Counter)
    edges = collections.Counter()
    outside = collections.Counter()
    hle = collections.Counter()
    code = Code(args.elf, args.objdump)
    line_fns = [x for x in args.lines.split(",") if x]
    line_pcs = collections.defaultdict(collections.Counter)
    drop = {int(x, 16) for x in args.drop_pc.split(",") if x}
    drop_fn = {x for x in args.drop_fn.split(",") if x}
    for (path, pc, ra, ticks), w in zip(rows, weights):
        weight = min(w, cap) if (args.weight == "ticks" and positive) else 1
        seconds = (ticks - base_ticks.get(path, 0)) / (args.mhz * 1e6)
        win = window_of(seconds)
        fn = sym.lookup(pc)
        if fn is None:
            outside[win] += weight
            continue
        if pc in drop or fn in drop_fn or (not args.keep_syscall_returns and
                          code.syscall_return(pc - args.prx_base, sym.bounds)):
            hle[win] += weight
            continue
        if win != "all":
            counts[win][fn] += weight
        counts["all"][fn] += weight
        for target in line_fns:
            if fn == target or fn.startswith(target + "."):
                line_pcs[target][pc - args.prx_base] += weight
        caller = sym.lookup(ra)
        if caller and caller != fn:
            edges[(caller, fn)] += weight

    report = {}
    windows = [w for w in counts if w != "all"] + ["all"]
    for win in windows:
        c = counts[win]
        total = sum(c.values())
        if not total:
            continue
        print("== window %s: %d in text (dropped: %d idle/HLE thread, %d at "
              "system-call returns)" % (win, total, outside[win], hle[win]))
        ranked = c.most_common()
        cum = 0
        cum_bytes = 0
        thresholds = [0.5, 0.8, 0.9, 0.95, 0.99]
        cover = {}
        for name, n in ranked:
            cum += n
            cum_bytes += sizes.get(name, 0)
            for t in thresholds:
                if t not in cover and cum >= t * total:
                    cover[t] = (cum_bytes, ranked.index((name, n)) + 1)
        print("   hot set: " + ", ".join(
            "%d%%=%d fns/%d B" % (t * 100, cover[t][1], cover[t][0])
            for t in thresholds if t in cover))
        print("   %6s %6s %8s %10s %5s  %s" % (
            "share", "cum", "size", "addr", "mod8K", "function"))
        cum = 0
        for name, n in ranked[:args.top]:
            cum += n
            a = addr_of.get(name, 0)
            print("   %5.1f%% %5.1f%% %8d %10x %5d  %s" % (
                100.0 * n / total, 100.0 * cum / total, sizes.get(name, 0),
                a, (a % WAY) // 64, name))
        report[win] = {"total": total, "outside": outside[win],
                       "functions": ranked,
                       "cover": {str(k): v for k, v in cover.items()}}

    # Conflict view: for the hot set of the whole run, how many hot bytes
    # land on each of the 128 64-byte line sets of an 8 KiB way.
    allc = counts["all"]
    total = sum(allc.values())
    hot = []
    cum = 0
    for name, n in allc.most_common():
        cum += n
        hot.append(name)
        if cum >= 0.9 * total:
            break
    sets = [0.0] * (WAY // 64)
    for name in hot:
        a, sz = addr_of[name], sizes[name]
        # weight each line by the function's samples per line
        lines = max(1, sz // 64)
        w = allc[name] / lines
        for off in range(0, sz, 64):
            sets[((a + off) % WAY) // 64] += w
    if total:
        span = (max(addr_of[n] + sizes[n] for n in hot)
                - min(addr_of[n] for n in hot))
        hot_bytes = sum(sizes[n] for n in hot)
        print("== 90%% hot set: %d functions, %d bytes, spread over %d bytes "
              "of .text" % (len(hot), hot_bytes, span))
        ws = sorted(sets, reverse=True)
        print("   sample weight per 64 B line set (of %d): max %.0f, "
              "median %.0f, min %.0f"
              % (len(sets), ws[0], ws[len(ws) // 2], ws[-1]))
        overlap = collections.Counter()
        for name in hot:
            a, sz = addr_of[name], sizes[name]
            for off in range(0, sz, 64):
                overlap[((a + off) % WAY) // 64] += 1
        depth = collections.Counter(overlap.values())
        print("   hot functions' lines per set (a 2-way set holds 2): "
              + ", ".join("%d lines x%d sets" % (k, v)
                          for k, v in sorted(depth.items())))

    for target in line_fns:
        line_report(target, line_pcs[target], syms)

    if args.json_out:
        with open(args.json_out, "w") as fh:
            json.dump({"windows": report,
                       "edges": [[a, b, n] for (a, b), n in edges.most_common(2000)]},
                      fh)


if __name__ == "__main__":
    main()
