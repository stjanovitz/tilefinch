#!/usr/bin/env python3
"""Fast, browser-free tests for the fail-closed reference capture index."""

from __future__ import annotations

import concurrent.futures
import json
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CAPTURE = ROOT / "benchmarks" / "capture-reference.js"
NODE = shutil.which("node")

sys.path.insert(0, str(ROOT / "benchmarks"))
from visual_scenario import (  # noqa: E402
    REFERENCE_RESPONSE_SCHEDULER_FIELDS,
    trace_digest,
)


def write_trace_meta(directory: Path, count: int = 1, origin_ms: int = 1000) -> None:
    (directory / "trace.meta").write_text(
        "psp-http-trace-clock=1\n"
        f"origin-ms={origin_ms}\n"
        "capture-complete=yes\n"
        f"record-count={count}\n"
    )


def inspect(directory: Path) -> subprocess.CompletedProcess[str]:
    assert NODE is not None
    return subprocess.run(
        (NODE, str(CAPTURE), "--inspect-trace", str(directory)),
        check=False,
        capture_output=True,
        text=True,
    )


def reference_environment_state() -> dict[str, object]:
    origin_ms = 1000
    rng = "splitmix64-url-scope-v1"
    seed_source = "trace-origin-ms-v1"
    seed = hashlib.sha256(
        f"{rng}\0{seed_source}\0{origin_ms}".encode()
    ).hexdigest()
    return {
        "version": "deterministic-hermetic-v3",
        "origin_ms": origin_ms,
        "clock_version": "playwright-clock-paused-v2",
        "clock_contract": "dual-domain-ms-call-v2",
        "clock_scope": "top-level-realm-v1",
        "rng_version": rng,
        "seed_source": seed_source,
        "intl_surface": "bounded-en-us-utc-v1",
        "seed_u64": str(origin_ms),
        "seed_sha256": seed,
        "ticks": 0,
        "tick_ms": 1,
        "host_elapsed_ms": 0,
        "wall_elapsed_ms": 0,
        "monotonic_elapsed_ms": 0,
        "wall_observations": 0,
        "monotonic_observations": 0,
        "monotonic_samples": 0,
        "clock_sources": {
            "date_now": 0,
            "date_function": 0,
            "date_constructor": 0,
            "performance_now": 0,
            "performance_mark": 0,
            "performance_measure": 0,
            "animation_timeline": 0,
            "idle_deadline_time_remaining": 0,
            "animation_frame": 0,
            "event_timestamp": 0,
            "intersection_observer": 0,
            "idle_callback_start": 0,
        },
        "performance_entries": "normalized-empty-v1",
        "document_timeline": "dual-domain-ms-call-v2",
        "animation_frame": "dual-domain-ms-call-v2",
    }


@unittest.skipIf(NODE is None, "Node.js is unavailable")
class ReferenceCaptureIndexTests(unittest.TestCase):
    def test_inspection_matches_canonical_digest_and_route_count(self) -> None:
        trace = ROOT / "fixtures" / "http-stream"
        completed = inspect(trace)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        state = json.loads(completed.stdout)
        self.assertEqual(state["trace_sha256"], trace_digest(trace))
        self.assertEqual(state["records"], 1)
        self.assertEqual(state["routes"], 1)
        self.assertEqual(state["ambiguous_routes"], 0)
        self.assertEqual(state["urls"], ["https://stream.test/document"])

    def test_manifest_preserves_css_viewport_and_rejects_unsafe_rows(self) -> None:
        manifest = ROOT / "benchmarks" / "visual-scenarios.tsv"
        script = (
            "const capture=require(process.argv[1]);"
            "process.stdout.write(JSON.stringify("
            "capture.loadScenario(process.argv[2],process.argv[3])));"
        )
        lines = manifest.read_text().splitlines()
        header = lines[0].split("\t")
        reddit = next(line.split("\t") for line in lines[1:] if line.startswith("reddit-old\t"))
        reddit[header.index("trace_sha256")] = "0" * 64
        with tempfile.TemporaryDirectory() as temporary:
            checked = Path(temporary) / "manifest.tsv"
            checked.write_text("\t".join(header) + "\n" + "\t".join(reddit) + "\n")
            completed = subprocess.run(
                (NODE, "-e", script, str(CAPTURE), str(checked), "reddit-old"),
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)
            state = json.loads(completed.stdout)
            self.assertEqual((state["cssWidth"], state["cssHeight"]), (1024, 581))
            self.assertEqual((state["deviceWidth"], state["deviceHeight"]), (480, 272))

        source = next(line.split("\t") for line in lines[1:] if line.startswith("hacker-news\t"))
        for label, field, value, expected in (
            ("path", "replay_dir", "../escape", "safe relative path"),
            ("scale", "device_width", "479", "do not produce the device geometry"),
            ("empty-number", "min_scripts_loaded", "", "must be an integer"),
        ):
            with self.subTest(label=label), tempfile.TemporaryDirectory() as temporary:
                row = source.copy()
                row[header.index(field)] = value
                bad = Path(temporary) / "manifest.tsv"
                bad.write_text("\t".join(header) + "\n" + "\t".join(row) + "\n")
                completed = subprocess.run(
                    (NODE, "-e", script, str(CAPTURE), str(bad), "hacker-news"),
                    check=False,
                    capture_output=True,
                    text=True,
                )
                self.assertNotEqual(completed.returncode, 0)
                self.assertIn(expected, completed.stderr)

    def test_chromium_manifest_clock_and_native_cli_bounds(self) -> None:
        manifest = ROOT / "benchmarks" / "visual-scenarios.tsv"
        script = (
            "const capture=require(process.argv[1]);"
            "process.stdout.write(JSON.stringify("
            "capture.loadScenario(process.argv[2],process.argv[3])));"
        )
        lines = manifest.read_text().splitlines()
        header = lines[0].split("\t")
        source = next(
            line.split("\t") for line in lines[1:] if line.startswith("hacker-news\t")
        )

        def load(row: list[str], root: Path) -> subprocess.CompletedProcess[str]:
            checked = root / "manifest.tsv"
            checked.write_text("\t".join(header) + "\n" + "\t".join(row) + "\n")
            return subprocess.run(
                (NODE, "-e", script, str(CAPTURE), str(checked), "hacker-news"),
                check=False,
                capture_output=True,
                text=True,
            )

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            boundary = source.copy()
            boundary[header.index("ticks")] = "1000"
            boundary[header.index("tick_ms")] = "60000"
            completed = load(boundary, root)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            parsed = json.loads(completed.stdout)
            self.assertEqual((parsed["ticks"], parsed["tickMs"]), (1000, 60000))

        cases = (
            ("ticks", "1001", "0..1000"),
            ("tick_ms", "60001", "1..60000"),
            ("expected_http", "99", "100..599"),
            ("expected_http", "600", "100..599"),
            ("limit_mb", "3", "4..512"),
            ("limit_mb", "513", "4..512"),
            ("max_download_kb", "65537", "1..65536"),
            ("script_timeout_ms", "300001", "1..300000"),
            ("script_heap_mb", "257", "1..256"),
            ("script_total_mb", "129", "1..128"),
            ("script_file_kb", "8193", "1..8192"),
            ("script_count", "257", "1..256"),
        )
        for field, value, expected in cases:
            with self.subTest(field=field), tempfile.TemporaryDirectory() as temporary:
                row = source.copy()
                row[header.index(field)] = value
                completed = load(row, Path(temporary))
                self.assertNotEqual(completed.returncode, 0)
                self.assertIn(expected, completed.stderr)

    def test_chromium_clock_evidence_enforces_v2_domain_partitions(self) -> None:
        script = r"""
const capture = require(process.argv[1]);
const origin = 1000;
const host = 60000000;
const valid = {
  contract: "dual-domain-ms-call-v2",
  scope: "top-level-realm-v1",
  originMs: origin,
  hostElapsedMs: host,
  playwrightElapsedMs: host,
  wallElapsedMs: host + 3,
  monotonicElapsedMs: host + 2,
  wallObservations: 3,
  monotonicObservations: 2,
  monotonicSamples: 1,
  clockSources: {
    date_now: 1, date_function: 1, date_constructor: 1,
    performance_now: 1, performance_mark: 0, performance_measure: 0,
    animation_timeline: 0, idle_deadline_time_remaining: 1,
    animation_frame: 0, event_timestamp: 1,
    intersection_observer: 0, idle_callback_start: 0,
  },
};
const changed = (values) => ({
  ...valid, ...values,
  clockSources: values.clockSources || valid.clockSources,
});
process.stdout.write(JSON.stringify({
  boundary: capture.replayClockEvidenceReady(valid, origin, host),
  hostOver: capture.replayClockEvidenceReady(
    changed({ hostElapsedMs: host + 1 }),
    origin, host + 1),
  wallIdentity: capture.replayClockEvidenceReady(
    changed({ wallElapsedMs: host + 2 }), origin, host),
  monotonicIdentity: capture.replayClockEvidenceReady(
    changed({ monotonicElapsedMs: host + 1 }), origin, host),
  stablePartition: capture.replayClockEvidenceReady(changed({
    monotonicSamples: 0,
  }), origin, host),
  advancingPartition: capture.replayClockEvidenceReady(changed({
    monotonicObservations: 1, monotonicElapsedMs: host + 1,
  }), origin, host),
  unknownSource: capture.replayClockEvidenceReady(changed({
    clockSources: { ...valid.clockSources, future_clock: 0 },
  }), origin, host),
  wrongScope: capture.replayClockEvidenceReady(
    changed({ scope: "child-realm" }), origin, host),
  legacyV1: capture.replayClockEvidenceReady({
    contract: "logical-ms-call-v1", originMs: origin,
    hostElapsedMs: host, playwrightElapsedMs: host,
    logicalElapsedMs: host, observations: 0, observationSources: {},
  }, origin, host),
}));
"""
        completed = subprocess.run(
            (NODE, "-e", script, str(CAPTURE)),
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(
            json.loads(completed.stdout),
            {
                "boundary": True,
                "hostOver": False,
                "wallIdentity": False,
                "monotonicIdentity": False,
                "stablePartition": False,
                "advancingPartition": False,
                "unknownSource": False,
                "wrongScope": False,
                "legacyV1": False,
            },
        )

    def test_conflicting_response_bodies_fail_closed_in_the_index(self) -> None:
        trace = ROOT / "fixtures" / "http-response-key-conflict"
        completed = inspect(trace)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        state = json.loads(completed.stdout)
        self.assertEqual(state["records"], 2)
        self.assertEqual(state["routes"], 1)
        self.assertEqual(state["ambiguous_routes"], 1)

    def test_length_mismatch_is_rejected_before_browser_launch(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            trace = Path(temporary)
            write_trace_meta(trace)
            (trace / "0000.meta").write_text(
                "method=GET\nurl=https://capture.test/\nsuccess=1\n"
                "status=200\nlength=9\ncontent-type=text/plain\n"
                "response-header-count=0\n"
            )
            (trace / "0000.body").write_bytes(b"short")
            completed = inspect(trace)
            self.assertEqual(completed.returncode, 2)
            self.assertIn("declared length does not match body", completed.stderr)

    def test_transport_failure_then_success_selects_one_route(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            trace = Path(temporary)
            write_trace_meta(trace, count=2)
            (trace / "0000.meta").write_text(
                "method=GET\nurl=https://retry.test/\nsuccess=0\n"
                "status=0\nlength=0\ncontent-type=\nresponse-header-count=0\n"
                "set-cookie-count=0\n"
            )
            (trace / "0001.meta").write_text(
                "method=GET\nurl=https://retry.test/\nsuccess=1\n"
                "status=200\nlength=2\ncontent-type=text/plain\n"
                "response-header-count=1\nset-cookie-count=0\n"
                "response-header-0=content-type: text/plain\n"
            )
            (trace / "0001.body").write_bytes(b"ok")
            completed = inspect(trace)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            state = json.loads(completed.stdout)
            self.assertEqual(state["records"], 2)
            self.assertEqual(state["routes"], 1)
            self.assertEqual(state["ambiguous_routes"], 0)

    def test_trace_ids_and_response_semantics_fail_closed(self) -> None:
        cases = (
            (
                "record-gap",
                "0001.meta",
                "method=GET\nurl=https://capture.test/\nsuccess=1\nstatus=200\n"
                "length=0\ncontent-type=text/plain\nresponse-header-count=0\n"
                "set-cookie-count=0\n",
                "trace record sequence is not contiguous",
            ),
            (
                "collapsed-redirect",
                "0000.meta",
                "method=GET\nurl=https://capture.test/\nsuccess=1\nstatus=200\n"
                "length=0\neffective-url=https://other.test/\ncontent-type=text/plain\n"
                "response-header-count=0\nset-cookie-count=0\n",
                "collapsed redirect/effective URL",
            ),
            (
                "response-cookie-without-retention-mode",
                "0000.meta",
                "method=GET\nurl=https://capture.test/\nsuccess=1\nstatus=200\n"
                "length=0\ncontent-type=text/plain\nresponse-header-count=0\n"
                "set-cookie-count=1\nset-cookie-0=value=x\n"
                "set-cookie-url-0=https://capture.test/\n",
                "response cookies require cookie-values metadata",
            ),
        )
        for label, record_name, metadata, expected in cases:
            with self.subTest(label=label), tempfile.TemporaryDirectory() as temporary:
                trace = Path(temporary)
                write_trace_meta(trace)
                (trace / record_name).write_text(metadata)
                completed = inspect(trace)
                self.assertEqual(completed.returncode, 2)
                self.assertIn(expected, completed.stderr)

    def test_redacted_response_cookies_parse_and_rebase_retained_ttls(self) -> None:
        origin_ms = 1_784_342_534_779
        with tempfile.TemporaryDirectory() as temporary:
            trace = Path(temporary)
            (trace / "trace.meta").write_text(
                f"psp-http-trace-clock=1\norigin-ms={origin_ms}\n"
                "capture-complete=yes\nrecord-count=1\n"
            )
            (trace / "0000.meta").write_text(
                "method=GET\nurl=https://www.npr.org/\nsuccess=1\nstatus=200\n"
                "length=0\ncontent-type=text/html\nresponse-header-count=0\n"
                "cookie-values=redacted\nset-cookie-count=2\n"
                "set-cookie-0=bm_ss=xxxxxxxxxx; Secure; SameSite=None; "
                "Domain=.npr.org; Path=/; HttpOnly; Max-Age=3600\n"
                "set-cookie-url-0=https://www.npr.org/\n"
                "set-cookie-1=__cf_bm=xxxxxxxx; HttpOnly; SameSite=None; Secure; "
                "Path=/; Domain=piano.io; Expires=Sat, 18 Jul 2026 05:57:53 GMT\n"
                "set-cookie-url-1=https://cdn.piano.io/api/tinypass.min.js\n"
            )
            completed = inspect(trace)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            inspected = json.loads(completed.stdout)
            self.assertEqual(inspected["response_cookies"], 2)
            self.assertEqual(inspected["clock_origin_ms"], origin_ms)

            script = r"""
const capture = require(process.argv[1]);
const trace = capture.loadTrace(process.argv[2]);
const now = 2000000000;
process.stdout.write(JSON.stringify(trace.records[0].cookies.map(
  cookie => ({ parsed: cookie, operation: capture.cookieReplayOperation(
    cookie, now, trace.originMs) })
)));
"""
            parsed = subprocess.run(
                (NODE, "-e", script, str(CAPTURE), str(trace)),
                check=False, capture_output=True, text=True,
            )
            self.assertEqual(parsed.returncode, 0, parsed.stderr)
            cookies = json.loads(parsed.stdout)
            npr = cookies[0]
            self.assertEqual(npr["parsed"]["domain"], ".npr.org")
            self.assertEqual(npr["parsed"]["sameSite"], "None")
            self.assertTrue(npr["parsed"]["httpOnly"])
            self.assertEqual(npr["operation"]["cookie"]["expires"], 2_000_003_600)
            piano = cookies[1]
            self.assertEqual(piano["parsed"]["domain"], ".piano.io")
            retained_expiry = 1_784_354_273
            self.assertAlmostEqual(
                piano["operation"]["cookie"]["expires"],
                2_000_000_000 + retained_expiry - origin_ms / 1000,
            )

    def test_cookie_parser_fails_closed_on_gaps_and_unsupported_semantics(self) -> None:
        cases = (
            (
                "gap",
                "set-cookie-count=1\nset-cookie-1=a=x; Path=/\n"
                "set-cookie-url-1=https://capture.test/\n",
                "gap at 0",
            ),
            (
                "extra",
                "set-cookie-count=0\nset-cookie-0=a=x; Path=/\n"
                "set-cookie-url-0=https://capture.test/\n",
                "response cookie count mismatch",
            ),
            (
                "noncanonical-index",
                "set-cookie-count=1\nset-cookie-00=a=x; Path=/\n"
                "set-cookie-url-00=https://capture.test/\n",
                "invalid response cookie index",
            ),
            (
                "nonnumeric-index",
                "set-cookie-count=0\nset-cookie-value=a=x; Path=/\n",
                "invalid response cookie index",
            ),
            (
                "domain-mismatch",
                "set-cookie-count=1\nset-cookie-0=a=x; Domain=other.test; Path=/\n"
                "set-cookie-url-0=https://capture.test/\n",
                "does not domain-match",
            ),
            (
                "unsupported-partitioned",
                "set-cookie-count=1\nset-cookie-0=a=x; Secure; Partitioned; Path=/\n"
                "set-cookie-url-0=https://capture.test/\n",
                "unsupported cookie attribute Partitioned",
            ),
            (
                "insecure-samesite-none",
                "set-cookie-count=1\nset-cookie-0=a=x; SameSite=None; Path=/\n"
                "set-cookie-url-0=https://capture.test/\n",
                "SameSite=None requires Secure",
            ),
            (
                "invalid-date",
                "set-cookie-count=1\nset-cookie-0=a=x; Path=/; "
                "Expires=Tue, 31 Feb 2026 00:00:00 GMT\n"
                "set-cookie-url-0=https://capture.test/\n",
                "invalid Expires calendar date",
            ),
            (
                "not-redacted",
                "set-cookie-count=1\nset-cookie-0=a=secret; Path=/\n"
                "set-cookie-url-0=https://capture.test/\n",
                "is not deterministically redacted",
            ),
        )
        for label, fields, expected in cases:
            with self.subTest(label=label), tempfile.TemporaryDirectory() as temporary:
                trace = Path(temporary)
                (trace / "trace.meta").write_text(
                    "psp-http-trace-clock=1\norigin-ms=1000\n"
                    "capture-complete=yes\nrecord-count=1\n"
                )
                (trace / "0000.meta").write_text(
                    "method=GET\nurl=https://capture.test/\nsuccess=1\nstatus=200\n"
                    "length=0\ncontent-type=text/plain\nresponse-header-count=0\n"
                    "cookie-values=redacted\n" + fields
                )
                completed = inspect(trace)
                self.assertEqual(completed.returncode, 2)
                self.assertIn(expected, completed.stderr)

    def test_expires_only_cookie_requires_trace_clock_during_inspection(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            trace = Path(temporary)
            (trace / "trace.meta").write_text("capture-complete=yes\nrecord-count=1\n")
            (trace / "0000.meta").write_text(
                "method=GET\nurl=https://capture.test/\nsuccess=1\nstatus=200\n"
                "length=0\ncontent-type=text/plain\nresponse-header-count=0\n"
                "cookie-values=redacted\nset-cookie-count=1\n"
                "set-cookie-0=a=x; Path=/; Expires=Tue, 01 Jan 2036 08:00:01 GMT\n"
                "set-cookie-url-0=https://capture.test/\n"
            )
            completed = inspect(trace)
            self.assertEqual(completed.returncode, 2)
            self.assertIn(
                "invalid replay clock version",
                completed.stderr,
            )

    def test_merged_trace_cookie_uses_its_response_date_for_retained_ttl(self) -> None:
        origin_ms = 1_784_342_534_779
        with tempfile.TemporaryDirectory() as temporary:
            trace = Path(temporary)
            (trace / "trace.meta").write_text(
                f"psp-http-trace-clock=1\norigin-ms={origin_ms}\n"
                "capture-complete=yes\nrecord-count=2\n"
            )
            (trace / "0000.meta").write_text(
                "method=GET\nurl=https://www.npr.org/\nsuccess=1\nstatus=200\n"
                "length=0\ncontent-type=text/html\nresponse-header-count=1\n"
                "response-header-0=date: Sat, 18 Jul 2026 02:42:14 GMT\n"
                "set-cookie-count=0\n"
            )
            (trace / "0001.meta").write_text(
                "method=GET\nurl=https://cdn.piano.io/api/tinypass.min.js\n"
                "success=1\nstatus=200\nlength=0\ncontent-type=text/javascript\n"
                "response-header-count=1\n"
                "response-header-0=date: Sat, 18 Jul 2026 05:27:53 GMT\n"
                "cookie-values=redacted\nset-cookie-count=1\n"
                "set-cookie-0=__cf_bm=xxxxxxxx; HttpOnly; SameSite=None; Secure; "
                "Path=/; Domain=piano.io; Expires=Sat, 18 Jul 2026 05:57:53 GMT\n"
                "set-cookie-url-0=https://cdn.piano.io/api/tinypass.min.js\n"
            )
            completed = inspect(trace)
            self.assertEqual(completed.returncode, 0, completed.stderr)

            script = r"""
const capture = require(process.argv[1]);
const trace = capture.loadTrace(process.argv[2]);
const record = trace.records[1];
const operation = capture.cookieReplayOperation(
  record.cookies[0], 2000000000, trace.originMs, record.responseDateSeconds);
process.stdout.write(JSON.stringify({ baseline: record.responseDateSeconds, operation }));
"""
            parsed = subprocess.run(
                (NODE, "-e", script, str(CAPTURE), str(trace)),
                check=False, capture_output=True, text=True,
            )
            self.assertEqual(parsed.returncode, 0, parsed.stderr)
            result = json.loads(parsed.stdout)
            self.assertEqual(result["baseline"], 1_784_352_473)
            self.assertEqual(result["operation"]["cookie"]["expires"], 2_000_001_800)

    def test_cookie_response_date_is_strict_only_when_expiry_needs_it(self) -> None:
        cases = (
            (
                "malformed",
                "response-header-count=1\nresponse-header-0=date: not-a-date\n",
                "malformed response Date for cookie expiry",
            ),
            (
                "ambiguous",
                "response-header-count=2\n"
                "response-header-0=date: Sat, 18 Jul 2026 05:27:53 GMT\n"
                "response-header-1=Date: Sat, 18 Jul 2026 05:27:54 GMT\n",
                "multiple response Date headers are ambiguous",
            ),
        )
        for label, headers, expected in cases:
            with self.subTest(label=label), tempfile.TemporaryDirectory() as temporary:
                trace = Path(temporary)
                (trace / "trace.meta").write_text(
                    "psp-http-trace-clock=1\norigin-ms=1784342534779\n"
                    "capture-complete=yes\nrecord-count=1\n"
                )
                (trace / "0000.meta").write_text(
                    "method=GET\nurl=https://capture.test/\nsuccess=1\nstatus=200\n"
                    "length=0\ncontent-type=text/plain\n" + headers +
                    "cookie-values=redacted\nset-cookie-count=1\n"
                    "set-cookie-0=a=x; Path=/; Expires=Sat, 18 Jul 2026 05:57:53 GMT\n"
                    "set-cookie-url-0=https://capture.test/\n"
                )
                completed = inspect(trace)
                self.assertEqual(completed.returncode, 2)
                self.assertIn(expected, completed.stderr)

        with tempfile.TemporaryDirectory() as temporary:
            trace = Path(temporary)
            write_trace_meta(trace)
            (trace / "0000.meta").write_text(
                "method=GET\nurl=https://capture.test/\nsuccess=1\nstatus=200\n"
                "length=0\ncontent-type=text/plain\nresponse-header-count=1\n"
                "response-header-0=date: retained-but-malformed\nset-cookie-count=0\n"
            )
            completed = inspect(trace)
            self.assertEqual(completed.returncode, 0, completed.stderr)

    def test_raw_set_cookie_response_header_is_not_folded_or_silently_dropped(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            trace = Path(temporary)
            write_trace_meta(trace)
            (trace / "0000.meta").write_text(
                "method=GET\nurl=https://capture.test/\nsuccess=1\nstatus=200\n"
                "length=0\ncontent-type=text/plain\nresponse-header-count=1\n"
                "response-header-0=Set-Cookie: a=x; Path=/\nset-cookie-count=0\n"
            )
            completed = inspect(trace)
            self.assertEqual(completed.returncode, 2)
            self.assertIn("must use structured numbered fields", completed.stderr)

    def test_cookie_operations_preserve_order_without_repeated_header_folding(self) -> None:
        script = r"""
const capture = require(process.argv[1]);
const calls = [];
const context = {
  addCookies: async cookies => calls.push(["add", cookies]),
  clearCookies: async scope => calls.push(["delete", scope]),
};
const add = capture.parseSetCookie(
  "first=x; Path=/; Secure; HttpOnly", "https://capture.test/path");
const remove = capture.parseSetCookie(
  "first=; Path=/; Secure; Max-Age=0", "https://capture.test/path");
capture.applyResponseCookies(context, [add, remove], 2000000000, 1000).then(
  () => process.stdout.write(JSON.stringify(calls)),
  error => { process.stderr.write(String(error)); process.exitCode = 1; },
);
"""
        completed = subprocess.run(
            (NODE, "-e", script, str(CAPTURE)),
            check=False, capture_output=True, text=True,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        calls = json.loads(completed.stdout)
        self.assertEqual([call[0] for call in calls], ["add", "delete"])
        self.assertEqual(calls[0][1][0]["name"], "first")
        self.assertEqual(calls[1][1], {
            "name": "first", "domain": "capture.test", "path": "/",
        })

    def test_duplicate_metadata_keys_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            trace = Path(temporary)
            write_trace_meta(trace)
            (trace / "0000.meta").write_text(
                "method=GET\nmethod=POST\nurl=https://capture.test/\nsuccess=1\n"
                "status=200\nlength=0\ncontent-type=text/plain\n"
                "response-header-count=0\nset-cookie-count=0\n"
            )
            completed = inspect(trace)
            self.assertEqual(completed.returncode, 2)
            self.assertIn("duplicate key method", completed.stderr)

    def test_replay_environment_is_repeatable_and_preserves_api_contracts(self) -> None:
        script = r"""
const capture = require(process.argv[1]);
const environment = {
  version: "deterministic-hermetic-v3",
  clockVersion: "playwright-clock-paused-v2",
  clockContract: "dual-domain-ms-call-v2",
  clockScope: "top-level-realm-v1",
  rngVersion: "splitmix64-url-scope-v1",
  seedSource: "trace-origin-ms-v1",
  intlContract: "bounded-en-us-utc-v1",
  seedSha256: "0".repeat(64),
  seedU64: String(0x0123456789abcdefn),
  originMs: 1700000000042,
};
globalThis.location = { href: "https://example.test/path?q=%E2%9C%93#frag" };
globalThis.top = globalThis;
class IsolatedCrypto {
  getRandomValues(array) { return array; }
  randomUUID() { return "00000000-0000-4000-8000-000000000000"; }
}
class IsolatedPerformance {
  now() { return 999; }
  get timeOrigin() { return 0; }
  mark() { return null; }
  measure() { return null; }
  clearMarks() {}
  clearMeasures() {}
  clearResourceTimings() {}
  setResourceTimingBufferSize() {}
  getEntries() { return [1]; }
  getEntriesByType() { return [1]; }
  getEntriesByName() { return [1]; }
}
class IsolatedTimeline { get currentTime() { return 999; } }
globalThis.Crypto = IsolatedCrypto;
Object.defineProperty(globalThis, "crypto", {
  value: new IsolatedCrypto(), configurable: true,
});
globalThis.Performance = IsolatedPerformance;
globalThis.performance = new IsolatedPerformance();
globalThis.AnimationTimeline = IsolatedTimeline;
globalThis.document = { timeline: new IsolatedTimeline() };
globalThis.requestAnimationFrame = function requestAnimationFrame(callback) {
  callback(999); return 1;
};
const originalDate = Date;
const descriptors = {
  random: Object.getOwnPropertyDescriptor(Math, "random"),
  randomValues: Object.getOwnPropertyDescriptor(IsolatedCrypto.prototype, "getRandomValues"),
  uuid: Object.getOwnPropertyDescriptor(IsolatedCrypto.prototype, "randomUUID"),
};
capture.installReplayEnvironment(environment);
const mathHex1 = Math.floor(Math.random() * 2 ** 53).toString(16);
const bytes = globalThis.crypto.getRandomValues(new Uint8Array(13));
const uuid = globalThis.crypto.randomUUID();
const one = globalThis.crypto.getRandomValues(new Uint8Array(1));
const mathHex2 = Math.floor(Math.random() * 2 ** 53).toString(16);
let floatError = "";
let quotaError = "";
try { globalThis.crypto.getRandomValues(new Float32Array(1)); } catch (error) { floatError = error.name; }
try { globalThis.crypto.getRandomValues(new Uint8Array(65537)); } catch (error) { quotaError = error.name; }
const vector = [performance.timeOrigin, Date.now(), performance.now()];
globalThis.__tilefinchAdvanceReplayClock(16);
vector.push(Date.now(), performance.now());
requestAnimationFrame(timestamp => {
  vector.push(timestamp, Date.now(), performance.now(), document.timeline.currentTime);
});
for (let index = 0; index < 200; index += 1) {
  performance.mark(`bounded-${index}`, { startTime: index, detail: index });
}
let evictedMarkError = "";
try { performance.measure("evicted", "bounded-72", "bounded-199"); }
catch (error) { evictedMarkError = error.name; }
const retainedMeasure = performance.measure("retained", "bounded-73", "bounded-199");
performance.clearMarks("bounded-199");
let clearedMarkError = "";
try { performance.measure("cleared", "bounded-199"); }
catch (error) { clearedMarkError = error.name; }
performance.clearMeasures("retained");
const flags = descriptor => [descriptor.configurable, descriptor.enumerable, descriptor.writable];
process.stdout.write(JSON.stringify({
  marker: globalThis.__tilefinchReplayEnvironment,
  clock: globalThis.__tilefinchReplayClock,
  mixed: [mathHex1, Buffer.from(bytes).toString("hex"), uuid,
    Buffer.from(one).toString("hex"), mathHex2],
  vector,
  performanceBound: {
    evictedMarkError, clearedMarkError,
    retained: retainedMeasure.toJSON(),
    detailOwn: Object.prototype.hasOwnProperty.call(retainedMeasure, "detail"),
    detail: retainedMeasure.detail ?? null,
    visible: performance.getEntries(),
  },
  floatError, quotaError,
  shape: {
    random: [Math.random.name, Math.random.length, Function.prototype.toString.call(Math.random)],
    randomValues: [globalThis.crypto.getRandomValues.name, globalThis.crypto.getRandomValues.length,
      Function.prototype.toString.call(globalThis.crypto.getRandomValues)],
    uuid: [globalThis.crypto.randomUUID.name, globalThis.crypto.randomUUID.length,
      Function.prototype.toString.call(globalThis.crypto.randomUUID)],
    flags: [flags(Object.getOwnPropertyDescriptor(Math, "random")),
      flags(Object.getOwnPropertyDescriptor(IsolatedCrypto.prototype, "getRandomValues")),
      flags(Object.getOwnPropertyDescriptor(IsolatedCrypto.prototype, "randomUUID"))],
    originalFlags: [flags(descriptors.random), flags(descriptors.randomValues), flags(descriptors.uuid)],
    uuidOwn: Object.prototype.hasOwnProperty.call(globalThis.crypto, "randomUUID"),
    dateInstance: new Date() instanceof originalDate,
    dateNative: Function.prototype.toString.call(Date).includes("[native code]"),
  },
  clockEvidence: globalThis.__tilefinchReplayClockEvidence(),
}));
"""
        outputs = []
        for _ in range(2):
            completed = subprocess.run(
                (NODE, "-e", script, str(CAPTURE)),
                check=False, capture_output=True, text=True,
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)
            outputs.append(json.loads(completed.stdout))
        self.assertEqual(outputs[0], outputs[1])
        self.assertEqual(outputs[0]["marker"], "deterministic-hermetic-v3")
        self.assertEqual(outputs[0]["clock"], "dual-domain-ms-call-v2")
        self.assertEqual(
            outputs[0]["mixed"],
            [
                "8a9695e646a54",
                "eae9cebd39442a5da02e95ab39",
                "0b2b3939-b393-4980-b97f-204ef0dd9220",
                "92",
                "d00a601d20868",
            ],
        )
        self.assertEqual(
            outputs[0]["vector"],
            [
                1_700_000_000_042,
                1_700_000_000_042,
                0,
                1_700_000_000_059,
                17,
                18,
                1_700_000_000_060,
                18,
                19,
            ],
        )
        self.assertEqual(outputs[0]["floatError"], "TypeMismatchError")
        self.assertEqual(outputs[0]["quotaError"], "QuotaExceededError")
        self.assertEqual(
            outputs[0]["performanceBound"],
            {
                "evictedMarkError": "SyntaxError",
                "clearedMarkError": "SyntaxError",
                "retained": {
                    "name": "retained", "entryType": "measure",
                    "startTime": 73, "duration": 126,
                },
                "detailOwn": True,
                "detail": None,
                "visible": [],
            },
        )
        shape = outputs[0]["shape"]
        self.assertEqual(shape["flags"], shape["originalFlags"])
        self.assertFalse(shape["uuidOwn"])
        self.assertTrue(shape["dateInstance"])
        self.assertTrue(shape["dateNative"])
        for function_shape in (shape["random"], shape["randomValues"], shape["uuid"]):
            self.assertIn("[native code]", function_shape[2])
        self.assertEqual(
            outputs[0]["clockEvidence"],
            {
                "contract": "dual-domain-ms-call-v2",
                "scope": "top-level-realm-v1",
                "originMs": 1_700_000_000_042,
                "hostElapsedMs": 16,
                "playwrightElapsedMs": 0,
                "wallElapsedMs": 20,
                "monotonicElapsedMs": 20,
                "wallObservations": 4,
                "monotonicObservations": 4,
                "monotonicSamples": 1,
                "clockSources": {
                    "date_now": 3,
                    "date_function": 0,
                    "date_constructor": 1,
                    "performance_now": 3,
                    "performance_mark": 0,
                    "performance_measure": 0,
                    "animation_timeline": 1,
                    "idle_deadline_time_remaining": 0,
                    "animation_frame": 1,
                    "event_timestamp": 0,
                    "intersection_observer": 0,
                    "idle_callback_start": 0,
                },
            },
        )

    def test_replay_seed_is_append_stable_and_names_its_trace_source(self) -> None:
        script = r"""
const capture = require(process.argv[1]);
const left = capture.replayEnvironment({ originMs: 1784342534779, digest: "a".repeat(64) });
const right = capture.replayEnvironment({ originMs: 1784342534779, digest: "b".repeat(64) });
let outOfRange = "";
try { capture.replayEnvironment({ originMs: 8640000000000001 }); }
catch (error) { outOfRange = error.message; }
process.stdout.write(JSON.stringify({ left, right, outOfRange }));
"""
        completed = subprocess.run(
            (NODE, "-e", script, str(CAPTURE)),
            check=False, capture_output=True, text=True,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        state = json.loads(completed.stdout)
        self.assertEqual(state["left"], state["right"])
        self.assertEqual(state["left"]["seedSource"], "trace-origin-ms-v1")
        self.assertEqual(state["left"]["seedU64"], "1784342534779")
        self.assertIn("origin-ms", state["outOfRange"])

    def test_replay_clock_v2_has_exact_domains_and_stable_callback_samples(self) -> None:
        script = r"""
const capture = require(process.argv[1]);
const origin = 1700000000042;
globalThis.location = { href: "https://clock-sources.test/" };
globalThis.top = globalThis;
const nowDescriptor = Object.getOwnPropertyDescriptor(Date, "now");
Object.defineProperty(Date, "now", { ...nowDescriptor, value: () => origin });
class IsolatedPerformance {
  now() { return 999; }
  get timeOrigin() { return 0; }
  mark() { return null; }
  measure() { return null; }
  clearMarks() {}
  clearMeasures() {}
  clearResourceTimings() {}
  setResourceTimingBufferSize() {}
  getEntries() { return []; }
  getEntriesByType() { return []; }
  getEntriesByName() { return []; }
}
class IsolatedTimeline { get currentTime() { return 999; } }
class IsolatedEvent { get timeStamp() { return 999; } }
class IsolatedIntersectionObserverEntry { get time() { return 999; } }
class IsolatedIntersectionObserver {
  constructor(callback) { this.callback = callback; }
  trigger(entries) { this.callback(entries, this); }
  observe() {}
  disconnect() {}
}
class IsolatedIdleDeadline {
  get didTimeout() { return false; }
  timeRemaining() { return 999; }
}
globalThis.Performance = IsolatedPerformance;
globalThis.performance = new IsolatedPerformance();
globalThis.AnimationTimeline = IsolatedTimeline;
globalThis.document = { timeline: new IsolatedTimeline() };
globalThis.Event = IsolatedEvent;
globalThis.IntersectionObserverEntry = IsolatedIntersectionObserverEntry;
globalThis.IntersectionObserver = IsolatedIntersectionObserver;
globalThis.IdleDeadline = IsolatedIdleDeadline;
const frames = [];
globalThis.requestAnimationFrame = callback => { frames.push(callback); return frames.length; };
let idleCallback = null;
globalThis.requestIdleCallback = callback => { idleCallback = callback; return 1; };
capture.installReplayEnvironment({
  version: "deterministic-hermetic-v3",
  clockVersion: "playwright-clock-paused-v2",
  clockContract: "dual-domain-ms-call-v2",
  clockScope: "top-level-realm-v1",
  rngVersion: "splitmix64-url-scope-v1",
  seedSource: "trace-origin-ms-v1",
  intlContract: "bounded-en-us-utc-v1",
  seedSha256: "0".repeat(64),
  seedU64: "42",
  originMs: origin,
});
const wall = [Date.now(), Date(), new Date().getTime()];
const performanceNow = performance.now();
const automaticMark = performance.mark("automatic");
performance.mark("explicit", { startTime: 1 });
const implicitMeasure = performance.measure("implicit", "explicit");
const timeline = document.timeline.currentTime;
const event = new Event("probe");
globalThis.__tilefinchAdvanceReplayClock(5);
const eventTimes = [event.timeStamp, event.timeStamp];
const observer = new IntersectionObserver(() => {});
const entries = [new IntersectionObserverEntry(), new IntersectionObserverEntry()];
observer.trigger(entries);
const intersectionTimes = [entries[0].time, entries[1].time, entries[0].time];
const frameTimes = [];
requestAnimationFrame(timestamp => frameTimes.push(timestamp));
requestAnimationFrame(timestamp => frameTimes.push(timestamp));
frames[0](99); frames[1](99);
const idle = {};
requestIdleCallback(deadline => {
  idle.values = [deadline.timeRemaining(), deadline.timeRemaining()];
});
idleCallback(new IdleDeadline());
process.stdout.write(JSON.stringify({
  wall: [wall[0], typeof wall[1], wall[2]], performanceNow,
  automaticMark: automaticMark.startTime,
  implicitMeasure: implicitMeasure.toJSON(), timeline,
  eventTimes, intersectionTimes, frameTimes, idle,
  evidence: globalThis.__tilefinchReplayClockEvidence(),
}));
"""
        completed = subprocess.run(
            (NODE, "-e", script, str(CAPTURE)),
            check=False, capture_output=True, text=True,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        state = json.loads(completed.stdout)
        self.assertEqual(state["wall"], [1_700_000_000_042, "string", 1_700_000_000_044])
        self.assertEqual(state["performanceNow"], 0)
        self.assertEqual(state["automaticMark"], 1)
        self.assertEqual(
            state["implicitMeasure"],
            {"name": "implicit", "entryType": "measure", "startTime": 1, "duration": 1},
        )
        self.assertEqual(state["timeline"], 3)
        self.assertEqual(state["eventTimes"], [4, 4])
        self.assertEqual(state["intersectionTimes"], [9, 9, 9])
        self.assertEqual(state["frameTimes"], [9, 9])
        self.assertEqual(state["idle"], {"values": [8, 7]})
        self.assertEqual(
            state["evidence"],
            {
                "contract": "dual-domain-ms-call-v2",
                "scope": "top-level-realm-v1",
                "originMs": 1_700_000_000_042,
                "hostElapsedMs": 5,
                "playwrightElapsedMs": 0,
                "wallElapsedMs": 8,
                "monotonicElapsedMs": 11,
                "wallObservations": 3,
                "monotonicObservations": 6,
                "monotonicSamples": 4,
                "clockSources": {
                    "date_now": 1,
                    "date_function": 1,
                    "date_constructor": 1,
                    "performance_now": 1,
                    "performance_mark": 1,
                    "performance_measure": 1,
                    "animation_timeline": 1,
                    "idle_deadline_time_remaining": 2,
                    "animation_frame": 1,
                    "event_timestamp": 1,
                    "intersection_observer": 1,
                    "idle_callback_start": 1,
                },
            },
        )

    def test_replay_wall_clock_saturates_at_the_date_limit(self) -> None:
        script = r"""
const capture = require(process.argv[1]);
globalThis.location = { href: "https://date-limit.test/" };
globalThis.top = globalThis;
capture.installReplayEnvironment({
  version: "deterministic-hermetic-v3",
  clockVersion: "playwright-clock-paused-v2",
  clockContract: "dual-domain-ms-call-v2",
  clockScope: "top-level-realm-v1",
  rngVersion: "splitmix64-url-scope-v1",
  seedSource: "trace-origin-ms-v1",
  intlContract: "bounded-en-us-utc-v1",
  seedSha256: "0".repeat(64),
  seedU64: "8639999999999998",
  originMs: 8639999999999998,
});
const called = Date();
const vector = [
  Date.now(), new Date().getTime(), Date.parse(called), Date.now(), performance.now(),
];
globalThis.__tilefinchAdvanceReplayClock(1000);
vector.push(Date.now(), performance.now());
process.stdout.write(JSON.stringify({
  vector, evidence: globalThis.__tilefinchReplayClockEvidence(),
}));
"""
        completed = subprocess.run(
            (NODE, "-e", script, str(CAPTURE)),
            check=False, capture_output=True, text=True,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        state = json.loads(completed.stdout)
        self.assertEqual(
            state["vector"],
            [
                8_639_999_999_999_999,
                8_640_000_000_000_000,
                8_639_999_999_999_000,
                8_640_000_000_000_000,
                0,
                8_640_000_000_000_000,
                1001,
            ],
        )
        self.assertEqual(state["evidence"]["hostElapsedMs"], 1000)
        self.assertEqual(state["evidence"]["wallElapsedMs"], 1005)
        self.assertEqual(state["evidence"]["monotonicElapsedMs"], 1002)
        self.assertEqual(state["evidence"]["wallObservations"], 5)
        self.assertEqual(state["evidence"]["monotonicObservations"], 2)
        self.assertEqual(state["evidence"]["monotonicSamples"], 0)
        self.assertEqual(
            state["evidence"]["clockSources"],
            {
                "date_now": 3,
                "date_function": 1,
                "date_constructor": 1,
                "performance_now": 2,
                "performance_mark": 0,
                "performance_measure": 0,
                "animation_timeline": 0,
                "idle_deadline_time_remaining": 0,
                "animation_frame": 0,
                "event_timestamp": 0,
                "intersection_observer": 0,
                "idle_callback_start": 0,
            },
        )

    def test_replay_intl_is_the_bounded_native_surface(self) -> None:
        script = r"""
const capture = require(process.argv[1]);
globalThis.location = { href: "https://intl.test/" };
globalThis.top = globalThis;
capture.installReplayEnvironment({
  version: "deterministic-hermetic-v3",
  clockVersion: "playwright-clock-paused-v2",
  clockContract: "dual-domain-ms-call-v2",
  clockScope: "top-level-realm-v1",
  rngVersion: "splitmix64-url-scope-v1",
  seedSource: "trace-origin-ms-v1",
  intlContract: "bounded-en-us-utc-v1",
  seedSha256: "0".repeat(64),
  seedU64: "1700000000042",
  originMs: 1700000000042,
});
const instant = 1700000000042;
const date = new Intl.DateTimeFormat("en-US", { timeZone: "UTC" });
const time = new Intl.DateTimeFormat("en-US", { timeZone: "UTC", hour: "numeric" });
let localeCall = "";
let pluralCall = "";
try { Intl.Locale("en-US"); } catch (error) { localeCall = error.name; }
try { Intl.PluralRules("en-US"); } catch (error) { pluralCall = error.name; }
process.stdout.write(JSON.stringify({
  keys: Object.keys(Intl),
  shapes: Object.entries(Intl).map(([key, value]) => [key, value.name, value.length]),
  date: date.format(instant),
  dateParts: date.formatToParts(instant).map(part => `${part.type}:${part.value}`),
  dateOptions: date.resolvedOptions(),
  time: time.format(instant),
  number: new Intl.NumberFormat("en-US").format(1234.5),
  percent: new Intl.NumberFormat("en-US", { style: "percent" }).format(.25),
  currency: new Intl.NumberFormat("en-US", {
    style: "currency", currency: "USD",
  }).format(1234.5),
  plurals: [
    new Intl.PluralRules("en-US").select(1),
    new Intl.PluralRules("en-US").select(2),
    new Intl.PluralRules("en-US", { type: "ordinal" }).select(2),
  ],
  locale: String(new Intl.Locale("en_US")),
  collator: new Intl.Collator("en-US", { numeric: true }).compare("a2", "a10"),
  relative: new Intl.RelativeTimeFormat("en-US", { numeric: "auto" }).format(-1, "day"),
  list: new Intl.ListFormat("en-US").format(["a", "b", "c"]),
  display: new Intl.DisplayNames("en-US", { type: "region" }).of("US"),
  localeCall, pluralCall,
  constructorsNative: Object.values(Intl).filter(value => typeof value === "function")
    .every(value => Function.prototype.toString.call(value).includes("[native code]")),
  methodsNative: [
    Intl.DateTimeFormat.prototype.format,
    Intl.DateTimeFormat.prototype.formatToParts,
    Intl.NumberFormat.prototype.format,
    Object.getOwnPropertyDescriptor(Intl.Collator.prototype, "compare").get,
  ].every(value => Function.prototype.toString.call(value).includes("[native code]")),
  dateConstructor: Date.prototype.constructor === Date,
}));
"""
        completed = subprocess.run(
            (NODE, "-e", script, str(CAPTURE)),
            check=False, capture_output=True, text=True,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        state = json.loads(completed.stdout)
        self.assertEqual(
            state["keys"],
            [
                "Locale", "NumberFormat", "PluralRules", "DateTimeFormat",
                "Collator", "RelativeTimeFormat", "ListFormat", "DisplayNames",
                "getCanonicalLocales",
            ],
        )
        self.assertEqual(
            state["shapes"],
            [
                ["Locale", "Locale", 1],
                ["NumberFormat", "NumberFormat", 0],
                ["PluralRules", "PluralRules", 1],
                ["DateTimeFormat", "DateTimeFormat", 0],
                ["Collator", "Collator", 0],
                ["RelativeTimeFormat", "RelativeTimeFormat", 1],
                ["ListFormat", "ListFormat", 1],
                ["DisplayNames", "DisplayNames", 1],
                ["getCanonicalLocales", "getCanonicalLocales", 1],
            ],
        )
        self.assertEqual(state["date"], "11/14/2023")
        self.assertEqual(
            state["dateParts"],
            ["month:11", "literal:/", "day:14", "literal:/", "year:2023"],
        )
        self.assertEqual(
            state["dateOptions"],
            {
                "locale": "en-US", "calendar": "gregory",
                "numberingSystem": "latn", "timeZone": "UTC",
                "year": "numeric", "month": "numeric", "day": "numeric",
            },
        )
        self.assertEqual(state["time"], "10:13:20 PM")
        self.assertEqual(
            (state["number"], state["percent"], state["currency"]),
            ("1,234.5", "25%", "USD 1,234.5"),
        )
        self.assertEqual(state["plurals"], ["one", "other", "two"])
        self.assertEqual(state["locale"], "en-US")
        self.assertEqual(state["collator"], -1)
        self.assertEqual(state["relative"], "yesterday")
        self.assertEqual(state["list"], "a, b, and c")
        self.assertEqual(state["display"], "United States")
        self.assertEqual((state["localeCall"], state["pluralCall"]), ("TypeError", "TypeError"))
        self.assertTrue(state["constructorsNative"])
        self.assertTrue(state["methodsNative"])
        self.assertTrue(state["dateConstructor"])

    def test_read_only_policy_denies_mutations_before_native_network_apis(self) -> None:
        script = r"""
const capture = require(process.argv[1]);
const calls = [];
const denied = [];
globalThis.document = { baseURI: "https://fixture.test/base/" };
globalThis.location = { origin: "https://fixture.test" };
globalThis.__tilefinchReadOnlyDenied = (...items) => { denied.push(items); return Promise.resolve(); };
globalThis.fetch = function fetch(input, init) { calls.push(["fetch", input, init]); return Promise.resolve("ok"); };
globalThis.fetchLater = function fetchLater(input, init) { calls.push(["fetchLater", input, init]); return {}; };
class FakeRequest {
  constructor(input, init = {}) {
    const source = input instanceof FakeRequest ? input : null;
    this.url = new URL(source ? source.url : input, document.baseURI).href;
    this.method = String(init.method || (source && source.method) || "GET").toUpperCase();
    this.mode = String(init.mode || (source && source.mode) || "cors");
    this.headers = new Headers(source ? source.headers : undefined);
    if (init.headers !== undefined) {
      this.headers = new Headers(init.headers);
    }
  }
}
globalThis.Request = FakeRequest;
class FakeXHR {
  open(method, url) { calls.push(["open", method, url]); }
  setRequestHeader(name, value) { calls.push(["header", name, value]); }
  send(body) { calls.push(["send", body]); }
}
globalThis.XMLHttpRequest = FakeXHR;
class FakeNavigator {}
FakeNavigator.prototype.sendBeacon = function sendBeacon(url) {
  if (!(this instanceof FakeNavigator)) throw new TypeError("Illegal invocation");
  calls.push(["beacon", url]); return true;
};
globalThis.Navigator = FakeNavigator;
class FakeForm {
  constructor(method) { this.method = method; this.action = "/submit"; }
  submit() { calls.push(["submit", this.method]); }
  requestSubmit() { calls.push(["requestSubmit", this.method, arguments.length]); }
}
globalThis.HTMLFormElement = FakeForm;
globalThis.addEventListener = () => {};
const descriptorFlags = value => [value.configurable, value.enumerable, value.writable];
const originalShape = {
  fetch: [fetch.name, fetch.length, descriptorFlags(Object.getOwnPropertyDescriptor(globalThis, "fetch"))],
  open: [FakeXHR.prototype.open.name, FakeXHR.prototype.open.length,
    descriptorFlags(Object.getOwnPropertyDescriptor(FakeXHR.prototype, "open"))],
};
capture.installReadOnlyPolicy({
  version: "get-head-only-v3",
  preflightPolicy: "cors-preflight-before-network-v1",
});
(async () => {
  await fetch("/safe", { method: "GET" });
  let fetchError = "";
  try { await fetch("/mutate", { method: "POST" }); } catch (error) { fetchError = error.name; }
  let preflightFetchError = "";
  try {
    await fetch("https://cross.test/data", { headers: { "X-Custom": "one" } });
  } catch (error) { preflightFetchError = error.name; }
  let fetchLaterError = "";
  try { fetchLater("/delayed", { method: "GET" }); } catch (error) { fetchLaterError = error.name; }
  const safe = new XMLHttpRequest(); safe.open("HEAD", "/safe"); safe.send();
  const unsafe = new XMLHttpRequest(); unsafe.open("POST", "/mutate");
  let xhrError = "";
  try { unsafe.send("body"); } catch (error) { xhrError = error.name; }
  const cross = new XMLHttpRequest(); cross.open("GET", "https://cross.test/xhr");
  cross.setRequestHeader("X-Custom", "one");
  let preflightXhrError = "";
  try { cross.send(); } catch (error) { preflightXhrError = error.name; }
  const beaconResult = Navigator.prototype.sendBeacon.call(new Navigator(), "/beacon");
  let brandError = "";
  try { Navigator.prototype.sendBeacon.call({}, "/beacon"); } catch (error) { brandError = error.name; }
  new HTMLFormElement("post").submit();
  new HTMLFormElement("dialog").submit();
  new HTMLFormElement("get").requestSubmit();
  process.stdout.write(JSON.stringify({
    calls, denied, fetchError, preflightFetchError, fetchLaterError, xhrError,
    preflightXhrError, beaconResult, brandError,
    marker: globalThis.__tilefinchReadOnlyPolicy,
    preflightMarker: globalThis.__tilefinchReadOnlyPreflightPolicy,
    shape: {
      fetch: [fetch.name, fetch.length, descriptorFlags(Object.getOwnPropertyDescriptor(globalThis, "fetch"))],
      open: [FakeXHR.prototype.open.name, FakeXHR.prototype.open.length,
        descriptorFlags(Object.getOwnPropertyDescriptor(FakeXHR.prototype, "open"))],
      native: [Function.prototype.toString.call(fetch),
        Function.prototype.toString.call(FakeXHR.prototype.open)],
    },
    originalShape,
  }));
})().catch(error => { process.stderr.write(String(error)); process.exitCode = 1; });
"""
        completed = subprocess.run(
            (NODE, "-e", script, str(CAPTURE)),
            check=False, capture_output=True, text=True,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        state = json.loads(completed.stdout)
        self.assertEqual(state["marker"], "get-head-only-v3")
        self.assertEqual(
            state["preflightMarker"], "cors-preflight-before-network-v1"
        )
        self.assertEqual(state["fetchError"], "TypeError")
        self.assertEqual(state["preflightFetchError"], "TypeError")
        self.assertEqual(state["fetchLaterError"], "NetworkError")
        self.assertEqual(state["xhrError"], "NetworkError")
        self.assertEqual(state["preflightXhrError"], "NetworkError")
        self.assertFalse(state["beaconResult"])
        self.assertEqual(state["brandError"], "TypeError")
        self.assertEqual(state["shape"]["fetch"], state["originalShape"]["fetch"])
        self.assertEqual(state["shape"]["open"], state["originalShape"]["open"])
        self.assertTrue(all("[native code]" in value for value in state["shape"]["native"]))
        self.assertEqual(
            [item[0] for item in state["denied"]],
            ["POST", "GET", "GET", "POST", "GET", "POST", "POST"],
        )
        self.assertEqual(
            [item[1] for item in state["denied"]],
            [
                "fetch", "fetch-cors-preflight", "fetchlater", "xmlhttprequest",
                "xmlhttprequest-cors-preflight", "beacon", "form-submit",
            ],
        )
        self.assertFalse(any(call[0] == "fetchLater" for call in state["calls"]))
        self.assertEqual(
            len([call for call in state["calls"] if call[0] == "fetch"]), 1
        )
        self.assertNotIn(["send", "body"], state["calls"])
        self.assertEqual(len([call for call in state["calls"] if call[0] == "send"]), 1)
        self.assertIn(["submit", "dialog"], state["calls"])
        self.assertIn(["requestSubmit", "get", 0], state["calls"])

    def test_request_diagnostics_and_read_only_plan_are_bounded_and_stable(self) -> None:
        script = r"""
const capture = require(process.argv[1]);
const forward = capture.createRequestDiagnostics();
const reverse = capture.createRequestDiagnostics();
const events = [];
for (let index = 0; index < 540; index += 1) {
  events.push(["unmatched", "GET", "script", `https://h${index % 70}.test/${index}`]);
}
for (const item of events) forward.record(...item);
for (const item of [...events].reverse()) reverse.record(...item);
const planned = capture.createRequestDiagnostics();
planned.record("unmatched", "GET", "image", "https://fixture.test/a");
planned.record("unmatched", "GET", "fetch", "https://fixture.test/a");
planned.record("unmatched", "POST", "fetch", "https://fixture.test/mutate");
process.stdout.write(JSON.stringify({
  forward: forward.summary(), reverse: reverse.summary(),
  plan: capture.buildReadOnlyAcquisitionPlan(planned.summary()),
}));
"""
        completed = subprocess.run(
            (NODE, "-e", script, str(CAPTURE)),
            check=False, capture_output=True, text=True,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        state = json.loads(completed.stdout)
        forward = state["forward"]
        reverse = state["reverse"]
        self.assertEqual((forward["total"], forward["retained"], forward["truncated"]), (540, 512, 28))
        self.assertEqual(forward["multiset_sha256"], reverse["multiset_sha256"])
        self.assertEqual(forward["entries"], reverse["entries"])
        self.assertEqual(forward["by_origin"], reverse["by_origin"])
        self.assertLessEqual(len(forward["by_origin"]), 65)
        plan = state["plan"]
        self.assertFalse(plan["complete"])
        self.assertEqual(plan["unplannable"], 1)
        self.assertEqual(plan["request_count"], 1)
        self.assertEqual(plan["requests"][0]["occurrences"], 2)
        self.assertEqual(plan["requests"][0]["resource_types"], ["fetch", "image"])

    def test_request_and_scheduler_ordering_is_locale_independent(self) -> None:
        script = r"""
String.prototype.localeCompare = function () {
  throw new Error("host locale ordering was used");
};
const capture = require(process.argv[1]);
const urls = [
  "https://ordering.test/\u00e4",
  "https://ordering.test/z",
  "https://ordering.test/A",
];
const diagnostics = capture.createRequestDiagnostics();
for (const url of [...urls].reverse()) {
  diagnostics.record("unmatched", "GET", "fetch", url);
}
const plan = capture.buildReadOnlyAcquisitionPlan(diagnostics.summary());
const claims = [];
const scheduler = capture.createResponseScheduler();
const deliveries = urls.map((url, browserOrdinal) =>
  scheduler.enqueueAdmission({
    routeKey: `GET\0${url}`, resourceType: "fetch",
    occurrence: 0, browserOrdinal,
  }, () => {
    claims.push(url);
    return {
      record: { id: String(browserOrdinal).padStart(4, "0"), asyncDelayPumps: 1 },
      work: async () => "served",
    };
  }),
);
Promise.all([...deliveries, scheduler.driveToIdle()]).then(() => {
  process.stdout.write(JSON.stringify({
    claims, planned: plan.requests.map(item => item.url),
    summary: scheduler.summary(),
  }));
}, error => { process.stderr.write(String(error)); process.exitCode = 1; });
"""
        completed = subprocess.run(
            (NODE, "-e", script, str(CAPTURE)),
            check=False,
            capture_output=True,
            text=True,
            timeout=3,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        state = json.loads(completed.stdout)
        expected = [
            "https://ordering.test/A",
            "https://ordering.test/z",
            "https://ordering.test/ä",
        ]
        self.assertEqual(state["claims"], expected)
        self.assertEqual(state["planned"], expected)
        self.assertTrue(state["summary"]["ready"])

    def test_response_scheduler_collects_inverted_arrivals_and_honors_pumps(self) -> None:
        script = r"""
const capture = require(process.argv[1]);
const events = [];
let scheduler;
let probePass = 0;
let active = 0;
const work = label => async () => {
  active += 1; events.push(`${label}:start:${active}`);
  await new Promise(resolve => setImmediate(resolve));
  events.push(`${label}:end:${active}`); active -= 1; return "served";
};
const identity = (routeKey, occurrence, browserOrdinal) => ({
  routeKey, resourceType: "script", occurrence, browserOrdinal,
});
scheduler = capture.createResponseScheduler({
  probe: async () => {
    probePass += 1;
    if (probePass === 1) {
      scheduler.enqueue(
        { id: "0001", asyncDelayPumps: 1 }, identity("GET\\0https://a.test/", 0, 1),
        work("one"),
      );
    }
  },
});
const second = scheduler.enqueue(
  { id: "0002", asyncDelayPumps: 1 }, identity("GET\\0https://b.test/", 0, 0),
  work("two"),
);
const delayed = scheduler.enqueue(
  { id: "0003", asyncDelayPumps: 3 }, identity("GET\\0https://c.test/", 0, 2),
  work("three"),
);
Promise.all([second, delayed, scheduler.driveToIdle()]).then(async () => {
  process.stdout.write(JSON.stringify({ events, summary: scheduler.summary() }));
}, error => { process.stderr.write(String(error)); process.exitCode = 1; });
"""
        completed = subprocess.run(
            (NODE, "-e", script, str(CAPTURE)),
            check=False, capture_output=True, text=True, timeout=3,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        state = json.loads(completed.stdout)
        self.assertEqual(
            state["events"],
            [
                "one:start:1", "one:end:1", "two:start:1", "two:end:1",
                "three:start:1", "three:end:1",
            ],
        )
        summary = state["summary"]
        self.assertEqual(set(summary), REFERENCE_RESPONSE_SCHEDULER_FIELDS)
        self.assertEqual(summary["version"], "admission-generations-v7")
        self.assertEqual(summary["terminal_boundary"], "all-http-terminal-v1")
        self.assertEqual(summary["order_digest_version"], "exact-request-identity-v3")
        self.assertEqual(
            summary["browser_ordinal_semantics"],
            "raw-playwright-route-callback-v1",
        )
        self.assertEqual(summary["pump_work_units"], 5)
        self.assertEqual(summary["scheduled_delay_work_units"], 5)
        self.assertEqual(summary["retained_delay_work_units"], 5)
        self.assertEqual(summary["terminal_delay_work_units"], 0)
        self.assertEqual(summary["retained_admissions"], 3)
        self.assertEqual(summary["terminal_admissions"], 0)
        self.assertNotIn("pump_epochs", summary)
        self.assertEqual(summary["batches"], 2)
        self.assertEqual(summary["semantic_pumps"], 3)
        self.assertEqual(summary["fast_forwarded_pumps"], 1)
        self.assertTrue(summary["ready"])
        self.assertEqual((summary["enqueued"], summary["completed"], summary["pending"]), (3, 3, 0))

    def test_response_scheduler_closes_claims_in_sorted_generation(self) -> None:
        script = r"""
const capture = require(process.argv[1]);
const claims = [];
const completions = [];
const scheduler = capture.createResponseScheduler();
const identity = (routeKey, browserOrdinal) => ({
  routeKey, resourceType: "fetch", occurrence: 0, browserOrdinal,
});
const enqueue = (id, routeKey, browserOrdinal) => scheduler.enqueueAdmission(
  identity(routeKey, browserOrdinal),
  () => {
    claims.push(routeKey);
    return {
      record: { id, asyncDelayPumps: 1 },
      work: async () => { completions.push(routeKey); return "served"; },
    };
  },
);
const second = enqueue("0001", "GET\\0https://b.test/", 0);
const first = enqueue("0002", "GET\\0https://a.test/", 1);
Promise.all([second, first, scheduler.driveToIdle()]).then(() => {
  process.stdout.write(JSON.stringify({ claims, completions, summary: scheduler.summary() }));
}, error => { process.stderr.write(String(error)); process.exitCode = 1; });
"""
        completed = subprocess.run(
            (NODE, "-e", script, str(CAPTURE)),
            check=False, capture_output=True, text=True, timeout=3,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        state = json.loads(completed.stdout)
        expected = ["GET\\0https://a.test/", "GET\\0https://b.test/"]
        self.assertEqual(state["claims"], expected)
        self.assertEqual(state["completions"], list(reversed(expected)))
        self.assertTrue(state["summary"]["ready"])

    def test_response_scheduler_unifies_terminal_and_retained_delivery(self) -> None:
        script = r"""
const capture = require(process.argv[1]);
const run = async reverse => {
  const scheduler = capture.createResponseScheduler();
  const delivered = [];
  const definitions = [
    {
      label: "unmatched-a", routeKey: "GET\0https://a.test/missing",
      resourceType: "fetch", record: { id: "0000", asyncDelayPumps: 1 },
      admissionKind: "terminal", disposition: "unmatched",
    },
    {
      label: "unmatched-b", routeKey: "GET\0https://b.test/missing",
      resourceType: "image", record: { id: "0000", asyncDelayPumps: 1 },
      admissionKind: "terminal", disposition: "unmatched",
    },
    {
      label: "matched", routeKey: "GET\0https://c.test/value",
      resourceType: "script", record: { id: "0001", asyncDelayPumps: 1 },
      admissionKind: "retained", disposition: "served",
    },
  ];
  const arrivals = reverse ? [...definitions].reverse() : definitions;
  const operations = arrivals.map((definition, browserOrdinal) =>
    scheduler.enqueueAdmission({
      routeKey: definition.routeKey, resourceType: definition.resourceType,
      occurrence: 0, browserOrdinal,
    }, () => ({
      record: definition.record, admissionKind: definition.admissionKind,
      work: async () => {
        delivered.push(`${definition.label}:${definition.disposition}`);
        return definition.disposition;
      },
    })),
  );
  await Promise.all([...operations, scheduler.driveToIdle()]);
  return { delivered, summary: scheduler.summary() };
};
Promise.all([run(false), run(true)]).then(([forward, reverse]) => {
  process.stdout.write(JSON.stringify({ forward, reverse }));
}, error => { process.stderr.write(String(error)); process.exitCode = 1; });
"""
        completed = subprocess.run(
            (NODE, "-e", script, str(CAPTURE)),
            check=False, capture_output=True, text=True, timeout=3,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        state = json.loads(completed.stdout)
        expected = [
            "unmatched-a:unmatched", "unmatched-b:unmatched", "matched:served",
        ]
        self.assertEqual(state["forward"]["delivered"], expected)
        self.assertEqual(state["reverse"]["delivered"], expected)
        for run in (state["forward"], state["reverse"]):
            summary = run["summary"]
            self.assertTrue(summary["ready"])
            self.assertEqual(summary["terminal_admissions"], 2)
            self.assertEqual(summary["retained_admissions"], 1)
            self.assertEqual(summary["scheduled_delay_work_units"], 3)
            self.assertEqual(summary["pump_work_units"], 3)
        # The scheduler never conceals real Playwright callback-order drift.
        self.assertNotEqual(
            state["forward"]["summary"]["raw_callback_arrival_sha256"],
            state["reverse"]["summary"]["raw_callback_arrival_sha256"],
        )
        self.assertEqual(
            state["forward"]["summary"]["semantic_delivery_order_sha256"],
            state["reverse"]["summary"]["semantic_delivery_order_sha256"],
        )
        self.assertNotEqual(
            state["forward"]["summary"]["order_sha256"],
            state["reverse"]["summary"]["order_sha256"],
        )

    def test_response_scheduler_uses_one_cross_resource_route_occurrence_stream(self) -> None:
        script = r"""
const capture = require(process.argv[1]);
const routeKey = "GET\0https://shared.test/value";
const route = {
  mode: "occurrence-sequence",
  candidates: [
    { id: "0000", asyncDelayPumps: 1 },
    { id: "0001", asyncDelayPumps: 1 },
  ],
};
const occurrences = new Map();
const claims = [];
const deliveries = [];
const scheduler = capture.createResponseScheduler();
const admission = (resourceType, occurrence, browserOrdinal) =>
  scheduler.enqueueAdmission(
    { routeKey, resourceType, occurrence, browserOrdinal },
    () => {
      const claim = capture.claimRouteRecord(route, occurrences, routeKey);
      claims.push({ resourceType, occurrence: claim.occurrence, id: claim.record.id });
      return {
        record: claim.record,
        work: async () => {
          deliveries.push({ resourceType, id: claim.record.id });
          return "served";
        },
      };
    },
  );
/* Intentionally enqueue in the opposite host-promise order. The callback
   ordinal/global occurrence, not resource-type lexicography, is authoritative. */
const fetch = admission("fetch", 1, 1);
const script = admission("script", 0, 0);
Promise.all([fetch, script, scheduler.driveToIdle()]).then(() => {
  process.stdout.write(JSON.stringify({
    claims, deliveries, occurrences: [...occurrences.entries()],
    summary: scheduler.summary(),
  }));
}, error => { process.stderr.write(String(error)); process.exitCode = 1; });
"""
        completed = subprocess.run(
            (NODE, "-e", script, str(CAPTURE)),
            check=False, capture_output=True, text=True, timeout=3,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        state = json.loads(completed.stdout)
        self.assertEqual(
            state["claims"],
            [
                {"resourceType": "script", "occurrence": 0, "id": "0000"},
                {"resourceType": "fetch", "occurrence": 1, "id": "0001"},
            ],
        )
        self.assertEqual(
            state["deliveries"],
            [
                {"resourceType": "script", "id": "0000"},
                {"resourceType": "fetch", "id": "0001"},
            ],
        )
        self.assertEqual(state["occurrences"], [["GET\0https://shared.test/value", 2]])
        self.assertTrue(state["summary"]["ready"])

    def test_response_scheduler_hashes_long_high_count_identities_incrementally(self) -> None:
        script = r"""
const capture = require(process.argv[1]);
const scheduler = capture.createResponseScheduler({ limit: 256 });
const operations = [];
for (let index = 0; index < 192; index += 1) {
  const suffix = String(index).padStart(4, "0") + "x".repeat(4096);
  operations.push(scheduler.enqueue(
    { id: String(index % 10000).padStart(4, "0"), asyncDelayPumps: 1 },
    { routeKey: `GET\0https://bounded.test/${suffix}`, resourceType: "fetch",
      occurrence: 0, browserOrdinal: index },
    async () => "served",
  ));
}
const before = scheduler.summary();
Promise.all([...operations, scheduler.driveToIdle()]).then(() => {
  const first = scheduler.summary();
  const second = scheduler.summary();
  process.stdout.write(JSON.stringify({ before, first, second }));
}, error => { process.stderr.write(String(error)); process.exitCode = 1; });
"""
        completed = subprocess.run(
            (NODE, "-e", script, str(CAPTURE)),
            check=False, capture_output=True, text=True, timeout=5,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        state = json.loads(completed.stdout)
        self.assertEqual(state["before"]["enqueued"], 192)
        self.assertEqual(state["before"]["completed"], 0)
        self.assertEqual(state["first"], state["second"])
        self.assertTrue(state["first"]["ready"])
        self.assertEqual(state["first"]["completed"], 192)
        self.assertEqual(state["first"]["exact_order_count"], 192)
        self.assertEqual(state["first"]["semantic_delivery_count"], 192)
        self.assertEqual(state["first"]["raw_callback_count"], 192)
        for field in (
            "order_sha256", "semantic_delivery_order_sha256",
            "raw_callback_arrival_sha256",
        ):
            self.assertRegex(state["first"][field], r"^[0-9a-f]{64}$")

    def test_response_scheduler_tracks_async_prepare_and_serializes_controls(self) -> None:
        script = r"""
const capture = require(process.argv[1]);
(async () => {
  let releasePreparation;
  let preparationStarted;
  const preparationGate = new Promise(resolve => { releasePreparation = resolve; });
  const started = new Promise(resolve => { preparationStarted = resolve; });
  let workRuns = 0;
  const scheduler = capture.createResponseScheduler({ prepareTimeoutMs: 1000 });
  const delivery = scheduler.enqueueAdmission({
    routeKey: "GET\0https://prepare.test/value", resourceType: "fetch",
    occurrence: 0, browserOrdinal: 0,
  }, async () => {
    preparationStarted();
    await preparationGate;
    return {
      record: { id: "0001", asyncDelayPumps: 1 },
      work: async () => { workRuns += 1; return "served"; },
    };
  });
  const close = scheduler.closeAdmissionGeneration();
  await started;
  let pumpSettled = false;
  let idleSettled = false;
  const pump = scheduler.pumpOnce().then(value => {
    pumpSettled = true;
    return value;
  });
  const idle = scheduler.whenIdle().then(() => { idleSettled = true; });
  await new Promise(resolve => setImmediate(resolve));
  const during = scheduler.summary();
  const overlap = { pumpSettled, idleSettled, workRuns };
  releasePreparation();
  const [closed, pumped, value] = await Promise.all([close, pump, delivery, idle])
    .then(results => [results[0], results[1], results[2]]);
  process.stdout.write(JSON.stringify({
    during, overlap, closed, pumped, value, workRuns,
    summary: scheduler.summary(),
  }));
})().catch(error => { process.stderr.write(String(error)); process.exitCode = 1; });
"""
        completed = subprocess.run(
            (NODE, "-e", script, str(CAPTURE)),
            check=False,
            capture_output=True,
            text=True,
            timeout=3,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        state = json.loads(completed.stdout)
        self.assertEqual(state["during"]["pending"], 1)
        self.assertFalse(state["during"]["ready"])
        self.assertEqual(
            state["overlap"],
            {"pumpSettled": False, "idleSettled": False, "workRuns": 0},
        )
        self.assertEqual((state["closed"], state["pumped"]), (1, 1))
        self.assertEqual((state["value"], state["workRuns"]), ("served", 1))
        self.assertTrue(state["summary"]["ready"])

    def test_response_scheduler_async_prepare_terminal_and_timeout_are_bounded(self) -> None:
        script = r"""
const capture = require(process.argv[1]);
const externalFailure = async () => {
  let releasePreparation;
  let preparationStarted;
  const gate = new Promise(resolve => { releasePreparation = resolve; });
  const started = new Promise(resolve => { preparationStarted = resolve; });
  const scheduler = capture.createResponseScheduler({ prepareTimeoutMs: 1000 });
  const delivery = scheduler.enqueueAdmission({
    routeKey: "GET\0https://prepare.test/external", resourceType: "fetch",
    occurrence: 0, browserOrdinal: 0,
  }, async () => {
    preparationStarted();
    await gate;
    return {
      record: { id: "0001", asyncDelayPumps: 1 },
      work: async () => "unexpected-work",
    };
  }).then(() => "unexpected-delivery", error => error.message);
  const drive = scheduler.driveToIdle().then(
    () => "unexpected-drive", error => error.message,
  );
  await started;
  scheduler.failTerminal(new Error("external stop"), "external-stop");
  await Promise.resolve();
  const during = scheduler.summary();
  releasePreparation();
  const [deliveryError, driveError] = await Promise.all([delivery, drive]);
  const idleError = await scheduler.whenIdle().then(
    () => "unexpected-idle", error => error.message,
  );
  return { during, deliveryError, driveError, idleError, summary: scheduler.summary() };
};
const timedOutPreparation = async () => {
  const scheduler = capture.createResponseScheduler({ prepareTimeoutMs: 10 });
  const delivery = scheduler.enqueueAdmission({
    routeKey: "GET\0https://prepare.test/timeout", resourceType: "fetch",
    occurrence: 0, browserOrdinal: 0,
  }, async () => new Promise(() => {})).then(
    () => "unexpected-delivery", error => error.message,
  );
  const drive = scheduler.driveToIdle().then(
    () => "unexpected-drive", error => error.message,
  );
  const [deliveryError, driveError] = await Promise.all([delivery, drive]);
  return { deliveryError, driveError, summary: scheduler.summary() };
};
Promise.all([externalFailure(), timedOutPreparation()]).then(([external, timeout]) => {
  process.stdout.write(JSON.stringify({ external, timeout }));
}, error => { process.stderr.write(String(error)); process.exitCode = 1; });
"""
        completed = subprocess.run(
            (NODE, "-e", script, str(CAPTURE)),
            check=False,
            capture_output=True,
            text=True,
            timeout=3,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        state = json.loads(completed.stdout)
        external = state["external"]
        self.assertEqual(external["during"]["pending"], 1)
        self.assertEqual(external["during"]["completed"], 1)
        for field in ("deliveryError", "driveError", "idleError"):
            self.assertIn("external stop", external[field])
        self.assertEqual(external["summary"]["pending"], 0)
        self.assertEqual(external["summary"]["terminal_failures"], 1)
        self.assertFalse(external["summary"]["ready"])
        timeout = state["timeout"]
        self.assertIn("admission preparation timed out", timeout["deliveryError"])
        self.assertIn("admission preparation timed out", timeout["driveError"])
        self.assertEqual(timeout["summary"]["pending"], 0)
        self.assertEqual(timeout["summary"]["terminal_failures"], 1)
        self.assertFalse(timeout["summary"]["ready"])

    def test_response_scheduler_configurable_limits_have_hard_ceilings(self) -> None:
        script = r"""
const capture = require(process.argv[1]);
const cases = {
  limit: { limit: 16385 },
  maxAdmissionProbes: { maxAdmissionProbes: 65 },
  maxDriveSteps: { maxDriveSteps: 32769 },
  maxPumps: { maxPumps: 16384000001 },
  maxWorkUnits: { maxWorkUnits: 16384000001 },
  probeTimeoutMs: { probeTimeoutMs: 120001 },
  prepareTimeoutMs: { prepareTimeoutMs: 120001 },
};
const failures = {};
for (const [name, options] of Object.entries(cases)) {
  try {
    capture.createResponseScheduler(options);
    failures[name] = "unexpected-success";
  } catch (error) {
    failures[name] = error.message;
  }
}
const boundary = capture.createResponseScheduler({
  limit: 16384, maxAdmissionProbes: 64, maxDriveSteps: 32768,
  maxPumps: 16384000000, maxWorkUnits: 16384000000,
  probeTimeoutMs: 120000, prepareTimeoutMs: 120000,
}).summary();
process.stdout.write(JSON.stringify({ failures, boundary }));
"""
        completed = subprocess.run(
            (NODE, "-e", script, str(CAPTURE)),
            check=False,
            capture_output=True,
            text=True,
            timeout=3,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        state = json.loads(completed.stdout)
        self.assertEqual(set(state["failures"]), {
            "limit", "maxAdmissionProbes", "maxDriveSteps", "maxPumps",
            "maxWorkUnits", "probeTimeoutMs", "prepareTimeoutMs",
        })
        for name, message in state["failures"].items():
            self.assertIn(f"{name} is invalid", message)
        self.assertTrue(state["boundary"]["ready"])
        self.assertEqual(state["boundary"]["request_limit"], 16384)
        self.assertEqual(state["boundary"]["semantic_pump_limit"], 16384000000)
        self.assertEqual(state["boundary"]["pump_work_limit"], 16384000000)
        self.assertEqual(state["boundary"]["drive_step_limit"], 32768)

    def test_response_scheduler_failure_is_terminal_but_never_ready(self) -> None:
        script = r"""
const capture = require(process.argv[1]);
const scheduler = capture.createResponseScheduler();
const failed = scheduler.enqueue(
  { id: "0001", asyncDelayPumps: 1 },
  { routeKey: "GET\\0https://fail.test/", resourceType: "fetch",
    occurrence: 0, browserOrdinal: 0 },
  async () => { throw new Error("delivery failed"); },
).catch(error => error.message);
const drive = scheduler.driveToIdle().then(
  () => "unexpected-drive-success", error => error.message,
);
Promise.all([failed, drive]).then(([message, driveError]) => {
  process.stdout.write(JSON.stringify({ message, driveError, summary: scheduler.summary() }));
}, error => { process.stderr.write(String(error)); process.exitCode = 1; });
"""
        completed = subprocess.run(
            (NODE, "-e", script, str(CAPTURE)),
            check=False, capture_output=True, text=True, timeout=3,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        state = json.loads(completed.stdout)
        self.assertIn("delivery failed", state["message"])
        self.assertIn("delivery failed", state["driveError"])
        self.assertEqual(state["driveError"], state["message"])
        self.assertEqual(state["summary"]["failures"], 1)
        self.assertEqual(state["summary"]["terminal_failures"], 1)
        self.assertFalse(state["summary"]["ready"])
        self.assertEqual(state["summary"]["pending"], 0)

    def test_bounded_host_failures_are_terminal_and_orphans_cannot_mutate_ledger(self) -> None:
        script = r"""
const capture = require(process.argv[1]);
(async () => {
  let resolveDelivery;
  let ledgerMutations = 0;
  let rejectedOperationRan = false;
  const delivery = capture.createHostOperationTracker({ timeoutMs: 10 });
  const rawDelivery = new Promise(resolve => { resolveDelivery = resolve; });
  const deliveryResult = delivery.run("delivery", () => rawDelivery)
    .then(() => { ledgerMutations += 1; return "unexpected-success"; }, error => error.message);
  const deliveryError = await deliveryResult;
  const timedOut = delivery.summary();
  const rejectedAfterTerminal = await delivery.run("late-handler", async () => {
    rejectedOperationRan = true;
  }).then(() => "unexpected-success", error => error.message);
  resolveDelivery("late-success");
  await new Promise(resolve => setImmediate(resolve));
  const afterLateSettlement = delivery.close();

  const rejectionCases = {};
  for (const label of ["route-handler", "context-close"]) {
    const tracker = capture.createHostOperationTracker({ timeoutMs: 100 });
    const first = await tracker.run(label, async () => {
      throw new Error(`${label} rejected`);
    }).then(() => "unexpected-success", error => error.message);
    let afterTerminalRan = false;
    const second = await tracker.run("after-terminal", async () => {
      afterTerminalRan = true;
    }).then(() => "unexpected-success", error => error.message);
    rejectionCases[label] = {
      first, second, afterTerminalRan, summary: tracker.close(),
    };
  }
  const handlerTracker = capture.createHostOperationTracker({ timeoutMs: 100 });
  const handlerScheduler = capture.createResponseScheduler();
  const handlerLifecycle = { handlers: new Set([new Promise(() => {})]) };
  const handlerDrainError = await capture.boundedDrainRouteHandlers(
    handlerTracker, handlerScheduler, handlerLifecycle,
  ).then(() => "unexpected-success", error => error.message);
  process.stdout.write(JSON.stringify({
    deliveryError, rejectedAfterTerminal, rejectedOperationRan, ledgerMutations,
    timedOut, afterLateSettlement, rejectionCases, handlerDrainError,
    handlerDrain: handlerTracker.close(),
  }));
})().catch(error => { process.stderr.write(String(error)); process.exitCode = 1; });
"""
        completed = subprocess.run(
            (NODE, "-e", script, str(CAPTURE)),
            check=False, capture_output=True, text=True, timeout=3,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        state = json.loads(completed.stdout)
        self.assertIn("delivery exceeded 10ms deadline", state["deliveryError"])
        self.assertEqual(state["rejectedAfterTerminal"], state["deliveryError"])
        self.assertFalse(state["rejectedOperationRan"])
        self.assertEqual(state["ledgerMutations"], 0)
        self.assertEqual(state["timedOut"]["timed_out"], 1)
        self.assertEqual(state["timedOut"]["orphaned"], 1)
        self.assertEqual(state["timedOut"]["orphan_pending"], 1)
        self.assertEqual(state["timedOut"]["terminal_failures"], 1)
        self.assertEqual(state["timedOut"]["rejected_after_terminal"], 0)
        self.assertFalse(state["timedOut"]["ready"])
        self.assertEqual(state["afterLateSettlement"]["orphan_pending"], 0)
        self.assertEqual(state["afterLateSettlement"]["late_completions"], 1)
        self.assertEqual(state["afterLateSettlement"]["rejected_after_terminal"], 1)
        self.assertTrue(state["afterLateSettlement"]["closed"])
        self.assertFalse(state["afterLateSettlement"]["ready"])
        for label in ("route-handler", "context-close"):
            case = state["rejectionCases"][label]
            self.assertIn(f"{label} rejected", case["first"])
            self.assertEqual(case["second"], case["first"])
            self.assertFalse(case["afterTerminalRan"])
            self.assertEqual(case["summary"]["terminal_failures"], 1)
            self.assertEqual(case["summary"]["rejected_after_terminal"], 1)
            self.assertFalse(case["summary"]["ready"])
        self.assertIn("route handlers did not reach a bounded drain", state["handlerDrainError"])
        self.assertEqual(state["handlerDrain"]["terminal_label"], "route-handler-drain")
        self.assertEqual(state["handlerDrain"]["terminal_failures"], 1)
        self.assertFalse(state["handlerDrain"]["ready"])

    def test_browser_clock_frame_and_evidence_promises_have_terminal_deadlines(self) -> None:
        script = r"""
const capture = require(process.argv[1]);
(async () => {
  const timeoutCase = async (callback) => {
    const tracker = capture.createHostOperationTracker({ timeoutMs: 10 });
    const error = await callback(tracker).then(
      () => "unexpected-success", failure => failure.message,
    );
    return { error, summary: tracker.close() };
  };
  const browser = await timeoutCase(tracker => capture.launchBrowser(
    { launch: () => new Promise(() => {}) }, {}, tracker,
  ));
  const frame = await timeoutCase(tracker => capture.runScenarioClock({
    pages: () => [{ frames: () => [{ evaluate: () => new Promise(() => {}) }] }],
    clock: { runFor: async () => {} },
  }, { pumpOnce: async () => {} }, 1, 16, tracker));
  const clock = await timeoutCase(tracker => capture.runScenarioClock({
    pages: () => [{ frames: () => [{ evaluate: async () => {} }] }],
    clock: { runFor: () => new Promise(() => {}) },
  }, { pumpOnce: async () => {} }, 1, 16, tracker));
  const evidence = await timeoutCase(tracker => capture.evaluateCaptureEvidence(
    { evaluate: () => new Promise(() => {}) },
    { requiredMarker: "ready", fallbackMarkers: [], interstitialMarkers: [] },
    tracker,
  ));
  process.stdout.write(JSON.stringify({ browser, frame, clock, evidence }));
})().catch(error => { process.stderr.write(String(error)); process.exitCode = 1; });
"""
        completed = subprocess.run(
            (NODE, "-e", script, str(CAPTURE)),
            check=False, capture_output=True, text=True, timeout=3,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        state = json.loads(completed.stdout)
        expected_labels = {
            "browser": "browser-launch",
            "frame": "replay-frame-clock-advance",
            "clock": "playwright-clock-advance",
            "evidence": "capture-evidence-evaluate",
        }
        for name, label in expected_labels.items():
            case = state[name]
            self.assertIn(f"{label} exceeded 10ms deadline", case["error"])
            self.assertEqual(case["summary"]["terminal_label"], label)
            self.assertEqual(case["summary"]["timed_out"], 1)
            self.assertEqual(case["summary"]["orphaned"], 1)
            self.assertFalse(case["summary"]["ready"])

    def test_reference_normalizer_subprocess_timeout_is_bounded_and_cleans_raw(self) -> None:
        script = r"""
const capture = require(process.argv[1]);
const fs = require("node:fs");
const raw = process.argv[2];
const output = process.argv[3];
const executable = process.argv[4];
const tracker = capture.createHostOperationTracker({ timeoutMs: 100 });
tracker.run("normalizer", () => capture.normalizePng(raw, output, {
  deviceWidth: 480, deviceHeight: 272,
}, executable, 20)).then(
  () => "unexpected-success", failure => failure.message,
).then(error => process.stdout.write(JSON.stringify({
  error, rawExists: fs.existsSync(raw), outputExists: fs.existsSync(output),
  summary: tracker.close(),
})));
"""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            raw = root / "raw.png"
            output = root / "output.png"
            executable = root / "slow-normalizer"
            raw.write_bytes(b"raw")
            executable.write_text("#!/bin/sh\nsleep 10\n")
            executable.chmod(0o700)
            completed = subprocess.run(
                (NODE, "-e", script, str(CAPTURE), str(raw), str(output),
                 str(executable)),
                check=False, capture_output=True, text=True, timeout=3,
            )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        state = json.loads(completed.stdout)
        self.assertIn(
            "reference normalization exceeded 20ms deadline and was terminated",
            state["error"],
        )
        self.assertFalse(state["rawExists"])
        self.assertFalse(state["outputExists"])
        self.assertEqual(state["summary"]["terminal_label"], "normalizer")
        self.assertEqual(state["summary"]["rejected"], 1)
        self.assertEqual(state["summary"]["timed_out"], 0)
        self.assertFalse(state["summary"]["ready"])

    def test_response_scheduler_probe_rejection_and_timeout_fail_pending(self) -> None:
        script = r"""
const capture = require(process.argv[1]);
const mode = process.argv[2];
const scheduler = capture.createResponseScheduler({
  probeTimeoutMs: 15,
  probe: mode === "reject"
    ? async () => { throw new Error("probe rejected"); }
    : async () => new Promise(() => {}),
});
const delivery = scheduler.enqueue(
  { id: "0001", asyncDelayPumps: 1 },
  { routeKey: "GET\\0https://probe.test/", resourceType: "document",
    occurrence: 0, browserOrdinal: 0 },
  async () => "served",
).then(() => "unexpected-delivery", error => error.message);
const drive = scheduler.driveToIdle().then(
  () => "unexpected-drive", error => error.message,
);
Promise.all([delivery, drive]).then(([deliveryError, driveError]) => {
  process.stdout.write(JSON.stringify({
    deliveryError, driveError, summary: scheduler.summary(),
  }));
}, error => { process.stderr.write(String(error)); process.exitCode = 1; });
"""
        for mode, marker in (
            ("reject", "probe rejected"),
            ("timeout", "quiescence probe timed out"),
        ):
            with self.subTest(mode=mode):
                completed = subprocess.run(
                    (NODE, "-e", script, str(CAPTURE), mode),
                    check=False, capture_output=True, text=True, timeout=3,
                )
                self.assertEqual(completed.returncode, 0, completed.stderr)
                state = json.loads(completed.stdout)
                self.assertIn(marker, state["deliveryError"])
                self.assertIn(marker, state["driveError"])
                self.assertEqual(state["summary"]["failures"], 1)
                self.assertEqual(state["summary"]["pending"], 0)
                self.assertTrue(state["summary"]["overflow"])
                self.assertFalse(state["summary"]["ready"])

    def test_response_scheduler_activity_failure_fails_pending(self) -> None:
        script = r"""
const capture = require(process.argv[1]);
const mode = process.argv[2];
let activityCalls = 0;
const scheduler = capture.createResponseScheduler({
  activity: () => {
    activityCalls += 1;
    if (mode === "initial" || activityCalls > 1) {
      throw new Error("activity failed");
    }
    return 0;
  },
});
const delivery = scheduler.enqueue(
  { id: "0001", asyncDelayPumps: 1 },
  { routeKey: "GET\\0https://activity.test/", resourceType: "document",
    occurrence: 0, browserOrdinal: 0 },
  async () => "served",
).then(() => "unexpected-delivery", error => error.message);
const drive = scheduler.driveToIdle().then(
  () => "unexpected-drive", error => error.message,
);
Promise.all([delivery, drive]).then(([deliveryError, driveError]) => {
  process.stdout.write(JSON.stringify({
    deliveryError, driveError, summary: scheduler.summary(),
  }));
}, error => { process.stderr.write(String(error)); process.exitCode = 1; });
"""
        for mode in ("initial", "subsequent"):
            with self.subTest(mode=mode):
                completed = subprocess.run(
                    (NODE, "-e", script, str(CAPTURE), mode),
                    check=False, capture_output=True, text=True, timeout=3,
                )
                self.assertEqual(completed.returncode, 0, completed.stderr)
                state = json.loads(completed.stdout)
                self.assertIn("activity failed", state["deliveryError"])
                self.assertIn("activity failed", state["driveError"])
                self.assertEqual(state["summary"]["failures"], 1)
                self.assertEqual(state["summary"]["pending"], 0)
                self.assertTrue(state["summary"]["overflow"])
                self.assertFalse(state["summary"]["ready"])

    def test_response_scheduler_bootstrap_driver_is_bounded_and_non_deadlocking(self) -> None:
        script = r"""
const capture = require(process.argv[1]);
let resolveNavigation;
const navigation = new Promise(resolve => { resolveNavigation = resolve; });
const scheduler = capture.createResponseScheduler();
const delivered = scheduler.enqueue(
  { id: "0001", asyncDelayPumps: 1000000 },
  { routeKey: "GET\\0https://nav.test/", resourceType: "document",
    occurrence: 0, browserOrdinal: 0 },
  async () => { resolveNavigation("loaded"); return "served"; },
);
Promise.all([
  scheduler.driveUntilSettled(navigation, "navigation"), delivered,
]).then(([value]) => {
  process.stdout.write(JSON.stringify({ value, summary: scheduler.summary() }));
}, error => { process.stderr.write(String(error)); process.exitCode = 1; });
"""
        completed = subprocess.run(
            (NODE, "-e", script, str(CAPTURE)),
            check=False, capture_output=True, text=True, timeout=3,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        state = json.loads(completed.stdout)
        self.assertEqual(state["value"], "loaded")
        self.assertEqual(state["summary"]["semantic_pumps"], 1_000_000)
        self.assertEqual(state["summary"]["pump_work_units"], 1_000_000)
        self.assertEqual(state["summary"]["fast_forwarded_pumps"], 999_999)
        self.assertTrue(state["summary"]["ready"])

    def test_capability_policy_removes_realm_entrypoints_before_page_code(self) -> None:
        script = r"""
const capture = require(process.argv[1]);
const reports = [];
globalThis.__tilefinchBlockedCapability = (...items) => {
  reports.push(items); return Promise.resolve();
};
let constructed = 0;
class FakeWorker { constructor() { constructed += 1; } }
class FakeSharedWorker { constructor() { constructed += 1; } }
class FakeShadowRealm {}
class FakeAudioWorkletNode { constructor() { constructed += 1; } }
class FakeWorklet { addModule(url, options) { return Promise.resolve([url, options]); } }
const retainedWorklet = new FakeWorklet();
class FakeBaseAudioContext {}
Object.defineProperty(FakeBaseAudioContext.prototype, "audioWorklet", {
  configurable: true, enumerable: true, get() { return retainedWorklet; },
});
class FakeNavigator {}
Object.defineProperty(FakeNavigator.prototype, "serviceWorker", {
  configurable: true, enumerable: true, get() { return { register() {} }; },
});
Object.defineProperty(globalThis, "navigator", {
  configurable: true, writable: true, value: new FakeNavigator(),
});
class FakeSubtleCrypto {
  generateKey() { return Promise.resolve("key"); }
  encrypt() { return Promise.resolve("encrypted"); }
  sign() { return Promise.resolve("signed"); }
  wrapKey() { return Promise.resolve("wrapped"); }
}
Object.assign(globalThis, {
  Worker: FakeWorker, SharedWorker: FakeSharedWorker, ShadowRealm: FakeShadowRealm,
  AudioWorkletNode: FakeAudioWorkletNode, Worklet: FakeWorklet,
  BaseAudioContext: FakeBaseAudioContext, Navigator: FakeNavigator,
  CSS: { paintWorklet: retainedWorklet },
  sharedStorage: { worklet: retainedWorklet },
  SharedStorage: class SharedStorage {},
  SharedStorageWorklet: class SharedStorageWorklet {},
  ServiceWorker: class ServiceWorker {},
  ServiceWorkerContainer: class ServiceWorkerContainer {},
  ServiceWorkerRegistration: class ServiceWorkerRegistration {},
  SubtleCrypto: FakeSubtleCrypto,
});
const originals = {
  addModule: FakeWorklet.prototype.addModule,
  addDescriptor: Object.getOwnPropertyDescriptor(FakeWorklet.prototype, "addModule"),
};
capture.installOfflineCapabilityPolicy({
  version: "offline-capabilities-v2",
  surfaceVersion: "realm-entrypoints-unavailable-v1",
});
const failures = [];
(async () => {
  try { await retainedWorklet.addModule("/paint.js"); }
  catch (error) { failures.push(["retained-worklet", error.name]); }
  const subtle = new SubtleCrypto();
  for (const [name, invoke] of [
    ["generateKey", () => subtle.generateKey({ name: "AES-GCM" })],
    ["encrypt", () => subtle.encrypt({ name: "AES-GCM" }, {}, new Uint8Array())],
    ["sign", () => subtle.sign("HMAC", {}, new Uint8Array())],
    ["wrapKey", () => subtle.wrapKey("raw", {}, {}, { name: "AES-KW" })],
  ]) {
    try { await invoke(); } catch (error) { failures.push([name, error.name]); }
  }
  const flags = descriptor => [descriptor.configurable, descriptor.enumerable, descriptor.writable];
  process.stdout.write(JSON.stringify({
    failures, reports, marker: globalThis.__tilefinchOfflineCapabilityPolicy,
    evidence: globalThis.__tilefinchOfflineCapabilityEvidence,
    evidenceReady: capture.offlineCapabilityEvidenceReady(
      globalThis.__tilefinchOfflineCapabilityEvidence),
    constructed,
    shape: {
      constructors: ["Worker", "SharedWorker", "ShadowRealm", "AudioWorkletNode", "Worklet"]
        .map(name => [name, name in globalThis, typeof globalThis[name]]),
      serviceWorker: ["serviceWorker" in navigator, typeof navigator.serviceWorker],
      serviceWorkerInterfaces: [
        "ServiceWorker", "ServiceWorkerContainer", "ServiceWorkerRegistration",
      ].map(name => [name, name in globalThis]),
      cssWorklet: ["paintWorklet" in CSS, typeof CSS.paintWorklet],
      audioWorklet: ["audioWorklet" in BaseAudioContext.prototype,
        typeof (new BaseAudioContext()).audioWorklet],
      sharedStorage: ["sharedStorage" in globalThis, typeof globalThis.sharedStorage,
        "SharedStorage" in globalThis, "SharedStorageWorklet" in globalThis],
      add: [FakeWorklet.prototype.addModule.name, FakeWorklet.prototype.addModule.length,
        Function.prototype.toString.call(FakeWorklet.prototype.addModule),
        flags(Object.getOwnPropertyDescriptor(FakeWorklet.prototype, "addModule"))],
      originalAdd: [originals.addModule.name, originals.addModule.length,
        flags(originals.addDescriptor)],
    },
  }));
})().catch(error => { process.stderr.write(String(error)); process.exitCode = 1; });
"""
        completed = subprocess.run(
            (NODE, "-e", script, str(CAPTURE)),
            check=False, capture_output=True, text=True,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        state = json.loads(completed.stdout)
        self.assertEqual(state["marker"], "offline-capabilities-v2")
        self.assertTrue(state["evidenceReady"])
        self.assertEqual(state["evidence"]["shared_worker_constructor"], "unavailable")
        self.assertEqual(state["constructed"], 0)
        self.assertEqual(len(state["failures"]), 5)
        self.assertEqual(len(state["reports"]), 5)
        self.assertEqual(
            state["shape"]["constructors"],
            [[name, False, "undefined"] for name in (
                "Worker", "SharedWorker", "ShadowRealm", "AudioWorkletNode", "Worklet"
            )],
        )
        self.assertEqual(state["shape"]["serviceWorker"], [False, "undefined"])
        self.assertTrue(all(not item[1] for item in state["shape"]["serviceWorkerInterfaces"]))
        self.assertEqual(state["shape"]["cssWorklet"], [False, "undefined"])
        self.assertEqual(state["shape"]["audioWorklet"], [False, "undefined"])
        self.assertEqual(
            state["shape"]["sharedStorage"],
            [False, "undefined", False, False],
        )
        self.assertEqual(state["shape"]["add"][:2], state["shape"]["originalAdd"][:2])
        self.assertEqual(state["shape"]["add"][3], state["shape"]["originalAdd"][2])
        self.assertIn("[native code]", state["shape"]["add"][2])

    def test_route_signature_covers_headers_delays_and_ignores_failed_statuses(self) -> None:
        script = r"""
const capture = require(process.argv[1]);
const trace = capture.loadTrace(process.argv[2]);
const route = [...trace.routes.values()][0];
process.stdout.write(JSON.stringify({
  ambiguous: route.ambiguous,
  selected: route.selected && route.selected.id,
  candidates: route.candidates.map(record => record.id),
}));
"""
        for label, varying, expected_ambiguous in (
            ("header", "response-header-0=x-test: two\n", True),
            ("delay", "response-header-0=x-test: one\nasync-delay-pumps=2\n", True),
        ):
            with self.subTest(label=label), tempfile.TemporaryDirectory() as temporary:
                trace = Path(temporary)
                write_trace_meta(trace, count=2)
                base = (
                    "method=GET\nurl=https://signature.test/\nsuccess=1\nstatus=200\n"
                    "length=0\ncontent-type=text/plain\nresponse-header-count=1\n"
                    "response-header-0=x-test: one\nset-cookie-count=0\n"
                )
                (trace / "0000.meta").write_text(base)
                (trace / "0001.meta").write_text(
                    base.replace("response-header-0=x-test: one\n", varying)
                )
                completed = subprocess.run(
                    (NODE, "-e", script, str(CAPTURE), str(trace)),
                    check=False, capture_output=True, text=True,
                )
                self.assertEqual(completed.returncode, 0, completed.stderr)
                self.assertEqual(json.loads(completed.stdout)["ambiguous"], expected_ambiguous)

        with tempfile.TemporaryDirectory() as temporary:
            trace = Path(temporary)
            write_trace_meta(trace, count=2)
            (trace / "0000.meta").write_text(
                "method=GET\nurl=https://signature.test/\nsuccess=0\nstatus=503\n"
                "length=0\ncontent-type=text/plain\nresponse-header-count=0\nset-cookie-count=0\n"
            )
            (trace / "0001.meta").write_text(
                "method=GET\nurl=https://signature.test/\nsuccess=1\nstatus=200\n"
                "length=0\ncontent-type=text/plain\nresponse-header-count=0\nset-cookie-count=0\n"
            )
            completed = subprocess.run(
                (NODE, "-e", script, str(CAPTURE), str(trace)),
                check=False, capture_output=True, text=True,
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)
            state = json.loads(completed.stdout)
            self.assertEqual(state["selected"], "0001")
            self.assertEqual(state["candidates"], ["0001"])

    def test_ranked_occurrence_routes_replay_a_a_b_then_fail_closed(self) -> None:
        script = r"""
const capture = require(process.argv[1]);
const record = (id, signature, success = true, status = 200) =>
  ({ id, signature, success, status });
const route = capture.selectRoute([
  record("0002", "b"), record("0000", "a"), record("0001", "a"),
  record("0003", "failure", false, 503),
]);
const occurrences = new Map();
const claims = [0, 1, 2, 3].map(() =>
  capture.claimRouteRecord(route, occurrences, "GET\0https://fixture.test/")
);
const reusable = capture.selectRoute([record("0009", "only")]);
const reusableOccurrences = new Map();
const failures = capture.selectRoute([
  record("0007", "zero", false, 0),
  record("0006", "http-b", false, 503),
  record("0005", "http-a", false, 503),
]);
const failureOccurrences = new Map();
const failureClaims = [0, 1, 2].map(() =>
  capture.claimRouteRecord(failures, failureOccurrences, "failure-key")
);
const zeroFailure = capture.selectRoute([record("0008", "zero-only", false, 0)]);
const zeroOccurrences = new Map();
process.stdout.write(JSON.stringify({
  version: route.version, mode: route.mode, rank: route.rank,
  candidates: route.candidates.map(item => item.id),
  claims: claims.map(claim => ({ id: claim.record && claim.record.id,
    exhausted: claim.exhausted, occurrence: claim.occurrence })),
  reusable: [
    capture.claimRouteRecord(reusable, reusableOccurrences, "key").record.id,
    capture.claimRouteRecord(reusable, reusableOccurrences, "key").record.id,
  ],
  failures: {
    rank: failures.rank, mode: failures.mode,
    candidates: failures.candidates.map(item => item.id),
    claims: failureClaims.map(claim => ({
      id: claim.record && claim.record.id, exhausted: claim.exhausted,
    })),
  },
  zero: [
    capture.claimRouteRecord(zeroFailure, zeroOccurrences, "zero").record.id,
    capture.claimRouteRecord(zeroFailure, zeroOccurrences, "zero").record.id,
  ],
  abortCodes: [
    capture.retainedFailureAbortCode({ success: false, externalCancel: true }),
    capture.retainedFailureAbortCode({ success: false, transportTimeout: true }),
    capture.retainedFailureAbortCode({ success: false, status: 503 }),
    capture.retainedFailureAbortCode({ success: false, status: 0 }),
  ],
}));
"""
        completed = subprocess.run(
            (NODE, "-e", script, str(CAPTURE)),
            check=False, capture_output=True, text=True,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        state = json.loads(completed.stdout)
        self.assertEqual(state["version"], "ranked-occurrence-v2")
        self.assertEqual(state["mode"], "occurrence-sequence")
        self.assertEqual(state["rank"], 2)
        self.assertEqual(state["candidates"], ["0000", "0001", "0002"])
        self.assertEqual([claim["id"] for claim in state["claims"][:3]], ["0000", "0001", "0002"])
        self.assertTrue(state["claims"][3]["exhausted"])
        self.assertIsNone(state["claims"][3]["id"])
        self.assertEqual(state["reusable"], ["0009", "0009"])
        self.assertEqual(state["failures"]["rank"], 1)
        self.assertEqual(state["failures"]["mode"], "occurrence-sequence")
        self.assertEqual(state["failures"]["candidates"], ["0005", "0006"])
        self.assertEqual(
            [claim["id"] for claim in state["failures"]["claims"]],
            ["0005", "0006", None],
        )
        self.assertTrue(state["failures"]["claims"][2]["exhausted"])
        self.assertEqual(state["zero"], ["0008", "0008"])
        self.assertEqual(
            state["abortCodes"],
            ["aborted", "timedout", "blockedbyresponse", "failed"],
        )

    def test_retained_failure_delivery_aborts_without_cookies_or_body(self) -> None:
        script = r"""
const capture = require(process.argv[1]);
(async () => {
  const events = [];
  const operations = {
    abort: async code => { events.push(`abort:${code}`); },
    applyCookies: async () => { events.push("cookies"); },
    fulfill: async () => { events.push("fulfill"); },
  };
  const rejected = await capture.deliverReplayRecord({
    success: false, status: 0, externalCancel: false, transportTimeout: true,
    body: Buffer.from("must-not-be-exposed"), cookies: ["must-not-apply"],
  }, operations);
  const served = await capture.deliverReplayRecord({
    success: true, status: 200, externalCancel: false, transportTimeout: false,
  }, operations);
  let invalid = "";
  try {
    await capture.deliverReplayRecord({ success: false }, {
      abort: async () => {}, applyCookies: async () => {},
    });
  } catch (error) { invalid = error.message; }
  process.stdout.write(JSON.stringify({ rejected, served, events, invalid }));
})().catch(error => { process.stderr.write(String(error)); process.exitCode = 1; });
"""
        completed = subprocess.run(
            (NODE, "-e", script, str(CAPTURE)),
            check=False, capture_output=True, text=True,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        state = json.loads(completed.stdout)
        self.assertEqual(state["rejected"], "rejected")
        self.assertEqual(state["served"], "served")
        self.assertEqual(
            state["events"], ["abort:timedout", "cookies", "fulfill"]
        )
        self.assertIn("operations are incomplete", state["invalid"])

    def test_teardown_evidence_reports_exact_bounded_field_deltas(self) -> None:
        script = r"""
const capture = require(process.argv[1]);
const before = {
  activity: 7, active: 0, requests: 3,
  scheduler: { completed: 3, pending: 0 },
};
const after = {
  activity: 8, active: 0, requests: 4,
  scheduler: { completed: 4, pending: 0 },
};
let bounded = "";
try {
  capture.teardownEvidenceChanges(
    { a: 0, b: 0 }, { a: 1, b: 1 }, "bounded", 1,
  );
} catch (error) { bounded = error.message; }
let invalidLimit = "";
try {
  capture.teardownEvidenceChanges(before, after, "invalid", 65);
} catch (error) { invalidLimit = error.message; }
process.stdout.write(JSON.stringify({
  changes: capture.teardownEvidenceChanges(
    before, after, "context-close",
  ),
  bounded,
  invalidLimit,
}));
"""
        completed = subprocess.run(
            (NODE, "-e", script, str(CAPTURE)),
            check=False, capture_output=True, text=True,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        state = json.loads(completed.stdout)
        self.assertEqual(
            state["changes"],
            [
                {
                    "phase": "context-close",
                    "field": "activity",
                    "before": 7,
                    "after": 8,
                },
                {
                    "phase": "context-close",
                    "field": "requests",
                    "before": 3,
                    "after": 4,
                },
                {
                    "phase": "context-close",
                    "field": "scheduler.completed",
                    "before": 3,
                    "after": 4,
                },
            ],
        )
        self.assertIn("exceeds its bound", state["bounded"])
        self.assertIn("limit is invalid", state["invalidLimit"])

    def test_teardown_evidence_rejects_late_callback_before_baseline(self) -> None:
        script = r"""
const capture = require(process.argv[1]);
const scheduler = { summary: () => ({ pending: 0 }) };
const ledger = {
  requests: 1, matched: 1, served: 1, rejected: 0,
  unmatched: 0, conflicts: 0, invalid: 0,
  occurrence_claims: 0, reusable_claims: 1, occurrence_exhausted: 0,
  active: 0,
};
const activity = { value: 2 };
const clean = {
  closingCallbacks: 0, closedCallbacks: 0, lateBindingCallbacks: 0,
  handlers: new Set(),
};
const late = { ...clean, closingCallbacks: 1 };
const cleanSnapshot = capture.teardownEvidenceSnapshot(
  ledger, scheduler, activity, clean,
);
const lateSnapshot = capture.teardownEvidenceSnapshot(
  ledger, scheduler, activity, late,
);
process.stdout.write(JSON.stringify({
  clean: capture.teardownEvidenceReady(cleanSnapshot),
  late: capture.teardownEvidenceReady(lateSnapshot),
  hiddenByDelta: capture.teardownEvidenceChanges(
    lateSnapshot, lateSnapshot, "context-close",
  ),
}));
"""
        completed = subprocess.run(
            (NODE, "-e", script, str(CAPTURE)),
            check=False, capture_output=True, text=True,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        state = json.loads(completed.stdout)
        self.assertTrue(state["clean"])
        self.assertFalse(state["late"])
        self.assertEqual(state["hiddenByDelta"], [])

    def test_trace_meta_requires_completion_clock_and_exact_count(self) -> None:
        record = (
            "method=GET\nurl=https://complete.test/\nsuccess=1\nstatus=200\n"
            "length=0\ncontent-type=text/plain\nresponse-header-count=0\nset-cookie-count=0\n"
        )
        cases = (
            ("not-complete", "psp-http-trace-clock=1\norigin-ms=1000\nrecord-count=1\n", "capture-complete"),
            ("bad-clock", "capture-complete=yes\norigin-ms=1000\nrecord-count=1\n", "invalid replay clock"),
            ("wrong-count", "capture-complete=yes\npsp-http-trace-clock=1\norigin-ms=1000\nrecord-count=2\n", "record-count"),
            ("date-overflow", "capture-complete=yes\npsp-http-trace-clock=1\norigin-ms=8640000000000001\nrecord-count=1\n", "invalid replay clock origin"),
        )
        for label, metadata, expected in cases:
            with self.subTest(label=label), tempfile.TemporaryDirectory() as temporary:
                trace = Path(temporary)
                (trace / "trace.meta").write_text(metadata)
                (trace / "0000.meta").write_text(record)
                completed = inspect(trace)
                self.assertEqual(completed.returncode, 2)
                self.assertIn(expected, completed.stderr)

    def test_determinism_child_has_a_derived_capped_end_to_end_deadline(self) -> None:
        comparator = ROOT / "benchmarks" / "check-reference-determinism.js"
        script = r"""
const check = require(process.argv[1]);
process.stdout.write(JSON.stringify({
  normal: check.captureChildDeadlineMs(["--scenario", "hacker-news"]),
  minimum: check.captureChildDeadlineMs([
    "--scenario", "hacker-news", "--timeout-ms", "1000", "--settle-ms", "0",
  ]),
  maximum: check.captureChildDeadlineMs([
    "--scenario", "hacker-news", "--timeout-ms", "120000",
  ]),
}));
"""
        completed = subprocess.run(
            (NODE, "-e", script, str(comparator)),
            check=False, capture_output=True, text=True,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        state = json.loads(completed.stdout)
        self.assertEqual(state["normal"], 429750)
        self.assertEqual(state["minimum"], 60000)
        self.assertEqual(state["maximum"], 15 * 60 * 1000)

    def test_determinism_child_timeout_terminates_a_hung_capture(self) -> None:
        comparator = ROOT / "benchmarks" / "check-reference-determinism.js"
        script = r"""
const check = require(process.argv[1]);
let message = "";
try {
  check.runCaptureChild(process.execPath, [
    "-e", "setInterval(() => {}, 1000)",
  ], 50);
} catch (error) { message = error.message; }
process.stdout.write(JSON.stringify({ message }));
"""
        completed = subprocess.run(
            (NODE, "-e", script, str(comparator)),
            check=False, capture_output=True, text=True, timeout=3,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        state = json.loads(completed.stdout)
        self.assertIn(
            "capture exceeded 50ms end-to-end deadline and was terminated",
            state["message"],
        )

    def test_two_run_comparator_requires_equal_state_and_frames(self) -> None:
        comparator = ROOT / "benchmarks" / "check-reference-determinism.js"
        script = (
            "const check=require(process.argv[1]);"
            "process.stdout.write(JSON.stringify(check.compareCaptureDirectories("
            "process.argv[2],process.argv[3])));"
        )
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            left = root / "left"
            right = root / "right"
            left.mkdir(); right.mkdir()
            state = {
                "schema": 2,
                "capture_ready": True,
                "failure": None,
                "eligibility_reasons": [],
                "scenario": "fixture",
                "trace_sha256": "1" * 64,
                "replay_environment": reference_environment_state(),
                "checkpoints": [{"frame": "top.png"}],
            }
            for directory in (left, right):
                (directory / "reference-state.json").write_text(
                    json.dumps(state, indent=2) + "\n"
                )
                (directory / "top.png").write_bytes(b"deterministic-png")
            completed = subprocess.run(
                (NODE, "-e", script, str(comparator), str(left), str(right)),
                check=False, capture_output=True, text=True,
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)
            proof = json.loads(completed.stdout)
            self.assertTrue(proof["equivalent"])
            self.assertEqual(proof["version"], "two-full-run-byte-equality-v3")
            self.assertEqual(proof["state_schema"], 2)
            self.assertEqual(proof["canonical_run"], "run-a")
            self.assertEqual(proof["comparison_run"], "run-b")
            self.assertEqual(proof["scenario"], "fixture")
            self.assertEqual(proof["trace_sha256"], "1" * 64)
            (right / "top.png").write_bytes(b"changed")
            changed = subprocess.run(
                (NODE, "-e", script, str(comparator), str(left), str(right)),
                check=False, capture_output=True, text=True,
            )
            self.assertNotEqual(changed.returncode, 0)
            self.assertIn("frame bytes differ", changed.stderr)

    def test_two_run_comparator_rejects_unsafe_artifacts_and_legacy_state(self) -> None:
        comparator = ROOT / "benchmarks" / "check-reference-determinism.js"
        script = (
            "const check=require(process.argv[1]);"
            "process.stdout.write(JSON.stringify(check.compareCaptureDirectories("
            "process.argv[2],process.argv[3])));"
        )
        state = {
            "schema": 2,
            "capture_ready": True,
            "failure": None,
            "eligibility_reasons": [],
            "scenario": "fixture",
            "trace_sha256": "1" * 64,
            "replay_environment": reference_environment_state(),
            "checkpoints": [{"frame": "top.png"}],
        }
        for mutation in (
            "symlink-state",
            "undeclared-frame",
            "legacy-v1",
            "diagnostic-ready",
            "state-not-ready",
            "ready-failure",
            "ready-reasons",
        ):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                left = root / "left"
                right = root / "right"
                left.mkdir(); right.mkdir()
                for directory in (left, right):
                    (directory / "reference-state.json").write_text(
                        json.dumps(state, indent=2) + "\n"
                    )
                    (directory / "top.png").write_bytes(b"deterministic-png")
                if mutation == "symlink-state":
                    target = root / "state.json"
                    target.write_bytes((left / "reference-state.json").read_bytes())
                    (left / "reference-state.json").unlink()
                    (left / "reference-state.json").symlink_to(target)
                else:
                    if mutation == "undeclared-frame":
                        for directory in (left, right):
                            (directory / "extra.png").write_bytes(b"extra")
                    elif mutation == "legacy-v1":
                        legacy = {
                            **state,
                            "schema": 1,
                            "replay_environment": {
                                "version": "deterministic-hermetic-v2",
                                "clock_version": "playwright-clock-paused-v1",
                                "clock_contract": "logical-ms-call-v1",
                            },
                        }
                        for directory in (left, right):
                            (directory / "reference-state.json").write_text(
                                json.dumps(legacy, indent=2) + "\n"
                            )
                    elif mutation == "diagnostic-ready":
                        for directory in (left, right):
                            (directory / "reference-state.json").rename(
                                directory / "reference-diagnostic.json"
                            )
                    else:
                        changed_state = json.loads(json.dumps(state))
                        if mutation == "state-not-ready":
                            changed_state["capture_ready"] = False
                        elif mutation == "ready-failure":
                            changed_state["failure"] = "navigation:timeout"
                        else:
                            changed_state["eligibility_reasons"] = [
                                "reference-title-mismatch"
                            ]
                        for directory in (left, right):
                            (directory / "reference-state.json").write_text(
                                json.dumps(changed_state, indent=2) + "\n"
                            )
                completed = subprocess.run(
                    (NODE, "-e", script, str(comparator), str(left), str(right)),
                    check=False,
                    capture_output=True,
                    text=True,
                )
                self.assertNotEqual(completed.returncode, 0)
                expected = {
                    "symlink-state": "regular non-symlink",
                    "undeclared-frame": "checkpoint frame set",
                    "legacy-v1": "schema is unsupported",
                    "diagnostic-ready": "filename does not match capture readiness",
                    "state-not-ready": "filename does not match capture readiness",
                    "ready-failure": "retains failure evidence",
                    "ready-reasons": "retains failure evidence",
                }[mutation]
                self.assertIn(expected, completed.stderr)

    def test_two_run_comparator_rejects_noncanonical_and_malformed_v2_state(self) -> None:
        comparator = ROOT / "benchmarks" / "check-reference-determinism.js"
        script = (
            "const check=require(process.argv[1]);"
            "process.stdout.write(JSON.stringify(check.compareCaptureDirectories("
            "process.argv[2],process.argv[3])));"
        )
        baseline = {
            "schema": 2,
            "capture_ready": True,
            "failure": None,
            "eligibility_reasons": [],
            "scenario": "fixture",
            "trace_sha256": "1" * 64,
            "replay_environment": reference_environment_state(),
            "checkpoints": [{"frame": "top.png"}],
        }
        cases: list[tuple[str, str, str]] = []
        compact = json.dumps(baseline, separators=(",", ":")) + "\n"
        cases.append(("compact", compact, "canonical duplicate-free JSON"))
        canonical = json.dumps(baseline, indent=2) + "\n"
        duplicate = canonical.replace(
            '  "scenario": "fixture",\n',
            '  "scenario": "fixture",\n  "scenario": "fixture",\n',
            1,
        )
        cases.append(("duplicate", duplicate, "canonical duplicate-free JSON"))
        bad_partition = json.loads(json.dumps(baseline))
        bad_partition["replay_environment"]["clock_sources"]["date_now"] = 1
        cases.append(
            (
                "source-partition",
                json.dumps(bad_partition, indent=2) + "\n",
                "clock closure is invalid",
            )
        )
        legacy_field = json.loads(json.dumps(baseline))
        legacy_field["replay_environment"]["logical_elapsed_ms"] = 0
        cases.append(
            (
                "legacy-field",
                json.dumps(legacy_field, indent=2) + "\n",
                "environment fields are unsupported",
            )
        )
        for label, state_text, expected in cases:
            with self.subTest(label=label), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                left = root / "left"
                right = root / "right"
                left.mkdir()
                right.mkdir()
                for directory in (left, right):
                    (directory / "reference-state.json").write_text(state_text)
                    (directory / "top.png").write_bytes(b"deterministic-png")
                completed = subprocess.run(
                    (NODE, "-e", script, str(comparator), str(left), str(right)),
                    check=False, capture_output=True, text=True,
                )
                self.assertNotEqual(completed.returncode, 0)
                self.assertIn(expected, completed.stderr)

    def test_determinism_child_result_is_bound_to_status_output_and_proof(self) -> None:
        comparator = ROOT / "benchmarks" / "check-reference-determinism.js"
        script = r"""
const check = require(process.argv[1]);
const root = process.argv[2];
const output = require("node:path").join(
  root, "fixture", "reference-state.json",
);
const result = { ready: true, output, replay_ledger: {} };
const evidence = check.validateCaptureResult(0, result, root);
const capture = { status: 0, result, ...evidence };
const proof = {
  capture_ready: true,
  scenario: "fixture",
  state: { name: "reference-state.json" },
};
const rejected = [];
const expectRejection = (name, callback) => {
  try { callback(); } catch (error) { rejected.push([name, error.message]); return; }
  process.exit(10);
};
expectRejection("status", () => check.validateCaptureResult(3, result, root));
expectRejection("kind", () => check.validateCaptureResult(3, {
  ready: false, output, replay_ledger: {},
}, root));
expectRejection("escape", () => check.validateCaptureResult(0, {
  ready: true,
  output: require("node:path").join(root, "..", "reference-state.json"),
  replay_ledger: {},
}, root));
expectRejection("proof-output", () => check.validateProofCaptureBinding(
  proof,
  { ...capture, result: { ...result, output: `${output}.other` } },
  capture,
));
expectRejection("proof-ready", () => check.validateProofCaptureBinding(
  { ...proof, capture_ready: false }, capture, capture,
));
process.stdout.write(JSON.stringify(rejected));
"""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            completed = subprocess.run(
                (NODE, "-e", script, str(comparator), str(root)),
                check=False,
                capture_output=True,
                text=True,
            )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        rejected = dict(json.loads(completed.stdout))
        self.assertIn("status disagrees", rejected["status"])
        self.assertIn("does not name its state artifact", rejected["kind"])
        self.assertIn("does not name its state artifact", rejected["escape"])
        self.assertIn("disagrees with parsed state proof", rejected["proof-output"])
        self.assertIn("disagrees with parsed state proof", rejected["proof-ready"])

    def test_settle_loop_has_a_hard_deadline(self) -> None:
        script = """
const capture = require(process.argv[1]);
let sample = 0;
const page = {
  waitForTimeout: (milliseconds) => new Promise(
    resolve => setTimeout(resolve, Math.min(milliseconds, 5))
  ),
  evaluate: async () => String(sample++),
};
capture.settlePage(page, 0).then(
  () => process.exit(1),
  error => {
    if (!String(error.message).includes("bounded visual stability")) {
      process.exit(2);
    }
  },
);
"""
        completed = subprocess.run(
            (NODE, "-e", script, str(CAPTURE)),
            check=False,
            capture_output=True,
            text=True,
            timeout=3,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)

    def test_unknown_options_do_not_fall_through_to_capture(self) -> None:
        completed = subprocess.run(
            (NODE, str(CAPTURE), "--unknown"),
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertEqual(completed.returncode, 2)
        self.assertIn("unknown option", completed.stderr)


class ParallelSuite(unittest.TestSuite):
    """Runs this file's tests concurrently.

    Every test here is a fresh temporary directory, one or more Node
    processes, and assertions on what they printed.  Nothing is shared: no
    environment, no working directory, no port, no fixture written outside
    the test's own directory.  What the suite does share is a cost it does
    not control - it starts about a hundred Node processes, and process
    startup is set by machine load, not by this test's work.  Idle that is
    5.5 s of wall clock for roughly 0.3 s of actual work; under a concurrent
    full-suite run it reaches 10.3 s, which is the CTest deadline.

    Dispatching the independent tests across a bounded pool makes the wall
    clock a function of the work again without touching a single deadline,
    cap, or assertion.  It adds no CPU: the same processes run, they just
    stop waiting for each other.
    """

    WORKERS = min(8, (os.cpu_count() or 4))

    def _flatten(self) -> list[unittest.TestCase]:
        tests: list[unittest.TestCase] = []
        for entry in self:
            if isinstance(entry, unittest.TestSuite):
                tests.extend(
                    ParallelSuite(entry)._flatten()
                    if not isinstance(entry, ParallelSuite)
                    else entry._flatten()
                )
            else:
                tests.append(entry)
        return tests

    def run(self, result, debug=False):  # type: ignore[override]
        tests = self._flatten()
        if len(tests) < 2 or debug:
            return super().run(result, debug)
        with concurrent.futures.ThreadPoolExecutor(
            max_workers=self.WORKERS
        ) as pool:
            outcomes = list(pool.map(self._run_one, tests))
        for test, outcome in zip(tests, outcomes):
            result.startTest(test)
            result.testsRun += outcome.testsRun - 1
            result.failures.extend(outcome.failures)
            result.errors.extend(outcome.errors)
            result.skipped.extend(outcome.skipped)
            result.expectedFailures.extend(outcome.expectedFailures)
            result.unexpectedSuccesses.extend(outcome.unexpectedSuccesses)
            if outcome.wasSuccessful() and not outcome.skipped:
                result.addSuccess(test)
            result.stopTest(test)
        return result

    @staticmethod
    def _run_one(test: unittest.TestCase) -> unittest.TestResult:
        outcome = unittest.TestResult()
        test.run(outcome)
        return outcome


if __name__ == "__main__":
    # Explicit arguments (a single test name, -v, -k) keep stock serial
    # behaviour so a failure is straightforward to re-run in isolation.
    loader = unittest.TestLoader()
    if len(sys.argv) == 1:
        loader.suiteClass = ParallelSuite
    unittest.main(testLoader=loader)
