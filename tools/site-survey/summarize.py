#!/usr/bin/env python3
"""Summarize a PPSSPP news-load run: summarize.py RUN_DIR [...]"""
import re, sys, os

def field(line, key):
    m = re.search(r'(?:^|\s)' + re.escape(key) + r'=("([^"]*)"|\S+)', line)
    if not m: return None
    return m.group(2) if m.group(2) is not None else m.group(1)

for run in sys.argv[1:]:
    path = os.path.join(run, 'tilefinch-validation.txt')
    if not os.path.exists(path):
        print(run, 'NO LOG'); continue
    lines = open(path, errors='replace').read().splitlines()
    out = {}
    peak = 0
    relayouts = []
    for l in lines:
        if l.startswith('tilefinch-load-experience:') and 'scope=initial' in l:
            out['first'] = field(l, 'first-present'); out['loaded'] = field(l, 'loaded')
            out['height'] = field(l, 'height')
        if l.startswith('tilefinch-psp-script: load'):
            out['load'] = l.split(':', 1)[1].strip()[:260]
        if l.startswith('tilefinch-budget:'):
            p = int(field(l, 'peak') or 0); peak = max(peak, p)
        if 'tilefinch-relayout-phases:' in l:
            b = field(l, 'build'); relayouts.append(int(b.rstrip('us')) // 1000)
        if l.startswith('tilefinch-input-script-js:') and 'mark=top' in l:
            out['js'] = 'disc=%s att=%s ok=%s fail=%s summary=%s' % (
                field(l, 'discovered'), field(l, 'attempted'), field(l, 'loaded'),
                field(l, 'failed'), field(l, 'summary'))
            e = l.find('error="')
            if e >= 0: out['jserr'] = l[e+7:e+120]
        if l.startswith('tilefinch-input-script-images:') and 'mark=top' in l:
            out['img'] = 'attempts=%s loaded=%s failed=%s' % (field(l, 'attempts'), field(l, 'loaded'), field(l, 'failed'))
        if l.startswith('tilefinch-input-script-js:') and 'mark=p2' in l:
            e = l.find('error="')
            if e >= 0: out['jserr-p2'] = l[e+7:e+120]
        if l.startswith('tilefinch-validation: outcome='):
            out['outcome'] = field(l, 'outcome')
        if 'refus' in l.lower() and 'tilefinch-' in l and len(out.get('refusals', [])) < 5:
            out.setdefault('refusals', []).append(l[:200])
    out['peak-MB'] = '%.1f' % (peak / 1048576)
    out['relayout-builds-ms'] = relayouts
    print('==', run)
    for k, v in out.items():
        print('  %s: %s' % (k, v))
