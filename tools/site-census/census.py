#!/usr/bin/env python3
"""Site census driver: live capture, offline keyed replay, metrics JSONL.

  census.py capture [--retry] [SELECTOR...]      live network, one page each
  census.py replay  [-j N] [--out DIR] [--probe]
                    [--revisit | --revisit-ticked | --restart] [--idle N]
                    [SELECTOR...]
  census.py extract RUN_ROOT                     re-extract a replay's JSONL
  census.py sheet OUT.png RUN_ROOT NAME...       480x272 screenshot sheet

SELECTOR is a page name or a tag from sites.tsv (default: every page).
The corpus selected by CENSUS_DIR holds private captured pages and
must never be committed: corpus/NAME/capture is the HTTP trace, meta.json the
page's URL and tags.

Every run uses Tilefinch's honest default UA, the PSP app's realistic profile
(--psp-profile realistic: memory, script, document caps), basic content
blocking and hidden cookie notices, at 480x272, with a seeded clock and
random source. Replays are response-keyed and never touch the network.
"""
import argparse
import concurrent.futures
import json
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import extract  # noqa: E402

DEFAULT_CENSUS = os.environ.get(
    "CENSUS_DIR", os.path.normpath(os.path.join(REPO, "..", "census")))
PROBE_CHUNKS = 64
TICKS = "tick 150 33"
# Page-idle turns after a visit's ticks (at most this many; the lab's "idle"
# stops at the first turn that leaves no work): the lab's "tick" advances only the
# page runtime, while the PSP loop also runs the engine's idle work (deferred
# bytecode stores, persistent-tier writes, tiles, background resources)
# whenever the page is quiet. A visit that is followed by another runs these.
IDLE_TURNS = 300

# Page facts the native summary lacks. Kept allocation-light: a page at its
# JS heap limit fails anything bigger (innerText of bbc.co.uk articles ran out
# of memory), and pages without scripts have no realm at all; text size and
# preview come from the native document summary instead.
PAGE_JS = (
    "'page=' + JSON.stringify({"
    "scrollHeight: document.documentElement.scrollHeight, "
    "scrollWidth: document.documentElement.scrollWidth, "
    "images: document.images.length, scripts: document.scripts.length, "
    "forms: document.forms.length, "
    "iframes: document.getElementsByTagName('iframe').length})"
)


def load_sites(path=None):
    path = path or os.path.join(HERE, "sites.tsv")
    sites = []
    for line in open(path):
        line = line.rstrip("\n")
        if not line or line.startswith("#"):
            continue
        name, tags, url = line.split("\t")
        sites.append({"name": name, "tags": tags.split(","), "url": url})
    return sites


def corpus_dir(args):
    return args.corpus or os.path.join(args.census, "corpus")


def select(sites, selectors):
    if not selectors:
        return sites
    wanted = set(selectors)
    picked = [s for s in sites if s["name"] in wanted or wanted & set(s["tags"])]
    unknown = wanted - {s["name"] for s in sites} - {t for s in sites for t in s["tags"]}
    if unknown:
        sys.exit("unknown selector(s): " + ", ".join(sorted(unknown)))
    return picked


def first_visit_script():
    """--restart's first process: one visit with its ticks and idle turns,
    then exit (the next process is the measured visit)."""
    return "\n".join(["status", TICKS, "idle %d" % IDLE_TURNS, "status",
                      "quit"]) + "\n"


def command_script(frames, probe, revisit_url=None, idle=0):
    # --revisit-ticked: run the first visit for the same ticks (so its async
    # and dynamic scripts load too) and its idle turns (the PSP loop runs
    # idle work whenever the page is quiet: classic bytecode is stored
    # there), then load the page URL again (not "reload": a page may have
    # replaced its own URL) and measure that visit.
    lines = (["status", TICKS, "idle %d" % IDLE_TURNS, "go " + revisit_url]
             if revisit_url else [])
    lines += ["status", TICKS]
    if idle:
        # Owner idle turns after the ticks, as the PSP loop runs between
        # frames: background images, fonts, display retargeting and the
        # classic-bytecode stores.
        lines += ["idle %d" % idle]
    lines += ["status",
             "render %s/top.ppm" % frames,
             "page-down", "render %s/p1.ppm" % frames,
             "page-down", "render %s/p2.ppm" % frames,
             "top", "census", "js " + PAGE_JS]
    if probe:
        lines += ["js 'probe-chunk=' + (globalThis.__tfApiProbe ? "
                  "__tfApiProbe.chunk(%d) : '')" % i for i in range(PROBE_CHUNKS)]
    lines += ["script-report"]
    if probe:
        # Basic view retires the page realm and rebuilds the DOM, which would
        # rewrite the end-of-run summary, so only the probe pass takes it.
        lines += ["basic-on", "render %s/basic.ppm" % frames]
    lines += ["status", "quit"]
    return "\n".join(lines) + "\n"


def lab_command(lab, site, run_dir, network_args, reload_count,
                commands="commands"):
    args = [lab, "--url", site["url"], "--fetch-scripts",
            "--psp-profile", "realistic",
            "--content-blocker", "basic", "--hide-cookie-banners",
            "--deterministic-replay-seed", "1",
            "--commands", os.path.join(run_dir, commands),
            "--no-loop-capture",
            "--output", os.path.join(run_dir, "frames", "final.ppm")]
    if reload_count:
        args += ["--reload", str(reload_count)]
    # Experiment knobs (for example --classic-bytecode-cache-kb 2048).
    args += os.environ.get("CENSUS_LAB_ARGS", "").split()
    return args + network_args


def run_page(lab, site, run_dir, network_args, mode, probe=False, revisit=False,
             timeout=600, revisit_ticked=False, restart=False, idle=0):
    if os.path.isdir(run_dir):
        shutil.rmtree(run_dir)
    frames = os.path.join(run_dir, "frames")
    os.makedirs(frames)
    with open(os.path.join(run_dir, "commands"), "w") as f:
        f.write(command_script(frames, probe,
                               site["url"] if revisit_ticked else None,
                               idle))
    env = dict(os.environ)
    env["TILEFINCH_TRACE_CENSUS"] = "1"
    for name in ("TILEFINCH_LAB_INIT_SCRIPT", "TILEFINCH_TRACE_SCRIPT_FAILURES"):
        env.pop(name, None)
    if probe:
        env["TILEFINCH_LAB_INIT_SCRIPT"] = os.path.join(HERE, "api-probe.js")
        # Script admission, fetch and quota refusals with URLs (stderr). Only
        # in the diagnostic pass: it also allocates a failure record per
        # inline script, which more than doubled amazon.com's lab load.
        env["TILEFINCH_TRACE_SCRIPT_FAILURES"] = "1"
    args = lab_command(lab, site, run_dir, network_args, 1 if revisit else 0)
    started = time.time()
    if restart:
        # A browser restart between visits: the first visit runs in its own
        # process, writing the persistent compiled-script tier in a fresh
        # directory, and the measured visit is a new process reading it.
        cache = os.path.join(run_dir, "script-cache")
        with open(os.path.join(run_dir, "commands.first"), "w") as f:
            f.write(first_visit_script())
        first = lab_command(lab, site, run_dir, network_args, 0,
                            "commands.first") + [
            "--script-cache-dir", cache, "--script-cache-write"]
        with open(os.path.join(run_dir, "lab.first.log"), "w") as out, \
                open(os.path.join(run_dir, "lab.first.err"), "w") as err:
            try:
                subprocess.run(first, stdout=out, stderr=err, env=env,
                               timeout=timeout, cwd=run_dir)
            except subprocess.TimeoutExpired:
                pass
        args += ["--script-cache-dir", cache, "--script-cache-write"]
        started = time.time()
    with open(os.path.join(run_dir, "lab.log"), "w") as out, \
            open(os.path.join(run_dir, "lab.err"), "w") as err:
        try:
            code = subprocess.run(args, stdout=out, stderr=err, env=env,
                                  timeout=timeout, cwd=run_dir).returncode
        except subprocess.TimeoutExpired:
            code = "timeout"
    meta = {"name": site["name"], "url": site["url"], "tags": site["tags"],
            "mode": mode + ("+probe" if probe else "") + ("+revisit" if revisit else "")
                    + ("+revisit-ticked" if revisit_ticked else "")
                    + ("+restart" if restart else "")
                    + ("+idle" if idle else ""),
            "exit": code, "wall_s": round(time.time() - started, 2)}
    with open(os.path.join(run_dir, "meta.json"), "w") as f:
        json.dump(meta, f)
    return meta


def capture(args):
    sites = select(load_sites(args.sites), args.selectors)
    corpus = corpus_dir(args)
    for site in sites:
        page_dir = os.path.join(corpus, site["name"])
        if trace_complete(os.path.join(page_dir, "capture")) and not args.force:
            print("skip %s (captured; --force to recapture)" % site["name"])
            continue
        attempts = 2 if args.retry else 1
        for attempt in range(attempts):
            trace = os.path.join(page_dir, "capture")
            if os.path.isdir(trace):
                shutil.rmtree(trace)
            os.makedirs(page_dir, exist_ok=True)
            meta = run_page(args.lab, site, os.path.join(page_dir, "capture-run"),
                            ["--capture-http", trace], "capture")
            rec = extract.extract(os.path.join(page_dir, "capture-run"))
            meta.update({"captured_at": time.strftime("%Y-%m-%dT%H:%M:%S"),
                         "attempt": attempt + 1, "http_status": rec["http_status"],
                         "outcome_auto": rec["outcome_auto"]})
            with open(os.path.join(page_dir, "meta.json"), "w") as f:
                json.dump(meta, f, indent=1)
            print("%-20s exit=%s http=%s outcome=%s wall=%.1fs" % (
                site["name"], meta["exit"], rec["http_status"],
                rec["outcome_auto"], meta["wall_s"]), flush=True)
            # The trace is usable once published, whatever the lab's exit
            # status (capture-mode teardown can report FAIL on pages that set
            # cookies; replays of the same trace pass). Retry only a capture
            # that did not publish or never reached the server; a block or
            # challenge is a finding, not a reason to retry.
            if trace_complete(trace) and rec["http_status"] != 0:
                break


def trace_complete(trace):
    try:
        return "capture-complete=yes" in open(os.path.join(trace, "trace.meta")).read()
    except OSError:
        return False


def replay(args):
    corpus = corpus_dir(args)
    sites = [s for s in select(load_sites(args.sites), args.selectors)
             if trace_complete(os.path.join(corpus, s["name"], "capture"))]
    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)
    started = time.time()

    def one(site):
        trace = os.path.join(corpus, site["name"], "capture")
        reps = []
        for rep in range(args.repeat):
            run_dir = os.path.join(out, site["name"] + ("" if rep == 0 else ".r%d" % (rep + 1)))
            run_page(args.lab, site, run_dir,
                     ["--replay-http-response-keyed", trace],
                     "replay", probe=args.probe, revisit=args.revisit,
                     revisit_ticked=args.revisit_ticked,
                     restart=args.restart, idle=args.idle)
            reps.append(extract.extract(run_dir))
            if (reps[0]["wall_s"] or 0) > 20:
                # A page that runs into the 60 s script watchdog is
                # dominated by that fixed timeout; repeating adds nothing.
                break
        return merge_repeats(reps)

    records = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for rec in pool.map(one, sorted(sites, key=lambda s: -page_cost(corpus, s))):
            records.append(rec)
            if not args.quiet:
                print("%-20s %-10s paint=%-7s loaded=%-7s peak=%sMB" % (
                    rec["name"], rec["outcome_auto"], rec["timing"]["first_paint_ms"],
                    rec["timing"]["loaded_ms"], rec["memory"]["peak_mb"]), flush=True)
    wall = time.time() - started
    records.sort(key=lambda r: r["name"])
    jsonl = os.path.join(out, "census.jsonl")
    with open(jsonl, "w") as f:
        for rec in records:
            f.write(json.dumps(rec, sort_keys=True) + "\n")
    with open(os.path.join(out, "run.json"), "w") as f:
        json.dump({"pages": len(records), "jobs": args.jobs, "wall_s": round(wall, 1),
                   "lab": os.path.realpath(args.lab), "probe": args.probe,
                   "repeat": args.repeat,
                   "revisit": args.revisit,
                   "revisit_ticked": args.revisit_ticked,
                   "restart": args.restart,
                   "idle": args.idle}, f, indent=1)
    print("replayed %d pages in %.1f s with %d jobs -> %s" % (
        len(records), wall, args.jobs, jsonl))


TIMED_JS = ("compile_ms", "execute_ms", "pre_paint_ms", "pre_paint_nonessential_ms",
            "cacheable_compile_ms", "inline_compile_ms")


def merge_repeats(reps):
    """Timings are host wall time and move with machine load; take each
    timing's minimum over the repeats (the least-disturbed run). Everything
    else (memory, work counts, errors) comes from the first run, and the
    per-script list from the run with the least JS time."""
    rec = reps[0]
    if len(reps) == 1:
        return rec
    # Pick the per-script source before rec's totals are replaced below.
    best = min(reps, key=lambda r: r["js"]["compile_ms"] + r["js"]["execute_ms"])
    best_scripts, best_categories = best["script_list"], best["js"]["by_category"]
    for key, value in list(rec["timing"].items()):
        if isinstance(value, (int, float)) and not isinstance(value, bool):
            values = [r["timing"].get(key) for r in reps
                      if isinstance(r["timing"].get(key), (int, float))]
            rec["timing"][key] = min(values) if values else value
    for key in TIMED_JS:
        rec["js"][key] = min(r["js"][key] for r in reps)
    rec["script_list"] = best_scripts
    rec["js"]["by_category"] = best_categories
    rec["repeats"] = len(reps)
    rec["wall_s"] = sum(r["wall_s"] or 0 for r in reps)
    return rec


def page_cost(corpus, site):
    """Previous capture wall time, to start the slowest pages first."""
    try:
        return json.load(open(os.path.join(corpus, site["name"], "meta.json")))["wall_s"]
    except (OSError, ValueError, KeyError):
        return 0


def reextract(args):
    root = os.path.abspath(args.run_root)
    groups = {}
    for name in sorted(os.listdir(root)):
        if os.path.exists(os.path.join(root, name, "meta.json")):
            groups.setdefault(name.split(".r")[0], []).append(
                extract.extract(os.path.join(root, name)))
    records = [merge_repeats(groups[n]) for n in sorted(groups)]
    with open(os.path.join(root, "census.jsonl"), "w") as f:
        for rec in records:
            f.write(json.dumps(rec, sort_keys=True) + "\n")
    print("%d records -> %s" % (len(records), os.path.join(root, "census.jsonl")))


def sheet(args):
    import sheet as sheet_mod
    rows = []
    for name in args.names:
        frames = os.path.join(args.run_root, name, "frames")
        rows.append([os.path.join(frames, f) for f in ("top.ppm", "p1.ppm", "p2.ppm")])
    sheet_mod.write_grid(args.output, rows)
    print("wrote", args.output)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--census", default=DEFAULT_CENSUS,
                        help="census directory (corpus/ lives here)")
    parser.add_argument("--corpus", default=None,
                        help="corpus directory, for another corpus generation "
                             "(default: CENSUS/corpus)")
    parser.add_argument("--sites", default=None,
                        help="page list (default: sites.tsv next to this "
                             "script)")
    parser.add_argument("--lab", default=os.path.join(
        REPO, "build-preset-release", "psp-browser-interactive-lab"))
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("capture")
    p.add_argument("--retry", action="store_true", help="one retry on failure")
    p.add_argument("--force", action="store_true")
    p.add_argument("selectors", nargs="*")
    p.set_defaults(func=capture)
    p = sub.add_parser("replay")
    p.add_argument("-j", "--jobs", type=int, default=max(1, (os.cpu_count() or 4) - 4))
    p.add_argument("--out", required=True)
    p.add_argument("--probe", action="store_true",
                   help="API-probe pass (perturbs timing; separate output)")
    p.add_argument("--revisit", action="store_true",
                   help="load twice in one session (--reload 1); metrics are "
                        "for the second visit")
    p.add_argument("--revisit-ticked", action="store_true",
                   help="like --revisit, but the first visit also runs its "
                        "ticks (async and dynamic scripts) and up to %d idle "
                        "turns before the page is loaded again" % IDLE_TURNS)
    p.add_argument("--restart", action="store_true",
                   help="like --revisit-ticked, but the first visit runs in "
                        "its own lab process and the measured visit in a new "
                        "one sharing a fresh persistent compiled-script "
                        "directory (a browser restart between visits)")
    p.add_argument("--idle", type=int, default=0,
                   help="owner idle turns after the ticks (background "
                        "images, fonts, display retargeting, classic "
                        "bytecode stores); default none")
    p.add_argument("--repeat", type=int, default=3,
                   help="runs per page; timings are the minimum (default 3)")
    p.add_argument("--quiet", action="store_true")
    p.add_argument("selectors", nargs="*")
    p.set_defaults(func=replay)
    p = sub.add_parser("extract")
    p.add_argument("run_root")
    p.set_defaults(func=reextract)
    p = sub.add_parser("sheet")
    p.add_argument("output")
    p.add_argument("run_root")
    p.add_argument("names", nargs="+")
    p.set_defaults(func=sheet)
    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
