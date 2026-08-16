#!/usr/bin/env python3

from __future__ import annotations

import http.client
import socket
import subprocess
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from urllib.parse import urlsplit


ROOT = Path(__file__).resolve().parents[1]
BENCHMARKS = ROOT / "benchmarks"
sys.path.insert(0, str(BENCHMARKS))

from trace_replay_server import (  # noqa: E402
    LOOPBACK_ADDRESS,
    TraceError,
    TraceReplayHTTPServer,
    load_trace,
)


def write_record(
    root: Path,
    number: int,
    *,
    url: str,
    body: bytes,
    status: int = 200,
    success: bool = True,
    method: str = "GET",
    content_type: str = "application/octet-stream",
    headers: tuple[str, ...] = (),
) -> None:
    stem = f"{number:04d}"
    values = [
        "psp-http-trace=9",
        f"method={method}",
        f"url={url}",
        f"success={int(success)}",
        "error=captured failure" if status == 0 else "error=",
        f"status={status}",
        f"length={len(body)}",
        f"content-type={content_type}",
        f"response-header-count={len(headers)}",
    ]
    values.extend(
        f"response-header-{index}={header}"
        for index, header in enumerate(headers)
    )
    (root / f"{stem}.meta").write_text("\n".join(values) + "\n", encoding="utf-8")
    if body:
        (root / f"{stem}.body").write_bytes(body)


class RunningServer:
    def __init__(self, trace: Path) -> None:
        self.server = TraceReplayHTTPServer(
            (LOOPBACK_ADDRESS, 0), load_trace(trace)
        )
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)

    def __enter__(self) -> "RunningServer":
        self.thread.start()
        return self

    def __exit__(self, *unused: object) -> None:
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)

    def request(self, method: str, target: str) -> tuple[int, dict[str, str], bytes]:
        connection = http.client.HTTPConnection(
            LOOPBACK_ADDRESS, self.server.server_address[1], timeout=2
        )
        connection.request(method, target)
        response = connection.getresponse()
        body = response.read()
        headers = {name.lower(): value for name, value in response.getheaders()}
        status = response.status
        connection.close()
        return status, headers, body


class TraceReplayServerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            probe.bind((LOOPBACK_ADDRESS, 0))
        except PermissionError as error:
            raise unittest.SkipTest(
                f"loopback socket binding is unavailable: {error}"
            ) from error
        finally:
            probe.close()

    def test_preserves_body_and_recalculates_decoded_transfer_headers(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            trace = Path(directory)
            body = b"\x00decoded\xffbody\r\n"
            write_record(
                trace,
                0,
                url="https://fixture.test/page?mode=small",
                body=body,
                status=206,
                content_type="application/octet-stream",
                headers=(
                    "Content-Type: application/octet-stream",
                    "Content-Encoding: gzip",
                    "Content-Length: 3",
                    "Cache-Control: max-age=60",
                    "X-Fixture: retained",
                ),
            )
            with RunningServer(trace) as running:
                status, headers, received = running.request(
                    "GET", "/page?mode=small"
                )
            self.assertEqual(status, 206)
            self.assertEqual(received, body)
            self.assertEqual(headers["content-length"], str(len(body)))
            self.assertNotIn("content-encoding", headers)
            self.assertEqual(headers["content-type"], "application/octet-stream")
            self.assertEqual(headers["cache-control"], "max-age=60")
            self.assertEqual(headers["x-fixture"], "retained")
            self.assertEqual(headers["x-trace-replay-record"], "0000")

    def test_query_and_method_are_part_of_the_exact_route(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            trace = Path(directory)
            write_record(
                trace,
                0,
                url="https://fixture.test/data?value=one",
                body=b"one",
            )
            with RunningServer(trace) as running:
                status, headers, body = running.request("GET", "/data?value=two")
                self.assertEqual(status, 404)
                self.assertEqual(headers["x-trace-replay-error"], "trace-replay-unmatched")
                self.assertIn(b'"target":"/data?value=two"', body)
                status, headers, body = running.request("POST", "/data?value=one")
                self.assertEqual(status, 405)
                self.assertEqual(headers["allow"], "GET")
                self.assertIn(b"trace-replay-method-not-captured", body)

    def test_successful_retry_wins_over_status_zero_and_identical_repeats(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            trace = Path(directory)
            url = "https://fixture.test/retried.js"
            write_record(trace, 0, url=url, body=b"", status=0, success=False)
            write_record(trace, 1, url=url, body=b"ready", status=200)
            write_record(trace, 2, url=url, body=b"ready", status=200)
            with RunningServer(trace) as running:
                status, headers, body = running.request("GET", "/retried.js")
            self.assertEqual((status, body), (200, b"ready"))
            self.assertEqual(headers["x-trace-replay-record"], "0001")

    def test_conflicting_same_path_responses_fail_clearly(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            trace = Path(directory)
            # Keep 0000 unambiguous so it remains a valid entry page.
            write_record(trace, 0, url="https://fixture.test/", body=b"entry")
            write_record(trace, 1, url="https://a.test/shared", body=b"alpha")
            write_record(trace, 2, url="https://b.test/shared", body=b"beta")
            with RunningServer(trace) as running:
                status, headers, body = running.request("GET", "/shared")
            self.assertEqual(status, 409)
            self.assertEqual(headers["x-trace-replay-error"], "trace-replay-ambiguous")
            self.assertIn(b"https://a.test/shared", body)
            self.assertIn(b"https://b.test/shared", body)

    def test_port_zero_cli_prints_loopback_ready_url(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            trace = Path(directory)
            write_record(
                trace,
                0,
                url="https://fixture.test/start?viewport=psp",
                body=b"entry",
                content_type="text/html; charset=utf-8",
            )
            process = subprocess.Popen(
                [
                    sys.executable,
                    str(BENCHMARKS / "trace_replay_server.py"),
                    str(trace),
                    "--port",
                    "0",
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            try:
                assert process.stdout is not None
                ready = process.stdout.readline().strip()
                self.assertTrue(ready.startswith("trace-replay-ready-url="), ready)
                parsed = urlsplit(ready.split("=", 1)[1])
                self.assertEqual(parsed.hostname, LOOPBACK_ADDRESS)
                self.assertGreater(parsed.port or 0, 0)
                self.assertEqual(parsed.path, "/start")
                self.assertEqual(parsed.query, "viewport=psp")
            finally:
                process.terminate()
                process.communicate(timeout=3)
                if process.stdout is not None:
                    process.stdout.close()
                if process.stderr is not None:
                    process.stderr.close()

    def test_missing_nonempty_body_rejects_trace(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            trace = Path(directory)
            (trace / "0000.meta").write_text(
                "method=GET\n"
                "url=https://fixture.test/\n"
                "success=1\n"
                "status=200\n"
                "length=4\n"
                "content-type=text/plain\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(TraceError, "missing 0000.body"):
                load_trace(trace)


if __name__ == "__main__":
    unittest.main()
