#!/usr/bin/env python3
"""Run a pinned, static HTML/CSS WPT subset through the Tilefinch lab."""

from __future__ import annotations

import argparse
import csv
import json
import mimetypes
import re
import subprocess
import sys
import threading
import urllib.parse
from dataclasses import asdict, dataclass
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Optional


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MANIFEST = ROOT / "benchmarks" / "wpt" / "selected.tsv"
DEFAULT_REPORTER = (
    ROOT / "benchmarks" / "wpt" / "tilefinch-testharnessreport.js"
)
DEFAULT_REVISION = ROOT / "benchmarks" / "wpt" / "REVISION"
SUMMARY_RE = re.compile(r'^javascript summary="(.*)"$', re.MULTILINE)
WPT_RE = re.compile(
    r"^WPT\|complete\|h=(?P<h>-?\d+)"
    r"\|p=(?P<p>\d+)\|f=(?P<f>\d+)\|t=(?P<t>\d+)"
    r"\|n=(?P<n>\d+)\|s=(?P<s>\d+)\|d=(?P<d>.*)$"
)


@dataclass(frozen=True)
class Case:
    name: str
    suite: str
    panel: str
    kind: str
    test: str
    reference: Optional[str]
    viewport_css_width: Optional[int] = None
    viewport_css_height: Optional[int] = None


@dataclass
class Result:
    name: str
    suite: str
    panel: str
    kind: str
    status: str
    path: str
    reference: str = ""
    pass_count: int = 0
    fail_count: int = 0
    timeout_count: int = 0
    notrun_count: int = 0
    skip_count: int = 0
    harness_status: int = -1
    different_pixels: int = 0
    maximum_channel_delta: int = 0
    detail: str = ""


def read_manifest(path: Path) -> list[Case]:
    cases: list[Case] = []
    names: set[str] = set()
    with path.open(newline="", encoding="utf-8") as source:
        for line_number, row in enumerate(
            csv.reader(source, delimiter="\t"), start=1
        ):
            if not row or row[0].lstrip().startswith("#"):
                continue
            if len(row) not in {6, 8}:
                raise ValueError(
                    f"{path}:{line_number}: expected six or eight "
                    "tab-separated fields"
                )
            name, suite, panel, kind, test, reference = row[:6]
            viewport_width = viewport_height = None
            if len(row) == 8:
                viewport_fields = row[6:]
                if (viewport_fields[0] == "-") != (
                    viewport_fields[1] == "-"
                ):
                    raise ValueError(
                        f"{path}:{line_number}: viewport width and height "
                        "must both be set or both be '-'"
                    )
                if viewport_fields[0] != "-":
                    try:
                        viewport_width, viewport_height = (
                            int(value) for value in viewport_fields
                        )
                    except ValueError as error:
                        raise ValueError(
                            f"{path}:{line_number}: invalid viewport size"
                        ) from error
                    if viewport_width <= 0 or viewport_height <= 0:
                        raise ValueError(
                            f"{path}:{line_number}: viewport dimensions "
                            "must be positive"
                        )
            if name in names:
                raise ValueError(f"{path}:{line_number}: duplicate id {name}")
            if suite not in {"html", "css"}:
                raise ValueError(
                    f"{path}:{line_number}: unsupported suite {suite}"
                )
            if kind not in {"testharness", "reftest"}:
                raise ValueError(
                    f"{path}:{line_number}: unsupported kind {kind}"
                )
            if not re.fullmatch(r"[a-z][a-z0-9-]*", panel):
                raise ValueError(
                    f"{path}:{line_number}: invalid panel {panel}"
                )
            if kind == "reftest" and reference == "-":
                raise ValueError(
                    f"{path}:{line_number}: reftest requires a reference"
                )
            names.add(name)
            cases.append(
                Case(
                    name=name,
                    suite=suite,
                    panel=panel,
                    kind=kind,
                    test=test,
                    reference=None if reference == "-" else reference,
                    viewport_css_width=viewport_width,
                    viewport_css_height=viewport_height,
                )
            )
    if not cases:
        raise ValueError(f"{path}: no cases")
    return cases


def safe_wpt_path(root: Path, relative: str) -> Path:
    candidate = (root / relative.lstrip("/")).resolve()
    try:
        candidate.relative_to(root.resolve())
    except ValueError as error:
        raise ValueError(f"path escapes WPT root: {relative}") from error
    return candidate


class QuietStaticHandler(SimpleHTTPRequestHandler):
    server_version = "TilefinchWPT/1"

    def __init__(self, *args, directory: str, reporter: bytes, **kwargs):
        self._reporter = reporter
        super().__init__(*args, directory=directory, **kwargs)

    def log_message(self, _format: str, *_args: object) -> None:
        return

    def end_headers(self) -> None:
        self.send_header("Cache-Control", "no-store")
        self.send_header("Access-Control-Allow-Origin", "*")
        super().end_headers()

    def copyfile(self, source, outputfile) -> None:
        # The bounded lab intentionally closes a response as soon as it has
        # consumed enough bytes.  That is a successful cancellation, not a
        # server failure worth dumping a thread traceback for every WPT.
        try:
            super().copyfile(source, outputfile)
        except (BrokenPipeError, ConnectionResetError):
            return

    def do_GET(self) -> None:
        path = urllib.parse.urlsplit(self.path).path
        if path == "/resources/testharnessreport.js":
            self.send_response(200)
            self.send_header("Content-Type", "text/javascript; charset=utf-8")
            self.send_header("Content-Length", str(len(self._reporter)))
            self.end_headers()
            try:
                self.wfile.write(self._reporter)
            except (BrokenPipeError, ConnectionResetError):
                pass
            return
        super().do_GET()

    def guess_type(self, path: str) -> str:
        guessed, _ = mimetypes.guess_type(path)
        if path.endswith(".xht") or path.endswith(".xhtml"):
            return "application/xhtml+xml"
        return guessed or "application/octet-stream"


class StaticWptServer:
    def __init__(self, root: Path, reporter: Path):
        reporter_bytes = reporter.read_bytes()

        def handler(*args, **kwargs):
            return QuietStaticHandler(
                *args,
                directory=str(root),
                reporter=reporter_bytes,
                **kwargs,
            )

        self._server = ThreadingHTTPServer(("127.0.0.1", 0), handler)
        self._thread = threading.Thread(
            target=self._server.serve_forever, daemon=True
        )

    @property
    def origin(self) -> str:
        return f"http://127.0.0.1:{self._server.server_port}"

    def __enter__(self) -> "StaticWptServer":
        self._thread.start()
        return self

    def __exit__(self, *_exc: object) -> None:
        self._server.shutdown()
        self._server.server_close()
        self._thread.join(timeout=2)


def run_lab(
    lab: Path,
    url: str,
    output: Path,
    log: Path,
    ticks: int,
    tick_ms: int,
    limit_mb: int,
    script_heap_mb: int,
    viewport_css_width: int,
    viewport_css_height: int,
    timeout: float,
) -> tuple[int, str]:
    command = [
        str(lab),
        "--url",
        url,
        "--fetch-scripts",
        "--ticks",
        str(ticks),
        "--tick-ms",
        str(tick_ms),
        "--limit-mb",
        str(limit_mb),
        "--script-heap-mb",
        str(script_heap_mb),
        "--viewport-css-width",
        str(viewport_css_width),
        "--viewport-css-height",
        str(viewport_css_height),
        "--no-loop-capture",
        "--wpt-test-rendered",
        "--output",
        str(output),
    ]
    try:
        completed = subprocess.run(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=timeout,
            check=False,
        )
        text = completed.stdout
        return_code = completed.returncode
    except subprocess.TimeoutExpired as error:
        partial = error.stdout or ""
        if isinstance(partial, bytes):
            partial = partial.decode("utf-8", errors="replace")
        text = partial + "\nRUNNER TIMEOUT\n"
        return_code = 124
    log.write_text(text, encoding="utf-8")
    return return_code, text


def parse_ppm(path: Path) -> tuple[int, int, int, bytes]:
    data = path.read_bytes()
    if not data.startswith(b"P6"):
        raise ValueError(f"{path}: expected binary P6 PPM")
    offset = 2
    tokens: list[bytes] = []
    while len(tokens) < 3:
        while offset < len(data) and data[offset] in b" \t\r\n":
            offset += 1
        if offset < len(data) and data[offset] == ord("#"):
            while offset < len(data) and data[offset] != ord("\n"):
                offset += 1
            continue
        start = offset
        while offset < len(data) and data[offset] not in b" \t\r\n":
            offset += 1
        if start == offset:
            raise ValueError(f"{path}: truncated PPM header")
        tokens.append(data[start:offset])
    if offset >= len(data) or data[offset] not in b" \t\r\n":
        raise ValueError(f"{path}: missing PPM header separator")
    if data[offset : offset + 2] == b"\r\n":
        offset += 2
    else:
        offset += 1
    width, height, maximum = (int(token) for token in tokens)
    pixels = data[offset:]
    expected = width * height * 3
    if maximum != 255 or len(pixels) != expected:
        raise ValueError(
            f"{path}: unsupported PPM maximum/size {maximum}/{len(pixels)}"
        )
    return width, height, maximum, pixels


def compare_ppm(left: Path, right: Path) -> tuple[int, int]:
    left_width, left_height, _, left_pixels = parse_ppm(left)
    right_width, right_height, _, right_pixels = parse_ppm(right)
    if (left_width, left_height) != (right_width, right_height):
        return max(left_width * left_height, right_width * right_height), 255
    different_pixels = 0
    maximum_delta = 0
    for offset in range(0, len(left_pixels), 3):
        left_pixel = left_pixels[offset : offset + 3]
        right_pixel = right_pixels[offset : offset + 3]
        if left_pixel != right_pixel:
            different_pixels += 1
            maximum_delta = max(
                maximum_delta,
                *(abs(a - b) for a, b in zip(left_pixel, right_pixel)),
            )
    return different_pixels, maximum_delta


def successful_navigation(return_code: int, log: str, output: Path) -> bool:
    return (
        return_code == 0
        and output.is_file()
        and re.search(
            r"^interactive teardown=0 active=0 .* status=PASS$",
            log,
            re.MULTILINE,
        )
        is not None
    )


def run_testharness_case(
    case: Case,
    lab: Path,
    origin: str,
    output_dir: Path,
    args: argparse.Namespace,
) -> Result:
    ppm = output_dir / f"{case.name}.ppm"
    log_path = output_dir / f"{case.name}.log"
    url = origin + "/" + urllib.parse.quote(case.test, safe="/")
    return_code, log = run_lab(
        lab,
        url,
        ppm,
        log_path,
        args.ticks,
        args.tick_ms,
        args.limit_mb,
        args.script_heap_mb,
        case.viewport_css_width or args.viewport_css_width,
        case.viewport_css_height or args.viewport_css_height,
        args.timeout,
    )
    result = Result(
        case.name,
        case.suite,
        case.panel,
        case.kind,
        "HARNESS-ERROR",
        case.test,
    )
    if not successful_navigation(return_code, log, ppm):
        result.detail = f"lab exit={return_code}; see {log_path.name}"
        return result
    matches = SUMMARY_RE.findall(log)
    summary = matches[-1] if matches else ""
    parsed = WPT_RE.match(summary)
    if parsed is None:
        result.detail = (
            f"no completed WPT summary ({summary[:180] or 'empty'}); "
            f"see {log_path.name}"
        )
        return result
    values = parsed.groupdict()
    result.harness_status = int(values["h"])
    result.pass_count = int(values["p"])
    result.fail_count = int(values["f"])
    result.timeout_count = int(values["t"])
    result.notrun_count = int(values["n"])
    result.skip_count = int(values["s"])
    result.detail = values["d"]
    if result.harness_status != 0:
        result.status = "HARNESS-ERROR"
    elif result.fail_count or result.timeout_count or result.notrun_count:
        result.status = "FAIL"
    elif result.pass_count:
        result.status = "PASS"
    elif result.skip_count:
        result.status = "SKIP"
    else:
        result.status = "HARNESS-ERROR"
        result.detail = "completed without subtests"
    return result


def run_reftest_case(
    case: Case,
    lab: Path,
    origin: str,
    output_dir: Path,
    args: argparse.Namespace,
) -> Result:
    assert case.reference is not None
    test_ppm = output_dir / f"{case.name}.ppm"
    reference_ppm = output_dir / f"{case.name}-ref.ppm"
    test_log = output_dir / f"{case.name}.log"
    reference_log = output_dir / f"{case.name}-ref.log"
    test_url = origin + "/" + urllib.parse.quote(case.test, safe="/")
    reference_url = origin + "/" + urllib.parse.quote(case.reference, safe="/")
    test_code, test_text = run_lab(
        lab,
        test_url,
        test_ppm,
        test_log,
        args.ticks,
        args.tick_ms,
        args.limit_mb,
        args.script_heap_mb,
        case.viewport_css_width or args.viewport_css_width,
        case.viewport_css_height or args.viewport_css_height,
        args.timeout,
    )
    reference_code, reference_text = run_lab(
        lab,
        reference_url,
        reference_ppm,
        reference_log,
        args.ticks,
        args.tick_ms,
        args.limit_mb,
        args.script_heap_mb,
        case.viewport_css_width or args.viewport_css_width,
        case.viewport_css_height or args.viewport_css_height,
        args.timeout,
    )
    result = Result(
        case.name,
        case.suite,
        case.panel,
        case.kind,
        "HARNESS-ERROR",
        case.test,
        reference=case.reference,
    )
    if not successful_navigation(test_code, test_text, test_ppm):
        result.detail = f"test navigation failed; see {test_log.name}"
        return result
    if not successful_navigation(reference_code, reference_text, reference_ppm):
        result.detail = (
            f"reference navigation failed; see {reference_log.name}"
        )
        return result
    try:
        different, maximum_delta = compare_ppm(test_ppm, reference_ppm)
    except ValueError as error:
        result.detail = str(error)
        return result
    result.different_pixels = different
    result.maximum_channel_delta = maximum_delta
    result.status = "PASS" if different == 0 else "FAIL"
    if different:
        result.detail = (
            f"{different} pixels differ; maximum channel delta {maximum_delta}"
        )
    return result


def check_revision(wpt_root: Path, revision_file: Path, allow_drift: bool) -> str:
    expected = revision_file.read_text(encoding="utf-8").strip()
    completed = subprocess.run(
        ["git", "-C", str(wpt_root), "rev-parse", "HEAD"],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
        check=False,
    )
    actual = completed.stdout.strip()
    if completed.returncode != 0:
        if allow_drift:
            return "unversioned"
        raise RuntimeError(f"{wpt_root}: not a Git WPT checkout")
    if actual != expected and not allow_drift:
        raise RuntimeError(
            f"WPT revision mismatch: expected {expected}, found {actual}; "
            "use --allow-revision-drift only for exploratory runs"
        )
    return actual


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lab", type=Path, required=True)
    parser.add_argument("--wpt-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--reporter", type=Path, default=DEFAULT_REPORTER)
    parser.add_argument("--revision-file", type=Path, default=DEFAULT_REVISION)
    parser.add_argument("--suite", choices=("all", "html", "css"), default="all")
    parser.add_argument(
        "--panel",
        action="append",
        default=[],
        help="run only the named manifest panel; may be repeated",
    )
    parser.add_argument(
        "--case",
        action="append",
        default=[],
        help="run only the named manifest case; may be repeated",
    )
    # One selected reftest deliberately advances an incremental scroll once
    # per animation frame. Keep enough bounded turns for reftest-wait to
    # settle without introducing a wall-clock-driven wait.
    parser.add_argument("--ticks", type=int, default=96)
    parser.add_argument("--tick-ms", type=int, default=10)
    parser.add_argument("--limit-mb", type=int, default=24)
    parser.add_argument(
        "--viewport-css-width",
        type=int,
        default=480,
        help="fixed WPT layout viewport width; avoids mobile-meta asymmetry",
    )
    parser.add_argument(
        "--viewport-css-height",
        type=int,
        default=272,
        help="fixed WPT layout viewport height",
    )
    parser.add_argument(
        "--script-heap-mb",
        type=int,
        default=12,
        help=(
            "per-document JS heap cap; the WPT harness's largest generated "
            "matrices need bounded headroom beyond the browser's 4 MiB default"
        ),
    )
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--strict", action="store_true")
    parser.add_argument("--allow-revision-drift", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.script_heap_mb <= 0:
        print("error: --script-heap-mb must be positive", file=sys.stderr)
        return 2
    if args.viewport_css_width <= 0 or args.viewport_css_height <= 0:
        print("error: WPT viewport dimensions must be positive", file=sys.stderr)
        return 2
    if not args.lab.is_file():
        print(f"error: lab executable not found: {args.lab}", file=sys.stderr)
        return 2
    if not args.wpt_root.is_dir():
        print(f"error: WPT root not found: {args.wpt_root}", file=sys.stderr)
        return 2
    try:
        revision = check_revision(
            args.wpt_root, args.revision_file, args.allow_revision_drift
        )
        cases = read_manifest(args.manifest)
    except (OSError, RuntimeError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    if args.suite != "all":
        cases = [case for case in cases if case.suite == args.suite]
    if args.panel:
        requested_panels = set(args.panel)
        known_panels = {case.panel for case in cases}
        missing_panels = sorted(requested_panels - known_panels)
        if missing_panels:
            print(
                "error: unknown or suite-filtered WPT panel(s): "
                + ", ".join(missing_panels),
                file=sys.stderr,
            )
            return 2
        cases = [case for case in cases if case.panel in requested_panels]
    if args.case:
        requested = set(args.case)
        known = {case.name for case in cases}
        missing_cases = sorted(requested - known)
        if missing_cases:
            print(
                "error: unknown or suite-filtered WPT case(s): "
                + ", ".join(missing_cases),
                file=sys.stderr,
            )
            return 2
        cases = [case for case in cases if case.name in requested]
    missing = []
    for case in cases:
        for relative in (case.test, case.reference):
            if relative and not safe_wpt_path(args.wpt_root, relative).is_file():
                missing.append(relative)
    if not safe_wpt_path(args.wpt_root, "resources/testharness.js").is_file():
        missing.append("resources/testharness.js")
    if missing:
        print(
            "error: sparse WPT checkout is missing:\n  "
            + "\n  ".join(sorted(set(missing))),
            file=sys.stderr,
        )
        print(
            "run benchmarks/prepare-upstream-wpt.sh first", file=sys.stderr
        )
        return 2

    args.output.mkdir(parents=True, exist_ok=True)
    results: list[Result] = []
    with StaticWptServer(args.wpt_root, args.reporter) as server:
        for case in cases:
            if case.kind == "testharness":
                result = run_testharness_case(
                    case, args.lab, server.origin, args.output, args
                )
            else:
                result = run_reftest_case(
                    case, args.lab, server.origin, args.output, args
                )
            results.append(result)
            counts = ""
            if case.kind == "testharness":
                counts = (
                    f" subtests={result.pass_count}/{result.fail_count}/"
                    f"{result.timeout_count}/{result.notrun_count}/"
                    f"{result.skip_count}"
                )
            elif result.status != "HARNESS-ERROR":
                counts = (
                    f" differing-pixels={result.different_pixels}"
                    f" max-delta={result.maximum_channel_delta}"
                )
            detail = f" detail={result.detail}" if result.detail else ""
            print(
                f"wpt case={case.name} suite={case.suite}"
                f" panel={case.panel} kind={case.kind}"
                f" status={result.status}{counts}{detail}"
            )

    totals = {
        status: sum(result.status == status for result in results)
        for status in ("PASS", "FAIL", "SKIP", "HARNESS-ERROR")
    }
    panels = {}
    for panel in sorted({result.panel for result in results}):
        panel_results = [result for result in results if result.panel == panel]
        panels[panel] = {
            "files": len(panel_results),
            "pass": sum(result.status == "PASS" for result in panel_results),
            "fail": sum(result.status == "FAIL" for result in panel_results),
            "skip": sum(result.status == "SKIP" for result in panel_results),
            "harness_error": sum(
                result.status == "HARNESS-ERROR" for result in panel_results
            ),
            "subtests_pass": sum(
                result.pass_count for result in panel_results
            ),
            "subtests_fail": sum(
                result.fail_count
                + result.timeout_count
                + result.notrun_count
                for result in panel_results
            ),
        }
        panel_counts = panels[panel]
        print(
            f"wpt-panel name={panel} files={panel_counts['files']}"
            f" pass={panel_counts['pass']} fail={panel_counts['fail']}"
            f" skip={panel_counts['skip']}"
            f" harness-error={panel_counts['harness_error']}"
            f" subtests={panel_counts['subtests_pass']}/"
            f"{panel_counts['subtests_fail']}"
        )
    payload = {
        "schema": 2,
        "wpt_revision": revision,
        "manifest": str(args.manifest),
        "lab": str(args.lab),
        "limit_mb": args.limit_mb,
        "script_heap_mb": args.script_heap_mb,
        "ticks": args.ticks,
        "tick_ms": args.tick_ms,
        "totals": totals,
        "panels": panels,
        "results": [asdict(result) for result in results],
    }
    (args.output / "results.json").write_text(
        json.dumps(payload, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    with (args.output / "results.tsv").open(
        "w", newline="", encoding="utf-8"
    ) as output:
        writer = csv.writer(output, delimiter="\t")
        writer.writerow(
            [
                "id",
                "suite",
                "panel",
                "kind",
                "status",
                "pass",
                "fail",
                "timeout",
                "notrun",
                "skip",
                "different_pixels",
                "detail",
            ]
        )
        for result in results:
            writer.writerow(
                [
                    result.name,
                    result.suite,
                    result.panel,
                    result.kind,
                    result.status,
                    result.pass_count,
                    result.fail_count,
                    result.timeout_count,
                    result.notrun_count,
                    result.skip_count,
                    result.different_pixels,
                    result.detail,
                ]
            )
    print(
        "upstream-wpt"
        f" revision={revision[:12]} tests={len(results)}"
        f" pass={totals['PASS']} fail={totals['FAIL']}"
        f" skip={totals['SKIP']} harness-error={totals['HARNESS-ERROR']}"
        f" results={args.output / 'results.json'}"
    )
    if totals["HARNESS-ERROR"]:
        return 2
    if args.strict and totals["FAIL"]:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
