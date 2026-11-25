"""Web conformance that needs the whole engine (navigation, style, layout).

Each test writes a small response-keyed replay into a temporary directory
and drives the host lab with `js` commands, whose values come back on the
`loop-js` line. Unit probes without layout live in
tests/test_web_platform_conformance.c.
"""
from pathlib import Path
import json
import os
import re
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

LAB = Path(sys.argv.pop(1)).resolve()
ORIGIN = 'https://conformance.test'
# The value is printed raw (up to 1024 bytes), so it may hold quotes.
JS = re.compile(r'^loop-js ok=(\w+) value="(.*)" error="([^"]*)"$', re.M)


def fnv1a(data):
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return value


def write_replay(root, pages):
    """pages: {path: (body, content type)}; writes one record per page.

    Version 10 records (as tests/test_content_blocker_pages.c writes them):
    older traces cannot vouch for their security headers, so their
    stylesheets and images fail closed."""
    root.mkdir()
    for index, (path, (body, content_type)) in enumerate(pages.items()):
        data = body if isinstance(body, bytes) else body.encode()
        url = ORIGIN + path
        empty = ('request-content-type request-extra-header-shape '
                 'request-if-none-match request-if-modified-since '
                 'request-referer request-origin request-accept '
                 'request-sec-fetch-dest request-sec-fetch-mode '
                 'request-sec-fetch-site request-client-hint-tokens '
                 'request-client-hint-origin request-user-agent '
                 'request-credential-origin request-initiator-url '
                 'request-referrer-source request-referrer-policy error '
                 'etag last-modified cf-mitigated accept-ch critical-ch '
                 'cf-ray response-referrer-policy').split()
        zero = ('async-delay-pumps external-cancel transport-timeout '
                'redirect-origin-tainted request-body-length '
                'request-cookie-bytes request-has-cf-clearance '
                'request-extra-header-bytes request-enforce-cors '
                'request-redirect-same-origin-only '
                'request-cors-cached-response-validated '
                'request-send-client-hints request-sec-fetch-user '
                'request-upgrade-insecure request-diagnostic-mobile-safari '
                'request-credentials response-referrer-policy-present '
                'response-security-headers-truncated '
                'set-cookie-count').split()
        lines = ['psp-http-trace=10', 'cookie-values=redacted',
                 'method=GET', 'url=' + url, 'logical-request-url=' + url,
                 'success=1', 'request-body-hash=cbf29ce484222325',
                 'request-allow-http-errors=1',
                 'request-send-low-client-hints=1', 'status=200',
                 'length=%d' % len(data),
                 'response-body-hash=%016x' % fnv1a(data),
                 'effective-url=' + url, 'content-type=' + content_type,
                 'server=fixture',
                 'response-referrer-policy-metadata-valid=1',
                 'response-header-count=1',
                 'response-header-0=content-type: ' + content_type]
        lines += [name + '=' for name in empty]
        lines += [name + '=0' for name in zero]
        (root / ('%04d.meta' % index)).write_text('\n'.join(lines) + '\n')
        (root / ('%04d.body' % index)).write_bytes(data)
    (root / 'trace.meta').write_text(
        'psp-http-trace-clock=1\norigin-ms=1700000000000\n'
        'capture-complete=yes\nrecord-count=%d\n' % len(pages))

def png(width, height, rgb):
    """A solid-color RGB PNG."""
    def chunk(kind, data):
        body = kind + data
        return (struct.pack('>I', len(data)) + body
                + struct.pack('>I', zlib.crc32(body) & 0xffffffff))
    rows = b''.join(b'\0' + bytes(rgb) * width for _ in range(height))
    return (b'\x89PNG\r\n\x1a\n'
            + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2,
                                         0, 0, 0))
            + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))


HTML = 'text/html; charset=utf-8'
# The lab's `js` command needs a page realm: give each page a script.
REALM = '<script>window.ready=1</script>'


class ConformanceLabTest(unittest.TestCase):
    def run_lab(self, pages, path, commands, extra=(), env=None):
        """The lab's output and the decoded value of each `js` command."""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            write_replay(root / 'replay', pages)
            (root / 'commands.txt').write_text(commands + 'quit\n')
            arguments = [str(LAB), '--url', ORIGIN + path,
                         '--psp-profile', 'realistic', '--fetch-scripts',
                         '--replay-http-response-keyed', str(root / 'replay'),
                         '--commands', str(root / 'commands.txt'),
                         '--no-loop-capture',
                         '--output', str(root / 'final.ppm'), *extra]
            result = subprocess.run(arguments, text=True, cwd=root,
                                    env=None if env is None
                                    else dict(os.environ, **env),
                                    stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout[-4000:])
        values = []
        for ok, value, error in JS.findall(result.stdout):
            self.assertEqual(ok, 'yes', error)
            values.append(value)
        return result.stdout, values

    def test_fractional_percentages_in_computed_style(self):
        # An element without a box reports its computed lengths.
        page = ('<!doctype html><body><div id=x style="display:none;'
                'position:absolute;width:33.333%;min-width:12.5%;'
                'max-height:0.25%;top:0.25%;left:-0.5%;flex-basis:62.5%">'
                '</div>' + REALM + '</body>')
        _, values = self.run_lab(
            {'/percent': (page, HTML)}, '/percent',
            'tick 3 33\njs (()=>{const c=getComputedStyle('
            'document.getElementById("x"));return [c.width,c.minWidth,'
            'c.maxHeight,c.top,c.left,c.flexBasis].join()})()\n')
        self.assertEqual(values, ['33.333%,12.5%,0.25%,0.25%,-0.5%,62.5%'])


    def test_calc_lengths_in_computed_style(self):
        # A calc() mixing a percentage and pixels comes back as calc();
        # one that is not linear (min()) is at least a length.
        page = ('<!doctype html><body><div id=x style="display:none;'
                'position:absolute;width:calc(100% - 20px);'
                'top:calc(50% + 4px);height:calc(10px + 2em);'
                'min-width:min(50%, 100px)"></div>' + REALM + '</body>')
        _, values = self.run_lab(
            {'/calc': (page, HTML)}, '/calc',
            'tick 3 33\njs (()=>{const c=getComputedStyle('
            'document.getElementById("x"));return [c.width,c.top,'
            'c.height,c.minWidth].join(";")})()\n')
        width, top, height, minimum = values[0].split(';')
        self.assertEqual((width, top, height),
                         ('calc(100% - 20px)', 'calc(50% + 4px)', '42px'))
        self.assertRegex(minimum, r'^(-?\d+px|min\(.*\))$')

    def test_frames_load_stylesheets_and_images(self):
        # A child frame's linked stylesheet styles it and its image loads,
        # as in the top-level page. The child reports what it sees.
        child = ('<!doctype html><link rel=stylesheet href=/frame.css>'
                 '<body><div id=b>x</div><img id=i src=/dot.png><script>'
                 'setTimeout(function(){parent.postMessage(['
                 'getComputedStyle(document.body).backgroundColor,'
                 'b.getBoundingClientRect().width,i.naturalWidth,'
                 'i.naturalHeight].join(),"*")},50)</script></body>')
        pages = {
            '/top': ('<!doctype html><body><iframe src=/child width=300 '
                     'height=150></iframe><script>addEventListener('
                     '"message",function(e){window.R=e.data})</script>'
                     '</body>', HTML),
            '/child': (child, HTML),
            '/frame.css': ('body{background:rgb(1,2,3)}#b{width:123px}',
                           'text/css'),
            '/dot.png': (png(7, 5, (200, 10, 10)), 'image/png'),
        }
        _, values = self.run_lab(pages, '/top', 'tick 20 33\njs String(window.R)\n')
        self.assertEqual(values, ['rgb(1, 2, 3),123,7,5'])
    def test_post_message_requires_a_message(self):
        # postMessage() with no arguments throws TypeError from a child
        # frame's parent and from the parent's view of the frame.
        child = ('<!doctype html><body><script>var a;try{parent.'
                 'postMessage();a="no throw"}catch(e){a=e.name}'
                 'parent.postMessage(a,"*")</script></body>')
        pages = {
            '/top': ('<!doctype html><body><iframe src=/child></iframe>'
                     '<script>addEventListener("message",function(e){'
                     'var b;try{frames[0].postMessage();b="no throw"}'
                     'catch(x){b=x.name}window.R=e.data+","+b})</script>'
                     '</body>', HTML),
            '/child': (child, HTML),
        }
        _, values = self.run_lab(pages, '/top',
                                 'tick 20 33\njs String(window.R)\n')
        self.assertEqual(values, ['TypeError,TypeError'])

    def test_device_media_features_read_the_screen(self):
        # Without a meta viewport the layout is wider than the screen; the
        # device-* features (in CSS and matchMedia) read the screen.
        page = ('<!doctype html><style>#x{width:5px}'
                '@media (device-width: 480px) and (max-device-height: 272px)'
                '{#x{width:7px}}</style><body><div id=x></div>'
                + REALM + '</body>')
        _, values = self.run_lab(
            {'/device': (page, HTML)}, '/device',
            'tick 3 33\njs [innerWidth>screen.width,screen.width,'
            'x.getBoundingClientRect().width,matchMedia("(device-width: '
            '"+screen.width+"px)").matches].join()\n')
        self.assertEqual(values, ['true,480,7,true'])

    def test_percentage_height_of_auto_block_in_estimate(self):
        # A node measured before it has a box gets an estimate. A
        # percentage height against an auto-height containing block is
        # auto, in the estimate as in the page's later layout.
        page = ('<!doctype html><body><p>text</p><script>var R={};'
                'function g(){var a=document.createElement("div"),'
                'd=document.createElement("div");d.style.height="500%";'
                'a.textContent=d.textContent="x";document.body.append(a,d);'
                'var r=[a.getBoundingClientRect().height,'
                'd.getBoundingClientRect().height];a.remove();d.remove();'
                'return r.join()}document.addEventListener('
                '"DOMContentLoaded",function(){R.early=g()});'
                'setTimeout(function(){R.later=g()},50)</script></body>')
        _, values = self.run_lab({'/height': (page, HTML)}, '/height',
                                 'tick 10 33\njs JSON.stringify(R)\n')
        results = json.loads(values[0])
        for phase in ('early', 'later'):
            auto, percent = (float(v) for v in results[phase].split(','))
            self.assertGreater(auto, 0, phase)
            self.assertEqual(percent, auto, phase)

    def test_load_handlers_read_the_layout(self):
        # DOMContentLoaded and load handlers measure what a later timer
        # does: the page's layout, not an estimate. Also measure a node
        # added in the handler, including when its CSS box overflows.
        page = ('<!doctype html><body><div id=box style="width:100px;'
                'height:20px">x</div><p id=para>A paragraph of text</p>'
                '<script>var R={};function g(){'
                'var r=box.getBoundingClientRect(),'
                'p=para.getBoundingClientRect(),'
                'd=document.createElement("div");'
                'd.style.cssText="width:33.333%;height:10px";'
                'document.body.appendChild(d);'
                'var w=d.getBoundingClientRect().width;'
                'd.style.width="500%";R.wide=d.getBoundingClientRect().width;'
                'd.style.cssText="width:1800px;height:1500px";'
                'var big=d.getBoundingClientRect();R.box=[big.width,big.height];'
                'd.remove();'
                'return [r.left,r.top,r.width,r.height,p.left,p.top,'
                'p.width,p.height,w].join()}'
                'document.addEventListener("DOMContentLoaded",'
                'function(){R.dcl=g();R.dclWide=R.wide;R.dclBox=R.box});'
                'addEventListener("load",function(){R.load=g()});'
                'setTimeout(function(){R.later=g()},50)</script></body>')
        for pending in (False, True):
            with self.subTest(pending_load=pending):
                _, values = self.run_lab(
                    {'/load': (page, HTML)}, '/load',
                    'tick 10 33\njs JSON.stringify(R)\n',
                    ['--pending-load'] if pending else [])
                results = json.loads(values[0])
                fields = [float(v) for v in results['later'].split(',')]
                for phase in ('dcl', 'load'):
                    got = [float(v) for v in results[phase].split(',')]
                    self.assertEqual(got[:8], fields[:8], phase)
                    self.assertAlmostEqual(got[8], fields[6] / 3, delta=1)
                self.assertAlmostEqual(results['dclWide'], fields[6] * 5, delta=1)
                self.assertEqual(results['dclBox'], [1800, 1500])

    def test_load_handlers_refresh_geometry_after_mutation(self):
        page = ('<!doctype html><body><div id=box style="width:100px;'
                'height:20px"></div><script>var R=[];'
                'function measure(){R.push(box.getBoundingClientRect().width)}'
                'document.addEventListener("DOMContentLoaded",function(){'
                'measure();box.style.width="180px";measure()});'
                'addEventListener("load",function(){measure();'
                'box.style.width="220px";measure()});'
                'setTimeout(measure,50)</script></body>')
        for pending in (False, True):
            with self.subTest(pending_load=pending):
                _, values = self.run_lab(
                    {'/load': (page, HTML)}, '/load',
                    'tick 10 33\njs JSON.stringify(R)\n',
                    ['--pending-load'] if pending else [])
                self.assertEqual(json.loads(values[0]), [100, 180, 180, 220, 220])

    # Load-time layout allowance (STYLE_INVALIDATION.md, "Reads during
    # page load"): a first layout only when its estimated work is at most
    # the floor, then a share of its work. A body estimated well under the
    # floor, and one well over it.
    LOAD_WORK_FLOOR = 16384
    MEDIUM_BODY = '<div id=filler>' + '<p>Item text</p>' * 300 + '</div>'
    BIG_BODY = '<div id=filler>' + '<p>Item text</p>' * 3000 + '</div>'

    @staticmethod
    def snapshot_builds(output):
        match = re.search(r'^javascript-geometry .* snapshot-builds=(\d+)',
                          output, re.M)
        return None if match is None else int(match[1])

    def test_load_geometry_work_is_lazy_and_bounded(self):
        # A listener that never measures doesn't build a scratch layout.
        # Repeated writes followed by reads lay the page out again only
        # while the work of those layouts stays within the allowance the
        # first layout set (the last may run over by its own work).
        for measure in (False, True):
            with self.subTest(measure=measure):
                read = 'box.getBoundingClientRect();' if measure else ''
                page = ('<!doctype html><body><div id=box></div>'
                        + self.MEDIUM_BODY + '<script>'
                        'addEventListener("load",function(){'
                        'for(var i=0;i<24;i++){box.style.width=i+"px";'
                        + read + '}})</script></body>')
                output, _ = self.run_lab(
                    {'/load': (page, HTML)}, '/load', '',
                    env={'TILEFINCH_TRACE_RUNTIME_STEPS': '1'})
                builds = self.snapshot_builds(output)
                self.assertIsNotNone(builds)
                if not measure:
                    self.assertEqual(builds, 0)
                    continue
                self.assertGreaterEqual(builds, 1)
                self.assertLessEqual(builds, 3)
                work = [int(w) for w in re.findall(
                    r'^geometry-snapshot .* work=(\d+) loading=1$',
                    output, re.M)]
                self.assertEqual(len(work), builds)
                allowance = max(work[0] // 2, self.LOAD_WORK_FLOOR)
                self.assertLessEqual(sum(work[1:]),
                                     allowance + max(work[1:], default=0))

    def load_read_builds(self, body, prepare='', env=None):
        """Snapshot builds of a page whose DOMContentLoaded handler runs
        `prepare`, then reads #box (123x7 by its style); asserts the read."""
        page = ('<!doctype html><body><div id=box style="width:123px;'
                'height:7px"></div><div id=app></div>' + body
                + '<script>var R={};'
                'document.addEventListener("DOMContentLoaded",function(){'
                + prepare + 'var r=box.getBoundingClientRect();'
                'R.box=[r.width,r.height]})</script></body>')
        output, values = self.run_lab(
            {'/load': (page, HTML)}, '/load', 'tick 10 33\njs JSON.stringify(R)\n',
            env=dict({'TILEFINCH_TRACE_RUNTIME_STEPS': '1'}, **(env or {})))
        self.assertEqual(json.loads(values[0])['box'], [123, 7])
        return self.snapshot_builds(output)

    def test_load_first_layout_is_admitted_by_estimate(self):
        # A handler's read on a small page gets a real layout, also with the
        # JS profiler's trampolines around the natives; on a page whose
        # first layout is estimated over the floor it builds none and
        # answers from the style estimate, also when the handler built the
        # page's tree just before its read.
        build = ('for(var i=0;i<3000;i++)'
                 'app.appendChild(document.createElement("p")).textContent='
                 '"Item text";')
        for name, body, prepare, env, builds in (
                ('small', '', '', None, 1),
                ('small-profiled', '', '', {'TILEFINCH_TRACE_JS_PROFILE': '1'}, 1),
                ('big', self.BIG_BODY, '', None, 0),
                ('script-built', '', build, None, 0)):
            with self.subTest(page=name):
                self.assertEqual(self.load_read_builds(body, prepare, env),
                                 builds)

    def test_load_reads_follow_writes_past_the_allowance(self):
        # Past the load allowance a read answers from the style estimate,
        # never from a layout the page has since changed: a width read back
        # after each write matches it, and a fit-to-width loop ends.
        page = ('<!doctype html><body><div id=box style="width:200px;'
                'height:20px"></div><div id=fit style="width:300px;'
                'height:20px"></div>' + self.MEDIUM_BODY + '<script>var R={};'
                'document.addEventListener("DOMContentLoaded",function(){'
                'var seq=[];for(var i=0;i<16;i++){'
                'box.style.width=(200+10*i)+"px";'
                'seq.push(box.getBoundingClientRect().width)}R.seq=seq.join();'
                'var w=300,steps=0;'
                'while(fit.getBoundingClientRect().width>100&&steps<100){'
                'w-=10;fit.style.width=w+"px";steps++}'
                'R.fit=[steps,fit.getBoundingClientRect().width].join()})'
                '</script></body>')
        expected = ','.join(str(200 + 10 * i) for i in range(16))
        for pending in (False, True):
            with self.subTest(pending_load=pending):
                _, values = self.run_lab(
                    {'/load': (page, HTML)}, '/load',
                    'tick 10 33\njs JSON.stringify(R)\n',
                    ['--pending-load'] if pending else [])
                results = json.loads(values[0])
                self.assertEqual((results['seq'], results['fit']),
                                 (expected, '20,100'))


if __name__ == '__main__':
    unittest.main()
