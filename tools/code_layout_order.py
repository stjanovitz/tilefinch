#!/usr/bin/env python3
"""Write a GNU ld --section-ordering-file that packs the sampled hot code of
the PSP browser at the start of .text, grouped by call affinity.

  tools/code_layout_order.py --elf ELF --map MAP --out ORDER.ld \\
      PROFILE.json[=WEIGHT] [...] [--coverage 0.995] [--max-bytes N]

PROFILE.json comes from tools/pc_profile_report.py --json-out (its "all"
window). Each profile is normalized to WEIGHT (default 1) so a short bench
run and a long journey can be mixed. ELF (with symbols) and MAP must come
from the same link (PSP_BROWSER_PSP_LINK_MAP=ON, no ordering file): the map
names each function's input section and object, which is what the ordering
file matches. Functions compiled with -ffunction-sections match as
`*object(.text.name)`; newlib/libgcc members (one function per object,
plain .text) as `*libc.a:member(.text)`.

Order: C3 call-chain clustering (as in HFSort) over the static call graph
of the hot set (jal and tail-call j edges, weighted by the lesser of the two
ends' heat), clusters capped at --cluster-bytes (16 KiB, the I-cache), then
clusters by heat density, hottest first. Placing the hot set
contiguously means any two hot functions within 16 KiB of each other cannot
evict one another in the 2-way, 8 KiB-per-way Allegrex I-cache, and a
phase's working set occupies as few cache sets as its size allows.
"""
import argparse
import bisect
import collections
import json
import os
import re
import subprocess
import sys


def load_symbols(elf, nm):
    out = subprocess.run([nm, "-n", "-S", "--radix=d", elf],
                         capture_output=True, text=True, check=True).stdout
    syms = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 4 and parts[2] in ("T", "t", "W", "w"):
            if int(parts[1]) > 0:
                syms.append((int(parts[0]), int(parts[1]), parts[3]))
    syms.sort()
    return syms


def load_text(elf, objdump):
    out = subprocess.run([objdump, "-h", elf], capture_output=True,
                         text=True, check=True).stdout
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 6 and parts[1] == ".text":
            vma, size, off = int(parts[3], 16), int(parts[2], 16), int(parts[5], 16)
            with open(elf, "rb") as fh:
                fh.seek(off)
                return vma, fh.read(size)
    sys.exit("no .text in %s" % elf)


SECTION_RE = re.compile(
    r"^ (\.text(?:\.\S+)?)\s*\n?\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*)$",
    re.M)


def load_input_sections(map_path):
    text = open(map_path, errors="replace").read()
    start = text.find("\n.text ")
    if start < 0:
        start = text.find("\n.text\n")
    end = text.find("\n.fini", start)
    body = text[start:end if end > 0 else len(text)]
    sections = []
    for m in SECTION_RE.finditer(body):
        name, addr, size, path = m.group(1), int(m.group(2), 16), \
            int(m.group(3), 16), m.group(4).strip()
        if size == 0:
            continue
        sections.append((addr, size, name, path))
    sections.sort()
    return sections


def pattern_for(name, path):
    m = re.match(r"(.*)\((.*)\)$", path)
    if m:
        archive = os.path.basename(m.group(1))
        return "*%s:%s(%s)" % (archive, m.group(2), name)
    return "*%s(%s)" % (os.path.basename(path), name)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("profiles", nargs="+")
    ap.add_argument("--elf", required=True)
    ap.add_argument("--map", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--nm", default="psp-nm")
    ap.add_argument("--objdump", default="psp-objdump")
    ap.add_argument("--window", default="all")
    ap.add_argument("--coverage", type=float, default=0.995)
    ap.add_argument("--max-bytes", type=int, default=0)
    ap.add_argument("--min-samples", type=int, default=3,
                    help="ignore functions with fewer raw samples over all "
                    "profiles (one or two samples are noise)")
    ap.add_argument("--cluster-bytes", type=int, default=16384)
    ap.add_argument("--shuffle", type=int,
                    help="control layout: the same hot set in a seeded "
                    "random order (for telling ordering from relinking)")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    syms = load_symbols(args.elf, args.nm)
    addrs = [s[0] for s in syms]
    by_name = collections.defaultdict(list)
    for s in syms:
        by_name[s[2]].append(s)
    text_vma, text = load_text(args.elf, args.objdump)
    sections = load_input_sections(args.map)
    sec_addrs = [s[0] for s in sections]

    heat = collections.Counter()
    raw = collections.Counter()
    labels = []
    for spec in args.profiles:
        path, _, weight = spec.partition("=")
        weight = float(weight or 1)
        data = json.load(open(path))
        fns = data["windows"][args.window]["functions"]
        total = float(sum(n for _, n in fns)) or 1.0
        for name, n in fns:
            heat[name] += weight * n / total
            raw[name] += n
        labels.append("%s=%g" % (os.path.basename(path), weight))

    total = sum(heat.values())
    chosen = []
    cum = 0.0
    nbytes = 0
    for name, h in heat.most_common():
        if cum >= args.coverage * total:
            break
        size = sum(s[1] for s in by_name.get(name, []))
        if not size or raw[name] < args.min_samples:
            continue
        if args.max_bytes and nbytes + size > args.max_bytes:
            continue
        chosen.append(name)
        cum += h
        nbytes += size
    chosen_set = set(chosen)

    def fn_at(addr):
        i = bisect.bisect_right(addrs, addr) - 1
        if i >= 0 and addr < syms[i][0] + syms[i][1]:
            return syms[i][2]
        return None

    # Static call graph among the chosen functions: caller -> callee, the
    # weight is the lesser heat of the two ends (a proxy for call counts,
    # which PC samples do not give).
    callers = collections.defaultdict(collections.Counter)
    for name in chosen:
        for addr, size, _ in by_name[name]:
            for a in range(addr, addr + size, 4):
                i = a - text_vma
                if i < 0 or i + 4 > len(text):
                    continue
                w = int.from_bytes(text[i:i + 4], "little")
                op = w >> 26
                if op not in (2, 3):
                    continue
                target = ((a + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
                callee = fn_at(target)
                if callee and callee != name and callee in chosen_set:
                    callers[callee][name] = min(heat[name], heat[callee])

    # C3 (call-chain clustering, HFSort): take functions hottest first and
    # append each one's cluster to its heaviest caller's cluster while the
    # result stays within --cluster-bytes, then order clusters by heat per
    # byte. Clusters no larger than the I-cache keep a caller and its hot
    # callees conflict-free.
    size_of = {n: sum(s[1] for s in by_name[n]) for n in chosen}
    cluster_of = {n: [n] for n in chosen}
    csize = {n: size_of[n] for n in chosen}
    for name in sorted(chosen, key=lambda n: -heat[n]):
        if not callers[name]:
            continue
        caller, _ = callers[name].most_common(1)[0]
        cf, cc = cluster_of[name], cluster_of[caller]
        if cf is cc:
            continue
        if csize[cc[0]] + csize[cf[0]] > args.cluster_bytes:
            continue
        merged_size = csize[cc[0]] + csize[cf[0]]
        cc.extend(cf)
        for n in cf:
            cluster_of[n] = cc
        csize[cc[0]] = merged_size
    chains = []
    seen = set()
    for n in chosen:
        ch = cluster_of[n]
        if id(ch) not in seen:
            seen.add(id(ch))
            chains.append(ch)
    # Heat per byte, with a 1 KiB floor on the size so that a lone tiny
    # function with a few samples does not outrank a busy cluster.
    chains.sort(key=lambda ch: -sum(heat[n] for n in ch)
                / max(1024, sum(size_of[n] for n in ch)))
    if args.shuffle is not None:
        import random
        rng = random.Random(args.shuffle)
        singles = [[n] for ch in chains for n in ch]
        rng.shuffle(singles)
        chains = singles
    if args.verbose:
        at = 0
        for ch in chains:
            size = sum(size_of[n] for n in ch)
            print("cluster @%7d %6d B heat %.4f: %s" % (
                at, size, sum(heat[n] for n in ch),
                " ".join(ch[:10]) + (" ..." if len(ch) > 10 else "")))
            at += size
    edges = [1 for c in callers.values() for _ in c]

    lines = []
    placed = set()
    placed_bytes = 0
    unmatched = 0
    for ch in chains:
        for name in ch:
            for addr, size, _ in by_name[name]:
                i = bisect.bisect_right(sec_addrs, addr) - 1
                if i < 0 or addr >= sections[i][0] + sections[i][1]:
                    unmatched += 1
                    continue
                sa, ssize, sname, spath = sections[i]
                if (sname.startswith((".text.unlikely", ".text.startup"))
                        or os.path.basename(spath).startswith("crt")):
                    continue  # entry and cold code keep their places
                pat = pattern_for(sname, spath)
                if pat in placed:
                    continue
                placed.add(pat)
                placed_bytes += ssize
                lines.append("  %s  /* %s */" % (pat, name))
    with open(args.out, "w") as fh:
        fh.write("/* Generated by tools/code_layout_order.py: %d input sections, "
                 "%d bytes, %.1f%% of sampled heat, from %s. */\n"
                 % (len(lines), placed_bytes, 100.0 * cum / max(total, 1e-9),
                    ", ".join(labels)))
        fh.write(".text : {\n")
        fh.write("\n".join(lines))
        fh.write("\n}\n")
    print("wrote %s: %d functions in %d chains, %d input sections, %d bytes "
          "(%d unmatched), %d call edges"
          % (args.out, len(chosen), len(chains), len(lines), placed_bytes,
             unmatched, len(edges)))


if __name__ == "__main__":
    main()
