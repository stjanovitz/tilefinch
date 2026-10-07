#!/usr/bin/env python3
"""Classic-script bytecode cache breakdown of a --revisit, --revisit-ticked
or --restart replay (TILEFINCH_TRACE_CENSUS ledger lines census-bytecode-*
and census-visit; a --restart run's first process, lab.first.log, is visit
1 and the measured process visit 2).

  bytecode.py RUN_DIR [--pages]

Prints, corpus-wide and (with --pages) per page: every classic external
script lookup of each visit by result (hit, miss-no-record, ...,
ineligible-*), the store attempts by result, and the entries dropped by
reason. The second visit's hit rate counts eligible lookups only.
"""
import collections
import os
import re
import sys

FIELD = re.compile(r"(\w[\w-]*)=(\S*)")


def ledger_lines(log):
    first = os.path.join(os.path.dirname(log), "lab.first.log")
    if os.path.exists(first):
        yield from open(first, errors="replace")
        yield "census-visit restart\n"
    yield from open(log, errors="replace")


def page_events(log):
    """(visit, record, result, segment) for every ledger line of one run;
    `segment` is true for a ResourceLoader segment (ordinal != 0, or the
    older ineligible-segment result)."""
    visit = 1
    for line in ledger_lines(log):
        if line.startswith("census-visit"):
            visit += 1
            continue
        if not line.startswith("census-bytecode-"):
            continue
        record = line.split()[0][len("census-bytecode-"):]
        fields = dict(FIELD.findall(line.split(" url=", 1)[0]))
        result = fields.get("result") or fields.get("reason") or "?"
        segment = fields.get("ordinal", "0") != "0" \
            or result == "ineligible-segment"
        yield visit, record, result, segment


def eligible(counter):
    return sum(n for k, n in counter.items() if not k.startswith("ineligible"))


def hits(counter):
    return sum(n for k, n in counter.items() if k.startswith("hit"))


def summarize(run):
    pages = {}
    for name in sorted(os.listdir(run)):
        log = os.path.join(run, name, "lab.log")
        if ".r" in name or not os.path.exists(log):
            continue
        tally = collections.defaultdict(collections.Counter)
        for visit, record, result, segment in page_events(log):
            tally[(visit, record)][result] += 1
            if record == "lookup":
                tally[(visit, "segment" if segment else "whole")][result] += 1
        pages[name] = tally
    return pages


def fmt(counter):
    return ", ".join("%s %d" % kv for kv in counter.most_common()) or "-"


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    pages = summarize(sys.argv[1])
    total = collections.defaultdict(collections.Counter)
    for tally in pages.values():
        for key, counter in tally.items():
            total[key].update(counter)
    visits = sorted({v for v, _ in total})
    for visit in visits:
        lookups = total[(visit, "lookup")]
        print("visit %d lookups: %s" % (visit, fmt(lookups)))
        print("  eligible %d, hits %d (%.0f%%)" % (
            eligible(lookups), hits(lookups),
            100.0 * hits(lookups) / max(1, eligible(lookups))))
        for part in ("whole", "segment"):
            c = total[(visit, part)]
            print("  %s scripts: %d lookups, %d eligible, %d hits" % (
                "ResourceLoader segment" if part == "segment" else "whole",
                sum(c.values()), eligible(c), hits(c)))
        print("  preflight: %s" % fmt(total[(visit, "preflight")]))
        print("  stores: %s" % fmt(total[(visit, "store")]))
        print("  drops: %s" % fmt(total[(visit, "drop")]))
    if "--pages" not in sys.argv:
        return
    last = visits[-1] if visits else 1
    print()
    print("| page | visit %d eligible | hits | misses | ineligible | "
          "skipped stores (visit 1) | drops (visit 1) |" % last)
    print("|---|---:|---:|---:|---:|---|---|")
    for name, tally in pages.items():
        lookups = tally[(last, "lookup")]
        if not sum(lookups.values()):
            continue
        skips = collections.Counter({k: n for k, n in tally[(1, "store")].items()
                                     if not k.startswith("stored")
                                     and k != "deferred"})
        print("| %s | %d | %d | %d | %d | %s | %s |" % (
            name, eligible(lookups), hits(lookups),
            eligible(lookups) - hits(lookups),
            sum(lookups.values()) - eligible(lookups),
            fmt(skips), fmt(tally[(1, "drop")])))


if __name__ == "__main__":
    main()
