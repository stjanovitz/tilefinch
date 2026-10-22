#!/usr/bin/env python3
"""Offline browser regression: redirects, lazy images, capture identity.

No server or network is required. Playwright is optional in public clones;
maintainers run this lane with their installed browser before capture changes.
"""
import json
import hashlib
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "benchmarks"))
from visual_scenario import trace_digest, load_manifest, validate_state
from reference_frame import read_png

NODE = shutil.which("node")
PLAYWRIGHT = NODE and subprocess.run(
    [NODE, "-e", "require('playwright')"], capture_output=True).returncode == 0
UA = "Mozilla/5.0 (PlayStation Portable; Tilefinch/0.1)"
spec = importlib.util.spec_from_file_location("capture_census", ROOT / "benchmarks/capture-census.py")
census = importlib.util.module_from_spec(spec)
spec.loader.exec_module(census)


class CaptureReceiptTests(unittest.TestCase):
    def test_cosmetic_receipt_requires_exact_shared_stylesheet(self):
        state = self.complete_state()
        proof = dict(version='shared-cosmetic-v1', ready=True, enabled=True,
            rules_sha256=hashlib.sha256((ROOT / 'src/cosmetic_blocker_css.inc').read_bytes()).hexdigest())
        state['offline_capability_policy'] = dict(cosmetic_blocking=proof)
        self.assertTrue(census.complete_state(state))
        for key, value in [('ready', False), ('enabled', 1), ('rules_sha256', '0'*64),
                           ('version', 'unshared')]:
            original = proof[key]
            proof[key] = value
            self.assertFalse(census.complete_state(state))
            proof[key] = original

    def test_inline_tracker_blocking_receipt_requires_exact_bounded_evidence(self):
        state = self.complete_state()
        proof = dict(version='shared-inline-trackers-v1', ready=True, blocked=1, enabled=True,
            rules_sha256=hashlib.sha256((ROOT / 'src/inline_tracker_rules.inc').read_bytes()).hexdigest(),
            records=[dict(rule='retail-client-metrics-v1', source_sha256='a'*64)])
        state['offline_capability_policy'] = dict(inline_trackers=proof)
        self.assertTrue(census.complete_state(state))
        for key, value in [('ready', False), ('blocked', 2), ('rules_sha256', 'b'*64),
                           ('records', [dict(rule='unknown', source_sha256='a'*64)])]:
            original = proof[key]
            proof[key] = value
            self.assertFalse(census.complete_state(state))
            proof[key] = original
    def test_challenge_preflight_never_launches_browser_or_claims_completion(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            trace = root / 'trace'
            trace.mkdir()
            (trace / 'trace.meta').write_text('capture-complete=yes\nrecord-count=1\n')
            (trace / '0000.meta').write_text('method=GET\nurl=https://page.test/\n'
                'status=202\nresponse-header-0=x-amzn-waf-action: challenge\n')
            (trace / '0000.body').write_bytes(b'')
            scenario = SimpleNamespace(scenario='barrier', replay_dir='trace',
                trace_sha256=trace_digest(trace), url='https://page.test/')
            options = SimpleNamespace(output_root=root / 'output', trace_root=root,
                settle_ms=0, timeout_ms=1000, attempt_timeout_seconds=30,
                resume=False, manifest=root / 'manifest.tsv', node=Path('node'), executable=None)
            with patch.object(census, 'run_bounded', side_effect=AssertionError('browser launched')):
                receipt = census.capture_one(scenario, options, {'files': {}})
            self.assertEqual(receipt['status'], 'incomplete')
            self.assertEqual(receipt['reasons'], ['source-access-challenge'])
            self.assertEqual(receipt['artifacts'], {})

    def test_source_access_barriers_are_not_geometry_or_resource_failures(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            meta = root / '0000.meta'
            prefix = 'method=GET\nurl=https://page.test/article\n'
            for header in ('x-amzn-waf-action: challenge', 'CF-Mitigated: Challenge'):
                meta.write_text(prefix + 'status=202\nlength=0\nresponse-header-0=' + header + '\n')
                self.assertEqual(census.source_response_barrier(root, 'https://page.test/article'),
                                 'source-access-challenge')
            meta.write_text(prefix + 'status=302\nresponse-header-0=location: /login/?dest=private\n')
            self.assertEqual(census.source_response_barrier(root, 'https://page.test/article'),
                             'source-signin-required')
            for response in ('status=202\n', 'status=302\nresponse-header-0=location: /news/\n',
                             'status=200\nresponse-header-0=location: /login/\n'):
                meta.write_text(prefix + response)
                self.assertIsNone(census.source_response_barrier(root, 'https://page.test/article'))
            meta.write_text(prefix + 'status=202\nresponse-header-0=x-amzn-waf-action: challenge\n')
            self.assertIsNone(census.source_response_barrier(root, 'https://page.test/other'))

    @staticmethod
    def complete_state():
        return {"capture_ready": True,
                "browser_request_audit": {"ready": True, "observed": 1, "intercepted": 1,
                    "pending": 0, "overflow": False, "failures": []},
                "target_interception": {"ready": True, "pending_commands": 0,
                    "failures": 0, "overflow": False},
                "full_document": {"version": "bounded-viewport-sweep-v2", "complete": True,
                    "full_screenshot": False, "full_screenshot_status": "omitted-mobile-emulation",
                    "viewport_css": {"width": 480, "height": 272},
                    "viewports": [{"fraction": p / 100, "frame": f"view-{p}.png", "ready": True,
                        "incomplete": 0, "truncated": False, "document_width": 480,
                        "document_height": 272, "maximum": 0, "scroll_y": 0, "requested": 0,
                        "input_profile": {point: {"coarse": True, "touch": 1, "width": 480, "height": 272}
                                          for point in ("before", "after")}}
                        for p in (0, 25, 50, 75, 100)],
                    "final_extent": {"width": 480, "height": 272, "maximum": 0,
                        "incomplete": 0, "ready": True},
                    },
                "checkpoints": [{"frame": "middle.png"}]}

    def test_malformed_state_and_receipts_are_not_resumable(self):
        for value in (None, [], {"capture_ready": True, "browser_request_audit": []},
                      {"capture_ready": True, "browser_request_audit": {"ready": True}, "full_document": []}):
            self.assertFalse(census.complete_state(value))
            self.assertFalse(census.receipt_valid(value, {}, Path("/tmp")))

    def test_receipt_accounts_only_proven_denied_preflight_parents(self):
        state = self.complete_state()
        audit = state['browser_request_audit']
        audit.update(version='browser-request-audit-v3', observed=2,
            preflight_denied_parents=1, failures=[dict(expected=True,
                reason='denied-preflight-parent', method='GET', type='fetch',
                error='net::ERR_BLOCKED_BY_CLIENT')])
        self.assertTrue(census.complete_state(state))
        for field, value in [('preflight_denied_parents', True),
                             ('preflight_denied_parents', -1),
                             ('preflight_denied_parents', 2),
                             ('observed', 3), ('intercepted', True),
                             ('version', 'browser-request-audit-v2'),
                             ('ready', False), ('pending', 1), ('overflow', True)]:
            original = audit[field]
            audit[field] = value
            self.assertFalse(census.complete_state(state), (field, value))
            audit[field] = original
        failure = audit['failures'][0]
        for field, value in [('reason', 'declared-policy'), ('expected', False),
                             ('method', 'POST'), ('error', 'net::ERR_FAILED')]:
            original = failure[field]
            failure[field] = value
            self.assertFalse(census.complete_state(state), (field, value))
            failure[field] = original
        audit['failures'] = []
        self.assertFalse(census.complete_state(state))

    def test_whole_attempt_timeout_is_terminal(self):
        with tempfile.TemporaryFile() as log:
            self.assertEqual(census.run_bounded([sys.executable, "-c", "import time;time.sleep(60)"],
                                               log, log, .1), 124)
    def test_resume_requires_state_and_every_mandatory_artifact(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            attempt = root / "attempt-1"
            output = attempt / "fixture"
            output.mkdir(parents=True)
            state = self.complete_state()
            (output / "reference-state.json").write_text(json.dumps(state))
            paths = [attempt / "stdout.log", attempt / "stderr.log"]
            paths.extend(output / name for name in ("view-0.png", "view-25.png",
                "view-50.png", "view-75.png", "view-100.png", "top.png", "bottom.png", "middle.png"))
            for path in paths:
                path.write_bytes(b"receipt fixture")
            paths.append(output / "reference-state.json")
            audit = output / "capture-audit.json"
            audit.write_text(json.dumps({"state_sha256": census.file_digest(output / "reference-state.json")}))
            paths.append(audit)
            identity = {"trace": "pinned"}
            receipt = {"identity": identity, "status": "complete", "exit": 0,
                       "attempt": str(attempt), "output": str(output), "scenario": "fixture",
                       "artifacts": {str(path.relative_to(root)): census.file_digest(path) for path in paths}}
            self.assertTrue(census.receipt_valid(receipt, identity, root))
            self.assertFalse(census.receipt_valid(receipt, {"trace": "changed"}, root))
            saved = receipt["artifacts"].pop("attempt-1/fixture/middle.png")
            self.assertFalse(census.receipt_valid(receipt, identity, root))
            receipt["artifacts"]["attempt-1/fixture/middle.png"] = saved
            (output / "middle.png").write_bytes(b"corrupt")
            self.assertFalse(census.receipt_valid(receipt, identity, root))

    def test_resume_refuses_missing_or_mixed_geometry_evidence(self):
        self.assertTrue(census.complete_state(self.complete_state()))
        for field in ("viewports", "final_extent", "viewport_css", "version"):
            state = self.complete_state()
            del state["full_document"][field]
            self.assertFalse(census.complete_state(state))
        state = self.complete_state()
        state["full_document"]["viewports"][2]["document_height"] = 500
        self.assertFalse(census.complete_state(state))
        state = self.complete_state()
        state["full_document"]["viewports"][1]["fraction"] = .75
        self.assertFalse(census.complete_state(state))
        state = self.complete_state()
        state["checkpoints"] = []
        self.assertFalse(census.complete_state(state))
        state = self.complete_state()
        state["full_document"]["viewports"][2]["input_profile"]["after"]["coarse"] = False
        self.assertFalse(census.complete_state(state))
        state = self.complete_state()
        state["full_document"]["full_screenshot"] = True
        self.assertFalse(census.complete_state(state))

    def test_long_discussion_receipt_requires_all_views_and_bounded_extent(self):
        state = self.complete_state()
        full = state['full_document']
        for view in full['viewports']:
            view.update(document_height=200000, maximum=199728,
                        scroll_y=int(199728 * view['fraction']),
                        requested=int(199728 * view['fraction']))
        full['final_extent'].update(height=200000, maximum=199728)
        self.assertTrue(census.complete_state(state))
        full['viewports'][2]['document_height'] = 199999
        self.assertFalse(census.complete_state(state))
        for view in full['viewports']:
            view.update(document_height=262145, maximum=261873,
                        scroll_y=int(261873 * view['fraction']),
                        requested=int(261873 * view['fraction']))
        full['final_extent'].update(height=262145, maximum=261873)
        self.assertFalse(census.complete_state(state))

    def test_geometry_retry_requires_preserved_screenshots(self):
        state = {"checkpoints": [], "full_document": {"geometry_attempts": [{
            "directory": "geometry-attempt-1", "views": [
                {"frame": f"view-{p}.png"} for p in (0, 25, 50, 75, 100)]}]}}
        paths = census.required_artifacts(state, Path("attempt"), Path("attempt/site"))
        self.assertEqual(sum("geometry-attempt-1" in str(path) for path in paths), 5)
        state["full_document"]["geometry_attempts"][0]["directory"] = "../outside"
        with self.assertRaises(ValueError):
            census.required_artifacts(state, Path("attempt"), Path("attempt/site"))

    def test_receipt_cannot_escape_output_root(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.assertFalse(census.receipt_valid({"identity": {}, "status": "complete", "exit": 0,
                "attempt": str(root.parent), "output": str(root.parent / "fixture"),
                "scenario": "fixture", "artifacts": {"../outside": "digest"}}, {}, root))


def record(trace, number, url, body, content_type, status=200, headers=()):
    stem = trace / f"{number:04d}"
    stem.with_suffix(".body").write_bytes(body)
    fields = ["method=GET", f"url={url}", "success=1", f"status={status}",
              f"length={len(body)}", f"content-type={content_type}",
              f"request-user-agent={UA}", "set-cookie-count=0",
              f"response-header-count={len(headers)}"]
    fields.extend(f"response-header-{index}={header}" for index, header in enumerate(headers))
    stem.with_suffix(".meta").write_text("\n".join(fields) + "\n")


@unittest.skipUnless(PLAYWRIGHT, "Node/Playwright is unavailable")
class CaptureFixtureBase(unittest.TestCase):
    def capture(self, directory, *, missing=False, full=True, iframe=False, viewport_units=False, corrupt_frame_image=False, hidden_frame=False, pointer_media=False, long_document=False, scroll_window=0, distant_image=False, query_mode="", dismiss=False, wide_layout=False, mobile_auto_fit=False, shadow_marker=False, hidden_shadow_marker=False, shadow_image="", internal_scroll="", worker_mode="", fragment=False):
        trace = directory / "trace"
        trace.mkdir()
        # Native lazy loading requires the scroll sweep; the hidden broken
        # image must not falsely fail visual qualification.
        html = b'''<!doctype html><meta name="viewport" content="width=device-width">
<title>Capture fixture</title><link rel="icon" href="/favicon.svg">
<style>body{margin:0}img{width:32px;height:32px}</style>
<script>if(!matchMedia('(pointer:coarse)').matches||navigator.maxTouchPoints!==1)
document.title='Capture fixture invalid input';</script>
<h1>Fixture ready</h1><img src="/redirect.svg"><div style="height:800px"></div>
<img loading="lazy" src="/lazy.svg"><img hidden src="/hidden.svg">'''
        if long_document:
            html += b'<div style="height:65000px"></div><p>Discussion end</p>'
        if fragment:
            html += b"<script>if(location.hash!=='#initial-state')document.title='Wrong fragment';</script>"
        if wide_layout:
            html += b'''<style>body{width:494px}#edge{position:fixed;bottom:0;left:0;width:100%;height:4px;background:#00ff00;z-index:999}</style><div id=edge></div>'''
        if shadow_marker:
            html = html.replace(b'<h1>Fixture ready</h1>',
                b'<div id=shadow></div><script>document.getElementById("shadow").attachShadow({mode:"open"}).innerHTML="<h1>Fixture ready</h1>"</script>')
            if hidden_shadow_marker:
                html += b'<style>#shadow{display:none}</style>'
        if shadow_image:
            html += b'''<div id=shadowimages></div><script>document.getElementById('shadowimages').attachShadow({mode:'open'}).innerHTML='<img width=32 height=32 src="/shadow-broken.svg">'</script>'''
            if shadow_image == 'hidden':
                html += b'<style>#shadowimages{display:none}</style>'
        if distant_image:
            html += b'<img loading="lazy" src="/outside-window.svg">'
        if dismiss:
            html+=b'''<div style="position:fixed;inset:0;background:white;z-index:9"><button onclick="document.querySelector('h1').textContent='Action completed';this.parentElement.remove()">Keep blocking</button></div>'''
        if viewport_units:
            html += (b'<div style="height:100vh;background:#00ff00"></div>'
                     b'<div style="height:16px;background:#0000ff"></div>')
        if pointer_media:
            html += (b'<style>#pointer{height:16px;background:#123456}'
                     b'@media(pointer:coarse){#pointer{background:#ff00ff}}</style>'
                     b'<div id="pointer"></div>')
        if internal_scroll:
            style = 'height:232px;overflow-y:auto'
            if internal_scroll == 'hidden': style += ';display:none'
            if internal_scroll == 'static': style = 'height:232px;overflow-y:visible'
            html = html.replace(b'<h1>', ('<header style="height:40px">Fixed header</header>'
                '<main id="scrollsurface" style="'+style+'"><h1>').encode(), 1) + b'</main>'
            if internal_scroll == 'duplicate':
                html += b'<main id="scrollsurface" style="height:30px;overflow-y:auto">duplicate</main>'
        if worker_mode:
            html += b'''<script>const w=new Worker('/worker.js');w.onmessage=e=>document.querySelector('h1').textContent=e.data;</script>'''
        query_body=json.dumps({'query':'query Read { public { id } }','variables':{}},separators=(',',':'))
        if query_mode:
            sent=query_body if query_mode!='different' else query_body.replace('public','other')
            if query_mode=='oversized':sent=json.dumps({'query':'{public{id}}','variables':{'padding':'x'*16384}})
            script="const done=value=>{const p=document.createElement('p');p.textContent=JSON.parse(value).message;document.body.append(p);};"
            if query_mode=='xhr':
                script+="const x=new XMLHttpRequest();x.open('POST','/api/query');x.setRequestHeader('Content-Type','application/json');x.onload=()=>done(x.responseText);x.send("+json.dumps(sent)+");"
            else:
                script+="fetch('/api/query',{method:'POST',headers:{'Content-Type':'application/json'},body:"+json.dumps(sent)+"}).then(r=>r.text()).then(done).catch(()=>{});"
            html+=('<script>'+script+'</script>').encode()
            if query_mode.startswith('socket-'):
                html+=b"<script>try{new WebSocket('wss://capture.test/live-updates')}catch(_){}</script>"
        svg = b'<svg xmlns="http://www.w3.org/2000/svg" width="32" height="32"><rect width="32" height="32" fill="red"/></svg>'
        records = [("https://capture.test/", html, "text/html", 200, ()),
                   ("https://capture.test/redirect.svg", b"", "text/plain", 302,
                    ("Location: https://images.test/final.svg",)),
                   ("https://capture.test/lazy.svg", svg, "image/svg+xml", 200, ()),
                   ("https://capture.test/favicon.svg", svg, "image/svg+xml", 200, ()),
                   ("https://capture.test/hidden.svg", svg, "image/svg+xml", 200, ())]
        if iframe:
            # Different registrable domains force Chromium's separate frame
            # target; the nested redirect must still remain fully offline.
            frame_style = b' style="display:none"' if hidden_frame else b''
            records[0] = ("https://capture.test/", html + b'<iframe' + frame_style + b' src="https://frame.example/frame"></iframe>',
                          "text/html", 200, ())
            records.extend([
                ("https://frame.example/frame", b'<img width="32" height="32" src="/redirect.svg">', "text/html", 200, ()),
                ("https://frame.example/redirect.svg", b"", "text/plain", 302,
                 ("Location: https://nested.example/final.svg",)),
                ("https://nested.example/final.svg", b"not an image" if corrupt_frame_image else svg,
                 "image/svg+xml", 200, ())])
        if not missing:
            records.append(("https://images.test/final.svg", svg, "image/svg+xml", 200, ()))
        if query_mode:
            records.append(("https://capture.test/api/query",b'{"message":"Query ready"}',"application/json",200,()))
        if shadow_image:
            records.append(("https://capture.test/shadow-broken.svg", b'not an image', "image/svg+xml", 200, ()))
        if worker_mode:
            source = b"setTimeout(()=>postMessage('Worker ready'),10);"
            if worker_mode == 'fetch': source = b"fetch('/worker-data').then(r=>r.text()).then(postMessage);"
            if worker_mode == 'import': source = b"importScripts('/worker-extra.js');"
            if worker_mode == 'write': source = b"fetch('/worker-data',{method:'POST',body:'write'}).catch(()=>{});postMessage('Worker ready');"
            if worker_mode == 'socket': source = b"try{new WebSocket('wss://capture.test/socket')}catch(_){};postMessage('Worker ready');"
            if worker_mode == 'pending': source = b"setTimeout(()=>postMessage('later'),100000);postMessage('Worker ready');"
            if worker_mode == 'unknown': source = b"fetch('/not-retained').catch(()=>postMessage('Worker ready'));"
            if worker_mode == 'quota': source = b"const ids=[];for(let i=0;i<256;i++)ids.push(setTimeout(()=>{},100000));try{setTimeout(()=>{},100000)}catch(_){};ids.forEach(clearTimeout);postMessage('Worker ready');"
            source = b"if(!globalThis.__tilefinchReplayEnvironment)throw new Error('worker author code escaped initialization');" + source
            if worker_mode == 'blob':
                html = html.replace(b"new Worker('/worker.js')", ("new Worker(URL.createObjectURL(new Blob(["+json.dumps(source.decode())+"],{type:'text/javascript'})))").encode())
                records[0] = ("https://capture.test/", html, "text/html", 200, ())
            records.append(("https://capture.test/worker.js", source, "text/javascript", 200, ()))
            records.append(("https://capture.test/worker-data", b'Worker ready', "text/plain", 200, ()))
            if worker_mode == 'import': records.append(("https://capture.test/worker-extra.js", b"postMessage('Worker ready');", "text/javascript", 200, ()))
        for index, values in enumerate(records):
            record(trace, index, *values)
        if query_mode:
            stem=trace/f'{len(records)-1:04d}';body=query_body.encode();hash_value=0xcbf29ce484222325
            for byte in body:hash_value=((hash_value^byte)*0x100000001b3)&0xffffffffffffffff
            stem.with_suffix('.request').write_bytes(body)
            p=stem.with_suffix('.meta');p.write_text(p.read_text().replace('method=GET','method=POST')+
                f'request-body-length={len(body)}\nrequest-body-hash={hash_value:016x}\nrequest-cookie-bytes=0\nrequest-credentials=0\n')
        (trace / "trace.meta").write_text(
            f"psp-http-trace-clock=1\norigin-ms=1000\ncapture-complete=yes\nrecord-count={len(records)}\n")
        header = (ROOT / "benchmarks/fidelity-scenarios.tsv").read_text().splitlines()[0].split("\t")
        row = dict.fromkeys(header, "-")
        row.update(scenario="capture-fixture", url="https://capture.test/", replay_dir="trace",
                   trace_sha256=trace_digest(trace), expected_http="200", required_title="Capture fixture",
                   required_state_marker="Action completed" if dismiss else "Query ready" if query_mode else "Fixture ready", device_width="480", device_height="272",
                   css_width="480", css_height="272", scale_numerator="1", scale_denominator="1",
                   checkpoints="top|selector:h1|bottom", reference_state="reference-state.json", limit_mb="32",
                   ticks="1", tick_ms="33", max_download_kb="4096", script_timeout_ms="1000",
                   script_heap_mb="8", script_total_mb="8", script_file_kb="512", script_count="16",
                   min_stylesheets_loaded="0", min_images_loaded="1", min_scripts_loaded="0",
                   min_network_completions="1", max_pending="0")
        if query_mode=='socket-blocked':row['blocked_origins']='capture.test/live-updates'
        if worker_mode: row['required_state_marker']='Worker ready'
        if fragment: row['url'] += '#initial-state'
        manifest = directory / "manifest.tsv"
        manifest.write_text("\t".join(header) + "\n" + "\t".join(row[key] for key in header) + "\n")
        command = [NODE, str(ROOT / "benchmarks/capture-reference.js"), "--scenario", "capture-fixture",
                   "--manifest", str(manifest), "--trace-root", str(directory),
                   "--output-root", str(directory / "output"), "--settle-ms", "0"]
        if full:
            command.append("--full-document")
        if scroll_window:
            command.extend(["--scroll-window-px", str(scroll_window)])
        if mobile_auto_fit:
            command.append("--mobile-auto-fit")
        if internal_scroll:
            command.extend(['--scroll-selector', '#absent' if internal_scroll == 'missing' else '#scrollsurface'])
        if worker_mode: command.append('--dedicated-workers')
        if query_mode and query_mode!='default':
            command.append('--retained-query-posts')
        if dismiss:command.extend(['--click-text','Keep blocking'])
        if os.environ.get("PLAYWRIGHT_CHROMIUM_EXECUTABLE_PATH"):
            command.extend(["--executable", os.environ["PLAYWRIGHT_CHROMIUM_EXECUTABLE_PATH"]])
        completed = subprocess.run(command, capture_output=True, text=True, timeout=90)
        output = directory / "output/capture-fixture"
        state_file = output / ("reference-state.json" if completed.returncode == 0 else "reference-diagnostic.json")
        self.assertTrue(state_file.exists(), completed.stdout + completed.stderr)
        state = json.loads(state_file.read_text())
        if not full and completed.returncode == 0:
            reasons, _ = validate_state(load_manifest(manifest)[0], state, state_file, "reference")
            self.assertEqual(reasons, [], str(reasons))
        audit = json.loads((output / "capture-audit.json").read_text())
        return completed, {**state, **audit}, output

class BrowserCaptureTests(CaptureFixtureBase):
    def test_shared_cosmetic_policy_preserves_content_and_detects_tampering(self):
        script = r'''
const {chromium}=require('playwright'),policy=require(process.argv[1]);
(async()=>{const b=await chromium.launch({headless:true,
 executablePath:process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE_PATH,
 args:['--disable-background-networking','--host-resolver-rules=MAP * ~NOTFOUND']});
try{for(const enabled of [false,true]){const c=await b.newContext();
await c.addInitScript(policy.installPolicy,{...policy.loadRules(),enabled});
await c.route('**/*',r=>r.fulfill({contentType:'text/html',body:'<style>body{margin:0}</style>'+
 '<div data-csa-c-slot-id="adplacements:detail:test" style="height:50px;margin:20px">Sponsored</div>'+
 '<div id="content" data-csa-c-slot-id="content:detail:test">Product</div>'}));
const p=await c.newPage();await p.goto('https://example.test/');
const result=await p.evaluate(()=>({y:document.querySelector('#content').getBoundingClientRect().y,
 text:document.body.textContent,evidence:__tilefinchCosmeticEvidence()}));
if(!result.evidence.ready||!result.text.includes('Sponsored')||!result.text.includes('Product')
 ||(enabled?result.y!==0:result.y<50))throw Error(JSON.stringify(result));
if(enabled){await p.evaluate(()=>Array.from(document.querySelectorAll('style'))
 .find(s=>s.textContent.includes('adplacements:')).sheet.cssRules[0].style.display='block');
if((await p.evaluate(()=>__tilefinchCosmeticEvidence())).ready)throw Error('CSSOM modification accepted');
await p.evaluate(()=>Array.from(document.querySelectorAll('style'))
 .find(s=>s.textContent.includes('adplacements:')).remove());
if((await p.evaluate(()=>__tilefinchCosmeticEvidence())).ready)throw Error('removed policy accepted');}
await c.close();}console.log('cosmetic policy passed');}finally{await b.close()}})()
.catch(e=>{console.error(e);process.exitCode=1});
'''
        result = subprocess.run([NODE, '-e', script, str(ROOT / 'benchmarks/cosmetic-blocker-policy.js')],
            capture_output=True, text=True, timeout=45)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_shared_inline_tracker_policy_preserves_other_scripts_and_errors(self):
        script = r'''
const {chromium}=require('playwright'), policy=require(process.argv[1]);
(async()=>{const b=await chromium.launch({headless:true,
 executablePath:process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE_PATH,
 args:['--disable-background-networking','--host-resolver-rules=MAP * ~NOTFOUND']});
try{const context=await b.newContext();const config=policy.loadRules(),blocks=[];
await context.exposeBinding('__tilefinchInlineTrackerBlocked',(_s,id,source)=>{
 if(policy.matchRule('www.amazon.com',source,config.rules)!==id)throw Error('bad evidence');blocks.push(id);
});
await context.addInitScript({content:`(${policy.installPolicy})(${JSON.stringify(config)},${policy.matchRule})`});
const tracker='window.ue_ibe = (window.ue_ibe || 0) + 1; /* n.RXVM=function rx.ex64( /rd/uedata */ window.trackerRan=true;';
await context.route('**/*',route=>route.request().url()==='https://www.amazon.com/synthetic-inline'
 ? route.fulfill({contentType:'text/html',body:'<script>window.before=1;</script><script>'+tracker+
 '</script><script>window.after=1;window.ue_ibe=7;</script><script>throw Error("ordinary author error")</script>'})
 : route.abort());
const p=await context.newPage(),errors=[];p.on('pageerror',e=>errors.push(e.message));
await p.goto('https://www.amazon.com/synthetic-inline');
const state=await p.evaluate(()=>({before,after,tracker:window.trackerRan,metric:ue_ibe,
 text:document.scripts[1].text,evidence:__tilefinchInlineTrackerEvidence()}));
if(state.before!==1||state.after!==1||state.tracker!==undefined||state.metric!==7||state.text!==tracker
 ||!state.evidence.ready||state.evidence.blocked!==1||blocks.length!==1
 ||JSON.stringify(errors)!==JSON.stringify(['ordinary author error']))throw Error(JSON.stringify({state,blocks,errors}));
await p.evaluate(()=>delete window.ue_ibe);
if((await p.evaluate(()=>__tilefinchInlineTrackerEvidence())).ready)throw Error('tampered guard accepted');
console.log('inline tracker policy passed');
}finally{await b.close()}})().catch(e=>{console.error(e);process.exitCode=1});
'''
        result = subprocess.run([NODE, '-e', script, str(ROOT / 'benchmarks/inline-tracker-policy.js')],
            capture_output=True, text=True, timeout=45)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_environment_preflight_and_failed_image_contracts(self):
        result = subprocess.run([NODE, str(ROOT / "tests/playwright_reference_contract.js")],
                                capture_output=True, text=True, timeout=90)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_mobile_auto_fit_preserves_the_entire_physical_viewport(self):
        with tempfile.TemporaryDirectory() as temporary:
            completed, state, output = self.capture(Path(temporary),
                wide_layout=True, mobile_auto_fit=True, scroll_window=1024)
            self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
            self.assertTrue(state['capture_ready'])
            self.assertEqual(state['capture_scope'], 'mobile-auto-fit-window-diagnostic-v1')
            self.assertEqual(state['viewport']['auto_fit']['profile']['width'], 494)
            for view in state['full_document']['viewports']:
                profile = view['input_profile']['after']
                self.assertEqual(profile, view['input_profile']['before'])
                self.assertEqual((profile['screenWidth'], profile['screenHeight']), (480,272))
                self.assertTrue(profile['coarse'])
                width, height, pixels = read_png(output/view['frame'])
                self.assertEqual((width, height), (480,272))
                # The physical bottom row must be present, not padded/cropped.
                self.assertEqual(pixels[((height-1)*width+width//2)*3:][:3], bytes((0,255,0)))

    def test_visible_shadow_content_is_required_not_only_a_matching_title(self):
        for hidden in (False, True):
            with self.subTest(hidden=hidden), tempfile.TemporaryDirectory() as temporary:
                completed, state, _ = self.capture(Path(temporary),
                    shadow_marker=True, hidden_shadow_marker=hidden, scroll_window=1024)
                self.assertEqual(completed.returncode, 3 if hidden else 0,
                                 completed.stdout + completed.stderr)
                self.assertEqual(state['capture_ready'], not hidden)
                if hidden:
                    self.assertIn('reference-state-marker-missing', state['eligibility_reasons'])

    def test_painted_shadow_image_failure_is_not_hidden_by_the_host_boundary(self):
        for hidden in (False, True):
            with self.subTest(hidden=hidden), tempfile.TemporaryDirectory() as temporary:
                completed, state, _ = self.capture(Path(temporary),
                    shadow_image='hidden' if hidden else 'visible')
                self.assertEqual(completed.returncode, 0 if hidden else 3,
                                 completed.stdout + completed.stderr)
                self.assertEqual(state['capture_ready'], hidden)
                if not hidden:
                    self.assertIn('reference-paintable-images-incomplete', state['eligibility_reasons'])

    def test_redirect_and_lazy_images_are_replayed_and_verified(self):
        with tempfile.TemporaryDirectory() as temp:
            result, state, output = self.capture(Path(temp))
            self.assertEqual(result.returncode, 0, json.dumps(state, indent=2))
            self.assertEqual(state["browser"]["user_agent"], UA)
            self.assertTrue(state["browser_request_audit"]["ready"])
            self.assertTrue(state["full_document"]["complete"])
            self.assertEqual(len(state["full_document"]["viewports"]), 5)
            self.assertFalse((output / "full-page.png").exists())
            self.assertEqual(state["full_document"]["full_screenshot_status"], "omitted-mobile-emulation")
            self.assertEqual(state["full_document"]["viewports"][-1]["incomplete"], 0)
            self.assertEqual(state["replay_ledger"]["requests"], 5)

    def test_window_capture_does_not_claim_the_document_bottom(self):
        with tempfile.TemporaryDirectory() as temp:
            result, state, output = self.capture(Path(temp), long_document=True,
                scroll_window=4096, distant_image=True)
            self.assertEqual(result.returncode, 0, json.dumps(state, indent=2))
            self.assertEqual(state['capture_scope'], 'viewport-window-diagnostic-v1')
            full = state['full_document']
            self.assertEqual(full['version'], 'bounded-viewport-window-v1')
            self.assertEqual(full['window_scroll_limit'], 4096)
            self.assertEqual([v['scroll_y'] for v in full['viewports']], [0,1024,2048,3072,4096])
            self.assertGreater(full['final_extent']['maximum'], 4096)
            self.assertTrue(all(not view['full_document'] for view in full['viewports']))
            self.assertTrue(all(not any('/outside-window.svg' in image['src']
                for image in view['images']) for view in full['viewports']))
            raw = json.loads((output/'reference-state.json').read_text())
            self.assertEqual(raw['checkpoints'][-1]['kind'], 'offset')
            self.assertTrue(state['capture_ready'])
            self.assertFalse(census.complete_state(json.loads((output/'reference-state.json').read_text())))

    def test_corrupt_painted_child_image_refuses_complete_capture(self):
        for full in (False, True):
            with self.subTest(full=full), tempfile.TemporaryDirectory() as temp:
                result, state, _ = self.capture(Path(temp), iframe=True, full=full, corrupt_frame_image=True)
                self.assertEqual(result.returncode, 3, json.dumps(state, indent=2))
                self.assertTrue(state["browser_request_audit"]["ready"])
                self.assertIn("reference-paintable-images-incomplete", state["eligibility_reasons"])
                evidence = [checkpoint["visual_evidence"] for checkpoint in state["checkpoints"]]
                evidence.extend(state["full_document"]["viewports"])
                children = [child for entry in evidence for child in entry["child_frames"]]
                self.assertTrue(any(child["paintable"] and child["visual_evidence"]["incomplete"] == 1
                                    for child in children))

    def test_hidden_child_corruption_is_not_a_paintable_image_failure(self):
        with tempfile.TemporaryDirectory() as temp:
            result, state, _ = self.capture(Path(temp), iframe=True, hidden_frame=True, corrupt_frame_image=True)
            self.assertEqual(result.returncode, 0, json.dumps(state, indent=2))
            self.assertTrue(all(not child["paintable"] for checkpoint in state["checkpoints"]
                                for child in checkpoint["visual_evidence"]["child_frames"]))

    def test_missing_redirect_target_is_an_exact_acquisition_request(self):
        with tempfile.TemporaryDirectory() as temp:
            result, state, _ = self.capture(Path(temp), missing=True)
            self.assertEqual(result.returncode, 3, result.stderr)
            self.assertIn("reference-paintable-images-incomplete", state["eligibility_reasons"])
            urls = [entry["url"] for entry in state["acquisition_plan"]["requests"]]
            self.assertIn("https://images.test/final.svg", urls)

    def test_viewport_sweep_preserves_viewport_unit_geometry(self):
        with tempfile.TemporaryDirectory() as temp:
            result, state, output = self.capture(Path(temp), viewport_units=True)
            self.assertEqual(result.returncode, 0, json.dumps(state, indent=2))
            full = state["full_document"]
            self.assertEqual(full["final_extent"]["height"], full["viewports"][0]["document_height"])
            width, height, pixels = read_png(output / "view-100.png")
            self.assertEqual((width, height), (480, 272))
            # Bottom viewport contains 256 rows of the 272px 100vh block
            # followed by its 16px blue sibling, never a resized 100vh box.
            green_rows = sum(pixels[(y * width + width // 2) * 3:
                                    (y * width + width // 2) * 3 + 3] == b'\x00\xff\x00'
                             for y in range(height))
            self.assertEqual(green_rows, 256)
            self.assertFalse(full["full_screenshot"])

    def test_viewport_sweep_preserves_coarse_pointer_pixels(self):
        with tempfile.TemporaryDirectory() as temp:
            result, state, output = self.capture(Path(temp), pointer_media=True)
            self.assertEqual(result.returncode, 0, json.dumps(state, indent=2))
            width, height, pixels = read_png(output / "view-100.png")
            coarse_rows = sum(pixels[(y * width + width // 2) * 3:
                                     (y * width + width // 2) * 3 + 3] == b'\xff\x00\xff'
                              for y in range(height))
            self.assertEqual(coarse_rows, 16,
                             "viewport capture changed mobile pointer media while painting")
            self.assertFalse(state["full_document"]["full_screenshot"])
            self.assertFalse((output / "full-page.png").exists())
            self.assertTrue(census.complete_state(json.loads((output / "reference-state.json").read_text())))

    def test_missing_redirect_target_is_not_false_success_in_ordinary_capture(self):
        with tempfile.TemporaryDirectory() as temp:
            result, state, _ = self.capture(Path(temp), missing=True, full=False)
            self.assertEqual(result.returncode, 3, result.stderr)
            self.assertFalse(state["capture_ready"])
            self.assertIn("reference-paintable-images-incomplete", state["eligibility_reasons"])

    def test_ordinary_capture_also_intercepts_redirect_targets(self):
        with tempfile.TemporaryDirectory() as temp:
            result, state, _ = self.capture(Path(temp), full=False)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue(state["browser_request_audit"]["ready"])
            self.assertEqual(state["response_scheduler"]["browser_ordinal_semantics"],
                             "raw-chromium-fetch-callback-v1")

    def test_cross_origin_frame_redirects_are_intercepted(self):
        with tempfile.TemporaryDirectory() as temp:
            result, state, _ = self.capture(Path(temp), iframe=True)
            self.assertEqual(result.returncode, 0, json.dumps(state, indent=2))
            self.assertTrue(state["browser_request_audit"]["ready"])
            self.assertEqual(state["replay_ledger"]["requests"], 8)
            self.assertGreaterEqual(state["target_interception"]["targets"], 2)
            self.assertTrue(state["target_interception"]["ready"])


class LongDocumentCaptureTests(CaptureFixtureBase):
    def test_long_discussion_is_swept_without_allocating_a_full_page_image(self):
        with tempfile.TemporaryDirectory() as temp:
            result, state, output = self.capture(Path(temp), long_document=True)
            self.assertEqual(result.returncode, 0, json.dumps(state, indent=2))
            self.assertGreater(state['full_document']['steps'], 256)
            self.assertGreater(state['full_document']['final_extent']['height'], 65000)
            self.assertTrue(census.complete_state(json.loads((output / 'reference-state.json').read_text())))
            self.assertFalse((output / 'full-page.png').exists())


class ScrollSurfaceCaptureTests(CaptureFixtureBase):
    def test_internal_scroller_captures_real_offsets_and_lazy_content(self):
        with tempfile.TemporaryDirectory() as temporary:
            completed, state, output = self.capture(Path(temporary), internal_scroll='normal', scroll_window=1024)
            self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
            self.assertTrue(state['capture_ready'])
            self.assertEqual(state['capture_scope'], 'element-scroll-window-diagnostic-v1')
            views = state['full_document']['viewports']
            self.assertGreater(views[-1]['scroll_y'], 500)
            for view in views:
                surface = view['scroll_surface']
                self.assertEqual(surface['selector'], '#scrollsurface')
                self.assertEqual(surface['y'], view['requested'])
                self.assertEqual((surface['outer_x'], surface['outer_y']), (0,0))
                self.assertEqual(surface['height'], 232)
                self.assertEqual((read_png(output/view['frame'])[:2]), (480,272))
            self.assertNotEqual((output/views[0]['frame']).read_bytes(), (output/views[-1]['frame']).read_bytes())

    def test_window_navigation_preserves_fragment_without_changing_http_identity(self):
        with tempfile.TemporaryDirectory() as temporary:
            completed, state, _ = self.capture(Path(temporary), fragment=True, scroll_window=1024)
            self.assertEqual(completed.returncode, 0, str(state.get('eligibility_reasons')))
            self.assertEqual(state['url'], 'https://capture.test/')
            self.assertEqual(state['navigation']['requested'], 'https://capture.test/#initial-state')
            self.assertEqual(state['navigation']['observed'], state['navigation']['requested'])

    def test_internal_scroller_refuses_missing_ambiguous_hidden_and_non_scrolling_targets(self):
        for mode in ('missing', 'duplicate', 'hidden', 'static'):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as temporary:
                completed, state, _ = self.capture(Path(temporary), internal_scroll=mode, scroll_window=1024)
                self.assertEqual(completed.returncode, 3, completed.stdout + completed.stderr)
                self.assertFalse(state['capture_ready'])
                self.assertTrue(state['failure'].startswith('full-document:'), state['failure'])


@unittest.skipUnless(os.name == "posix", "worker capture requires POSIX process ownership")
class WorkerCaptureTests(CaptureFixtureBase):
    def test_dedicated_worker_has_offline_routing_and_logical_timers(self):
        for mode in ('timer', 'fetch', 'blob', 'import'):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as temporary:
                completed, state, _ = self.capture(Path(temporary), worker_mode=mode, scroll_window=1024)
                self.assertEqual(completed.returncode, 0, str(state.get('failure')) + str(state.get('eligibility_reasons')) + completed.stderr)
                self.assertTrue(state['capture_ready'])
                self.assertEqual(state['capture_scope'], 'dedicated-worker-window-diagnostic-v1')
                self.assertTrue(state['offline_capability_policy']['worker_evidence']['ready'])
                self.assertEqual(state['offline_capability_policy']['worker_evidence']['admitted'], 1)
                evidence = state['offline_capability_policy']['worker_evidence']
                self.assertGreaterEqual(evidence['transport']['held_total'], 1)
                self.assertEqual(evidence['transport']['held'], 0)
                self.assertTrue(evidence['transport']['profile_removed'])
                self.assertEqual(evidence['realms'][0]['messages'], 1)
                if mode in ('timer', 'blob'): self.assertEqual(evidence['realms'][0]['callbacks'], 1)

    def test_worker_writes_are_denied_and_uncontained_work_cannot_qualify(self):
        for mode in ('write', 'socket', 'pending', 'unknown', 'quota'):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as temporary:
                completed, state, _ = self.capture(Path(temporary), worker_mode=mode, scroll_window=1024)
                self.assertEqual(completed.returncode, 0 if mode == 'write' else 3,
                    str(state.get('failure')) + str(state.get('eligibility_reasons')))
                if mode == 'write':
                    self.assertEqual(state['read_only_policy']['diagnostics']['by_method'].get('POST'), 1)
                    self.assertEqual(state['replay_ledger']['unmatched'], 0)
                else: self.assertFalse(state['capture_ready'])


@unittest.skipUnless(PLAYWRIGHT, "Node/Playwright is unavailable")
class QueryBrowserCaptureTests(unittest.TestCase):
    capture = BrowserCaptureTests.capture

    def test_retained_query_replay_is_body_exact_and_opt_in(self):
        for mode in ('fetch','xhr','different','default','oversized','socket-blocked','socket-unknown'):
            with self.subTest(mode=mode),tempfile.TemporaryDirectory() as temp:
                result,state,output=self.capture(Path(temp),scroll_window=4096,query_mode=mode)
                if mode in ('fetch','xhr','socket-blocked'):
                    self.assertEqual(result.returncode,0,json.dumps(state,indent=2))
                    self.assertEqual(state['capture_scope'],'retained-query-window-diagnostic-v1')
                    self.assertEqual(state['read_only_policy']['denied_before_network'],0)
                    self.assertEqual(len(state['read_only_policy']['retained_query_sha256']),1)
                    self.assertFalse(census.complete_state(json.loads((output/'reference-state.json').read_text())))
                else:
                    self.assertEqual(result.returncode,3,json.dumps(state,indent=2))
                    if mode=='socket-unknown':
                        self.assertGreater(state['offline_capability_policy']['diagnostics']['total'],0)
                    else:self.assertGreater(state['read_only_policy']['denied_before_network'],0)
                    if mode=='different':self.assertFalse(state['read_only_policy']['ready'])

    def test_reviewed_control_action_is_recorded_and_not_a_default_state_oracle(self):
        with tempfile.TemporaryDirectory() as temp:
            result,state,output=self.capture(Path(temp),scroll_window=4096,dismiss=True)
            self.assertEqual(result.returncode,0,json.dumps(state,indent=2))
            self.assertEqual(state['capture_scope'],'interaction-window-diagnostic-v1')
            self.assertEqual(state['pre_capture_action'],{'kind':'visible-control-click','text':'Keep blocking','count':1})
            self.assertFalse(census.complete_state(json.loads((output/'reference-state.json').read_text())))


if __name__ == "__main__":
    unittest.main()
