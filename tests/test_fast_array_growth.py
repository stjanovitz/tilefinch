#!/usr/bin/env python3
"""Qualify bounded dense-array capacity without a site-specific workload."""

from __future__ import annotations

import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path


EXPECTED_SUMMARY = 'javascript summary="FAST-ARRAY-OK:300000:2052427040"'


def run(lab: Path, fixture: Path, growth_kb) -> int:
    environment = os.environ.copy()
    if growth_kb is None:
        environment.pop("TILEFINCH_JS_ARRAY_GROWTH_KB", None)
    else:
        environment["TILEFINCH_JS_ARRAY_GROWTH_KB"] = str(growth_kb)
    mode = "default" if growth_kb is None else str(growth_kb)
    with tempfile.TemporaryDirectory(prefix="tilefinch-fast-array-") as work:
        completed = subprocess.run(
            [str(lab), "--fixture", str(fixture), "--fetch-scripts",
             "--ticks", "1", "--limit-mb", "64", "--script-heap-mb", "32",
             "--script-total-mb", "8", "--script-file-kb", "512",
             "--script-count", "8", "--no-loop-capture", "--output",
             str(Path(work) / "frame.ppm")],
            capture_output=True, text=True, env=environment, timeout=30,
        )
    output = completed.stdout + completed.stderr
    if completed.returncode != 0:
        raise RuntimeError(f"lab failed for growth={mode}: {output[-500:]}")
    if EXPECTED_SUMMARY not in output or "status=PASS" not in output:
        raise RuntimeError(
            f"array semantics or teardown failed for growth={mode}: "
            f"{output[-500:]}")
    match = re.search(
        r"^memory-categories phase=interactive-stable .*"
        r"global-peak=(\d+) .*reconcile=yes$", output, re.MULTILINE)
    if match is None:
        raise RuntimeError(f"stable memory ledger missing: {output[-500:]}")
    return int(match.group(1))


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} LAB FIXTURE", file=sys.stderr)
        return 2
    lab, fixture = map(Path, sys.argv[1:])
    legacy_peak = run(lab, fixture, 0)
    bounded_peak = run(lab, fixture, None)
    if bounded_peak >= legacy_peak:
        print(
            f"bounded array growth did not reduce peak: bounded={bounded_peak} "
            f"legacy={legacy_peak}", file=sys.stderr)
        return 1
    print(
        f"fast-array growth passed: bounded={bounded_peak} legacy={legacy_peak} "
        f"saved={legacy_peak - bounded_peak}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
