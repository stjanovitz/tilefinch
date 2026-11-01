#!/usr/bin/env python3
"""Record page video with the lab and replay it with the server gone.

`record-page-media` (under --capture-http) must capture the page-media probe
and the progressive MP4 it routes to, whichever transport the probe takes;
`check-page-media` (under --replay-http-response-keyed) must then play the
recording with no server at all. Two pages, both on a loopback server that
honours Range like a CDN:

- a page whose <video> names an MP4 directly;
- a page that declares a player page (the embed hop), whose script fetches
  JSON naming the MP4;
- a player page whose JSON names an MP4 the PSP's decoders refuse, then an
  HLS stream. The device walks to the second candidate, so the recorder
  must take the same decoder verdict on what it recorded and record the HLS
  stream too; the replay check must then choose the HLS candidate;
- a page whose MP4 is indexed at its end, beyond the bytes the cap lets the
  recorder keep: the verdict comes from the moov, found by its box headers
  and read on its own;
- a player page naming such a tail-indexed MP4 that the decoders refuse,
  then an HLS master whose only variant is 640x360 Main. The firmware plays
  that variant and the software bound has none; one bound playing is
  enough, so the walk must record and choose it.

A recorder that took the probe's worker route without waiting for it (it
spun its idle limit in a few milliseconds while the captured request was
still in flight) failed every progressive page with "probe refused (HTTP 0)";
the media server answers after half a second so that cannot pass here.
"""
import http.server
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import threading
import time

lab = Path(sys.argv[1]).resolve()
root = Path(__file__).resolve().parents[1]
clip = (root / 'tests/fixtures/psp-media/baseline-320x240.mp4').read_bytes()


def refused_clip(movie):
    """The fixture with its SPS (and avcC) naming High 4:4:4 Predictive,
    a profile no PSP decoder takes."""
    at = movie.index(b'avcC') + 4
    data = bytearray(movie)
    assert data[at + 8] & 0x1f == 7  # the first SPS NAL header
    data[at + 1] = 244
    data[at + 9] = 244
    return bytes(data)


big = refused_clip(clip)


def tail_indexed(movie, padding):
    """MOVIE rewritten as ftyp, free(PADDING), mdat, moov: the index after
    the media, past any first window, with its chunk offsets moved."""
    boxes, at = {}, 0
    while at < len(movie):
        size = int.from_bytes(movie[at:at + 4], 'big')
        boxes[movie[at + 4:at + 8]] = (at, movie[at:at + size])
        at += size
    ftyp, moov = boxes[b'ftyp'][1], bytearray(boxes[b'moov'][1])
    old_mdat, mdat = boxes[b'mdat']
    free = (padding + 8).to_bytes(4, 'big') + b'free' + bytes(padding)
    delta = len(ftyp) + len(free) - old_mdat
    at = moov.find(b'stco')
    while at >= 0:
        count = int.from_bytes(moov[at + 8:at + 12], 'big')
        for i in range(count):
            field = at + 12 + 4 * i
            value = int.from_bytes(moov[field:field + 4], 'big') + delta
            moov[field:field + 4] = value.to_bytes(4, 'big')
        at = moov.find(b'stco', at + 4)
    return ftyp + free + mdat + bytes(moov)


PADDING = 6 * 1024 * 1024
tail = tail_indexed(clip, PADDING)
tail_refused = tail_indexed(big, PADDING)


# ---- A small H.264 + AAC transport stream (tests/test_trace_media_replay.c)
class Bits:
    def __init__(self):
        self.value, self.count = 0, 0

    def put(self, value, count):
        self.value = (self.value << count) | (value & ((1 << count) - 1))
        self.count += count

    def ue(self, value):
        code = value + 1
        width = code.bit_length()
        self.put(0, width - 1)
        self.put(code, width)

    def bytes(self):
        pad = (8 - self.count % 8) % 8
        return (self.value << pad).to_bytes((self.count + pad) // 8, 'big')


def sps(main=False):
    """High 432x240 (software decoder), or Main 3.0 640x360 (firmware)."""
    b = Bits()
    for value, count in ((0x67, 8), (77 if main else 100, 8), (0, 8),
                         (30, 8)):
        b.put(value, count)
    b.ue(0)                     # sps id
    if not main:
        for value in (1, 0, 0):  # 4:2:0, 8-bit luma and chroma
            b.ue(value)
        b.put(0, 1)
        b.put(0, 1)
    for value in (0, 0, 0, 1):   # frame num, POC type, POC lsb, refs
        b.ue(value)
    b.put(0, 1)
    if main:
        b.ue(39)                # 640 pixels
        b.ue(22)                # 368 rows, cropped to 360
        b.put(1, 1)
        b.put(1, 1)
        b.put(1, 1)
        for value in (0, 0, 0, 4):
            b.ue(value)
        b.put(0, 1)
    else:
        b.ue(26)                # 432 pixels
        b.ue(14)                # 240 pixels
        for value in (1, 1, 0, 0):
            b.put(value, 1)
    b.put(1, 1)
    return b.bytes()


def ts_packet(pid, payload):
    adaptation = 183 - len(payload)
    head = bytes([0x47, 0x40 | (pid >> 8), pid & 0xff, 0x30, adaptation])
    if adaptation:
        head += b'\x00' + b'\xff' * (adaptation - 1)
    return head + payload


def pes(stream, pts, payload):
    stamp = bytes([0x21 | ((pts >> 29) & 0x0e), (pts >> 22) & 0xff,
                   1 | ((pts >> 14) & 0xfe), (pts >> 7) & 0xff,
                   1 | ((pts << 1) & 0xfe)])
    return bytes([0, 0, 1, stream, 0, 0, 0x80, 0x80, 5]) + stamp + payload


def segment(pts, main=False):
    """PAT, PMT, one IDR access unit and one ADTS frame."""
    pat = bytes([0x00, 0xb0, 0x0d, 0, 1, 0xc1, 0, 0, 0, 1, 0xe1, 0,
                 0, 0, 0, 0])
    pmt = bytes([0x02, 0xb0, 0x17, 0, 1, 0xc1, 0, 0, 0xe1, 1, 0xf0, 0,
                 0x1b, 0xe1, 1, 0xf0, 0, 0x0f, 0xe1, 2, 0xf0, 0,
                 0, 0, 0, 0])
    au = (b'\x00\x00\x01' + sps(main)
          + bytes([0, 0, 1, 0x68, 0xce, 0x06, 0xe2, 0, 0, 1, 0x65,
                   0x80, 0x11]))
    adts = bytes([0xff, 0xf1, 0x50, 0x80, 0x00, 0xff, 0xfc])
    return (ts_packet(0, b'\x00' + pat) + ts_packet(0x100, b'\x00' + pmt)
            + ts_packet(0x101, pes(0xe0, pts, au))
            + ts_packet(0x102, pes(0xc0, pts, adts)))


MASTER = (b'#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=300000,RESOLUTION=432x240,'
          b'CODECS="avc1.64001e,mp4a.40.2"\nlow/index.m3u8\n')
MAIN_MASTER = (b'#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=800000,'
               b'RESOLUTION=640x360,CODECS="avc1.4d401e,mp4a.40.2"\n'
               b'low/index.m3u8\n')
VARIANT = (b'#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:2\n'
           b'#EXT-X-MEDIA-SEQUENCE:0\n'
           + b''.join(b'#EXTINF:2.0,\nseg%d.ts\n' % i for i in range(3))
           + b'#EXT-X-ENDLIST\n')


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def answer(self, status, content_type, body, extra=()):
        self.send_response(status)
        self.send_header('Content-Type', content_type)
        self.send_header('Content-Length', str(len(body)))
        for name, value in extra:
            self.send_header(name, value)
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        port = self.server.server_port
        path = self.path.split('?')[0]
        if path == '/direct':
            self.answer(200, 'text/html', (
                '<!doctype html><title>Direct</title><body><p>Story</p>'
                '<video controls width=320 height=240 '
                'src="http://127.0.0.1:%d/media/direct.mp4"></video>'
                '</body>' % port).encode())
        elif path == '/watch':
            self.answer(200, 'text/html', (
                '<!doctype html><title>Watch</title>'
                '<meta property=og:title content="Clip">'
                '<meta name=twitter:player '
                'content=http://127.0.0.1:%d/embed/clip>'
                '<body><p>Story</p><video controls width=320 height=240>'
                '</video></body>' % port).encode())
        elif path == '/embed/clip':
            self.answer(200, 'text/html', (
                '<!doctype html><title>Player</title><body><script>'
                "fetch('/api/play/clip').then(function(r){"
                "return r.text()}).then(function(t){"
                "document.title='fetched '+t.length})</script></body>"
            ).encode())
        elif path == '/walk':
            self.answer(200, 'text/html', (
                '<!doctype html><title>Walk</title>'
                '<meta property=og:title content="Walk">'
                '<meta name=twitter:player '
                'content=http://127.0.0.1:%d/embed/walk>'
                '<body><p>Story</p><video controls width=320 height=240>'
                '</video></body>' % port).encode())
        elif path == '/embed/walk':
            self.answer(200, 'text/html', (
                '<!doctype html><title>Player</title><body><script>'
                "fetch('/api/play/walk').then(function(r){"
                "return r.text()}).then(function(t){"
                "document.title='fetched '+t.length})</script></body>"
            ).encode())
        elif path == '/api/play/walk':
            self.answer(200, 'application/json', (
                '{"sources":[{"src":"http:\\/\\/127.0.0.1:%d\\/media\\/big.mp4",'
                '"type":"video/mp4"},{"src":"http:\\/\\/127.0.0.1:%d\\/hls\\/'
                'master.m3u8","type":"application/x-mpegURL"}]}'
                % (port, port)).encode())
        elif path == '/tail':
            self.answer(200, 'text/html', (
                '<!doctype html><title>Tail</title><body><p>Story</p>'
                '<video controls width=320 height=240 '
                'src="http://127.0.0.1:%d/media/tail.mp4"></video>'
                '</body>' % port).encode())
        elif path == '/walk2':
            self.answer(200, 'text/html', (
                '<!doctype html><title>Walk</title>'
                '<meta property=og:title content="Walk">'
                '<meta name=twitter:player '
                'content=http://127.0.0.1:%d/embed/walk2>'
                '<body><p>Story</p><video controls width=320 height=240>'
                '</video></body>' % port).encode())
        elif path == '/embed/walk2':
            self.answer(200, 'text/html', (
                '<!doctype html><title>Player</title><body><script>'
                "fetch('/api/play/walk2').then(function(r){"
                "return r.text()}).then(function(t){"
                "document.title='fetched '+t.length})</script></body>"
            ).encode())
        elif path == '/api/play/walk2':
            self.answer(200, 'application/json', (
                '{"sources":[{"src":"http:\\/\\/127.0.0.1:%d\\/media\\/'
                'tail-refused.mp4","type":"video/mp4"},{"src":"http:\\/\\/'
                '127.0.0.1:%d\\/main\\/master.m3u8",'
                '"type":"application/x-mpegURL"}]}' % (port, port)).encode())
        elif path == '/main/master.m3u8':
            self.answer(200, 'application/vnd.apple.mpegurl', MAIN_MASTER)
        elif path == '/main/low/index.m3u8':
            self.answer(200, 'application/vnd.apple.mpegurl', VARIANT)
        elif path.startswith('/main/low/seg') and path.endswith('.ts'):
            index = int(path[len('/main/low/seg'):-3])
            self.answer(200, 'video/mp2t',
                        segment(index * 180000, main=True))
        elif path == '/hls/master.m3u8':
            self.answer(200, 'application/vnd.apple.mpegurl', MASTER)
        elif path == '/hls/low/index.m3u8':
            self.answer(200, 'application/vnd.apple.mpegurl', VARIANT)
        elif path.startswith('/hls/low/seg') and path.endswith('.ts'):
            index = int(path[len('/hls/low/seg'):-3])
            self.answer(200, 'video/mp2t', segment(index * 180000))
        elif path == '/api/play/clip':
            self.answer(200, 'application/json', (
                '{"playbackUrl":"http:\\/\\/127.0.0.1:%d\\/media\\/hop.mp4",'
                '"type":"video/mp4"}' % port).encode())
        elif path in ('/media/direct.mp4', '/media/hop.mp4',
                      '/media/big.mp4', '/media/tail.mp4',
                      '/media/tail-refused.mp4'):
            # A CDN's first byte is never instant: the recorder must wait
            # for a request in flight rather than count idle turns.
            time.sleep(0.5)
            movie = {'/media/big.mp4': big, '/media/tail.mp4': tail,
                     '/media/tail-refused.mp4': tail_refused}.get(path, clip)
            match = re.match(r'bytes=(\d+)-(\d*)$',
                             self.headers.get('Range', ''))
            if match is None:
                self.answer(200, 'video/mp4', movie)
                return
            first = int(match.group(1))
            last = min(int(match.group(2) or len(movie) - 1), len(movie) - 1)
            self.answer(206, 'video/mp4', movie[first:last + 1],
                        [('Content-Range',
                          'bytes %d-%d/%d' % (first, last, len(movie)))])
        else:
            self.answer(404, 'text/plain', b'not found')

    def log_message(self, *args):
        pass


def run_lab(work, url, commands, trace_option, trace_dir):
    script = work / 'commands.txt'
    script.write_text(commands)
    arguments = [str(lab), '--url', url, '--fetch-scripts',
                 '--psp-profile', 'realistic', '--commands', str(script),
                 '--no-loop-capture', '--loop-output-dir', str(work),
                 '--output', str(work / 'final.ppm'),
                 trace_option, str(trace_dir)]
    result = subprocess.run(arguments, env=dict(os.environ,
                                                SDL_AUDIODRIVER='dummy'),
                            capture_output=True, text=True, timeout=120)
    output = result.stdout + result.stderr
    assert re.search(r'teardown=0\b', output), output
    return output


def check_line(output, mode, hls=None):
    """The first successful check line of MODE (one per candidate tried)."""
    pattern = r'loop-page-media-check status=ok mode=%s .*' % mode
    if hls is not None:
        pattern = (r'loop-page-media-check status=ok mode=%s ms=\d+ hls=%d .*'
                   % (mode, hls))
    match = re.search(pattern, output)
    assert match is not None, output
    return match.group(0)


RECORD = ('pump 120 30\npage-media-url\nrecord-page-media 60 30\n'
          'pump 60 30\nquit\n')
CHECK = ('pump 120 30\npage-media-url\ncheck-page-media 60\n'
         'pump 60 30\nquit\n')

server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()
base = 'http://127.0.0.1:%d' % server.server_port
pages = (('direct', base + '/direct'), ('hop', base + '/watch'))
walk = base + '/walk'
try:
    with tempfile.TemporaryDirectory(prefix='tilefinch-record-media-') as d:
        work = Path(d)
        for name, url in pages:
            line = check_line(run_lab(work, url, RECORD, '--capture-http',
                                      work / name), 'record')
            # The whole file was recorded as one Range response.
            assert ' length=%d payload=%d ' % (len(clip), len(clip)) in line, \
                line
        # The walk: the refused MP4 is recorded, judged, and passed over;
        # the HLS candidate after it is recorded as the device plays it.
        output = run_lab(work, walk, RECORD, '--capture-http', work / 'walk')
        refused = re.search(r'loop-page-media-check status=failed mode=record'
                            r' .*candidate=1/2 decoder=refused .*', output)
        assert refused is not None, output
        assert ' length=%d payload=%d ' % (len(big), len(big)) \
            in refused.group(0), refused.group(0)
        line = check_line(output, 'record', hls=1)
        assert ' candidate=2/2 ' in line and ' segments=3 ' in line, line
        assert 'loop-page-media-check chosen=2 of=2' in output, output
        # A tail-indexed file larger than the 4 MiB cap: its verdict is
        # taken from the moov beyond the recorded prefix.
        output = run_lab(work, base + '/tail',
                         RECORD.replace('record-page-media 60 30',
                                        'record-page-media 60 4'),
                         '--capture-http', work / 'tail')
        line = check_line(output, 'record')
        assert ' decoder=firmware ' in line and ' size=320x240 ' in line, line
        assert 'loop-page-media-check chosen=1 of=1' in output, output
        # Refused tail-indexed file, then a Main-only HLS master: the refused
        # file costs its head and moov reads only (no bulk record), and the
        # firmware variant is chosen
        # although the software bound has nothing to select.
        output = run_lab(work, base + '/walk2', RECORD, '--capture-http',
                         work / 'walk2')
        refused = re.search(r'loop-page-media-check status=failed mode=record'
                            r' .*candidate=1/2 decoder=refused .*', output)
        assert refused is not None, output
        payload = re.search(r' payload=(\d+) ', refused.group(0))
        assert int(payload.group(1)) < len(tail_refused), refused.group(0)
        line = check_line(output, 'record', hls=1)
        assert ' candidate=2/2 ' in line and ' size=640x360 ' in line, line
        assert ' decoder=firmware ' in line, line
        assert ' software-bound=failed ' in line, line
        assert 'loop-page-media-check chosen=2 of=2' in output, output
        server.shutdown()
        server.server_close()
        for name, url in pages:
            output = run_lab(work, url, CHECK,
                             '--replay-http-response-keyed', work / name)
            line = check_line(output, 'check')
            video = re.search(r' video=([0-9.]+)s ', line)
            assert video is not None and float(video.group(1)) > 1.0, line
            assert ' size=320x240 ' in line, line
            assert re.search(r'range-slices=[1-9]', output), output
        # With no server, the check walks to the recorded HLS fallback.
        output = run_lab(work, walk, CHECK, '--replay-http-response-keyed',
                         work / 'walk')
        assert re.search(r'loop-page-media-check status=failed mode=check '
                         r'.*candidate=1/2 decoder=refused ', output), output
        line = check_line(output, 'check', hls=1)
        assert ' candidate=2/2 ' in line and ' segments=3 ' in line, line
        assert ' size=432x240 ' in line, line
        assert 'loop-page-media-check chosen=2 of=2' in output, output
        assert 'replay has no record' not in output, output
        output = run_lab(work, base + '/walk2', CHECK,
                         '--replay-http-response-keyed', work / 'walk2')
        assert re.search(r'loop-page-media-check status=failed mode=check '
                         r'.*candidate=1/2 decoder=refused ', output), output
        line = check_line(output, 'check', hls=1)
        assert ' candidate=2/2 ' in line and ' size=640x360 ' in line, line
        assert 'loop-page-media-check chosen=2 of=2' in output, output
        assert 'replay has no record' not in output, output
finally:
    server.shutdown()
    server.server_close()
print('record-page-media tests passed')
