#!/usr/bin/env python3
"""Check that a PSP link placed the committed hot order where it promises.

  tools/check_psp_hot_order.py --nm psp-nm --elf ELF --order cmake/PspHotOrder.ld \\
      [--min-placed 0.95]

GNU ld silently ignores an ordering-file pattern that matches nothing, so a
renamed function, a moved source file or a section a build names
differently would quietly fall out of its fixed place.
This reads the linked ELF's symbols and, for every entry of the order file
(its comment names the function and its share of the profiled heat):

- placed: the function (under any GCC clone name: an entry's pattern
  matches all of them) lies in the ordered block, below the
  __tilefinch_hot_order_end symbol the order file defines at its end;
- missed: the function is in the image but outside the ordered block, for
  example because its object or section was renamed;
- absent: the image has no such function at all (compiled out of this
  build, such as validation-only instrumentation, or inlined everywhere);
  absent entries are reported and not charged.

It fails when placed heat is below --min-placed of placed plus missed heat.
"""
import argparse
import re
import subprocess
import sys

ENTRY = re.compile(r"^\s+\*\S+\((\.text[^)]*)\)\s*/\*\s*(\S+)\s+([0-9.]+)%\s*\*/")
CLONE = re.compile(r"(?:\.(?:constprop|isra|part|cold|lto_priv)(?:\.\d+)*)+$")


def base_name(name):
    return CLONE.sub("", name)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--nm", required=True)
    ap.add_argument("--elf", required=True)
    ap.add_argument("--order", required=True)
    ap.add_argument("--min-placed", type=float, default=0.95)
    args = ap.parse_args()

    entries = []
    for line in open(args.order, encoding="utf-8"):
        m = ENTRY.match(line)
        if m:
            entries.append((m.group(2), float(m.group(3))))
    if not entries:
        sys.exit("%s lists no functions" % args.order)

    out = subprocess.run([args.nm, "-n", "-S", "--radix=d", args.elf],
                         capture_output=True, text=True, check=True).stdout
    text = []  # (addr, name) of sized functions
    for row in out.splitlines():
        parts = row.split()
        if len(parts) == 4 and parts[2] in "TtWw" and int(parts[1]) > 0:
            text.append((int(parts[0]), parts[3]))
    if not text:
        sys.exit("%s has no function symbols (stripped?)" % args.elf)
    # The order file ends its block with this symbol.
    ends = [int(r.split()[0]) for r in out.splitlines()
            if r.split()[-1:] == ["__tilefinch_hot_order_end"]]
    if not ends:
        sys.exit("%s has no __tilefinch_hot_order_end: was it linked with "
                 "%s?" % (args.elf, args.order))
    block_end = ends[0]
    bases = {}
    for addr, name in text:
        bases.setdefault(base_name(name), []).append((addr, name))

    placed = missed = absent = 0.0
    misses = []
    absent_count = 0
    for name, share in entries:
        copies = bases.get(base_name(name))
        if copies:
            if any(a < block_end for a, _ in copies):
                placed += share
            else:
                missed += share
                misses.append((share, name, "outside the ordered block, as " +
                               ", ".join(n for _, n in copies[:3])))
        else:
            absent += share
            absent_count += 1
    ratio = placed / (placed + missed) if placed + missed else 1.0
    print("-- PSP hot order: %.1f%% of present heat placed (%.2f%% placed, "
          "%.2f%% missed, %.2f%% in %d entries absent from this image; "
          "minimum %.0f%%)" % (100 * ratio, placed, missed, absent,
                               absent_count, 100 * args.min_placed))
    for share, name, why in sorted(misses, reverse=True)[:10]:
        print("--   missed %s (%.3f%%): %s" % (name, share, why))
    if ratio < args.min_placed:
        sys.exit("PSP hot order placed too little of its heat in %s: "
                 "regenerate cmake/PspHotOrder.ld "
                 "(docs/engineering/CODE_LAYOUT.md)" % args.elf)


if __name__ == "__main__":
    main()
