#!/usr/bin/env python3
"""Join `tilefinch-js-bench:` reports and print per-kernel cost ratios.

  js_bench_compare.py LABEL=FILE [LABEL=FILE ...]

Each FILE is the output of tilefinch-js-bench (host) or a PSP validation log
from a validation_js_bench=N run (PPSSPP or device). The first file is the
reference; every later column is that run's ns-per-iteration divided by the
reference's. A PPSSPP column is an instruction-count proxy (about one
instruction per cycle at 333 MHz, no cache model): its ratio to the host is
the instruction-count ratio, and a device column divided by the PPSSPP
column is the device's effective CPI (memory stalls and other device-only
costs). Kernels whose ratio stands well above the plain int_loop's name a
cost the slower run pays disproportionately.
"""
import re
import sys

LINE = re.compile(r"tilefinch-js-bench: kernel=(\S+)(.*)")


def load(path):
    rows = {}
    with open(path, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            match = LINE.search(line)
            if not match:
                continue
            name, rest = match.group(1), match.group(2)
            fields = dict(re.findall(r"(\S+?)=(\S+)", rest))
            key = name + ("/" + fields["alloc"] if "alloc" in fields else "")
            cost = None
            for field in ("ns-per-iter", "ns-per-object", "ns-per-byte"):
                if field in fields:
                    cost = float(fields[field])
                    break
            if cost is None and "us" in fields:
                cost = float(fields["us"]) * 1000.0
            if cost is not None:
                rows[key] = (cost, fields)
    return rows


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    runs = []
    for spec in argv[1:]:
        label, _, path = spec.partition("=")
        if not path:
            label, path = path or spec, spec
        runs.append((label, load(path)))
    reference_label, reference = runs[0]
    keys = list(reference)
    width = max(len(key) for key in keys) + 2
    header = "%-*s %10s" % (width, "kernel", reference_label + " ns")
    for label, _ in runs[1:]:
        header += " %10s %8s" % (label + " ns", "x" + reference_label)
    if len(runs) > 2:
        header += " %8s" % ("%s/%s" % (runs[2][0], runs[1][0]))
    print(header)
    for key in keys:
        base = reference[key][0]
        line = "%-*s %10.0f" % (width, key, base)
        ratios = []
        for label, rows in runs[1:]:
            cost = rows.get(key, (None, None))[0]
            if cost is None or not base:
                line += " %10s %8s" % ("-", "-")
                ratios.append(None)
            else:
                line += " %10.0f %8.1f" % (cost, cost / base)
                ratios.append(cost)
        if len(runs) > 2:
            if ratios[0] and ratios[1]:
                line += " %8.2f" % (ratios[1] / ratios[0])
            else:
                line += " %8s" % "-"
        print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
