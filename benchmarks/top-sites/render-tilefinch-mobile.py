#!/usr/bin/env python3
"""Render local-only Tilefinch peers for mobile browser references."""

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import json
from pathlib import Path
import re
import subprocess
from typing import Optional

from PIL import Image


POSITIONS = ("top", "middle", "bottom")
VIEWPORT_HEIGHT = 272
SCROLL_TOLERANCE = 2
MIDDLE_CONVERGENCE_ATTEMPTS = 4
SCROLL_STATUS = re.compile(
    r"^interactive status=ok .*? height=(\d+).*? scroll-y=(-?\d+) "
    r"links=(\d+) controls=(\d+) ticks=(\d+) callbacks=(\d+) "
    r"pending=(\d+) relayouts=(\d+)(?: |$)",
    re.M,
)
DOCUMENT_MEMORY = re.compile(
    r"^document-memory .*? body-text=(\d+)$", re.M,
)
LAYOUT_RETAINED = re.compile(
    r"^layout-retained .*? commands=(\d+)/(\d+) .*? "
    r"links=(\d+)/(\d+) controls=(\d+)/(\d+)$", re.M,
)
LAYOUT_PRESENTATION = re.compile(
    r"^layout-presentation visually-blank=(yes|no)$", re.M,
)
READER_MODE = re.compile(
    r"^reader-mode kind=([^ ]+) high-confidence=(yes|no) "
    r"entries=(\d+) visited=(\d+) bounded=(yes|no)"
    r"(?: .*?available=(yes|no))?", re.M,
)
READER_RECOVERY = re.compile(
    r"^reader-recovery available=(yes|no) kind=([^ ]+) "
    r"high-confidence=(yes|no) entries=(\d+) visited=(\d+) "
    r"bounded=(yes|no) applied=(yes|no)$", re.M,
)
USABILITY_PROBE = re.compile(
    r'^usability-probe focus=(ready|missing) initial=(authored|pointer|none) '
    r'activation=(accepted|refused|not-run) action=([^ ]+) url="([^"]*)"'
    r'(?: handlers=(\d+)/(\d+) network=(\d+)/(\d+))?$',
    re.M,
)
JAVASCRIPT_ERROR = re.compile(r'^javascript-error=(.*)$', re.M)


def extract_scroll_status(log: Path) -> Optional[dict[str, int]]:
    try:
        match = SCROLL_STATUS.search(
            log.read_text(encoding="utf-8", errors="replace")
        )
    except OSError:
        return None
    if match is None:
        return None
    height = int(match.group(1))
    scroll_y = int(match.group(2))
    return {
        "height": height,
        "scrollY": scroll_y,
        "maximumScrollY": max(0, height - VIEWPORT_HEIGHT),
        "links": int(match.group(3)),
        "controls": int(match.group(4)),
        "ticks": int(match.group(5)),
        "callbacks": int(match.group(6)),
        "pendingTasks": int(match.group(7)),
        "relayouts": int(match.group(8)),
    }


def extract_scroll_y(log: Path) -> Optional[int]:
    status = extract_scroll_status(log)
    return status["scrollY"] if status is not None else None


def frame_pixel_signals(frame: Path) -> dict:
    result = {
        "dominantPixelShare": None,
        "foregroundPixelShare": None,
        "uniqueColors": None,
    }
    try:
        with Image.open(frame) as image:
            rgb = image.convert("RGB")
            pixel_count = rgb.width * rgb.height
            colors = rgb.getcolors(pixel_count)
    except (OSError, ValueError):
        return result
    if pixel_count == 0 or colors is None:
        return result
    dominant = max(count for count, _ in colors)
    share = dominant / pixel_count
    result.update({
        "dominantPixelShare": round(share, 6),
        "foregroundPixelShare": round(1.0 - share, 6),
        "uniqueColors": len(colors),
    })
    return result


def extract_usability_signals(log: Path, frame: Path) -> dict:
    try:
        text = log.read_text(encoding="utf-8", errors="replace")
    except OSError:
        text = ""
    status = SCROLL_STATUS.search(text)
    memory = DOCUMENT_MEMORY.search(text)
    layout = LAYOUT_RETAINED.search(text)
    presentation = LAYOUT_PRESENTATION.search(text)
    reader = READER_MODE.search(text)
    recovery = READER_RECOVERY.search(text)
    probe = USABILITY_PROBE.search(text)
    js_error = JAVASCRIPT_ERROR.search(text)
    links = int(status.group(3)) if status is not None else 0
    controls = int(status.group(4)) if status is not None else 0
    commands = int(layout.group(1)) if layout is not None else 0
    body_text = int(memory.group(1)) if memory is not None else 0
    pixels = frame_pixel_signals(frame)
    dominant = pixels["dominantPixelShare"]
    # Use the engine's authoritative settled-layout predicate. A normal blank
    # page can retain one full-viewport background command plus substantial
    # hidden body text; a command-count heuristic would mislabel that exact
    # hydration failure as useful. Old logs without this record retain a
    # conservative pixel fallback for audit compatibility.
    blank = ((presentation is not None and presentation.group(1) == "yes")
             or (presentation is None and dominant is not None
                 and dominant >= 0.995 and commands <= 1
                 and links == 0 and controls == 0))
    useful = (status is not None and not blank
              and (body_text >= 64 or links + controls != 0
                   or commands >= 2))
    error_text = js_error.group(1).strip() if js_error is not None else ""
    js_failed = bool(error_text and error_text not in ('""', '"" source=""'))
    return {
        "classification": ("missing-status" if status is None else
                           "blank" if blank else
                           "useful" if useful else "visual-shell"),
        "useful": useful,
        "blank": blank,
        "bodyTextBytes": body_text,
        "layoutCommands": commands,
        "layoutVisuallyBlank": (presentation.group(1) == "yes"
                                  if presentation is not None else None),
        "links": links,
        "controls": controls,
        "javascriptError": js_failed,
        "readerKind": reader.group(1) if reader is not None else None,
        "readerHighConfidence": (reader.group(2) == "yes"
                                  if reader is not None else None),
        "readerEntries": int(reader.group(3)) if reader is not None else None,
        "readerAvailable": (reader.group(6) == "yes"
                            if reader is not None and reader.group(6)
                            else None),
        "readerRecoveryAvailable": (
            recovery.group(1) == "yes" if recovery is not None else None),
        "readerRecoveryKind": (recovery.group(2)
                               if recovery is not None else None),
        "readerRecoveryHighConfidence": (
            recovery.group(3) == "yes" if recovery is not None else None),
        "readerRecoveryBoundedOut": (
            recovery.group(6) == "yes" if recovery is not None else None),
        "readerRecoveryApplied": (
            recovery.group(7) == "yes" if recovery is not None else None),
        "probeFocusReady": (probe.group(1) == "ready"
                            if probe is not None else None),
        "probeInitial": probe.group(2) if probe is not None else None,
        "probeActivationAttempted": (
            probe.group(3) != "not-run" if probe is not None else None),
        "probeActivationAccepted": (
            probe.group(3) == "accepted"
            if probe is not None and probe.group(3) != "not-run" else None),
        "probeAction": probe.group(4) if probe is not None else None,
        "probeSideEffectsStable": (
            int(probe.group(6)) == int(probe.group(7))
            and int(probe.group(8)) == int(probe.group(9))
            if probe is not None and probe.group(6) is not None else None),
        **pixels,
    }


def capture_scroll_metadata(position: str, requested, log: Path) -> dict:
    status = extract_scroll_status(log)
    result = {
        "requestedScrollY": requested,
        "actualScrollY": None,
        "maximumScrollY": None,
        "desiredScrollY": None,
        "delta": None,
        "valid": False,
    }
    if status is None:
        result["reason"] = "missing-status"
        return result
    maximum = status["maximumScrollY"]
    desired = 0
    if position == "middle":
        desired = maximum // 2
    elif position == "bottom":
        desired = maximum
    actual = status["scrollY"]
    delta = abs(actual - desired)
    result.update({
        "actualScrollY": actual,
        "maximumScrollY": maximum,
        "desiredScrollY": desired,
        "delta": delta,
        "valid": delta <= SCROLL_TOLERANCE,
    })
    if not result["valid"]:
        result["reason"] = "position-mismatch"
    return result


def base_command(lab: Path, url: str) -> list[str]:
    return [
        str(lab), "--url", url, "--fetch-scripts",
        "--ticks", "100", "--tick-ms", "33", "--limit-mb", "28",
        "--pace-real-time",
        "--script-timeout-ms", "10000", "--script-heap-mb", "8",
        "--script-total-mb", "24", "--script-file-kb", "512",
        "--script-count", "48", "--max-download-kb", "4096",
        "--content-blocker", "basic",
    ]


def run_capture(command: list[str], frame: Path, log: Path,
                timeout: int) -> int:
    command = [*command, "--output", str(frame)]
    # A failed refresh must never leave a prior run's frame eligible for the
    # compositor. The log is opened below with truncation as usual.
    frame.unlink(missing_ok=True)
    try:
        with log.open("wb") as stream:
            completed = subprocess.run(
                command, stdout=stream, stderr=subprocess.STDOUT,
                timeout=timeout, check=False,
            )
        return completed.returncode
    except subprocess.TimeoutExpired:
        with log.open("ab") as stream:
            stream.write(b"\ntilefinch audit capture timed out\n")
        return 124


def render_full_audit(lab: Path, metadata: dict, name: str, output: Path,
                      timeout: int) -> dict:
    result = {"name": name, "url": metadata["url"], "modes": {}}
    for mode in ("raw", "reader"):
        directory = output / mode
        directory.mkdir(parents=True, exist_ok=True)
        command = base_command(lab, metadata["url"])
        if mode == "raw":
            command.append("--hide-cookie-banners")
        else:
            # This is the actual product content-shape Reader transform, not
            # one of the older hostname-oriented --reader-profile sheets.
            command.append("--reader-mode")
        captures = {}

        def capture(position: str, scroll_options: list[str], requested) -> int:
            frame = directory / f"{name}-{position}.ppm"
            invalid_frame = directory / f"{name}-{position}-invalid.ppm"
            log = directory / f"{name}-{position}.log"
            invalid_frame.unlink(missing_ok=True)
            attempts = []
            current_options = scroll_options
            current_requested = requested
            maximum_attempts = (MIDDLE_CONVERGENCE_ATTEMPTS
                                if position == "middle" else 1)
            status = 124
            scroll = {}
            for attempt in range(maximum_attempts):
                capture_command = [*command, *current_options]
                if mode == "raw" and position == "top":
                    capture_command.append("--probe-usability")
                status = run_capture(
                    capture_command, frame, log, timeout,
                )
                scroll = capture_scroll_metadata(
                    position, current_requested, log,
                )
                attempts.append({"status": status, **scroll})
                if status == 0 and scroll["valid"]:
                    break
                desired = scroll.get("desiredScrollY")
                if (position != "middle" or status != 0
                        or desired is None
                        or attempt + 1 >= maximum_attempts):
                    break
                current_requested = desired
                current_options = ["--scroll-y", str(desired)]
            captures[position] = {
                "status": status,
                "frame": str(frame.relative_to(output)),
                "log": str(log.relative_to(output)),
                # Keep scrollY for readers of the first full-audit format.
                "scrollY": scroll["actualScrollY"],
                **scroll,
                "attemptCount": len(attempts),
                "attempts": attempts,
            }
            if status != 0:
                captures[position]["valid"] = False
                captures[position]["reason"] = "capture-failed"
            if frame.exists() and not captures[position]["valid"]:
                frame.replace(invalid_frame)
                captures[position]["diagnosticFrame"] = str(
                    invalid_frame.relative_to(output)
                )
            return status

        capture("top", [], 0)
        bottom_status = capture("bottom", ["--scroll-bottom"], "bottom")
        bottom_y = captures["bottom"]["actualScrollY"]
        if (bottom_status == 0 and captures["bottom"]["valid"]
                and bottom_y is not None):
            middle_y = max(0, bottom_y // 2)
            capture("middle", ["--scroll-y", str(middle_y)], middle_y)
        else:
            (directory / f"{name}-middle.ppm").unlink(missing_ok=True)
            (directory / f"{name}-middle-invalid.ppm").unlink(missing_ok=True)
            (directory / f"{name}-middle.log").unlink(missing_ok=True)
            captures["middle"] = {
                "status": "skipped-no-bottom",
                "frame": f"{mode}/{name}-middle.ppm",
                "log": f"{mode}/{name}-middle.log",
                "scrollY": None,
                "requestedScrollY": None,
                "actualScrollY": None,
                "maximumScrollY": None,
                "desiredScrollY": None,
                "delta": None,
                "valid": False,
                "reason": "invalid-bottom",
                "attemptCount": 0,
                "attempts": [],
                "usability": {},
            }
        for position in POSITIONS:
            capture_record = captures[position]
            capture_record["usability"] = {}
            if (capture_record.get("valid", False)
                    and capture_record.get("attemptCount", 0) != 0):
                capture_record["usability"] = extract_usability_signals(
                    directory / f"{name}-{position}.log",
                    directory / f"{name}-{position}.ppm",
                )
        result["modes"][mode] = {
            position: captures[position] for position in POSITIONS
        }
    result["usability"] = {
        mode: result["modes"][mode]["top"].get("usability", {})
        for mode in ("raw", "reader")
    }
    return result


def render(lab: Path, metadata_path: Path, output: Path, timeout: int,
           full_audit: bool = False):
    metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    name = metadata_path.stem
    if full_audit:
        return render_full_audit(lab, metadata, name, output, timeout)
    frame = output / f"{name}.ppm"
    log = output / f"{name}.log"
    status = run_capture(
        [*base_command(lab, metadata["url"]), "--hide-cookie-banners"],
        frame, log, timeout,
    )
    return f"{name} {'timeout' if status == 124 else f'exit={status}'}"


def summarize_usability(results: list[dict]) -> dict:
    summary = {
        "rawClassifications": {},
        "readerClassifications": {},
        "readerKinds": {},
        "readerAvailability": {"reported": 0, "available": 0},
        "rawProbe": {"reported": 0, "focusReady": 0,
                     "activationAttempted": 0,
                     "activationAccepted": 0},
        "rawReaderRecovery": {"reported": 0, "applied": 0},
    }
    for result in results:
        usability = result.get("usability", {})
        for mode, key in (("raw", "rawClassifications"),
                          ("reader", "readerClassifications")):
            signals = usability.get(mode, {})
            classification = signals.get("classification", "missing")
            values = summary[key]
            values[classification] = values.get(classification, 0) + 1
        raw = usability.get("raw", {})
        if raw.get("readerRecoveryApplied") is not None:
            summary["rawReaderRecovery"]["reported"] += 1
            if raw.get("readerRecoveryApplied"):
                summary["rawReaderRecovery"]["applied"] += 1
        if raw.get("probeFocusReady") is not None:
            summary["rawProbe"]["reported"] += 1
            if raw.get("probeFocusReady"):
                summary["rawProbe"]["focusReady"] += 1
            if raw.get("probeActivationAttempted"):
                summary["rawProbe"]["activationAttempted"] += 1
            if raw.get("probeActivationAccepted"):
                summary["rawProbe"]["activationAccepted"] += 1
        reader_kind = usability.get("reader", {}).get("readerKind")
        if reader_kind is not None:
            kinds = summary["readerKinds"]
            kinds[reader_kind] = kinds.get(reader_kind, 0) + 1
        reader_available = usability.get("reader", {}).get("readerAvailable")
        if reader_available is not None:
            summary["readerAvailability"]["reported"] += 1
            if reader_available:
                summary["readerAvailability"]["available"] += 1
    return summary


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--references", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--lab", type=Path,
        default=Path("build-preset-release/psp-browser-interactive-lab"),
    )
    parser.add_argument("--concurrency", type=int, default=2)
    parser.add_argument("--timeout-seconds", type=int, default=60)
    parser.add_argument(
        "--full-audit", action="store_true",
        help=("capture top/middle/bottom raw and product Reader frames "
              "under output/raw and output/reader"),
    )
    args = parser.parse_args()
    if args.concurrency < 1 or args.timeout_seconds < 1:
        parser.error("concurrency and timeout must be positive")
    args.output.mkdir(parents=True, exist_ok=True)
    metadata = sorted(args.references.glob("[0-9][0-9]-*.json"))
    # A browser-side DNS/TLS failure has no page oracle; omit it rather than
    # comparing Tilefinch to an error bitmap.
    eligible = []
    for item in metadata:
        record = json.loads(item.read_text(encoding="utf-8"))
        if record.get("status", 0) > 0 and record.get("devicePixelRatio") == 1:
            eligible.append(item)
    results = []
    with ThreadPoolExecutor(max_workers=args.concurrency) as executor:
        futures = {
            executor.submit(
                render, args.lab, item, args.output, args.timeout_seconds,
                args.full_audit,
            ): item
            for item in eligible
        }
        for future in as_completed(futures):
            result = future.result()
            results.append(result)
            if isinstance(result, str):
                print(result, flush=True)
            else:
                statuses = []
                for mode in ("raw", "reader"):
                    values = result["modes"][mode]
                    statuses.append(
                        f"{mode}=" + "/".join(
                            (str(values[position]["status"])
                             if values[position].get("valid") is True
                             else f"{values[position]['status']}:invalid")
                            for position in POSITIONS
                        )
                    )
                print(f"{result['name']} {' '.join(statuses)}", flush=True)
    if args.full_audit:
        ordered = sorted(results, key=lambda result: result["name"])
        usability = summarize_usability(ordered)
        (args.output / "audit-summary.json").write_text(
            json.dumps({"mode": "full", "usability": usability,
                        "results": ordered}, indent=2) + "\n",
            encoding="utf-8",
        )
        print(f"usability-summary {json.dumps(usability, sort_keys=True)}")


if __name__ == "__main__":
    main()
