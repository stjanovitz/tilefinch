#!/usr/bin/env python3
"""Compare Treadline Arena's key scenes with approved reference screenshots.

Renders each scene at 480x272 in the interactive lab (deterministic: fixed
16 ms ticks, a pinned save, remarks off and a fixed Daily date, so frames are
byte-identical from run to run) and compares it pixel for pixel with
tests/visual/treadline/<scene>.png. The scenes are the title, every main
menu screen, the Controls pages, the Practice Range panel, the pause menu,
the HUD in play and the opening view of each authored arena. Menu scenes are
reached through scripts/check-treadline-menu-layout.py's SCREENS sequence
(the same visits the layout gate makes), so the two stay in step.

On a mismatch the run fails and writes, per changed scene, the current
render and a diff image (changed pixels in magenta over a dimmed reference,
with the reference and the render side by side) into --out, and prints how
to approve the change. Approving is a deliberate act for an intended visual
change only: look at the diffs, then

    python3 scripts/check-treadline-references.py --update-references

rewrites the references from the current render (commit them with the
change that caused them). --only NAME[,NAME] limits a run to some scenes.

Exit 0 match, 1 mismatch or failure, 77 when loopback serving is unavailable.
"""
import argparse
import importlib.util
from pathlib import Path
import re
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
REFERENCES = ROOT / "tests/visual/treadline"
sys.path.insert(0, str(ROOT / "benchmarks"))
sys.path.insert(0, str(ROOT / "scripts"))
from reference_frame import read_png, read_ppm, write_png_rgb  # noqa: E402
from treadline_lab import run_game_lab  # noqa: E402

_spec = importlib.util.spec_from_file_location(
    "treadline_menu_layout", ROOT / "scripts/check-treadline-menu-layout.py")
layout = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(layout)
C, M, B, P = layout.C, layout.M, layout.B, layout.P

# Menu screens kept as references (names from the layout gate's SCREENS).
MENU_REFERENCES = [
    "01-main", "02-difficulty", "03-campaign", "05-briefing", "06-faults",
    "08-range-logs", "09-quick-match", "10-multiplayer", "12-replays", "13-garage",
    "14-settings-controls", "14b-controls-arcade-driving", "14c-controls-arcade-shooting",
    "14c2-controls-aim-guide", "14d-controls-classic-driving", "14f-controls-gunner-driving",
    "15-settings-display", "16-settings-audio", "17-pause", "21-practice",
    "22-range-panel",
]
# Play scenes after the menu sequence: (name, action, ticks before render).
# Each authored arena is where its Quick Match mode starts (Survival on
# arena 1, Team Control on 2, Convoy Escort on 3). The "first-frame" scenes
# render after two ticks: the first tick runs the arena change, so the
# second is the first frame a player sees in that arena. Then the HUD after
# two seconds of Survival.
QUICK = f"{B}.setMode('title'),{B}.startOrResume()"
PLAY = [
    ("30-arena-1-first-frame", f"({M}.toMain(),{B}.state.gameMode=0,{QUICK},1)", 2),
    ("31-arena-2-first-frame", f"({M}.toMain(),{B}.state.gameMode=1,{QUICK},1)", 2),
    ("32-arena-3-first-frame", f"({M}.toMain(),{B}.state.gameMode=2,{QUICK},1)", 2),
    ("33-hud-in-play", f"({M}.toMain(),{B}.state.gameMode=0,{QUICK},1)", 60),
]
# Before the first scene: no time-of-day remarks, no stale-save remark.
PIN = f"({C}.save.rr=0,{C}.save.last=0,{C}.writeSave(),1)"


def scenes():
    """(name, action, ticks, keep) in visiting order."""
    out = [("00-pin", PIN, 1, False)]
    for name, action in layout.SCREENS:
        out.append((name, action, 12, name in MENU_REFERENCES))
    out.extend((name, action, ticks, True) for name, action, ticks in PLAY)
    return out


def write_png(path, width, height, pixels):
    """RGB PNG with a per-row filter choice (the canvas is upscaled 1.5x, so
    Up often zeroes whole rows); smaller than unfiltered references."""
    write_png_rgb(path, width, height, pixels, filtered=True)


def diff_image(path, width, height, expected, actual):
    """Reference, render and a diff (changed pixels magenta over a dimmed
    reference) side by side."""
    diff = bytearray(len(actual))
    for i in range(0, len(actual), 3):
        if actual[i:i + 3] != expected[i:i + 3]:
            diff[i:i + 3] = b"\xff\x00\xff"
        else:
            diff[i:i + 3] = bytes(v // 3 for v in expected[i:i + 3])
    gap = b"\xff\xff\xff" * 4
    rows = bytearray()
    stride = width * 3
    for y in range(height):
        part = slice(y * stride, (y + 1) * stride)
        rows += expected[part] + gap + actual[part] + gap + diff[part]
    write_png(path, width * 3 + 8, height, bytes(rows))


def render(lab, chosen, work):
    lines = ["tick 30 16"]
    for name, action, ticks, keep in scenes():
        lines += [f"js {action}", f"tick {ticks} 16"]
        if keep and name in chosen:
            lines.append(f"render {work / (name + '.ppm')}")
    (work / "references.commands").write_text("\n".join(lines) + "\n")
    run = run_game_lab(lab, work / "references.commands", work / "final.ppm",
                       query="?qualification=references&daily=20261001")
    if run is None:
        return None
    (work / "lab.log").write_text(run.stdout + run.stderr)
    failed = re.findall(r'^loop-js ok=false .*$', run.stdout, re.M)
    if run.returncode or failed:
        sys.stderr.write("\n".join(failed[:5]) + "\n" + run.stderr[-2000:])
        print(f"FAIL: lab exit {run.returncode}, {len(failed)} failed actions "
              f"(log: {work / 'lab.log'})")
        return False
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--lab", type=Path,
                        default=ROOT / "build-preset-release/psp-browser-interactive-lab")
    parser.add_argument("--out", type=Path,
                        default=ROOT / "build-preset-release/treadline-references",
                        help="current renders and diff images (default: %(default)s)")
    parser.add_argument("--update-references", action="store_true",
                        help="approve: rewrite tests/visual/treadline from this render")
    parser.add_argument("--only", help="comma-separated scene names")
    parser.add_argument("--list", action="store_true", help="list the scenes and exit")
    args = parser.parse_args()
    kept = [name for name, _, _, keep in scenes() if keep]
    if args.list:
        print("\n".join(kept))
        return 0
    chosen = set(kept)
    if args.only:
        chosen = set(args.only.split(","))
        unknown = chosen - set(kept)
        if unknown:
            print(f"unknown scenes: {', '.join(sorted(unknown))}")
            return 1
    args.out.mkdir(parents=True, exist_ok=True)
    for stale in list(args.out.glob("*.png")) + list(args.out.glob("*.ppm")):
        stale.unlink()
    with tempfile.TemporaryDirectory(dir=args.out) as work:
        work = Path(work)
        ok = render(args.lab, chosen, work)
        if ok is None:
            return 77
        if not ok:
            return 1
        changed = []
        for name in kept:
            if name not in chosen:
                continue
            capture = work / (name + ".ppm")
            if not capture.exists():
                print(f"FAIL: {name}: no render")
                return 1
            width, height, pixels = read_ppm(capture)
            reference = REFERENCES / (name + ".png")
            if args.update_references:
                REFERENCES.mkdir(parents=True, exist_ok=True)
                write_png(reference, width, height, pixels)
                continue
            current = args.out / (name + ".png")
            if not reference.exists():
                write_png(current, width, height, pixels)
                changed.append(f"{name}: no reference yet ({current})")
                continue
            ref_width, ref_height, expected = read_png(reference)
            if (ref_width, ref_height) != (width, height):
                write_png(current, width, height, pixels)
                changed.append(f"{name}: size {width}x{height}, reference {ref_width}x{ref_height}")
                continue
            count = sum(pixels[i:i + 3] != expected[i:i + 3] for i in range(0, len(pixels), 3))
            if count:
                write_png(current, width, height, pixels)
                diff = args.out / (name + "-diff.png")
                diff_image(diff, width, height, expected, pixels)
                changed.append(f"{name}: {count} changed pixels; diff {diff}")
    if args.update_references:
        print(f"approved {len(chosen)} references in {REFERENCES.relative_to(ROOT)}; "
              "review them and commit with the change that caused them")
        return 0
    if changed:
        print("Treadline reference screenshots changed:")
        print("\n".join("  " + line for line in changed))
        print("If (and only if) the change is intended, look at the diffs, then approve with:\n"
              "  python3 scripts/check-treadline-references.py --update-references"
              + (f" --only {args.only}" if args.only else ""))
        return 1
    print(f"{len(chosen)} Treadline scenes match their references")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
