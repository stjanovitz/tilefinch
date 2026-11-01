#!/usr/bin/env python3
"""arena_fit.py RESULTS [CFG] : every bench clip's decoder-arena peak fits its arena.

The bench sizes each clip's arena with swdec_arena_estimate (max=, refs=) and
reports `arena=` and `peak=` on its summary line; exit 1 if any peak exceeds
its arena. The report must also be whole: every clip has its summary, the
bench reached its DONE marker, no clip reported an ERROR, and (given the
bench.cfg the run used) every `decode` line of it has its clip, in order. An
empty, truncated or unfinished report fails."""
import sys


def expected_clips(path):
    clips = []
    for line in open(path):
        words = line.split()
        if len(words) >= 2 and words[0] == 'decode':
            # The bench looks for "stages" anywhere after the clip name.
            rest = ' '.join(words[2:])
            clips.append((words[1], 1 if 'stages' in rest else 0))
    return clips


def main(argv):
    if len(argv) not in (2, 3):
        print(__doc__.strip(), file=sys.stderr)
        return 2
    bad = False
    seen = []
    clip = None
    summarized = True
    done = False
    for line in open(argv[1]):
        if done:
            continue
        if line.startswith('ERROR'):
            print(f"bench error: {line.strip()}")
            bad = True
        elif line.startswith('clip '):
            if not summarized:
                print(f"{clip}: no summary line")
                bad = True
            words = line.split()
            clip = words[1]
            stages = 1 if 'stages=1' in words[2:] else 0
            seen.append((clip, stages))
            summarized = False
        elif line.startswith('summary') and clip is not None:
            if summarized:
                print(f"{clip}: second summary line")
                bad = True
                continue
            summarized = True
            fields = dict(f.split('=', 1) for f in line.split()[1:] if '=' in f)
            arena = int(fields.get('arena', 0))
            peak = int(fields.get('peak', 0))
            fits = 0 < arena and peak <= arena
            print(f"{clip}: arena-peak={peak}/{arena} {'fits' if fits else 'OVER'}")
            bad |= not fits
        elif line.rstrip('\n') == 'DONE':
            done = True
    if not summarized:
        print(f"{clip}: no summary line (report truncated)")
        bad = True
    if not done:
        print("report incomplete: no DONE marker (the bench did not finish)")
        bad = True
    if not seen:
        print("report has no clips")
        bad = True
    if len(argv) == 3:
        want = expected_clips(argv[2])
        if seen != want:
            missing = [c for c, _ in want if c not in [s for s, _ in seen]]
            print(f"report clips {len(seen)} != configured {len(want)}"
                  f" (missing: {', '.join(missing) or 'none'})")
            bad = True
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
