#!/usr/bin/env python3
"""Run bounded, deterministic Treadline simulations without rendering/audio."""
import argparse
import collections
import json
from pathlib import Path
import statistics
import subprocess
import sys
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--matches', type=int, default=108,
                        help='1..1000 seeded matches, at most 60 simulated seconds each')
    parser.add_argument('--binary', type=Path, default=Path(__file__).resolve().parents[1]
                        / 'build-preset-release/tilefinch-canvas-webgl-conformance-tests')
    parser.add_argument('--output', type=Path, help='Optional JSONL results outside the source tree')
    args = parser.parse_args()
    if not 1 <= args.matches <= 1000:
        parser.error('--matches must be 1..1000')
    started = time.monotonic()
    run = subprocess.run([str(args.binary), '--treadline-league', str(args.matches)],
                         text=True, capture_output=True, timeout=300)
    if run.returncode:
        sys.stderr.write(run.stdout + run.stderr)
        return run.returncode
    rows = [json.loads(line.removeprefix('TREADLINE-LEAGUE '))
            for line in run.stdout.splitlines() if line.startswith('TREADLINE-LEAGUE ')]
    if len(rows) != args.matches:
        raise RuntimeError('Incomplete league result stream')
    if args.output:
        args.output.write_text(''.join(json.dumps(row, separators=(',', ':')) + '\n' for row in rows))
    groups = collections.defaultdict(list)
    for row in rows:
        groups[(row['script'], row['difficultyA'], row['difficultyB'])].append(row['result'])
    print(f'{len(rows)} matches in {time.monotonic() - started:.2f}s; '
          'fixed 30Hz steps, no rendering/audio. Unfinished matches are draws.')
    for (script, a, b), matches in sorted(groups.items()):
        shots = sum(t['shots'] for m in matches for t in m['tanks'])
        walls = sum(t['wastedWallShots'] for m in matches for t in m['tanks'])
        stuck = sum(t['stuckSeconds'] for m in matches for t in m['tanks'])
        actor_seconds = sum(m['seconds'] * 2 for m in matches)
        wins = sum(m['winner'] == 0 for m in matches)
        draws = sum(m['winner'] == -1 for m in matches)
        finished = [m['seconds'] for m in matches if m['end'] == 'kill']
        ttk = f'{statistics.median(finished):.1f}s' if finished else 'n/a'
        hits = sum(t['hits'] for m in matches for t in m['tanks'])
        objectives = sum(m['end'] == 'objective' for m in matches)
        print(f'script={script} A={a} B={b} n={len(matches)} '
              f'A-wins={wins}/{len(matches)} draws={draws} median-TTK={ttk} '
              f'objective-wins={objectives} hits={hits}/{shots} wall-waste={walls}/{shots} '
              f'stuck={100 * stuck / max(.01, actor_seconds):.1f}%')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
