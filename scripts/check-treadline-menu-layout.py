#!/usr/bin/env python3
"""Lay out every Treadline menu screen at 480x272 in the interactive lab.

Serves examples/treadline-arena on a loopback port (the lab refuses file://),
visits each screen through the campaign menu API (qualification.js's
__treadlineDebug.campaign, on a ?qualification=layout URL), and fails when the panel
leaves the screen or needs scrolling, when an element's box leaves the panel,
when a nowrap button clips its label, when the focused control is not
visible, or when the page listens for focus events (with a listener, every
D-pad focus move dispatches four focus events through script on the PSP).
--shots DIR also writes one PNG per screen.

Exit 0 pass, 1 layout failure, 77 when loopback serving is unavailable.
"""
import argparse
import json
from pathlib import Path
import re
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "benchmarks"))
sys.path.insert(0, str(ROOT / "scripts"))
from reference_frame import read_ppm, write_png_rgb  # noqa: E402
from treadline_lab import run_game_lab  # noqa: E402
# The harness surfaces of qualification.js, which a qualification URL
# (?qualification=layout below) loads; ordinary play has none.
C = "__treadlineDebug.campaign"
M = C + ".menu"
B = C + ".bridge()"
P = "__treadlineDebug.practice"
# rr=0 and last=0: the first campaign visit must not depend on the clock
# (campaign.js remark() writes a time-of-day line from 00:00 to 04:59 and a
# "days away" line after two days), as in check-treadline-references.py.
SAVE = (f"(()=>{{const s={C}.save;s.rr=0;s.last=0;s.d=1;for(let i=0;i<25;i++){{s.m[i]=i<5?15:i<12?7:i===12?1:0;"
        f"s.b[i]=i<13?12000+i*731:0;}}s.k=0x3ff;s.x=0x1fff;s.f=1|4;s.e=0;{C}.writeSave();return 1}})()")
SCREENS = [
    ("01-main", f"({M}.toMain(),1)"),
    ("02-difficulty", f"({C}.save.d=-1,{M}.show('difficulty'),1)"),
    ("03-campaign", f"({SAVE},{M}.toMain(),{M}.show('campaign'),1)"),
    ("04-campaign-glacier", f"({M}.theater=3,{M}.render(),1)"),
    ("05-briefing", f"({M}.theater=0,{M}.select(4),{M}.show('briefing'),{M}.skipRadio(),1)"),
    # Longest briefing (1-1) with the Reprise line: radio pages of two lines.
    ("05b-briefing-radio", f"({C}.save.e=1,{C}.save.rep=1,{C}.save.r=0,{C}.save.m[0]=0,{M}.toMain(),"
     f"{M}.select(0),{M}.show('briefing'),{M}.skipRadio(),1)"),
    ("05c-briefing-radio-page1", f"({M}.switchTab(-1),1)"),
    ("05d-briefing-long-objective", f"({C}.save.e=0,{C}.save.rep=0,{M}.toMain(),"
     f"{C}.save.m[0]=15,{M}.select(19),{M}.show('briefing'),{M}.skipRadio(),1)"),
    ("06-faults", f"({M}.toMain(),{M}.show('campaign'),{M}.show('faults'),1)"),
    ("07-faults-2", f"({M}.switchTab(1),1)"),
    ("08-range-logs", f"({M}.toMain(),{M}.show('campaign'),{M}.show('logs'),1)"),
    ("08b-range-logs-2", f"({M}.switchTab(1),1)"),
    ("08c-range-logs-3", f"({M}.switchTab(1),1)"),
    ("08d-range-logs-4", f"({M}.switchTab(1),1)"),
    ("08e-range-logs-5", f"({M}.switchTab(1),1)"),
    ("09-quick-match", f"({M}.toMain(),{M}.show('quick'),1)"),
    ("10-multiplayer", f"({M}.toMain(),{M}.show('multiplayer'),1)"),
    ("11-code-entry", "(document.getElementById('online-code').click(),1)"),
    ("12-replays", "(document.getElementById('code-cancel').click(),"
     f"{M}.toMain(),{M}.show('replays'),1)"),
    ("13-garage", f"({M}.toMain(),{M}.show('garage'),1)"),
    ("14-settings-controls", f"({M}.toMain(),{M}.show('settings'),1)"),
    ("14b-controls-arcade-driving", f"({M}.show('controls'),1)"),
    ("14c-controls-arcade-shooting", f"({M}.switchTab(1),1)"),
    # Page 3: the aim guide choice and one row per level.
    ("14c2-controls-aim-guide", f"({M}.switchTab(1),1)"),
    ("14d-controls-classic-driving", f"({M}.cycleScheme(1),{M}.switchTab(1),1)"),
    ("14e-controls-classic-shooting", f"({M}.switchTab(1),1)"),
    ("14f-controls-gunner-driving", f"({M}.cycleScheme(1),{M}.switchTab(-1),1)"),
    ("14g-controls-gunner-shooting", f"({M}.switchTab(1),1)"),
    ("14h-settings-back", f"({M}.cycleScheme(1),{M}.back(),1)"),
    ("15-settings-display", f"({M}.switchTab(1),1)"),
    ("16-settings-audio", f"({M}.switchTab(1),1)"),
    ("17-pause", f"({M}.toMain(),{C}.setRadio(0),{C}.startMission(4,false),{B}.setMode('paused'),1)"),
    ("17b-pause-gunner", f"({B}.preferences.controls=2,{B}.refreshSetupLabels(),"
     f"{B}.updateControlHint(),{M}.render(),1)"),
    ("17c-pause-controls-gunner", f"({M}.show('controls'),1)"),
    ("17d-pause-controls-gunner-shooting", f"({M}.switchTab(1),1)"),
    ("17d2-pause-controls-aim-guide", f"({M}.switchTab(1),1)"),
    ("17e-pause-back", f"({M}.back(),1)"),
    ("18-results-mission", f"({B}.preferences.controls=0,{B}.updateControlHint(),"
     f"{B}.setMode('playing'),{B}.setMode('victory'),1)"),
    ("19-results-quick", f"({M}.toMain(),{B}.state.gameMode=0,{B}.startOrResume(),"
     f"{B}.setMode('game-over'),1)"),
    ("19b-replay-done", f"({M}.toMain(),{B}.setMode('replay-done'),1)"),
    ("20-ending", f"({M}.toMain(),{C}.save.e=1,{C}.startMission(24,false),"
     f"{B}.setMode('victory'),1)"),
    # Practice Range: drill list (longest note focused), the in-run Range
    # panel, its Controls link and drill list, and a drill result.
    ("21-practice", f"({C}.save.e=0,{M}.toMain(),{M}.show('practice'),1)"),
    ("21b-practice-note", f"({B}.preferences.controls=2,{M}.render(),"
     "document.querySelectorAll('#menu button')[3].focus(),1)"),
    ("22-range-panel", f"({B}.preferences.controls=0,{P}.enter(0),{B}.setMode('paused'),1)"),
    ("22b-range-controls", f"({M}.show('controls'),1)"),
    ("22b2-range-controls-aim-guide", f"({M}.switchTab(-1),1)"),
    ("22c-range-drills", f"({M}.back(),{M}.show('practice'),1)"),
    ("23-drill-result", f"({M}.back(),{P}.begin(1),{B}.setMode('playing'),{P}.runtime.gate=7,"
     f"{B}.pickups[0].active=false,1)"),
    ("24-leave-range", f"({M}.toMain(),1)"),
]
# The lab's scrollWidth does not report clipped inline text and canvas
# metrics resolve a different face, so PROBE lays every visible button label
# out in a hidden, absolutely positioned nowrap span appended to the element
# that owns the text; the span inherits that element's font (computed
# fontFamily reports only the generic family, so a copied family would
# measure Arial text in DejaVu Sans). Nodes are laid out at the next tick,
# then MEASURE compares widths and removes the spans before any other check.
PROBE = (
    "(()=>{const P=document.getElementById('panel');"
    "const add=(e,x)=>{const g=document.createElement('span');g.className='layout-probe';"
    "g.style.cssText='position:absolute;left:0;top:0;visibility:hidden;display:block;"
    "white-space:nowrap;padding:0;margin:0;border:0';"
    "g.textContent=x;e.appendChild(g);return g};"
    "globalThis.__probes=[];for(const e of P.querySelectorAll('button')){"
    "const r=e.getBoundingClientRect();if(!r.width||!r.height)continue;let own='';const parts=[];"
    "for(const n of e.childNodes){if(n.nodeType===3||getComputedStyle(n).display!=='block')"
    "own+=n.textContent;else parts.push([n,n.textContent]);}"
    "parts.push([e,own.trim()]);__probes.push([e,parts.map(([n,x])=>add(n,x))]);}"
    "return 1})()")
# A label may need one pixel more than its shrink-to-fit button reports: the
# probe and the button round a fractional text width (Arial digits at 11 px)
# to different whole pixels. Boxes are compared with half-pixel slack. The
# lab reports the panel's box without its own translate(-50%,-50%)
# (descendants include it), so the panel box is re-centred here. Hidden
# descendants report 0x1 boxes.
MEASURE = (
    "(()=>{const P=document.getElementById('panel'),q=P.getBoundingClientRect(),bad=[];"
    "const p={left:q.left-q.width/2,top:q.top-q.height/2,"
    "right:q.left+q.width/2,bottom:q.top+q.height/2};"
    "const name=e=>(e.id||e.tagName)+':'+String(e.textContent).trim().slice(0,18);"
    "if(p.left<0||p.top<0||p.right>480.5||p.bottom>272.5)bad.push('panel-offscreen');"
    "for(const [e,parts] of __probes){const c=getComputedStyle(e);"
    "const room=e.getBoundingClientRect().width-parseFloat(c.paddingLeft)-parseFloat(c.paddingRight)-2;"
    "let need=0;for(const g of parts)need=Math.max(need,g.getBoundingClientRect().width);"
    "for(const g of parts)g.remove();const label=name(e);"
    "if(!need&&e.textContent.trim())bad.push('unmeasured '+label);"
    "else if(need>room+1)bad.push('clipped '+label+' +'+(need-room).toFixed(1));}"
    "if(P.scrollHeight>P.clientHeight+1)bad.push('panel-scroll:'+P.scrollHeight+'/'+P.clientHeight);"
    "for(const e of P.querySelectorAll('button,h1,p,span,output,small,strong,b,textarea')){"
    "const r=e.getBoundingClientRect();if(!r.width||!r.height)continue;"
    "if(r.left<p.left-.5||r.right>p.right+.5||r.top<p.top-.5||r.bottom>p.bottom+.5)"
    "bad.push('outside '+name(e));}"
    "const f=document.activeElement;if(f&&f!==document.body&&P.contains(f)){"
    "const r=f.getBoundingClientRect();if(!r.width||r.left<0||r.top<0||r.right>480||r.bottom>272)"
    "bad.push('focus-hidden '+name(f));}"
    "return JSON.stringify([P.getAttribute('data-screen'),bad.slice(0,6)])})()")


def ppm_to_png(source, target):
    write_png_rgb(target, *read_ppm(source))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lab", type=Path,
                        default=ROOT / "build-preset-release/psp-browser-interactive-lab")
    parser.add_argument("--shots", type=Path, help="write one PNG per screen here")
    args = parser.parse_args()
    # Scratch lives in the lab's build directory, never the source tree.
    with tempfile.TemporaryDirectory(dir=args.lab.resolve().parent) as work:
        work = Path(work)
        lines = ["tick 30 16"]
        for name, action in SCREENS:
            # 12 ticks let a focus note settle (it waits 0.15 s).
            lines += [f"js {action}", "tick 12 16", f"js {PROBE}", "tick 1 16",
                      f"js {MEASURE}"]
            if args.shots:
                lines.append(f"render {work / (name + '.ppm')}")
        lines.append("js String(globalThis.__tilefinchFocusEventsObserved())")
        (work / "menus.commands").write_text("\n".join(lines) + "\n")
        run = run_game_lab(args.lab, work / "menus.commands", work / "final.ppm",
                           query="?qualification=layout")
        if run is None:
            return 77
        values = re.findall(r'^loop-js ok=(\w+) value="(.*)" error="(.*)"$',
                            run.stdout, re.M)
        if run.returncode or len(values) != 3 * len(SCREENS) + 1:
            sys.stderr.write(run.stdout[-4000:] + run.stderr[-2000:])
            print(f"FAIL: lab exit {run.returncode}, {len(values)} probe results")
            return 1
        failures = 0
        for (name, _), (_, measured, error) in zip(SCREENS, values[2::3]):
            screen, bad = json.loads(measured.replace('\\"', '"')) if measured else ("?", [error])
            print(f"{name:26} {screen:12} {'ok' if not bad else '; '.join(bad)}")
            failures += bool(bad)
            if args.shots:
                args.shots.mkdir(parents=True, exist_ok=True)
                ppm_to_png(work / (name + ".ppm"), args.shots / (name + ".png"))
        print(f"{len(SCREENS) - failures}/{len(SCREENS)} screens fit")
        observed = values[-1][1]
        if observed != "false":
            print(f"FAIL: focus-event listeners present (observed={observed})")
            failures += 1
        return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
