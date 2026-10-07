#!/usr/bin/env python3
"""Paired fixed-step AI/instance work proxy; never predicts PSP/vblank timings."""
import argparse
import collections
import json
from pathlib import Path
import re
import statistics
import subprocess

ROOT = Path(__file__).resolve().parent.parent
ROW = re.compile(r'^TREADLINE-HOST frame=(\d+) arena=(\d+) seed=(\d+) ns=(\d+) '
                 r'strategy=(\d+) bank=(\d+) rays=(\d+) bots=(\d+) reset=(\d+) owners=(\d+)$')


def percentile(values, fraction):
    values = sorted(values)
    return values[min(len(values) - 1, int((len(values) - 1) * fraction))]


def summarize(runs):
    frames = collections.defaultdict(list)
    resets = 0
    for rows in runs:
        warm = {}
        for frame, arena, seed, ns, strategy, bank, rays, bots, reset, owners in rows:
            key = (arena, seed)
            if frame == 0 or reset:
                warm[key] = 60
                resets += reset
            if warm.get(key, 0):
                warm[key] -= 1
                continue
            frames[(arena, seed, frame)].append((ns, strategy, bank, rays, bots, owners))
    # Matched deterministic frames across repeats suppress occasional host
    # scheduling noise. Raw maximum is retained, not silently discarded.
    complete = [rows for rows in frames.values() if len(rows) == len(runs)]
    if not complete:
        raise RuntimeError('No complete post-warmup frame matches')
    times = [statistics.median(row[0] for row in rows) for rows in complete]
    work = [rows[0][1:] for rows in complete]
    if any(any(row[1:] != rows[0][1:] for row in rows) for rows in complete):
        raise RuntimeError('Fixed-step work counts are not deterministic')
    tail = sorted(zip(times, work), reverse=True)[:max(1, len(times) // 20)]
    return {
        'frames': len(times), 'resets_across_runs': resets,
        'median_us': statistics.median(times) / 1000,
        'p95_us': percentile(times, .95) / 1000,
        'p99_us': percentile(times, .99) / 1000,
        'max_frame_median_us': max(times) / 1000,
        'raw_max_us': max(row[3] for rows in runs for row in rows) / 1000,
        'strategy_max': max(row[0] for row in work),
        'bank_max': max(row[1] for row in work),
        'combined_planning_max': max(row[0] + row[1] for row in work),
        'strategy_refreshes_by_bot': [sum(bool(row[4] & (1 << bot)) for row in work)
                                       for bot in range(6)],
        'rays_p95': percentile([row[2] for row in work], .95),
        'rays_max': max(row[2] for row in work),
        'tail_strategy_average': statistics.mean(row[1][0] for row in tail),
        'tail_bank_average': statistics.mean(row[1][1] for row in tail),
        'tail_rays_average': statistics.mean(row[1][2] for row in tail),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, default=ROOT / 'build-preset-release/tilefinch-canvas-webgl-conformance-tests')
    parser.add_argument('--baseline', default='examples/treadline-arena/game.js')
    parser.add_argument('--candidate', required=True, help='Source-root-relative experimental game')
    parser.add_argument('--runs', type=int, default=3)
    parser.add_argument('--planning-limit', type=int,
                        help='Optional candidate per-frame combined strategy/bank ceiling')
    parser.add_argument('--work-dir', type=Path, default=ROOT / 'build-preset-release/treadline-host-proxy')
    args = parser.parse_args()
    if not 3 <= args.runs <= 10:
        parser.error('--runs must be 3..10')
    args.work_dir.mkdir(parents=True, exist_ok=True)
    samples = {'baseline': [], 'candidate': []}
    work_samples = {'baseline': [], 'candidate': []}
    for pair in range(args.runs):
        order = ('baseline', 'candidate') if pair % 2 == 0 else ('candidate', 'baseline')
        for label in order:
            source = getattr(args, label)
            result = subprocess.run([str(args.binary), '--treadline-host-timing', source],
                                    cwd=ROOT, capture_output=True, text=True, timeout=60)
            (args.work_dir / f'{label}-{pair + 1}.txt').write_text(result.stdout + result.stderr)
            if result.returncode:
                raise RuntimeError(f'{label} failed: {result.stderr[-2000:]}')
            rows = [tuple(map(int, match.groups())) for line in result.stdout.splitlines()
                    if (match := ROW.match(line))]
            if len(rows) != 7200:
                raise RuntimeError(f'{label}: incomplete profile ({len(rows)}/7200 frames)')
            samples[label].append(rows)
            if pair < 2:
                work = subprocess.run([str(args.binary), '--treadline-host-profile', source],
                                      cwd=ROOT, capture_output=True, text=True, timeout=60)
                (args.work_dir / f'{label}-work-{pair + 1}.txt').write_text(work.stdout + work.stderr)
                if work.returncode:
                    raise RuntimeError(f'{label} work/fairness failed: {work.stderr[-2000:]}')
                work_rows = [tuple(map(int, match.groups())) for line in work.stdout.splitlines()
                             if (match := ROW.match(line))]
                if len(work_rows) != 7200:
                    raise RuntimeError(f'{label}: incomplete work-count profile')
                work_samples[label].append(work_rows)
    summary = {}
    for label in samples:
        timing = summarize(samples[label])
        work = summarize(work_samples[label])
        summary[label] = {key: value for key, value in timing.items()
                          if key.endswith('_us') or key in ('frames', 'resets_across_runs')}
        summary[label].update({key: value for key, value in work.items()
                               if not key.endswith('_us') and key not in ('frames', 'resets_across_runs')})
        # Work rows and timed rows are separate, so no correlation is claimed
        # from counters which would themselves add cost to every wall ray.
        for key in ('tail_strategy_average', 'tail_bank_average', 'tail_rays_average'):
            summary[label].pop(key)
    if (args.planning_limit is not None
            and summary['candidate']['combined_planning_max'] > args.planning_limit):
        raise RuntimeError('Candidate exceeded the requested per-frame planning ceiling')
    print(json.dumps(summary, indent=2))
    (args.work_dir / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print('CPU proxy: counter-free timing; separate deterministic work/fairness runs. '
          'Fixed 30Hz simulation and instance preparation only; '
          'no rendering, vblank, network, active audio graph, or PSP-ms prediction.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
