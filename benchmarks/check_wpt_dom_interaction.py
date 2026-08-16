#!/usr/bin/env python3
"""Enforce the pinned, all-green DOM-interaction WPT result contract."""

import argparse
import json
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
EXPECTED_REVISION = (
    ROOT / "benchmarks" / "wpt" / "REVISION"
).read_text(encoding="utf-8").strip()
EXPECTED_FILES = 65
EXPECTED_PASSING_ASSERTIONS = 3512


def validate(payload: dict) -> list[str]:
    errors: list[str] = []
    if payload.get("schema") != 2:
        errors.append(f"unexpected results schema: {payload.get('schema')!r}")
    if payload.get("wpt_revision") != EXPECTED_REVISION:
        errors.append(
            "WPT revision changed: "
            f"{payload.get('wpt_revision')!r} != {EXPECTED_REVISION!r}"
        )
    if Path(str(payload.get("manifest", ""))).name != "dom-interaction.tsv":
        errors.append("results were not produced from dom-interaction.tsv")

    results = payload.get("results")
    if not isinstance(results, list):
        return errors + ["results is not a list"]
    if len(results) != EXPECTED_FILES:
        errors.append(f"expected {EXPECTED_FILES} files, got {len(results)}")

    names = [result.get("name") for result in results]
    if len(names) != len(set(names)):
        errors.append("result identifiers are not unique")

    passing_assertions = 0
    for result in results:
        name = result.get("name")
        status = result.get("status")
        fail_total = sum(
            int(result.get(field, 0) or 0)
            for field in (
                "fail_count",
                "timeout_count",
                "notrun_count",
                "skip_count",
            )
        )
        if fail_total:
            errors.append(f"{name}: {fail_total} non-passing assertion(s)")
        if status != "PASS":
            errors.append(f"{name}: expected PASS, got {status!r}")
        passing_assertions += int(result.get("pass_count", 0) or 0)
    if passing_assertions != EXPECTED_PASSING_ASSERTIONS:
        errors.append(
            f"expected {EXPECTED_PASSING_ASSERTIONS} passing engine "
            f"assertions, got {passing_assertions}"
        )

    totals = payload.get("totals")
    expected_totals = {
        "PASS": EXPECTED_FILES,
        "FAIL": 0,
        "SKIP": 0,
        "HARNESS-ERROR": 0,
    }
    if totals != expected_totals:
        errors.append(f"unexpected file totals: {totals!r}")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("results", type=Path)
    args = parser.parse_args()
    try:
        payload = json.loads(args.results.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        print(f"DOM-interaction WPT gate: invalid results: {error}",
              file=sys.stderr)
        return 2
    errors = validate(payload)
    if errors:
        for error in errors:
            print(f"DOM-interaction WPT gate: {error}", file=sys.stderr)
        return 1
    print(
        "DOM-interaction WPT gate: PASS "
        f"files={EXPECTED_FILES} engine-assertions="
        f"{EXPECTED_PASSING_ASSERTIONS}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
