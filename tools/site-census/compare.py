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
    ("focus ms", "timing.focus_ms", "T"),
    ("focus relayout moves", "timing.focus_relayout_moves", "T"),
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


def focus_view(rec):
    """A record's focus-move data as (issued, moves, after, mode, legacy).
    moves and after map a frame-hash label (focus-N, focus-final) to its
    hash; None means the record has no such data at all. Records from
    before the per-label keys carry one positional `focus_frames` list:
    its interaction-latency half used another hash and is returned as
    `legacy` (compared only against another legacy list), and its
    loop-frame-hash half (one per move, then focus-final) as `after`."""
    if "focus_after_hashes" in rec or "focus_issued" in rec:
        return (rec.get("focus_issued"), rec.get("focus_move_hashes"),
                rec.get("focus_after_hashes"), rec.get("focus_hashes"), None)
    frames = rec.get("focus_frames")
    if frames is None:
        return None, None, None, None, None
    presented = rec.get("timing", {}).get("focus_moves") or 0
    tail = frames[presented:]
    if not tail:
        # Before per-move frame-hash commands: no extra renders.
        return None, None, None, "none", frames
    labels = ["focus-%d" % (i + 1) for i in range(len(tail) - 1)] + ["focus-final"]
    return None, None, dict(zip(labels, tail)), "each", frames[:presented]


def label_key(label):
    tail = label.rsplit("-", 1)[-1]
    return (0, int(tail), "") if tail.isdigit() else (1, 0, label)


def hash_changes(kind, ha, hb):
    """Notes for one label-keyed hash map: one side missing it entirely,
    labels measured on one side only, and labels whose hashes differ. An
    empty hash is not measured, never equal."""
    if ha is None and hb is None:
        return []
    if ha is None or hb is None:
        return ["focus %s hashes on %s side only" % (kind, "new" if ha is None else "base")]
    one_sided = []
    differ = []
    for label in sorted(set(ha) | set(hb), key=label_key):
        x, y = ha.get(label, ""), hb.get(label, "")
        if not x and not y:
            continue
        if not x or not y:
            one_sided.append("%s(%s)" % (label[6:] if label.startswith("focus-") else label,
                                         "new" if not x else "base"))
        elif x != y:
            differ.append(label[6:] if label.startswith("focus-") else label)
    notes = []
    if one_sided:
        notes.append("FOCUS %s HASHES ON ONE SIDE ONLY at %s" % (
            kind.upper(), ",".join(one_sided[:12]) + ("..." if len(one_sided) > 12 else "")))
    if differ:
        notes.append("focus %s frames differ at %s" % (
            kind, ",".join(differ[:12]) + ("..." if len(differ) > 12 else "")))
    return notes


def focus_changes(a, b):
    ia, ma, fa, _, la = focus_view(a)
    ib, mb, fb, _, lb = focus_view(b)
    pa = a.get("timing", {}).get("focus_moves")
    pb = b.get("timing", {}).get("focus_moves")
    has_a = any(v is not None for v in (ia, fa, la, pa))
    has_b = any(v is not None for v in (ib, fb, lb, pb))
    if has_a != has_b:
        # The case the hashes exist for: one run's moves left no data.
        return ["FOCUS DATA ON %s SIDE ONLY" % ("BASE" if has_a else "NEW")]
    notes = []
    if ia is not None and ib is not None and ia != ib:
        notes.append("focus moves issued %s -> %s" % (ia, ib))
    if pa is not None and pb is not None and pa != pb:
        notes.append("focus moves presented %s -> %s" % (pa, pb))
    if la is not None and lb is not None:
        # Both records predate per-label keys: the old positional check.
        if la != lb:
            moved = [str(i + 1) for i in range(max(len(la), len(lb)))
                     if i >= len(la) or i >= len(lb) or la[i] != lb[i]]
            notes.append("focus frames differ at move " + ",".join(moved[:12])
                         + ("..." if len(moved) > 12 else ""))
    elif la is None and lb is None:
        notes += hash_changes("move", ma, mb)
    # A record from before per-move frame-hash commands has no after
    # frames; focus_modes() says so once rather than on every page.
    if not ((fa is None and la is not None) or (fb is None and lb is not None)):
        notes += hash_changes("after", fa, fb)
    return notes


def focus_modes(base, new, names):
    """One line when the two runs took focus timings under different
    command scripts (TB-7: per-move frame-hash renders cost the moves
    1-2%), or when only one side has per-label move hashes."""
    def modes(records):
        return {focus_view(records[n])[3] for n in names} - {None}
    ma, mb = modes(base), modes(new)
    out = []
    if ma and mb and ma != mb:
        out.append("note: focus timings are not like-for-like (base frame-hash "
                   "mode %s, new %s)" % ("/".join(sorted(ma)), "/".join(sorted(mb))))
    legacy = [side for side, records in (("base", base), ("new", new))
              if any(focus_view(records[n])[4] is not None for n in names)]
    if len(legacy) == 1:
        out.append("note: %s focus move hashes predate per-label keys; "
                   "only the after-move frames are compared" % legacy[0])
    return out


def changes(base, new, names):
    print("\noutcome / error / limit / focus-frame changes:")
    for line in focus_modes(base, new, names):
        print("  " + line)
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
        notes += focus_changes(a, b)
        for side, rec in (("base", a), ("new", b)):
            if rec.get("focus_frames_unstable"):
                notes.append("focus frames vary across %s repeats" % side)
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
