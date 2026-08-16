#!/usr/bin/env python3
"""Device-cost regression gate: compare PSP cost counters to a baseline.

tests/test_counter_baselines.py ratchets the HOST lab and deliberately
excludes timing and byte totals, so cost is the one budget dimension the
project never regressed on the device.  This closes that gap.  Feed it one
or more tilefinch-validation.txt logs produced by
scripts/run-ppsspp-device-cost.sh and it

  1. proves determinism, by requiring every log to agree with every other
     within the same treatment the baseline comparison will use (a baseline
     minted from a boot that is noisier than its own tolerances is worthless
     whichever way it later compares), then
  2. compares the first log against tests/psp-device-cost-baseline.tsv
     with an explicit per-counter treatment.

Treatments, one per baseline row, so that nothing is silently forgiven:
  exact     the counter is a pure function of the binary and the scenario
  band:N    numeric, allowed to differ from the baseline by at most N
  masked    wall-clock-derived; recorded for the artifact, never compared
             (masked counters are still required to be *present*)

Usage:
    compare_psp_device_cost.py [--scenario NAME] [--update] LOG [LOG...]

Regenerate the baseline (see scripts/run-ppsspp-device-cost.sh --help for
the full recipe):

    scripts/run-ppsspp-device-cost.sh --runs 2 --update-baseline
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

BASELINE_PATH = (Path(__file__).resolve().parent
                 / "psp-device-cost-baseline.tsv")

# (log prefix, counter label, fields that name the row rather than measure
# it).  Every remaining key=value token on the line becomes a counter.  Only
# cost-bearing lines are listed: this is a budget ratchet, not a log diff.
SPECS = [
    ("tilefinch-budget:", "budget", ["phase"]),
    ("tilefinch-budget-category:", "budget-category", ["phase", "name"]),
    ("tilefinch-render-job:", "render-job", []),
    ("tilefinch-engine-memory:", "engine-memory", []),
    ("tilefinch-render-suppression:", "render-suppression", []),
    ("tilefinch-ui-cadence:", "ui-cadence", ["phase"]),
    ("tilefinch-js-retention:", "js-retention", []),
]

# Counters whose value is a function of how fast the host ran, not of what
# the browser did.  They are harvested (so the artifact records them and a
# missing one is still a failure) but never compared.  Suffix match on the
# counter name, which keeps the phase/category qualifiers out of the rule.
MASKED_SUFFIXES = (
    ".max-slice",       # longest render-job slice, microseconds
    ".max-unit",        # longest render-job unit, microseconds
    ".compose-total",   # cumulative compose time, microseconds
    ".compose-max",
    ".compose-average",
    ".start-gap-max",
    ".over-16ms",       # frame-time histogram buckets
    ".over-33ms",
)

# The resource category is the one place the boot is not a fixed amount of
# work: whether a single ~95-byte resource allocation has landed by the time
# the report is taken depends on how many frames elapsed first, so it is
# stable within a session and moves between them.  Measured over six
# sessions of the start-page scenario the whole spread was 102 bytes and six
# allocations; the bands below are ~40x that, which is still far tighter than
# any regression worth catching (owned bytes move in kilobytes at least).
# Everything the resource category feeds into — the budget totals, the
# engine-memory totals — inherits the band; the other eight categories are
# exact.
BANDS = (
    (re.compile(r"^budget\.[^.]+\.(current|peak)$"), 4096),
    (re.compile(r"^budget\.[^.]+\.(allocs|frees)$"), 32),
    (re.compile(r"^budget-category\.[^.]+\.resource\.(current|peak)$"), 4096),
    (re.compile(r"^budget-category\.[^.]+\.resource\.(active|allocs|frees)$"),
     32),
    (re.compile(r"^engine-memory\.(current|peak)$"), 4096),
    (re.compile(r"^engine-memory\.active$"), 32),
)

TOKEN = re.compile(r"([a-z][a-z0-9-]*)=([^ \t]+)")


def treatment_for(counter: str) -> str:
    if counter.endswith(MASKED_SUFFIXES):
        return "masked"
    for pattern, width in BANDS:
        if pattern.match(counter):
            return f"band:{width}"
    return "exact"


def within(treatment: str, expected: str, got: str) -> tuple[bool, str]:
    """Compare one counter. Returns (ok, explanation-if-not)."""
    if treatment == "exact":
        return (got == expected, "")
    if treatment.startswith("band:"):
        tolerance = treatment.split(":", 1)[1]
        try:
            delta = abs(int(got) - int(expected))
            allowed = int(tolerance)
        except ValueError:
            return (False, f"band:{tolerance} needs integers")
        if delta > allowed:
            return (False, f"differs by {delta}, band allows {allowed}")
        return (True, "")
    return (False, f"unknown treatment {treatment!r}")


def harvest(log_path: Path) -> dict[str, str]:
    """Extract every cost counter from one validation log."""
    counters: dict[str, str] = {}
    text = log_path.read_text(encoding="utf-8", errors="replace")
    for line in text.splitlines():
        for prefix, label, key_fields in SPECS:
            index = line.find(prefix)
            if index < 0:
                continue
            fields = dict(TOKEN.findall(line[index + len(prefix):]))
            qualifier = ".".join(
                fields[key] for key in key_fields if key in fields)
            base = f"{label}.{qualifier}" if qualifier else label
            for key, value in fields.items():
                if key in key_fields:
                    continue
                # A later boot phase overwrites nothing: the phase is part
                # of the counter name, so each report stands on its own.
                counters[f"{base}.{key}"] = value
            break
    return counters


def missing_baseline(scenario: str) -> SystemExit:
    return SystemExit(
        f"no baseline counters for scenario {scenario!r} in "
        f"{BASELINE_PATH}")


def read_baseline(
        scenario: str, required: bool = True,
) -> dict[str, tuple[str, str]] | None:
    """The committed treatments and values for one scenario.

    `required=False` returns None for a scenario the baseline does not
    cover, so a caller that still has something useful to print can decide
    when to fail. menu-tour is measured and deliberately not baselined.
    """
    if not BASELINE_PATH.exists():
        raise SystemExit(
            f"missing {BASELINE_PATH}; regenerate it with "
            f"scripts/run-ppsspp-device-cost.sh --update-baseline")
    baseline: dict[str, tuple[str, str]] = {}
    for line in BASELINE_PATH.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#"):
            continue
        row_scenario, counter, treatment, value = line.split("\t")
        if row_scenario == scenario:
            baseline[counter] = (treatment, value)
    if not baseline:
        if not required:
            return None
        raise missing_baseline(scenario)
    return baseline


def write_baseline(scenario: str, counters: dict[str, str]) -> int:
    kept: list[str] = []
    for line in (BASELINE_PATH.read_text(encoding="utf-8").splitlines()
                 if BASELINE_PATH.exists() else []):
        if line.startswith("#") or not line:
            continue
        if line.split("\t", 1)[0] != scenario:
            kept.append(line)
    rows = []
    for counter in sorted(counters):
        treatment = treatment_for(counter)
        value = "-" if treatment == "masked" else counters[counter]
        rows.append(f"{scenario}\t{counter}\t{treatment}\t{value}")
    header = [
        "# Device cost baseline: what one hermetic PSP boot is allowed to "
        "cost.",
        "# scenario\tcounter\ttreatment\tvalue",
        "# treatment: exact | band:N (absolute tolerance) | masked "
        "(wall-clock, recorded not compared)",
        "# Regenerate: scripts/run-ppsspp-device-cost.sh --runs 2 "
        "--update-baseline",
        "# Every value below came from build-preset-psp-validation under "
        "PPSSPP; a shipping",
        "# build compiles printf to (0) and reports nothing.",
    ]
    BASELINE_PATH.write_text(
        "\n".join(header + sorted(kept) + rows) + "\n", encoding="utf-8")
    return len(rows)


def check_determinism(
        harvests: list[tuple[Path, dict[str, str]]],
        baseline: dict[str, tuple[str, str]] | None = None,
) -> tuple[list[str], list[str]]:
    """Return (failures, counters that drifted inside their treatment).

    A counter fails determinism only if it drifts further than the treatment
    it will be compared with allows: a masked counter moving is exactly what
    masking predicts, and a banded one moving inside its band is what the
    band was measured for.  Both are reported, neither is fatal.
    """
    first_path, first = harvests[0]
    failures: list[str] = []
    tolerated: list[str] = []
    for path, other in harvests[1:]:
        for counter in sorted(set(first) | set(other)):
            a = first.get(counter)
            b = other.get(counter)
            if a == b:
                continue
            entry = f"  {counter}: {first_path.name}={a} {path.name}={b}"
            # An existing baseline is authoritative. In particular, a row
            # deliberately tightened from a generic band to exact must not
            # pass determinism because only the first run happened to match.
            # While minting a baseline there is no prior declaration, so use
            # the generator's treatment policy.
            treatment = (baseline[counter][0]
                         if baseline is not None and counter in baseline
                         else treatment_for(counter))
            if treatment == "masked" or (
                    a is not None and b is not None
                    and within(treatment, a, b)[0]):
                tolerated.append(entry)
            else:
                failures.append(entry)
    return failures, tolerated


def compare(baseline: dict[str, tuple[str, str]],
            counters: dict[str, str]) -> list[str]:
    failures: list[str] = []
    for counter in sorted(baseline):
        treatment, expected = baseline[counter]
        got = counters.get(counter)
        if got is None:
            failures.append(
                f"  {counter}: baseline {expected} but the device reported "
                f"no such counter")
            continue
        if treatment == "masked":
            continue
        ok, explanation = within(treatment, expected, got)
        if not ok:
            failures.append(
                f"  {counter}: baseline {expected} device {got}"
                + (f" ({explanation})" if explanation else ""))
    for counter in sorted(set(counters) - set(baseline)):
        failures.append(
            f"  {counter}: device reported {counters[counter]} but the "
            f"baseline has no such counter")
    return failures


def main() -> int:
    arguments = sys.argv[1:]
    scenario = "start-page"
    update = False
    logs: list[Path] = []
    index = 0
    while index < len(arguments):
        argument = arguments[index]
        if argument == "--scenario":
            index += 1
            if index >= len(arguments):
                print(__doc__, file=sys.stderr)
                return 2
            scenario = arguments[index]
        elif argument.startswith("--scenario="):
            scenario = argument.split("=", 1)[1]
        elif argument == "--update":
            update = True
        elif argument in ("-h", "--help"):
            print(__doc__)
            return 0
        elif argument.startswith("-"):
            print(f"unknown option: {argument}", file=sys.stderr)
            return 2
        else:
            logs.append(Path(argument))
        index += 1
    if not logs:
        print(__doc__, file=sys.stderr)
        return 2

    harvests: list[tuple[Path, dict[str, str]]] = []
    for log in logs:
        if not log.exists():
            print(f"missing validation log: {log}", file=sys.stderr)
            return 2
        counters = harvest(log)
        if not counters:
            print(f"no cost counters in {log}; the EBOOT was probably not "
                  f"built with TILEFINCH_PSP_VALIDATION_LOG=ON",
                  file=sys.stderr)
            return 1
        harvests.append((log, counters))

    # Read the baseline before the determinism check, because that check
    # must use each counter's committed treatment rather than the
    # generator's default policy -- but do not fail on its absence yet. A
    # measured, deliberately unbaselined scenario (menu-tour) still has a
    # determinism report worth printing, and that report is the whole point
    # of the measure-only flow. The missing-baseline failure comes after it,
    # with the same message and the same exit code as before.
    baseline = None if update else read_baseline(scenario, required=False)
    if len(harvests) > 1:
        drift, tolerated = check_determinism(harvests, baseline)
        if drift:
            print(f"device cost is NOT deterministic across {len(harvests)} "
                  f"runs:", file=sys.stderr)
            print("\n".join(drift), file=sys.stderr)
            print("A baseline cannot be trusted until these agree.",
                  file=sys.stderr)
            return 1
        exact = sum(
            1 for counter in harvests[0][1]
            if (baseline[counter][0]
                if baseline is not None and counter in baseline
                else treatment_for(counter)) == "exact")
        print(f"determinism: {len(harvests)} runs agree on {exact} exact "
              f"counters ({len(tolerated)} masked-or-banded drifts, within "
              f"their declared treatment)")
    else:
        print("determinism: only one run supplied; not proven")

    counters = harvests[0][1]
    if update:
        written = write_baseline(scenario, counters)
        print(f"wrote {written} counters for {scenario} to {BASELINE_PATH}")
        return 0

    if baseline is None:
        raise missing_baseline(scenario)
    failures = compare(baseline, counters)
    treatments = [treatment for treatment, _v in baseline.values()]
    exact = treatments.count("exact")
    masked = treatments.count("masked")
    banded = len(treatments) - exact - masked
    if failures:
        print(f"device cost drifted for scenario {scenario}:", file=sys.stderr)
        print("\n".join(failures), file=sys.stderr)
        print("If the change is intentional, regenerate with:\n"
              "  scripts/run-ppsspp-device-cost.sh --runs 2 "
              "--update-baseline", file=sys.stderr)
        return 1
    print(f"device cost: {exact} exact counters match the baseline "
          f"({banded} banded, {masked} masked as wall-clock)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
