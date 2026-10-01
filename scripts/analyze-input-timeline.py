#!/usr/bin/env python3
"""Account for an explicit monotonic window in a PSP validation log.

Only outer loop phases are added. JS/native/curl samples overlap them and
must not be added to this wall-clock ledger. Gaps stay unassigned, not network.
"""
import argparse
import json
import re
from pathlib import Path

PHASES = ("wait", "input", "action", "probe", "navigation", "runtime",
          "idle", "raster", "present", "images", "tail")


def nested_records(lines, start, end):
    """Report timed children separately; never add them to owner-loop totals.

    Boundary-crossing records remain explicitly partial rather than assigning
    their whole work to a clipped window. Legacy untimed diagnostics are not
    evidence of which event/checkpoint incurred a cost.
    """
    records = []
    for line in lines:
        dispatch = "tilefinch-dispatch-timing:" in line
        checkpoint = "runtime-checkpoint promise=" in line
        if not dispatch and not checkpoint:
            continue
        fields = dict(re.findall(r"([a-z-]+)=([^\s]+)", line))
        if "begin-us" not in fields or "end-us" not in fields:
            continue
        begin, finish = int(fields["begin-us"]), int(fields["end-us"])
        if finish < begin:
            raise ValueError("reversed nested timing record")
        if finish <= start or begin >= end:
            continue
        record = {"kind": "dispatch" if dispatch else "checkpoint",
                  "begin_us": begin, "end_us": finish,
                  "complete_in_window": begin >= start and finish <= end}
        if dispatch:
            times = {key: int(fields[key]) for key in ("handler", "jobs", "refresh")}
            if sum(times.values()) != finish - begin:
                raise ValueError("inconsistent dispatch timing record")
            record.update(event=fields["event"], phase=int(fields["phase"]),
                          phases_us=times)
        else:
            for kind in ("promise", "continuation"):
                count, total, maximum = map(int, fields[kind].split("/"))
                if maximum > total or (count == 0 and total != 0):
                    raise ValueError("inconsistent checkpoint timing record")
                record[kind] = {"count": count, "total_us": total, "max_us": maximum}
            record["pending"] = bool(int(fields["pending"]))
        records.append(record)
    return records


def summarize(lines, start, end):
    if start < 0 or end <= start:
        raise ValueError("require 0 <= start < end")
    totals = dict.fromkeys(PHASES, 0)
    previous_end = None
    frames = 0
    for line in lines:
        if "tilefinch-loop-timing:" not in line:
            continue
        values = dict(re.findall(r"([a-z-]+)=([0-9]+)(?:\s|$)", line))
        required = ("begin-us", "end-us") + PHASES
        if any(key not in values for key in required):
            raise ValueError("incomplete loop timing record")
        values = {key: int(values[key]) for key in required}
        begin, finish = values["begin-us"], values["end-us"]
        if (finish < begin or sum(values[key] for key in PHASES) != finish - begin
                or (previous_end is not None and begin < previous_end)):
            raise ValueError("inconsistent or overlapping loop timing record")
        previous_end = finish
        if finish <= start or begin >= end:
            continue
        frames += 1
        cursor = begin
        for key in PHASES:
            following = cursor + values[key]
            totals[key] += max(0, min(following, end) - max(cursor, start))
            cursor = following
    if not frames:
        raise ValueError("no loop records overlap the requested window")
    accounted = sum(totals.values())
    return {"window_us": end - start, "frames": frames, "phases_us": totals,
            "accounted_us": accounted, "unassigned_us": end - start - accounted}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--start-us", type=int, required=True)
    parser.add_argument("--end-us", type=int, required=True)
    args = parser.parse_args()
    try:
        with args.log.open(encoding="utf-8", errors="replace") as source:
            lines = source.readlines()
        result = summarize(lines, args.start_us, args.end_us)
        result["nested_not_additive"] = nested_records(lines, args.start_us, args.end_us)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
