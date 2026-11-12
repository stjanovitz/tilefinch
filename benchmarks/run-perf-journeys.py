#!/usr/bin/env python3
"""Run the PSP performance journeys under PPSSPP and compare them with the
committed baselines.

A journey is one scripted-input scenario (tests/input-scripts) run by
scripts/run-ppsspp-input-script.sh at a fixed emulated clock, with every
page served from a recorded HTTP trace (response-keyed replay) instead of
the network. Load, scroll and layout timings are then a function of the
emulated CPU, not of host network timing, so a change in them is a change
in the build. Journeys are listed in benchmarks/perf-journeys.tsv; their
baselines live in tests/perf-baselines.tsv (journey, metric, value,
tolerance). Every metric is lower-is-better.

Recorded traces are not committed (like the fidelity corpus): each journey
names a directory under --corpus (default perf/traces) and pins its
SHA-256, and a journey whose trace is absent is skipped, not failed.
An optional `boot` column lists extra validation boot.cfg keys, separated
by ';' (for example `trace_ignore_request_body=1;validation_js_profile=0`).
An optional `cpu_mhz` column overrides the emulated clock for one journey:
a page whose own wall-clock limits (the parser-stage script budget) trip at
111 MHz, such as chatgpt.com, runs at the PSP's 333 MHz instead.
An optional `clock` column is the PSP's local time of day when the run
starts (HH:MM, default 12:00; `host` keeps the host's own). Night mode's
Auto setting follows the local clock (dark from 19:00 to 07:00), so a
journey is day or night whenever it runs; 22:00 gives a night run.
An optional `sheet_marks` column names the input-script marks (separated
by ',') whose tilefinch-input-stylesheets counters become sheets.MARK.*
metrics; '-' keeps none.
An optional `frames` column compares this run's frame marks with another
journey's: `MARK=JOURNEY:MARK` (several separated by ','). The metric
frame.MARK.differs is 1 when the two captures differ, so its baseline is 0
with no tolerance. The other journey must run first in the same pass (it
comes earlier in the manifest) or have kept artifacts.

Usage:
    run-perf-journeys.py [--journey NAME ...] [--runs N]
        [--record | --ratchet] [--record-missing] [--from-artifacts]
        [--corpus DIR]
        [--build-dir DIR]
    run-perf-journeys.py --prepare-trace CAPTURE_DIR NAME [--corpus DIR]

--record    writes this run's values as the baselines of the journeys run
            (a new journey, or a deliberate reset recorded in
            .private-investigations/PERFORMANCE_LEDGER.md).
--ratchet   lowers baselines that this run beat by more than their
            tolerance; it never raises one. A regression is fixed, or the
            baseline is raised by hand with the reason in the ledger.
--record-missing PREFIX adds a baseline for each metric starting with
            PREFIX that has none (a new metric for an existing journey) and
            only compares the rest; combine it with --ratchet to lower
            them too.
--prepare-trace copies a lab capture (psp-browser-interactive-lab
            --capture-http DIR) into the corpus and adds a record under the
            final URL of every redirected document, so a reload of the page
            finds its response; warns about records that were still in
            flight when the capture ended; prints the SHA-256 to put in the
            manifest.
"""
import argparse
import hashlib
import os
import re
import shutil
import statistics
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MANIFEST = os.path.join(ROOT, "benchmarks", "perf-journeys.tsv")
BASELINES = os.path.join(ROOT, "tests", "perf-baselines.tsv")
RUNNER = os.path.join(ROOT, "scripts", "run-ppsspp-input-script.sh")
CLOCK_MHZ = "111"

# Relative tolerance and minimum absolute slack a newly recorded baseline
# gets, by metric name prefix (first match wins). Load and layout times
# repeat exactly under trace replay; frame composition and latency tails
# move by a few percent with emulated-thread interleaving.
TOLERANCES = [
    ("load.", 0.02, 20.0),
    ("layout.", 0.05, 20.0),
    ("relayout.", 0.05, 20.0),
    ("scroll.", 0.10, 5.0),
    ("control.", 0.10, 5.0),
    ("ui.compose_avg", 0.10, 200.0),
    ("ui.", 0.15, 2.0),
    ("memory.", 0.02, 64.0),
]


# Counts (layout steps, presses the page ignored, slow frames) are exact
# under replay; they get a small absolute allowance instead.
COUNT_TOLERANCES = [
    ("_steps", 1.0),
    ("_presses", 2.0),
    ("ui.over_33ms", 1.0),
    # Stylesheet-cache and rebuild counters, and frame comparisons, are
    # exact under replay: any rise fails.
    (".differs", 0.0),
    ("_compiles", 0.0),
    ("_refusals", 0.0),
    ("_takes", 0.0),
    (".unrestored", 0.0),
    ("_relayouts", 0.0),
]
DEFAULT_CLOCK = "12:00"


def tolerance_for(metric, value):
    for suffix, absolute in COUNT_TOLERANCES:
        if metric.endswith(suffix):
            return absolute
    for prefix, relative, absolute in TOLERANCES:
        if metric.startswith(prefix):
            return max(relative * value, absolute)
    return max(0.10 * value, 1.0)


def read_tsv(path):
    with open(path, encoding="utf-8") as handle:
        lines = [line.rstrip("\n") for line in handle
                 if line.strip() and not line.startswith("#")]
    header = lines[0].split("\t")
    return [dict(zip(header, line.split("\t"))) for line in lines[1:]]


def trace_sha256(directory):
    digest = hashlib.sha256()
    for name in sorted(os.listdir(directory)):
        path = os.path.join(directory, name)
        if not os.path.isfile(path):
            continue
        digest.update(name.encode() + b"\0")
        with open(path, "rb") as handle:
            digest.update(handle.read())
        digest.update(b"\0")
    return digest.hexdigest()


def prepare_trace(source, name, corpus):
    target = os.path.join(corpus, name)
    if os.path.exists(target):
        sys.exit("refusing to overwrite %s" % target)
    shutil.copytree(source, target)
    meta_path = os.path.join(target, "trace.meta")
    with open(meta_path, encoding="utf-8") as handle:
        trace_meta = handle.read()
    count = int(re.search(r"^record-count=(\d+)$", trace_meta, re.M).group(1))
    urls = set()
    records = []
    for index in range(count):
        with open(os.path.join(target, "%04d.meta" % index),
                  encoding="utf-8") as handle:
            meta = handle.read()
        records.append(meta)
        urls.add(re.search(r"^url=(.*)$", meta, re.M).group(1))
    added = 0
    for index, meta in enumerate(records):
        final = re.search(r"^effective-url=(.*)$", meta, re.M)
        status = re.search(r"^status=(\d+)$", meta, re.M)
        if (final is None or status is None or status.group(1) != "200"
                or final.group(1) in urls or not final.group(1)):
            continue
        alias = count + added
        original = re.search(r"^url=(.*)$", meta, re.M).group(1)
        # The request-side fields name the URL the request was made for;
        # replay checks that the logical URL keys to the record's url.
        for key in ("url", "logical-request-url",
                    "request-credential-origin"):
            meta = re.sub(r"^%s=%s$" % (re.escape(key), re.escape(original)),
                          lambda _m, key=key: "%s=%s" % (key, final.group(1)),
                          meta, flags=re.M)
        with open(os.path.join(target, "%04d.meta" % alias), "w",
                  encoding="utf-8") as handle:
            handle.write(meta)
        shutil.copyfile(os.path.join(target, "%04d.body" % index),
                        os.path.join(target, "%04d.body" % alias))
        urls.add(final.group(1))
        added += 1
        print("alias %04d -> %s" % (alias, final.group(1)))
    # A request in flight at capture teardown is retained as a cancellation
    # (usually status 0, no body) and replays as one: a send whose answer
    # stream was cut off replays as "No internet", not as the answer.
    for index, meta in enumerate(records):
        error = re.search(r"^error=(.*)$", meta, re.M)
        if error is not None and "during scheduler teardown" in error.group(1):
            method = re.search(r"^method=(.*)$", meta, re.M).group(1)
            url = re.search(r"^url=(.*)$", meta, re.M).group(1)
            status = re.search(r"^status=(\d+)$", meta, re.M)
            print("warning: %04d %s %s was in flight at capture teardown "
                  "(status=%s); replay serves the cancellation"
                  % (index, method, url[:160],
                     status.group(1) if status else "?"))
    with open(meta_path, "w", encoding="utf-8") as handle:
        handle.write(re.sub(r"^record-count=\d+$",
                            "record-count=%d" % (count + added),
                            trace_meta, flags=re.M))
    print("%s sha256=%s" % (target, trace_sha256(target)))


def percentile(values, fraction):
    ordered = sorted(values)
    rank = max(0, min(len(ordered) - 1,
                      int(round(fraction * (len(ordered) - 1)))))
    return ordered[rank]


def field(line, name):
    match = re.search(r"\b" + re.escape(name) + r"=(-?\d+)", line)
    return int(match.group(1)) if match else None


class MetricShapeError(ValueError):
    """A validation-log line this reader cannot turn into a metric."""


def extract_metrics(log):
    """Lower-is-better numbers from one run's tilefinch-validation.txt."""
    metrics = {}
    scope_counts = {}
    for line in log.splitlines():
        if line.startswith("tilefinch-load-experience:"):
            scope = re.search(r"scope=(\w+)", line).group(1)
            if field(line, "status") != 2:
                continue
            # A journey with several navigations keeps each one: the second
            # follow load is follow2, and so on.
            scope_counts[scope] = scope_counts.get(scope, 0) + 1
            if scope_counts[scope] > 1:
                scope = "%s%d" % (scope, scope_counts[scope])
            # first-present counts any preview frame; first-content and
            # first-text the reader's screen only ("never" leaves them out).
            for name, key in (("first-present", "first_present_ms"),
                              ("first-content", "first_content_ms"),
                              ("first-text", "first_text_ms"),
                              ("loaded", "loaded_ms")):
                value = field(line, name)
                if value is not None and value > 0:
                    metrics["load.%s.%s" % (scope, key)] = value
            # When the "preview ready" hint showed (reading-hint=SHOWN-CLEARED,
            # or none).
            hint = re.search(r"\breading-hint=(\d+)ms-", line)
            if hint and int(hint.group(1)) > 0:
                metrics["load.%s.reading_hint_ms" % scope] = int(
                    hint.group(1))
            reach = re.search(r"preview-reach=\d+px@(\d+)ms", line)
            if reach and int(reach.group(1)) > 0:
                metrics["load.%s.preview_reach_ms" % scope] = int(
                    reach.group(1))
            presses = re.search(r"load-scroll=(\d+)/(\d+)", line)
            if presses:
                metrics["load.%s.unmoved_presses" % scope] = (
                    int(presses.group(2)) - int(presses.group(1)))
    # A press at the page's edge draws nothing and is answered at once
    # (noop=1). Its sub-millisecond time would dilute the percentiles of
    # presses that move the page, so it is kept apart: a no-op that starts
    # drawing again loses noop=1 and lands in the percentiles anyway, and a
    # slow no-op shows in scroll.noop_max_ms.
    #
    # Presses made during a load are drawn first on the load preview
    # (action=preview): that line times the press to the preview frame and
    # has no elapsed= because the page owes its complete frame later. It is
    # kept apart as scroll.preview_first_visible_*.
    visible, complete, noop, preview = [], [], [], []
    for line in log.splitlines():
        if line.startswith("tilefinch-scroll-feedback:"):
            elapsed = field(line, "elapsed")
            first_visible = field(line, "first-visible")
            is_preview = re.search(r"\baction=preview\b", line)
            if first_visible is None or (elapsed is None and not is_preview):
                # A line of a shape this reader does not know would
                # otherwise drop out of the percentiles unnoticed.
                raise MetricShapeError("unrecognised scroll feedback: "
                                       + line)
            if is_preview:
                preview.append(first_visible / 1000.0)
                continue
            if re.search(r"\bnoop=1\b", line):
                noop.append(elapsed / 1000.0)
                continue
            visible.append(first_visible / 1000.0)
            complete.append(elapsed / 1000.0)
    if noop:
        metrics["scroll.noop_max_ms"] = round(max(noop), 1)
    for label, values in (("first_visible", visible),
                          ("complete", complete),
                          ("preview_first_visible", preview)):
        if not values:
            continue
        metrics["scroll.%s_p50_ms" % label] = round(
            statistics.median(values), 1)
        metrics["scroll.%s_p95_ms" % label] = round(
            percentile(values, 0.95), 1)
        metrics["scroll.%s_max_ms" % label] = round(max(values), 1)
    completions = [field(line, "elapsed") for line in log.splitlines()
                   if "tilefinch-layout-completion:" in line]
    if completions:
        metrics["layout.completion_total_ms"] = round(
            sum(completions) / 1000.0, 1)
        metrics["layout.completion_steps"] = len(completions)
    for line in log.splitlines():
        if (line.startswith("tilefinch-ui-cadence:")
                and "phase=controlled-exit" in line):
            metrics["ui.compose_avg_us"] = field(line, "compose-average")
            metrics["ui.compose_max_us"] = field(line, "compose-max")
            metrics["ui.over_33ms"] = field(line, "over-33ms")
            break
    activations = [field(line, "elapsed") for line in log.splitlines()
                   if line.startswith("tilefinch-control-activation:")
                   and field(line, "elapsed") is not None]
    if activations:
        metrics["control.activation_max_ms"] = round(
            max(activations) / 1000.0, 1)
    # Where each mark's page stylesheet came from (tilefinch-input-stylesheets):
    # sheet_compiles counts the session's loads that compiled their sheet
    # from scratch (compiled-cache misses not served by a <head> cut);
    # unrestored is 1 when the mark's sheet did not come from the cache;
    # head_takes counts loads served by a cached sheet cut back to its
    # <head> (pinned at 0 where none may be taken, such as across a night
    # mode change; a missing take shows in sheet_compiles instead);
    # head_refusals counts cached sheets the cache refused to cut back to
    # their <head>; full_relayouts counts whole-sheet rebuilds. All but
    # unrestored are cumulative for the session.
    for line in log.splitlines():
        if not line.startswith("tilefinch-input-stylesheets:"):
            continue
        mark = re.search(r"\bmark=(\S+)", line)
        cache = re.search(r"\bcompiled-cache=(\d+)/(\d+)/(\d+)", line)
        restored = field(line, "sheet-restored")
        head = re.search(r"\bhead-cache=(\d+)/(\d+)", line)
        relayouts = re.search(r"\brelayouts=(\d+)/(\d+)", line)
        if (mark is None or cache is None or restored is None
                or head is None or relayouts is None):
            raise MetricShapeError("unrecognised stylesheet report: " + line)
        prefix = "sheets.%s." % mark.group(1)
        metrics[prefix + "sheet_compiles"] = (int(cache.group(2))
                                              - int(head.group(1)))
        metrics[prefix + "unrestored"] = 0 if restored else 1
        metrics[prefix + "head_takes"] = int(head.group(1))
        metrics[prefix + "head_refusals"] = int(head.group(2))
        metrics[prefix + "full_relayouts"] = int(relayouts.group(2))
    # Relayouts after a load (tilefinch-relayout-phases): prepare is the
    # style side (sheet appends or a whole-sheet rebuild, the style and
    # image passes), build the layout pass.
    prepare, build = [], []
    for line in log.splitlines():
        match = re.search(r"tilefinch-relayout-phases: prepare=(\d+)us "
                          r"build=(\d+)us", line)
        if match:
            prepare.append(int(match.group(1)))
            build.append(int(match.group(2)))
    if prepare:
        metrics["relayout.prepare_ms"] = round(sum(prepare) / 1000.0, 1)
        metrics["relayout.build_ms"] = round(sum(build) / 1000.0, 1)
    # The session's budget peak (KiB), from the exit report.
    for line in log.splitlines():
        if (line.startswith("tilefinch-budget:")
                and "phase=controlled-exit" in line):
            peak = field(line, "peak")
            if peak is not None:
                metrics["memory.peak_kib"] = round(peak / 1024.0)
            break
    # Input-script `until` milestones (chatgpt-ask: the first is the first
    # usable input), from boot, and the page-load phase split.
    for line in log.splitlines():
        if line.startswith("tilefinch-input-script: until-met"):
            step, at_us = field(line, "step"), field(line, "at-us")
            if step is not None and at_us is not None:
                metrics["milestone.until_step%d_ms" % step] = round(
                    at_us / 1000.0)
        elif line.startswith("tilefinch-navigation-phases:") \
                and "load.phase.parse_ms" not in metrics:
            for name in ("parse", "script", "resource", "layout"):
                value = re.search(r"\b%s=(\d+)us" % name, line)
                if value:
                    metrics["load.phase.%s_ms" % name] = round(
                        int(value.group(1)) / 1000.0)
    return metrics


def clock_tz(clock, now=None):
    """A POSIX TZ string whose local time is `clock` (HH:MM) now, or None
    for `host`. The PSP's local clock is the host's, so this pins the hour
    the run starts at (and Night mode's Auto setting with it)."""
    clock = clock if clock and clock != "-" else DEFAULT_CLOCK
    if clock == "host":
        return None
    match = re.fullmatch(r"(\d{1,2}):(\d{2})", clock)
    if match is None or int(match.group(1)) > 23 or int(match.group(2)) > 59:
        raise ValueError("clock must be HH:MM or host: %r" % clock)
    target = int(match.group(1)) * 3600 + int(match.group(2)) * 60
    utc = int(time.time() if now is None else now) % 86400
    # POSIX offsets are west-positive: local = UTC - offset.
    offset = (utc - target) % 86400
    if offset > 43200:
        offset -= 86400
    sign = "-" if offset < 0 else "+"
    offset = abs(offset)
    return "TFC%s%02d:%02d:%02d" % (sign, offset // 3600,
                                     offset // 60 % 60, offset % 60)


def run_journey(journey, corpus, build_dir, runs):
    args = [RUNNER, "--script", journey["script"], "--measure",
            "--runs", str(runs), "--timeout", "1500",
            "--build-dir", build_dir]
    if journey["url"] != "-":
        args += ["--url", journey["url"]]
    if journey["data_dir"] != "-":
        args += ["--data-dir", journey["data_dir"]]
    if journey["trace"] != "-":
        args += ["--trace", os.path.join(corpus, journey["trace"]),
                 "--trace-keyed"]
    for key in journey.get("boot", "-").split(";"):
        if key and key != "-":
            args += ["--boot", key]
    clock = journey.get("cpu_mhz", "-")
    env = dict(os.environ,
               TILEFINCH_PPSSPP_CPU_MHZ=CLOCK_MHZ if clock == "-" else clock,
               TILEFINCH_PPSSPP_GRAPHICS="vulkan")
    env.pop("TILEFINCH_PPSSPP_TZ", None)
    tz = clock_tz(journey.get("clock", "-"))
    if tz is not None:
        env["TILEFINCH_PPSSPP_TZ"] = tz
    completed = subprocess.run(args, cwd=ROOT, env=env,
                               stdin=subprocess.DEVNULL,
                               capture_output=True, text=True)
    result_dir = os.path.join(build_dir, "ppsspp-input-script-latest")
    keep = os.path.join(build_dir, "perf-journeys-latest", journey["name"])
    shutil.rmtree(keep, ignore_errors=True)
    if os.path.isdir(result_dir):
        shutil.copytree(result_dir, keep)
    with open(os.path.join(os.path.dirname(keep),
                           journey["name"] + ".out"), "w") as handle:
        handle.write(completed.stdout + completed.stderr)
    if completed.returncode != 0:
        return None, completed.stdout + completed.stderr
    return read_log(os.path.join(keep, "run-1", "tilefinch-validation.txt"))


def read_log(log):
    """Metrics of one kept log, or (None, reason) when a line has a shape
    extract_metrics does not know; that journey fails, the rest still run."""
    with open(log, encoding="utf-8", errors="replace") as handle:
        try:
            return extract_metrics(handle.read()), None
        except MetricShapeError as error:
            return None, "%s: %s" % (log, error)


def compare_frames(journey, build_dir):
    """frame.MARK.differs for each `frames` spec of the journey: 0 when its
    kept capture of MARK is byte-identical to the other journey's."""
    metrics = {}
    spec = journey.get("frames", "-")
    if not spec or spec == "-":
        return metrics, None
    kept = os.path.join(build_dir, "perf-journeys-latest")
    for item in spec.split(","):
        match = re.fullmatch(r"([\w-]+)=([\w-]+):([\w-]+)", item.strip())
        if match is None:
            return None, "bad frames spec %r" % item
        mark, other, other_mark = match.groups()
        mine = os.path.join(kept, journey["name"], "run-1",
                            "frame-mark-%s.ppm" % mark)
        theirs = os.path.join(kept, other, "run-1",
                              "frame-mark-%s.ppm" % other_mark)
        for path in (mine, theirs):
            if not os.path.isfile(path):
                return None, "no frame capture " + path
        with open(mine, "rb") as a, open(theirs, "rb") as b:
            metrics["frame.%s.differs" % mark] = 0 if a.read() == b.read() \
                else 1
    return metrics, None


def read_artifacts(journey, build_dir):
    log = os.path.join(build_dir, "perf-journeys-latest", journey["name"],
                       "run-1", "tilefinch-validation.txt")
    if not os.path.isfile(log):
        return None, "no kept log " + log
    return read_log(log)


def main():
    sys.stdout.reconfigure(line_buffering=True)
    parser = argparse.ArgumentParser()
    parser.add_argument("--journey", action="append", default=[])
    parser.add_argument("--corpus", default=os.path.join(ROOT, "perf",
                                                          "traces"))
    parser.add_argument("--build-dir", default=os.path.join(
        ROOT, "build-preset-psp-validation"))
    parser.add_argument("--runs", type=int, default=1)
    parser.add_argument("--record", action="store_true")
    parser.add_argument("--ratchet", action="store_true")
    parser.add_argument("--record-missing", action="append", default=[],
                        metavar="PREFIX",
                        help="add a baseline for each metric starting "
                             "with PREFIX that has none; existing rows "
                             "are only compared")
    parser.add_argument("--from-artifacts", action="store_true",
                        help="compare the last run's kept logs instead of "
                             "running PPSSPP again")
    parser.add_argument("--prepare-trace", nargs=2,
                        metavar=("CAPTURE_DIR", "NAME"))
    options = parser.parse_args()
    if options.prepare_trace:
        prepare_trace(options.prepare_trace[0], options.prepare_trace[1],
                      options.corpus)
        return 0

    journeys = read_tsv(MANIFEST)
    if options.journey:
        journeys = [j for j in journeys if j["name"] in options.journey]
    baselines = read_tsv(BASELINES) if os.path.exists(BASELINES) else []
    table = {(b["journey"], b["metric"]): b for b in baselines}
    failed = False
    for journey in journeys:
        if journey["trace"] != "-":
            trace = os.path.join(options.corpus, journey["trace"])
            if not os.path.isdir(trace):
                print("%s: skipped (no trace %s)" % (journey["name"], trace))
                continue
            digest = trace_sha256(trace)
            if digest != journey["trace_sha256"]:
                print("%s: FAIL trace sha256 %s, manifest pins %s" % (
                    journey["name"], digest, journey["trace_sha256"]))
                failed = True
                continue
        if options.from_artifacts:
            metrics, error = read_artifacts(journey, options.build_dir)
        else:
            metrics, error = run_journey(journey, options.corpus,
                                         options.build_dir, options.runs)
        if metrics is not None:
            marks = [m for m in journey.get("sheet_marks", "-").split(",")
                     if m and m != "-"]
            metrics = {m: v for m, v in metrics.items()
                       if not m.startswith("sheets.")
                       or m.split(".")[1] in marks}
            frames, error = compare_frames(journey, options.build_dir)
            if frames is None:
                metrics = None
            else:
                metrics.update(frames)
        if metrics is None:
            print("%s: FAIL run\n%s" % (journey["name"], error[-2000:]))
            failed = True
            continue
        expected = {m: b for (j, m), b in table.items()
                    if j == journey["name"]}
        for metric in sorted(set(metrics) | set(expected)):
            value = metrics.get(metric)
            base = expected.get(metric)
            if options.record and value is not None:
                table[(journey["name"], metric)] = {
                    "journey": journey["name"], "metric": metric,
                    "value": str(value),
                    "tolerance": str(round(tolerance_for(metric, value), 1))}
                print("%s %s = %s (recorded)" % (
                    journey["name"], metric, value))
                continue
            if base is None:
                if value is not None and any(
                        metric.startswith(prefix)
                        for prefix in options.record_missing):
                    table[(journey["name"], metric)] = {
                        "journey": journey["name"], "metric": metric,
                        "value": str(value),
                        "tolerance": str(round(
                            tolerance_for(metric, value), 1))}
                    print("%s %s = %s (recorded, was missing)" % (
                        journey["name"], metric, value))
                    continue
                print("%s %s = %s (no baseline)" % (
                    journey["name"], metric, value))
                continue
            if value is None:
                print("%s %s: FAIL missing (baseline %s)" % (
                    journey["name"], metric, base["value"]))
                failed = True
                continue
            limit = float(base["value"]) + float(base["tolerance"])
            status = "ok"
            if value > limit:
                status = "REGRESSION"
                failed = True
            elif value < float(base["value"]) - float(base["tolerance"]):
                status = "improved"
                if options.ratchet:
                    base["value"] = str(value)
                    base["tolerance"] = str(round(
                        tolerance_for(metric, value), 1))
                    status = "improved (ratcheted)"
            print("%s %s = %s (baseline %s +%s) %s" % (
                journey["name"], metric, value, base["value"],
                base["tolerance"], status))
    if options.record or options.ratchet or options.record_missing:
        with open(BASELINES, "w", encoding="utf-8") as handle:
            handle.write(
                "# Written by benchmarks/run-perf-journeys.py; see\n"
                "# docs/engineering/PERF_JOURNEYS.md. Lower is better.\n")
            handle.write("journey\tmetric\tvalue\ttolerance\n")
            for key in sorted(table):
                row = table[key]
                handle.write("%s\t%s\t%s\t%s\n" % (
                    row["journey"], row["metric"], row["value"],
                    row["tolerance"]))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
