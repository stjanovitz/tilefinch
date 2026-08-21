#!/usr/bin/env python3
"""Render local-only Tilefinch peers for mobile browser references."""

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import json
from pathlib import Path
import subprocess


def render(lab: Path, metadata_path: Path, output: Path, timeout: int) -> str:
    metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    name = metadata_path.stem
    frame = output / f"{name}.ppm"
    log = output / f"{name}.log"
    command = [
        str(lab), "--url", metadata["url"], "--fetch-scripts",
        "--ticks", "100", "--tick-ms", "33", "--limit-mb", "28",
        "--script-timeout-ms", "10000", "--script-heap-mb", "8",
        "--script-total-mb", "24", "--script-file-kb", "512",
        "--script-count", "48", "--max-download-kb", "4096",
        "--content-blocker", "basic", "--hide-cookie-banners",
        "--output", str(frame),
    ]
    try:
        with log.open("wb") as stream:
            completed = subprocess.run(
                command, stdout=stream, stderr=subprocess.STDOUT,
                timeout=timeout, check=False,
            )
        return f"{name} exit={completed.returncode}"
    except subprocess.TimeoutExpired:
        return f"{name} timeout"


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
    with ThreadPoolExecutor(max_workers=args.concurrency) as executor:
        futures = {
            executor.submit(
                render, args.lab, item, args.output, args.timeout_seconds,
            ): item
            for item in eligible
        }
        for future in as_completed(futures):
            print(future.result(), flush=True)


if __name__ == "__main__":
    main()
