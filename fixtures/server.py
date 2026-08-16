from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import json
import time
import urllib.parse
import urllib.request
from urllib.parse import urlparse

ROOT = Path(__file__).resolve().parent


class FixtureHandler(SimpleHTTPRequestHandler):
    def translate_path(self, path):
        relative = urlparse(path).path.lstrip("/") or "interactive.html"
        return str(ROOT / relative)

    def do_GET(self):
        path = urlparse(self.path).path
        if path == "/slow-style.css":
            time.sleep(0.4)
            body = b"main { color: #123456; }"
            self.send_response(200)
            self.send_header("Content-Type", "text/css")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if path == "/critical-hints.html":
            has_arch = self.headers.get("Sec-CH-UA-Arch") == '"MIPS"'
            if not has_arch:
                body = (b"<!doctype html><title>Pre-hint response</title>"
                        b"<script>globalThis.pocSummary='critical-hints:pre'"
                        b"</script><p>PRE-HINT RESPONSE</p>")
                self.send_response(200)
                self.send_header("Content-Type", "text/html")
                self.send_header("Accept-CH", "Sec-CH-UA-Arch")
                self.send_header("Critical-CH", "Sec-CH-UA-Arch")
                self.send_header("Set-Cookie", "hint_retry=seen; Path=/")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            cookie_seen = "hint_retry=seen" in self.headers.get("Cookie", "")
            state = "cookie" if cookie_seen else "missing-cookie"
            navigation = "navigation" if (
                self.headers.get("Sec-Fetch-User") == "?1"
                and self.headers.get("Upgrade-Insecure-Requests") == "1"
            ) else "missing-navigation"
            body = ("<!doctype html><title>Post-hint response</title>"
                    f"<script>globalThis.pocSummary="
                    f"'critical-hints:post:{state}:{navigation}'"
                    "</script><p>POST-HINT RESPONSE</p>").encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if path == "/critical-frame-parent.html":
            body = (
                b"<!doctype html><title>Critical frame parent</title>"
                b"<script src='http://127.0.0.1:8765/"
                b"referrer-script.js'></script>"
                b"<script>addEventListener('message',event=>"
                b"globalThis.pocSummary='critical-frame:'+event.data+':'"
                b"+globalThis.scriptReferrer)"
                b"</script><iframe src='http://localhost:8765/"
                b"critical-frame.html'></iframe>"
            )
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Referrer-Policy", "same-origin")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if path == "/referrer-script.js":
            clean = (
                self.headers.get("Referer") is None
                and self.headers.get("Sec-Fetch-User") is None
                and self.headers.get("Upgrade-Insecure-Requests") is None
            )
            state = "clean-script-metadata" if clean \
                else "unexpected-script-metadata"
            body = f"globalThis.scriptReferrer='{state}';".encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/javascript")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if path == "/critical-frame.html":
            has_arch = self.headers.get("Sec-CH-UA-Arch") == '"MIPS"'
            has_language = self.headers.get("Accept-Language") == \
                "en-US,en;q=0.9"
            if not has_arch:
                body = (
                    b"<!doctype html><title>Pre-hint frame</title>"
                    b"<script>parent.postMessage('pre','*')</script>"
                )
                self.send_response(200)
                self.send_header("Content-Type", "text/html")
                self.send_header("Accept-CH", "Sec-CH-UA-Arch")
                self.send_header("Critical-CH", "Sec-CH-UA-Arch")
                self.send_header(
                    "Set-Cookie", "frame_hint_retry=seen; Path=/")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            cookie_seen = "frame_hint_retry=seen" in self.headers.get(
                "Cookie", "")
            state = "cookie" if cookie_seen else "missing-cookie"
            language = "language" if has_language else "missing-language"
            referrer = "same-origin-referrer" if self.headers.get(
                "Referer") == (
                    "http://localhost:8765/critical-frame-parent.html") \
                else "unexpected-referrer"
            navigation = "frame-navigation" if (
                self.headers.get("Sec-Fetch-User") is None
                and self.headers.get("Upgrade-Insecure-Requests") == "1"
            ) else "unexpected-frame-navigation"
            body = (
                "<!doctype html><title>Post-hint frame</title>"
                f"<script>parent.postMessage("
                f"'post:{state}:{language}:{referrer}:{navigation}','*')"
                "</script>"
            ).encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if path in ("/api", "/slow-api"):
            if path == "/slow-api":
                time.sleep(0.25)
            cookie_seen = "mode=interactive" in self.headers.get("Cookie", "")
            body = (b'{"value":"yes"}' if cookie_seen
                    else b'{"value":"missing-cookie"}')
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("X-Tilefinch-Response", "visible")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if path == "/binary":
            body = bytes((0, 255, 128, 65))
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if path == "/next":
            body = b"<!doctype html><title>Next</title><p>NEXT PAGE</p>"
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        file_path = Path(self.translate_path(self.path))
        if file_path.suffix == ".js" and file_path.exists():
            etag = f'"{file_path.name}-v1"'
            if self.headers.get("If-None-Match") == etag:
                self.send_response(304)
                self.send_header("ETag", etag)
                self.end_headers()
                return
            body = file_path.read_bytes()
            if file_path.name == "external.js" and "mode=interactive" in self.headers.get("Cookie", ""):
                body += b"\nglobalThis.cookieTransport='yes';\n"
            self.send_response(200)
            self.send_header("Content-Type", "text/javascript")
            self.send_header("ETag", etag)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if path in ("/", "/interactive.html"):
            self.send_response(200)
            body = (ROOT / "interactive.html").read_bytes()
            self.send_header("Content-Type", "text/html")
            self.send_header("Set-Cookie", "server=seen; Path=/")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        super().do_GET()

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        submitted_bytes = self.rfile.read(length)
        submitted = submitted_bytes.decode("ascii", "replace")
        if urlparse(self.path).path == "/api":
            body = json.dumps({
                "origin": self.headers.get("Origin", "missing"),
                "body": submitted,
                "bytes": "-".join(str(value) for value in submitted_bytes),
            }, separators=(",", ":")).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if urlparse(self.path).path == "/turnstile-submit":
            fields = urllib.parse.parse_qs(submitted)
            token = fields.get("cf-turnstile-response", [""])[0]
            payload = urllib.parse.urlencode({
                "secret": "1x0000000000000000000000000000000AA",
                "response": token,
            }).encode()
            request = urllib.request.Request(
                "https://challenges.cloudflare.com/turnstile/v0/siteverify",
                data=payload,
                headers={"Content-Type": "application/x-www-form-urlencoded"},
            )
            try:
                with urllib.request.urlopen(request, timeout=15) as response:
                    result = json.loads(response.read())
                success = result.get("success") is True
                detail = ",".join(result.get("error-codes", [])) or "none"
            except Exception as error:
                success = False
                detail = type(error).__name__
            body = ("<!doctype html><title>Turnstile result</title>"
                    f"<p id=result>TURNSTILE {'PASS' if success else 'FAIL'} "
                    f"ERRORS {detail}</p>").encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        cookie = self.headers.get("Cookie", "")
        body = ("<!doctype html><title>Submitted</title>"
                f"<p id=result>POST {submitted} COOKIE {cookie}</p>").encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Set-Cookie", "posted=yes; Path=/")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


if __name__ == "__main__":
    ThreadingHTTPServer(("127.0.0.1", 8765), FixtureHandler).serve_forever()
