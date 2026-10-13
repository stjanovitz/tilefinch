"""Adapt anonymous, per-hop visual captures to the lab's logical-fetch model.

Chrome needs the individual redirect hops. The native fetch recorder instead
stores one completed response for a followed request. Never edit either source
capture or invent a missing response; retain a digest-bound derived trace.
"""
from pathlib import Path
import shutil
from urllib.parse import urljoin, urlsplit


def _read(path, maximum):
    if path.is_symlink() or not path.is_file() or path.stat().st_size > maximum:
        raise ValueError('invalid visual trace artifact')
    data = path.read_bytes()
    if len(data) > maximum:
        raise ValueError('visual trace artifact grew beyond its bound')
    return data


def _metadata(path):
    pairs = [line.split('=', 1) for line in _read(path, 65536).decode().splitlines() if line]
    if any(len(p) != 2 for p in pairs) or len({p[0] for p in pairs}) != len(pairs):
        raise ValueError('ambiguous visual trace metadata')
    return dict(pairs)


def _anonymous(meta):
    return (meta.get('method') == 'GET' and meta.get('request-body-length') == '0'
            and meta.get('request-cookie-bytes', '0') == '0'
            and meta.get('request-credentials', '0') == '0'
            and meta.get('set-cookie-count', '0') == '0'
            and meta.get('request-extra-header-bytes', '0') == '0'
            and meta.get('request-enforce-cors', '0') == '0'
            and meta.get('request-redirect-same-origin-only', '0') == '0')


def prepare_native_visual_trace(source: Path, destination: Path) -> dict:
    """Collapse only fully captured anonymous visual chains, at most 10 hops.

    Other requests are copied unchanged, including author fetches. Missing,
    cyclic or ambiguous chains are refused rather than mistaken for images.
    """
    entries = sorted(source.iterdir())
    if source.is_symlink() or len(entries) > 8192:
        raise ValueError('visual trace entry cap exceeded')
    total = 0
    records = {}
    for path in entries:
        if path.is_symlink() or not path.is_file():
            raise ValueError('visual trace must contain regular artifacts')
        total += path.stat().st_size
        if total > 512 * 1024 * 1024:
            raise ValueError('visual trace byte cap exceeded')
        if path.suffix == '.meta':
            meta = _metadata(path)
            records.setdefault(meta.get('url'), []).append((path, meta))
    changes = []
    for path in entries:
        if path.suffix != '.meta':
            continue
        original = _metadata(path)
        current = original
        chain = [path]
        while current.get('status') in ('301', '302', '303', '307', '308'):
            locations = [v.split(':', 1)[1].strip() for k, v in current.items()
                         if k.startswith('response-header-') and v.lower().startswith('location:')]
            if not locations:
                break
            if len(locations) != 1 or len(chain) > 10:
                raise ValueError('ambiguous or excessive visual redirect chain')
            target = urljoin(current['url'], locations[0])
            parsed = urlsplit(target)
            if parsed.scheme not in ('http', 'https') or parsed.username or parsed.password:
                raise ValueError('invalid visual redirect target')
            candidates = records.get(target, [])
            if len(candidates) != 1:
                if Path(urlsplit(original['url']).path).suffix.lower() in (
                        '.png', '.jpg', '.jpeg', '.gif', '.webp', '.svg', '.css', '.woff', '.woff2'):
                    raise ValueError('missing or ambiguous visual redirect response')
                # Non-visual author redirects need not belong to this adapter.
                break
            next_path, next_meta = candidates[0]
            if next_path in chain:
                raise ValueError('cyclic visual redirect chain')
            chain.append(next_path)
            current = next_meta
        kind = current.get('content-type', '').split(';', 1)[0]
        visual = kind.startswith(('image/', 'font/')) or kind == 'text/css'
        if len(chain) == 1 or not visual:
            continue
        if current.get('status') != '200' or current.get('success') != '1':
            raise ValueError('visual redirect has no successful terminal response')
        if not all(_anonymous(_metadata(p)) for p in chain):
            raise ValueError('credentialed/policy-sensitive visual redirects are not adapted')
        body_path = chain[-1].with_suffix('.body')
        body = _read(body_path, 64 * 1024 * 1024)
        body_hash = 14695981039346656037
        for byte in body:
            body_hash = ((body_hash ^ byte) * 1099511628211) & ((1 << 64) - 1)
        if len(body) != int(current['length']) or f'{body_hash:016x}' != current['response-body-hash']:
            raise ValueError('visual terminal body does not match its metadata')
        # Keep the initial request identity and the exact final response.
        response_start = list(current).index('status')
        response = dict(list(current.items())[response_start:])
        merged = dict(list(original.items())[:list(original).index('status')])
        merged.update(response)
        merged['effective-url'] = current['url']
        merged['redirect-origin-tainted'] = str(int(any(
            urlsplit(_metadata(p)['url']).netloc != urlsplit(original['url']).netloc for p in chain)))
        changes.append((path.name, merged, body, [p.name for p in chain]))
    if not changes:
        return {'trace': str(source), 'visual_redirects': []}
    destination.mkdir(exist_ok=False)
    for path in entries:
        shutil.copyfile(path, destination / path.name)
    for name, meta, body, chain in changes:
        (destination / name).write_text(''.join(f'{k}={v}\n' for k, v in meta.items()))
        (destination / name).with_suffix('.body').write_bytes(body)
    return {'trace': str(destination),
            'visual_redirects': [{'record': name, 'chain': chain} for name, _, _, chain in changes]}
