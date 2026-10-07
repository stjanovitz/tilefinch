#!/usr/bin/env python3
"""Compare two site-census JSONL files (base vs. branch, or run 1 vs. run 2).

usage: compare.py BASE.jsonl NEW.jsonl [--pages] [--metric NAME]... [--threshold PCT]

Prints, per metric, corpus totals, the change, and the per-page spread
(median and 90th-percentile absolute per-page change), then pages whose
outcome, errors or limits changed, and with --pages a per-page table of the
metrics that moved by more than --threshold percent.

Run the same build twice and compare the two files to see replay noise: the
p90 column is the per-page noise floor a real change must clear.
"""
import argparse
import json
import statistics

# (label, dotted path, group). Groups follow the planned work:
# A = JavaScript cost, B = memory headroom, C = fallbacks, T = timings,
# W = deterministic work counts (exact across runs of one build).
METRICS = [
    ("first paint ms", "timing.first_paint_ms", "T"),
    ("loaded ms", "timing.loaded_ms", "T"),
    ("longest step ms", "timing.longest_step_ms", "T"),
    ("relayouts", "timing.relayouts", "T"),
    ("relayout ms", "timing.relayout_ms", "T"),
    ("JS compile ms", "js.compile_ms", "A"),
    ("JS execute ms", "js.execute_ms", "A"),
    ("JS pre-paint ms", "js.pre_paint_ms", "A"),
    ("non-essential pre-paint ms", "js.pre_paint_nonessential_ms", "A"),
    ("JS source bytes", "js.source_bytes", "A"),
    ("JS heap peak bytes", "js.heap_peak_bytes", "A"),
    ("peak MB", "memory.peak_mb", "B"),
    ("javascript peak MB", "memory.categories_peak_mb.javascript", "B"),
    ("layout peak MB", "memory.categories_peak_mb.layout", "B"),
    ("resource peak MB", "memory.categories_peak_mb.resource", "B"),
    ("dom peak MB", "memory.categories_peak_mb.dom", "B"),
    ("render peak MB", "memory.categories_peak_mb.render", "B"),
    ("decoded image bytes", "images.decoded_bytes", "B"),
    ("layout retained bytes", "layout.retained_bytes", "B"),
    ("reader extracted bytes", "fallback.reader_extracted_bytes", "C"),
    ("basic nodes", "fallback.basic_nodes", "C"),
    ("basic text bytes", "fallback.basic_text", "C"),
    ("js.work_units", "work.js.work_units", "W"),
    ("style.resolutions", "work.style.resolutions", "W"),
    ("layout.passes", "work.layout.passes", "W"),
    ("dom.mutations", "work.dom.mutations", "W"),
    ("fetch.replay_served", "work.fetch.replay_served", "W"),
]


def get(record, path):
    value = record
    for part in path.split(".") if not path.startswith("work.") else ["work", path[5:]]:
        if not isinstance(value, dict) or part not in value:
            return None
        value = value[part]
    return value if isinstance(value, (int, float)) and not isinstance(value, bool) else None


def load(path):
    out = {}
    for line in open(path):
        if line.strip():
            rec = json.loads(line)
            out[rec["name"]] = rec
    return out


def pct(a, b):
    if a in (None, 0):
        return None if b in (None, 0) else float("inf")
    return 100.0 * (b - a) / abs(a)


def fmt(v):
    if v is None:
        return "-"
    if isinstance(v, float):
        return "%.1f" % v if abs(v) < 1e6 else "%.3g" % v
    return str(v)


def summary(base, new, names, metrics):
    print("%-28s %3s %14s %14s %8s %9s %9s %5s" % (
        "metric", "grp", "base total", "new total", "delta%", "med|d|%", "p90|d|%", "pages"))
    for label, path, group in metrics:
        a_total = b_total = 0
        per_page = []
        count = 0
        for n in names:
            a, b = get(base[n], path), get(new[n], path)
            if a is None or b is None:
                continue
            count += 1
            a_total += a
            b_total += b
            d = pct(a, b)
            if d is not None and d != float("inf"):
                per_page.append(abs(d))
            elif d == float("inf"):
                per_page.append(100.0)
        if not count:
            continue
        per_page.sort()
        med = statistics.median(per_page) if per_page else 0.0
        p90 = per_page[min(len(per_page) - 1, int(0.9 * len(per_page)))] if per_page else 0.0
        print("%-28s %3s %14s %14s %8s %9.1f %9.1f %5d" % (
            label, group, fmt(round(a_total, 1)), fmt(round(b_total, 1)),
            fmt(pct(a_total, b_total)), med, p90, count))


def changes(base, new, names):
    print("\noutcome / error / limit changes:")
    any_change = False
    for n in names:
        a, b = base[n], new[n]
        notes = []
        if a.get("outcome_auto") != b.get("outcome_auto"):
            notes.append("outcome %s -> %s" % (a.get("outcome_auto"), b.get("outcome_auto")))
        ea = {e["message"] for e in a.get("errors", {}).get("exceptions", [])}
        eb = {e["message"] for e in b.get("errors", {}).get("exceptions", [])}
        if ea != eb:
            if eb - ea:
                notes.append("new errors: " + "; ".join(sorted(eb - ea))[:200])
            if ea - eb:
                notes.append("gone errors: " + "; ".join(sorted(ea - eb))[:200])
        if set(a.get("limits", [])) != set(b.get("limits", [])):
            notes.append("limits %s -> %s" % (",".join(a.get("limits", [])) or "-",
                                              ",".join(b.get("limits", [])) or "-"))
        if a.get("fallback", {}).get("reader_kind") != b.get("fallback", {}).get("reader_kind"):
            notes.append("reader %s -> %s" % (a["fallback"].get("reader_kind"),
                                               b["fallback"].get("reader_kind")))
        if notes:
            any_change = True
            print("  %-20s %s" % (n, " | ".join(notes)))
    if not any_change:
        print("  none")


def pages(base, new, names, metrics, threshold):
    print("\nper-page changes over %.0f%%:" % threshold)
    for n in names:
        moved = []
        for label, path, _ in metrics:
            a, b = get(base[n], path), get(new[n], path)
            if a is None or b is None:
                continue
            d = pct(a, b)
            if d is not None and abs(d) > threshold:
                moved.append("%s %s->%s (%+.0f%%)" % (label, fmt(a), fmt(b), d)
                             if d != float("inf") else "%s %s->%s" % (label, fmt(a), fmt(b)))
        if moved:
            print("  %-20s %s" % (n, "; ".join(moved)))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("base")
    parser.add_argument("new")
    parser.add_argument("--pages", action="store_true")
    parser.add_argument("--metric", action="append",
                        help="restrict to metrics whose label or path contains this")
    parser.add_argument("--group", help="restrict to groups, e.g. AB")
    parser.add_argument("--threshold", type=float, default=10.0)
    args = parser.parse_args()
    base, new = load(args.base), load(args.new)
    names = sorted(set(base) & set(new))
    only = sorted(set(base) ^ set(new))
    metrics = METRICS
    if args.metric:
        metrics = [m for m in metrics if any(s in m[0] or s in m[1] for s in args.metric)]
    if args.group:
        metrics = [m for m in metrics if m[2] in args.group]
    print("%d pages in both; %s" % (len(names), "only in one: " + ", ".join(only) if only else "same page set"))
    summary(base, new, names, metrics)
    changes(base, new, names)
    if args.pages:
        pages(base, new, names, metrics, args.threshold)


if __name__ == "__main__":
    main()
