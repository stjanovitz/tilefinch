#!/usr/bin/env python3
"""Site census driver: live capture, offline keyed replay, metrics JSONL.

  census.py capture [--retry] [SELECTOR...]      live network, one page each
  census.py replay  [-j N] [--out DIR] [--probe]
                    [--revisit | --revisit-ticked | --restart] [--idle N]
                    [--verify-style-appends] [--user-css-before-load]
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
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(REPO, "tools"))
sys.path.insert(0, os.path.join(REPO, "benchmarks"))
import extract  # noqa: E402
import style_append_verify  # noqa: E402
import trace_document  # noqa: E402

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
# --focus-moves: idle turns after the moves (a bounded focus layout's
# completion is one turn; image and font work may take others).
FOCUS_IDLE_TURNS = 20

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


def command_script(frames, probe, revisit_url=None, idle=0, focus_moves=0,
                   focus_hashes="each"):
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
    if focus_moves:
        # D-pad focus moves after every frame and page fact is taken, so
        # they change no frame; extract.py reports their latency and the
        # layout-reuse resets they caused. A hash of the frame after each
        # move, and after the idle turns that complete a provisional
        # layout, lets an A/B compare every presented frame. Each hash
        # renders the frame again: on 12 pages (2026-10-26) that left the
        # next moves' glyph misses, decodes and scales unchanged (it warms
        # nothing) but put focus_ms 1-2% above --focus-hashes final, so
        # compare focus timings only between runs of one mode (compare.py
        # warns when the modes differ).
        for move in range(focus_moves):
            lines += ["focus-next"]
            if focus_hashes == "each":
                lines += ["frame-hash focus-%d" % (move + 1)]
        lines += ["idle %d" % FOCUS_IDLE_TURNS, "frame-hash focus-final"]
    if probe:
        # Basic view retires the page realm and rebuilds the DOM, which would
        # rewrite the end-of-run summary, so only the probe pass takes it.
        lines += ["basic-on", "render %s/basic.ppm" % frames]
    lines += ["status", "quit"]
    return "\n".join(lines) + "\n"


def lab_command(lab, site, run_dir, network_args, reload_count,
                commands="commands", user_css_before_load=False):
    args = [lab, "--url", site["url"], "--fetch-scripts",
            "--psp-profile", "realistic",
            "--content-blocker", "basic", "--hide-cookie-banners",
            "--deterministic-replay-seed", "1",
            "--commands", os.path.join(run_dir, commands),
            "--no-loop-capture",
            "--output", os.path.join(run_dir, "frames", "final.ppm")]
    if reload_count:
        args += ["--reload", str(reload_count)]
    if user_css_before_load:
        # The cookie-banner sheet is set before the load, as the PSP app
        # sets its presentation CSS, instead of applied after it with a
        # whole-sheet rebuild the device never does.
        args += ["--user-css-before-load"]
    # Per-page lab arguments from the page's corpus meta.json
    # ("replay_lab_args"), such as a tighter --limit-mb for a canary entry.
    args += site.get("lab_args", [])
    # Experiment knobs (for example --classic-bytecode-cache-kb 2048).
    args += os.environ.get("CENSUS_LAB_ARGS", "").split()
    return args + network_args


def run_page(lab, site, run_dir, network_args, mode, probe=False, revisit=False,
             timeout=600, revisit_ticked=False, restart=False, idle=0,
             focus_moves=0, focus_hashes="each", verify_style_appends=False,
             user_css_before_load=False):
    if os.path.isdir(run_dir):
        shutil.rmtree(run_dir)
    frames = os.path.join(run_dir, "frames")
    os.makedirs(frames)
    with open(os.path.join(run_dir, "commands"), "w") as f:
        f.write(command_script(frames, probe,
                               site["url"] if revisit_ticked else None,
                               idle, focus_moves, focus_hashes))
    env = dict(os.environ)
    env["TILEFINCH_TRACE_CENSUS"] = "1"
    for name in ("TILEFINCH_LAB_INIT_SCRIPT", "TILEFINCH_TRACE_SCRIPT_FAILURES",
                 "TILEFINCH_TRACE_STYLE_APPEND_VERIFY"):
        env.pop(name, None)
    if verify_style_appends:
        # Every in-place sheet change is followed by a forced rebuild and
        # compared (lab.err); see tools/style_append_verify.py.
        env["TILEFINCH_TRACE_STYLE_APPEND_VERIFY"] = "1"
    if probe:
        env["TILEFINCH_LAB_INIT_SCRIPT"] = os.path.join(HERE, "api-probe.js")
        # Script admission, fetch and quota refusals with URLs (stderr). Only
        # in the diagnostic pass: it also allocates a failure record per
        # inline script, which more than doubled amazon.com's lab load.
        env["TILEFINCH_TRACE_SCRIPT_FAILURES"] = "1"
    args = lab_command(lab, site, run_dir, network_args, 1 if revisit else 0,
                       user_css_before_load=user_css_before_load)
    started = time.time()
    if restart:
        # A browser restart between visits: the first visit runs in its own
        # process, writing the persistent compiled-script tier in a fresh
        # directory, and the measured visit is a new process reading it.
        cache = os.path.join(run_dir, "script-cache")
        with open(os.path.join(run_dir, "commands.first"), "w") as f:
            f.write(first_visit_script())
        first = lab_command(lab, site, run_dir, network_args, 0,
                            "commands.first",
                            user_css_before_load=user_css_before_load) + [
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
                    + ("+idle" if idle else "")
                    + ("+verify-style-appends" if verify_style_appends else "")
                    + ("+user-css-before-load" if user_css_before_load else ""),
            "exit": code, "wall_s": round(time.time() - started, 2)}
    with open(os.path.join(run_dir, "meta.json"), "w") as f:
        json.dump(meta, f)
    return meta


def capture(args):
    sites = select(load_sites(args.sites), args.selectors)
    corpus = corpus_dir(args)
    moved = []
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
            final, reason = capture_destination(
                os.path.join(page_dir, "capture-run"), trace, site["url"])
            meta.update({"captured_at": time.strftime("%Y-%m-%dT%H:%M:%S"),
                         "attempt": attempt + 1, "http_status": rec["http_status"],
                         "outcome_auto": rec["outcome_auto"],
                         "final_url": final})
            if reason:
                meta["final_url_mismatch"] = reason
            with open(os.path.join(page_dir, "meta.json"), "w") as f:
                json.dump(meta, f, indent=1)
            print("%-20s exit=%s http=%s outcome=%s wall=%.1fs%s" % (
                site["name"], meta["exit"], rec["http_status"],
                rec["outcome_auto"], meta["wall_s"],
                " FAIL %s: landed on %s" % (reason, final) if reason else ""),
                flush=True)
            if reason:
                # Not the page asked for (old-reddit's capture is a login
                # redirect): a finding, so no retry, and the run fails.
                moved.append((site["name"], reason, final))
                break
            # The trace is usable once published, whatever the lab's exit
            # status (capture-mode teardown can report FAIL on pages that set
            # cookies; replays of the same trace pass). Retry only a capture
            # that did not publish or never reached the server; a block or
            # challenge is a finding, not a reason to retry.
            if trace_complete(trace) and rec["http_status"] != 0:
                break
    for name, reason, final in moved:
        print("capture of %s is not the requested page (%s): %s" % (
            name, reason, final))
    if moved:
        sys.exit(1)


def capture_destination(run_dir, trace, url):
    """(final URL, mismatch reason or None) of a capture: a final host or
    path other than the requested one means the capture holds some other
    page (a sign-in wall, a regional or consent redirect). The final URL is
    the page URL the lab reported last (it follows redirects and native
    routes such as YouTube's, whose document is not a trace record); a run
    that reported none falls back to the trace's redirect chain."""
    final = None
    try:
        with open(os.path.join(run_dir, "lab.log"), errors="replace") as f:
            for line in f:
                if line.startswith("loop frame="):
                    match = re.search(r'\burl="([^"]*)"', line)
                    final = match.group(1) if match else final
    except OSError:
        pass
    if not final:
        try:
            final = trace_document.final_url(trace, url)
        except (OSError, ValueError) as error:
            return None, "unreadable-trace: %s" % error
    return final, trace_document.moved(url, final)


def trace_complete(trace):
    try:
        with open(os.path.join(trace, "trace.meta")) as f:
            return "capture-complete=yes" in f.read()
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
        meta_path = os.path.join(corpus, site["name"], "meta.json")
        if os.path.exists(meta_path):
            with open(meta_path) as f:
                lab_args = json.load(f).get("replay_lab_args", [])
            site = dict(site, lab_args=[str(a) for a in lab_args])
        reps = []
        for rep in range(args.repeat):
            run_dir = os.path.join(out, site["name"] + ("" if rep == 0 else ".r%d" % (rep + 1)))
            run_page(args.lab, site, run_dir,
                     ["--replay-http-response-keyed", trace],
                     "replay", probe=args.probe, revisit=args.revisit,
                     revisit_ticked=args.revisit_ticked,
                     restart=args.restart, idle=args.idle,
                     focus_moves=args.focus_moves,
                     focus_hashes=args.focus_hashes,
                     verify_style_appends=args.verify_style_appends,
                     user_css_before_load=args.user_css_before_load)
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
                   "idle": args.idle,
                   "focus_moves": args.focus_moves,
                   "focus_hashes": args.focus_hashes,
                   "verify_style_appends": args.verify_style_appends,
                   "user_css_before_load": args.user_css_before_load},
                  f, indent=1)
    print("replayed %d pages in %.1f s with %d jobs -> %s" % (
        len(records), wall, args.jobs, jsonl))
    if args.verify_style_appends and not verify_style_appends(out):
        sys.exit(1)


def verify_style_appends(out):
    """Judges every run's style-append-verify lines (tools/
    style_append_verify.py, not strict: a layout-only difference on a
    captured page is image-load timing and is listed, not failed), writes
    style-append-verify.tsv and returns whether the replay passed: no check
    failed or differed, and at least one ran."""
    rows, everything, failed = [], [], False
    for name in sorted(os.listdir(out)):
        log = os.path.join(out, name, "lab.err")
        if not os.path.isfile(log):
            continue
        with open(log, encoding="utf-8", errors="replace") as handle:
            found = style_append_verify.checks(handle.read())
        everything += found
        problems, summary = style_append_verify.judge(found, strict=False)
        if not found:
            problems = []  # a page that changed no sheet in place
        failed = failed or bool(problems)
        rows.append((name, summary, problems))
    problems, total = style_append_verify.judge(everything, strict=False)
    if not everything:
        failed = True
    with open(os.path.join(out, "style-append-verify.tsv"), "w") as f:
        f.write("page\tchecks\tconclusive\tinconclusive\tlayout_only\t"
                "problems\n")
        for name, summary, page_problems in rows:
            f.write("%s\t%d\t%d\t%d\t%d\t%s\n" % (
                name, summary["checks"], summary["conclusive"],
                summary["inconclusive"], summary["layout_only"],
                "; ".join(sorted(set(page_problems))) or "-"))
    for name, summary, page_problems in rows:
        if summary["checks"] or page_problems:
            print("%-20s checks=%d conclusive=%d inconclusive=%d "
                  "layout-only=%d%s" % (
                      name, summary["checks"], summary["conclusive"],
                      summary["inconclusive"], summary["layout_only"],
                      "".join("\n    FAIL " + p
                              for p in sorted(set(page_problems)))))
    print("style-append-verify: %d pages with checks, %d checks, %d "
          "conclusive, %d inconclusive, %d layout-only differences%s" % (
              sum(1 for _, summary, _ in rows if summary["checks"]),
              total["checks"], total["conclusive"], total["inconclusive"],
              total["layout_only"],
              "" if everything else "; FAIL no check ran"))
    return not failed


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
    if any(r.get(key) != rec.get(key) for r in reps
           for key in ("focus_move_hashes", "focus_after_hashes")):
        rec["focus_frames_unstable"] = True
    rec["wall_s"] = sum(r["wall_s"] or 0 for r in reps)
    return rec


def page_cost(corpus, site):
    """Previous capture wall time, to start the slowest pages first."""
    try:
        with open(os.path.join(corpus, site["name"], "meta.json")) as f:
            return json.load(f)["wall_s"]
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
    p.add_argument("--focus-moves", type=int, default=0,
                   help="D-pad focus moves (focus-next) after the frames and "
                        "page facts; timing.focus_ms and focus_full_resets "
                        "report them (default none)")
    p.add_argument("--focus-hashes", choices=("each", "final"), default="each",
                   help="frame-hash renders with --focus-moves: 'each' "
                        "hashes the frame after every move and after the "
                        "idle turns, 'final' only after the idle turns "
                        "(default each)")
    p.add_argument("--verify-style-appends", action="store_true",
                   help="after every in-place sheet change force a rebuild "
                        "and compare (TILEFINCH_TRACE_STYLE_APPEND_VERIFY=1); "
                        "fails when a check fails or differs, or none ran. "
                        "Each check costs a rebuild: never use for timing")
    p.add_argument("--user-css-before-load", action="store_true",
                   help="set the cookie-banner user sheet before the load, "
                        "as the PSP app does, instead of after it with a "
                        "whole-sheet rebuild; use it for timing")
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
