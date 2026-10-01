#!/usr/bin/env python3
"""Where script time went, from the script split (include/tilefinch/script_split.h).

  tools/script_split_report.py LOG [--from MARK] [--to MARK] [--top N]

LOG is a PSP validation log (PPSSPP or device) or a lab log. It reads the
cumulative `tilefinch-script-split: label=...` lines printed beside every
tilefinch-work record (input-script marks, lab `work` commands) and the
`split={...} longest-split={...}` fields of `runtime-checkpoint` records
(validation builds; TILEFINCH_TRACE_RUNTIME_STEPS=1 in the lab).

It prints, for the window between two marks (default: every consecutive
pair), the split of all script time, then the window's longest promise
checkpoints with their split and their longest job's split. Timed kinds
(style, query, mutate, fetch, layout, compile, gc, host) are exact; the JS
kind (interpreter, built-ins, untimed natives) is divided into page,
bootstrap and native-at-poll by sampling at VM polls, an estimate.
"""
import argparse
import re
import sys

KINDS = ["js", "style", "query", "mutate", "fetch", "layout", "compile",
         "gc", "host"]
SAMPLES = ["page", "bootstrap", "native"]
FIELD = re.compile(r"([a-z-]+)=(\d+)")


def fields(text):
    return {k: int(v) for k, v in FIELD.findall(text)}


def minus(a, b):
    return {k: a.get(k, 0) - b.get(k, 0) for k in a}


def describe(split, indent="  "):
    total = sum(split.get(k, 0) for k in KINDS)
    if total <= 0:
        return indent + "(no script time)"
    out = []
    for k in KINDS:
        us = split.get(k, 0)
        calls = split.get(k + "-calls")
        extra = "" if calls is None or k == "js" else "  %d calls" % calls
        if k == "js":
            polled = sum(split.get(s, 0) for s in SAMPLES) or 1
            parts = ", ".join("%s ~%.0f%%" % (s, 100.0 * split.get(s, 0)
                                              / polled) for s in SAMPLES)
            unpolled = us - sum(split.get(s, 0) for s in SAMPLES)
            extra = "  (%s; %d polls; %.1f ms after the last poll)" % (
                parts, split.get("polls", 0), max(0, unpolled) / 1000.0)
        out.append("%s%-8s %9.1f ms %5.1f%%%s" % (
            indent, k, us / 1000.0, 100.0 * us / total, extra))
    out.append("%s%-8s %9.1f ms" % (indent, "total", total / 1000.0))
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--from", dest="start")
    ap.add_argument("--to", dest="end")
    ap.add_argument("--top", type=int, default=3)
    args = ap.parse_args()
    marks = []        # (time us, label, cumulative split)
    checkpoints = []  # (begin us, duration us, jobs, split, longest)
    with open(args.log, errors="replace") as handle:
        for index, line in enumerate(handle):
            at = line.find("tilefinch-script-split: label=")
            if at >= 0:
                rest = line[at + len("tilefinch-script-split: label="):]
                label, _, tail = rest.partition(" ")
                split = fields(tail)
                # A mark can log twice (observed, then its full record):
                # the first is the mark.
                if any(label == seen for _, seen, _ in marks):
                    continue
                marks.append((split.pop("at-us", index), label, split))
                continue
            at = line.find("runtime-checkpoint promise=")
            if at >= 0 and "split={" in line:
                head, _, tail = line[at:].partition(" split={")
                whole, _, longest = tail.partition("} longest-split={")
                record = fields(head)
                promise = re.search(r"promise=(\d+)/(\d+)/(\d+)", head)
                duration = record.get("end-us", 0) - record.get("begin-us", 0)
                checkpoints.append((record.get("begin-us", index), duration,
                                    promise.groups()
                                    if promise else ("?", "0", "0"),
                                    fields(whole),
                                    fields(longest.rstrip("}\n"))))
    if len(marks) < 2:
        sys.exit("fewer than two tilefinch-script-split marks in %s"
                 % args.log)
    labels = [label for _, label, _ in marks]
    first = labels.index(args.start) if args.start in labels else 0
    if args.start and args.start not in labels:
        sys.exit("no mark %s in %s" % (args.start, args.log))
    if args.end and args.end not in labels[first + 1:]:
        sys.exit("no mark %s after %s in %s" % (args.end, labels[first],
                                                args.log))
    if args.start or args.end:
        last = labels.index(args.end, first + 1) if args.end else first + 1
        windows = [(marks[first], marks[last])]
    else:
        windows = list(zip(marks, marks[1:]))
    for (i0, l0, s0), (i1, l1, s1) in windows:
        print("== %s -> %s: script split" % (l0, l1))
        print(describe(minus(s1, s0)))
        inside = [c for c in checkpoints if i0 < c[0] < i1]
        if not inside:
            continue
        total = {}
        for c in inside:
            for k, v in c[3].items():
                total[k] = total.get(k, 0) + v
        print("  %d promise checkpoints recorded, %.1f ms of them:" % (
            len(inside), sum(c[1] for c in inside) / 1000.0))
        print(describe(total, "    "))
        for c in sorted(inside, key=lambda c: -c[1])[:args.top]:
            jobs, job_us, job_max = c[2]
            print("  checkpoint %.1f ms (%s jobs, longest %.1f ms):" % (
                c[1] / 1000.0, jobs, int(job_max) / 1000.0))
            print(describe(c[3], "    "))
            print("    longest job:")
            print(describe(c[4], "      "))


if __name__ == "__main__":
    main()
