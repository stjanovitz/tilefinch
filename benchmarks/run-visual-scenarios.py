#!/usr/bin/env python3
"""Run and qualify deterministic multi-checkpoint visual scenarios."""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

from visual_scenario import (
    ArtifactError,
    Eligibility,
    ManifestError,
    PENDING_DIGEST,
    REFERENCE_PROOF_VERSION,
    candidate_state_from_log,
    commands_for,
    load_manifest,
    load_state,
    qualify,
    summary_row,
    trace_digest,
    trace_replay_origin_ms,
    validate_reference_replay_contract,
    verify_candidate_state_binding,
    verify_reference_proof_binding,
    write_state,
    write_summary,
)


ROOT = Path(__file__).resolve().parents[1]


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("build_dir", type=Path, nargs="?")
    parser.add_argument("replay_root", type=Path, nargs="?")
    parser.add_argument("reference_root", type=Path, nargs="?")
    parser.add_argument("output_dir", type=Path, nargs="?")
    parser.add_argument(
        "--manifest",
        type=Path,
        default=ROOT / "benchmarks" / "visual-scenarios.tsv",
    )
    parser.add_argument("--scenario", help="run one manifest row")
    parser.add_argument(
        "--qualify-only",
        action="store_true",
        help="do not invoke the lab; validate existing candidate artifacts",
    )
    parser.add_argument(
        "--require-reference-proof",
        action="store_true",
        help=(
            "require a two-full-run determinism proof; reference_root must be "
            "the proof root or its canonical run-a directory"
        ),
    )
    parser.add_argument(
        "--trace-digest",
        type=Path,
        help="print the deterministic digest for one trace and exit",
    )
    return parser.parse_args()


def not_comparable(
    scenario: str,
    digest: str,
    detail: str,
    count: int,
    reference_proof: str = "legacy-unbound",
) -> Eligibility:
    return Eligibility(
        scenario, "NOT_COMPARABLE", (detail,), digest, count, reference_proof
    )


def with_reference_proof(result: Eligibility, contract: str) -> Eligibility:
    return Eligibility(
        scenario=result.scenario,
        status=result.status,
        details=result.details,
        trace_sha256=result.trace_sha256,
        checkpoint_count=result.checkpoint_count,
        reference_proof=contract,
    )


def main() -> int:
    args = parse_arguments()
    if args.trace_digest is not None:
        try:
            print(trace_digest(args.trace_digest))
            return 0
        except (OSError, ArtifactError) as error:
            print(f"trace digest failed: {error}", file=sys.stderr)
            return 2
    if any(
        value is None
        for value in (args.build_dir, args.replay_root, args.reference_root, args.output_dir)
    ):
        print(
            "build_dir, replay_root, reference_root, and output_dir are required",
            file=sys.stderr,
        )
        return 2
    try:
        scenarios = load_manifest(args.manifest)
    except ManifestError as error:
        print(f"visual manifest invalid: {error}", file=sys.stderr)
        return 2
    if args.scenario:
        scenarios = [item for item in scenarios if item.scenario == args.scenario]
        if not scenarios:
            print(f"scenario not found: {args.scenario}", file=sys.stderr)
            return 2
    if args.require_reference_proof and len(scenarios) != 1:
        print(
            "--require-reference-proof needs exactly one selected scenario; use --scenario",
            file=sys.stderr,
        )
        return 2

    build = args.build_dir if args.build_dir.is_absolute() else ROOT / args.build_dir
    lab = build / "psp-browser-interactive-lab"
    if not args.qualify_only and not os.access(lab, os.X_OK):
        print(f"build the interactive lab first: {lab}", file=sys.stderr)
        return 2
    args.output_dir.mkdir(parents=True, exist_ok=True)
    rows: list[tuple[str, ...]] = []
    incomplete = False
    for scenario in scenarios:
        scenario_output = args.output_dir / scenario.scenario
        candidate_state = scenario_output / "candidate-state.json"
        replay = args.replay_root / scenario.replay_dir
        reference_state = args.reference_root / scenario.reference_state
        reference_proof = "legacy-unbound"
        if args.require_reference_proof:
            reference_proof = REFERENCE_PROOF_VERSION
            try:
                binding = verify_reference_proof_binding(
                    scenario, replay, args.reference_root
                )
                reference_state = binding.reference_state_path
            except (OSError, ArtifactError) as error:
                result = not_comparable(
                    scenario.scenario,
                    scenario.trace_sha256,
                    f"reference-proof-invalid:{error}",
                    len(scenario.checkpoints),
                    f"{REFERENCE_PROOF_VERSION}:invalid",
                )
                rows.append(summary_row(scenario, result))
                incomplete = True
                continue
        if not args.qualify_only:
            scenario_output.mkdir(parents=True, exist_ok=True)
            try:
                candidate_state.unlink()
            except FileNotFoundError:
                pass
            if scenario.trace_sha256 == PENDING_DIGEST:
                result = not_comparable(
                    scenario.scenario,
                    scenario.trace_sha256,
                    "trace-digest-pending",
                    len(scenario.checkpoints),
                    reference_proof,
                )
                rows.append(summary_row(scenario, result))
                incomplete = True
                continue
            try:
                actual = trace_digest(replay)
                origin_ms = trace_replay_origin_ms(replay)
            except (OSError, ArtifactError) as error:
                result = not_comparable(
                    scenario.scenario,
                    scenario.trace_sha256,
                    f"replay-trace-unavailable:{error}",
                    len(scenario.checkpoints),
                    reference_proof,
                )
                rows.append(summary_row(scenario, result))
                incomplete = True
                continue
            try:
                reference = load_state(reference_state)
                contract_reasons = validate_reference_replay_contract(
                    scenario, reference, origin_ms
                )
            except ArtifactError as error:
                contract_reasons = [f"reference-state-invalid:{error}"]
            if contract_reasons:
                result = not_comparable(
                    scenario.scenario,
                    scenario.trace_sha256,
                    "reference-replay-contract-invalid:"
                    + ",".join(dict.fromkeys(contract_reasons)),
                    len(scenario.checkpoints),
                    reference_proof,
                )
                rows.append(summary_row(scenario, result))
                incomplete = True
                continue
            if actual != scenario.trace_sha256:
                result = not_comparable(
                    scenario.scenario,
                    scenario.trace_sha256,
                    "replay-trace-mismatch",
                    len(scenario.checkpoints),
                    reference_proof,
                )
                rows.append(summary_row(scenario, result))
                incomplete = True
                continue
            commands = scenario_output / "commands.txt"
            try:
                command_text = commands_for(scenario)
            except ArtifactError as error:
                result = not_comparable(
                    scenario.scenario,
                    scenario.trace_sha256,
                    f"candidate-checkpoint-unsupported:{error}",
                    len(scenario.checkpoints),
                    reference_proof,
                )
                rows.append(summary_row(scenario, result))
                incomplete = True
                continue
            commands.write_text(command_text, encoding="utf-8")
            log_path = scenario_output / "candidate.log"
            frames = scenario_output / "frames"
            command = (
                str(lab),
                "--url",
                scenario.url,
                "--replay-http-response-keyed",
                str(replay),
                "--deterministic-replay-seed",
                str(origin_ms),
                "--fetch-scripts",
                "--ticks",
                str(scenario.ticks),
                "--tick-ms",
                str(scenario.tick_ms),
                "--limit-mb",
                str(scenario.limit_mb),
                "--max-download-kb",
                str(scenario.max_download_kb),
                "--script-timeout-ms",
                str(scenario.script_timeout_ms),
                "--script-heap-mb",
                str(scenario.script_heap_mb),
                "--script-total-mb",
                str(scenario.script_total_mb),
                "--script-file-kb",
                str(scenario.script_file_kb),
                "--script-count",
                str(scenario.script_count),
                "--visual-state-marker",
                scenario.required_state_marker,
                "--visual-evidence",
                "--commands",
                str(commands),
                "--loop-output-dir",
                str(frames),
                "--output",
                str(scenario_output / "initial.ppm"),
            )
            completed = subprocess.run(command, capture_output=True)
            log = (completed.stdout + completed.stderr).decode(
                "utf-8", errors="replace"
            )
            log_path.write_text(log, encoding="utf-8")
            if completed.returncode == 0:
                try:
                    state = candidate_state_from_log(
                        scenario, log, frames, origin_ms=origin_ms
                    )
                    write_state(candidate_state, state)
                except ArtifactError as error:
                    result = not_comparable(
                        scenario.scenario,
                        scenario.trace_sha256,
                        f"candidate-evidence-invalid:{error}",
                        len(scenario.checkpoints),
                        reference_proof,
                    )
                    rows.append(summary_row(scenario, result))
                    incomplete = True
                    continue
            else:
                result = not_comparable(
                    scenario.scenario,
                    scenario.trace_sha256,
                    f"candidate-run-failed:{completed.returncode}",
                    len(scenario.checkpoints),
                    reference_proof,
                )
                rows.append(summary_row(scenario, result))
                incomplete = True
                continue
        binding_detail: str | None = None
        if args.qualify_only:
            try:
                verify_candidate_state_binding(
                    scenario,
                    replay,
                    candidate_state,
                    scenario_output / "candidate.log",
                    scenario_output / "frames",
                )
            except (OSError, ArtifactError) as error:
                binding_detail = f"candidate-binding-invalid:{error}"

        if args.require_reference_proof:
            try:
                binding = verify_reference_proof_binding(
                    scenario, replay, args.reference_root
                )
                if binding.reference_state_path != reference_state:
                    raise ArtifactError("canonical reference state path changed")
            except (OSError, ArtifactError) as error:
                proof_result = not_comparable(
                    scenario.scenario,
                    scenario.trace_sha256,
                    f"reference-proof-invalid:{error}",
                    len(scenario.checkpoints),
                    f"{REFERENCE_PROOF_VERSION}:invalid",
                )
                rows.append(summary_row(scenario, proof_result))
                incomplete = True
                continue

        result = qualify(scenario, replay, reference_state, candidate_state)
        if binding_detail is not None:
            result = Eligibility(
                scenario=result.scenario,
                status="NOT_COMPARABLE",
                details=(binding_detail,)
                + tuple(detail for detail in result.details if detail != "ok"),
                trace_sha256=result.trace_sha256,
                checkpoint_count=result.checkpoint_count,
                reference_proof=reference_proof,
            )
        else:
            result = with_reference_proof(result, reference_proof)
        if not args.require_reference_proof:
            result = Eligibility(
                scenario=result.scenario,
                status="NOT_COMPARABLE",
                details=("reference-proof-required",)
                + tuple(detail for detail in result.details if detail != "ok"),
                trace_sha256=result.trace_sha256,
                checkpoint_count=result.checkpoint_count,
                reference_proof="legacy-unbound",
            )
        rows.append(summary_row(scenario, result))
        incomplete |= result.status != "ELIGIBLE"

    write_summary(args.output_dir / "summary.tsv", rows)
    print((args.output_dir / "summary.tsv").read_text(encoding="utf-8"), end="")
    return 3 if incomplete else 0


if __name__ == "__main__":
    raise SystemExit(main())
