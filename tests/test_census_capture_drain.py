"""The site census capture's drain before quit (census.py capture
--drain-ms, lab `drain-fetches`), against a local server.

A capture's frames run far faster than a live network answers, so a request
still on the wire when the command script reaches `quit` is cancelled and
its trace record says "image pipeline aborted": a replay of that trace never
gets the response. The drain lets the in-flight requests finish, admitting
no new ones, and stops at its cap when one never does, saying which.
"""
import argparse
import http.server
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import threading
import time
import unittest
import zlib

LAB = Path(sys.argv.pop(1)).resolve()
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools/site-census"))
import census  # noqa: E402

# The images' bodies arrive a few bytes every DRIBBLE_S: steady progress (a
# request with no headers or bytes for a second or more is the engine's own
# no-progress cancellation, not this), over longer than the census frames
# take on a small page, so the image is in flight when the script ends.
DRIBBLE_S = 0.25
SLOW_CHUNK = 8      # the 4x4 PNG in about 3 s
STUCK_CHUNK = 1     # a padded body that outlasts STUCK_CAP_MS many times
STUCK_PADDING = 200
STUCK_CAP_MS = 1000


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


def page(image):
    # A script runtime leaves the <img> to the post-commit image queue,
    # which the census frames (lab "pump") run, as on most census pages.
    return ('<!doctype html><title>drain</title><script>var a = 1;</script>'
            '<p>Text.</p><img src="%s" width="4" height="4">' % image).encode()


PAGES = {'/slow.html': page('/slow.png'), '/stuck.html': page('/stuck.png')}


class CaptureDrainTest(unittest.TestCase):
    def setUp(self):
        release = self.release = threading.Event()

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                chunk = None
                if self.path in PAGES:
                    body, kind = PAGES[self.path], 'text/html; charset=utf-8'
                elif self.path == '/slow.png':
                    body, kind, chunk = png(), 'image/png', SLOW_CHUNK
                elif self.path == '/stuck.png':
                    body = png() + b'\0' * STUCK_PADDING
                    kind, chunk = 'image/png', STUCK_CHUNK
                else:
                    self.send_error(404)
                    return
                try:
                    self.send_response(200)
                    self.send_header('Content-Type', kind)
                    self.send_header('Content-Length', str(len(body)))
                    self.end_headers()
                    if chunk is None:
                        self.wfile.write(body)
                        return
                    self.wfile.flush()
                    for offset in range(0, len(body), chunk):
                        if release.wait(DRIBBLE_S):
                            return
                        self.wfile.write(body[offset:offset + chunk])
                        self.wfile.flush()
                except OSError:
                    pass  # the lab gave up on the request

            def log_message(self, *arguments):
                pass

        self.server = http.server.ThreadingHTTPServer(('127.0.0.1', 0),
                                                      Handler)
        self.thread = threading.Thread(target=self.server.serve_forever,
                                       daemon=True)
        self.thread.start()
        self.origin = 'http://127.0.0.1:%d' % self.server.server_address[1]
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)

    def tearDown(self):
        self.release.set()
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=5)
        self.directory.cleanup()

    def capture(self, name, path, drain_ms):
        sites = self.root / ('%s.tsv' % name)
        sites.write_text('%s\ttest\t%s%s\n' % (name, self.origin, path))
        corpus = self.root / 'corpus'
        args = argparse.Namespace(
            sites=str(sites), selectors=[], corpus=str(corpus),
            census=str(self.root), lab=str(LAB), force=True, retry=False,
            settle_pumps=0, idle=0, drain_ms=drain_ms, show_page_ads=False,
            user_css_after_load=False)
        started = time.monotonic()
        census.capture(args)
        elapsed = time.monotonic() - started
        page_dir = corpus / name
        meta = json.loads((page_dir / 'meta.json').read_text())
        return meta, self.image_record(page_dir / 'capture', path), elapsed

    @staticmethod
    def image_record(trace, page_path):
        image = page_path.replace('.html', '.png')
        records = []
        for meta in sorted(trace.glob('*.meta')):
            fields = dict(line.split('=', 1)
                          for line in meta.read_text().splitlines()
                          if '=' in line)
            if fields.get('url', '').endswith(image):
                records.append(fields)
        return records

    def test_script_drains_only_when_asked_and_just_before_quit(self):
        plain = census.command_script('F', False).splitlines()
        self.assertFalse(any(l.startswith('drain-fetches') for l in plain))
        drained = census.command_script('F', False, drain_ms=2500).splitlines()
        self.assertEqual(drained[-3:], ['drain-fetches 2500', 'status',
                                        'quit'])
        self.assertEqual(drained[:-3], plain[:-2])

    def test_quit_without_drain_records_the_image_as_aborted(self):
        # What the census captured before the drain: the image's record is
        # the cancellation at quit, so no replay of this trace can paint it.
        meta, records, _ = self.capture('nodrain', '/slow.html', 0)
        self.assertNotIn('drain', meta)
        self.assertEqual(len(records), 1, records)
        self.assertEqual(records[0]['success'], '0', records)
        self.assertEqual(records[0]['external-cancel'], '1', records)

    def test_drain_lets_the_in_flight_image_finish(self):
        meta, records, _ = self.capture('drained', '/slow.html', 15000)
        drain = meta['drain']
        self.assertTrue(drain['drained'], drain)
        self.assertEqual(drain['reason'], 'idle', drain)
        self.assertEqual(drain['clock'], 'live', drain)
        self.assertGreaterEqual(drain['at_start'], 1, drain)
        self.assertEqual(drain['in_flight'], 0, drain)
        self.assertEqual(drain['pending_urls'], [], drain)
        self.assertEqual(meta['drain_ms'], 15000, meta)
        self.assertTrue(meta['mode'].startswith('capture+drain'), meta)
        self.assertEqual(len(records), 1, records)
        self.assertEqual(records[0]['success'], '1', records)
        self.assertEqual(records[0]['status'], '200', records)

    def test_drain_stops_at_its_cap_and_names_what_is_pending(self):
        meta, records, elapsed = self.capture('stuck', '/stuck.html',
                                              STUCK_CAP_MS)
        drain = meta['drain']
        self.assertFalse(drain['drained'], drain)
        self.assertEqual(drain['reason'], 'cap', drain)
        self.assertGreaterEqual(drain['ms'], STUCK_CAP_MS, drain)
        self.assertEqual(drain['in_flight'], 1, drain)
        self.assertEqual(drain['resources'], 1, drain)
        self.assertEqual(drain['pending_urls'],
                         [self.origin + '/stuck.png'], drain)
        # Bounded: the cap, not the response, ended the run.
        self.assertLess(elapsed, (STUCK_PADDING + 80) * DRIBBLE_S / 2,
                        elapsed)
        self.assertEqual(len(records), 1, records)
        self.assertEqual(records[0]['success'], '0', records)


if __name__ == '__main__':
    unittest.main()
