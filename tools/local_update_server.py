#!/usr/bin/env python3
"""Bounded HTTPS origin used by Tilefinch's local signed-update qualification.

The server intentionally exposes only the two release artifacts named on its
command line.  It supports the single byte-range form used by the update
transport and a small set of deterministic transport faults; it is not a
general-purpose file server.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import socket
import ssl
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from socketserver import TCPServer
from urllib.parse import urlsplit


RANGE_RE = re.compile(r"bytes=(\d+)-(\d*)\Z")
MAX_REQUESTS = 512


class BoundedThreadingHTTPServer(ThreadingHTTPServer):
    def server_bind(self) -> None:
        # HTTPServer.server_bind() performs socket.getfqdn() on the listening
        # address. A local qualification must not depend on external DNS, and
        # macOS can leave that lookup blocked for seconds while PPSSPP owns the
        # network service. Bind directly and retain the literal endpoint.
        TCPServer.server_bind(self)
        host, port = self.server_address[:2]
        self.server_name = str(host)
        self.server_port = int(port)


class UpdateOrigin:
    def __init__(self, directory: Path, metadata: str, package: str,
                 fault: str, log_path: Path | None) -> None:
        self.directory = directory.resolve(strict=True)
        self.metadata = metadata
        self.package = package
        self.fault = fault
        self.log_path = log_path
        self.lock = threading.Lock()
        self.requests = 0
        self.package_faults = 0

    def artifact(self, request_path: str) -> tuple[Path, str] | None:
        path = urlsplit(request_path).path
        mapping = {
            "/" + self.metadata: (self.directory / self.metadata, "metadata"),
            "/" + self.package: (self.directory / self.package, "package"),
        }
        selected = mapping.get(path)
        if selected is None:
            return None
        resolved = selected[0].resolve(strict=True)
        if resolved.parent != self.directory or not resolved.is_file():
            return None
        return resolved, selected[1]

    def record(self, **fields: object) -> None:
        with self.lock:
            self.requests += 1
            fields["request"] = self.requests
            fields["time_ns"] = time.time_ns()
            line = json.dumps(fields, sort_keys=True, separators=(",", ":"))
            if self.log_path is not None:
                with self.log_path.open("a", encoding="utf-8") as stream:
                    stream.write(line + "\n")
            print(line, flush=True)

    def consume_drop_once(self) -> bool:
        with self.lock:
            if self.fault != "drop-package-once" or self.package_faults != 0:
                return False
            self.package_faults += 1
            return True


class UpdateHandler(BaseHTTPRequestHandler):
    server_version = "TilefinchUpdateTest/1"
    protocol_version = "HTTP/1.1"

    @property
    def origin(self) -> UpdateOrigin:
        return self.server.origin  # type: ignore[attr-defined]

    def log_message(self, _format: str, *args: object) -> None:
        del args

    def _send_artifact(self, include_body: bool) -> None:
        if self.origin.requests >= MAX_REQUESTS:
            self.send_error(429, "request bound exceeded")
            return
        selected = self.origin.artifact(self.path)
        if selected is None:
            self.origin.record(method=self.command, path=self.path,
                               outcome="not-found")
            self.send_error(404)
            return
        artifact, kind = selected
        payload = artifact.read_bytes()
        if kind == "metadata" and self.origin.fault == "corrupt-metadata":
            if payload:
                payload = payload[:-1] + bytes([payload[-1] ^ 0x01])

        start = 0
        end = len(payload) - 1
        status = 200
        range_value = self.headers.get("Range")
        if range_value:
            match = RANGE_RE.fullmatch(range_value.strip())
            if match is None:
                self.send_error(416)
                return
            start = int(match.group(1))
            end = int(match.group(2)) if match.group(2) else end
            if start >= len(payload) or end < start:
                self.send_error(416)
                return
            end = min(end, len(payload) - 1)
            status = 206

        body = payload[start:end + 1]
        drop = kind == "package" and include_body \
            and self.origin.consume_drop_once()
        truncate = kind == "package" and include_body \
            and self.origin.fault == "truncate-package"
        self.send_response(status)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Connection", "close")
        if status == 206:
            self.send_header(
                "Content-Range", f"bytes {start}-{end}/{len(payload)}")
        self.end_headers()

        sent = 0
        outcome = "ok"
        if include_body:
            if drop or truncate:
                limit = max(1, min(len(body) // 2, 65536))
                self.wfile.write(body[:limit])
                self.wfile.flush()
                sent = limit
                outcome = "dropped" if drop else "truncated"
                try:
                    self.connection.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
                self.close_connection = True
            else:
                self.wfile.write(body)
                sent = len(body)
        self.origin.record(method=self.command, path=self.path, kind=kind,
                           status=status, range=range_value or "", sent=sent,
                           declared=len(body), outcome=outcome)

    def do_GET(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        self._send_artifact(True)

    def do_HEAD(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        self._send_artifact(False)

    def do_POST(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        self.send_error(405)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--directory", required=True, type=Path)
    parser.add_argument("--certificate", required=True, type=Path)
    parser.add_argument("--private-key", required=True, type=Path)
    parser.add_argument("--metadata", default="tilefinch-update-v1.tfum")
    parser.add_argument("--package", default="tilefinch-update-v1.tfup")
    parser.add_argument("--bind", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=0)
    parser.add_argument("--port-file", type=Path)
    parser.add_argument("--log", type=Path)
    parser.add_argument(
        "--fault", choices=("normal", "drop-package-once",
                            "truncate-package", "corrupt-metadata"),
        default="normal")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if not (1 <= len(args.metadata) <= 128
            and 1 <= len(args.package) <= 128
            and "/" not in args.metadata and "/" not in args.package):
        raise SystemExit("artifact names must be bounded basenames")
    origin = UpdateOrigin(args.directory, args.metadata, args.package,
                          args.fault, args.log)
    server = BoundedThreadingHTTPServer(
        (args.bind, args.port), UpdateHandler)
    server.daemon_threads = True
    server.origin = origin  # type: ignore[attr-defined]
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    context.load_cert_chain(args.certificate, args.private_key)
    server.socket = context.wrap_socket(server.socket, server_side=True)
    port = server.server_address[1]
    if args.port_file is not None:
        temporary = args.port_file.with_suffix(args.port_file.suffix + ".tmp")
        temporary.write_text(str(port) + "\n", encoding="ascii")
        os.replace(temporary, args.port_file)
    print(json.dumps({"event": "ready", "bind": args.bind, "port": port,
                      "fault": args.fault}, sort_keys=True), flush=True)
    try:
        server.serve_forever(poll_interval=0.1)
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
