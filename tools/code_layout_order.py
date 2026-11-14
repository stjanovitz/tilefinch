#!/usr/bin/env python3
"""Write a GNU ld --section-ordering-file that packs the sampled hot code of
the PSP browser at the start of .text, grouped by call affinity.

  tools/code_layout_order.py --elf ELF --map MAP --out ORDER.ld \\
      PROFILE.json[=WEIGHT] [...] [--coverage 0.995] [--max-bytes N] \\
      [--pin FUNCTION ...]

PROFILE.json comes from tools/pc_profile_report.py --json-out (its "all"
window). Each profile is normalized to WEIGHT (default 1) so a short bench
run and a long journey can be mixed. ELF (with symbols) and MAP must come
from the same link (PSP_BROWSER_PSP_LINK_MAP=ON; the hot order may be on or
off): the map names each function's input section and object, which is what the ordering
file matches. Functions compiled with -ffunction-sections match as
`*object(.text.name)`; newlib/libgcc members (one function per object,
plain .text) as `*libc.a:member(.text)`.

--stable writes the committed hot order (cmake/PspHotOrder.ld,
docs/engineering/CODE_LAYOUT.md): the hot set in two tiers, code Tilefinch
does not edit first (toolchain, SDK and vendored libraries, QuickJS) and
Tilefinch's own code second. Each tier, and the rest of .text after them,
starts on an 8 KiB link-address boundary, so at the same offset within an
I-cache way relative to the image base. A size change outside the hot
set then moves no hot function, and one inside a tier moves only that
tier's later functions. Each entry's comment carries its share of the
profiled heat, which tools/check_psp_hot_order.py weighs after each link.

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


CLONE = re.compile(r"(?:\.(?:constprop|isra|part|cold|lto_priv)(?:\.\d+)*)+$")


def base_name(name):
    """A function's name without GCC clone suffixes (foo.constprop.0 -> foo)."""
    return CLONE.sub("", name)


def section_patterns(section):
    """A function section matches under any GCC clone suffix: shipping and
    validation builds clone different functions (.text.foo in one,
    .text.foo.constprop.0 in the other). C names have no dots, so
    .text.foo.* matches only foo's clones."""
    if not section.startswith(".text."):
        return section
    base = ".text." + base_name(section[len(".text."):])
    return "%s %s.*" % (base, base)


def pattern_for(name, path):
    sections = section_patterns(name)
    m = re.match(r"(.*)\((.*)\)$", path)
    if m:
        archive = os.path.basename(m.group(1))
        return "*%s:%s(%s)" % (archive, m.group(2), sections)
    return "*%s(%s)" % (os.path.basename(path), sections)


# Header-only libraries fetched at a pinned commit and compiled into
# Tilefinch objects (stb_image, stb_truetype, nanosvg): their functions
# change only with the pin, like any other vendored code.
VENDORED_PREFIXES = ("stbi_", "stbtt_", "nsvg")


def is_tilefinch(path, section=""):
    """Tilefinch's own code: its libtilefinch_* archives and the browser
    executable's loose objects, less the header-only vendored libraries
    compiled into them. Everything else (newlib, libgcc, the SDK, QuickJS
    and the other vendored libraries) changes only with a pin."""
    if section.startswith(".text.") and \
            section[len(".text."):].startswith(VENDORED_PREFIXES):
        return False
    m = re.match(r"(.*)\((.*)\)$", path)
    if m:
        return os.path.basename(m.group(1)).startswith("libtilefinch")
    # The map names the executable's own objects relative to the build
    # directory: CMakeFiles/psp-browser-script.dir/src/....c.obj.
    return re.search(r"(^|/)CMakeFiles/", path) is not None


def placeable(name, path):
    """Entry and cold code keep their places; LTO partitions are named at
    random per link, so no pattern could find them again."""
    base = os.path.basename(path)
    return not (name.startswith((".text.unlikely", ".text.startup"))
                or base.startswith("crt") or ".ltrans" in base)


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
    ap.add_argument("--stable", action="store_true",
                    help="two tiers (code Tilefinch does not edit, then its "
                    "own), each on an 8 KiB way boundary: the committed "
                    "hot order")
    ap.add_argument("--command", default="",
                    help="regeneration command recorded in the header")
    ap.add_argument("--pin", action="append", default=[],
                    help="also place this function (repeatable): code the "
                    "profiles under-sample, such as the game audio mixer's "
                    "per-block helpers; it gets the heat of the coldest "
                    "chosen function")
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
    floor = min(heat[n] for n in chosen) if chosen else 0.0
    for name in args.pin:
        if not by_name.get(name):
            sys.exit("--pin %s: no such function in %s" % (name, args.elf))
        if name not in chosen:
            heat[name] = max(heat[name], floor)
            chosen.append(name)
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

    def sections_of(name):
        out = []
        for addr, size, _ in by_name[name]:
            i = bisect.bisect_right(sec_addrs, addr) - 1
            if i < 0 or addr >= sections[i][0] + sections[i][1]:
                out.append(None)
            else:
                out.append(sections[i])
        return out

    # Tier 0: code Tilefinch does not edit; tier 1: Tilefinch's own. Only
    # --stable separates them. A function's tier steers clustering; each
    # input section is placed in its own object's tier (a static name can
    # exist in a vendored and a Tilefinch object at once).
    tier = {}
    for n in chosen:
        secs = [x for x in sections_of(n) if x]
        tier[n] = int(args.stable
                      and any(is_tilefinch(x[3], x[2]) for x in secs))
    cluster_of = {n: [n] for n in chosen}
    csize = {n: size_of[n] for n in chosen}
    for name in sorted(chosen, key=lambda n: -heat[n]):
        if not callers[name]:
            continue
        caller, _ = callers[name].most_common(1)[0]
        cf, cc = cluster_of[name], cluster_of[caller]
        if cf is cc or tier[name] != tier[caller]:
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
    chains.sort(key=lambda ch: (tier[ch[0]], -sum(heat[n] for n in ch)
                / max(1024, sum(size_of[n] for n in ch))))
    if args.shuffle is not None:
        import random
        rng = random.Random(args.shuffle)
        singles = [[n] for ch in chains for n in ch]
        rng.shuffle(singles)
        chains = sorted(singles, key=lambda ch: tier[ch[0]])
    if args.verbose:
        at = 0
        for ch in chains:
            size = sum(size_of[n] for n in ch)
            print("cluster @%7d %6d B heat %.4f: %s" % (
                at, size, sum(heat[n] for n in ch),
                " ".join(ch[:10]) + (" ..." if len(ch) > 10 else "")))
            at += size
    edges = [1 for c in callers.values() for _ in c]

    tier_entries = [[], []]
    placed = {}
    counted = set()
    placed_bytes = 0
    unmatched = 0
    tier_bytes = [0, 0]
    tier_heat = [0.0, 0.0]
    for ch in chains:
        for name in ch:
            secs = sections_of(name)
            live = [x for x in secs if x and placeable(x[2], x[3])]
            for sec in secs:
                if sec is None:
                    unmatched += 1
                    continue
                sa, ssize, sname, spath = sec
                if not placeable(sname, spath):
                    continue
                pat = pattern_for(sname, spath)
                t = int(args.stable and is_tilefinch(spath, sname))
                share = 100.0 * heat[name] / max(total, 1e-9) / max(1, len(live))
                if sa not in counted:  # several functions can share one section
                    counted.add(sa)
                    placed_bytes += ssize
                    tier_bytes[t] += ssize
                tier_heat[t] += share
                if pat in placed:
                    # another clone of the same function: one pattern
                    # places both, so its entry carries both shares
                    placed[pat][2] += share
                    continue
                placed[pat] = [pat, base_name(name), share]
                tier_entries[t].append(placed[pat])
    # The heat share in each comment is what tools/check_psp_hot_order.py
    # weighs after each link.
    tier_lines = [["  %s  /* %s %.4f%% */" % tuple(e) for e in entries]
                  for entries in tier_entries]
    # A fixed offset within an 8 KiB way: the link address is aligned, so
    # every tier keeps the same cache sets whatever the load address.
    align = "  . = ALIGN(8192);  /* the next block starts at a fixed offset within a cache way */"
    if args.stable:
        lines = ["  /* tier 0: toolchain, SDK and vendored libraries */"]
        lines += tier_lines[0] + [align, "  /* tier 1: Tilefinch */"]
        lines += tier_lines[1] + [align]
        # tools/check_psp_hot_order.py measures placement against this.
        lines.append("  __tilefinch_hot_order_end = .;")
    else:
        lines = tier_lines[0] + tier_lines[1]
    with open(args.out, "w") as fh:
        fh.write("/* Generated by tools/code_layout_order.py: %d entries, "
                 "%d bytes, %.1f%% of sampled heat, from %s. */\n"
                 % (len(placed), placed_bytes, 100.0 * cum / max(total, 1e-9),
                    ", ".join(labels)))
        if args.stable:
            fh.write("/* Tier 0: %d bytes, %.1f%% of heat; tier 1: %d bytes, "
                     "%.1f%%. Do not edit by hand: regenerate as in "
                     "docs/engineering/CODE_LAYOUT.md. */\n"
                     % (tier_bytes[0], tier_heat[0], tier_bytes[1], tier_heat[1]))
        if args.command:
            fh.write("/* %s */\n" % args.command)
        fh.write(".text : {\n")
        fh.write("\n".join(lines))
        fh.write("\n}\n")
    print("wrote %s: %d functions in %d chains, %d entries, %d bytes "
          "(tiers %d + %d; %d unmatched), %d call edges"
          % (args.out, len(chosen), len(chains), len(placed), placed_bytes,
             tier_bytes[0], tier_bytes[1], unmatched, len(edges)))


if __name__ == "__main__":
    main()
