#!/usr/bin/env python3
"""Measure a disposable build tree. Never point this at a concurrently used tree.

Touches only the selected source's mtime to force an incremental rebuild, then
restores that mtime. Contents are unchanged. Keeps complete build logs.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess
import time


def compiled_quickjs_directory(build):
    cache = dict(line.split("=", 1) for line in (build / "CMakeCache.txt").read_text().splitlines()
                 if "=" in line and not line.startswith(("#", "//")))
    configured = cache.get("TILEFINCH_QUICKJS_COMPILE_SOURCE_DIR:INTERNAL", "")
    if not configured:
        raise SystemExit("Reconfigure this Bellard build before measuring its selected QuickJS sources")
    return Path(configured)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--samples", type=int, default=3)
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--reconfigure", action="store_true",
                        help="also measure no-change configure/build and assert VM mtimes stay stable")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    build = args.build.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    command = ["cmake", "--build", str(build), "--target",
               "tilefinch-browser-engine-tests", f"-j{args.jobs}"]
    results = {}
    if args.reconfigure:
        qjs = compiled_quickjs_directory(build)
        files = (qjs / "quickjs.c", qjs / "quickjs.h")
        original = [(hashlib.sha256(p.read_bytes()).hexdigest(), p.stat().st_mtime_ns)
                    for p in files]
        for name, arguments in (("reconfigure", ["cmake", "-S", str(root), "-B", str(build)]),
                                ("post-configure-build", command)):
            start = time.perf_counter()
            with (args.output / f"{name}.log").open("wb") as log:
                result = subprocess.run(arguments, stdout=log, stderr=subprocess.STDOUT)
            elapsed = time.perf_counter() - start
            print(f"{name} seconds={elapsed:.4f} status={result.returncode}", flush=True)
            if result.returncode:
                raise SystemExit(f"{name} failed")
            results[name] = {"seconds": elapsed}
        after = [(hashlib.sha256(p.read_bytes()).hexdigest(), p.stat().st_mtime_ns)
                 for p in files]
        if original != after:
            raise SystemExit("No-change configuration rewrote the compiled QuickJS sources")
    for name, relative in (("noop", None),
                           ("journey-edit", "tests/suites/browser_engine_journeys.inc"),
                           ("engine-edit", "src/browser_engine.c")):
        source = root / relative if relative else None
        original = source.stat() if source else None
        times = []
        try:
            for sample in range(args.samples):
                if source:
                    # Unix Make can compare mtimes at whole-second precision.
                    # Keep this wait outside the timed build, and prove below
                    # that a compile really ran rather than timing a no-op.
                    time.sleep(1.05)
                    os.utime(source, None)
                log = args.output / f"{name}-{sample + 1}.log"
                start = time.perf_counter()
                with log.open("wb") as stream:
                    result = subprocess.run(command, stdout=stream,
                                            stderr=subprocess.STDOUT)
                elapsed = time.perf_counter() - start
                print(f"{name} sample={sample + 1} seconds={elapsed:.4f} "
                      f"status={result.returncode}", flush=True)
                if result.returncode:
                    raise SystemExit(f"Build failed; see {log}")
                if source:
                    expected = ("test_browser_engine" if name == "journey-edit"
                                else "src/browser_engine.c")
                    if not any("Building C object" in line and expected in line
                               for line in log.read_text().splitlines()):
                        raise SystemExit(f"Invalid sample: no expected compile in {log}")
                times.append(elapsed)
        finally:
            if source:
                os.utime(source, ns=(original.st_atime_ns, original.st_mtime_ns))
        results[name] = {"samples_seconds": times,
                         "median_seconds": statistics.median(times)}
    (args.output / "timings.json").write_text(json.dumps(results, indent=2) + "\n")


if __name__ == "__main__":
    main()
