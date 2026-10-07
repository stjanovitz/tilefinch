#!/usr/bin/env python3
"""Record Treadline's standard scenarios in PPSSPP and assemble one review folder.

The milestone visual review (docs/DEVELOPMENT.md, "Treadline visual QA"):
for each scenario and clock it packages the game with the scenario's URL,
records a PPSSPP run (scripts/run-ppsspp-input-script.sh --record-video),
runs scripts/analyze-game-video.py, and builds an overview contact sheet (one
frame every two seconds). It also runs the reference-screenshot check
(scripts/check-treadline-references.py) and writes index.html and index.md
at the top of the folder: per scenario, the event summary, the overview
sheet, and every flagged event with its contact sheet, zoomed sheet (blinks)
and clip, followed by the review checklist. A person or an agent reads the
index top to bottom.

    scripts/visual-review.py --build-dir build-preset-psp-validation --out review

Scenarios (--scenarios, comma-separated; default: the standard set):
  soak            Quick Match long soak, seed 12345 (fixed and follow cameras)
  onslaught-2     Onslaught long soak, seed 2
  onslaught-7     Onslaught long soak, seed 7
  onslaught-12345 Onslaught long soak, seed 12345
  mission-1-1     campaign mission 1-1, real pad input
  mission-3-4     campaign mission 3-4 (Glacier), real pad input
  range           Practice Range free range, real pad input
--clock picks 111 (device-like 1/30 s steps, the default), normal (PPSSPP's
own clock, about 58 presented frames a second) or both. --software records
with PPSSPP's software renderer as well: the hardware renderer does not
show browser chrome or toasts drawn by the CPU over the game's frames, so
overlay glitches only appear in software runs (labelled -sw; slower).

The build directory is a validation PSP build (EBOOT.PBP and its
CMakeCache.txt); game JavaScript is packaged from this checkout, so an older
EBOOT still reviews the current game. Recordings are deleted after analysis
unless --keep-video. Exit 0 when every run recorded and was analyzed; with
--strict, also fail when the analyzer counts problems or a reference
screenshot changed.
"""
import argparse
import html
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
SCENARIOS = {
    "soak": ("treadline-long-soak", "qualification=long-soak&seed=12345"),
    "onslaught-2": ("treadline-long-soak", "qualification=long-soak&seed=2&mode=onslaught"),
    "onslaught-7": ("treadline-long-soak", "qualification=long-soak&seed=7&mode=onslaught"),
    "onslaught-12345": ("treadline-long-soak",
                        "qualification=long-soak&seed=12345&mode=onslaught"),
    "mission-1-1": ("treadline-visual-play", "qualification=visual&visual=mission&mission=0"),
    "mission-3-4": ("treadline-visual-play", "qualification=visual&visual=mission&mission=13"),
    "range": ("treadline-visual-play", "qualification=visual&visual=range"),
}
STANDARD = ["soak", "onslaught-2", "onslaught-7", "mission-1-1", "range"]
CLOCKS = {"111": "111", "normal": "0"}

CHECKLIST = """\
## Review checklist

Look at every overview sheet, then every flagged event's sheets. For each
event decide: intended effect, known issue, or new problem.

- **Blinks** (`blink hud`, `blink object`): something that appears or
  vanishes for one to four frames. Intended: the enemy hit flash (white,
  0.12 s), muzzle and bounce flashes (pale yellow), sparks, the pickup bob
  peeking at a screen edge. Not intended: HUD text, indicators or toasts
  that blink; scenery, tanks, smoke or decals missing for a frame; the
  objective arrow jumping sides; the clear colour showing through.
- **Shimmer** (`shimmer line`, info): thin lines (floor grid, wall tops)
  dropping in and out as the camera moves. Compare the count with the last
  review; a jump means a line got thinner or the camera noisier.
- **Transients, popping, flicker, partial, black, garbage**: large areas
  wrong for a frame: browser chrome over the game, a dropped static
  triangle, an unfinished frame.
- **Camera** (`oscillation`, `pumping`, `jerk`; `jump` is info): the view
  bobbing, pumping in and out, or jerking. Arena changes, respawns and the
  killcam cut on purpose.
- **Frozen** (info): pauses and forging freeze on purpose; a freeze in play
  is a stall.
- **References**: every changed scene's diff. Approve only intended changes
  (`scripts/check-treadline-references.py --update-references`).
- Compare the 111 MHz and normal-clock runs of a scenario when both exist:
  a problem only at 111 MHz is a frame-budget effect (deferred HUD, dropped
  optional geometry).
"""



def package_files(query):
    """The canonical package list (examples/treadline-arena/package-files.txt);
    a qualification URL's package also carries the files it marks."""
    qualification = "qualification=" in query
    files = []
    for line in (ROOT / "examples/treadline-arena/package-files.txt").read_text().splitlines():
        fields = line.split()
        if fields and not fields[0].startswith("#") and (
                len(fields) == 1 or (qualification and fields[1] == "qualification")):
            files.append(fields[0])
    return files

def run(command, log, env=None, timeout=900):
    with open(log, "w") as handle:
        return subprocess.run(command, stdout=handle, stderr=subprocess.STDOUT,
                              env=env, timeout=timeout).returncode


def overview(video, target, seconds_per_tile, fps):
    step = max(1, int(round(seconds_per_tile * fps)))
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", str(video), "-vf",
                    f"select='not(mod(n\\,{step}))',scale=240:136,tile=6x6:padding=2:color=white",
                    "-frames:v", "1", str(target)], check=True)


def review_one(args, name, clock, software, out):
    scenario, query = SCENARIOS[name]
    label = f"{name}-{clock}" + ("-sw" if software else "")
    work = out / "work"
    package = work / f"package-{label}"
    recording = work / f"recording-{label}"
    analysis = out / label
    for stale in (package, recording, analysis):
        if stale.exists():
            shutil.rmtree(stale)
    analysis.mkdir(parents=True)
    work.mkdir(exist_ok=True)
    started = time.time()
    status = run([str(args.fixture), "--web-app", str(package),
                  f"https://game.test/index.html?{query}", str(ROOT / "examples/treadline-arena"),
                  *package_files(query)], analysis / "package.log")
    if status:
        return {"label": label, "error": f"packaging failed ({analysis / 'package.log'})"}
    env = dict(os.environ, TILEFINCH_PPSSPP_CPU_MHZ=CLOCKS[clock],
               TILEFINCH_PPSSPP_SOFTWARE_RENDERER="1" if software else "0")
    # --measure: no golden comparison (Onslaught's forge time and real input
    # timing vary); the recording is what is reviewed.
    status = run([str(ROOT / "scripts/run-ppsspp-input-script.sh"), "--build-dir",
                  str(args.build_dir), "--script", scenario,
                  "--url", "https://tilefinch.local/offline/app?id=1",
                  "--offline-library", str(package), "--script-file-kb", "512",
                  "--measure", "--record-video", str(recording)],
                 analysis / "ppsspp.log", env=env)
    videos = sorted(recording.glob("VIDEO/*.avi"))
    if not videos:
        return {"label": label, "error": f"no recording (runner exit {status}; "
                                         f"{analysis / 'ppsspp.log'})"}
    recorded = time.time() - started
    status = run([sys.executable, str(ROOT / "scripts/analyze-game-video.py"), str(recording),
                  "--out", str(analysis), "--skip", "3", "--max-media", str(args.max_media),
                  "--motion-helper", str(args.tools / "tilefinch-video-motion"),
                  "--blink-helper", str(args.tools / "tilefinch-video-blink")],
                 analysis / "analyze.log", timeout=1800)
    if status:
        return {"label": label, "error": f"analysis failed ({analysis / 'analyze.log'})"}
    summary = json.loads((analysis / "events.json").read_text())
    overview(videos[-1], analysis / "overview.png", 2.0, summary["fps"])
    if (recording / "tilefinch-validation.txt").exists():
        shutil.copy(recording / "tilefinch-validation.txt", analysis / "tilefinch-validation.txt")
    if args.keep_video:
        shutil.copy(videos[-1], analysis / "recording.avi")
    shutil.rmtree(recording)
    shutil.rmtree(package)
    return {"label": label, "scenario": scenario, "query": query,
            "clock": clock + (" software renderer" if software else ""),
            "summary": summary, "record_seconds": round(recorded),
            "total_seconds": round(time.time() - started)}


def references(args, out):
    target = out / "references"
    command = [sys.executable, str(ROOT / "scripts/check-treadline-references.py"),
               "--out", str(target)]
    if args.lab:
        command += ["--lab", str(args.lab)]
    result = subprocess.run(command, capture_output=True, text=True, timeout=900)
    return {"status": result.returncode, "output": (result.stdout + result.stderr).strip(),
            "diffs": sorted(p.name for p in target.glob("*-diff.png")) if target.exists() else []}


def event_line(event):
    text = (f"{event['start']:.2f}-{event['end']:.2f} s, frames {event['first']}-{event['last']}: "
            f"{event['type']} {event['what']} (peak {event['peak']} at frame {event['peak_frame']})")
    if "box" in event:
        text += (f", box {','.join(map(str, event['box']))}, run {event['run']}, "
                 f"{'appears' if event['appears'] else 'vanishes'}")
    return text


def write_index(out, results, refs, args):
    md = ["# Treadline visual review", "",
          f"Generated {time.strftime('%Y-%m-%d %H:%M')} from {ROOT} "
          f"(EBOOT {args.build_dir}).", ""]
    page = ["<!doctype html><meta charset='utf-8'><title>Treadline visual review</title>",
            "<style>body{font:14px system-ui,sans-serif;margin:16px;max-width:1500px}"
            "img{max-width:100%;border:1px solid #888;margin:4px 0}"
            "li{margin:6px 0}.err{color:#b00}code{background:#eee;padding:0 3px}</style>",
            "<h1>Treadline visual review</h1>",
            f"<p>Generated {html.escape(time.strftime('%Y-%m-%d %H:%M'))}; EBOOT "
            f"<code>{html.escape(str(args.build_dir))}</code>.</p>"]
    md.append("| run | frames | fps | problems | events |")
    md.append("| --- | ---: | ---: | ---: | --- |")
    for result in results:
        if "error" in result:
            md.append(f"| {result['label']} | | | | ERROR: {result['error']} |")
            continue
        s = result["summary"]
        counts = ", ".join(f"{k} {v}" for k, v in sorted(s["counts"].items()))
        md.append(f"| [{result['label']}](#{result['label']}) | {s['frames']} | {s['fps']} | "
                  f"{s['problems']} | {counts} |")
    md.append("")
    if refs is not None:
        md += ["## Reference screenshots", "", "```", refs["output"], "```", ""]
        page.append("<h2>Reference screenshots</h2><pre>" + html.escape(refs["output"]) + "</pre>")
        for diff in refs["diffs"]:
            md.append(f"![{diff}](references/{diff})")
            page.append(f"<p>{html.escape(diff)}<br><img src='references/{diff}'></p>")
    for result in results:
        label = result["label"]
        md += [f"## {label}", ""]
        page.append(f"<h2 id='{label}'>{html.escape(label)}</h2>")
        if "error" in result:
            md += [f"ERROR: {result['error']}", ""]
            page.append(f"<p class='err'>{html.escape(result['error'])}</p>")
            continue
        s = result["summary"]
        md += [f"`{result['scenario']}` with `?{result['query']}` at clock {result['clock']}; "
               f"{s['frames']} frames at {s['fps']} fps; recorded in {result['record_seconds']} s.",
               "", f"[report]({label}/report.txt) | [events]({label}/events.json) | "
               f"[validation log]({label}/tilefinch-validation.txt)", "",
               f"![overview]({label}/overview.png)", ""]
        page.append(f"<p><code>{html.escape(result['scenario'])}</code> with "
                    f"<code>?{html.escape(result['query'])}</code>; {s['frames']} frames at "
                    f"{s['fps']} fps; {s['problems']} problems. <a href='{label}/report.txt'>"
                    f"report</a> | <a href='{label}/events.json'>events</a></p>"
                    f"<p>Overview (one frame every 2 s):<br><img src='{label}/overview.png'></p><ol>")
        for event in sorted(s["events"], key=lambda e: e["first"]):
            line = event_line(event)
            media = event.get("media")
            md.append(f"- {line}" + (f": [sheet]({label}/{media}.png), [clip]({label}/{media}.mp4)"
                                     + (f", [zoom]({label}/{media}-zoom.png)"
                                        if (out / label / f"{media}-zoom.png").exists() else "")
                                     if media else ""))
            item = f"<li>{html.escape(line)}"
            if media:
                item += (f" <a href='{label}/{media}.mp4'>clip</a><br>"
                         f"<img src='{label}/{media}.png' loading='lazy'>")
                if (out / label / f"{media}-zoom.png").exists():
                    item += f"<br><img src='{label}/{media}-zoom.png' loading='lazy'>"
            page.append(item + "</li>")
        page.append("</ol>")
        md.append("")
    md.append(CHECKLIST)
    page.append("<pre>" + html.escape(CHECKLIST) + "</pre>")
    (out / "index.md").write_text("\n".join(md) + "\n")
    (out / "index.html").write_text("\n".join(page) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-dir", type=Path,
                        default=ROOT / "build-preset-psp-validation",
                        help="validation PSP build with EBOOT.PBP (default: %(default)s)")
    parser.add_argument("--tools", type=Path, default=ROOT / "build-preset-release",
                        help="host build with tilefinch-offline-library-fixture and the video "
                             "helpers (default: %(default)s)")
    parser.add_argument("--lab", type=Path, help="interactive lab for the reference check")
    parser.add_argument("--out", type=Path, required=True, help="review folder (created)")
    parser.add_argument("--scenarios", default=",".join(STANDARD),
                        help=f"comma-separated, from: {', '.join(SCENARIOS)}")
    parser.add_argument("--clock", choices=["111", "normal", "both"], default="111")
    parser.add_argument("--software", action="store_true",
                        help="also record every run with PPSSPP's software renderer")
    parser.add_argument("--max-media", type=int, default=30)
    parser.add_argument("--keep-video", action="store_true",
                        help="keep each lossless recording (60-80 MB) in the folder")
    parser.add_argument("--no-references", action="store_true")
    parser.add_argument("--strict", action="store_true",
                        help="fail on analyzer problems or changed references")
    args = parser.parse_args()
    names = [n for n in args.scenarios.split(",") if n]
    unknown = [n for n in names if n not in SCENARIOS]
    if unknown:
        parser.error(f"unknown scenarios: {', '.join(unknown)}")
    args.build_dir = args.build_dir.resolve()
    args.tools = args.tools.resolve()
    args.fixture = args.tools / "tilefinch-offline-library-fixture"
    for needed in (args.fixture, args.tools / "tilefinch-video-motion",
                   args.tools / "tilefinch-video-blink", args.build_dir / "EBOOT.PBP"):
        if not needed.exists():
            parser.error(f"missing {needed}")
    if os.environ.get("TMPDIR", "").startswith(str(Path.home() / "Documents")):
        parser.error("TMPDIR is under ~/Documents; LaunchServices refuses the PPSSPP launch")
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    clocks = ["111", "normal"] if args.clock == "both" else [args.clock]
    results = []
    renderers = [False, True] if args.software else [False]
    for name in names:
        for clock, software in [(c, r) for c in clocks for r in renderers]:
            print(f"{name} at {clock}{' (software renderer)' if software else ''}...", flush=True)
            result = review_one(args, name, clock, software, out)
            results.append(result)
            print("  " + (result.get("error") or
                          f"{result['summary']['problems']} problems, "
                          f"{len(result['summary']['events'])} events, "
                          f"{result['total_seconds']} s"), flush=True)
    work = out / "work"
    if work.exists() and not any(work.iterdir()):
        work.rmdir()
    refs = None if args.no_references else references(args, out)
    write_index(out, results, refs, args)
    print(f"review: {out / 'index.html'} (and index.md)")
    failed = any("error" in r for r in results)
    if args.strict:
        failed = failed or any(r["summary"]["problems"] for r in results if "summary" in r)
        failed = failed or bool(refs and refs["status"] not in (0, 77))
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
