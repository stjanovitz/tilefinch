#!/usr/bin/env python3
"""Separate shipping-clock cursor qualification from underclock profiling."""
import argparse
from pathlib import Path
import re


def fields(log, prefix):
    lines = [line for line in log.splitlines()
             if line.startswith(prefix + ": phase=controlled-exit ")]
    if not lines:
        raise ValueError("missing " + prefix)
    return {key: int(value) for key, value in
            re.findall(r"([a-z-]+)=(\d+)(?:us)?(?= |$)", lines[-1])}


def check(log, cpu_mhz):
    ui = fields(log, "tilefinch-ui-cadence")
    cadence = fields(log, "tilefinch-cursor-cadence")
    required = ("cursor-samples", "cursor-presents", "cursor-coalesced",
                "cursor-average", "cursor-max")
    if any(key not in ui for key in required) or any(
            key not in cadence for key in ("intervals", "average", "max", "segments")):
        raise ValueError("missing cursor measurements (rebuild validation EBOOT)")
    if (ui["cursor-samples"] == 0
            or ui["cursor-samples"] != ui["cursor-presents"]
            or ui["cursor-coalesced"] != 0):
        raise ValueError("cursor samples lack one accepted presentation each")
    if cadence["intervals"] == 0 or cadence["segments"] < 2:
        raise ValueError("missing continuous-motion segments")
    summary = (f"clock={cpu_mhz}MHz cadence-average={cadence['average']}us "
               f"cadence-max={cadence['max']}us "
               f"latency-average={ui['cursor-average']}us "
               f"latency-max={ui['cursor-max']}us")
    if cpu_mhz != 333:
        # Still require complete input/presentation accounting and an exact
        # receiver trace. Never label an underclock run a shipping perf pass.
        return "PRESSURE (timings reported, not stock-clock qualified): " + summary
    if cadence["average"] > 20000 or cadence["max"] > 34000:
        raise ValueError("cursor sampling missed display cadence: " + summary)
    if ui["cursor-average"] > 20000 or ui["cursor-max"] > 34000:
        raise ValueError("cursor input-to-presentation latency exceeded budget: " + summary)
    return "QUALIFIED: " + summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--cpu-mhz", type=int, required=True)
    args = parser.parse_args()
    try:
        if args.cpu_mhz <= 0:
            raise ValueError("cursor qualification requires an explicit positive clock")
        print(check(args.log.read_text(), args.cpu_mhz))
    except (ValueError, OSError) as error:
        parser.exit(1, f"FAIL: {error}\n")


if __name__ == "__main__":
    main()
