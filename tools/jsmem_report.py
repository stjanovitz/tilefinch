#!/usr/bin/env python3
"""Summarise TILEFINCH_TRACE_JS_HEAP_DETAIL runs (docs/DEVELOPMENT.md,
"JavaScript heap composition").

  jsmem_report.py RUNROOT [PAGE...]

RUNROOT has one directory per page holding the lab's lab.log (stdout) and
lab.err (stderr). For each page the census at the first refused allocation
(or, without one, the largest census) is split into MB columns: bytecode,
line tables, retained source, atoms, strings, shapes, plain objects, DOM
wrappers (instances of DOM prototypes), arrays, closures (function objects
and variable references), typed arrays, Maps/Sets, promises and jobs,
arena slack and headers, and the rest of QuickJS's malloc total (parser and
compiler state, strings reached only from the stack). "GC frees" needs a
=gc run. FILES=1 adds the ten largest scripts and twelve largest prototypes.
"""
import os, re, sys, json, collections
MB = 1048576.0
def parse(err):
    cens = []; cur = None
    for line in open(err, errors='replace'):
        if line.startswith('jsmem-census '):
            kv = dict(re.findall(r'(\S+?)=(\S+)', line))
            cur = {'hdr': kv, 'rows': []}; cens.append(cur)
        elif line.startswith('jsmem-row ') and cur is not None:
            kv = dict(re.findall(r' (v\d+|kind)=(\S+)', line))
            name = line.rsplit('name="', 1)[1].rstrip('"\n')
            vals = [int(kv['v%d' % i]) for i in range(12) if 'v%d' % i in kv]
            cur['rows'].append((kv['kind'], name, vals))
    return cens

FN_CLASSES = {'Function', 'GeneratorFunction', 'AsyncFunction', 'AsyncGeneratorFunction'}
PROMISE = {'Promise', 'PromiseResolveFunction', 'PromiseRejectFunction', 'AsyncFunctionResolve', 'AsyncFunctionReject'}
TYPED = {'ArrayBuffer', 'SharedArrayBuffer', 'Uint8ClampedArray', 'Int8Array', 'Uint8Array', 'Int16Array', 'Uint16Array', 'Int32Array', 'Uint32Array', 'BigInt64Array', 'BigUint64Array', 'Float16Array', 'Float32Array', 'Float64Array', 'DataView'}
MAPS = {'Map', 'Set', 'WeakMap', 'WeakSet'}
ARR = {'Array', 'Arguments'}

def compose(c):
    h = c['hdr']; rows = c['rows']
    out = collections.OrderedDict()
    alloc = [r for r in rows if r[0] == 'alloc'][0][2]
    malloc, limit, arenas, arena_alloc, cap, used, large = alloc[:7]
    gc = {r[1]: r[2] for r in rows if r[0] == 'gc'}
    files = [r for r in rows if r[0] == 'file']
    bc = sum(f[2][1] + f[2][3] + f[2][9] for f in files)
    pc2 = sum(f[2][4] for f in files)
    src_own = sum(f[2][5] for f in files); src_sh = sum(f[2][6] for f in files)
    cls = collections.defaultdict(int)
    for k, n, v in rows:
        if k != 'class': continue
        b = v[2] + v[3] + v[4]
        if n in FN_CLASSES: cls['closures'] += b
        elif n in PROMISE: cls['promises'] += b
        elif n in TYPED: cls['typed'] += b
        elif n in MAPS: cls['maps'] += b
        elif n in ARR: cls['arrays'] += b
        elif n in ('C function', 'Bound', 'BoundFunction'): cls['natives'] += b
        else: cls['objects'] += b
    prot = [r for r in rows if r[0] == 'proto']
    dom_names = re.compile(r'^(HTML\w*|SVG\w*|Text|Comment|Attr|Node|Element|Document\w*|CharacterData|DOMTokenList|NamedNodeMap|CSSStyleDeclaration|NodeList|HTMLCollection|Window|EventTarget|DocumentFragment|ShadowRoot|Event|\w*Event)$')
    dom = sum(p[2][1] for p in prot if dom_names.match(p[1]))
    domn = sum(p[2][0] for p in prot if dom_names.match(p[1]))
    strings = sum(r[2][1] for r in rows if r[0] == 'strings')
    big_str = sum(r[2][1] for r in rows if r[0] == 'strings' and r[1] in ('<256K', '>=256K'))
    atoms = [r for r in rows if r[0] == 'atoms'][0][2]
    shapes = int(h['shapes'].split('/')[1])
    jobs = [r for r in rows if r[0] == 'jobs'][0][2]
    out['malloc'] = malloc
    out['bytecode'] = bc
    out['pc2line'] = pc2
    out['source'] = src_own + src_sh
    out['src_lazy_own'] = src_own
    out['src_shared'] = src_sh
    out['atoms'] = atoms[1] + atoms[2]
    out['strings'] = strings
    out['strings_big'] = big_str
    out['shapes'] = shapes
    out['objects'] = cls['objects'] - dom  # dom wrappers are class Object
    out['dom_wrappers'] = dom
    out['dom_wrapper_count'] = domn
    out['arrays'] = cls['arrays']
    out['closures'] = cls['closures'] + gc.get('var_ref', [0, 0])[1]
    out['typed'] = cls['typed']
    out['maps'] = cls['maps']
    out['promises_jobs'] = cls['promises'] + jobs[1] + gc.get('async_function', [0, 0])[1]
    out['natives'] = cls['natives']
    out['arena_waste'] = cap - used
    out['arena_headers'] = arena_alloc - cap
    known = sum(v for k, v in out.items() if k not in ('malloc', 'src_lazy_own', 'src_shared', 'strings_big', 'dom_wrapper_count'))
    out['other'] = malloc - known
    out['reserved'] = int(h['pool-reserved'])
    out['budget'] = int(h.get('budget', 0))
    out['js_cat'] = int(h.get('javascript', 0))
    return out, files, prot

def pick(cens):
    oom = [c for c in cens if c['hdr']['reason'].startswith('oom') and not c['hdr']['reason'].endswith('aftergc')]
    if oom: return oom[0], 'oom'
    best = max(cens, key=lambda c: int(c['hdr']['pool-current'])) if cens else None
    return best, 'peak'

def main():
    root = sys.argv[1]; sites = sys.argv[2:] or sorted(os.listdir(root))
    table = []
    for s in sites:
        err = os.path.join(root, s, 'lab.err')
        if not os.path.exists(err): continue
        cens = parse(err)
        # per realm: choose the realm with the largest pool
        if not cens: continue
        c, why = pick(cens)
        comp, files, prot = compose(c)
        after = [x for x in cens if x['hdr']['reason'].endswith('aftergc') and x['hdr']['rt'] == c['hdr']['rt']]
        freed = (int(c['hdr']['pool-current']) - int(after[0]['hdr']['pool-current'])) if after else None
        log = open(os.path.join(root, s, 'lab.log'), errors='replace').read()
        m = re.search(r'interactive teardown=.*?peak=(\d+)', log)
        jsp = re.search(r'memory-category name=javascript current=\d+ peak=(\d+)', log)
        ooms = log.count('InternalError: out of memory')
        table.append((s, why, c['hdr']['reason'], comp, files, prot, freed, int(m.group(1)) if m else 0, int(jsp.group(1)) if jsp else 0, ooms))
    return table

if __name__ == '__main__':
    t = main()
    cols = ['malloc', 'bytecode', 'pc2line', 'source', 'atoms', 'strings', 'shapes', 'objects', 'dom_wrappers', 'arrays', 'closures', 'typed', 'maps', 'promises_jobs', 'arena_waste', 'arena_headers', 'other']
    print('| site | at | ooms | page peak | JS cat peak | ' + ' | '.join(cols) + ' | GC frees |')
    print('|' + '---|' * (len(cols) + 6))
    for s, why, reason, comp, files, prot, freed, peak, jsp, ooms in t:
        print('| %s | %s | %d | %.1f | %.1f | ' % (s, why, ooms, peak / MB, jsp / MB) + ' | '.join('%.2f' % (comp[k] / MB) for k in cols) + ' | %s |' % ('-' if freed is None else '%.2f' % (freed / MB)))
    if os.environ.get('FILES'):
        for s, why, reason, comp, files, prot, freed, peak, jsp, ooms in t:
            print('\n##', s, why, reason, 'dom wrappers', comp['dom_wrapper_count'], 'lazy-own-src %.2f shared-src %.2f' % (comp['src_lazy_own'] / MB, comp['src_shared'] / MB))
            fs = sorted(files, key=lambda f: -(f[2][1] + f[2][3] + f[2][4] + f[2][5] + f[2][6] + f[2][8] + f[2][9]))[:10]
            for f in fs:
                v = f[2]
                print('  %-90s funcs=%d bc=%.0fK lazy=%d/%.0fK pc2line=%.0fK src-own=%.0fK src-shared=%.0fK closures=%d/%.0fK body=%.0fK' % (f[1][:90], v[0], v[1] / 1024, v[2], v[3] / 1024, v[4] / 1024, v[5] / 1024, v[6] / 1024, v[7], v[8] / 1024, v[9] / 1024))
            for p in prot[:12]:
                print('  proto %-30s n=%d %.0fK props=%d' % (p[1][:30], p[2][0], p[2][1] / 1024, p[2][2]))
