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
import statistics
import subprocess
import sys
import time

try:
    import resource
except ImportError:  # Windows has no child-process rusage interface.
    resource = None

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
    if "errors" in value and value["errors"] != 0:
        raise ValueError("reply diagnostic reported author errors")
    if "loop status=PASS" not in log or "interactive status=ok" not in log:
        raise ValueError("journey did not complete")
    if not re.search(r"memory-categories phase=interactive-teardown current=0 expected=0 .*reconcile=yes", log):
        raise ValueError("missing zero-owned teardown")
    return value["answer"]


def window_metrics(log, labels):
    """Sum exclusive command wall intervals, not their nested phase timers.

    The first work marker must precede activation, not follow it. This is a
    buffered processing window, not an exact first-visible-pixel timestamp.
    """
    begin, end = labels
    markers = list(re.finditer(r"tilefinch-work: label=(\S+) ", log))
    starts = [m for m in markers if m[1] == begin]
    ends = [m for m in markers if m[1] == end]
    if len(starts) != 1 or len(ends) != 1 or starts[0].end() >= ends[0].start():
        raise ValueError("window requires unique, ordered work markers")
    rows = []
    for match in re.finditer(r"interaction-latency command=(\S+) ([^\n]+)",
                            log[starts[0].end():ends[0].start()]):
        fields = dict((key, int(value)) for key, value in
                      re.findall(r"([\w-]+)=(\d+)", match[2]))
        if "total-us" not in fields:
            raise ValueError("incomplete interaction timing row")
        rows.append(dict(command=match[1], **fields))
    if not rows:
        raise ValueError("window has no timed commands")
    return {"labels": list(labels), "command_wall_us": sum(r["total-us"] for r in rows),
            "commands": rows, "scope": "buffered command processing; nested phases are not additive"}


def replay_environment(source):
    environment = dict(source)
    environment["TILEFINCH_JS_BOOT_WINDOW_KB"] = "4096"
    environment["TILEFINCH_REPLAY_IGNORE_REQUEST_BODY"] = "1"
    # Timed native wrappers are attribution observers, not the timing Base.
    # An explicit setting remains available for a separate attribution run.
    environment.setdefault("TILEFINCH_SCRIPT_SPLIT", "0")
    return environment


def child_cpu_us():
    """Whole child-process CPU, not the activation/reply window's CPU."""
    if resource is None:
        return None
    usage = resource.getrusage(resource.RUSAGE_CHILDREN)
    return round((usage.ru_utime + usage.ru_stime) * 1_000_000)


def unique_work_record(records, label):
    matches = [fields for name, fields in records
               if name == label or name.startswith(label + "#")]
    if len(matches) != 1:
        raise ValueError(f"work label {label!r} must occur exactly once")
    return matches[0]


def reference_work_check(reference, current, reference_label, current_label, fields):
    """Check an explicit endpoint contract, not cross-machine speed equivalence.

    All other endpoint differences remain visible: matching selected DOM or
    admission counts does not imply identical VM, layout or scheduling work.
    """
    if not fields:
        raise ValueError("reference comparison requires explicit work fields")
    left = unique_work_record(reference, reference_label)
    right = unique_work_record(current, current_label)
    matched = {}
    for field in fields:
        expected, actual = left.get(field), right.get(field)
        if type(expected) is not int or type(actual) is not int or min(expected, actual) < 0:
            raise ValueError(f"reference contract {field}: both runs need nonnegative integer counts")
        if expected != actual:
            raise ValueError(f"reference contract {field}: expected {expected}, got {actual}")
        matched[field] = actual
    other = compare([("endpoint", left)], [("endpoint", right)], "")
    return {"reference_label": reference_label, "current_label": current_label,
            "matched": matched, "other_differences": other,
            "all_endpoint_work_equal": not other,
            "scope": "selected cumulative endpoint counts; not device timing or full execution parity"}


def page_replay_options(content_blocker, user_css):
    """Carry presentation/policy settings instead of assuming host defaults.

    User CSS is applied by the lab's ordinary user-stylesheet path. It does
    not reproduce the PSP frontend's initial-load ordering or native chrome.
    """
    if content_blocker not in ("off", "basic"):
        raise ValueError("content blocker must be off or basic")
    options = ["--content-blocker", content_blocker]
    evidence = {"content_blocker": content_blocker, "user_css_sha256": None}
    if user_css is not None:
        if not user_css.is_file() or user_css.stat().st_size > 65536:
            raise ValueError("user CSS must be an existing file of at most 64 KiB")
        evidence["user_css_sha256"] = hashlib.sha256(user_css.read_bytes()).hexdigest()
        options += ["--user-css", str(user_css.resolve())]
    return options, evidence


def ppm_region_evidence(raw, region):
    """Exact shared-page pixels; native chrome can be excluded explicitly."""
    header = re.match(rb"P6\s+(\d+)\s+(\d+)\s+255\s", raw)
    if header is None:
        raise ValueError("reference/frame must be a binary RGB PPM")
    width, height = map(int, header.groups())
    if not (0 < width <= 480 and 0 < height <= 272) or len(raw) - header.end() != width*height*3:
        raise ValueError("reference/frame has invalid dimensions or payload length")
    x, y, w, h = region
    if min(x, y) < 0 or min(w, h) <= 0 or x > width-w or y > height-h:
        raise ValueError("pixel-match region exceeds the frame")
    digest = hashlib.sha256()
    for row in range(y, y+h):
        begin = header.end() + (row*width+x)*3
        digest.update(raw[begin:begin+w*3])
    return {"dimensions": [width, height], "region": list(region),
            "region_sha256": digest.hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--responses", type=Path, required=True)
    parser.add_argument("--commands", type=Path, required=True)
    parser.add_argument("--url", required=True)
    parser.add_argument("--expect-text", required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--runs", type=int, choices=range(1, 11), default=3)
    parser.add_argument("--window-labels", nargs=2, metavar=("BEGIN", "END"),
                        help="unique work markers bracketing activation through reply")
    parser.add_argument("--require-work", action="append", default=[], metavar="FIELD=VALUE",
                        help="require an integer counter at the end marker (or final); repeatable")
    parser.add_argument("--reference-log", type=Path,
                        help="existing host/PSP work log to qualify selected endpoint counts against")
    parser.add_argument("--reference-label",
                        help="unique reference work label (defaults to this run's end marker)")
    parser.add_argument("--match-work", action="append", default=[], metavar="FIELD",
                        help="endpoint counter that must match the reference; repeatable, no implicit fields")
    parser.add_argument("--content-blocker", choices=("off", "basic"), default="off",
                        help="match the device's request policy; default preserves prior host behavior")
    parser.add_argument("--user-css", type=Path,
                        help="bounded presentation CSS matching device text scale/cosmetic settings")
    parser.add_argument("--reference-frame", type=Path,
                        help="qualified host/PSP RGB PPM for an exact shared-page pixel check")
    parser.add_argument("--match-region", nargs=4, type=int, metavar=("X", "Y", "W", "H"),
                        help="explicit reference pixel region, excluding native chrome if needed")
    parser.add_argument("--build-dir", type=Path, default=Path("build-preset-release"))
    parser.add_argument("--lab-target", choices=("psp-browser-interactive-lab",
                        "psp-browser-native-tier-native-lab",
                        "psp-browser-native-tier-reference-lab"),
                        default="psp-browser-interactive-lab")
    args = parser.parse_args()
    page_options, page_configuration = page_replay_options(args.content_blocker, args.user_css)
    if bool(args.reference_frame) != bool(args.match_region):
        parser.error("--reference-frame and --match-region are required together")
    reference_pixels = None
    if args.reference_frame:
        if args.reference_frame.stat().st_size > 480*272*3+256:
            raise ValueError("reference frame is oversized")
        raw = args.reference_frame.read_bytes()
        reference_pixels = ppm_region_evidence(raw, args.match_region)
        reference_pixels["frame_sha256"] = hashlib.sha256(raw).hexdigest()
    if bool(args.reference_log) != bool(args.match_work) or (args.reference_label and not args.reference_log):
        parser.error("--reference-log and at least one --match-work are required together")
    if any(not re.fullmatch(r"[\w.-]+", field) for field in args.match_work):
        parser.error("--match-work requires a work counter name")
    reference_bytes = args.reference_log.read_bytes() if args.reference_log else None
    reference_text = reference_bytes.decode(errors="replace") if reference_bytes is not None else None
    reference_records = parse(reference_text) if reference_text is not None else None
    final_label = args.window_labels[1] if args.window_labels else "final"
    reference_label = args.reference_label or final_label
    if reference_records is not None:
        # Refuse incomplete reference evidence before building/running anything.
        reference_work_check(reference_records, reference_records, reference_label,
                             reference_label, args.match_work)
    required = {}
    for item in args.require_work:
        field, separator, value = item.partition("=")
        if not separator or not re.fullmatch(r"[\w.-]+", field) or not value.isdigit():
            parser.error("--require-work requires FIELD=nonnegative-integer")
        required[field] = int(value)
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
                "binary_sha256": hashlib.sha256((args.build_dir / args.lab_target).read_bytes()).hexdigest(),
                "core_library_sha256": {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                                        for path in sorted(args.build_dir.glob("*tilefinch_core*"))
                                        if path.is_file() and path.suffix in (".dylib", ".so", ".dll", ".a")},
                "quickjs_flags": (args.build_dir / "CMakeFiles/qjs.dir/flags.make").read_text(),
                "work_requirements": required, "page_configuration": page_configuration, "runs": []}
    if reference_text is not None:
        manifest["reference"] = {
            "log_sha256": hashlib.sha256(reference_bytes).hexdigest(),
            "label": reference_label, "matched_fields": args.match_work,
            "scope": "selected endpoint work only; reference hardware/build provenance is separate"}
    if reference_pixels is not None:
        manifest["reference_pixels"] = reference_pixels
    environment = replay_environment(os.environ)
    diagnostic_keys = ("TILEFINCH_JS_BOOT_WINDOW_KB", "TILEFINCH_JS_LAZY_FUNCTIONS",
                       "TILEFINCH_REPLAY_IGNORE_REQUEST_BODY", "TILEFINCH_REPLAY_VOLATILE_UUIDS",
                       "TILEFINCH_REPLAY_PUMP_US", "TILEFINCH_TRACE_JS_PROFILE",
                       "TILEFINCH_JS_PROFILE_OUTLIER_US", "TILEFINCH_DISABLE_BOOTSTRAP_BYTECODE",
                       "TILEFINCH_SCRIPT_SPLIT", "TILEFINCH_EXECUTION_CENSUS")
    manifest["diagnostic_environment"] = {key: environment[key] for key in diagnostic_keys
                                           if key in environment}
    manifest["other_diagnostic_names"] = sorted(key for key in environment
                                                if key.startswith("TILEFINCH_") and key not in diagnostic_keys)
    # Capture adaptation is a replay-only harness setting. The normal native
    # request policies still run; no request is sent to the live service.
    for index in range(args.runs):
        if page_replay_options(args.content_blocker, args.user_css)[1] != page_configuration:
            raise ValueError("page settings changed during the replay batch")
        output = args.output_dir / f"run-{index + 1}"
        command = [str(args.build_dir.resolve() / args.lab_target),
                   "--url", args.url, "--fetch-scripts", "--psp-profile", "realistic",
                   "--limit-mb", "48", "--script-heap-mb", "10", "--script-count", "256",
                   "--script-total-mb", "2", "--script-file-kb", "512",
                   "--replay-http-response-keyed", str(args.responses.resolve()),
                   "--commands", str(args.commands.resolve()), "--no-loop-capture",
                   "--output", str(output.with_suffix(".ppm"))] + page_options
        with output.with_suffix(".log").open("w") as log:
            cpu_before = child_cpu_us()
            started = time.monotonic_ns()
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                           env=environment, check=True, timeout=120)
            process_us = (time.monotonic_ns() - started) // 1000
            cpu_after = child_cpu_us()
            process_cpu_us = (cpu_after - cpu_before
                              if cpu_before is not None and cpu_after is not None else None)
        text = output.with_suffix(".log").read_text(errors="replace")
        answer = verify(text, args.expect_text)
        frame_bytes = output.with_suffix(".ppm").read_bytes()
        pixel_hash = hashlib.sha256(frame_bytes).hexdigest()
        pixel_match = None
        if reference_pixels is not None:
            pixel_match = ppm_region_evidence(frame_bytes, args.match_region)
            if any(pixel_match[key] != reference_pixels[key]
                   for key in ("dimensions", "region", "region_sha256")):
                raise ValueError("shared-page reference pixels differ")
        if manifest["runs"] and pixel_hash != manifest["runs"][0]["pixels_sha256"]:
            raise ValueError("repeated journey final pixels differ")
        work = parse(text)
        final_work = unique_work_record(work, final_label) if required or reference_records is not None else {}
        for field, expected in required.items():
            if final_work.get(field) != expected:
                raise ValueError(f"{final_label}: {field} must be {expected}, got {final_work.get(field)!r}")
        differences = compare(first_work, work, "") if index else []
        reference_result = (reference_work_check(reference_records, work, reference_label,
                                                final_label, args.match_work)
                            if reference_records is not None else None)
        manifest["runs"].append({"pixels_sha256": pixel_hash,
                                 "answer_sha256": hashlib.sha256(answer.encode()).hexdigest(),
                                 "process_us": process_us,
                                 "process_cpu_us": process_cpu_us,
                                 "window": window_metrics(text, args.window_labels) if args.window_labels else None,
                                 "work_differences": differences,
                                 "reference_work": reference_result,
                                 "reference_pixels": pixel_match,
                                 "timing": re.findall(r"^performance-us .*", text, re.M)[-1],
                                 "work": [{"label": label, **fields}
                                          for label, fields in work]})
        if index == 0:
            first_work = work
            continue
        print(f"run {index + 1} work vector: "
              + ("identical to run 1" if not differences
                 else f"{len(differences)} differences from run 1"))
        for line in differences[:20]:
            print(f"  {line}")
    manifest["process_median_us"] = statistics.median(row["process_us"] for row in manifest["runs"])
    cpu_times = [row["process_cpu_us"] for row in manifest["runs"]
                 if row["process_cpu_us"] is not None]
    manifest["process_cpu_median_us"] = statistics.median(cpu_times) if cpu_times else None
    if args.window_labels:
        manifest["window_median_us"] = statistics.median(row["window"]["command_wall_us"] for row in manifest["runs"])
    manifest["all_work_vectors_equal"] = all(not row["work_differences"] for row in manifest["runs"])
    (args.output_dir / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Verified {args.runs} replies, identical pixels and zero-owned teardown: {args.output_dir}")
    print(f"Median process: {manifest['process_median_us'] / 1e6:.3f}s; "
          f"work vectors {'equal' if manifest['all_work_vectors_equal'] else 'differ (recorded; not identical-work evidence)'}")
    if args.window_labels:
        print(f"Median buffered command window: {manifest['window_median_us'] / 1e6:.3f}s")
    if reference_records is not None:
        print(f"Reference endpoint contract: {len(args.match_work)} named counters match in every run; "
              "other differences recorded (not a device-speed prediction)")


if __name__ == "__main__":
    main()
