#!/usr/bin/env python3
"""Read `tilefinch-work:` records (docs/engineering/LAB_USAGE.md, "Work
vector") from lab, PPSSPP or device logs.

  work_vector_report.py RUN                 values per label
  work_vector_report.py --steps RUN         mark-to-mark deltas in one run
  work_vector_report.py RUN_A RUN_B         values of B and B - A per label
  work_vector_report.py --check-equal A B   exit 1 unless the vectors match

Counters are cumulative per document, so a delta between consecutive marks
of the same page is the work done between them. A label that repeats in one
run is numbered label#2, label#3, ... in order. `--fields PREFIX` keeps only
fields starting with PREFIX (for example `js.` or `style.`); `--ignore
PREFIX` (repeatable) drops fields starting with PREFIX. Values that
are not integers (`n/a` for counters a build did not compile) compare as
text and never produce a delta.
"""
import argparse
import re
import sys

RECORD = re.compile(r"tilefinch-work: label=(\S+)((?: [A-Za-z0-9_.\-]+=\S*)*)")
FIELD = re.compile(r" ([A-Za-z0-9_.\-]+)=(\S*)")


def parse_value(text):
    try:
        return int(text)
    except ValueError:
        return text


def parse(text):
    """Return an ordered list of (label, {field: value}) records."""
    records = []
    seen = {}
    for line in text.splitlines():
        match = RECORD.search(line)
        if match is None:
            continue
        label = match.group(1)
        seen[label] = seen.get(label, 0) + 1
        if seen[label] > 1:
            label = f"{label}#{seen[label]}"
        fields = {key: parse_value(value)
                  for key, value in FIELD.findall(match.group(2))}
        records.append((label, fields))
    return records


def load(path):
    with open(path, errors="replace") as stream:
        return parse(stream.read())


IGNORED = []


def keep(fields, prefix):
    return {key: value for key, value in fields.items()
            if key.startswith(prefix or "")
            and not any(key.startswith(item) for item in IGNORED)}


def delta(before, after):
    if isinstance(before, int) and isinstance(after, int):
        return after - before
    return None


def format_delta(before, after):
    change = delta(before, after)
    if change is None:
        return "" if before == after else "changed"
    if change == 0:
        return "0"
    percent = f" ({100.0 * change / before:+.1f}%)" if before else ""
    return f"{change:+d}{percent}"


def print_values(records, prefix, out):
    for label, fields in records:
        print(f"[{label}]", file=out)
        for key, value in keep(fields, prefix).items():
            print(f"  {key} = {value}", file=out)


def print_steps(records, prefix, out):
    previous = None
    for label, fields in records:
        fields = keep(fields, prefix)
        print(f"[{label}]", file=out)
        for key, value in fields.items():
            if previous is None or key not in previous:
                print(f"  {key} = {value}", file=out)
                continue
            change = delta(previous[key], value)
            shown = "" if change is None else f" (+{change})" if change >= 0 \
                else f" ({change}: counter reset, new document?)"
            print(f"  {key} = {value}{shown}", file=out)
        previous = fields


def compare(first, second, prefix, out=None):
    """Print B's values with B - A (when out is given). Returns the list of
    differences."""
    def say(text):
        if out is not None:
            print(text, file=out)
    a = dict(first)
    b = dict(second)
    differences = []
    for label in [label for label, _ in first] + \
            [label for label, _ in second if label not in a]:
        if label not in b:
            differences.append(f"{label}: missing from the second run")
            say(f"[{label}] missing from the second run")
            continue
        if label not in a:
            differences.append(f"{label}: missing from the first run")
            say(f"[{label}] missing from the first run")
            continue
        left = keep(a[label], prefix)
        right = keep(b[label], prefix)
        say(f"[{label}]")
        for key in list(left) + [key for key in right if key not in left]:
            if key not in right or key not in left:
                differences.append(f"{label}: {key} only in one run")
                say(f"  {key} only in one run")
                continue
            if left[key] != right[key]:
                differences.append(
                    f"{label}: {key} {left[key]} -> {right[key]}")
            say(f"  {key} = {right[key]}  {format_delta(left[key], right[key])}"
                  .rstrip())
    return differences


def main(argv=None):
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("runs", nargs="+", metavar="RUN",
                        help="log file(s) containing tilefinch-work: lines")
    parser.add_argument("--check-equal", action="store_true",
                        help="fail unless two runs' vectors are identical")
    parser.add_argument("--steps", action="store_true",
                        help="show mark-to-mark deltas within one run")
    parser.add_argument("--fields", default="",
                        help="only fields whose name starts with this prefix")
    parser.add_argument("--ignore", action="append", default=[],
                        metavar="PREFIX",
                        help="drop fields whose name starts with PREFIX")
    args = parser.parse_args(argv)
    IGNORED[:] = args.ignore
    runs = [load(path) for path in args.runs]
    for path, records in zip(args.runs, runs):
        if not records:
            print(f"work_vector_report: no tilefinch-work records in {path}",
                  file=sys.stderr)
            return 2
    if args.check_equal or len(runs) == 2:
        if len(runs) != 2:
            parser.error("comparing needs exactly two runs")
        differences = compare(runs[0], runs[1], args.fields,
                              None if args.check_equal else sys.stdout)
        if args.check_equal:
            if differences:
                print(f"work vectors differ ({len(differences)}):", file=sys.stderr)
                for line in differences:
                    print(f"  {line}", file=sys.stderr)
                return 1
            print(f"work vectors identical: {len(runs[0])} records")
        return 0
    if len(runs) != 1:
        parser.error("give one run, or two to compare")
    if args.steps:
        print_steps(runs[0], args.fields, sys.stdout)
    else:
        print_values(runs[0], args.fields, sys.stdout)
    return 0


if __name__ == "__main__":
    sys.exit(main())
