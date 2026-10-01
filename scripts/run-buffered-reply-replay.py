#!/usr/bin/env python3
"""Build and repeat a private buffered reply journey with completion checks.

The supplied commands must finish with a diagnostic returning JSON.stringify
of an object whose `answer` field is the actual rendered reply's textContent.
Capture-specific request-ID adaptation belongs in that private command file,
not in the browser or this runner. This measures local processing, not Wi-Fi
or server latency. Use an unused private output directory.

Each run's `tilefinch-work:` records (the deterministic work vector) are kept
in the manifest, and later runs are diffed against the first: a difference
means the runs did not execute the same work, so their timings are not a
like-for-like comparison.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from work_vector_report import compare, parse  # noqa: E402


def verify(log, expected):
    diagnostics = re.findall(r'loop-js ok=yes value="(.*)" error=""', log)
    if not diagnostics:
        raise ValueError("missing final reply diagnostic")
    value = json.loads(diagnostics[-1])
    if not isinstance(value, dict) or not isinstance(value.get("answer"), str):
        raise ValueError("final diagnostic must have an answer string")
    if expected not in value["answer"]:
        raise ValueError("captured answer did not render (wrong capture or stalled journey)")
    if "loop status=PASS" not in log or "interactive status=ok" not in log:
        raise ValueError("journey did not complete")
    if not re.search(r"memory-categories phase=interactive-teardown current=0 expected=0 .*reconcile=yes", log):
        raise ValueError("missing zero-owned teardown")
    return value["answer"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--responses", type=Path, required=True)
    parser.add_argument("--commands", type=Path, required=True)
    parser.add_argument("--url", required=True)
    parser.add_argument("--expect-text", required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--runs", type=int, choices=range(1, 11), default=3)
    parser.add_argument("--build-dir", type=Path, default=Path("build-preset-release"))
    parser.add_argument("--lab-target", choices=("psp-browser-interactive-lab",
                        "psp-browser-native-tier-native-lab",
                        "psp-browser-native-tier-reference-lab"),
                        default="psp-browser-interactive-lab")
    args = parser.parse_args()
    if not args.expect_text or not args.responses.is_dir() or not args.commands.is_file():
        parser.error("require a capture directory, command file and nonempty expected reply")
    args.output_dir.mkdir(parents=True, exist_ok=False)
    with (args.output_dir / "build.log").open("w") as log:
        subprocess.run(["cmake", "--build", str(args.build_dir), "--target",
                        args.lab_target, "-j8"], stdout=log,
                       stderr=subprocess.STDOUT, check=True)
    capture_hash = hashlib.sha256()
    for path in sorted(args.responses.iterdir()):
        if path.is_file():
            capture_hash.update(path.name.encode() + b"\0")
            with path.open("rb") as source:
                for chunk in iter(lambda: source.read(65536), b""):
                    capture_hash.update(chunk)
    manifest = {"capture_sha256": capture_hash.hexdigest(),
                "commands_sha256": hashlib.sha256(args.commands.read_bytes()).hexdigest(),
                "git_head": subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip(),
                "working_diff_sha256": hashlib.sha256(subprocess.check_output(["git", "diff", "HEAD"])).hexdigest(),
                "runs": []}
    environment = dict(os.environ)
    environment["TILEFINCH_JS_BOOT_WINDOW_KB"] = "4096"
    environment["TILEFINCH_REPLAY_IGNORE_REQUEST_BODY"] = "1"
    # Capture adaptation is a replay-only harness setting. The normal native
    # request policies still run; no request is sent to the live service.
    for index in range(args.runs):
        output = args.output_dir / f"run-{index + 1}"
        command = [str(args.build_dir.resolve() / args.lab_target),
                   "--url", args.url, "--fetch-scripts", "--psp-profile", "realistic",
                   "--limit-mb", "48", "--script-heap-mb", "10", "--script-count", "256",
                   "--script-total-mb", "2", "--script-file-kb", "512",
                   "--replay-http-response-keyed", str(args.responses.resolve()),
                   "--commands", str(args.commands.resolve()), "--no-loop-capture",
                   "--output", str(output.with_suffix(".ppm"))]
        with output.with_suffix(".log").open("w") as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                           env=environment, check=True, timeout=120)
        text = output.with_suffix(".log").read_text(errors="replace")
        answer = verify(text, args.expect_text)
        pixel_hash = hashlib.sha256(output.with_suffix(".ppm").read_bytes()).hexdigest()
        if manifest["runs"] and pixel_hash != manifest["runs"][0]["pixels_sha256"]:
            raise ValueError("repeated journey final pixels differ")
        work = parse(text)
        manifest["runs"].append({"pixels_sha256": pixel_hash,
                                 "answer_sha256": hashlib.sha256(answer.encode()).hexdigest(),
                                 "timing": re.findall(r"^performance-us .*", text, re.M)[-1],
                                 "work": [{"label": label, **fields}
                                          for label, fields in work]})
        if index == 0:
            first_work = work
            continue
        differences = compare(first_work, work, "")
        print(f"run {index + 1} work vector: "
              + ("identical to run 1" if not differences
                 else f"{len(differences)} differences from run 1"))
        for line in differences[:20]:
            print(f"  {line}")
    (args.output_dir / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Verified {args.runs} replies, identical pixels and zero-owned teardown: {args.output_dir}")


if __name__ == "__main__":
    main()
