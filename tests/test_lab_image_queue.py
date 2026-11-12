"""The host lab's report of the post-commit image queue.

A page with a script runtime does not load its parser-inserted <img>
elements during the commit: they are planned as weak targets and loaded by
post-commit work (the PSP loop's page-idle and deferred-image pumps, the
lab's `pump` and `drain`). The lab's `tick` advances only the page runtime,
so a tick-only run ends with images=0/0 although the page has images. The
`image-queue` line reports the queue so that run is not read as "no
images", and this test pins both halves against a local server: tick leaves
the image queued and unrequested, drain requests and loads it, and a page
without a runtime loads its image during the commit.
"""
import http.server
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import threading
import unittest
import zlib

LAB = Path(sys.argv.pop(1)).resolve()
QUEUE = re.compile(r'^image-queue queued=(\d+) batches=(\d+) pending=(yes|no)$',
                   re.M)
IMAGES = re.compile(r'^resources .* images=(\d+)/(\d+) ', re.M)


def png(width=4, height=4):
    def chunk(kind, data):
        body = kind + data
        return (struct.pack('>I', len(data)) + body
                + struct.pack('>I', zlib.crc32(body) & 0xffffffff))
    rows = b''.join(b'\x00' + b'\x80\x40\x20' * width for _ in range(height))
    return (b'\x89PNG\r\n\x1a\n'
            + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2,
                                         0, 0, 0))
            + chunk(b'IDAT', zlib.compress(rows))
            + chunk(b'IEND', b''))


PAGES = {
    '/script.html': b'<!doctype html><title>s</title><script>var a=1;</script>'
                    b'<img src="/a.png" width="4" height="4">',
    '/plain.html': b'<!doctype html><title>p</title>'
                   b'<img src="/b.png" width="4" height="4">',
}
IMAGE_BODIES = {'/a.png': png(), '/b.png': png()}


class ImageQueueLabTest(unittest.TestCase):
    def setUp(self):
        requests = self.requests = []

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                requests.append(self.path)
                if self.path in PAGES:
                    body, kind = PAGES[self.path], 'text/html; charset=utf-8'
                elif self.path in IMAGE_BODIES:
                    body, kind = IMAGE_BODIES[self.path], 'image/png'
                else:
                    self.send_error(404)
                    return
                self.send_response(200)
                self.send_header('Content-Type', kind)
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *arguments):
                pass

        self.server = http.server.ThreadingHTTPServer(('127.0.0.1', 0),
                                                      Handler)
        self.thread = threading.Thread(target=self.server.serve_forever,
                                       daemon=True)
        self.thread.start()
        self.origin = 'http://127.0.0.1:%d' % self.server.server_address[1]

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=5)

    def run_lab(self, path, commands):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'commands.txt').write_text(commands + 'quit\n')
            result = subprocess.run(
                [str(LAB), '--url', self.origin + path,
                 '--commands', str(root / 'commands.txt'),
                 '--no-loop-capture', '--output', str(root / 'final.ppm')],
                text=True, cwd=root, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout[-4000:])
        queue = QUEUE.search(result.stdout)
        images = IMAGES.search(result.stdout)
        self.assertIsNotNone(queue, result.stdout[-4000:])
        self.assertIsNotNone(images, result.stdout[-4000:])
        return ((int(queue.group(1)), int(queue.group(2)), queue.group(3)),
                (int(images.group(1)), int(images.group(2))))

    def test_tick_leaves_script_page_image_queued_and_unrequested(self):
        queue, images = self.run_lab('/script.html', 'tick 5 16\n')
        self.assertEqual(images, (0, 0))
        self.assertEqual(queue, (1, 0, 'yes'))
        self.assertNotIn('/a.png', self.requests)

    def test_drain_loads_the_queued_image(self):
        queue, images = self.run_lab('/script.html', 'drain 200 200\n')
        self.assertEqual(images, (1, 1))
        self.assertEqual(queue[0], 0)
        self.assertGreater(queue[1], 0)
        self.assertEqual(queue[2], 'no')
        self.assertIn('/a.png', self.requests)

    def test_page_without_runtime_loads_during_commit(self):
        queue, images = self.run_lab('/plain.html', 'tick 5 16\n')
        self.assertEqual(images, (1, 1))
        self.assertEqual(queue, (0, 0, 'no'))
        self.assertIn('/b.png', self.requests)


if __name__ == '__main__':
    unittest.main()
