#!/usr/bin/env python3
"""Render every fidelity scenario with the engine and score it against
its Chrome 480x272 reference frames.

For each manifest row with captured references (top.png / bottom.png
from capture-reference.js), this renders the same hermetic trace with
psp-browser-lab --replay-http --scroll-all, picks the first and last
scroll frames, and scores candidate-vs-reference with
compare-reference-frame.py (luma SSIM, multi-scale SSIM, edge F1, MAE).

Output: a TSV scoreboard (scenario, checkpoint, metric columns) on
stdout and optionally --output FILE.  Compare against the committed
tests/fidelity-baselines.tsv to see movement.

Usage:
    run-fidelity-scoreboard.py --manifest FILE --trace-root DIR \
        --reference-root DIR --work-dir DIR [--lab BIN] [--output FILE]
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
COMPARE = ROOT / "benchmarks" / "compare-reference-frame.py"
sys.path.insert(0, str(ROOT / "benchmarks"))

from visual_scenario import trace_replay_origin_ms

METRICS = ["luma_ssim", "luma_ms_ssim", "edge_f1", "mae_rgb565",
           "pixel_mismatch_rgb565_pct", "foreground_delta_pct"]

# capture-reference.js settles each checkpoint for at most this many
# milliseconds before it screenshots; the lab's drain gets the same budget.
SETTLE_MS = 750


def scenarios(manifest: Path):
    lines = manifest.read_text(encoding="utf-8").splitlines()
    header = lines[0].split("\t")
    for line in lines[1:]:
        if not line:
            continue
        yield dict(zip(header, line.split("\t")))


def checkpoints_for(row: dict) -> list[tuple[str, str, str | None]]:
    """Parse the manifest checkpoints column into (name, kind, target)
    triples, mirroring capture-reference.js parseCheckpoints so candidate
    frame names line up with the captured reference filenames."""
    value = row.get("checkpoints", "top|-|bottom") or "top|-|bottom"
    items = value.split("|")
    if items == ["top", "-", "bottom"]:
        return [("top", "top", None), ("bottom", "bottom", None)]
    if len(items) < 3 or items[0] != "top" or items[-1] != "bottom":
        raise RuntimeError(f"invalid checkpoints column: {value}")
    parsed: list[tuple[str, str, str | None]] = [("top", "top", None)]
    for index, item in enumerate(items[1:-1], start=1):
        kind, _, target = item.partition(":")
        if kind not in ("anchor", "selector", "text") or not target:
            raise RuntimeError(f"invalid checkpoint {item}")
        parsed.append((f"{kind}-{index}", kind, target))
    parsed.append(("bottom", "bottom", None))
    return parsed


def checkpoint_commands(kind: str, target: str | None,
                        hydration_selector: str | None = None) -> str:
    # Scripted references wait for their hydration predicate while allowing
    # the browser to drain ready work without advancing the replay clock.
    # The lab command protocol has no predicate-wait primitive, so use a
    # bounded zero-time settle only for scenarios that declare hydration.
    # This preserves Date/timer observations while preventing an exact-clock
    # replay from capturing the pre-hydration shell.
    settle = "tick 1000 0\n" if hydration_selector else ""
    if kind == "top" and hydration_selector:
        # The hydration gate rides the top-checkpoint run: the page must
        # produce at least one match for the selector, or the scenario
        # fails outright instead of quietly scoring its unhydrated shell.
        return (settle +
                f"js document.querySelectorAll({json.dumps(hydration_selector)}).length\n"
                "status\nquit\n")
    if kind == "top":
        return "status\nquit\n"
    if kind == "bottom":
        # Position after the configured replay clock and all initial resource
        # work have completed.  --scroll-bottom runs before that work, so a
        # late relayout can leave the captured frame above the real bottom.
        return settle + "bottom\nstatus\nquit\n"
    # Positioned checkpoints bracket the scroll with the lab's drain, which
    # is the engine-side counterpart of the two driveUntilSettled boundaries
    # capture-reference.js puts around settleCheckpoint: the leading drain
    # resolves the target against a settled layout instead of the page as it
    # stood 128ms in, and the trailing one confirms that nothing moved under
    # the frozen scroll offset.  It replaces both the hydration settle (which
    # it subsumes) and the old `tick 40`, whose 640ms of replay clock the
    # reference never spends and which relaid the page out *after* the scroll
    # had been fixed.  The settle budget matches capture-reference.js's
    # --settle-ms default.
    drain = f"drain 2048 {SETTLE_MS}\n"
    if kind == "anchor":
        return drain + f"anchor {target}\n" + drain + "status\nquit\n"
    # selector/text mirror capture-reference.js checkpointPosition: put the
    # matched element's top at the top of the viewport, in the engine's own
    # layout geometry.
    find = (f"document.querySelector({json.dumps(target)})" if kind == "selector"
            else "(()=>{const w=document.createTreeWalker(document.body,1);"
                 "for(let n=w.nextNode();n;n=w.nextNode())"
                 f"{{if(n.children.length===0&&n.textContent.includes({json.dumps(target)}))return n;}}"
                 "return null;})()")
    return (drain + "js (()=>{const e=" + find + ";if(!e)return 'missing';"
            "const y=Math.max(0,Math.floor(e.getBoundingClientRect().top+scrollY));"
            "scrollTo(0,y);return y;})()\n"
            + drain + "status\nquit\n")


def positioned_checkpoint_found(output: str) -> bool:
    """A successful evaluation is not enough: the bounded finder reports
    the literal value `missing` when the requested DOM target is absent."""
    value = re.search(r'loop-js ok=yes value="([^"]*)"', output)
    return value is not None and value.group(1) != "missing"


def script_render_command(interactive: Path, row: dict, trace: Path,
                          commands: Path, frame_dir: Path) -> list[str]:
    """Build a script-enabled fidelity command with the reference clock.

    The generous resource ceilings are intentional: this scoreboard measures
    the engine's best fidelity rather than PSP-shaped admission policy.  The
    observable replay clock is not a resource ceiling, however, and must match
    the captured reference exactly.
    """
    return [
        str(interactive), "--url", row["url"],
        "--replay-http-response-keyed", str(trace),
        "--deterministic-replay-seed", str(trace_replay_origin_ms(trace)),
        "--fetch-scripts", "--script-heap-mb", "192",
        "--script-total-mb", "48", "--script-file-kb", "8192",
        "--script-count", "256", "--script-timeout-ms", "60000",
        "--limit-mb", "256", "--ticks", row.get("ticks", "0"),
        "--tick-ms", row.get("tick_ms", "16"),
        "--image-count", "160", "--image-total-kb", "16384",
        "--image-file-kb", "1024", "--image-decoded-mb", "32",
        "--font-attempts", "8", "--font-total-kb", "4096",
        "--font-file-kb", "1024", "--font-backend-kb", "4096",
        "--resource-timeout-ms", "600000",
        "--session-cache-kb", "16384",
        "--viewport-css-width", row.get("css_width", "480"),
        "--viewport-css-height", row.get("css_height", "272"),
        *[flag for host in row.get("blocked_origins", "-").split("|")
          if host and host != "-"
          for flag in ("--block-origin", host)],
        "--commands", str(commands),
        "--loop-output-dir", str(frame_dir),
    ]


def render_with_scripts(lab: str, row: dict, trace: Path,
                        out_dir: Path) -> dict[str, Path]:
    """Script-enabled scenarios render through the interactive lab: the
    page's JavaScript runs against the hermetic trace with a fixed
    deterministic seed, and each checkpoint is one command-loop frame."""
    interactive = Path(lab).parent / "psp-browser-interactive-lab"
    out_dir.mkdir(parents=True, exist_ok=True)
    hydration = row.get("hydration_selector", "-")
    hydration = hydration if hydration and hydration != "-" else None
    frames: dict[str, Path] = {}
    for checkpoint, kind, target in checkpoints_for(row):
        commands = out_dir / f"commands-{checkpoint}.txt"
        commands.write_text(checkpoint_commands(kind, target, hydration),
                            encoding="utf-8")
        frame_dir = out_dir / f"frames-{checkpoint}"
        for stale in frame_dir.glob("frame-*.ppm") if frame_dir.exists() else []:
            stale.unlink()
        # Response-keyed replay ranks successful records above captured
        # failures, so a URL whose first capture attempt failed still
        # renders when a later record holds its body.
        completed = subprocess.run(
            script_render_command(
                interactive, row, trace, commands, frame_dir),
            capture_output=True, text=True)
        rendered = sorted(frame_dir.glob("frame-*.ppm")) if frame_dir.exists() else []
        if completed.returncode != 0 or not rendered:
            raise RuntimeError(
                f"interactive render failed for {row['scenario']}/"
                f"{checkpoint}: {completed.stderr.strip()[-300:]}")
        # The drain reports whether it reached a quiet engine or ran out of
        # turns; a truncated drain means the frame below is not the settled
        # page the reference is, so fail instead of scoring it silently.
        truncated = [line for line in completed.stdout.splitlines()
                     if line.startswith("loop-drain drained=no")]
        if truncated:
            raise RuntimeError(
                f"{row['scenario']}/{checkpoint}: drain did not settle: "
                + truncated[0])
        if kind in ("selector", "text"):
            if not positioned_checkpoint_found(completed.stdout):
                raise RuntimeError(
                    f"{row['scenario']}/{checkpoint}: scroll target not found")
        if kind == "top" and hydration is not None:
            match = re.search(r'loop-js ok=yes value="(\d+)"',
                              completed.stdout)
            if match is None or int(match.group(1)) == 0:
                raise RuntimeError(
                    f"{row['scenario']}: hydration selector "
                    f"{hydration!r} matched nothing -- page did not hydrate")
        frames[checkpoint] = rendered[-1]
    return frames


def render(lab: str, row: dict, trace: Path, out_dir: Path) -> dict[str, Path]:
    if row.get("engine_scripts", "-") == "1":
        return render_with_scripts(lab, row, trace, out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    for stale in out_dir.glob("frame_*.ppm"):
        stale.unlink()
    completed = subprocess.run(
        [lab, "--url", row["url"], "--replay-http", str(trace),
         "--output-dir", str(out_dir), "--scroll-all",
         "--fetch-css", "--fetch-images",
         "--max-download-kb", row.get("max_download_kb", "4096"),
         "--limit-mb", row.get("limit_mb", "24"),
         "--max-images", "48", "--inline-scripts-only",
         "--viewport-css-width", row.get("css_width", "480"),
         "--viewport-css-height", row.get("css_height", "272")],
        capture_output=True, text=True)
    if completed.returncode != 0:
        raise RuntimeError(
            f"lab render failed for {row['scenario']}: "
            f"{completed.stderr.strip()[-300:]}")
    frames = sorted(out_dir.glob("frame_*.ppm"))
    if not frames:
        raise RuntimeError(f"no frames rendered for {row['scenario']}")
    return {"top": frames[0], "bottom": frames[-1]}


def score(candidate: Path, reference: Path) -> dict[str, str]:
    completed = subprocess.run(
        [sys.executable, str(COMPARE), str(candidate), str(reference),
         "--output-format", "json"],
        capture_output=True, text=True)
    if completed.returncode not in (0, 1):
        raise RuntimeError(f"comparator failed: {completed.stderr[-300:]}")
    data = json.loads(completed.stdout).get("metrics", {})
    return {name: f"{data[name]:.4f}" if isinstance(data.get(name), float)
            else str(data.get(name, "-")) for name in METRICS}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--trace-root", required=True, type=Path)
    parser.add_argument("--reference-root", required=True, type=Path)
    parser.add_argument("--work-dir", required=True, type=Path)
    parser.add_argument("--lab", default="build-preset-release/psp-browser-lab")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--check-floors", type=Path,
                        help="baseline TSV; regress past tolerance -> exit 2")
    parser.add_argument("--floor-tolerance", type=float, default=0.02)
    parser.add_argument(
        "--scenario", action="append", default=[],
        help="render only this scenario (repeatable; default: all)")
    args = parser.parse_args()

    if not args.trace_root.exists():
        print("fidelity corpus missing; skipping (see docs/FIDELITY.md)",
              file=sys.stderr)
        return 77

    rows = ["scenario\tcheckpoint\t" + "\t".join(METRICS)]
    failures = 0
    for row in scenarios(args.manifest):
        name = row["scenario"]
        if args.scenario and name not in args.scenario:
            continue
        reference_dir = args.reference_root / name
        if not (reference_dir / "reference-state.json").exists():
            print(f"# {name}: no eligible reference captured; skipped",
                  file=sys.stderr)
            continue
        try:
            frames = render(args.lab, row,
                            args.trace_root / row["replay_dir"],
                            args.work_dir / name)
            for checkpoint in frames:
                reference = reference_dir / f"{checkpoint}.png"
                if not reference.exists():
                    print(f"# {name}/{checkpoint}: no reference captured; "
                          "skipped", file=sys.stderr)
                    continue
                metrics = score(frames[checkpoint], reference)
                rows.append(name + "\t" + checkpoint + "\t"
                            + "\t".join(metrics[m] for m in METRICS))
        except RuntimeError as error:
            print(f"# {name}: {error}", file=sys.stderr)
            failures += 1
    report = "\n".join(rows) + "\n"
    sys.stdout.write(report)
    if args.output:
        args.output.write_text(report, encoding="utf-8")
    if args.check_floors:
        higher_better = {"luma_ssim", "luma_ms_ssim", "edge_f1"}
        baseline = {}
        for line in args.check_floors.read_text().splitlines()[1:]:
            parts = line.split("\t")
            if len(parts) < 2 + len(METRICS):
                continue
            baseline[(parts[0], parts[1])] = dict(zip(METRICS, parts[2:]))
        regressions = []
        for line in rows[1:]:
            parts = line.split("\t")
            key = (parts[0], parts[1])
            expected = baseline.get(key)
            if expected is None:
                continue
            got = dict(zip(METRICS, parts[2:]))
            for name in ("luma_ssim", "luma_ms_ssim", "edge_f1"):
                floor = float(expected[name]) - args.floor_tolerance
                if float(got[name]) < floor:
                    regressions.append(
                        f"{key[0]}/{key[1]} {name}: {got[name]} < floor "
                        f"{floor:.4f} (baseline {expected[name]})")
        if regressions:
            print("fidelity floor regressions:", file=sys.stderr)
            for r in regressions:
                print("  " + r, file=sys.stderr)
            return 2
        print(f"fidelity floors hold ({len(rows) - 1} rows)",
              file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
