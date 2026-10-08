#!/usr/bin/env python3
"""Run the headless Treadline invariant sweep and summarise violations.

Seeded matches in every mode, class, campaign mission (plus Reprise, Mirror
and a Range Fault sample) and Practice drill, played by bot proxies or a
scripted keyboard player that lunges, pivots and brake-drifts. Every 1/30 s
step is checked for tanks or shells inside or through scenery, overlaps,
unreachable pickups and objectives, non-finite or out-of-range values,
transitions that never end and long bot stalls; keyboard runs also replay
their recording and compare digests. See tests/fixtures/treadline-invariants.js.

Every step also composes the camera as a device frame does and tracks it for
oscillation, re-retraction (pumping), sudden jumps, sustained jerk and
unsettled shake (tests/fixtures/treadline-camera-motion.js). A flagged case
fails the sweep like any other violation, including pumping and follow-turn
regressions.

Runs in the foreground, single-threaded, for about a minute; exits 1 when any
invariant is violated.
"""
import argparse
import collections
import json
from pathlib import Path
import subprocess
import sys
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--binary', type=Path, default=Path(__file__).resolve().parents[1]
                        / 'build-preset-release/tilefinch-canvas-webgl-conformance-tests')
    parser.add_argument('--first', type=int, default=0, help='first case index')
    parser.add_argument('--count', type=int, default=10000, help='number of cases')
    parser.add_argument('--output', type=Path, help='optional JSONL results')
    parser.add_argument('--verbose', '-v', action='store_true',
                        help='print every violation and the worst camera cases')
    args = parser.parse_args()
    started = time.monotonic()
    run = subprocess.run([str(args.binary), '--treadline-invariants', str(args.first),
                          str(args.count)], text=True, capture_output=True, timeout=1200)
    if run.returncode:
        sys.stderr.write(run.stdout[-4000:] + run.stderr[-4000:])
        return run.returncode
    rows = [json.loads(line.removeprefix('TREADLINE-INVARIANTS '))
            for line in run.stdout.splitlines() if line.startswith('TREADLINE-INVARIANTS ')]
    if not rows:
        raise RuntimeError('No invariant results')
    if args.output:
        args.output.write_text(''.join(json.dumps(row, separators=(',', ':')) + '\n'
                                       for row in rows))
    seconds = sum(row['seconds'] for row in rows)
    print(f'{len(rows)} cases, {seconds:.0f} simulated s in '
          f'{time.monotonic() - started:.1f}s (1/30 s steps, no rendering/audio).')
    by_type = collections.defaultdict(list)
    for row in rows:
        for violation in row['violations']:
            by_type[violation['type']].append((row, violation))
        for flag in row.get('camera', {}).get('flags', []):
            by_type['camera-' + flag['type']].append((row, flag))
    for kind, entries in sorted(by_type.items()):
        cases = sorted({row['label'] for row, _ in entries})
        print(f'VIOLATION {kind}: {len(entries)} in {len(cases)} case kinds '
              f'({", ".join(cases[:8])}{" ..." if len(cases) > 8 else ""})')
        for row, violation in entries[:None if args.verbose else 3]:
            detail = {k: v for k, v in violation.items() if k not in ('type', 'count')}
            print(f'  case {row["case"]} {row["label"]} {row["proxy"]}: {json.dumps(detail)}')
    ends = collections.Counter(row['end'] for row in rows)
    print('ends: ' + ', '.join(f'{name}={n}' for name, n in sorted(ends.items())))
    replays = collections.Counter(row['replay'] for row in rows)
    print('replays: ' + ', '.join(f'{name}={n}' for name, n in sorted(replays.items())))
    stuck = [(row, s) for row in rows for s in row['stuck']]
    print(f'bot stalls >= 8 s: {len(stuck)}')
    for row, s in sorted(stuck, key=lambda e: -e[1]['seconds'])[:None if args.verbose else 8]:
        print(f'  case {row["case"]} {row["label"]}: tank {s["tank"]} {s["role"]} '
              f'at ({s["x"]}, {s["z"]}) from {s["t"]} s for {s["seconds"]} s')
    summarize_camera(rows, args.verbose)
    return 1 if by_type else 0


def summarize_camera(rows, verbose):
    """One summary line for the camera track, worst cases under -v."""
    tracked = [row for row in rows if 'camera' in row]
    if not tracked:
        return
    flagged = [row for row in tracked if row['camera']['flags']]
    kinds = collections.Counter()
    for row in flagged:
        for kind in {f['type'] for f in row['camera']['flags']}:
            kinds[kind] += 1
    samples = sum(row['camera']['samples'] for row in tracked)
    retractions = sum(row['camera']['retractions'] for row in tracked)
    pumps = sum(row['camera']['reRetractions'] for row in tracked)
    detail = ', '.join(f'{kind} {n}' for kind, n in sorted(kinds.items())) or 'none'
    print(f'camera motion: {len(flagged)} of {len(tracked)} cases flagged ({detail}); '
          f'{samples} frames, {retractions} retractions, {pumps} re-retractions within 1.5 s')

    def severity(row):
        c = row['camera']
        return (c['maxReRetractions'] + c['maxDistanceReversals'] + c['maxPitchReversals']
                + c['maxYawReversals'] + c['maxJerkSteps'], c['reRetractions'])
    for row in sorted(flagged, key=severity, reverse=True)[:None if verbose else 0]:
        c = row['camera']
        print(f'  case {row["case"]} {row["label"]} {row["proxy"]}: '
              f'reversals/1.5s d={c["maxDistanceReversals"]} pitch={c["maxPitchReversals"]} '
              f'yaw={c["maxYawReversals"]}, re-retractions/1.5s={c["maxReRetractions"]} '
              f'(total {c["reRetractions"]}), jerk steps/1.5s={c["maxJerkSteps"]}, '
              f'distance {c["minDistance"]}..{c["maxDistance"]}')
        for flag in c['flags']:
            extra = {k: v for k, v in flag.items() if k not in ('type', 'what', 't', 'end', 'peak', 'excerpt')}
            print(f'    {flag["type"]} {flag["what"]} {flag["t"]}-{flag["end"]} s peak {flag["peak"]}'
                  f'{" " + json.dumps(extra) if extra else ""} distance [{flag["excerpt"]}]')


if __name__ == '__main__':
    raise SystemExit(main())
