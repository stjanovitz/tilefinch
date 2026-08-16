#!/usr/bin/env python3
"""Serve decoded response bodies from a Tilefinch HTTP trace on loopback.

The server is deliberately a lab adapter, not a network proxy.  A request's
method and raw path plus query select the captured original URL.  Response
bodies are never decoded, encoded, or rewritten by this tool: the trace has
already stored the transport-decoded bytes, and those bytes are written to the
socket verbatim.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from dataclasses import dataclass
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Iterable, Sequence
from urllib.parse import urlsplit


LOOPBACK_ADDRESS = "127.0.0.1"
TRACE_FILE = re.compile(r"[0-9]{4}\.meta\Z")
HEADER_FILE_KEY = re.compile(r"response-header-([0-9]+)\Z")
HEADER_NAME = re.compile(r"[!#$%&'*+.^_`|~0-9A-Za-z-]+\Z")

# Values tied to the captured transfer cannot be replayed after libcurl has
# decoded the body.  The server emits a new, accurate Content-Length.
STRIPPED_RESPONSE_HEADERS = frozenset(
    {
        "connection",
        "content-encoding",
        "content-length",
        "keep-alive",
        "proxy-authenticate",
        "proxy-authorization",
        "proxy-connection",
        "te",
        "trailer",
        "transfer-encoding",
        "upgrade",
    }
)


class TraceError(ValueError):
    """The on-disk trace cannot be replayed truthfully."""


@dataclass(frozen=True)
class CapturedResponse:
    record_id: str
    method: str
    original_url: str
    target: str
    success: bool
    status: int
    content_type: str
    headers: tuple[tuple[str, str], ...]
    body: bytes
    error: str

    @property
    def response_signature(self) -> tuple[int, str, str]:
        return (
            self.status,
            self.content_type,
            hashlib.sha256(self.body).hexdigest(),
        )


@dataclass(frozen=True)
class ReplayRoute:
    selected: CapturedResponse | None
    candidates: tuple[CapturedResponse, ...]

    @property
    def ambiguous(self) -> bool:
        return self.selected is None


@dataclass(frozen=True)
class TraceIndex:
    trace_dir: Path
    routes: dict[tuple[str, str], ReplayRoute]
    methods_by_target: dict[str, tuple[str, ...]]
    entry: CapturedResponse
    record_count: int


def _metadata(path: Path) -> dict[str, str]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeError) as exc:
        raise TraceError(f"cannot read metadata {path}: {exc}") from exc
    result: dict[str, str] = {}
    for line_number, line in enumerate(lines, 1):
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            raise TraceError(f"{path}:{line_number}: expected key=value")
        name, value = line.split("=", 1)
        if not name:
            raise TraceError(f"{path}:{line_number}: empty metadata key")
        result[name] = value
    return result


def _integer(meta: dict[str, str], name: str, source: Path) -> int:
    value = meta.get(name)
    if value is None:
        raise TraceError(f"{source}: missing {name}")
    try:
        return int(value, 10)
    except ValueError as exc:
        raise TraceError(f"{source}: invalid {name}={value!r}") from exc


def _target_for(original_url: str, source: Path) -> str:
    if "\r" in original_url or "\n" in original_url:
        raise TraceError(f"{source}: URL contains a newline")
    parts = urlsplit(original_url)
    if parts.scheme not in ("http", "https") or not parts.netloc:
        raise TraceError(f"{source}: url is not an absolute HTTP(S) URL")
    target = parts.path or "/"
    if parts.query:
        target += "?" + parts.query
    return target


def _response_headers(
    meta: dict[str, str], source: Path
) -> tuple[tuple[str, str], ...]:
    indexed: list[tuple[int, str]] = []
    for key, value in meta.items():
        match = HEADER_FILE_KEY.fullmatch(key)
        if match:
            indexed.append((int(match.group(1), 10), value))
    indexed.sort()
    declared = meta.get("response-header-count")
    if declared is not None:
        try:
            declared_count = int(declared, 10)
        except ValueError as exc:
            raise TraceError(
                f"{source}: invalid response-header-count={declared!r}"
            ) from exc
        if declared_count != len(indexed):
            raise TraceError(
                f"{source}: response-header-count={declared_count} but found "
                f"{len(indexed)} headers"
            )
    headers: list[tuple[str, str]] = []
    for expected, (index, line) in enumerate(indexed):
        if index != expected:
            raise TraceError(
                f"{source}: response headers are not contiguous at index {expected}"
            )
        if ":" not in line:
            raise TraceError(f"{source}: malformed response-header-{index}")
        name, value = line.split(":", 1)
        name = name.strip()
        value = value.strip()
        if not HEADER_NAME.fullmatch(name) or "\r" in value or "\n" in value:
            raise TraceError(f"{source}: unsafe response-header-{index}")
        if name.lower() not in STRIPPED_RESPONSE_HEADERS:
            headers.append((name, value))
    return tuple(headers)


def _load_record(meta_path: Path) -> CapturedResponse:
    meta = _metadata(meta_path)
    method = meta.get("method", "").upper()
    if not method or not HEADER_NAME.fullmatch(method):
        raise TraceError(f"{meta_path}: missing or invalid method")
    original_url = meta.get("url", "")
    target = _target_for(original_url, meta_path)
    status = _integer(meta, "status", meta_path)
    if status != 0 and not 100 <= status <= 599:
        raise TraceError(f"{meta_path}: HTTP status is outside 100..599")
    expected_length = _integer(meta, "length", meta_path)
    if expected_length < 0:
        raise TraceError(f"{meta_path}: negative response length")
    body_path = meta_path.with_suffix(".body")
    if body_path.exists():
        try:
            body = body_path.read_bytes()
        except OSError as exc:
            raise TraceError(f"cannot read body {body_path}: {exc}") from exc
    elif expected_length == 0:
        body = b""
    else:
        raise TraceError(
            f"{meta_path}: missing {body_path.name} for {expected_length} bytes"
        )
    if len(body) != expected_length:
        raise TraceError(
            f"{meta_path}: length={expected_length} but body has {len(body)} bytes"
        )
    recorded_hash = meta.get("response-body-hash")
    if recorded_hash is not None:
        actual_hash = 0xCBF29CE484222325
        for byte in body:
            actual_hash ^= byte
            actual_hash = (actual_hash * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
        if recorded_hash != f"{actual_hash:016x}":
            raise TraceError(f"{meta_path}: response-body-hash does not match body")
    success_value = meta.get("success", "0")
    if success_value not in ("0", "1"):
        raise TraceError(f"{meta_path}: invalid success={success_value!r}")
    content_type = meta.get("content-type", "").strip()
    if "\r" in content_type or "\n" in content_type:
        raise TraceError(f"{meta_path}: content-type contains a newline")
    return CapturedResponse(
        record_id=meta_path.stem,
        method=method,
        original_url=original_url,
        target=target,
        success=success_value == "1",
        status=status,
        content_type=content_type,
        headers=_response_headers(meta, meta_path),
        body=body,
        error=meta.get("error", ""),
    )


def _route(records: Sequence[CapturedResponse]) -> ReplayRoute:
    # A live capture can record a transport failure followed by a successful
    # retry.  Prefer successful HTTP records, then any record with a real HTTP
    # status (including an intentionally retained 4xx/5xx response).
    successful = [record for record in records if record.success and record.status]
    eligible = successful or [record for record in records if record.status]
    if not eligible:
        # Status 0 is representable only as a diagnostic 502.  Repeated copies
        # of the same failure are still deterministic.
        eligible = list(records)
    signatures = {record.response_signature for record in eligible}
    if len(signatures) != 1:
        return ReplayRoute(None, tuple(eligible))
    return ReplayRoute(eligible[0], tuple(eligible))


def load_trace(trace_dir: Path, entry_url: str | None = None) -> TraceIndex:
    trace_dir = trace_dir.resolve()
    if not trace_dir.is_dir():
        raise TraceError(f"trace directory does not exist: {trace_dir}")
    meta_paths = sorted(
        path
        for path in trace_dir.iterdir()
        if path.is_file() and TRACE_FILE.fullmatch(path.name)
    )
    if not meta_paths:
        raise TraceError(f"trace contains no NNNN.meta records: {trace_dir}")
    records = [_load_record(path) for path in meta_paths]

    grouped: dict[tuple[str, str], list[CapturedResponse]] = {}
    methods: dict[str, set[str]] = {}
    for record in records:
        grouped.setdefault((record.method, record.target), []).append(record)
        methods.setdefault(record.target, set()).add(record.method)
    routes = {key: _route(value) for key, value in grouped.items()}

    if entry_url is None:
        entry = records[0]
    else:
        matching = [record for record in records if record.original_url == entry_url]
        if not matching:
            raise TraceError(f"entry URL is not present in trace: {entry_url}")
        entry = matching[0]
    entry_route = routes[(entry.method, entry.target)]
    if entry.method != "GET":
        raise TraceError(f"entry response must be GET, found {entry.method}")
    if entry_route.ambiguous:
        raise TraceError(
            f"entry route {entry.target!r} has conflicting captured responses"
        )
    return TraceIndex(
        trace_dir=trace_dir,
        routes=routes,
        methods_by_target={
            target: tuple(sorted(target_methods))
            for target, target_methods in methods.items()
        },
        entry=entry_route.selected or entry,
        record_count=len(records),
    )


def _json_bytes(value: object) -> bytes:
    return (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode(
        "utf-8"
    )


class TraceReplayHTTPServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(
        self,
        address: tuple[str, int],
        index: TraceIndex,
        *,
        verbose: bool = False,
    ) -> None:
        self.trace_index = index
        self.verbose = verbose
        super().__init__(address, TraceReplayHandler)


class TraceReplayHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "TilefinchTraceReplay/1"

    @property
    def replay_server(self) -> TraceReplayHTTPServer:
        return self.server  # type: ignore[return-value]

    def log_message(self, template: str, *arguments: object) -> None:
        if self.replay_server.verbose:
            super().log_message(template, *arguments)

    def _send_bytes(
        self,
        status: int,
        body: bytes,
        headers: Iterable[tuple[str, str]],
        *,
        include_body: bool,
    ) -> None:
        self.send_response_only(status)
        for name, value in headers:
            self.send_header(name, value)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        if include_body:
            self.wfile.write(body)
        self.close_connection = True

    def _error(
        self,
        status: int,
        code: str,
        details: dict[str, object],
        *,
        include_body: bool,
        extra_headers: Iterable[tuple[str, str]] = (),
    ) -> None:
        value = {"error": code, **details}
        headers = [
            ("Content-Type", "application/json; charset=utf-8"),
            ("X-Trace-Replay-Error", code),
            *extra_headers,
        ]
        self._send_bytes(status, _json_bytes(value), headers, include_body=include_body)

    def _serve(self, *, include_body: bool = True) -> None:
        method = self.command.upper()
        target = self.path.split("#", 1)[0] or "/"
        route = self.replay_server.trace_index.routes.get((method, target))
        if route is None:
            methods = self.replay_server.trace_index.methods_by_target.get(target, ())
            if methods:
                self._error(
                    405,
                    "trace-replay-method-not-captured",
                    {"method": method, "target": target, "available_methods": methods},
                    include_body=include_body,
                    extra_headers=(("Allow", ", ".join(methods)),),
                )
            else:
                self._error(
                    404,
                    "trace-replay-unmatched",
                    {"method": method, "target": target},
                    include_body=include_body,
                )
            return
        if route.ambiguous:
            self._error(
                409,
                "trace-replay-ambiguous",
                {
                    "method": method,
                    "target": target,
                    "records": [
                        {
                            "id": record.record_id,
                            "status": record.status,
                            "url": record.original_url,
                        }
                        for record in route.candidates
                    ],
                },
                include_body=include_body,
            )
            return
        record = route.selected
        if record is None:  # Kept explicit for static type checkers.
            raise AssertionError("non-ambiguous route without selected response")
        if record.status == 0:
            self._error(
                502,
                "trace-replay-captured-transport-failure",
                {
                    "method": method,
                    "target": target,
                    "record": record.record_id,
                    "captured_error": record.error,
                },
                include_body=include_body,
            )
            return

        headers: list[tuple[str, str]] = []
        has_content_type = False
        for name, value in record.headers:
            if name.lower() == "content-type":
                if has_content_type:
                    continue
                has_content_type = True
            headers.append((name, value))
        if not has_content_type and record.content_type:
            headers.append(("Content-Type", record.content_type))
        headers.append(("X-Trace-Replay-Record", record.record_id))
        self._send_bytes(
            record.status, record.body, headers, include_body=include_body
        )

    def do_GET(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        self._serve()

    def do_HEAD(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        self._serve(include_body=False)

    def do_POST(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        self._serve()

    def do_PUT(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        self._serve()

    def do_PATCH(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        self._serve()

    def do_DELETE(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        self._serve()

    def do_OPTIONS(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        self._serve()


def local_url(server: TraceReplayHTTPServer, target: str) -> str:
    port = int(server.server_address[1])
    return f"http://{LOOPBACK_ADDRESS}:{port}{target}"


def parse_args(arguments: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Serve a decoded Tilefinch NNNN.meta/.body trace on loopback."
    )
    parser.add_argument("trace_dir", type=Path, help="captured HTTP trace directory")
    parser.add_argument(
        "--port",
        type=int,
        default=0,
        help="loopback TCP port; 0 asks the OS for a free port (default: 0)",
    )
    parser.add_argument(
        "--entry-url",
        help="captured original URL to expose as the ready URL (default: 0000)",
    )
    parser.add_argument("--verbose", action="store_true", help="log HTTP requests")
    options = parser.parse_args(arguments)
    if not 0 <= options.port <= 65535:
        parser.error("--port must be in 0..65535")
    return options


def main(arguments: Sequence[str] | None = None) -> int:
    options = parse_args(arguments)
    try:
        index = load_trace(options.trace_dir, options.entry_url)
        server = TraceReplayHTTPServer(
            (LOOPBACK_ADDRESS, options.port), index, verbose=options.verbose
        )
    except (OSError, TraceError) as exc:
        print(f"trace-replay-error={exc}", file=sys.stderr)
        return 2

    ready_url = local_url(server, index.entry.target)
    print(f"trace-replay-ready-url={ready_url}", flush=True)
    print(
        "trace-replay-info="
        + json.dumps(
            {
                "entry_original_url": index.entry.original_url,
                "records": index.record_count,
                "routes": len(index.routes),
                "trace_dir": str(index.trace_dir),
            },
            sort_keys=True,
            separators=(",", ":"),
        ),
        file=sys.stderr,
        flush=True,
    )
    try:
        server.serve_forever(poll_interval=0.1)
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
