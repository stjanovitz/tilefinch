#!/usr/bin/env python3
"""Offline, resumable full-document references for a pinned scenario manifest.

This runner never acquires resources or changes exclusions. Each attempt has
its own directory, logs, source identity, and explicit completion status.
Missing resources produce the maintained exact acquisition plan. Use the
separate approved acquisition workflow, then rerun with the new trace pin.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import time

from visual_scenario import load_manifest, trace_digest

HERE = Path(__file__).resolve().parent
CAPTURE = HERE / "capture-reference.js"
MAX_ARTIFACT_BYTES = 1024 * 1024 * 1024


def file_digest(path: Path) -> str:
    if path.is_symlink() or not path.is_file():
        raise ValueError(f"not a regular artifact: {path}")
    if path.stat().st_size > MAX_ARTIFACT_BYTES:
        raise ValueError(f"artifact exceeds capture bound: {path}")
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def toolchain_preflight(node: Path, executable: Path | None) -> tuple[Path, dict]:
    """Fail once, before a batch, and pin the browser actually launched."""
    program = r'''(async () => {
      const p = require('playwright');
      const executable = process.argv[1] || p.chromium.executablePath();
      const browser = await p.chromium.launch({headless:true, executablePath:executable,
        args:['--disable-background-networking','--host-resolver-rules=MAP * ~NOTFOUND']});
      try { console.log(JSON.stringify({executable, version:browser.version(),
        package:require.resolve('playwright/package.json')})); }
      finally { await browser.close(); }
    })().catch(e => { console.error(e.message); process.exitCode=1; });'''
    result = subprocess.run([str(node), "-e", program, str(executable or "")],
                            capture_output=True, text=True, timeout=45)
    if result.returncode or len(result.stdout) > 65536:
        raise ValueError("browser preflight failed: " + result.stderr[-2048:])
    info = json.loads(result.stdout)
    actual = Path(info["executable"]).resolve(strict=True)
    package = Path(info["package"]).resolve(strict=True)
    # PNG normalization is dependency-free; check the actual entry point,
    # not an optional JPEG decoder which these screenshots never need.
    check = subprocess.run([sys.executable, str(HERE / "normalize-reference-frame.py"), "--help"],
                           capture_output=True, text=True, timeout=15)
    if check.returncode:
        raise ValueError("capture PNG normalizer preflight failed")
    return actual, {"browser_version": info["version"],
                    "playwright_package_sha256": file_digest(package),
                    "python_executable_sha256": file_digest(Path(sys.executable).resolve())}


def source_identity(manifest: Path, node: Path, executable: Path) -> dict:
    files = [manifest, CAPTURE, HERE / "reference-capture-audit.js",
             HERE / "normalize-reference-frame.py", HERE / "reference_frame.py",
             HERE / "visual_scenario.py", Path(__file__), node, executable]
    return {str(path.resolve()): file_digest(path.resolve()) for path in files}


def complete_state(state) -> bool:
    """Validate the completion evidence, not only its aggregate success flag."""
    if not isinstance(state, dict) or state.get("capture_ready") is not True:
        return False
    audit, targets, full = (state.get(name) for name in
                            ("browser_request_audit", "target_interception", "full_document"))
    if (not isinstance(audit, dict) or audit.get("ready") is not True
            or audit.get("pending") != 0 or audit.get("overflow") is not False
            or type(audit.get("observed")) is not int or audit["observed"] < 1
            or audit.get("intercepted") != audit["observed"]
            or not isinstance(audit.get("failures"), list)
            or any(not isinstance(failure, dict) or failure.get("expected") is not True
                   for failure in audit["failures"])
            or not isinstance(targets, dict) or targets.get("ready") is not True
            or targets.get("pending_commands") != 0 or targets.get("failures") != 0
            or targets.get("overflow") is not False
            or not isinstance(full, dict) or full.get("complete") is not True
            or full.get("version") != "bounded-viewport-sweep-v2"
            or full.get("full_screenshot") is not False
            or full.get("full_screenshot_status") != "omitted-mobile-emulation"):
        return False
    views, final, viewport = (full.get(name) for name in ("viewports", "final_extent", "viewport_css"))
    if (not isinstance(views, list) or len(views) != 5
            or not isinstance(final, dict) or not isinstance(viewport, dict)
            or any(type(viewport.get(axis)) is not int or not 1 <= viewport[axis] <= 16384
                   for axis in ("width", "height"))
            or not isinstance(state.get("checkpoints"), list) or not state["checkpoints"]):
        return False
    for percentage, view in zip((0, 25, 50, 75, 100), views):
        if (not isinstance(view, dict) or view.get("fraction") != percentage / 100
                or view.get("frame") != f"view-{percentage}.png" or view.get("ready") is not True
                or view.get("incomplete") != 0 or view.get("truncated") is not False):
            return False
        for field, maximum in (("document_width", 2048), ("document_height", 61440),
                               ("maximum", 61440), ("scroll_y", 61440), ("requested", 61440)):
            if type(view.get(field)) is not int or not 0 <= view[field] <= maximum:
                return False
        if (view["document_width"] < 1 or view["document_height"] < 1
                or view["scroll_y"] > view["maximum"]
                or abs(view["scroll_y"] - int(view["maximum"] * percentage / 100)) > 2):
            return False
        if any(view[field] != views[0][field] for field in
               ("document_width", "document_height", "maximum")):
            return False
        profile = view.get("input_profile")
        if not isinstance(profile, dict):
            return False
        for point in ("before", "after"):
            value = profile.get(point)
            if (not isinstance(value, dict) or value.get("coarse") is not True
                    or value.get("touch") != 1 or value.get("width") != viewport["width"]
                    or value.get("height") != viewport["height"]):
                return False
    if (views[0]["scroll_y"] != 0 or views[-1]["scroll_y"] != views[-1]["maximum"]
            or final.get("ready") is not True or final.get("incomplete") != 0
            or final.get("width") != views[-1]["document_width"]
            or final.get("height") != views[-1]["document_height"]
            or final.get("maximum") != views[-1]["maximum"]):
        return False
    return all(isinstance(checkpoint, dict) and isinstance(checkpoint.get("frame"), str)
               and checkpoint["frame"] for checkpoint in state["checkpoints"])


def required_artifacts(state: dict, attempt: Path, output: Path) -> list[Path]:
    paths = [attempt / "stdout.log", attempt / "stderr.log",
             output / "reference-state.json", output / "capture-audit.json"]
    paths.extend(output / name for name in ("view-0.png", "view-25.png",
                 "view-50.png", "view-75.png", "view-100.png", "top.png", "bottom.png"))
    paths.extend(output / checkpoint["frame"] for checkpoint in state["checkpoints"])
    archived = state["full_document"].get("geometry_attempts", [])
    if not isinstance(archived, list) or len(archived) > 1:
        raise ValueError("successful capture exceeds its geometry retry bound")
    for generation in archived:
        if (not isinstance(generation, dict) or generation.get("directory") != "geometry-attempt-1"
                or not isinstance(generation.get("views"), list)
                or any(not isinstance(view, dict) for view in generation["views"])
                or [view.get("frame") for view in generation["views"]]
                != [f"view-{percentage}.png" for percentage in (0, 25, 50, 75, 100)]):
            raise ValueError("capture geometry retry evidence is malformed")
        directory = output / generation["directory"]
        paths.extend(directory / name for name in ("view-0.png", "view-25.png",
                     "view-50.png", "view-75.png", "view-100.png"))
    return paths


def run_bounded(command, stdout, stderr, timeout) -> int:
    """Own one process group, including Chromium children, on timeout."""
    process = subprocess.Popen(command, stdout=stdout, stderr=stderr,
                               stdin=subprocess.DEVNULL, start_new_session=True)
    try:
        return process.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGTERM)
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait(timeout=10)
        return 124


def receipt_valid(receipt: dict, identity: dict, root: Path) -> bool:
    if (not isinstance(receipt, dict) or receipt.get("identity") != identity or receipt.get("status") != "complete"
            or receipt.get("exit") != 0):
        return False
    artifacts = receipt.get("artifacts")
    if not isinstance(artifacts, dict) or not 1 <= len(artifacts) <= 32:
        return False
    try:
        attempt = Path(receipt["attempt"])
        output = Path(receipt["output"])
        if not attempt.resolve().is_relative_to(root.resolve()) or output != attempt / receipt["scenario"]:
            return False
        state = json.loads((output / "reference-state.json").read_text())
        if not complete_state(state):
            return False
        audit_path = output / "capture-audit.json"
        audit = json.loads(audit_path.read_text())
        if not isinstance(audit, dict) or audit.get("state_sha256") != file_digest(output / "reference-state.json"):
            return False
        required = required_artifacts(state, attempt, output)
        if not {str(path.relative_to(root)) for path in required}.issubset(artifacts):
            return False
        for relative, digest in artifacts.items():
            path = root / relative
            if path.is_symlink() or not path.resolve().is_relative_to(root.resolve()):
                return False
            if file_digest(path) != digest:
                return False
    except (OSError, ValueError, KeyError, TypeError):
        return False
    return True


def save(path: Path, value: dict) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2) + "\n")
    temporary.replace(path)


def capture_one(scenario, options, sources) -> dict:
    root = options.output_root / scenario.scenario
    root.mkdir(parents=True, exist_ok=True)
    identity = {"sources": sources, "trace_sha256": scenario.trace_sha256,
                "full_document": True, "settle_ms": options.settle_ms,
                "timeout_ms": options.timeout_ms, "attempt_timeout_seconds": options.attempt_timeout_seconds}
    trace = options.trace_root / scenario.replay_dir
    try:
        if not trace.resolve().is_relative_to(options.trace_root.resolve()):
            raise ValueError("trace escapes capture root")
        if trace_digest(trace) != scenario.trace_sha256:
            raise ValueError("manifest trace pin differs from retained input")
        latest = root / "latest.json"
        if options.resume and latest.exists():
            try:
                prior = json.loads(latest.read_text())
                if receipt_valid(prior, identity, root):
                    return {**prior, "resumed": True}
            except (OSError, ValueError, TypeError):
                pass  # Corrupt or partial receipts always trigger a fresh attempt.
        attempt = root / f"attempt-{time.time_ns()}"
        attempt.mkdir()
        command = [str(options.node), str(CAPTURE), "--full-document",
                   "--scenario", scenario.scenario, "--manifest", str(options.manifest),
                   "--trace-root", str(options.trace_root), "--output-root", str(attempt),
                   "--timeout-ms", str(options.timeout_ms), "--settle-ms", str(options.settle_ms),
                   "--python", sys.executable]
        if options.executable:
            command.extend(["--executable", str(options.executable)])
        with (attempt / "stdout.log").open("w") as stdout, (attempt / "stderr.log").open("w") as stderr:
            # Browser operations close normally first; this outer bound also
            # contains startup/helper hangs and owns child cleanup.
            code = run_bounded(command, stdout, stderr, options.attempt_timeout_seconds)
        output = attempt / scenario.scenario
        state_path = output / ("reference-state.json" if code == 0 else "reference-diagnostic.json")
        state = json.loads(state_path.read_text()) if state_path.exists() else {}
        if not isinstance(state, dict):
            raise ValueError("capture published a non-object state")
        complete = code == 0 and complete_state(state)
        unchanged = (trace_digest(trace) == scenario.trace_sha256
                     and all(file_digest(Path(name)) == digest for name, digest in sources["files"].items()))
        if not unchanged:
            complete = False
            state["eligibility_reasons"] = [*state.get("eligibility_reasons", []),
                                            "capture inputs or tooling changed during this attempt"]
        paths = [attempt / "stdout.log", attempt / "stderr.log"]
        if state_path.exists():
            paths.append(state_path)
        if (output / "capture-audit.json").exists():
            paths.append(output / "capture-audit.json")
        if complete:
            paths = required_artifacts(state, attempt, output)
        receipt = {"scenario": scenario.scenario, "identity": identity,
                   "status": "complete" if complete else "incomplete", "exit": code,
                   "output": str(output), "attempt": str(attempt),
                   "reasons": state.get("eligibility_reasons", ["capture failed before state publication"]),
                   "acquisition_plan": state.get("acquisition_plan"),
                   "artifacts": {str(path.relative_to(root)): file_digest(path) for path in paths}}
    except (OSError, ValueError, KeyError, TypeError) as error:
        receipt = {"scenario": scenario.scenario, "identity": identity,
                   "status": "incomplete", "reasons": [str(error)], "artifacts": {}}
    save(root / "latest.json", receipt)
    return receipt


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--trace-root", required=True, type=Path)
    parser.add_argument("--output-root", required=True, type=Path)
    parser.add_argument("--scenario", action="append")
    parser.add_argument("--node", type=Path, default=Path(shutil.which("node") or "node"))
    parser.add_argument("--executable", type=Path)
    parser.add_argument("--jobs", type=int, default=3, choices=range(1, 5))
    parser.add_argument("--timeout-ms", type=int, default=15000)
    parser.add_argument("--settle-ms", type=int, default=100)
    parser.add_argument("--attempt-timeout-seconds", type=int, default=300)
    parser.add_argument("--resume", action="store_true")
    options = parser.parse_args()
    if not 1000 <= options.timeout_ms <= 120000 or not 0 <= options.settle_ms <= 10000:
        parser.error("invalid bounded browser timeout or settle interval")
    if not 30 <= options.attempt_timeout_seconds <= 900:
        parser.error("whole-attempt timeout must be between 30 and 900 seconds")
    options.manifest = options.manifest.resolve(strict=True)
    options.trace_root = options.trace_root.resolve(strict=True)
    options.output_root = options.output_root.resolve()
    options.output_root.mkdir(parents=True, exist_ok=True)
    options.node = options.node.resolve(strict=True)
    scenarios = load_manifest(options.manifest)
    if options.scenario:
        wanted = set(options.scenario)
        scenarios = [scenario for scenario in scenarios if scenario.scenario in wanted]
        if {scenario.scenario for scenario in scenarios} != wanted:
            parser.error("requested scenario is missing from the manifest")
    if len(scenarios) > 256:
        parser.error("census exceeds 256 scenarios")
    try:
        options.executable, toolchain = toolchain_preflight(options.node, options.executable)
        sources = {"files": source_identity(options.manifest, options.node, options.executable),
                   "toolchain": toolchain}
    except (OSError, ValueError, KeyError, subprocess.TimeoutExpired) as error:
        parser.error(str(error))
    with concurrent.futures.ThreadPoolExecutor(max_workers=options.jobs) as pool:
        results = list(pool.map(lambda scenario: capture_one(scenario, options, sources), scenarios))
    summary = {"schema": 1, "offline": True, "complete": all(row["status"] == "complete" for row in results),
               "scenarios": results}
    save(options.output_root / "census.json", summary)
    for row in results:
        print(f"{row['scenario']}: {row['status']}" + (" (verified resume)" if row.get("resumed") else ""))
    return 0 if summary["complete"] else 3


if __name__ == "__main__":
    sys.exit(main())
