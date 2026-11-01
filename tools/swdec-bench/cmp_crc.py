#!/usr/bin/env python3
"""cmp_crc.py RESULTS CLIP REF.framecrc : compare bench per-frame adler32 with ffmpeg framecrc.

Every non-stages run of CLIP in RESULTS (each variant, e.g. mc=new and mc=ref)
must match REF frame for frame. A run with restarts (restart=N: the decoder
closed mid-stream and decoded from the start again) has one segment per
decoder: each segment before a restart must match the start of REF, the last
one all of it."""
import sys
res, clip, ref = sys.argv[1:4]
want = [int(l.split(',')[-1], 16) for l in open(ref) if l.startswith('0,')]
runs, got, on = [], None, False
for l in open(res):
    if l.startswith('clip '):
        on = l.split()[1] == clip and "stages=0" in l
        if on:
            mode = ' '.join(w for w in l.split()[2:] if '=' in w and not w.startswith('stages='))
            got = {'mode': mode, 'segments': [[]], 'summarized': False}
            runs.append(got)
    elif on and l.startswith('frame '):
        got['segments'][-1].append(int(l.split()[2], 16))
    elif on and l.startswith('restart '):
        got['segments'].append([])
    elif on and l.startswith('summary'):
        on, got['summarized'] = False, True
status = 0 if runs and want else 1
if not runs:
    print(f"{clip}: no run in the report")
for r in runs:
    *cut, last = r['segments']
    bad = [i for i, (a, b) in enumerate(zip(last, want)) if a != b]
    cut_bad = sum(1 for s in cut for a, b in zip(s, want) if a != b) \
        + sum(1 for s in cut if len(s) > len(want))
    # A clip without its summary line was cut off mid-decode.
    label = f"{clip}{' ' + r['mode'] if r['mode'] else ''}"
    extra = f" restarts={len(cut)} before-restart={[len(s) for s in cut]} mismatched-before={cut_bad}" if cut else ""
    print(f"{label}: frames got={len(last)} ref={len(want)} mismatched={len(bad)} first-bad={bad[:3]}"
          + extra + ("" if r['summarized'] else " (no summary: report truncated)"))
    if not (r['summarized'] and not bad and not cut_bad and len(last) == len(want)):
        status = 1
sys.exit(status)
