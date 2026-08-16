#!/usr/bin/env python3
"""Deterministic-counter regression guard.

Runs psp-browser-lab on hermetic fixtures and compares the engine's
deterministic diagnostics counters (layout geometry, stylesheet
diagnostics, tile/glyph cache behaviour) against checked-in baselines.
Unlike wall-clock timings these counters are exact for a given binary and
fixture, so drift means a real behaviour change: either a regression, or
an intentional change that should update the baseline.

Usage:
    test_counter_baselines.py <psp-browser-lab> [--update]

With --update (or TILEFINCH_UPDATE_COUNTER_BASELINES=1) the golden file
tests/counter-baselines.tsv is rewritten from the current build instead
of being checked; commit the result alongside the change that moved the
numbers.
"""

from __future__ import annotations

import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
GOLDEN_PATH = Path(__file__).resolve().parent / "counter-baselines.tsv"

SCENARIOS = [
    ("float-article", ["--fixture", "fixtures/float-article.html",
                       "--skip-js", "--scroll-all"]),
    ("grid-tracks", ["--fixture", "fixtures/grid-tracks.html",
                     "--skip-js", "--scroll-all"]),
    ("demo", ["--fixture", "fixtures/demo.html",
              "--skip-js", "--scroll-all"]),
]

# (line prefix, key) pairs harvested from the lab's stdout.  Timing counters
# (raster-us, frame-us, ...) and byte totals that track pointer-width or
# allocator details are deliberately excluded.
HARVEST = {
    "layout ": ["width", "scroll-width", "height", "commands", "links",
                "sticky", "fixed"],
    "stylesheet ": ["rules", "important-rules", "layers", "variables",
                    "scoped-variables", "deferred-bytes"],
    "stylesheet-diagnostics ": ["declarations", "supported", "rejected",
                                "deferred", "custom-drops", "selector-drops",
                                "unknown-media", "supports-false"],
    "tiles ": ["hits", "misses", "evictions", "rasterized", "candidates",
               "spatial-bands", "glyph-misses", "glyph-evictions"],
    "scroll ": ["frames", "blank", "revisit"],
    "document ": ["nodes", "elements", "text-nodes", "attributes"],
}


def harvest(stdout: str) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in stdout.splitlines():
        for prefix, keys in HARVEST.items():
            if not line.startswith(prefix):
                continue
            fields = dict(
                pair.split("=", 1)
                for pair in re.findall(r"[a-z-]+=[^ ]+", line)
            )
            label = prefix.strip()
            for key in keys:
                if key in fields:
                    values[f"{label}.{key}"] = fields[key]
    return values


def run_scenario(lab: Path, arguments: list[str]) -> dict[str, str]:
    with tempfile.TemporaryDirectory(prefix="tilefinch-counters-") as output:
        completed = subprocess.run(
            [str(lab), *arguments, "--output-dir", output],
            cwd=REPO_ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            timeout=120,
        )
    if completed.returncode != 0:
        raise RuntimeError(
            f"lab exited with {completed.returncode} for {arguments}")
    return harvest(completed.stdout)


def collect(lab: Path) -> dict[str, dict[str, str]]:
    return {name: run_scenario(lab, arguments)
            for name, arguments in SCENARIOS}


def write_golden(results: dict[str, dict[str, str]]) -> None:
    lines = ["# scenario\tcounter\tvalue",
             "# Regenerate: tests/test_counter_baselines.py <lab> --update"]
    for scenario in sorted(results):
        for counter in sorted(results[scenario]):
            lines.append(f"{scenario}\t{counter}\t{results[scenario][counter]}")
    GOLDEN_PATH.write_text("\n".join(lines) + "\n", encoding="utf-8")


def read_golden() -> dict[str, dict[str, str]]:
    if not GOLDEN_PATH.exists():
        raise SystemExit(
            f"missing {GOLDEN_PATH}; run with --update to create it")
    golden: dict[str, dict[str, str]] = {}
    for line in GOLDEN_PATH.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#"):
            continue
        scenario, counter, value = line.split("\t")
        golden.setdefault(scenario, {})[counter] = value
    return golden


def main() -> int:
    arguments = [a for a in sys.argv[1:]]
    update = "--update" in arguments
    if update:
        arguments.remove("--update")
    if os.environ.get("TILEFINCH_UPDATE_COUNTER_BASELINES") == "1":
        update = True
    if len(arguments) != 1:
        print(__doc__, file=sys.stderr)
        return 2
    lab = Path(arguments[0]).resolve()

    results = collect(lab)
    if update:
        write_golden(results)
        total = sum(len(v) for v in results.values())
        print(f"wrote {total} counters to {GOLDEN_PATH}")
        return 0

    golden = read_golden()
    failures: list[str] = []
    for scenario, counters in golden.items():
        actual = results.get(scenario)
        if actual is None:
            failures.append(f"{scenario}: scenario missing from run")
            continue
        for counter, expected in counters.items():
            got = actual.get(counter)
            if got != expected:
                failures.append(
                    f"{scenario}: {counter} expected {expected} got {got}")
    for scenario in results:
        if scenario not in golden:
            failures.append(f"{scenario}: not present in golden file")

    if failures:
        print("counter baselines drifted (deterministic counters changed):",
              file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        print("If the change is intentional, regenerate with:\n"
              f"  python3 {Path(__file__).name} {lab} --update",
              file=sys.stderr)
        return 1
    total = sum(len(v) for v in golden.values())
    print(f"{total} counters match baselines")
    return 0


if __name__ == "__main__":
    sys.exit(main())
