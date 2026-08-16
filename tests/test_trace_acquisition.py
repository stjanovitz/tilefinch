#!/usr/bin/env python3
"""Hermetic tests for the digest-pinned trace acquisition boundary."""

from __future__ import annotations

import hashlib
import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TOOL_PATH = ROOT / "benchmarks" / "acquire-trace-plan.py"
SPEC = importlib.util.spec_from_file_location("trace_acquisition", TOOL_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError("cannot load trace acquisition module")
ACQUIRE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ACQUIRE)


def _optional_test_binary(index: int, expected_name: str) -> Path | None:
    if len(sys.argv) <= index:
        return None
    candidate = Path(sys.argv[index])
    if (
        not candidate.name.startswith(expected_name)
        or not candidate.is_file()
        or not os.access(candidate, os.X_OK)
    ):
        return None
    return candidate.resolve()


RECORDER_BINARY = _optional_test_binary(1, "psp-browser-trace-acquire")
NATIVE_INVENTORY_BINARY = _optional_test_binary(
    2, "psp-browser-trace-inventory"
)


FAKE_RECORDER = r'''#!/usr/bin/env python3
import argparse
import os
import sys
from pathlib import Path

FORBIDDEN = (
    "HTTP_PROXY", "http_proxy", "HTTPS_PROXY", "https_proxy",
    "ALL_PROXY", "all_proxy", "NO_PROXY", "no_proxy", "NETRC",
    "CURL_HOME", "TILEFINCH_TRACE_RAW_COOKIES",
    "TILEFINCH_DIAGNOSTIC_MOBILE_SAFARI",
)
if any(name in os.environ for name in FORBIDDEN):
    raise SystemExit(91)

parser = argparse.ArgumentParser()
parser.add_argument("--method", required=True)
parser.add_argument("--url", required=True)
parser.add_argument("--output", type=Path, required=True)
parser.add_argument("--max-bytes", required=True)
parser.add_argument("--timeout-ms", required=True)
args = parser.parse_args()

marker = os.environ.get("FAKE_RECORDER_MARKER")
if marker:
    with open(marker, "a", encoding="utf-8") as stream:
        stream.write(args.method + " " + args.url + "\n")
mode = os.environ.get("FAKE_RECORDER_MODE", "ok")
if mode == "fail-head" and args.method == "HEAD":
    raise SystemExit(17)

args.output.mkdir(mode=0o700)
body = b"decoded response bytes"
if args.method == "HEAD":
    body = b"not allowed" if mode == "head-body" else b""

value = 0xCBF29CE484222325
for byte in body:
    value ^= byte
    value = (value * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF

headers = ["date: Wed, 21 Oct 2015 07:28:00 GMT"]
cookies = []
if args.method == "GET":
    headers.extend([
        "content-encoding: gzip",
        "location: https://redirect.example.test/not-followed",
    ])
    cookies = [
        "set-cookie-0=sid=xxxx; Path=/; Secure; HttpOnly; SameSite=Lax; "
        "Expires=Wed, 21 Oct 2099 07:28:00 GMT",
        "set-cookie-url-0=" + args.url,
    ]

meta = {
    "psp-http-trace": "11",
    "cookie-values": "redacted",
    "method": args.method,
    "url": args.url,
    "logical-request-url": args.url,
    "success": "1",
    "async-delay-pumps": "0",
    "external-cancel": "0",
    "transport-timeout": "0",
    "redirect-origin-tainted": "0",
    "error": "",
    "request-body-length": "0",
    "request-body-hash": "cbf29ce484222325",
    "request-content-type": "",
    "request-cookie-bytes": "0",
    "request-has-cf-clearance": "0",
    "request-extra-header-bytes": "0",
    "request-extra-header-shape": "",
    "request-allow-http-errors": "1",
    "request-enforce-cors": "0",
    "request-redirect-same-origin-only": "0",
    "request-cors-cached-response-validated": "0",
    "request-if-none-match": "",
    "request-if-modified-since": "",
    "request-referer": "",
    "request-origin": "",
    "request-accept": "",
    "request-sec-fetch-dest": "",
    "request-sec-fetch-mode": "",
    "request-sec-fetch-site": "",
    "request-send-client-hints": "0",
    "request-client-hint-tokens": "",
    "request-client-hint-origin": "",
    "request-send-low-client-hints": "0",
    "request-sec-fetch-user": "0",
    "request-upgrade-insecure": "0",
    "request-user-agent": "Tilefinch acquisition test",
    "request-diagnostic-mobile-safari": "0",
    "request-credentials": "0",
    "request-credential-origin": "",
    "request-initiator-url": "",
    "request-referrer-source": "",
    "request-referrer-policy": "",
    "status": "302" if args.method == "GET" else "204",
    "length": str(len(body)),
    "response-body-hash": f"{value:016x}",
    "effective-url": args.url,
    "content-type": "text/plain",
    "etag": "",
    "last-modified": "",
    "cf-mitigated": "0",
    "accept-ch": "",
    "critical-ch": "",
    "server": "fixture",
    "cf-ray": "",
    "response-referrer-policy-metadata-valid": "1",
    "response-referrer-policy-present": "0",
    "response-referrer-policy": "",
    "response-security-headers-truncated": "0",
    "response-security-metadata": "1,0,0,0,0,0,0,0,0,0,0,0,0,0,0",
    "response-security-allow-origin": "",
    "response-header-count": str(len(headers)),
    "set-cookie-count": "1" if cookies else "0",
}
if mode == "old-version":
    meta["psp-http-trace"] = "9"
lines = [f"{key}={value}" for key, value in meta.items()]
lines.extend(f"response-header-{index}={header}" for index, header in enumerate(headers))
lines.extend(cookies)
(args.output / "0000.meta").write_text("\n".join(lines) + "\n", encoding="utf-8")
(args.output / "0000.body").write_bytes(body)
if mode == "partial":
    trace_info = "psp-http-trace-clock=1\norigin-ms=9000\nrecord-count=2\n"
else:
    trace_info = (
        "psp-http-trace-clock=1\norigin-ms=9000\n"
        "capture-complete=yes\nrecord-count=1\n"
    )
(args.output / "trace.meta").write_text(trace_info, encoding="utf-8")
'''

FAKE_NATIVE_INVENTORY = r'''#!/usr/bin/env python3
import argparse
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("--response-keyed", type=Path, required=True)
parser.add_argument("--expect-records", type=int, required=True)
args = parser.parse_args()
values = {}
for line in (args.response_keyed / "trace.meta").read_text(encoding="utf-8").splitlines():
    if "=" in line:
        key, value = line.split("=", 1)
        values[key] = value
if values.get("capture-complete") != "yes" or int(values.get("record-count", "-1")) != args.expect_records:
    raise SystemExit(1)
print(f"trace-inventory-ok mode=response-keyed records={args.expect_records}")
'''

MUTATING_NATIVE_INVENTORY = r'''#!/usr/bin/env python3
import argparse
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("--response-keyed", type=Path, required=True)
parser.add_argument("--expect-records", type=int, required=True)
args = parser.parse_args()
values = {}
for line in (args.response_keyed / "trace.meta").read_text(encoding="utf-8").splitlines():
    if "=" in line:
        key, value = line.split("=", 1)
        values[key] = value
if values.get("capture-complete") != "yes" or int(values.get("record-count", "-1")) != args.expect_records:
    raise SystemExit(1)
print(
    f"trace-inventory-ok mode=response-keyed records={args.expect_records}",
    flush=True,
)
if args.expect_records > 1:
    body = args.response_keyed / "0000.body"
    body.write_bytes(body.read_bytes() + b"mutated-after-native-inventory\n")
'''

MUTATING_CLOSURE_NODE = r'''#!/usr/bin/env python3
import os
import subprocess
import sys
from pathlib import Path

payload = sys.stdin.buffer.read()
completed = subprocess.run(
    [os.environ["TILEFINCH_TEST_REAL_NODE"], *sys.argv[1:]],
    check=False,
    input=payload,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
)
sys.stdout.buffer.write(completed.stdout)
sys.stdout.buffer.flush()
sys.stderr.buffer.write(completed.stderr)
sys.stderr.buffer.flush()
if completed.returncode != 0:
    raise SystemExit(completed.returncode)
if len(sys.argv) > 1 and Path(sys.argv[1]).name == "verify-trace-acquisition.js":
    try:
        merged = Path(sys.argv[sys.argv.index("--merged") + 1])
    except (ValueError, IndexError):
        raise SystemExit(92)
    body = merged / "0000.body"
    body.write_bytes(body.read_bytes() + b"mutated-after-route-closure\n")
'''


class TraceAcquisitionTests(unittest.TestCase):
    # The stub executables are constant read-only fixtures, so they are
    # written once per class rather than once per test method.  The first
    # execution of a newly written file is authorized by one system-wide
    # macOS scanner whose latency is unbounded under load (~0.2 s idle,
    # ~4 s when other first executions are queued), so writing them per
    # method made this test's wall clock a multiple of that cost instead of
    # a function of its own work.  Isolation is unchanged: every trace,
    # output, and marker still lives under a private per-test directory.
    @classmethod
    def setUpClass(cls) -> None:
        cls.tools = tempfile.TemporaryDirectory(
            prefix="tilefinch-trace-acquisition-tools-"
        )
        tools = Path(cls.tools.name)
        cls.recorder = cls._stub(tools / "fake-recorder.py", FAKE_RECORDER)
        cls.mutating_inventory = cls._stub(
            tools / "mutating-native-inventory.py", MUTATING_NATIVE_INVENTORY
        )
        cls.mutating_node_directory = tools / "mutating-node-bin"
        cls.mutating_node_directory.mkdir()
        cls._stub(cls.mutating_node_directory / "node", MUTATING_CLOSURE_NODE)
        if NATIVE_INVENTORY_BINARY is None:
            cls.native_inventory = cls._stub(
                tools / "fake-native-inventory.py", FAKE_NATIVE_INVENTORY
            )
        else:
            cls.native_inventory = NATIVE_INVENTORY_BINARY

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tools.cleanup()

    @staticmethod
    def _stub(path: Path, source: str) -> Path:
        path.write_text(source, encoding="utf-8")
        path.chmod(0o700)
        return path

    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(
            prefix="tilefinch-trace-acquisition-"
        )
        self.root = Path(self.temporary.name)
        self.source = self.root / "source"
        recorder_environment = os.environ.copy()
        for name in (
            "HTTP_PROXY", "http_proxy", "HTTPS_PROXY", "https_proxy",
            "ALL_PROXY", "all_proxy", "NO_PROXY", "no_proxy", "NETRC",
            "CURL_HOME", "TILEFINCH_TRACE_RAW_COOKIES",
            "TILEFINCH_DIAGNOSTIC_MOBILE_SAFARI",
        ):
            recorder_environment.pop(name, None)
        completed = subprocess.run(
            [
                str(self.recorder),
                "--method", "GET",
                "--url", "https://source.example.test/",
                "--output", str(self.source),
                "--max-bytes", "4096",
                "--timeout-ms", "1000",
            ],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=recorder_environment,
        )
        if completed.returncode != 0:
            raise RuntimeError(f"cannot construct source fixture: {completed.stderr!r}")
        (self.source / "trace.meta").write_text(
            "psp-http-trace-clock=1\n"
            "origin-ms=123456789\n"
            "capture-complete=yes\n"
            "record-count=1\n",
            encoding="utf-8",
        )
        self.marker = self.root / "recorder.calls"
        self.requests = [
            self._request("GET", "https://assets.example.test/a.css", "style"),
            self._request("HEAD", "https://media.example.test/b.js", "script"),
        ]
        self.diagnostic, self.contract = self._write_authority(self.requests)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    @staticmethod
    def _request(method: str, url: str, resource_type: str) -> dict[str, object]:
        return {
            "method": method,
            "url": url,
            "url_sha256": hashlib.sha256(url.encode("utf-8")).hexdigest(),
            "resource_types": [resource_type],
            "occurrences": 1,
        }

    def _write_authority(
        self, requests: list[dict[str, object]]
    ) -> tuple[Path, Path]:
        source_sha, _source_bytes = ACQUIRE.trace_digest(
            self.source, 1024 * 1024
        )
        entries: list[dict[str, object]] = []
        for request in requests:
            resource_types = request["resource_types"]
            occurrences = request["occurrences"]
            assert isinstance(resource_types, list)
            assert isinstance(occurrences, int)
            for occurrence in range(occurrences):
                resource_type = resource_types[occurrence % len(resource_types)]
                entries.append(
                    {
                        "classification": "unmatched",
                        "method": request["method"],
                        "resource_type": resource_type,
                        "url": request["url"],
                        "url_sha256": request["url_sha256"],
                        "url_truncated": False,
                    }
                )
        diagnostic_origins = [
            ACQUIRE._url_authority(entry["url"], "test diagnostic URL")[1]
            for entry in entries
        ]
        unexpected_requests = {
            "total": len(entries),
            "retained": len(entries),
            "truncated": 0,
            "overflow": False,
            "multiset_sha256": ACQUIRE._diagnostic_multiset_digest(entries),
            "by_method": ACQUIRE._bounded_diagnostic_counter(
                entry["method"] for entry in entries
            ),
            "by_resource_type": ACQUIRE._bounded_diagnostic_counter(
                entry["resource_type"] for entry in entries
            ),
            "by_origin": ACQUIRE._bounded_diagnostic_counter(diagnostic_origins),
            "entries": entries,
        }
        diagnostic = self.root / "diagnostic.json"
        diagnostic.write_text(
            json.dumps(
                {
                    "trace_sha256": source_sha,
                    "capture_transport": "cdp-response-keyed",
                    "replay_environment": {"origin_ms": 123456789},
                    "replay_ledger": {
                        "mode": "cdp-response-keyed",
                        "records": 1,
                        "claimed": 1,
                        "requests": len(entries) + 1,
                        "matched": 1,
                        "served": 1,
                        "rejected": 0,
                        "unmatched": len(entries),
                        "conflicts": 0,
                        "invalid": 0,
                        "claimed_routes": [0],
                        "route_selection_version": ACQUIRE.REFERENCE_ROUTE_SELECTION,
                        "occurrence_claims": 0,
                        "reusable_claims": 1,
                        "occurrence_exhausted": 0,
                        "unexpected_requests": unexpected_requests,
                    },
                    "acquisition_plan": {
                        "mode": ACQUIRE.PLAN_MODE,
                        "complete": True,
                        "request_count": len(requests),
                        "unplannable": 0,
                        "requests": requests,
                    }
                },
                sort_keys=True,
                separators=(",", ":"),
            ),
            encoding="utf-8",
        )
        origins = sorted(
            {
                ACQUIRE._url_authority(item["url"], "test URL")[1]
                for item in requests
            }
        )
        contract_data = {
            "schema": 1,
            "name": "hermetic-test",
            "diagnostic_sha256": hashlib.sha256(
                diagnostic.read_bytes()
            ).hexdigest(),
            "allowlist_sha256": ACQUIRE._allowlist_digest(requests),
            "source_trace_sha256": source_sha,
            "source_record_count": 1,
            "source_origin_ms": 123456789,
            "request_count": len(requests),
            "max_response_bytes": 4096,
            "timeout_ms": 1000,
            "max_trace_bytes": 1024 * 1024,
            "allowed_methods": ["GET", "HEAD"],
            "allowed_schemes": ["https"],
            "allowed_origins": origins,
        }
        contract = self.root / "contract.json"
        contract.write_text(
            json.dumps(contract_data, sort_keys=True, separators=(",", ":")),
            encoding="utf-8",
        )
        return diagnostic, contract

    def _environment(self, mode: str = "ok") -> dict[str, str]:
        environment = os.environ.copy()
        for name in (
            "HTTP_PROXY",
            "http_proxy",
            "HTTPS_PROXY",
            "https_proxy",
            "ALL_PROXY",
            "all_proxy",
            "NO_PROXY",
            "no_proxy",
            "NETRC",
            "CURL_HOME",
            "TILEFINCH_TRACE_RAW_COOKIES",
            "TILEFINCH_DIAGNOSTIC_MOBILE_SAFARI",
        ):
            environment[name] = "must-not-reach-recorder"
        environment["FAKE_RECORDER_MODE"] = mode
        environment["FAKE_RECORDER_MARKER"] = str(self.marker)
        return environment

    @staticmethod
    def _refresh_unexpected_evidence(diagnostic: dict[str, object]) -> None:
        ledger = diagnostic["replay_ledger"]
        assert isinstance(ledger, dict)
        summary = ledger["unexpected_requests"]
        assert isinstance(summary, dict)
        entries = summary["entries"]
        assert isinstance(entries, list)
        origins = [
            ACQUIRE._url_authority(entry["url"], "test diagnostic URL")[1]
            for entry in entries
        ]
        summary.update(
            {
                "total": len(entries),
                "retained": len(entries),
                "truncated": 0,
                "overflow": False,
                "multiset_sha256": ACQUIRE._diagnostic_multiset_digest(entries),
                "by_method": ACQUIRE._bounded_diagnostic_counter(
                    entry["method"] for entry in entries
                ),
                "by_resource_type": ACQUIRE._bounded_diagnostic_counter(
                    entry["resource_type"] for entry in entries
                ),
                "by_origin": ACQUIRE._bounded_diagnostic_counter(origins),
            }
        )
        ledger["unmatched"] = len(entries)
        ledger["requests"] = (
            ledger["matched"] + ledger["conflicts"] + ledger["invalid"] + len(entries)
        )

    def _mutated_validation(
        self,
        mutate: object,
        *,
        reauthorize_plan: bool = False,
    ) -> list[dict[str, object]]:
        diagnostic_data = json.loads(self.diagnostic.read_text(encoding="utf-8"))
        assert callable(mutate)
        mutate(diagnostic_data)
        mutated = self.root / "mutated-diagnostic.json"
        mutated.write_text(
            json.dumps(diagnostic_data, sort_keys=True, separators=(",", ":")),
            encoding="utf-8",
        )
        contract = ACQUIRE._validate_contract(self.contract).copy()
        contract["diagnostic_sha256"] = hashlib.sha256(mutated.read_bytes()).hexdigest()
        if reauthorize_plan:
            plan = diagnostic_data["acquisition_plan"]
            requests = plan["requests"]
            contract["request_count"] = len(requests)
            contract["allowlist_sha256"] = ACQUIRE._allowlist_digest(requests)
            contract["allowed_origins"] = sorted(
                {
                    ACQUIRE._url_authority(request["url"], "test plan URL")[1]
                    for request in requests
                }
            )
        return ACQUIRE._validate_plan(mutated, contract)

    def _run(
        self,
        output: Path,
        mode: str = "ok",
        *,
        native_inventory: Path | None = None,
        environment_updates: dict[str, str] | None = None,
    ) -> subprocess.CompletedProcess[str]:
        environment = self._environment(mode)
        if environment_updates is not None:
            environment.update(environment_updates)
        return subprocess.run(
            [
                sys.executable,
                str(TOOL_PATH),
                "--contract",
                str(self.contract),
                "--diagnostic",
                str(self.diagnostic),
                "--source-trace",
                str(self.source),
                "--output-trace",
                str(output),
                "--recorder",
                str(self.recorder),
                "--native-inventory",
                str(native_inventory or self.native_inventory),
            ],
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=environment,
        )

    def test_atomic_merge_retains_origin_and_redacted_response_cookie(self) -> None:
        source_sha, _source_bytes = ACQUIRE.trace_digest(
            self.source, 1024 * 1024
        )
        output = self.root / "merged"
        completed = self._run(output)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        summary = json.loads(completed.stdout)
        self.assertEqual(summary["record_count"], 3)
        self.assertEqual(summary["origin_ms"], 123456789)
        published_sha, published_bytes = ACQUIRE.trace_digest(
            output, 1024 * 1024
        )
        self.assertEqual(summary["trace_sha256"], published_sha)
        self.assertEqual(summary["trace_bytes"], published_bytes)
        _published_info, _published_raw, published_keys = (
            ACQUIRE._trace_inventory(output)
        )
        self.assertEqual(
            published_keys,
            {
                ("GET", "https://source.example.test/"),
                *((item["method"], item["url"]) for item in self.requests),
            },
        )
        info, _raw = ACQUIRE._metadata(output / "trace.meta")
        self.assertEqual(info["capture-complete"], "yes")
        self.assertEqual(info["record-count"], "3")
        self.assertEqual(info["origin-ms"], "123456789")
        acquired, _raw = ACQUIRE._metadata(output / "0001.meta")
        self.assertEqual(acquired["status"], "302")
        self.assertEqual(acquired["effective-url"], self.requests[0]["url"])
        self.assertEqual(acquired["request-cookie-bytes"], "0")
        self.assertEqual(acquired["request-has-cf-clearance"], "0")
        self.assertEqual(acquired["request-credentials"], "0")
        self.assertEqual(
            acquired["set-cookie-0"],
            "sid=xxxx; Path=/; Secure; HttpOnly; SameSite=Lax; "
            "Expires=Wed, 21 Oct 2099 07:28:00 GMT",
        )
        self.assertEqual(acquired["set-cookie-url-0"], self.requests[0]["url"])
        self.assertIn("content-encoding: gzip", acquired.values())
        self.assertIn(
            "location: https://redirect.example.test/not-followed",
            acquired.values(),
        )
        self.assertEqual((output / "0001.body").read_bytes(), b"decoded response bytes")
        head, _raw = ACQUIRE._metadata(output / "0002.meta")
        self.assertEqual(head["method"], "HEAD")
        self.assertEqual(head["length"], "0")
        self.assertEqual((output / "0002.body").read_bytes(), b"")
        source_sha_after, _source_bytes = ACQUIRE.trace_digest(
            self.source, 1024 * 1024
        )
        self.assertEqual(source_sha_after, source_sha)
        self.assertEqual(len(self.marker.read_text(encoding="utf-8").splitlines()), 2)
        self.assertFalse(any(self.root.glob(".merged.*")))

    def test_mutating_route_closure_never_publishes_attested_bytes(self) -> None:
        source_sha, _source_bytes = ACQUIRE.trace_digest(
            self.source, 1024 * 1024
        )
        real_node = ACQUIRE._node_executable()
        wrapper_directory = self.mutating_node_directory
        output = self.root / "merged"
        completed = self._run(
            output,
            environment_updates={
                "PATH": str(wrapper_directory)
                + os.pathsep
                + os.environ.get("PATH", ""),
                "TILEFINCH_TEST_REAL_NODE": str(real_node),
            },
        )
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn(
            "final staged trace changed after external validation",
            completed.stderr,
        )
        self.assertFalse(output.exists())
        self.assertFalse(any(self.root.glob(".merged.*")))
        source_sha_after, _source_bytes = ACQUIRE.trace_digest(
            self.source, 1024 * 1024
        )
        self.assertEqual(source_sha_after, source_sha)

    def test_mutating_native_inventory_never_publishes_attested_bytes(self) -> None:
        source_sha, _source_bytes = ACQUIRE.trace_digest(
            self.source, 1024 * 1024
        )
        output = self.root / "merged"
        completed = self._run(
            output, native_inventory=self.mutating_inventory
        )
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn(
            "final staged trace changed after external validation",
            completed.stderr,
        )
        self.assertFalse(output.exists())
        self.assertFalse(any(self.root.glob(".merged.*")))
        source_sha_after, _source_bytes = ACQUIRE.trace_digest(
            self.source, 1024 * 1024
        )
        self.assertEqual(source_sha_after, source_sha)

    def test_recorder_failure_never_publishes_partial_output(self) -> None:
        source_sha, _source_bytes = ACQUIRE.trace_digest(
            self.source, 1024 * 1024
        )
        output = self.root / "merged"
        completed = self._run(output, "fail-head")
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("isolated recorder exited 17", completed.stderr)
        self.assertFalse(output.exists())
        self.assertFalse(any(self.root.glob(".merged.*")))
        source_sha_after, _source_bytes = ACQUIRE.trace_digest(
            self.source, 1024 * 1024
        )
        self.assertEqual(source_sha_after, source_sha)

    def test_existing_output_is_never_replaced_or_contacted(self) -> None:
        output = self.root / "merged"
        output.mkdir()
        sentinel = output / "owned-by-caller"
        sentinel.write_bytes(b"keep")
        completed = self._run(output)
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("output trace already exists", completed.stderr)
        self.assertEqual(sentinel.read_bytes(), b"keep")
        self.assertFalse(self.marker.exists())

    def test_recorder_must_finalize_exactly_one_record(self) -> None:
        output = self.root / "merged"
        completed = self._run(output, "partial")
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("finalized clock metadata", completed.stderr)
        self.assertFalse(output.exists())
        self.assertFalse(any(self.root.glob(".merged.*")))

    def test_head_body_is_rejected_before_publication(self) -> None:
        output = self.root / "merged"
        completed = self._run(output, "head-body")
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("HEAD acquisition retained a response body", completed.stderr)
        self.assertFalse(output.exists())

    def test_recorder_must_emit_current_trace_security_contract(self) -> None:
        output = self.root / "merged"
        completed = self._run(output, "old-version")
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn(
            "acquired record psp-http-trace differs",
            completed.stderr,
        )
        self.assertFalse(output.exists())

    def test_digest_tamper_fails_before_recorder(self) -> None:
        self.diagnostic.write_bytes(self.diagnostic.read_bytes() + b"\n")
        completed = subprocess.run(
            [
                sys.executable,
                str(TOOL_PATH),
                "--contract",
                str(self.contract),
                "--diagnostic",
                str(self.diagnostic),
                "--source-trace",
                str(self.source),
                "--validate-only",
            ],
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=self._environment(),
        )
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("diagnostic SHA-256 mismatch", completed.stderr)
        self.assertFalse(self.marker.exists())

    def test_v2_plan_is_bound_to_exact_complete_unmatched_evidence(self) -> None:
        contract = ACQUIRE._validate_contract(self.contract)
        self.assertEqual(
            ACQUIRE._validate_plan(self.diagnostic, contract),
            self.requests,
        )

        def stale_route(diagnostic: dict[str, object]) -> None:
            diagnostic["replay_ledger"]["route_selection_version"] = "ranked-occurrence-v1"

        def wrong_transport(diagnostic: dict[str, object]) -> None:
            diagnostic["capture_transport"] = "loopback"

        def wrong_ledger_mode(diagnostic: dict[str, object]) -> None:
            diagnostic["replay_ledger"]["mode"] = "response-keyed"

        def wrong_trace(diagnostic: dict[str, object]) -> None:
            diagnostic["trace_sha256"] = "0" * 64

        def wrong_record_count(diagnostic: dict[str, object]) -> None:
            diagnostic["replay_ledger"]["records"] = 2

        def wrong_origin(diagnostic: dict[str, object]) -> None:
            diagnostic["replay_environment"]["origin_ms"] = 123456790

        def incomplete_entries(diagnostic: dict[str, object]) -> None:
            summary = diagnostic["replay_ledger"]["unexpected_requests"]
            summary["total"] += 1
            summary["truncated"] = 1

        def inconsistent_counter(diagnostic: dict[str, object]) -> None:
            summary = diagnostic["replay_ledger"]["unexpected_requests"]
            summary["by_method"]["GET"] += 1

        def inconsistent_multiset(diagnostic: dict[str, object]) -> None:
            summary = diagnostic["replay_ledger"]["unexpected_requests"]
            summary["multiset_sha256"] = "0" * 64

        def non_unmatched_entry(diagnostic: dict[str, object]) -> None:
            summary = diagnostic["replay_ledger"]["unexpected_requests"]
            summary["entries"][0]["classification"] = "conflict"
            self._refresh_unexpected_evidence(diagnostic)

        def duplicate_occurrence_without_plan_count(
            diagnostic: dict[str, object]
        ) -> None:
            summary = diagnostic["replay_ledger"]["unexpected_requests"]
            summary["entries"].append(dict(summary["entries"][0]))
            self._refresh_unexpected_evidence(diagnostic)

        def impossible_claims(diagnostic: dict[str, object]) -> None:
            ledger = diagnostic["replay_ledger"]
            ledger["claimed"] = 0
            ledger["claimed_routes"] = []

        cases = (
            ("stale-route", stale_route, "route_selection_version"),
            ("wrong-transport", wrong_transport, "capture_transport"),
            ("wrong-ledger-mode", wrong_ledger_mode, "replay_ledger mode"),
            ("wrong-trace", wrong_trace, "trace_sha256"),
            ("wrong-record-count", wrong_record_count, "records does not bind"),
            ("wrong-origin", wrong_origin, "origin_ms"),
            ("truncated", incomplete_entries, "incomplete or inconsistent"),
            ("counter", inconsistent_counter, "does not exactly match"),
            ("multiset", inconsistent_multiset, "multiset SHA-256"),
            ("classification", non_unmatched_entry, "not an exact unmatched"),
            (
                "duplicate-occurrence",
                duplicate_occurrence_without_plan_count,
                "exact canonical projection",
            ),
            ("impossible-claims", impossible_claims, "route claims"),
        )
        for label, mutate, expected in cases:
            with self.subTest(label=label), self.assertRaisesRegex(
                ACQUIRE.AcquisitionError, expected
            ):
                self._mutated_validation(mutate)

        def omit_authorized_request(diagnostic: dict[str, object]) -> None:
            plan = diagnostic["acquisition_plan"]
            plan["requests"] = plan["requests"][:-1]
            plan["request_count"] = len(plan["requests"])

        with self.assertRaisesRegex(
            ACQUIRE.AcquisitionError, "exact canonical projection"
        ):
            self._mutated_validation(omit_authorized_request, reauthorize_plan=True)

    def test_url_authority_rejects_non_https_and_credentialed_urls(self) -> None:
        for url in (
            "http://assets.example.test/a.css",
            "https://user:secret@assets.example.test/a.css",
            "https://assets.example.test/a.css#fragment",
            "https://assets.example.test/a\\b",
        ):
            with self.subTest(url=url), self.assertRaises(ACQUIRE.AcquisitionError):
                ACQUIRE._url_authority(url, "test URL")

    def test_origin_authority_rejects_values_beyond_ecmascript_date(self) -> None:
        contract_data = json.loads(self.contract.read_text(encoding="utf-8"))
        contract_data["source_origin_ms"] = ACQUIRE.ECMASCRIPT_DATE_MAX_MS + 1
        invalid_contract = self.root / "invalid-origin-contract.json"
        invalid_contract.write_text(
            json.dumps(contract_data, sort_keys=True, separators=(",", ":")),
            encoding="utf-8",
        )
        with self.assertRaises(ACQUIRE.AcquisitionError):
            ACQUIRE._validate_contract(invalid_contract)

        (self.source / "trace.meta").write_text(
            "psp-http-trace-clock=1\n"
            f"origin-ms={ACQUIRE.ECMASCRIPT_DATE_MAX_MS + 1}\n"
            "capture-complete=yes\n"
            "record-count=1\n",
            encoding="utf-8",
        )
        with self.assertRaises(ACQUIRE.AcquisitionError):
            ACQUIRE._trace_inventory(self.source)


class RecorderPolicyTests(unittest.TestCase):
    @unittest.skipUnless(RECORDER_BINARY is not None, "recorder binary not supplied")
    def test_host_recorder_rejects_http_before_network(self) -> None:
        recorder = RECORDER_BINARY
        self.assertIsNotNone(recorder)
        with tempfile.TemporaryDirectory(prefix="tilefinch-recorder-policy-") as root:
            output = Path(root) / "must-not-exist"
            completed = subprocess.run(
                [
                    str(recorder),
                    "--method",
                    "GET",
                    "--url",
                    "http://example.invalid/",
                    "--output",
                    str(output),
                    "--max-bytes",
                    "1024",
                    "--timeout-ms",
                    "10000",
                ],
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            self.assertEqual(completed.returncode, 2)
            self.assertFalse(output.exists())

    @unittest.skipUnless(RECORDER_BINARY is not None, "recorder binary not supplied")
    def test_host_recorder_refuses_raw_cookie_capture_before_network(self) -> None:
        recorder = RECORDER_BINARY
        self.assertIsNotNone(recorder)
        with tempfile.TemporaryDirectory(prefix="tilefinch-recorder-policy-") as root:
            output = Path(root) / "must-not-exist"
            environment = os.environ.copy()
            environment["TILEFINCH_TRACE_RAW_COOKIES"] = "1"
            completed = subprocess.run(
                [
                    str(recorder),
                    "--method",
                    "GET",
                    "--url",
                    "https://example.invalid/",
                    "--output",
                    str(output),
                    "--max-bytes",
                    "1024",
                    "--timeout-ms",
                    "10000",
                ],
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                env=environment,
            )
            self.assertEqual(completed.returncode, 2)
            self.assertIn(b"refuses TILEFINCH_TRACE_RAW_COOKIES", completed.stderr)
            self.assertFalse(output.exists())


@unittest.skipUnless(
    NATIVE_INVENTORY_BINARY is not None,
    "native inventory binary not supplied",
)
class RetainedNprAuthorityTests(unittest.TestCase):
    def test_stale_round1_npr_authority_is_rejected_without_network(self) -> None:
        diagnostic = (
            ROOT.parent
            / "tilefinch-corpora"
            / "visual-references-qualified-20260717-v1"
            / "npr"
            / "reference-diagnostic.json"
        )
        source = (
            ROOT.parent
            / "tilefinch-corpora"
            / "visual-qualified-20260717-v1"
            / "npr"
        )
        if not diagnostic.is_file() or not (source / "trace.meta").is_file():
            self.skipTest("retained NPR authority is not present in this checkout")
        completed = subprocess.run(
            [
                sys.executable,
                str(TOOL_PATH),
                "--contract",
                str(
                    ROOT
                    / "benchmarks"
                    / "acquisition-plans"
                    / "npr-visual-round1.json"
                ),
                "--diagnostic",
                str(diagnostic),
                "--source-trace",
                str(source),
                "--native-inventory",
                str(NATIVE_INVENTORY_BINARY),
                "--validate-only",
            ],
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("route_selection_version", completed.stderr)
        self.assertEqual(completed.stdout, "")

if __name__ == "__main__":
    # Keep an optional recorder binary argument out of unittest's option parser.
    sys.argv[1:] = []
    unittest.main()
