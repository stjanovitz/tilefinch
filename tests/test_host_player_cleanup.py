#!/usr/bin/env python3
"""Exercise file and anonymous localhost playback without audible output."""
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

def assert_decoded_picture(path):
    picture = path.read_bytes().split(b'\n', 3)[-1]
    colors = {picture[(y*480+x)*3:(y*480+x)*3+3]
              for y in range(50, 190) for x in range(40, 440)}
    colorful = {color for color in colors if color[0] != color[1] or color[1] != color[2]}
    assert len(colorful) > 256, ('decoded picture is not visible beneath native controls',
                                len(colorful))

with tempfile.TemporaryDirectory(prefix="tilefinch-player-cleanup-") as directory:
    work = Path(directory)
    commands = work / "commands.txt"
    commands.write_text("media-play\nmedia-run 2 33\nmedia-seek 1\n"
                        "media-run 2 33\nmedia-seek 0\nmedia-close\nquit\n")
    environment = dict(os.environ, SDL_AUDIODRIVER="dummy")
    result = subprocess.run([
        str(lab), "--fixture", str(root / "benchmarks/fixtures/youtube-video-lab.html"),
        "--media-file", str(root / "tests/fixtures/psp-media/main-640x360.mp4"),
        "--no-javascript", "--psp-profile", "realistic", "--commands", str(commands),
        "--no-loop-capture", "--loop-output-dir", str(work), "--output", os.devnull,
    ], env=environment, capture_output=True, text=True, timeout=30)
    output = result.stdout + result.stderr
    assert result.returncode == 0, output
    assert re.search(r"teardown=0\b", output), output
    assert "interaction-latency command=media-seek" in output, output
    assert "interaction-latency command=media-close" in output, output
print("local playback, seeks and title-cache teardown passed")

class MediaHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path != '/clip.mp4':
            self.send_error(404)
            return
        body = (root / 'tests/fixtures/psp-media/main-640x360.mp4').read_bytes()
        self.send_response(200)
        self.send_header('Content-Type', 'video/mp4')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass

server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), MediaHandler)
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()
try:
    with tempfile.TemporaryDirectory(prefix='tilefinch-declared-player-') as directory:
        work = Path(directory)
        page = work / 'page.html'
        page.write_text('<!doctype html><meta property="og:video" content="http://127.0.0.1:'
                        + str(server.server_port) + '/clip.mp4"><title>Declared movie</title>'
                        '<meta property="og:image" content="data:image/png;base64,'
                        'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVQIHWP4z8DwHwAF'
                        'gAI/ScLbtAAAAABJRU5ErkJggg==">'
                        '<style>p{height:80px}'
                        'video{visibility:hidden!important}</style>'
                        '<a id="keep" href="/item">Keep focus</a>'
                        + '<p>Reading content</p>' * 8)
        commands = work / 'commands.txt'
        commands.write_text('focus-id keep\nscroll-by 64\nstatus\nrender '
                            + str(work/'before.ppm') + '\nplay-page-media\n'
                            'media-run 2 33\nrender '+str(work/'playing.ppm')+'\n'
                            'media-seek 1\nmedia-run 2 33\n'
                            'media-seek 0\nmedia-close\nmedia-open\nmedia-run 2 33\n'
                            'media-close\nstatus\nrender '
                            + str(work/'after.ppm') + '\nquit\n')
        result = subprocess.run([
            str(lab), '--fixture', str(page), '--no-javascript',
            '--psp-profile', 'realistic', '--commands', str(commands),
            '--no-loop-capture', '--loop-output-dir', str(work), '--output', os.devnull,
        ], env=environment, capture_output=True, text=True, timeout=30)
        output = result.stdout + result.stderr
        assert result.returncode == 0, output
        assert re.search(r'teardown=0\b.*status=PASS', output), output
        assert 'interaction-latency command=play-page-media' in output, output
        assert (work/'before.ppm').read_bytes() == (work/'after.ppm').read_bytes(), output
        assert_decoded_picture(work/'playing.ppm')
        states = re.findall(r'loop frame=.* scroll=([^ ]+) focus=([^ ]+) rect=([^ ]+)', output)
        assert states[0][1] == 'none:0', states
        assert states[-1] == states[3], states
        # Keep the normal page's exact-return comparison meaningful. Exercise
        # an invisible author tree separately: it must not hide native video.
        page.write_text(page.read_text().replace('<style>',
                                                '<style>body{opacity:0!important}'))
        commands.write_text('play-page-media\nmedia-run 2 33\nrender '
                            + str(work/'hidden-playing.ppm')
                            + '\nmedia-close\nquit\n')
        hidden = subprocess.run(result.args, env=environment, capture_output=True,
                                text=True, timeout=30)
        assert hidden.returncode == 0, hidden.stdout + hidden.stderr
        assert re.search(r'teardown=0\b.*status=PASS', hidden.stdout), hidden.stdout
        assert_decoded_picture(work/'hidden-playing.ppm')
finally:
    server.shutdown()
    server.server_close()
    thread.join()
print('declared page playback preserves pixels, scroll, focus and ownership')
