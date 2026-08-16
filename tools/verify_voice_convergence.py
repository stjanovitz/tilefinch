#!/usr/bin/env python3
"""Require exact recognition convergence between two fixed voice builds."""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import subprocess
import sys
from typing import Any


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Run every WAV in a corpus through an optimized voice probe and "
            "an oracle probe, requiring identical hypotheses and final scores."
        )
    )
    parser.add_argument("--optimized-probe", required=True, type=pathlib.Path)
    parser.add_argument("--oracle-probe", required=True, type=pathlib.Path)
    parser.add_argument("--hmm", required=True, type=pathlib.Path)
    parser.add_argument("--dict", dest="dictionary", required=True, type=pathlib.Path)
    parser.add_argument("--lm", required=True, type=pathlib.Path)
    parser.add_argument("--corpus", required=True, type=pathlib.Path)
    parser.add_argument(
        "--oracle-env",
        action="append",
        default=[],
        metavar="NAME=VALUE",
        help="Environment override for the oracle probe; may be repeated.",
    )
    parser.add_argument(
        "--max-optimized-bytes",
        type=int,
        help="Optional ceiling for the probe's decode_alloc_delta field.",
    )
    return parser.parse_args()


def run_probe(
    executable: pathlib.Path,
    hmm: pathlib.Path,
    dictionary: pathlib.Path,
    lm: pathlib.Path,
    wave: pathlib.Path,
    environment: dict[str, str] | None = None,
) -> dict[str, Any]:
    completed = subprocess.run(
        [str(executable), str(hmm), str(dictionary), str(lm), str(wave)],
        check=True,
        capture_output=True,
        text=True,
        env=environment,
    )
    for line in reversed(completed.stdout.splitlines()):
        try:
            result = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(result, dict):
            return result
    raise RuntimeError(f"{executable} emitted no JSON result for {wave}")


def main() -> int:
    args = parse_args()
    waves = sorted(args.corpus.glob("*.wav"))
    if not waves:
        raise RuntimeError(f"no WAV fixtures found in {args.corpus}")

    oracle_environment = os.environ.copy()
    for assignment in args.oracle_env:
        name, separator, value = assignment.partition("=")
        if not separator or not name:
            raise RuntimeError(f"invalid --oracle-env value: {assignment}")
        oracle_environment[name] = value

    failures: list[dict[str, Any]] = []
    optimized_decode_ms = 0.0
    oracle_decode_ms = 0.0
    optimized_peak_bytes = 0
    for wave in waves:
        optimized = run_probe(
            args.optimized_probe, args.hmm, args.dictionary, args.lm, wave
        )
        oracle = run_probe(
            args.oracle_probe,
            args.hmm,
            args.dictionary,
            args.lm,
            wave,
            oracle_environment,
        )
        actual = (optimized.get("hypothesis"), optimized.get("score"))
        expected = (oracle.get("hypothesis"), oracle.get("score"))
        if actual != expected:
            failures.append(
                {
                    "fixture": wave.name,
                    "optimized": {"hypothesis": actual[0], "score": actual[1]},
                    "oracle": {"hypothesis": expected[0], "score": expected[1]},
                }
            )
        optimized_decode_ms += float(optimized.get("decode_ms", 0.0))
        oracle_decode_ms += float(oracle.get("decode_ms", 0.0))
        optimized_peak_bytes = max(
            optimized_peak_bytes,
            int(optimized.get("decode_alloc_delta", 0)),
        )

    if (
        args.max_optimized_bytes is not None
        and optimized_peak_bytes > args.max_optimized_bytes
    ):
        failures.append(
            {
                "memory": {
                    "actual_bytes": optimized_peak_bytes,
                    "maximum_bytes": args.max_optimized_bytes,
                }
            }
        )

    report = {
        "fixtures": len(waves),
        "exact_matches": len(waves) - sum("fixture" in item for item in failures),
        "optimized_decode_ms": round(optimized_decode_ms, 3),
        "oracle_decode_ms": round(oracle_decode_ms, 3),
        "optimized_peak_bytes": optimized_peak_bytes,
        "failures": failures,
    }
    print(json.dumps(report, indent=2, sort_keys=True))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
