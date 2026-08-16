#!/usr/bin/env python3
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from subprocess import run
from threading import Lock, Thread
from tempfile import TemporaryDirectory
import json
import os
import socket
import sys
import time
from urllib.parse import urlparse


class RedirectHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, _format, *_args):
        pass

    def reply(self, status, body=b"", headers=()):
        self.send_response(status)
        for name, value in headers:
            self.send_header(name, value)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if body:
            self.wfile.write(body)

    def redirect(self, status, location, cookie):
        self.reply(status, b"redirect-body-must-not-appear", (
            ("Location", location),
            ("Content-Type", "text/plain"),
            ("ETag", '"intermediate"'),
            ("Critical-CH", "Sec-CH-UA-Arch"),
            ("X-Intermediate", "must-not-survive"),
            ("Set-Cookie", cookie),
        ))

    def filled_policy_redirect(self):
        # Location remains in the bounded page-visible snapshot, while the
        # late Referrer-Policy deliberately falls just beyond its capacity.
        # The transport's direct security metadata must still observe it.
        self.send_response_only(302)
        self.send_header("Location", "/final?policy_filled=1")
        self.send_header("X-Fill", "x" * 8125)
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header("Content-Length", "0")
        self.end_headers()

    def filled_policy_final(self):
        body = b"filled-policy-final"
        self.send_response_only(200)
        self.send_header("X-Fill", "x" * 8170)
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def filled_hsts_final(self):
        body = b"filled-hsts-final"
        self.send_response_only(200)
        self.send_header("X-Fill", "x" * 8170)
        self.send_header("Strict-Transport-Security", "max-age=3600")
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def early_hints(self):
        # Write an informational block explicitly; BaseHTTPRequestHandler's
        # send_response machinery otherwise treats it as the final response.
        self.wfile.write(
            b"HTTP/1.1 103 Early Hints\r\n"
            b"Set-Cookie: early=must-not-survive; Path=/\r\n"
            b"Accept-CH: Sec-CH-UA-Model\r\n"
            b"Critical-CH: Sec-CH-UA-Model\r\n"
            b"ETag: \"early\"\r\n"
            b"X-Early: must-not-survive\r\n\r\n")
        self.wfile.flush()

    def final(self, method, body=b""):
        query = urlparse(self.path).query
        cookie = self.headers.get("Cookie", "<none>")
        origin = self.headers.get("Origin", "<none>")
        referer = self.headers.get("Referer", "<none>")
        authorization = self.headers.get("Authorization", "<none>")
        public = self.headers.get("X-Public", "<none>")
        content_type = self.headers.get("Content-Type", "<none>")
        fetch_dest = self.headers.get("Sec-Fetch-Dest", "<none>")
        fetch_mode = self.headers.get("Sec-Fetch-Mode", "<none>")
        fetch_site = self.headers.get("Sec-Fetch-Site", "<none>")
        payload = (
            f"method={method};cookie={cookie};origin={origin};"
            f"referer={referer};authorization={authorization};"
            f"x-public={public};content-type={content_type};"
            f"fetch-dest={fetch_dest};"
            f"fetch-mode={fetch_mode};fetch-site={fetch_site};"
            f"query={query};wire-fragment={int('#' in self.path)};body="
        ).encode() + body
        self.reply(200, payload, (
            ("Content-Type", "text/plain"),
            ("ETag", '"final"'),
            ("X-Hop", "final"),
            ("Set-Cookie", "final_cookie=seen; Path=/"),
        ))

    def cors_final(self, method, body=b"", wildcard=False,
                   credentials=False, expose=True, duplicate=False):
        origin = self.headers.get("Origin", "<none>")
        cookie = self.headers.get("Cookie", "<none>")
        custom = self.headers.get("X-Test", "<none>")
        payload = (f"cors-method={method};origin={origin};cookie={cookie};"
                   f"x-test={custom};body=").encode() + body
        allow_origin = "*" if wildcard else origin
        headers = [
            ("Content-Type", "text/plain"),
            ("Access-Control-Allow-Origin", allow_origin),
            ("X-Cors-Secret", "hidden"),
            ("X-Cors-Visible", "visible"),
            ("Set-Cookie", "cors_response=seen; Path=/"),
        ]
        if credentials:
            headers.append(("Access-Control-Allow-Credentials", "true"))
        if duplicate:
            headers.append(("Access-Control-Allow-Origin", allow_origin))
        if expose:
            headers.append(("Access-Control-Expose-Headers",
                            "X-Cors-Visible"))
        self.reply(200, payload, tuple(headers))

    def script_metadata(self):
        cookie = self.headers.get("Cookie", "<none>")
        origin = self.headers.get("Origin", "<none>")
        fetch_dest = self.headers.get("Sec-Fetch-Dest", "<none>")
        fetch_mode = self.headers.get("Sec-Fetch-Mode", "<none>")
        fetch_site = self.headers.get("Sec-Fetch-Site", "<none>")
        summary = (f"method=GET;cookie={cookie};origin={origin};"
                   f"fetch-dest={fetch_dest};fetch-mode={fetch_mode};"
                   f"fetch-site={fetch_site}")
        body = ("globalThis.pocSummary=" + json.dumps(summary) + ";").encode()
        self.reply(200, body, (
            ("Content-Type", "text/javascript"),
            ("Set-Cookie", "script_final=seen; Path=/"),
        ))

    def high_hint_subset_is_arch_only(self):
        expected = self.headers.get("Sec-CH-UA-Arch") == '"MIPS"'
        forbidden = (
            "Sec-CH-UA-Bitness", "Sec-CH-UA-Model",
            "Sec-CH-UA-Platform-Version", "Sec-CH-UA-Full-Version",
            "Sec-CH-UA-Full-Version-List", "UA", "UA-Arch",
            "UA-Bitness", "UA-Model", "UA-Platform-Version",
            "UA-Full-Version", "UA-Full-Version-List",
        )
        return expected and all(self.headers.get(name) is None
                                for name in forbidden)

    def no_high_hints(self):
        names = (
            "Sec-CH-UA-Arch", "Sec-CH-UA-Bitness", "Sec-CH-UA-Model",
            "Sec-CH-UA-Platform-Version", "Sec-CH-UA-Full-Version",
            "Sec-CH-UA-Full-Version-List", "UA", "UA-Arch",
            "UA-Bitness", "UA-Model", "UA-Platform-Version",
            "UA-Full-Version", "UA-Full-Version-List",
        )
        return all(self.headers.get(name) is None for name in names)

    def do_GET(self):
        path = urlparse(self.path).path
        if path == "/same":
            self.redirect(302, "/final", "hop=seen; Path=/")
        elif path == "/fragment-inherit":
            self.redirect(302, "/final?fragment=inherit",
                          "fragment=inherit; Path=/")
        elif path == "/fragment-replace":
            self.redirect(302, "/final?fragment=replace#server",
                          "fragment=replace; Path=/")
        elif path == "/fragment-empty":
            self.redirect(302, "/final?fragment=empty#",
                          "fragment=empty; Path=/")
        elif path == "/fragment-chain-start":
            self.redirect(302, "/fragment-chain-middle#middle",
                          "fragment=start; Path=/")
        elif path == "/fragment-chain-middle":
            self.redirect(302, "/final?fragment=chain",
                          "fragment=middle; Path=/")
        elif path == "/referrer-policy-start":
            self.reply(302, b"referrer-policy-start", (
                ("Location", "/referrer-policy-middle"),
                ("Referrer-Policy", "no-referrer, origin, invalid-future"),
            ))
        elif path == "/referrer-policy-middle":
            port = self.server.server_address[1]
            expected = f"http://127.0.0.1:{port}/"
            observed = int(self.headers.get("Referer") == expected)
            self.reply(302, b"referrer-policy-middle", (
                ("Location", f"/final?policy_middle={observed}"),
            ))
        elif path == "/referrer-policy-invalid":
            self.reply(302, b"referrer-policy-invalid", (
                ("Location", "/final?policy_invalid=1"),
                ("Referrer-Policy", "invalid-one, invalid-two"),
            ))
        elif path == "/referrer-policy-filled":
            self.filled_policy_redirect()
        elif path == "/referrer-policy-filled-final":
            self.filled_policy_final()
        elif path == "/hsts-filled-final":
            self.filled_hsts_final()
        elif path == "/location-empty":
            key = self.path
            with self.server.state_lock:
                attempt = self.server.empty_location_attempts.get(key, 0)
                self.server.empty_location_attempts[key] = attempt + 1
            if attempt == 0:
                self.reply(302, headers=(("Location", ""),))
            else:
                self.final("GET")
        elif path == "/script-start":
            self.redirect(302, "/script-metadata",
                          "script_hop=seen; Path=/")
        elif path == "/script-metadata":
            self.script_metadata()
        elif path == "/callback-abort-shape":
            self.reply(302, b"redirect-body-must-not-appear" * 16, (
                ("Location", "/final?callback=followed"),
                ("Content-Type", "text/html; charset=UTF-8"),
                ("X-Hallmonitor-Challenge", "redirect-callback-shape"),
                ("Set-Cookie", "callback_a=seen; Path=/"),
                ("Set-Cookie", "callback_b=seen; Path=/"),
            ))
        elif path == "/accept-encoding":
            encoding = self.headers.get("Accept-Encoding", "<none>")
            self.reply(200, f"encoding={encoding}".encode(), (
                ("Content-Type", "text/plain"),
            ))
        elif path == "/private/start":
            cookie = self.headers.get("Cookie", "")
            ordered = int(cookie.startswith("id=private; id=base"))
            self.redirect(302, f"/public/final?ordered={ordered}",
                          "private_hop=seen; Path=/private")
        elif path == "/public/final":
            self.final("GET")
        elif path == "/cross":
            port = self.server.server_address[1]
            self.redirect(302, f"http://localhost:{port}/final",
                          "origin_only=seen; Path=/")
        elif path == "/hsts-redirect":
            port = self.server.server_address[1]
            self.redirect(302, f"http://hsts.test:{port}/final", "")
        elif path == "/stylesheet-cross":
            port = self.server.server_address[1]
            self.redirect(302, f"http://localhost:{port}/stylesheet-final", "")
        elif path == "/stylesheet-final":
            self.reply(200, b"#csp-probe{color:#123456}", (
                ("Content-Type", "text/css"),
            ))
        elif path == "/cross-port":
            self.redirect(302,
                          f"http://127.0.0.1:{self.server.cross_port}/final",
                          "origin_only=seen; Path=/")
        elif path == "/cross-cookie-policy":
            port = self.server.server_address[1]
            self.reply(302, b"policy redirect body", (
                ("Location", f"http://localhost:{port}/policy-hop"),
            ))
        elif path == "/policy-hop":
            self.reply(302, b"policy hop body", (
                ("Location", "/final"),
                ("Set-Cookie", "strict_hop=blocked; Path=/; SameSite=Strict"),
                ("Set-Cookie", "partitioned_hop=blocked; Path=/; Secure; Partitioned"),
            ))
        elif path == "/cross-port-return":
            own_port = self.server.server_address[1]
            self.redirect(
                302,
                f"http://127.0.0.1:{self.server.cross_port}"
                f"/return-port?port={own_port}",
                "origin_only=seen; Path=/")
        elif path == "/return-port":
            parsed = urlparse(self.path)
            return_port = parsed.query.removeprefix("port=")
            cross_received = int(self.headers.get("Cookie") is not None)
            self.redirect(
                302,
                f"http://127.0.0.1:{return_port}"
                f"/final?cross_received={cross_received}",
                "cross_only=blocked; Path=/")
        elif path == "/final":
            self.final("GET")
        elif path == "/cors-simple":
            query = urlparse(self.path).query
            self.cors_final(
                "GET", wildcard="wildcard=1" in query,
                credentials="credentials=1" in query,
                expose="expose=0" not in query,
                duplicate="duplicate=1" in query)
        elif path == "/cors-denied":
            self.reply(200, b"cors-body-must-not-be-exposed", (
                ("Content-Type", "text/plain"),
                ("X-Cors-Secret", "hidden"),
            ))
        elif path == "/cors-cross-redirect":
            target = (f"http://127.0.0.1:{self.server.cross_port}"
                      "/cors-simple")
            self.reply(302, b"cors-redirect-body", (
                ("Location", target),
                ("Access-Control-Allow-Origin",
                 self.headers.get("Origin", "null")),
            ))
        elif path == "/cors-chain-same-cross":
            allow = "allow=1" in urlparse(self.path).query
            target = (f"http://127.0.0.1:{self.server.cross_port}"
                      f"/cors-chain-final?allow={int(allow)}")
            self.reply(302, b"same-cross-redirect-body", (
                ("Location", target),
            ))
        elif path == "/cors-chain-cross-unreachable":
            target = (f"http://127.0.0.1:{self.server.blocked_port}"
                      "/cors-chain-never-reached")
            self.reply(302, b"cross-unreachable-redirect-body", (
                ("Location", target),
            ))
        elif path == "/cors-cache-304":
            query = urlparse(self.path).query
            headers = [("ETag", '"cors-cache"')]
            if "acao=origin" in query:
                headers.append(("Access-Control-Allow-Origin",
                                self.headers.get("Origin", "null")))
            elif "acao=bad" in query:
                headers.append(("Access-Control-Allow-Origin",
                                "http://invalid.test"))
            if "acac=true" in query:
                headers.append(("Access-Control-Allow-Credentials", "true"))
            elif "acac=false" in query:
                headers.append(("Access-Control-Allow-Credentials", "false"))
            self.reply(304, headers=tuple(headers))
        elif path == "/cors-chain-cross-same":
            query = urlparse(self.path).query
            allow_redirect = "allow_redirect=1" in query
            allow_final = "allow_final=1" in query
            target = (f"http://127.0.0.1:{self.server.cross_port}"
                      f"/cors-chain-final?allow={int(allow_final)}")
            headers = [("Location", target)]
            if allow_redirect:
                headers.append(("Access-Control-Allow-Origin",
                                self.headers.get("Origin", "null")))
            self.reply(302, b"cross-same-redirect-body", tuple(headers))
        elif path == "/cors-chain-same-cross-same":
            query = urlparse(self.path).query
            allow_middle = "allow_middle=1" in query
            allow_final = "allow_final=1" in query
            own_port = self.server.server_address[1]
            target = (f"http://127.0.0.1:{self.server.cross_port}"
                      "/cors-chain-cross-return"
                      f"?return_port={own_port}"
                      f"&allow_middle={int(allow_middle)}"
                      f"&allow_final={int(allow_final)}")
            self.reply(302, b"same-cross-same-initial-body", (
                ("Location", target),
            ))
        elif path == "/cors-chain-cross-return":
            query = dict(part.split("=", 1)
                         for part in urlparse(self.path).query.split("&")
                         if "=" in part)
            target = (f"http://127.0.0.1:{query.get('return_port', '')}"
                      "/cors-chain-final"
                      f"?allow={query.get('allow_final', '0')}")
            headers = [("Location", target)]
            if query.get("allow_middle") == "1":
                headers.append(("Access-Control-Allow-Origin",
                                self.headers.get("Origin", "null")))
            self.reply(302, b"cross-return-redirect-body", tuple(headers))
        elif path == "/cors-chain-final":
            allow = "allow=1" in urlparse(self.path).query
            origin = self.headers.get("Origin", "<none>")
            cookie = self.headers.get("Cookie", "<none>")
            payload = (f"cors-chain-final;origin={origin};cookie={cookie}"
                       ).encode()
            headers = [("Content-Type", "text/plain")]
            if allow:
                headers.append(("Access-Control-Allow-Origin", origin))
            self.reply(200, payload, tuple(headers))
        elif path == "/binary":
            self.reply(200, b"\x00A\xc3\xa9B", (
                ("Content-Type", "application/octet-stream"),
            ))
        elif path == "/binary-json":
            self.reply(200, b'{"ok":true,"n":7}', (
                ("Content-Type", "application/octet-stream"),
            ))
        elif path == "/large":
            self.reply(200, bytes((index * 37 + 11) & 0xff
                                  for index in range(48 * 1024)), (
                ("Content-Type", "application/octet-stream"),
            ))
        elif path == "/early-final":
            self.early_hints()
            self.final("GET")
        elif path == "/early-redirect":
            self.early_hints()
            self.redirect(302, "/final", "hop=early-redirect; Path=/")
        elif path == "/loop":
            self.reply(302, b"loop-body", (("Location", "/loop"),))
        elif path == "/slow-redirect":
            self.send_response(302)
            self.send_header("Location", "/final")
            self.send_header("Content-Length", "1")
            self.end_headers()
            time.sleep(1.0)
            try:
                self.wfile.write(b"x")
            except (BrokenPipeError, ConnectionResetError):
                pass
        elif path == "/max-cookie":
            self.reply(200, b"max-cookie", (
                ("Set-Cookie", "x=" + "a" * 4093),
            ))
        elif path == "/oversized-cookie":
            self.reply(200, b"oversized-cookie", (
                ("Set-Cookie", "x=" + "a" * 4094),
            ))
        elif path == "/response-cookie-capacity":
            self.reply(200, b"response-cookie-capacity", tuple(
                ("Set-Cookie", f"cookie{i}=value; Path=/")
                for i in range(16)))
        elif path == "/response-cookie-overflow":
            self.reply(200, b"response-cookie-overflow", tuple(
                ("Set-Cookie", f"cookie{i}=value; Path=/")
                for i in range(17)))
        elif path == "/duplicate-client-hints":
            self.reply(200, b"duplicate-client-hints", (
                ("Accept-CH", "Sec-CH-UA-Arch"),
                ("Accept-CH", "Sec-CH-UA-Bitness"),
                ("Critical-CH", "Sec-CH-UA-Arch"),
                ("Critical-CH", "Sec-CH-UA-Bitness"),
            ))
        elif path == "/oversized-client-hints":
            self.reply(200, b"oversized-client-hints", (
                ("Accept-CH", "A" * 600),
                ("Accept-CH", "B" * 600),
            ))
        elif path == "/nav-start":
            self.redirect(302, "/nav-final#server", "nav=seen; Path=/")
        elif path == "/nav-final":
            self.reply(200, (
                b"<!doctype html><html><head><title>Redirected</title>"
                b"</head><body>final navigation</body></html>"), (
                ("Content-Type", "text/html"),
            ))
        elif path == "/hint-retry":
            parsed = urlparse(self.path)
            parameters = dict(part.split("=", 1)
                              for part in parsed.query.split("&") if "=" in part)
            target_port = parameters.get("target_port", "")
            target_host = parameters.get("target_host", "127.0.0.1")
            subset = int(self.high_hint_subset_is_arch_only())
            terminal_path = "/hint-bounce" if parameters.get("bounce") == "1" \
                else "/hint-terminal"
            source_port = self.server.server_address[1]
            self.reply(302, b"scoped retry redirect body", (
                ("Location", f"http://{target_host}:{target_port}{terminal_path}"
                             f"?subset={subset}&source_port={source_port}"),
            ))
        elif path == "/hint-bounce":
            parsed = urlparse(self.path)
            parameters = dict(part.split("=", 1)
                              for part in parsed.query.split("&") if "=" in part)
            clean = int(self.no_high_hints())
            self.reply(302, b"cross-origin bounce body", (
                ("Location", f"http://127.0.0.1:{parameters.get('source_port', '')}"
                             f"/hint-terminal?subset={parameters.get('subset', '0')}"
                             f"&bounce_clean={clean}"),
            ))
        elif path == "/hint-terminal":
            parsed = urlparse(self.path)
            subset = "subset=1" in parsed.query
            bounce_clean = "bounce_clean=0" not in parsed.query
            no_high = self.no_high_hints()
            low = self.headers.get("Sec-CH-UA") is not None
            body = (f"subset={int(subset and bounce_clean)};terminal-high="
                    f"{int(not no_high)};terminal-low={int(low)}").encode()
            self.reply(200, body, (("Content-Type", "text/plain"),))
        elif path == "/critical-chain-start":
            own_port = self.server.server_address[1]
            start_clean = int(self.no_high_hints())
            target = (f"http://127.0.0.1:{self.server.cross_port}/"
                      f"critical-chain?source_port={own_port}"
                      f"&start_clean={start_clean}")
            self.reply(302, b"initial redirect body", (("Location", target),))
        elif path == "/critical-chain":
            parsed = urlparse(self.path)
            parameters = dict(part.split("=", 1)
                              for part in parsed.query.split("&") if "=" in part)
            source_port = parameters.get("source_port", "")
            has_arch = self.headers.get("Sec-CH-UA-Arch") == '"MIPS"'
            if not has_arch:
                body = (
                    b"<!doctype html><title>Discarded scoped body</title>"
                    b"<script>document.cookie='discarded_hint_body=ran; "
                    b"Path=/critical-chain'</script>"
                )
                self.reply(200, body, (
                    ("Content-Type", "text/html"),
                    ("Accept-CH", "Sec-CH-UA-Arch, Sec-CH-UA-Bitness"),
                    ("Critical-CH", "Sec-CH-UA-Arch"),
                    ("Set-Cookie", "retry_path=seen; Path=/critical-chain"),
                ))
            else:
                expected_referrer = f"http://127.0.0.1:{source_port}/"
                scoped = (self.high_hint_subset_is_arch_only()
                          and parameters.get("start_clean") == "1"
                          and self.headers.get("Referer") == expected_referrer
                          # Ports do not split a schemeful site. Both hops use
                          # 127.0.0.1, so the PSL-backed request policy must
                          # report same-site even though their origins differ.
                          and self.headers.get("Sec-Fetch-Site") == "same-site"
                          and "retry_path=seen" in self.headers.get("Cookie", ""))
                target = (f"http://localhost:{source_port}/critical-terminal"
                          f"?scoped={int(scoped)}")
                self.reply(302, b"retry redirect body", (("Location", target),))
        elif path == "/critical-terminal":
            scoped = "scoped=1" in urlparse(self.path).query
            no_high = self.no_high_hints()
            low = self.headers.get("Sec-CH-UA") is not None
            state = (f"critical-scope:{int(scoped)}:{int(no_high)}:"
                     f"{int(low)}")
            body = ("<!doctype html><title>Scoped Critical-CH final</title>"
                    f"<script>globalThis.pocSummary='{state}'</script>"
                    "<p>final scoped response</p>").encode()
            self.reply(200, body, (("Content-Type", "text/html"),))
        elif path == "/critical-without-accept":
            high = int(self.headers.get("Sec-CH-UA-Arch") == '"MIPS"')
            body = ("<!doctype html><title>No unaccepted retry</title>"
                    f"<script>globalThis.pocSummary='no-retry:{high}'</script>"
                    "<p>Critical-CH without Accept-CH</p>").encode()
            self.reply(200, body, (
                ("Content-Type", "text/html"),
                ("Critical-CH", "Sec-CH-UA-Arch"),
            ))
        elif path == "/method":
            self.final("GET")
        else:
            self.reply(404)

    def body_method(self, method):
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length)
        path = urlparse(self.path).path
        if path == "/custom307":
            self.redirect(307, "/method", "custom307=seen; Path=/")
        elif path == "/method":
            self.final(method, body)
        elif path == "/cors-preflight":
            self.cors_final(
                method, body,
                credentials="credentials=1" in urlparse(self.path).query)
        elif path == "/cors-preflight-denied":
            self.cors_final(method, body)
        else:
            self.reply(404)

    def do_PUT(self):
        self.body_method("PUT")

    def do_PATCH(self):
        self.body_method("PATCH")

    def do_HEAD(self):
        if urlparse(self.path).path != "/head":
            self.reply(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        # A HEAD response reports the corresponding GET length but sends no
        # bytes. A client using only CUSTOMREQUEST without NOBODY will wait.
        self.send_header("Content-Length", "17")
        self.end_headers()

    def do_OPTIONS(self):
        path = urlparse(self.path).path
        if path not in ("/cors-preflight", "/cors-preflight-denied"):
            self.reply(404)
            return
        origin = self.headers.get("Origin", "")
        method = self.headers.get("Access-Control-Request-Method", "")
        headers = self.headers.get("Access-Control-Request-Headers", "")
        valid = (origin.startswith("http://localhost:")
                 and method == "PUT"
                 and headers == "content-type, x-test")
        if not valid:
            self.reply(403, b"invalid preflight request")
            return
        response = [
            ("Access-Control-Allow-Methods", "PUT"),
            ("Access-Control-Allow-Headers", "content-type, x-test"),
        ]
        if path == "/cors-preflight":
            response.append(("Access-Control-Allow-Origin", origin))
            if "credentials=1" in urlparse(self.path).query:
                response.append(("Access-Control-Allow-Credentials", "true"))
        self.reply(204, headers=tuple(response))

    def do_CONNECT(self):
        self.send_response(200, "Connection established")
        self.send_header("Set-Cookie", "proxy_injected=blocked; Path=/")
        self.send_header("Accept-CH", "Sec-CH-UA-Arch")
        self.send_header("Critical-CH", "Sec-CH-UA-Arch")
        self.send_header("X-Proxy-Only", "must-not-reach-page")
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length)
        path = urlparse(self.path).path
        if path == "/post302":
            self.redirect(302, "/method", "post302=seen; Path=/")
        elif path == "/post307":
            self.redirect(307, "/method", "post307=seen; Path=/")
        elif path == "/method":
            self.final("POST", body)
        else:
            self.reply(404)


class QuietThreadingHTTPServer(ThreadingHTTPServer):
    daemon_threads = True

    def handle_error(self, _request, _client_address):
        # Header-time redirect termination deliberately resets keep-alive
        # connections before the fixture can write its unused response body.
        pass


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: run_fetch_redirect_test.py TEST_EXECUTABLE")
    probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        probe.bind(("127.0.0.1", 0))
    except PermissionError as error:
        print(f"redirect integration skipped: loopback unavailable: {error}")
        return 77
    finally:
        probe.close()
    server = QuietThreadingHTTPServer(("127.0.0.1", 0), RedirectHandler)
    cross_server = QuietThreadingHTTPServer(("127.0.0.1", 0), RedirectHandler)
    proxy_server = QuietThreadingHTTPServer(("127.0.0.1", 0), RedirectHandler)
    blocked_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    blocked_socket.bind(("127.0.0.1", 0))
    server.cross_port = cross_server.server_address[1]
    cross_server.cross_port = server.server_address[1]
    server.blocked_port = blocked_socket.getsockname()[1]
    cross_server.blocked_port = blocked_socket.getsockname()[1]
    for fixture in (server, cross_server, proxy_server):
        fixture.state_lock = Lock()
        fixture.empty_location_attempts = {}
    thread = Thread(target=server.serve_forever, daemon=True)
    cross_thread = Thread(target=cross_server.serve_forever, daemon=True)
    proxy_thread = Thread(target=proxy_server.serve_forever, daemon=True)
    thread.start()
    cross_thread.start()
    proxy_thread.start()
    try:
        with TemporaryDirectory(prefix="tilefinch-redirect-trace-") as trace:
            environment = os.environ.copy()
            for name in ("HTTP_PROXY", "http_proxy", "ALL_PROXY",
                         "all_proxy", "NO_PROXY", "no_proxy"):
                environment.pop(name, None)
            proxy = f"http://127.0.0.1:{proxy_server.server_address[1]}"
            environment["HTTPS_PROXY"] = proxy
            environment["https_proxy"] = proxy
            completed = run(
                [sys.argv[1], str(server.server_address[1]),
                 str(cross_server.server_address[1]),
                 str(proxy_server.server_address[1]), trace],
                check=False, env=environment)
            if completed.returncode != 0:
                print(f"redirect test executable exited {completed.returncode}",
                      file=sys.stderr)
            return completed.returncode
    finally:
        blocked_socket.close()
        server.shutdown()
        server.server_close()
        cross_server.shutdown()
        cross_server.server_close()
        proxy_server.shutdown()
        proxy_server.server_close()
        thread.join()
        cross_thread.join()
        proxy_thread.join()


if __name__ == "__main__":
    raise SystemExit(main())
