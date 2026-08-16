#!/usr/bin/env python3
"""Cross-check PSP-build counters against the host golden baselines.

Feed it a PPSSPP log (or raw stdout) containing the psp-counter lines
emitted by the psp-browser-fixture EBOOT, and it compares every counter
against tests/counter-baselines.tsv for the same scenario.  Layout,
style, and document counters are integer/26.6 fixed-point and must match
the host exactly; a mismatch is a genuine 32-bit portability bug.

Usage:
    compare_psp_counters.py <ppsspp-log> [scenario]
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

GOLDEN_PATH = Path(__file__).resolve().parent / "counter-baselines.tsv"


def read_golden(scenario: str) -> dict[str, str]:
    golden: dict[str, str] = {}
    for line in GOLDEN_PATH.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#"):
            continue
        row_scenario, counter, value = line.split("\t")
        if row_scenario == scenario:
            golden[counter] = value
    if not golden:
        raise SystemExit(f"no golden counters for scenario {scenario!r}")
    return golden


def read_psp(log_path: Path, scenario: str) -> dict[str, str]:
    values: dict[str, str] = {}
    # PPSSPP's log renders the tabs as whitespace; accept both.
    pattern = re.compile(
        r"psp-counter\s+" + re.escape(scenario) + r"\s+(\S+)\s+(\S+)")
    for line in log_path.read_text(encoding="utf-8",
                                   errors="replace").splitlines():
        match = pattern.search(line)
        if match:
            # Later boots overwrite earlier ones; runs are deterministic.
            values[match.group(1)] = match.group(2)
    if not values:
        raise SystemExit(f"no psp-counter lines for {scenario!r} in {log_path}")
    return values


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    log_path = Path(sys.argv[1])
    scenario = sys.argv[2] if len(sys.argv) > 2 else "float-article"

    golden = read_golden(scenario)
    psp = read_psp(log_path, scenario)

    matched = 0
    mismatched: list[str] = []
    missing: list[str] = []
    for counter, expected in sorted(golden.items()):
        got = psp.get(counter)
        if got is None:
            missing.append(counter)
        elif got == expected:
            matched += 1
        else:
            mismatched.append(f"  {counter}: host {expected} psp {got}")

    extra = sorted(set(psp) - set(golden))
    print(f"scenario {scenario}: {matched} counters match host baselines")
    if missing:
        print(f"not reported by the PSP build ({len(missing)}):"
              f" {', '.join(missing)}")
    if extra:
        print(f"psp-only counters ({len(extra)}): {', '.join(extra)}")
    if mismatched:
        print("MISMATCHES (32-bit determinism breaks):")
        print("\n".join(mismatched))
        return 1
    print("determinism cross-check: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
