#!/usr/bin/env python3
"""Host-only browsing gates and optional captured-page latency distributions.

No live requests, PSP builds, emulator launches, or audible media. Timings are
observations, not machine-independent wall-clock assertions. Raw captures and
logs belong in ignored/private output directories, never in a public journey log.
"""

import argparse
import json
import math
import os
from pathlib import Path
import re
import statistics
import subprocess

ROOT = Path(__file__).resolve().parents[1]
TIMINGS = {"elapsed-us", "max-pump-us", "max-gap-us", "acknowledgement-us",
           "drain-us", "total-response-us", "render-us", "request-to-pixels-us",
           "runtime-us", "max-runtime-us", "idle-us", "max-idle-us",
           "last-dynamic-completion-us", "preceding-idle-us",
           "font-us", "image-us"}


def distribution(values):
    ordered = sorted(values)
    return {"samples": len(ordered), "median_us": statistics.median(ordered),
            "p95_us": ordered[math.ceil(len(ordered) * .95) - 1],
            "maximum_us": ordered[-1]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=ROOT / "build-preset-release")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--wikipedia-home-trace", type=Path)
    parser.add_argument("--wikipedia-article-trace", type=Path)
    parser.add_argument("--search-trace", type=Path)
    parser.add_argument("--search-url", default="https://en.wikipedia.org/")
    parser.add_argument("--startup-trace", type=Path)
    parser.add_argument("--startup-url")
    parser.add_argument("--startup-ticks", type=int, default=1800,
                        help="logical 16 ms startup turns, including delayed timers")
    args = parser.parse_args()
    if not 1 <= args.runs <= 30:
        parser.error("--runs must be 1..30")
    if not 2 <= args.startup_ticks <= 4096:
        parser.error("--startup-ticks must be 2..4096")
    if bool(args.startup_trace) != bool(args.startup_url):
        parser.error("--startup-trace and --startup-url must be supplied together")
    build = args.build.resolve()
    output = (args.output or build / "host-browsing-responsiveness").resolve()
    output.mkdir(parents=True, exist_ok=True)

    def run(name, command, installed_settings=False):
        print(name, flush=True)
        environment = os.environ.copy()
        if installed_settings:
            environment.update(TILEFINCH_JS_BOOT_WINDOW_KB="4096",
                               TILEFINCH_JS_GC_GROWTH_PCT="150",
                               TILEFINCH_JS_ARRAY_CAP_KB="4096")
        with (output / (name + ".log")).open("w") as log:
            subprocess.run([str(part) for part in command], cwd=ROOT,
                           stdout=log, stderr=subprocess.STDOUT, check=True,
                           timeout=600, env=environment)
        return (output / (name + ".log")).read_text()

    # Build first: a green test from a stale executable is not evidence.
    run("build", ["cmake", "--build", build, "-j8"])
    engine = build / "tilefinch-browser-engine-tests"
    gates = []
    for mode in ("font-staging", "interaction-journey", "deferred-startup",
                 "provider-navigation", "pointer-search"):
        run(mode, [engine, "--" + mode + "-only"])
        gates.append(mode)
    for target in ("layout", "psp-ui", "psp-network-supervisor",
                   "psp-media-presentation", "psp-media-state"):
        run(target, [build / ("tilefinch-" + target + "-tests")])
        gates.append(target)

    samples = {}
    checksums = {}

    def collect(name, log):
        for line in log.splitlines():
            if not line.startswith(("navigation-responsiveness ", "font-publication ",
                                    "font-interruption ", "font-input-feedback ",
                                    "navigation-settle ", "navigation-startup ",
                                    "navigation-publication ")):
                continue
            prefix = line.split()[0]
            for key, value in re.findall(r"([\w-]+)=([\w-]+)", line):
                if key in TIMINGS:
                    samples.setdefault(name + "/" + prefix + "/" + key, []).append(int(value))
        raster = re.findall(r"font-raster view=(\d+) checksum=(\d+)", log)
        if raster:
            if name in checksums and checksums[name] != raster:
                raise RuntimeError("non-deterministic raster: " + name)
            checksums[name] = raster

    for name, trace, url in (
        ("home", args.wikipedia_home_trace, "https://en.wikipedia.org/wiki/Main_Page"),
        ("article", args.wikipedia_article_trace,
         "https://en.wikipedia.org/wiki/PlayStation_Portable"),
    ):
        if trace is None:
            continue
        trace = trace.resolve()
        for sample in range(args.runs + 1):
            log = run(f"{name}-navigation-{sample}",
                      [engine, "--navigation-staging-replay", trace, url])
            if sample:  # Exclude one warm-up.
                collect(name, log)
        for delay in ((500, 1000, 2000) if name == "home" else (500, 8000, 50000)):
            for sample in range(args.runs):
                log = run(f"{name}-feedback-{delay}-{sample}",
                          [engine, "--navigation-interrupt-replay", trace, url, delay])
                collect(name + f"/delay-{delay}", log)
    if args.search_trace is not None:
        run("captured-pointer-search", [engine, "--search-journey-replay",
            args.search_trace.resolve(), args.search_url], installed_settings=True)
        gates.append("captured-pointer-search")
    if args.startup_trace is not None:
        for sample in range(args.runs + 1):
            log = run(f"startup-settle-{sample}", [engine, "--navigation-settle-replay",
                      args.startup_trace.resolve(), args.startup_url,
                      args.startup_ticks], installed_settings=True)
            if sample:
                collect("startup", log)
        gates.append("captured-startup-settle")
    report = {"scope": "host; no physical input, scanout, firmware decoding, or network timing",
              "gates": gates, "timings": {key: distribution(value)
                  for key, value in samples.items()}, "raster_checksums": checksums}
    (output / "summary.json").write_text(json.dumps(report, indent=2) + "\n")
    print(output / "summary.json")


if __name__ == "__main__":
    main()
