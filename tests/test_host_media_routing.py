#!/usr/bin/env python3
"""Route page media by what its address answers, against a loopback server.

An offline live channel's player can still hand over a playback address;
opening it used to fail as "could not open MP4 media". Each address below
has no extension (or an HLS one), and the host player must take its route
from the response (media_response_kind): an HLS playlist opens as HLS, a
real MP4 as MP4, and an address that answers 404, an error page, or a
playlist with nothing in it fails as "stream is offline or not available".
The embed hop through a player page whose stream answers 404 ends with
STREAM OFFLINE OR NOT AVAILABLE and goes Back, as its timeout does.
"""
import http.server
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import threading

lab = Path(sys.argv[1]).resolve()
root = Path(__file__).resolve().parents[1]
clip = (root / 'tests/fixtures/psp-media/main-640x360.mp4').read_bytes()
OFFLINE_JSON = b'[{"error":"Can not find channel"}]'

def media_playlist(port):
    return ('#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:4\n'
            '#EXT-X-MEDIA-SEQUENCE:0\n#EXTINF:4.0,\n'
            'http://127.0.0.1:%d/segment/0\n#EXT-X-ENDLIST\n' % port).encode()

class Handler(http.server.BaseHTTPRequestHandler):
    def answer(self, status, content_type, body):
        self.send_response(status)
        if content_type:
            self.send_header('Content-Type', content_type)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Access-Control-Allow-Origin', '*')
        self.end_headers()
        self.wfile.write(body)

    def answer_ranged(self, content_type, body):
        """Honour a single Range the way a CDN does (206 + Content-Range)."""
        match = re.match(r'bytes=(\d+)-(\d*)$', self.headers.get('Range', ''))
        if match is None:
            self.answer(200, content_type, body)
            return
        first = int(match.group(1))
        last = min(int(match.group(2) or len(body) - 1), len(body) - 1)
        part = body[first:last + 1]
        self.send_response(206)
        self.send_header('Content-Type', content_type)
        self.send_header('Content-Range',
                         'bytes %d-%d/%d' % (first, last, len(body)))
        self.send_header('Content-Length', str(len(part)))
        self.end_headers()
        self.wfile.write(part)

    def do_GET(self):
        port = self.server.server_port
        path = self.path.split('?')[0]
        if path.startswith('/watch/'):
            name = path[len('/watch/'):]
            self.answer(200, 'text/html', (
                '<!doctype html><title>Watch</title>'
                '<meta property=og:title content="Channel %s">'
                '<meta name=twitter:player '
                'content=http://127.0.0.1:%d/embed/%s>'
                '<body><p>Story</p><video controls width=320 height=180>'
                '</video></body>' % (name, port, name)).encode())
        elif path.startswith('/embed/'):
            name = path[len('/embed/'):]
            self.answer(200, 'text/html', (
                '<!doctype html><title>Player</title><body><script>'
                "fetch('/api/v1/play/%s').then(function(r){"
                "return r.text()}).then(function(t){"
                "document.title='fetched '+t.length})</script></body>"
                % name).encode())
        elif path.startswith('/api/v1/play/'):
            name = path[len('/api/v1/play/'):]
            # An address with no extension, declared as a direct file:
            # what it answers decides the route.
            target = ('\\/live\\/%s\\/index.m3u8?token=t' % name
                      if name == 'chanm3u8'
                      else '\\/media\\/%s' % name)
            self.answer(200, 'application/json', (
                '{"playbackUrl":"http:\\/\\/127.0.0.1:%d%s",'
                '"type":"video/mp4"}' % (port, target)).encode())
        elif path == '/media/chan404':
            self.answer(404, 'application/json', OFFLINE_JSON)
        elif path == '/media/chan410':
            self.answer(410, '', b'')
        elif path == '/media/chanempty':
            self.answer(200, 'application/vnd.apple.mpegurl',
                        b'#EXTM3U\n#EXT-X-VERSION:3\n'
                        b'#EXT-X-INDEPENDENT-SEGMENTS\n')
        elif path == '/media/chanended':
            # No HLS Content-Type: the body alone says it is a playlist.
            self.answer(200, 'application/octet-stream',
                        b'#EXTM3U\n#EXT-X-TARGETDURATION:2\n'
                        b'#EXT-X-MEDIA-SEQUENCE:40\n#EXT-X-ENDLIST\n')
        elif path == '/media/chanhtml':
            self.answer(200, 'text/html; charset=utf-8',
                        b'<!doctype html><title>Offline</title>'
                        b'<p>This channel is offline.</p>')
        elif path == '/media/chanlive':
            self.answer(200, 'application/octet-stream',
                        ('#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=900000,'
                         'RESOLUTION=640x360\nhttp://127.0.0.1:%d/variant\n'
                         % port).encode())
        elif path in ('/media/chanrangetext', '/media/chanrangeoctet'):
            # A server that honours Range, serving the playlist with no HLS
            # type: the probe sees only the bytes it asked for.
            self.answer_ranged('text/plain' if path.endswith('text')
                               else 'application/octet-stream',
                               ('#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=900000,'
                                'RESOLUTION=640x360\nhttp://127.0.0.1:%d/'
                                'variant\n' % port).encode())
        elif path == '/variant':
            self.answer(200, 'application/octet-stream', media_playlist(port))
        elif path == '/segment/0':
            self.answer(200, 'application/octet-stream', clip)
        elif path == '/media/chanmovie':
            self.answer_ranged('video/mp4', clip)
        elif path == '/live/chanm3u8/index.m3u8':
            self.answer(404, 'application/json', OFFLINE_JSON)
        else:
            self.answer(404, 'text/plain', b'not found')

    def log_message(self, *args):
        pass

def run_lab(work, url, commands, javascript=False, succeeds=True):
    script = work / 'commands.txt'
    script.write_text(commands)
    arguments = [str(lab), '--url', url, '--psp-profile', 'realistic',
                 '--commands', str(script), '--no-loop-capture',
                 '--loop-output-dir', str(work), '--output', str(work / 'final.ppm')]
    if javascript:
        arguments[3:3] = ['--fetch-scripts']
    else:
        arguments.append('--no-javascript')
    result = subprocess.run(arguments, env=dict(os.environ,
                                                SDL_AUDIODRIVER='dummy'),
                            capture_output=True, text=True, timeout=60)
    output = result.stdout + result.stderr
    # A failed open fails its command; ownership must still unwind.
    assert (result.returncode == 0) == succeeds, output
    assert re.search(r'teardown=0\b', output), output
    return output

server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()
base = 'http://127.0.0.1:%d' % server.server_port
try:
    with tempfile.TemporaryDirectory(prefix='tilefinch-media-routing-') as d:
        work = Path(d)
        def hop(name, commands='pump 120 30\nstatus\nquit\n'):
            return run_lab(work, base + '/watch/' + name,
                           'play-page-media\n' + commands, javascript=True)

        # Each player page hands over a stream address that says there is
        # nothing to play: the hop ends with the message and goes Back.
        unavailable = {'chan404': '404', 'chan410': '410', 'chanempty': '200',
                       'chanended': '200', 'chanhtml': '200',
                       'chanm3u8': '404'}
        for name, status in unavailable.items():
            output = hop(name)
            assert 'loop-embed-hop status=ready' in output, (name, output)
            assert ('host-media-probe http=%s ' % status) in output, \
                (name, output)
            assert 'route=unavailable' in output, (name, output)
            failure = re.search(
                r'loop-embed-hop status=open-failed error="([^"]*)"', output)
            assert failure is not None, (name, output)
            assert 'stream is offline or not available' in failure.group(1), \
                (name, failure.group(1))
            assert 'MP4' not in failure.group(1), (name, failure.group(1))
            assert ('loop-embed-hop status=unavailable '
                    'message="STREAM OFFLINE OR NOT AVAILABLE"') in output, \
                (name, output)
            frames = re.findall(r'loop frame=\d+ url="([^"]*)"', output)
            assert frames and frames[-1] == base + '/watch/' + name, \
                (name, frames)
        print('404, 410, empty and ended playlists and an error page end the '
              'embed hop with STREAM OFFLINE OR NOT AVAILABLE and go Back')

        # An address declared as a file but answering a playlist is handed
        # to FFmpeg as HLS (its segment here is no transport stream, so
        # only the route and the words are checked); a real MP4 opens.
        output = hop('chanlive', 'pump 120 30\nmedia-status\nquit\n')
        assert 'kind=1 ' in output and 'route=hls' in output, output
        failure = re.search(
            r'loop-embed-hop status=open-failed error="([^"]*)"', output)
        assert failure is None or failure.group(1) == \
            'could not open HLS media', output
        assert 'loop-embed-hop status=unavailable' not in output, output
        for name in ('chanrangetext', 'chanrangeoctet'):
            output = hop(name, 'pump 120 30\nmedia-status\nquit\n')
            assert 'host-media-probe http=206 ' in output, (name, output)
            assert 'route=hls' in output, (name, output)
            assert 'loop-embed-hop status=unavailable' not in output, \
                (name, output)
        output = hop('chanmovie', 'pump 120 30\nmedia-status\nquit\n')
        assert 'host-media-probe http=206 ' in output, output
        assert 'route=mp4' in output, output
        assert 'loop-embed-hop status=opened' in output, output
        assert 'loop-embed-hop status=unavailable' not in output, output
        print('an extensionless playlist (whole, or ranged as text/plain or '
              'octet-stream) routes to HLS and an MP4 to MP4')
finally:
    server.shutdown()
    server.server_close()
    thread.join()
