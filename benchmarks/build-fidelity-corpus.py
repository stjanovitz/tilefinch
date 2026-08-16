#!/usr/bin/env python3
"""Drive a fidelity scenario's trace to Chrome-reference eligibility.

Starting from an engine-side capture (psp-browser-lab --capture-http),
Chrome usually wants more resources than the engine fetched (srcset
variants, lazy images, fonts, scripts).  Each capture-reference.js run
emits an exact acquisition plan for what was missing; this tool loops:

    capture-reference -> acquisition plan -> attested recorder fetch ->
    updated trace -> capture-reference ...

until the reference capture is eligible (or --max-rounds is exhausted).
The scenario manifest row's replay_dir and trace_sha256 are updated in
place after every successful acquisition round.

Usage:
    build-fidelity-corpus.py --scenario NAME --manifest FILE \
        --trace-root DIR --output-root DIR \
        [--recorder BIN] [--native-inventory BIN] [--max-rounds N]
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CAPTURE = ROOT / "benchmarks" / "capture-reference.js"

spec = importlib.util.spec_from_file_location(
    "trace_acquisition", ROOT / "benchmarks" / "acquire-trace-plan.py")
ACQUIRE = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ACQUIRE)


def manifest_row(manifest: Path, scenario: str) -> tuple[list[str], list[str]]:
    lines = manifest.read_text(encoding="utf-8").splitlines()
    for line in lines[1:]:
        if line.startswith(scenario + "\t"):
            return lines, line.split("\t")
    raise SystemExit(f"scenario {scenario!r} not in {manifest}")


def update_manifest(manifest: Path, scenario: str, replay_dir: str,
                    digest: str) -> None:
    lines = manifest.read_text(encoding="utf-8").splitlines()
    out = []
    for line in lines:
        if line.startswith(scenario + "\t"):
            parts = line.split("\t")
            parts[2] = replay_dir
            parts[3] = digest
            line = "\t".join(parts)
        out.append(line)
    manifest.write_text("\n".join(out) + "\n", encoding="utf-8")


def inspect_digest(trace: Path) -> str:
    completed = subprocess.run(
        ["node", str(CAPTURE), "--inspect-trace", str(trace)],
        capture_output=True, text=True)
    if completed.returncode != 0:
        raise SystemExit(f"inspect failed for {trace}: {completed.stderr}")
    return json.loads(completed.stdout)["trace_sha256"]


def run_capture(scenario: str, manifest: Path, trace_root: Path,
                output_root: Path) -> tuple[int, dict]:
    log = output_root / f"{scenario}-capture.log"
    with log.open("w") as sink:
        completed = subprocess.run(
            ["node", str(CAPTURE), "--scenario", scenario,
             "--trace-root", str(trace_root),
             "--output-root", str(output_root),
             "--manifest", str(manifest)],
            stdout=sink, stderr=subprocess.STDOUT)
    diagnostic = output_root / scenario / "reference-diagnostic.json"
    info = json.loads(diagnostic.read_text()) if diagnostic.exists() else {}
    return completed.returncode, info


def acquire(scenario: str, diagnostic: Path, source: Path, output: Path,
            recorder: str, inventory: str) -> bool:
    plan = json.loads(diagnostic.read_text()).get("acquisition_plan")
    if not plan or not plan.get("requests"):
        print(f"{scenario}: no acquisition plan available", file=sys.stderr)
        return False
    if not plan.get("complete"):
        print(f"{scenario}: acquisition plan incomplete (dynamic URLs; "
              "site cannot be made hermetic as configured)", file=sys.stderr)
        return False
    requests = plan["requests"]
    origins = sorted({u.split("/")[0] + "//" + u.split("/")[2]
                      for u in [r["url"] for r in requests]})
    sha, _ = ACQUIRE.trace_digest(source, 512 * 1024 * 1024)
    info, _ = ACQUIRE._metadata(source / "trace.meta")
    contract = {
        "schema": 1, "name": scenario,
        "diagnostic_sha256": hashlib.sha256(
            diagnostic.read_bytes()).hexdigest(),
        "allowlist_sha256": ACQUIRE._allowlist_digest(requests),
        "source_trace_sha256": sha,
        "source_record_count": int(info["record-count"]),
        "source_origin_ms": int(info["origin-ms"]),
        "request_count": len(requests),
        "max_response_bytes": 8 * 1024 * 1024,
        "timeout_ms": 20000,
        "max_trace_bytes": 512 * 1024 * 1024,
        "allowed_methods": ["GET", "HEAD"],
        "allowed_schemes": ["https"],
        "allowed_origins": origins,
    }
    contract_path = source.parent / f"{scenario}-contract.json"
    contract_path.write_text(json.dumps(contract, indent=1))
    completed = subprocess.run(
        [sys.executable, str(ROOT / "benchmarks" / "acquire-trace-plan.py"),
         "--contract", str(contract_path),
         "--diagnostic", str(diagnostic),
         "--source-trace", str(source),
         "--output-trace", str(output),
         "--recorder", recorder,
         "--native-inventory", inventory],
        capture_output=True, text=True)
    if completed.returncode != 0:
        print(f"{scenario}: acquisition failed: "
              f"{completed.stderr.strip()[-300:]}", file=sys.stderr)
        return False
    return True


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--scenario", required=True)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--trace-root", required=True, type=Path)
    parser.add_argument("--output-root", required=True, type=Path)
    parser.add_argument("--recorder",
                        default="build-dev/psp-browser-trace-acquire")
    parser.add_argument("--native-inventory",
                        default="build-dev/psp-browser-trace-inventory")
    parser.add_argument("--max-rounds", type=int, default=5)
    args = parser.parse_args()

    _, row = manifest_row(args.manifest, args.scenario)
    replay_dir = row[2]
    for round_index in range(args.max_rounds):
        code, info = run_capture(args.scenario, args.manifest,
                                 args.trace_root, args.output_root)
        if code == 0:
            print(f"{args.scenario}: eligible after {round_index} "
                  f"acquisition round(s); replay_dir={replay_dir}")
            return 0
        diagnostic = args.output_root / args.scenario / \
            "reference-diagnostic.json"
        if not diagnostic.exists():
            print(f"{args.scenario}: capture failed without diagnostic "
                  f"(rc={code}); see capture log", file=sys.stderr)
            return 1
        source = args.trace_root / replay_dir
        suffix = round_index + 1
        while (args.trace_root / f"{args.scenario}-r{suffix}").exists():
            suffix += 1
        next_dir = f"{args.scenario}-r{suffix}"
        output = args.trace_root / next_dir
        if not acquire(args.scenario, diagnostic, source, output,
                       args.recorder, args.native_inventory):
            return 1
        replay_dir = next_dir
        update_manifest(args.manifest, args.scenario, replay_dir,
                        inspect_digest(output))
    print(f"{args.scenario}: not eligible after {args.max_rounds} rounds",
          file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
