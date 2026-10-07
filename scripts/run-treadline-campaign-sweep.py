#!/usr/bin/env python3
"""Sweep every Treadline campaign mission headlessly at each difficulty.

A bot-league proxy (the existing bot AI at Cadet, Veteran or Ace skill) plays the
player tank. Results measure combat difficulty only: the proxy hunts and does
not escort, collect cards, demolish barriers or bank shots on purpose, so
objective-only missions mostly time out. Compare columns, not absolutes.
"""
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
    parser.add_argument('--seeds', type=int, default=3, help='1..20 seeds per cell')
    parser.add_argument('--binary', type=Path, default=Path(__file__).resolve().parents[1]
                        / 'build-preset-release/tilefinch-canvas-webgl-conformance-tests')
    parser.add_argument('--output', type=Path, help='Optional JSONL results outside the source tree')
    args = parser.parse_args()
    if not 1 <= args.seeds <= 20:
        parser.error('--seeds must be 1..20')
    started = time.monotonic()
    run = subprocess.run([str(args.binary), '--treadline-campaign-sweep', str(args.seeds)],
                         text=True, capture_output=True, timeout=3600)
    if run.returncode:
        sys.stderr.write(run.stdout[-4000:] + run.stderr[-4000:])
        return run.returncode
    rows = [json.loads(line.removeprefix('TREADLINE-CAMPAIGN-SWEEP '))
            for line in run.stdout.splitlines() if line.startswith('TREADLINE-CAMPAIGN-SWEEP ')]
    if len(rows) != 27 * 3 * 3 * args.seeds:
        raise RuntimeError('Incomplete campaign sweep result stream')
    if args.output:
        args.output.write_text(''.join(json.dumps(row, separators=(',', ':')) + '\n' for row in rows))
    cells = collections.defaultdict(list)
    for row in rows:
        cells[(row['mission'], row['difficulty'], row['proxy'])].append(row)
    print(f'{len(rows)} runs in {time.monotonic() - started:.1f}s; 60Hz steps, 240 s cap, '
          'no rendering/audio. Cells: clears/runs, median clear time, mean deaths.')
    print('mission | Cadet: proxy C/V/A | Veteran: proxy C/V/A | Ace: proxy C/V/A')
    for mission in sorted({row['mission'] for row in rows}):
        parts = []
        for difficulty in range(3):
            for proxy in range(3):
                group = cells[(mission, difficulty, proxy)]
                wins = [row for row in group if row['won']]
                median = f"{statistics.median(r['seconds'] for r in wins):.0f}s" if wins else '-'
                deaths = sum(r['deaths'] for r in group) / len(group)
                parts.append(f"{len(wins)}/{len(group)} {median} d{deaths:.1f}")
        print(f"{mission} | " + " | ".join(", ".join(parts[at:at + 3]) for at in (0, 3, 6)))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
