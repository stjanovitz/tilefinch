#!/usr/bin/env python3
"""Mock-only qualification; never put real account inputs in this test."""

import importlib.util
import http.server
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "private_lab", ROOT / "scripts/run-private-interactive-lab.py")
PRIVATE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PRIVATE)
KEEPER_SPEC = importlib.util.spec_from_file_location(
    "private_keeper", ROOT / "scripts/run-private-signin-session.py")
KEEPER = importlib.util.module_from_spec(KEEPER_SPEC)
KEEPER_SPEC.loader.exec_module(KEEPER)
LAB = Path(sys.argv.pop(1)).resolve() if len(sys.argv) > 1 else None


class PrivateConsoleTests(unittest.TestCase):
    @unittest.skipUnless(LAB, "requires the host lab executable")
    def test_synchronous_script_form_keeps_formdata_entry_list(self):
        for delayed in (False, True):
            with self.subTest(delayed=delayed):
                self.check_script_form_entry_list(delayed)

    def check_script_form_entry_list(self, delayed):
        with tempfile.TemporaryDirectory() as directory:
            received = []
            class Handler(http.server.BaseHTTPRequestHandler):
                def log_message(self, *_):
                    pass
                def do_GET(self):
                    body = b"""<form id=f method=post action='/continue'>
                      <input name=original value=mock></form><script>
                      var f=document.getElementById('f');
                      f.addEventListener('formdata',function(e){
                        e.formData.set('continuation','mock-listener-field');
                        e.formData.delete('original');
                      });f.submit();</script>"""
                    if delayed:
                        body = body.replace(b'f.submit();', b'setTimeout(function(){f.submit()},0);')
                    self.send_response(200)
                    self.send_header("Content-Type", "text/html")
                    self.send_header("Content-Length", str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)
                def do_POST(self):
                    received.append(self.rfile.read(int(self.headers['Content-Length'])))
                    body = b"<p>Continuation completed</p>"
                    self.send_response(200)
                    self.send_header("Content-Type", "text/html")
                    self.send_header("Content-Length", str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)
            server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            session = PRIVATE.PrivateLab(LAB, directory,
                url=f"http://127.0.0.1:{server.server_port}/")
            try:
                session.status(timeout=30)
                if delayed:
                    session.send(b"pump 8 16\n")
                self.assertEqual(received, [b"continuation=mock-listener-field"])
            finally:
                session.close()
                server.shutdown()
                server.server_close()
                thread.join(timeout=5)

    @unittest.skipUnless(LAB, "requires the host lab executable")
    def test_private_navigation_keeps_http_only_session_in_browser(self):
        with tempfile.TemporaryDirectory() as directory:
            seen = []
            class Handler(http.server.BaseHTTPRequestHandler):
                def log_message(self, *_):
                    pass
                def do_GET(self):
                    second = self.path == "/second"
                    if second:
                        seen.append(self.headers.get("Cookie", ""))
                    body = b"<button>Second</button>" if second else b"<p>First</p>"
                    self.send_response(200)
                    self.send_header("Content-Type", "text/html")
                    self.send_header("Content-Length", str(len(body)))
                    if not second:
                        self.send_header("Set-Cookie", "mock_private_session=opaque; HttpOnly; Path=/")
                    self.end_headers()
                    self.wfile.write(body)
            server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            session = PRIVATE.PrivateLab(LAB, directory,
                url=f"http://127.0.0.1:{server.server_port}/")
            try:
                session.status(timeout=30)
                result = session.send(
                    f"private-open http://127.0.0.1:{server.server_port}/second\n".encode())
                self.assertEqual(result[8], 1)
                self.assertEqual(result[5], 1)
                self.assertIn("mock_private_session=opaque", seen[0])
                PRIVATE.create_private_snapshot(session, Path(directory))
                text = (Path(directory) / "redacted.ppm.txt").read_text()
                self.assertNotIn("mock_private_session", text)
                self.assertNotIn("opaque", text)
                self.assertIn("Second", text)
            finally:
                session.close()
                server.shutdown()
                server.server_close()
                thread.join(timeout=5)

    def test_keeper_youtube_actions_use_fixed_destinations_without_inputs(self):
        commands = []
        class Session:
            def send(self, command):
                commands.append(command)
                return [0, 1, 0, 0, 0, 0, 0, "https://www.youtube.com", 1]
        with tempfile.TemporaryDirectory() as directory:
            keeper = KEEPER.SessionKeeper("mock", directory, None, service="youtube")
            self.assertEqual(keeper.entry_url, KEEPER.SERVICE_URLS["youtube"])
            keeper.session = Session()
            for action, destination in KEEPER.YOUTUBE_PAGES.items():
                keeper.run(action)
                self.assertEqual(commands[-2], b"private-open " + destination.encode() + b"\n")
                self.assertEqual(commands[-1], b"pump 192 16\n")
                self.assertEqual(keeper.status()["phase"], "youtube_checked")
            self.assertEqual(keeper.password_attempts, 0)
            self.assertFalse(keeper.inspection_ready)
            self.assertEqual(keeper.request("https://arbitrary.invalid/")["accepted"], False)

    @unittest.skipUnless(LAB, "requires the host lab executable")
    def test_private_navigation_status_uses_committed_document_origin(self):
        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass
            def do_GET(self):
                body = b"<button>Mock destination</button>"
                self.send_response(200)
                self.send_header("Content-Type", "text/html")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
        servers = [http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
                   for _ in range(2)]
        threads = [threading.Thread(target=server.serve_forever, daemon=True)
                   for server in servers]
        for thread in threads:
            thread.start()
        try:
            with tempfile.TemporaryDirectory() as directory:
                origins = [f"http://127.0.0.1:{server.server_port}" for server in servers]
                session = PRIVATE.PrivateLab(LAB, directory, url=origins[0] + "/")
                try:
                    self.assertEqual(session.status(timeout=30)[7], origins[0])
                    result = session.send(f"private-open {origins[1]}/\n".encode())
                    self.assertEqual(result[8], 1)
                    # Private navigation intentionally records no history.
                    # Authority must follow the live document, not that entry.
                    self.assertEqual(result[7], origins[1])
                finally:
                    session.close()
        finally:
            for server in servers:
                server.shutdown()
                server.server_close()
            for thread in threads:
                thread.join(timeout=5)

    @unittest.skipUnless(LAB, "requires the host lab executable")
    def test_private_navigation_starts_at_top_without_recording_history(self):
        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass
            def do_GET(self):
                body = b"""<meta name=viewport content='width=device-width,initial-scale=1'>
                  <p id=report>Mock scroll pending</p><div style='height:2000px'></div>
                  <script>setTimeout(function(){document.getElementById('report').textContent=
                    'Mock scroll '+window.scrollY;},0);</script>"""
                self.send_response(200)
                self.send_header("Content-Type", "text/html")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            with tempfile.TemporaryDirectory() as directory:
                origin = f"http://127.0.0.1:{server.server_port}"
                session = PRIVATE.PrivateLab(LAB, directory, url=origin + "/first")
                try:
                    session.status(timeout=30)
                    self.assertEqual(session.send(b"scroll-by 400\n")[8], 1)
                    PRIVATE.create_private_snapshot(session, Path(directory))
                    self.assertIn("Presentation diagnostics: scroll-y=400",
                                  (Path(directory) / "redacted.ppm.txt").read_text())
                    self.assertEqual(session.send(f"private-open {origin}/second\n".encode())[8], 1)
                    self.assertEqual(session.send(b"pump 8 16\n")[8], 1)
                    PRIVATE.create_private_snapshot(session, Path(directory))
                    text = (Path(directory) / "redacted.ppm.txt").read_text()
                    self.assertIn("Presentation diagnostics: scroll-y=0", text)
                    self.assertIn("Mock scroll 0", " ".join(text.split()))
                finally:
                    session.close()
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)

    def test_keeper_rating_actions_are_explicit_ui_operations(self):
        commands = []
        class Session:
            def send(self, command):
                commands.append(command)
                return [0, 1, 0, 0, 0, 0, 0, "https://m.youtube.com", 1]
        with tempfile.TemporaryDirectory() as directory:
            keeper = KEEPER.SessionKeeper("mock", directory, None)
            keeper.session = Session()
            for action, name in KEEPER.YOUTUBE_UI_ACTIONS.items():
                keeper.run(action)
                self.assertEqual(commands[-3:], [b"focus-id " + name + b"\n",
                                                b"activate\n", b"pump 192 16\n"])
            self.assertEqual(keeper.password_attempts, 0)

    @unittest.skipUnless(LAB, "requires the host lab executable")
    def test_script_staging_census_contains_numbers_not_targets(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            html = b"""<button id=check>Check</button><script>
              document.getElementById('check').onclick=()=>{
                const script=document.createElement('script');
                script.src='/mock-staging-private-target';
                document.head.appendChild(script);
              };</script>"""
            class Handler(http.server.BaseHTTPRequestHandler):
                def log_message(self, *_):
                    pass
                def do_GET(self):
                    script = self.path == '/mock-staging-private-target'
                    body = b"globalThis.loaded=1" if script else html
                    self.send_response(200)
                    self.send_header("Content-Type", "text/javascript" if script else "text/html")
                    self.send_header("Content-Length", str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)
            server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
            worker = threading.Thread(target=server.serve_forever, daemon=True)
            worker.start()
            session = PRIVATE.PrivateLab(LAB, root,
                url=f"http://127.0.0.1:{server.server_port}/")
            try:
                session.status(timeout=30)
                session.send(b"focus-id check\n")
                session.send(b"activate\n")
                session.send(b"pump 8 16\n")
                PRIVATE.create_private_snapshot(session, root)
                text = (root / "redacted.ppm.txt").read_text()
                self.assertRegex(text, r"Page memory diagnostics: current=\d+ peak=\d+ limit=\d+ refusals=\d+")
                self.assertRegex(text, r"Script staging diagnostics: sequence=\d+ bound=\d+ free=\d+ other=\d+ dom=\d+ js=\d+ style=\d+ resource=\d+ layout=\d+ render=\d+ session=\d+ navigation=\d+")
                self.assertNotIn("mock-staging-private-target", text)
            finally:
                session.close()
                server.shutdown()
                server.server_close()
                worker.join(timeout=5)

    @unittest.skipUnless(LAB, "requires the host lab executable")
    def test_navigation_diagnostics_keep_counts_not_targets(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            html = b"""<button id=check>Check</button><script>
              document.getElementById('check').onclick=()=>{
                try { location.href='blob:https://example.invalid/mock-private-target'; } catch (_) {}
                location.href='/mock-private-target';
              };</script>"""
            class Handler(http.server.BaseHTTPRequestHandler):
                def log_message(self, *_):
                    pass
                def do_GET(self):
                    body = b"unavailable" if self.path == '/mock-private-target' else html
                    self.send_response(503 if self.path == '/mock-private-target' else 200)
                    self.send_header("Content-Type", "text/html")
                    self.send_header("Content-Length", str(len(body) +
                        (64 if self.path == '/mock-private-target' else 0)))
                    self.end_headers()
                    self.wfile.write(body)
                    self.close_connection = True
            server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
            worker = threading.Thread(target=server.serve_forever, daemon=True)
            worker.start()
            session = PRIVATE.PrivateLab(LAB, root,
                url=f"http://127.0.0.1:{server.server_port}/")
            try:
                session.status(timeout=30)
                session.send(b"focus-id check\n")
                session.send(b"activate\n")
                session.send(b"pump 8 16\n")
                PRIVATE.create_private_snapshot(session, root)
                text = (root / "redacted.ppm.txt").read_text()
                self.assertIn("Navigation diagnostics: accepted=1 refused=1 consumed=1", text)
                self.assertRegex(text, r"Navigation outcome diagnostics: http-status=503 transport-code=18 "
                    r"tls-failed=0 timed-out=0 error-present=1 committed=1 error-kind=other")
                self.assertNotIn("mock-private-target", text)
                self.assertIn("Layout failure diagnostics: available=0 phase=0 cancelled=0 refusals=0 current=0 limit=0", text)
            finally:
                session.close()
                server.shutdown()
                server.server_close()
                worker.join(timeout=5)

    def test_keeper_copies_only_safe_explicit_classifier(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source.js"
            source.write_text("(()=>0)")
            source.chmod(0o600)
            target = root / "target"
            target.mkdir()
            KEEPER.install_local_classifier(source, target)
            copied = target / "outcome-classifier.js"
            self.assertEqual(copied.read_text(), "(()=>0)")
            self.assertEqual(copied.stat().st_mode & 0o777, 0o600)
            copied.unlink()
            source.chmod(0o644)
            with self.assertRaises(ValueError):
                KEEPER.install_local_classifier(source, target)
            source.chmod(0o600)
            link = root / "link.js"
            link.symlink_to(source)
            with self.assertRaises(OSError):
                KEEPER.install_local_classifier(link, target)
            source.write_bytes(b"\0")
            with self.assertRaises(ValueError):
                KEEPER.install_local_classifier(source, target)
            source.write_bytes(b" " * (KEEPER.MAX_CLASSIFIER_BYTES + 1))
            with self.assertRaises(ValueError):
                KEEPER.install_local_classifier(source, target)
            self.assertFalse(copied.exists())

    @unittest.skipUnless(LAB, "requires the host lab executable")
    def test_handled_promise_diagnostics_retain_categories_not_contents(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixture = root / "mock.html"
            classifier = root / "outcome-classifier.js"
            classifier.write_text("(function(value,parse,event){"
                "if(event===2)return 5;if(event===3)return 9;"
                "if(event===1)return value instanceof TypeError?3:0;return 0})")
            classifier.chmod(0o600)
            fixture.write_text("""<button id=check>Check</button><script>
              document.getElementById('check').onclick=()=>{
                for(const value of [new TypeError('mock-private-secret'),
                  new RangeError('mock-private-secret'),
                  new ReferenceError('mock-private-secret'),
                  new SyntaxError('mock-private-secret'),
                  new InternalError('mock-private-secret'),
                  new Error('mock-private-secret'),'mock-private-secret',undefined])
                  Promise.reject(value).catch(()=>{});
              };</script>""")
            session = PRIVATE.PrivateLab(LAB, root, fixture=fixture)
            try:
                session.status(timeout=30)
                session.send(b"focus-id check\n")
                session.send(b"activate\n")
                session.send(b"pump 8 16\n")
                PRIVATE.create_private_snapshot(session, root)
                text = (root / "redacted.ppm.txt").read_text()
                self.assertIn("Promise outcome diagnostics: type=1 range=1 "
                    "reference=1 syntax=1 internal=1 error=1 other=1 undefined=1", text)
                self.assertIn("unhandled-rejections=0 handled-rejections=8", text)
                self.assertIn("Promise stage diagnostics: stage=3 count=1", text)
                self.assertIn("Promise stage diagnostics: stage=0 count=7", text)
                self.assertIn("Local stage diagnostics: entered=5 failed=9", text)
                self.assertNotIn("mock-private-secret", text)
            finally:
                session.close()

    @unittest.skipUnless(LAB, "requires the host lab executable")
    def test_private_outcome_classifier_exports_only_categories(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            classifier = root / "outcome-classifier.js"
            classifier.write_text("""(function(value,parse){
              if(value.fail)throw Error('mock-private-response-secret');
              if(value.nested){parse('"mock-private-response-secret"');return 3}
              return value.outcome;
            })""")
            classifier.chmod(0o600)
            fixture = root / "mock.html"
            fixture.write_text("""<button id=check>Check</button><div id=result></div>
              <script>document.getElementById('check').onclick=()=>{
                let ok=true;
                for(let i=0;i<13;i++)ok=ok&&JSON.parse(JSON.stringify({
                  outcome:i,secret:'mock-private-response-secret'})).outcome===i;
                for(const v of [{outcome:'mock-private-response-secret'},
                  {outcome:{toString:'mock-private-response-secret'}},
                  {fail:true},{nested:true}])JSON.parse(JSON.stringify(v));
                try{JSON.parse('mock-private-response-secret');ok=false}
                catch(e){ok=ok&&e instanceof SyntaxError}
                document.getElementById('result').textContent=ok?'outcomes-preserved':'failed';
              };</script>""")
            session = PRIVATE.PrivateLab(LAB, root, fixture=fixture)
            try:
                session.status(timeout=30)
                session.send(b"focus-id check\n")
                session.send(b"activate\n")
                self.assertEqual(PRIVATE.create_private_snapshot(session, root)[8], 1)
                text = (root / "redacted.ppm.txt").read_text()
                self.assertIn("outcomes-preserved", text)
                self.assertIn("json-parse-calls=18 json-parse-failures=1", text)
                self.assertIn("Outcome diagnostics: installed=1 unrecognized=4 "
                    "rejected=1 verification-required=1 redirect=2 "
                    "credential-transition=1 protocol-error=1 challenge-update=1", text)
                self.assertIn("Handoff diagnostics: navigation=1 form-post=1 "
                    "native-account=1 close=1 prerequisite=1 other=1", text)
                self.assertNotIn("mock-private-response-secret", text)
            finally:
                session.close()

    @unittest.skipUnless(LAB, "requires the host lab executable")
    def test_private_outcome_classifier_refuses_unsafe_files(self):
        for kind in ("public-mode", "symlink", "oversized", "not-function"):
            with self.subTest(kind=kind), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                source = root / "outcome-classifier.js"
                if kind == "symlink":
                    target = root / "other.js"
                    target.write_text("(()=>1)")
                    target.chmod(0o600)
                    source.symlink_to(target)
                else:
                    source.write_text("(()=>1)" if kind != "not-function" else "17")
                    source.chmod(0o644 if kind == "public-mode" else 0o600)
                    if kind == "oversized":
                        source.write_text(" " * 16385)
                fixture = root / "mock.html"
                fixture.write_text("<div>safe-page</div>")
                session = PRIVATE.PrivateLab(LAB, root, fixture=fixture)
                try:
                    session.status(timeout=30)
                    self.assertEqual(PRIVATE.create_private_snapshot(session, root)[8], 1)
                    text = (root / "redacted.ppm.txt").read_text()
                    self.assertIn("safe-page", text)
                    self.assertIn("Outcome diagnostics: installed=0", text)
                finally:
                    session.close()

    @unittest.skipUnless(LAB, "requires the host lab executable")
    def test_redacted_json_observer_preserves_parse_semantics(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixture = root / "mock.html"
            fixture.write_text("""<button id=check>Check</button><div id=result></div>
                <script>const cachedParse=JSON.parse;
                document.getElementById('check').onclick=()=>{
                  let ok=cachedParse('[1]')[0]===1;
                  ok=ok&&JSON.parse.name==='parse'&&JSON.parse.length===2;
                  ok=ok&&JSON.parse('{"value":"mock-json-secret"}').value==='mock-json-secret';
                  ok=ok&&JSON.parse('{"n":2}',function(k,v){return k==='n'?v+3:v}).n===5;
                  try{JSON.parse('mock-json-secret');ok=false}catch(e){ok=ok&&e instanceof SyntaxError}
                  const marker={};try{JSON.parse({toString(){throw marker}});ok=false}
                  catch(e){ok=ok&&e===marker}
                  ok=ok&&JSON.parse('2',function(k,v){return k===''?v+JSON.parse('3'):v})===5;
                  document.getElementById('result').textContent=ok?'parse-preserved':'parse-failed';
                };</script>""")
            session = PRIVATE.PrivateLab(LAB, root, fixture=fixture)
            try:
                session.status(timeout=30)
                session.send(b"focus-id check\n")
                session.send(b"activate\n")
                self.assertEqual(PRIVATE.create_private_snapshot(session, root)[8], 1)
                text = (root / "redacted.ppm.txt").read_text()
                self.assertIn("parse-preserved", text)
                self.assertIn("json-observer-installed=1", text)
                self.assertIn("json-parse-calls=6 json-parse-failures=2", text)
                self.assertNotIn("mock-json-secret", text)
            finally:
                session.close()

    @unittest.skipUnless(LAB, "requires the host lab executable")
    def test_private_json_observer_does_not_invoke_author_accessors(self):
        for target in ("globalThis", "JSON"):
            with self.subTest(target=target), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                fixture = root / "mock.html"
                name = "JSON" if target == "globalThis" else "parse"
                fixture.write_text("<div id=result>accessor-not-called</div><script>"
                    "Object.defineProperty(" + target + ", '" + name + "', {configurable:true,"
                    "get(){document.getElementById('result').textContent='accessor-called';"
                    "throw Error('mock-accessor-secret')}})</script>")
                session = PRIVATE.PrivateLab(LAB, root, fixture=fixture)
                try:
                    session.status(timeout=30)
                    self.assertEqual(PRIVATE.create_private_snapshot(session, root)[8], 1)
                    text = (root / "redacted.ppm.txt").read_text()
                    self.assertIn("accessor-not-called", text)
                    self.assertIn("json-observer-installed=0", text)
                    self.assertNotIn("mock-accessor-secret", text)
                finally:
                    session.close()

    @unittest.skipUnless(LAB, "requires the host lab executable")
    def test_redacted_dynamic_failures_distinguish_transport_from_authority(self):
        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass
            def do_GET(self):
                status = 200
                content_type = "text/plain"
                body = b"mock-private-response-do-not-export"
                if self.path == "/missing.js":
                    status = 404
                elif self.path == "/":
                    content_type = "text/html"
                    body = b"""<!doctype html><button id=start>Start</button><script>
                    document.getElementById('start').onclick=()=>{
                      let done=0;const finish=()=>{if(++done===2){
                        const b=document.createElement('button');b.id='finished';
                        b.textContent='Finished';document.body.appendChild(b)}};
                      for(const [src,type] of [['/missing.js',''],['/wrong-mime.js','module']]){
                        const s=document.createElement('script');s.src=src;s.type=type;
                        s.onload=s.onerror=finish;document.head.appendChild(s)}};
                    </script>"""
                self.send_response(status)
                self.send_header("Content-Type", content_type)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            with tempfile.TemporaryDirectory() as directory:
                session = PRIVATE.PrivateLab(LAB, directory,
                    url=f"http://127.0.0.1:{server.server_port}/")
                try:
                    session.status(timeout=30)
                    session.send(b"focus-id start\n")
                    session.send(b"activate\n")
                    session.send(b"pump 128 16\n")
                    self.assertEqual(session.send(b"focus-id finished\n")[8], 1)
                    PRIVATE.create_private_snapshot(session, Path(directory))
                    text = (Path(directory) / "redacted.ppm.txt").read_text()
                    self.assertIn("dynamic-transport-failures=1", text)
                    self.assertIn("dynamic-authority-failures=1", text)
                    self.assertIn("dynamic-response-limit-failures=0", text)
                    self.assertIn("dynamic-memory-failures=0", text)
                    self.assertIn("dynamic-source-failures=0", text)
                    self.assertNotIn("mock-private-response-do-not-export", text)
                    self.assertNotIn("/wrong-mime.js", text)
                    self.assertNotIn("/missing.js", text)
                finally:
                    session.close()
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)

    @unittest.skipUnless(LAB, "requires the host lab executable")
    def test_native_xhr_callbacks_checkpoint_before_next_listener(self):
        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass
            def do_GET(self):
                if self.path == "/response":
                    body = b"mock-response"
                else:
                    body = b"""<!doctype html><button id=start>Start</button><script>
                    document.getElementById('start').onclick=()=>{
                      const xhr=new XMLHttpRequest;let headers=false,progress=false;
                      const checks=[];xhr.open('GET','/response');
                      xhr.addEventListener('readystatechange',()=>{
                        if(xhr.readyState===2)Promise.resolve().then(()=>headers=true)});
                      xhr.addEventListener('readystatechange',()=>{
                        if(xhr.readyState===2)checks.push(headers)});
                      xhr.onprogress=()=>{checks.push(headers);
                        Promise.resolve().then(()=>progress=true)};
                      xhr.onload=()=>{checks.push(progress,
                        xhr.responseText==='mock-response',xhr.response==='mock-response');
                        const b=document.createElement('button');
                        b.id=checks.every(Boolean)?'checkpoint-ok':'checkpoint-failed';
                        b.textContent=b.id;document.body.appendChild(b)};xhr.send()};
                    </script>"""
                self.send_response(200)
                self.send_header("Content-Type", "text/plain" if self.path == "/response" else "text/html")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            with tempfile.TemporaryDirectory() as directory:
                session = PRIVATE.PrivateLab(LAB, directory,
                    url=f"http://127.0.0.1:{server.server_port}/")
                try:
                    session.status(timeout=30)
                    session.send(b"focus-id start\n")
                    session.send(b"activate\n")
                    session.send(b"tick 128 16\n")
                    self.assertEqual(session.send(b"focus-id checkpoint-ok\n")[8], 1)
                    PRIVATE.create_private_snapshot(session, Path(directory))
                    text = (Path(directory) / "redacted.ppm.txt").read_text()
                    self.assertNotIn("mock-response", text)
                    self.assertIn("Scheduler diagnostics: available=1", text)
                    self.assertIn("Response diagnostics: available=1 count=1", text)
                    self.assertIn("status=200 bytes=13 text-units=13 phase=5", text)
                    self.assertIn("text-reads=1 response-reads=1", text)
                    self.assertIn("state-events=3 progress-events=1 load-events=1", text)
                finally:
                    session.close()
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)

    @unittest.skipUnless(LAB, "requires the host lab executable")
    def test_private_ticks_complete_dynamic_image_decodes(self):
        requests = []
        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass
            def do_GET(self):
                requests.append(self.path)
                if self.path == "/pic.svg":
                    body = b"<svg xmlns='http://www.w3.org/2000/svg' width='24' height='24'><rect width='24' height='24' fill='red'/></svg>"
                    content_type = "image/svg+xml"
                else:
                    body = b"""<!doctype html><body><script>
                    setTimeout(()=>{const img=new Image();img.width=24;img.height=24;
                    img.loading='lazy';const loaded=()=>{
                      const button=document.createElement('button');
                      button.id='resource-ready';button.textContent='Ready';
                      document.body.appendChild(button)};
                    img.src='/pic.svg';document.body.appendChild(img);
                    img.decode().then(loaded)},1);
                    </script></body>"""
                    content_type = "text/html"
                self.send_response(200)
                self.send_header("Content-Type", content_type)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            with tempfile.TemporaryDirectory() as directory:
                session = PRIVATE.PrivateLab(LAB, directory,
                    url=f"http://127.0.0.1:{server.server_port}/")
                try:
                    session.status(timeout=30)
                    session.send(b"tick 64 16\n")
                    self.assertIn("/pic.svg", requests)
                    self.assertEqual(session.send(b"focus-id resource-ready\n")[8], 1)
                finally:
                    session.close()
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)

    @unittest.skipUnless(LAB, "requires the host lab executable")
    def test_private_pump_services_browser_resource_maintenance(self):
        requests = []
        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass
            def do_GET(self):
                requests.append(self.path)
                if self.path == "/pic.svg":
                    body = b"<svg xmlns='http://www.w3.org/2000/svg' width='24' height='24'><rect width='24' height='24' fill='red'/></svg>"
                    content_type = "image/svg+xml"
                else:
                    body = b"""<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'><body><script>
                    setTimeout(()=>{const img=new Image();img.width=24;img.height=24;
                    img.src='/pic.svg';document.body.appendChild(img)},1);
                    </script></body>"""
                    content_type = "text/html"
                self.send_response(200)
                self.send_header("Content-Type", content_type)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            with tempfile.TemporaryDirectory() as directory:
                session = PRIVATE.PrivateLab(LAB, directory,
                    url=f"http://127.0.0.1:{server.server_port}/")
                try:
                    session.status(timeout=30)
                    values = session.send(b"pump 64 16\n")
                    self.assertEqual(values[8], 1, "normal browser pump was refused")
                    self.assertIn("/pic.svg", requests)
                    PRIVATE.create_private_snapshot(session, Path(directory))
                    pixels = (Path(directory) / "redacted.ppm").read_bytes().split(b"\n", 3)[3]
                    self.assertGreater(pixels.count(b"\xff\x00\x00"), 256,
                                       "background image was requested but not painted")
                finally:
                    session.close()
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)

    def test_child_exit_during_close_still_releases_status_pipe(self):
        from unittest.mock import Mock
        session = PRIVATE.PrivateLab.__new__(PRIVATE.PrivateLab)
        session.process = Mock()
        session.process.poll.return_value = 0
        session.process.stdin.close.side_effect = BrokenPipeError()
        session.status_fd, write_fd = os.pipe()
        try:
            session.close()
            with self.assertRaises(OSError):
                os.fstat(session.status_fd)
        finally:
            os.close(write_fd)

    @unittest.skipUnless(LAB, "requires the host lab executable")
    def test_redacted_full_page_preserves_live_fields_and_reveals_bottom(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixture = root / "mock.html"
            fixture.write_text("""<meta name=viewport content='width=device-width,initial-scale=1'>
                <style>body{min-height:560px}input{width:200px}#error{position:absolute;top:400px;background:#00ff00}</style>
                <input autocomplete=username value=mock-identifier>
                <input type=password id=pass><textarea>mock-textarea-secret</textarea>
                <div id=error>Mock error below first screen</div><button id=check>Check</button>
                <script>Promise.reject(new TypeError('mock-private-secret not a function'));
                document.getElementById('check').onclick = function () {
                  if (document.getElementById('pass').value === 'mock-private-secret')
                    document.body.innerHTML = '<input id=passed>';
                };</script>""")
            session = PRIVATE.PrivateLab(LAB, root, fixture=fixture)
            try:
                session.status(timeout=30)
                session.send(b"private-focus password\n")
                session.send(PRIVATE.secret_command("mock-private-secret"))
                result = PRIVATE.create_private_snapshot(session, root)
                self.assertEqual(result[8], 1)
                text = (root / "redacted.ppm.txt").read_text()
                for value in ("mock-identifier", "mock-private-secret", "mock-textarea-secret"):
                    self.assertNotIn(value, text)
                self.assertIn("Mock error below first screen", " ".join(text.split()))
                self.assertIn("Runtime diagnostics: ", text)
                self.assertIn("rejection-kind=type", text)
                for metric in ("xhr-bytes=", "xhr-text-units=", "timers=",
                               "pending-tasks=", "pending-network=", "heap-refusals=",
                               "dynamic-queued=", "dynamic-completed="):
                    self.assertIn(metric, text)
                self.assertNotIn("not a function", text)
                ppm = (root / "redacted.ppm").read_bytes()
                header, dimensions, maximum, pixels = ppm.split(b"\n", 3)
                self.assertGreater(int(dimensions.split()[1]), 400)
                self.assertIn(b"\x00\xff\x00", pixels[272 * 480 * 3:])
                self.assertEqual((root / "redacted.ppm").stat().st_mode & 0o777, 0o600)
                self.assertEqual((root / "redacted.ppm.txt").stat().st_mode & 0o777, 0o600)
                session.send(b"focus-id check\n")
                session.send(b"activate\n")
                self.assertEqual(session.send(b"focus-id passed\n")[8], 1)
            finally:
                session.close()

    def test_local_view_scroll_does_not_submit_or_request_credentials(self):
        class Lab:
            def __init__(self):
                self.commands = []
            def send(self, command):
                self.commands.append(command)
                return [0, 1, 0, 0, 0, 0, 0, "https://accounts.google.com", 1]
        with tempfile.TemporaryDirectory() as directory:
            keeper = KEEPER.SessionKeeper("mock-lab", directory, None)
            keeper.session = Lab()
            with patch.object(KEEPER.PRIVATE, "show_private_view",
                              side_effect=lambda session, _: session.send(b"status\n")):
                keeper.run("view-below")
                keeper.run("view-above")
            self.assertEqual(keeper.session.commands,
                             [b"page-down\n", b"status\n", b"page-up\n", b"status\n"])
            self.assertEqual(keeper.password_attempts, 0)

    def test_password_validation_stops_and_reveals_without_resubmission(self):
        class Lab:
            def __init__(self):
                self.stage = "username"
                self.commands = []
            def values(self, accepted=True, editable=False):
                return [0, 1, int(editable), 0, 0, 2, 0,
                        "https://accounts.google.com", int(accepted)]
            def send(self, command):
                self.commands.append(command)
                if command.startswith(b"private-focus "):
                    matches = command.decode().split()[1] == self.stage
                    return self.values(matches, matches)
                if command == b"activate\n":
                    self.stage = "password"
                return self.values()
        session = Lab()
        entries = iter(["mock-email", "mock-password"])
        phases = []
        with patch.object(PRIVATE.time, "sleep"):
            PRIVATE.guided_signin(session, session.values(),
                                  input_value=lambda _: next(entries),
                                  progress=phases.append)
        self.assertEqual(phases[-1], "password_rejected")
        self.assertEqual(session.commands.count(b"activate\n"), 2)
        self.assertEqual(session.commands.count(PRIVATE.secret_command("mock-password")), 1)
        self.assertEqual(session.commands[-1], b"private-feedback\n")

    @unittest.skipUnless(LAB, "requires the host lab executable")
    def test_action_focus_uses_normal_blur_before_single_activation(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixture = root / "mock.html"
            fixture.write_text("""<input type=password id=pass>
                <div id=action><button id=next>Next</button></div>
                <div id=hidden style=display:none><button>Hidden</button></div>
                <div id=ambiguous><button>One</button><button>Two</button></div>
                <div id=disabled><button disabled>Disabled</button></div>
                <script>
                let committed = false, attempts = 0;
                document.getElementById('pass').onblur = function () {
                  committed = this.value === 'mock-password';
                };
                document.getElementById('next').onclick = function () {
                  attempts++;
                  if (committed && attempts === 1 && document.activeElement === this)
                    document.body.innerHTML = '<input id=passed>';
                };
                </script>""")
            session = PRIVATE.PrivateLab(LAB, root, fixture=fixture)
            try:
                session.status(timeout=30)
                self.assertEqual(session.send(b"private-focus password\n")[8], 1)
                self.assertEqual(session.send(PRIVATE.secret_command("mock-password"))[8], 1)
                for container in (b"missing", b"hidden", b"ambiguous", b"disabled"):
                    self.assertEqual(session.send(b"private-focus-action " + container + b"\n")[8], 0)
                self.assertEqual(session.send(b"private-focus-action action\n")[8], 1)
                self.assertEqual(session.send(b"activate\n")[8], 1)
                self.assertEqual(session.send(b"focus-id passed\n")[8], 1)
            finally:
                session.close()

    def test_duplicate_account_entries_are_refused_before_submission(self):
        with self.assertRaises(ValueError):
            KEEPER.CredentialMemory(lambda _: "mock-same-entry")
        class Lab:
            def __init__(self):
                self.commands = []
            def send(self, command):
                self.commands.append(command)
                return [0, 1, 1, 0, 0, 2, 0,
                        "https://accounts.google.com", 1]
        lab = Lab()
        phases = []
        PRIVATE.guided_signin(lab, lab.send(b"status\n"),
                              input_value=lambda _: "mock-same-entry",
                              progress=phases.append)
        self.assertEqual(phases[-1], "account_entries_match")
        self.assertFalse(any(command.startswith(b"private-text ")
                             or command.startswith(b"click ")
                             for command in lab.commands))

    def test_live_private_console_uses_real_time(self):
        with tempfile.TemporaryDirectory() as directory, \
                patch.object(PRIVATE.subprocess, "Popen") as spawn:
            spawn.return_value.poll.return_value = 0
            session = PRIVATE.PrivateLab("mock-lab", directory,
                                         url="https://fixture.test/login")
            try:
                args, kwargs = spawn.call_args
                self.assertIn("--pace-real-time", args[0])
                self.assertEqual(kwargs["stdout"], subprocess.DEVNULL)
                self.assertEqual(kwargs["stderr"], subprocess.DEVNULL)
            finally:
                session.close()

    def test_rejected_username_stops_without_password_delivery(self):
        class Lab:
            def __init__(self):
                self.submitted = False
                self.commands = []

            def values(self, accepted=True, editable=False):
                return [0, 1, int(editable), 0, 0, 2, 0,
                        "https://accounts.google.com", int(accepted)]

            def send(self, command):
                self.commands.append(command)
                if command == b"private-focus username\n":
                    return self.values(True, True)
                if command == b"private-focus password\n":
                    return self.values(False)
                if command == b"activate\n":
                    self.submitted = True
                if command == b"private-feedback\n":
                    return self.values(self.submitted)
                return self.values()

        session = Lab()
        phases = []
        entries = iter(["mock-email", "mock-password"])
        with patch.object(PRIVATE.time, "sleep"):
            PRIVATE.guided_signin(session, session.values(),
                                 input_value=lambda _: next(entries),
                                 progress=phases.append)
        self.assertEqual(phases[-1], "username_rejected")
        self.assertNotIn(PRIVATE.secret_command("mock-password"), session.commands)
        self.assertEqual(session.commands.count(b"private-focus-action identifierNext\n"), 1)
        self.assertEqual(session.commands.count(b"activate\n"), 1)
        self.assertIn(b"private-feedback\n", session.commands)

    @unittest.skipUnless(LAB, "requires the host lab executable")
    def test_private_feedback_reveals_offscreen_validation_without_text(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixture = root / "mock.html"
            fixture.write_text("""<!doctype html><style>
                input {position:absolute;top:230px;height:25px}
                #feedback {position:absolute;top:300px;width:200px;height:20px;background:#00ff00}
                </style><input autocomplete=username aria-describedby=feedback>
                <div id=feedback>Mock validation message</div><script>
                document.querySelector('input').oninput = function () {
                  this.setAttribute('aria-invalid', 'true');
                };</script>""")
            session = PRIVATE.PrivateLab(LAB, root, fixture=fixture)
            try:
                session.status(timeout=30)
                session.send(b"private-focus username\n")
                self.assertEqual(session.send(b"private-feedback\n")[8], 0)
                session.send(PRIVATE.secret_command("mock-rejected"))
                values = session.send(b"private-feedback\n")
                self.assertEqual(values[8], 1)
                self.assertNotIn("Mock validation message", str(values))
                session.send(f"render {root / 'feedback.ppm'}\n".encode())
                pixels = (root / "feedback.ppm").read_bytes()[len(b"P6\n480 272\n255\n"):]
                self.assertGreater(sum(pixels[i:i + 3] == b"\x00\xff\x00"
                                       for i in range(0, len(pixels), 3)), 100)
            finally:
                session.close()

    def test_keeper_retains_once_and_gates_uncertain_password_retry(self):
        prompts = []
        def account_input(prompt):
            prompts.append(prompt)
            return "mock-email" if prompt.endswith("email") else "mock-password"

        class Lab:
            def __init__(self, *args, **kwargs):
                self.stage = "username"
                self.commands = []
                self.closed = False
                self.fail_password = False

            def status(self, accepted=True, editable=False):
                return [0, 1, int(editable), 0, 0, 2, 0,
                        "https://accounts.google.com", int(accepted)]

            def send(self, command):
                self.commands.append(command)
                if command.startswith(b"private-focus "):
                    selected = command.decode().split()[1] == self.stage
                    return self.status(selected, selected)
                if command == b"activate\n":
                    self.stage = "password"
                if command == b"private-feedback\n":
                    return self.status(False)
                if command == PRIVATE.secret_command("mock-password") and self.fail_password:
                    raise RuntimeError("mock-password must never reach diagnostics")
                return self.status()

            def close(self):
                self.closed = True

        labs = []
        def factory(*args, **kwargs):
            lab = Lab()
            labs.append(lab)
            return lab

        approvals = []
        def deny(cancelled):
            approvals.append(True)
            return False

        with tempfile.TemporaryDirectory() as directory:
            credentials = KEEPER.CredentialMemory(account_input)
            keeper = KEEPER.SessionKeeper("mock-lab", directory, credentials,
                                         factory=factory, retry_confirmation=lambda _: True,
                                         confirm_each_attempt=True)
            with patch.object(KEEPER.PRIVATE.time, "sleep"), \
                    patch.object(KEEPER.PRIVATE, "show_private_view",
                                 side_effect=lambda session, _: session.status()):
                keeper.run("attempt")
                self.assertEqual(labs[0].commands[0], b"pump 192 16\n",
                                 "static field appearance is not initialized input")
                self.assertEqual(keeper.password_attempts, 1)
                keeper.run("restart")
                keeper.retry_confirmation = deny
                keeper.run("attempt")
                self.assertEqual(len(approvals), 1)
                self.assertEqual(keeper.phase, "retry_not_approved")
                self.assertNotIn(PRIVATE.secret_command("mock-password"), labs[-1].commands)
                keeper.retry_confirmation = lambda cancelled: True
                labs[-1].fail_password = True
                keeper.run("attempt")
                self.assertEqual(keeper.password_attempts, 2,
                                 "uncertain delivery must count before sending")
                self.assertEqual(keeper.phase, "browser_operation_failed")
                keeper.run("restart")
                keeper.run("attempt")
                self.assertEqual(keeper.password_attempts, 3)
                keeper.run("attempt")
                self.assertEqual(keeper.phase, "password_attempt_limit")
            self.assertEqual(prompts, ["Test account email", "Test account password"])
            self.assertNotIn("mock-email", str(keeper.status()))
            self.assertNotIn("mock-password", str(keeper.status()))
            self.assertEqual(list(Path(directory).iterdir()), [])
            retained = list(credentials._values)
            keeper.close()
            self.assertTrue(all(all(b == 0 for b in value) for value in retained))
            self.assertFalse(credentials._values)

    def test_keeper_single_confirmed_attempt_survives_restart(self):
        class Lab:
            def status(self):
                return [0, 1, 0, 0, 0, 2, 0, "https://accounts.google.com", 1]
            def send(self, _):
                return self.status()
            def close(self):
                pass

        def guided(session, values, **kwargs):
            kwargs["before_password"]()
            return values

        with tempfile.TemporaryDirectory() as directory:
            credentials = KEEPER.CredentialMemory(
                lambda prompt: "mock-email" if prompt.endswith("email") else "mock-password")
            keeper = KEEPER.SessionKeeper("mock", directory, credentials,
                factory=lambda *a, **k: Lab(), password_attempt_limit=1,
                confirm_first_attempt=True, retry_confirmation=lambda _: False)
            try:
                with patch.object(KEEPER.PRIVATE, "guided_signin", side_effect=guided) as driver, \
                        patch.object(KEEPER.PRIVATE, "show_private_view",
                                     side_effect=lambda session, _: session.status()):
                    keeper.run("attempt")
                    self.assertEqual(keeper.phase, "retry_not_approved")
                    self.assertEqual(keeper.password_attempts, 0)
                    driver.assert_not_called()
                    keeper.retry_confirmation = lambda _: True
                    keeper.run("attempt")
                    self.assertEqual(keeper.password_attempts, 1)
                    keeper.run("restart")
                    keeper.run("attempt")
                    self.assertEqual(keeper.phase, "password_attempt_limit")
                    self.assertEqual(driver.call_count, 1)
                    with self.assertRaises(RuntimeError):
                        keeper.before_password()
            finally:
                keeper.close()
            for invalid in (-1, 4, True):
                with self.assertRaises(ValueError):
                    KEEPER.SessionKeeper("mock", directory, None,
                                         password_attempt_limit=invalid)

    def test_keeper_uncapped_attempts_require_confirmation_and_stop_cleanly(self):
        class Lab:
            def status(self):
                return [0, 1, 0, 0, 0, 2, 0, "https://accounts.google.com", 1]
            def send(self, _):
                return self.status()
            def close(self):
                pass
        def guided(session, values, **kwargs):
            kwargs["before_password"]()
            return values
        with tempfile.TemporaryDirectory() as directory:
            credentials = KEEPER.CredentialMemory(
                lambda prompt: "mock-email" if prompt.endswith("email") else "mock-password")
            keeper = KEEPER.SessionKeeper("mock", directory, credentials,
                factory=lambda *a, **k: Lab(), password_attempt_limit=0,
                retry_confirmation=lambda _: False, confirm_each_attempt=True)
            retained = list(credentials._values)
            try:
                with patch.object(KEEPER.PRIVATE, "guided_signin", side_effect=guided) as driver, \
                        patch.object(KEEPER.PRIVATE, "show_private_view",
                                     side_effect=lambda session, _: session.status()):
                    # Uncapped is explicit, but never authorizes even the first
                    # submission without a local confirmation.
                    keeper.run("attempt")
                    driver.assert_not_called()
                    self.assertEqual(keeper.password_attempts, 0)
                    self.assertEqual(keeper.phase, "retry_not_approved")
                    approvals = []
                    def approve(cancelled):
                        self.assertFalse(cancelled())
                        approvals.append(keeper.password_attempts)
                        return True
                    keeper.retry_confirmation = approve
                    for _ in range(5):
                        keeper.run("restart")
                        keeper.run("attempt")
                    self.assertEqual(keeper.password_attempts, 5)
                    self.assertEqual(driver.call_count, 5)
                    self.assertEqual(approvals, list(range(5)))
                    keeper.retry_confirmation = lambda _: False
                    keeper.run("attempt")
                    self.assertEqual(driver.call_count, 5)
                    self.assertEqual(keeper.password_attempts, 5)
                    self.assertTrue(keeper.request("stop")["accepted"])
                    keeper.run("attempt")
                    self.assertEqual(driver.call_count, 5)
            finally:
                keeper.close()
            self.assertTrue(all(all(b == 0 for b in value) for value in retained))
            self.assertFalse(credentials._values)

    def test_keeper_start_authorizes_requested_attempts_without_popups(self):
        class Lab:
            def status(self):
                return [0, 1, 0, 0, 0, 2, 0, "https://accounts.google.com", 1]
            def send(self, _):
                return self.status()
            def close(self):
                pass
        def guided(session, values, **kwargs):
            kwargs["before_password"]()
            return values
        with tempfile.TemporaryDirectory() as directory:
            credentials = KEEPER.CredentialMemory(
                lambda prompt: "mock-email" if prompt.endswith("email") else "mock-password")
            keeper = KEEPER.SessionKeeper("mock", directory, credentials,
                factory=lambda *a, **k: Lab(), password_attempt_limit=0)
            try:
                with patch.object(keeper, "retry_confirmation") as confirmation, \
                        patch.object(KEEPER.PRIVATE, "guided_signin", side_effect=guided) as driver, \
                        patch.object(KEEPER.PRIVATE, "show_private_view") as preview:
                    for _ in range(5):
                        keeper.run("restart")
                        keeper.run("attempt")
                    self.assertEqual(keeper.password_attempts, 5)
                    self.assertEqual(driver.call_count, 5)
                    confirmation.assert_not_called()
                    preview.assert_not_called()
                    keeper.run("advance")
                    keeper.run("restart")
                    self.assertEqual(driver.call_count, 5, "never retry automatically")
                    keeper.run("view")
                    preview.assert_called_once()
                    keeper.request("stop")
                    keeper.run("attempt")
                    self.assertEqual(driver.call_count, 5)
            finally:
                keeper.close()

    def test_keeper_browser_failure_does_not_request_credentials_again(self):
        entries = iter(["mock-email", "mock-password"])
        credentials = KEEPER.CredentialMemory(lambda _: next(entries))
        with tempfile.TemporaryDirectory() as directory:
            keeper = KEEPER.SessionKeeper("mock", directory, credentials,
                                         factory=lambda *a, **k: (_ for _ in ()).throw(OSError()))
            keeper.run("attempt")
            self.assertEqual(keeper.phase, "browser_operation_failed")
            self.assertEqual(credentials.input_value("Test account email"), "mock-email")
            self.assertEqual(credentials.input_value("Test account password"), "mock-password")
            keeper.close()

    def test_keeper_bounded_control_and_status_while_busy(self):
        credentials = KEEPER.CredentialMemory(
            lambda prompt: "mock-email" if prompt.endswith("email") else "mock-password")
        with tempfile.TemporaryDirectory(prefix="tf-auth-test-", dir="/private/tmp") as directory:
            keeper = KEEPER.SessionKeeper("mock", directory, credentials)
            keeper.run = lambda action: keeper.stopping.wait(5)
            path = Path(directory) / "control.sock"
            ready = threading.Event()
            server = threading.Thread(target=KEEPER.serve, args=(keeper, path),
                                      kwargs={"ready": ready})
            server.start()
            try:
                self.assertTrue(ready.wait(5), "control listener did not become ready")
                self.assertEqual(path.stat().st_mode & 0o777, 0o600)
                status = KEEPER.control(path, "status")
                self.assertTrue(status["busy"])
                self.assertNotIn("mock-password", str(status))
                self.assertEqual(KEEPER.control(path, "restart")["reason"], "busy")
                self.assertFalse(keeper.request("private-text mock-password")["accepted"])
                self.assertTrue(KEEPER.control(path, "stop")["accepted"])
            finally:
                keeper.stopping.set()
                server.join(timeout=5)
                keeper.close()
            self.assertFalse(server.is_alive())

    def test_private_view_png_exact_pixels_and_limits(self):
        import struct
        import zlib
        with tempfile.TemporaryDirectory() as directory:
            source, target = Path(directory) / "frame.ppm", Path(directory) / "frame.png"
            pixels = bytes(range(256)) * (480 * 272 * 3 // 256)
            source.write_bytes(b"P6\n480 272\n255\n" + pixels)
            PRIVATE.private_view_png(source, target)
            png = target.read_bytes()
            self.assertEqual(png[:8], b"\x89PNG\r\n\x1a\n")
            at, compressed = 8, bytearray()
            while at < len(png):
                length, = struct.unpack(">I", png[at:at + 4])
                kind = png[at + 4:at + 8]
                body = png[at + 8:at + 8 + length]
                crc, = struct.unpack(">I", png[at + 8 + length:at + 12 + length])
                self.assertEqual(crc, zlib.crc32(kind + body))
                if kind == b"IDAT": compressed.extend(body)
                at += length + 12
            rows = zlib.decompress(compressed)
            self.assertEqual(b"".join(rows[y * 1441 + 1:(y + 1) * 1441]
                                      for y in range(272)), pixels)
            self.assertEqual(target.stat().st_mode & 0o777, 0o600)
            source.write_bytes(b"P6\n480 272\n255\n" + pixels + b"extra")
            with self.assertRaises(ValueError): PRIVATE.private_view_png(source, target)

    def test_native_dialog_keeps_value_out_of_arguments(self):
        reply = subprocess.CompletedProcess([], 0, stdout=b"  mock-password  \n")
        with patch.object(PRIVATE.sys, "platform", "darwin"), \
                patch.object(PRIVATE.subprocess, "run", return_value=reply) as run:
            self.assertEqual(PRIVATE.local_account_input("Test account password"),
                             "  mock-password  ")
            args, kwargs = run.call_args
            self.assertNotIn("mock-password", repr(args))
            self.assertIn("with hidden answer", args[0][2])
            self.assertEqual(kwargs["stderr"], subprocess.DEVNULL)
            run.return_value = subprocess.CompletedProcess([], 1, stdout=b"")
            self.assertIsNone(PRIVATE.local_account_input("Test account password"))

    def test_guided_signin_single_submission_and_origin_guard(self):
        class Session:
            def __init__(self, switch_origin=False):
                self.commands = []
                self.stage = "username"
                self.origin = "https://accounts.google.com"
                self.switch_origin = switch_origin

            def values(self, success=True, editable=False):
                return [0, 1, int(editable), 0, 0, 2, 0, self.origin, int(success)]

            def send(self, command):
                self.commands.append(command)
                if command.startswith(b"private-focus "):
                    kind = command.decode().split()[1]
                    return self.values(kind == self.stage, kind == self.stage)
                if command == b"activate\n" and self.stage == "username":
                    self.stage = "password"
                    if self.switch_origin:
                        self.origin = "https://unexpected.test"
                if command == b"private-feedback\n":
                    return self.values(False)
                return self.values()

        session = Session()
        entries = iter(["mock-email", "mock-password"])
        PRIVATE.guided_signin(session, session.values(),
                              input_value=lambda _: next(entries))
        self.assertEqual(session.commands.count(PRIVATE.secret_command("mock-email")), 1)
        self.assertEqual(session.commands.count(PRIVATE.secret_command("mock-password")), 1)
        self.assertEqual(session.commands.count(b"private-focus-action passwordNext\n"), 1)
        self.assertEqual(session.commands.count(b"activate\n"), 2)
        session = Session(switch_origin=True)
        entries = iter(["mock-email", "mock-password"])
        PRIVATE.guided_signin(session, session.values(),
                              input_value=lambda _: next(entries))
        self.assertNotIn(PRIVATE.secret_command("mock-password"), session.commands)
        session = Session()
        session.origin = "https://unexpected.test"
        PRIVATE.guided_signin(session, session.values(),
                              input_value=lambda _: self.fail("must not prompt"))
        self.assertFalse(session.commands)

    def test_encoding_and_bounds(self):
        mock = "  mock-ñ-中  "
        encoded = PRIVATE.secret_command(mock)
        self.assertEqual(bytes.fromhex(encoded.decode().split()[1]).decode(), mock)
        for value in ("a\nb", "a\rb", "a\0b", "x" * 1024, "\ud800"):
            with self.assertRaises((ValueError, UnicodeError)):
                PRIVATE.secret_command(value)
        self.assertEqual(PRIVATE.secret_command(""), b"private-text \n")

    def test_environment_drops_capture_and_cache(self):
        old = os.environ.copy()
        try:
            os.environ.update(TILEFINCH_TRACE_GC="1", SSLKEYLOGFILE="bad",
                              TILEFINCH_SCRIPT_CACHE_DIR="bad", PYTHONPATH="bad")
            environment = PRIVATE.private_environment(Path("/private/mock"))
            self.assertEqual(environment["HOME"], "/private/mock")
            self.assertFalse(any(key.startswith("TILEFINCH_") for key in environment))
            self.assertNotIn("SSLKEYLOGFILE", environment)
            self.assertNotIn("PYTHONPATH", environment)
        finally:
            os.environ.clear()
            os.environ.update(old)

    def test_piped_input_refused(self):
        result = subprocess.run([sys.executable,
                                 str(ROOT / "scripts/run-private-interactive-lab.py"),
                                 "--url", "https://example.com/login"],
                                input=b"mock-secret\n", capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn(b"mock-secret", result.stdout + result.stderr)
        self.assertIn(b"piped credential input is refused", result.stderr)

    @unittest.skipUnless(LAB, "requires the host lab executable")
    def test_keeper_survives_real_browser_exit_with_credentials_retained(self):
        entries = iter(["mock-email", "mock-password"])
        credentials = KEEPER.CredentialMemory(lambda _: next(entries))
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixture = root / "mock.html"
            fixture.write_text("<input autocomplete=username>")
            factory = lambda lab, path, **kwargs: PRIVATE.PrivateLab(
                lab, path, fixture=fixture)
            keeper = KEEPER.SessionKeeper(LAB, root, credentials, factory=factory)
            try:
                keeper.run("restart")
                old = keeper.session
                old.process.terminate()
                old.process.wait(timeout=10)
                keeper.run("restart")
                self.assertIsNot(keeper.session, old)
                self.assertEqual(keeper.status()["origin"], "https://fixture.test")
                self.assertEqual(credentials.input_value("Test account password"), "mock-password")
                self.assertEqual({p.name for p in root.iterdir()}, {"mock.html"})
            finally:
                keeper.close()

    @unittest.skipUnless(LAB, "requires the host lab executable")
    def test_cancelled_beforeinput_is_reported_without_value_disclosure(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixture = root / "mock.html"
            fixture.write_text("""<input id=user autocomplete=username><script>
                document.getElementById('user').addEventListener('beforeinput',
                  function (event) { event.preventDefault(); });</script>""")
            session = PRIVATE.PrivateLab(LAB, root, fixture=fixture)
            try:
                session.status(timeout=30)
                session.send(b"private-focus username\n")
                values = session.send(PRIVATE.secret_command("mock-refused"))
                self.assertEqual(values[8], 0, "dispatch success is not field acceptance")
                self.assertNotIn("mock-refused", str(values))
                self.assertEqual(session.send(b"status\n")[8], 1)
            finally:
                session.close()

    @unittest.skipUnless(LAB, "requires the host lab executable")
    def test_semantic_focus_and_recoverable_refusal(self):
        html = """<!doctype html><input type=hidden autocomplete=username>
        <input id=user autocomplete=username><button id=next>Next</button>
        <script>document.getElementById('next').onclick = function () {
          if (document.getElementById('user').value !== 'mock-email') return;
          document.body.innerHTML = '<input readonly id=account value=mock-email>' +
            '<input type=password style=display:none id=decoy>' +
            '<input type=password id=pass><button id=submit>Next</button>';
          document.getElementById('submit').onclick = function () {
            if (document.getElementById('pass').value === 'mock-password' &&
                document.getElementById('account').value === 'mock-email' &&
                document.getElementById('decoy').value === '')
              document.body.innerHTML = '<input id=passed>';
          };
        };</script>"""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixture = root / "mock.html"
            fixture.write_text(html)
            session = PRIVATE.PrivateLab(LAB, root, fixture=fixture)
            try:
                session.status(timeout=30)
                # Missing password must acknowledge refusal, not kill the lab.
                self.assertEqual(session.send(b"private-focus password\n")[8], 0)
                self.assertEqual(session.send(b"private-text zzzz\n")[8], 0)
                values = session.send(b"private-focus username\n")
                self.assertEqual((values[2], values[8]), (1, 1))
                self.assertEqual(session.send(PRIVATE.secret_command("mock-email"))[8], 1)
                session.send(b"click #next\n")
                values = session.send(b"private-focus password\n")
                self.assertEqual((values[2], values[8]), (1, 1))
                self.assertEqual(session.send(PRIVATE.secret_command("mock-password"))[8], 1)
                session.send(b"click #submit\n")
                self.assertEqual(session.send(b"focus-id passed\n")[8], 1)
                self.assertEqual({p.name for p in root.iterdir()}, {"mock.html"})
            finally:
                session.close()

    @unittest.skipUnless(LAB, "requires the host lab executable")
    def test_exact_field_replace_and_no_automatic_files(self):
        # A page deliberately logs its field. The private console must discard
        # even this author-controlled output, rather than filtering log prefixes.
        html = """<!doctype html><meta name="viewport" content="width=device-width,initial-scale=1">
        <input id="entry" type="password">
        <button id="check" style="position:absolute;left:40px;top:80px;width:80px;height:24px">Check</button><script>
        document.getElementById('check').addEventListener('click', function () {
          var value = document.getElementById('entry').value;
          console.log('loop S 999 ' + value);
          if (value === '  mock-ñ-中  ') {
            var input = document.createElement('input');
            input.id = 'passed'; document.body.appendChild(input);
          }
        });</script>"""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixture = root / "mock.html"
            fixture.write_text(html, encoding="utf-8")
            session = PRIVATE.PrivateLab(LAB, root, fixture=fixture)
            try:
                initial = session.status(timeout=30)
                self.assertEqual(initial[5], 2)
                self.assertEqual(initial[7], "https://fixture.test")
                self.assertEqual(session.send(b"focus-id entry\n")[2], 1)
                session.send(PRIVATE.secret_command("wrong"))
                session.send(PRIVATE.secret_command(""))
                session.send(PRIVATE.secret_command("  mock-ñ-中  "))
                session.send(b"focus-id check\n")
                values = session.send(b"activate\n")
                self.assertEqual(values[5], 3, "replacement must preserve exact UTF-8 and spaces")
                values = session.send(b"click 50 90\n")
                self.assertEqual(values[5], 4, "pointer activation must finish before its single acknowledgement")
                session.send(b"status\n")
                self.assertEqual({p.name for p in root.iterdir()}, {"mock.html"})
                # An explicit private view is the only permitted image capture.
                session.send(f"render {root / 'explicit.ppm'}\n".encode())
                self.assertTrue((root / "explicit.ppm").is_file())
                with self.assertRaises(RuntimeError):
                    session.send(b"control-value entry\n")  # forbidden diagnostic
            finally:
                session.close()

    @unittest.skipUnless(LAB, "requires the host lab executable")
    def test_private_mode_rejects_capture_flags(self):
        with tempfile.TemporaryDirectory() as directory:
            fixture = Path(directory) / "mock.html"
            fixture.write_text("<input>")
            read_fd, write_fd = os.pipe()
            try:
                result = subprocess.run(
                    [str(LAB), "--fixture", str(fixture), "--commands", "/dev/stdin",
                     "--no-loop-capture", "--output", "/dev/null",
                     "--private-console-fd", str(write_fd),
                     "--capture-http", str(Path(directory) / "capture")],
                    pass_fds=(write_fd,), input=b"quit\n", capture_output=True,
                    env=PRIVATE.private_environment(directory), timeout=30)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse((Path(directory) / "capture").exists())
            finally:
                os.close(read_fd)
                os.close(write_fd)

    @unittest.skipUnless(LAB, "requires the host lab executable")
    def test_native_output_is_discarded_even_with_pipe_capture(self):
        with tempfile.TemporaryDirectory() as directory:
            fixture = Path(directory) / "mock.html"
            fixture.write_text("<input id=entry><script>console.log('author-output');</script>")
            read_fd, write_fd = os.pipe()
            try:
                result = subprocess.run(
                    [str(LAB), "--fixture", str(fixture), "--commands", "/dev/stdin",
                     "--fetch-scripts", "--no-loop-capture", "--output", "/dev/null",
                     "--private-console-fd", str(write_fd)],
                    pass_fds=(write_fd,), input=b"focus-id entry\n"
                    + PRIVATE.secret_command("mock-no-output") + b"quit\n",
                    capture_output=True, env=PRIVATE.private_environment(directory), timeout=30)
                self.assertEqual(result.returncode, 0)
                self.assertEqual(result.stdout, b"")
                self.assertEqual(result.stderr, b"")
                records = os.read(read_fd, 1024)
                self.assertEqual(len(records.splitlines()), 3)
                self.assertNotIn(b"mock-no-output", records)
                self.assertEqual({p.name for p in Path(directory).iterdir()}, {"mock.html"})
            finally:
                os.close(read_fd)
                os.close(write_fd)


if __name__ == "__main__":
    unittest.main()
