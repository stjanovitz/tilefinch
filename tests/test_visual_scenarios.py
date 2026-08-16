#!/usr/bin/env python3

from __future__ import annotations

import copy
import hashlib
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from dataclasses import replace
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BENCHMARKS = ROOT / "benchmarks"
sys.path.insert(0, str(BENCHMARKS))

from reference_frame import write_png_rgb  # noqa: E402
from visual_scenario import (  # noqa: E402
    ArtifactError,
    CANDIDATE_EVIDENCE_FIELDS,
    CANDIDATE_EVIDENCE_VERSION,
    CANDIDATE_LOG_BYTE_LIMIT,
    ECMASCRIPT_DATE_MAX_MS,
    MAX_REPLAY_DOMAIN_ELAPSED_MS,
    MAX_REPLAY_HOST_ELAPSED_MS,
    MAX_REPLAY_OBSERVATIONS,
    MAX_REPLAY_TICKS,
    MAX_REPLAY_TICK_MS,
    REFERENCE_FRAME_BYTE_LIMIT,
    REFERENCE_PROOF_BYTE_LIMIT,
    REFERENCE_PROOF_VERSION,
    REFERENCE_STATE_BYTE_LIMIT,
    Checkpoint,
    ManifestError,
    Scenario,
    candidate_evidence_from_log,
    candidate_state_from_log,
    commands_for,
    load_manifest,
    load_state,
    qualify,
    replay_seed_sha256,
    replay_ledger_from_log,
    trace_digest,
    trace_replay_origin_ms,
    verify_reference_proof_binding,
    verify_candidate_state_binding,
    write_state,
)


TRACE_ORIGIN_MS = 1_784_342_534_779


def zero_clock_sources() -> dict[str, int]:
    return {
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
    }


def scenario(digest: str, *, trivial: bool = False) -> Scenario:
    checkpoints = (
        (Checkpoint("top", "top"), Checkpoint("bottom", "bottom"))
        if trivial
        else (
            Checkpoint("top", "top"),
            Checkpoint("anchor-1", "anchor", "middle"),
            Checkpoint("bottom", "bottom"),
        )
    )
    return Scenario(
        scenario="fixture",
        url="https://fixture.test/",
        replay_dir="fixture",
        trace_sha256=digest,
        expected_http=200,
        required_title="Fixture",
        required_state_marker="Ready marker",
        fallback_markers=("Fallback shell",),
        interstitial_markers=("Checking browser",),
        device_width=2,
        device_height=1,
        css_width=2,
        css_height=1,
        scale_numerator=1,
        scale_denominator=1,
        checkpoints=checkpoints,
        reference_state="fixture/reference-state.json",
        limit_mb=4,
        ticks=1,
        tick_ms=1,
        max_download_kb=1,
        script_timeout_ms=1,
        script_heap_mb=1,
        script_total_mb=1,
        script_file_kb=1,
        script_count=1,
        min_stylesheets_loaded=1,
        min_images_loaded=1,
        min_scripts_loaded=1,
        min_network_completions=1,
        max_pending=0,
    )


def healthy_replay_ledger() -> dict[str, object]:
    return {
        "mode": "response-keyed",
        "records": 3,
        "claimed": 2,
        "requests": 3,
        "matched": 3,
        "served": 3,
        "rejected": 0,
        "unmatched": 0,
        "conflicts": 0,
        "invalid": 0,
        "shape_mismatches": 2,
        "claimed_routes": [0, 2],
        "route_selection_version": "ranked-occurrence-v2",
        "occurrence_claims": 0,
        "reusable_claims": 3,
        "occurrence_exhausted": 0,
    }


def healthy_reference_replay_ledger() -> dict[str, object]:
    return {
        **healthy_replay_ledger(),
        "mode": "cdp-response-keyed",
        "routes": 3,
        "claimed": 3,
        "scheduled": 3,
        "claimed_routes": [0, 1, 2],
        "shape_mismatches": 0,
        "shape_comparison": "not-collected",
        "claimed_route_ranges": "0000-0002",
        "unmatched_urls": [],
        "route_selection_version": "ranked-occurrence-v2",
        "occurrence_claims": 2,
        "reusable_claims": 1,
        "occurrence_exhausted": 0,
        "unexpected_requests": healthy_request_diagnostics(),
    }


def healthy_request_diagnostics(total: int = 0) -> dict[str, object]:
    return {
        "total": total,
        "retained": 0,
        "truncated": total,
        "overflow": False,
        "multiset_sha256": hashlib.sha256(
            b"tilefinch-diagnostic-multiset-v1\0" + b"0\0" + b"0" * 64
        ).hexdigest() if total == 0 else "1" * 64,
        "by_method": {} if total == 0 else {"POST": total},
        "by_resource_type": {} if total == 0 else {"fetch": total},
        "by_origin": {} if total == 0 else {"https://fixture.test": total},
        "entries": [],
    }


def candidate_evidence_line(
    item: Scenario, **overrides: str
) -> str:
    scrolls = "0,0" if item.trivial_viewport else "0,10,20"
    values = {
        "schema": "2",
        "title-hex": "Fixture title".encode().hex(),
        "url-hex": item.url.encode().hex(),
        "body-preview-hex": "Ready marker".encode().hex(),
        "http-status": "200",
        "marker-hex": item.required_state_marker.encode().hex(),
        "marker-found": "yes",
        "marker-bytes": str(len(item.required_state_marker.encode("utf-8"))),
        "stylesheets-loaded": "1",
        "images-loaded": "1",
        "scripts-loaded": "1",
        "network-completions": "1",
        "active-native": "0",
        "pending-logical": "0",
        "seed": str(TRACE_ORIGIN_MS),
        "seed-source": "trace-origin-ms-v1",
        "rng": "splitmix64-url-scope-v1",
        "clock": "dual-domain-ms-call-v2",
        "clock-scope": "top-level-realm-v1",
        "host-elapsed-ms": str(item.ticks * item.tick_ms),
        "wall-elapsed-ms": str(item.ticks * item.tick_ms),
        "monotonic-elapsed-ms": str(item.ticks * item.tick_ms),
        "wall-observations": "0",
        "monotonic-observations": "0",
        "monotonic-samples": "0",
        **{f"clock-{name.replace('_', '-')}": "0" for name in zero_clock_sources()},
        "performance-entries": "normalized-empty-v1",
        "intl": "bounded-en-us-utc-v1",
        "frames": str(len(item.checkpoints)),
        "scrolls": scrolls,
        "replay-mode": "response-keyed",
        "route-selection": "ranked-occurrence-v2",
        "occurrence-claims": "0",
        "reusable-claims": "3",
        "occurrence-exhausted": "0",
        "records": "3",
        "claimed": "2",
        "requests": "3",
        "matched": "3",
        "served": "3",
        "rejected": "0",
        "unmatched": "0",
        "conflicts": "0",
        "invalid": "0",
        "shape-mismatches": "2",
        "claimed-routes": "0,2",
    }
    values.update(overrides)
    return CANDIDATE_EVIDENCE_VERSION + " " + " ".join(
        f"{name}={values[name]}" for name in CANDIDATE_EVIDENCE_FIELDS
    )


def write_visual_manifest(path: Path, item: Scenario) -> None:
    header = (BENCHMARKS / "visual-scenarios.tsv").read_text().splitlines()[0]
    values = [
        item.scenario,
        item.url,
        item.replay_dir,
        item.trace_sha256,
        "200",
        "Fixture",
        "Ready marker",
        "Fallback shell",
        "Checking browser",
        "2",
        "1",
        "2",
        "1",
        "1",
        "1",
        "top|anchor:middle|bottom",
        item.reference_state,
        "4",
        "1",
        "1",
        "1",
        "1",
        "1",
        "1",
        "1",
        "1",
        "1",
        "1",
        "1",
        "1",
        "0",
    ]
    path.write_text(header + "\n" + "\t".join(values) + "\n")


def make_modern_reference(state: dict[str, object], item: Scenario) -> None:
    state["capture_url"] = item.url
    state["capture_transport"] = "cdp-response-keyed"
    state["capture_ready"] = True
    state["replay_ledger"] = healthy_reference_replay_ledger()
    state["replay_environment"] = {
        "version": "deterministic-hermetic-v3",
        "origin_ms": TRACE_ORIGIN_MS,
        "clock_version": "playwright-clock-paused-v2",
        "clock_contract": "dual-domain-ms-call-v2",
        "clock_scope": "top-level-realm-v1",
        "rng_version": "splitmix64-url-scope-v1",
        "seed_source": "trace-origin-ms-v1",
        "intl_surface": "bounded-en-us-utc-v1",
        "seed_u64": str(TRACE_ORIGIN_MS),
        "seed_sha256": replay_seed_sha256(TRACE_ORIGIN_MS),
        "ticks": item.ticks,
        "tick_ms": item.tick_ms,
        "host_elapsed_ms": item.ticks * item.tick_ms,
        "wall_elapsed_ms": item.ticks * item.tick_ms,
        "monotonic_elapsed_ms": item.ticks * item.tick_ms,
        "wall_observations": 0,
        "monotonic_observations": 0,
        "monotonic_samples": 0,
        "clock_sources": zero_clock_sources(),
        "performance_entries": "normalized-empty-v1",
        "document_timeline": "dual-domain-ms-call-v2",
        "animation_frame": "dual-domain-ms-call-v2",
    }
    state["read_only_policy"] = {
        "version": "get-head-only-v3",
        "preflight_policy": "cors-preflight-before-network-v1",
        "ready": True,
        "denied_before_network": 0,
        "preflight_denied_before_network": 0,
        "diagnostics": healthy_request_diagnostics(),
    }
    state["offline_capability_policy"] = {
        "version": "offline-capabilities-v2",
        "ready": True,
        "worker_realms": "api-unavailable-before-page-script",
        "shared_worker_realms": "api-unavailable-before-page-script",
        "worklet_realms": "api-unavailable-before-module-load",
        "shadow_realms": "api-unavailable-before-page-script",
        "service_workers": "api-unavailable-and-browser-context-blocked",
        "delayed_fetch": "blocked-before-network",
        "randomized_webcrypto": "blocked-before-operation",
        "surface_evidence": {
            "version": "realm-entrypoints-unavailable-v1",
            "dedicated_worker_constructor": "unavailable",
            "shared_worker_constructor": "unavailable",
            "shadow_realm_constructor": "unavailable",
            "audio_worklet_node_constructor": "unavailable",
            "worklet_constructor": "unavailable",
            "css_worklet_loaders": "unavailable",
            "audio_worklet_loader": "unavailable",
            "shared_storage_worklet_loader": "unavailable",
            "service_worker_registration": "unavailable",
            "service_worker_interfaces": "unavailable",
        },
        "diagnostics": healthy_request_diagnostics(),
    }
    state["response_scheduler"] = {
        "version": "admission-generations-v7",
        "terminal_boundary": "all-http-terminal-v1",
        "ordering": (
            "semantic-pump,admission-generation,record-id,exact-request-identity"
        ),
        "admission_ordering": (
            "exact-route-key,global-route-occurrence,resource-type,"
            "playwright-callback-ordinal"
        ),
        "admission_probe": "bounded-playwright-context-roundtrip-quiescence",
        "order_digest_version": "exact-request-identity-v3",
        "browser_ordinal_semantics": "raw-playwright-route-callback-v1",
        "request_limit": 16_384,
        "retained_delay_limit_pumps": 1_000_000,
        "semantic_pump_limit": 16_384_000_000,
        "pump_work_limit": 16_384_000_000,
        "drive_step_limit": 32_768,
        "pump_work_units": 3,
        "scheduled_delay_work_units": 3,
        "retained_delay_work_units": 3,
        "terminal_delay_work_units": 0,
        "retained_admissions": 3,
        "terminal_admissions": 0,
        "batches": 1,
        "generations": 1,
        "semantic_pumps": 1,
        "fast_forwarded_pumps": 0,
        "admission_probe_stable_passes": 2,
        "admission_probe_limit": 64,
        "admission_probe_timeout_ms": 2000,
        "enqueued": 3,
        "completed": 3,
        "pending": 0,
        "max_pending": 3,
        "failures": 0,
        "terminal_failures": 0,
        "rejected_after_terminal": 0,
        "exact_order_count": 3,
        "semantic_delivery_count": 3,
        "raw_callback_count": 3,
        "overflow": False,
        "order_sha256": "3" * 64,
        "semantic_delivery_order_sha256": "4" * 64,
        "raw_callback_arrival_sha256": "5" * 64,
        "ready": True,
    }
    state["host_operations"] = {
        "version": "bounded-host-operations-v1",
        "timeout_ms": 15_000,
        "started": 6,
        "completed": 6,
        "rejected": 0,
        "timed_out": 0,
        "pending": 0,
        "orphaned": 0,
        "orphan_pending": 0,
        "late_completions": 0,
        "terminal_failures": 0,
        "rejected_after_terminal": 0,
        "terminal_label": "",
        "closed": True,
        "ready": True,
    }
    state["publication_boundary"] = {
        "version": "close-then-publish-v1",
        "ready": True,
        "context_closed": True,
        "browser_closed": True,
        "final_activity": 6,
        "late_callbacks": {
            "closing": 0, "closed": 0, "binding": 0, "handlers": 0,
            "terminal_failures": 0,
        },
        "teardown_changes": [],
    }
    state["acquisition_plan"] = {
        "mode": "exact-get-head-plan-v1",
        "complete": True,
        "request_count": 0,
        "unplannable": 0,
        "requests": [],
    }
    state["browser"] = {
        "engine": "chromium",
        "version": "149.0.7827.55",
        "user_agent": (
            "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
            "(KHTML, like Gecko) HeadlessChrome/149.0.7827.55 Safari/537.36"
        ),
        "platform": "fixture",
        "locale": "en-US",
        "timezone": "UTC",
    }
    state["failure"] = None
    state["eligibility_reasons"] = []


def state_for(item: Scenario, side: str, root: Path) -> dict[str, object]:
    checkpoints: list[dict[str, object]] = []
    for index, checkpoint in enumerate(item.checkpoints):
        if side == "reference":
            frame_name = f"frame-{index}.png"
            write_png_rgb(root / frame_name, 2, 1, b"\x10\x20\x30" * 2)
            frame_format = "png-rgb8"
        else:
            frame_name = f"frame-{index}.ppm"
            (root / frame_name).write_bytes(b"P6\n2 1\n255\n" + b"\x10\x20\x30" * 2)
            frame_format = "ppm-p6-rgb8"
        checkpoints.append(
            {
                "name": checkpoint.name,
                "kind": checkpoint.kind,
                "target": checkpoint.target,
                "scroll_y": 0 if item.trivial_viewport else index * 10,
                "frame": frame_name,
                "format": frame_format,
                "width": 2,
                "height": 1,
            }
        )
    state: dict[str, object] = {
        "schema": 2,
        "scenario": item.scenario,
        "trace_sha256": item.trace_sha256,
        "url": item.url,
        "capture_url": (
            "http://127.0.0.1:8765/fixture" if side == "reference" else item.url
        ),
        "http_status": 200,
        "title": "Fixture title",
        "state_markers": ["Ready marker"],
        "fallback": False,
        "interstitial": False,
        "top_bottom_coincident": item.trivial_viewport,
        "viewport": item.viewport,
        "resources": {
            "ready": True,
            "stylesheets_loaded": 1,
            "images_loaded": 1,
            "scripts_loaded": 1,
            "network_completions": 3 if side == "reference" else 1,
            "deferred_images": 0,
            "pending": 0,
        },
        "checkpoints": checkpoints,
    }
    if side == "candidate":
        state["replay_ledger"] = healthy_replay_ledger()
        state["candidate_evidence"] = {
            "version": CANDIDATE_EVIDENCE_VERSION,
            "schema": 2,
            "encoding": "fixed-order-hex-utf8-v1",
            "record_sha256": "4" * 64,
        }
        state["replay_environment"] = {
            "version": "deterministic-hermetic-v3",
            "origin_ms": TRACE_ORIGIN_MS,
            "clock_version": "native-script-runtime-v2",
            "clock_contract": "dual-domain-ms-call-v2",
            "clock_scope": "top-level-realm-v1",
            "rng_version": "splitmix64-url-scope-v1",
            "seed_source": "trace-origin-ms-v1",
            "intl_surface": "bounded-en-us-utc-v1",
            "seed_u64": str(TRACE_ORIGIN_MS),
            "seed_sha256": replay_seed_sha256(TRACE_ORIGIN_MS),
            "ticks": item.ticks,
            "tick_ms": item.tick_ms,
            "host_elapsed_ms": item.ticks * item.tick_ms,
            "wall_elapsed_ms": item.ticks * item.tick_ms,
            "monotonic_elapsed_ms": item.ticks * item.tick_ms,
            "wall_observations": 0,
            "monotonic_observations": 0,
            "monotonic_samples": 0,
            "clock_sources": zero_clock_sources(),
            "performance_entries": "normalized-empty-v1",
            "document_timeline": "dual-domain-ms-call-v2",
            "animation_frame": "dual-domain-ms-call-v2",
            "requested_origin_ms": TRACE_ORIGIN_MS,
        }
    else:
        make_modern_reference(state, item)
    return state


class VisualScenarioTest(unittest.TestCase):
    def make_trace(self, root: Path) -> Path:
        trace = root / "fixture"
        trace.mkdir()
        (trace / "trace.meta").write_text(
            "psp-http-trace=fixture\n"
            "psp-http-trace-clock=1\n"
            f"origin-ms={TRACE_ORIGIN_MS}\n"
            "capture-complete=yes\n"
            "record-count=1\n"
        )
        (trace / "0000.meta").write_text("status=200\n")
        (trace / "0000.body").write_text("Ready marker\n")
        return trace

    def make_qualify_only_fixture(
        self, root: Path
    ) -> tuple[Scenario, Path, Path, Path, Path]:
        trace = self.make_trace(root)
        item = scenario(trace_digest(trace))
        manifest = root / "manifest.tsv"
        write_visual_manifest(manifest, item)

        reference_directory = root / "references" / "fixture"
        reference_directory.mkdir(parents=True)
        write_state(
            reference_directory / "reference-state.json",
            state_for(item, "reference", reference_directory),
        )

        scenario_output = root / "output" / item.scenario
        frames = scenario_output / "frames"
        frames.mkdir(parents=True)
        for index in range(len(item.checkpoints)):
            (frames / f"frame-{index:04d}.ppm").write_bytes(
                b"P6\n2 1\n255\n" + b"\x10\x20\x30" * 2
            )
        log_path = scenario_output / "candidate.log"
        log = candidate_evidence_line(item) + "\n"
        log_path.write_text(log, encoding="utf-8")
        candidate_state = scenario_output / "candidate-state.json"
        write_state(
            candidate_state,
            candidate_state_from_log(
                item, log, frames, origin_ms=TRACE_ORIGIN_MS
            ),
        )
        return item, manifest, root / "output", log_path, candidate_state

    def make_reference_proof(self, root: Path, item: Scenario) -> Path:
        proof_root = root / "proof"
        states: list[dict[str, object]] = []
        state_paths: list[Path] = []
        for run_name in ("run-a", "run-b"):
            reference_directory = proof_root / run_name / item.scenario
            reference_directory.mkdir(parents=True)
            state = state_for(item, "reference", reference_directory)
            state_path = reference_directory / "reference-state.json"
            write_state(state_path, state)
            states.append(state)
            state_paths.append(state_path)
        self.assertEqual(state_paths[0].read_bytes(), state_paths[1].read_bytes())
        checkpoints = states[0]["checkpoints"]
        assert isinstance(checkpoints, list)
        frame_names = sorted(str(checkpoint["frame"]) for checkpoint in checkpoints)
        proof = {
            "version": REFERENCE_PROOF_VERSION,
            "equivalent": True,
            "capture_ready": True,
            "scenario": item.scenario,
            "trace_sha256": item.trace_sha256,
            "canonical_run": "run-a",
            "comparison_run": "run-b",
            "state_schema": 2,
            "replay_environment": states[0]["replay_environment"],
            "state": {
                "name": "reference-state.json",
                "sha256": hashlib.sha256(state_paths[0].read_bytes()).hexdigest(),
            },
            "frames": [
                {
                    "name": name,
                    "sha256": hashlib.sha256(
                        (state_paths[0].parent / name).read_bytes()
                    ).hexdigest(),
                }
                for name in frame_names
            ],
        }
        write_state(proof_root / "determinism-proof.json", proof)
        return proof_root

    def run_qualify_only(
        self,
        root: Path,
        manifest: Path,
        output: Path,
        *,
        reference_root: Path | None = None,
        require_proof: bool = False,
    ) -> subprocess.CompletedProcess[str]:
        command = [
            sys.executable,
            str(BENCHMARKS / "run-visual-scenarios.py"),
            str(root / "build"),
            str(root),
            str(reference_root or root / "references"),
            str(output),
            "--manifest",
            str(manifest),
            "--qualify-only",
        ]
        if require_proof:
            command.append("--require-reference-proof")
        return subprocess.run(
            command,
            capture_output=True,
            text=True,
        )

    def test_checked_manifest_has_nine_explicit_scenarios(self) -> None:
        scenarios = load_manifest(BENCHMARKS / "visual-scenarios.tsv")
        self.assertEqual(len(scenarios), 9)
        self.assertEqual(scenarios[0].scenario, "example")
        self.assertTrue(scenarios[0].trivial_viewport)
        self.assertEqual(scenarios[5].checkpoints[1].kind, "selector")

    def test_manifest_clock_bounds_match_the_native_lab(self) -> None:
        lines = (BENCHMARKS / "visual-scenarios.tsv").read_text().splitlines()
        header = lines[0].split("\t")
        source = next(
            line.split("\t") for line in lines[1:] if line.startswith("hacker-news\t")
        )

        def checked(root: Path, **overrides: int) -> Scenario:
            row = source.copy()
            for name, value in overrides.items():
                row[header.index(name)] = str(value)
            manifest = root / "manifest.tsv"
            manifest.write_text("\t".join(header) + "\n" + "\t".join(row) + "\n")
            return load_manifest(manifest)[0]

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            boundary = checked(
                root,
                ticks=MAX_REPLAY_TICKS,
                tick_ms=MAX_REPLAY_TICK_MS,
            )
            self.assertEqual(boundary.ticks, 1_000)
            self.assertEqual(boundary.tick_ms, 60_000)
            for name, value in (
                ("ticks", MAX_REPLAY_TICKS + 1),
                ("tick_ms", MAX_REPLAY_TICK_MS + 1),
            ):
                with self.subTest(name=name), self.assertRaisesRegex(
                    ManifestError, rf"{name} must be <="
                ):
                    checked(root, **{name: value})

    def test_manifest_direct_cli_numeric_bounds_fail_before_launch(self) -> None:
        lines = (BENCHMARKS / "visual-scenarios.tsv").read_text().splitlines()
        header = lines[0].split("\t")
        source = next(
            line.split("\t") for line in lines[1:] if line.startswith("hacker-news\t")
        )
        cases = (
            ("expected_http", 99),
            ("expected_http", 600),
            ("limit_mb", 3),
            ("limit_mb", 513),
            ("max_download_kb", 65_537),
            ("script_timeout_ms", 300_001),
            ("script_heap_mb", 257),
            ("script_total_mb", 129),
            ("script_file_kb", 8_193),
            ("script_count", 257),
        )
        for field, value in cases:
            with self.subTest(field=field, value=value), tempfile.TemporaryDirectory() as directory:
                row = source.copy()
                row[header.index(field)] = str(value)
                manifest = Path(directory) / "manifest.tsv"
                manifest.write_text(
                    "\t".join(header) + "\n" + "\t".join(row) + "\n"
                )
                with self.assertRaises(ManifestError):
                    load_manifest(manifest)

    def test_matching_states_and_exact_decoded_frames_are_eligible(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            trace = self.make_trace(root)
            item = scenario(trace_digest(trace))
            reference = root / "reference"
            candidate = root / "candidate"
            reference.mkdir()
            candidate.mkdir()
            reference_state = reference / "state.json"
            candidate_state = candidate / "state.json"
            write_state(reference_state, state_for(item, "reference", reference))
            write_state(candidate_state, state_for(item, "candidate", candidate))
            result = qualify(item, trace, reference_state, candidate_state)
            self.assertEqual(result.status, "ELIGIBLE", result.details)

    def test_qualification_accepts_the_shared_clock_source_boundary(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            trace = self.make_trace(root)
            item = replace(
                scenario(trace_digest(trace)),
                ticks=MAX_REPLAY_TICKS,
                tick_ms=MAX_REPLAY_TICK_MS,
            )
            reference = root / "reference"
            candidate = root / "candidate"
            reference.mkdir()
            candidate.mkdir()
            reference_value = state_for(item, "reference", reference)
            candidate_value = state_for(item, "candidate", candidate)
            for value in (reference_value, candidate_value):
                environment = value["replay_environment"]
                assert isinstance(environment, dict)
                environment["host_elapsed_ms"] = MAX_REPLAY_HOST_ELAPSED_MS
                environment["wall_elapsed_ms"] = MAX_REPLAY_DOMAIN_ELAPSED_MS
                environment["monotonic_elapsed_ms"] = MAX_REPLAY_HOST_ELAPSED_MS
                environment["wall_observations"] = MAX_REPLAY_OBSERVATIONS
                environment["monotonic_observations"] = 0
                environment["monotonic_samples"] = 0
                environment["clock_sources"]["date_now"] = MAX_REPLAY_OBSERVATIONS
            reference_state = reference / "state.json"
            candidate_state = candidate / "state.json"
            write_state(reference_state, reference_value)
            write_state(candidate_state, candidate_value)

            result = qualify(item, trace, reference_state, candidate_state)

            self.assertEqual(result.status, "ELIGIBLE", result.details)

    def test_candidate_state_requires_native_machine_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            trace = self.make_trace(root)
            item = scenario(trace_digest(trace))
            reference = root / "reference"
            candidate = root / "candidate"
            reference.mkdir()
            candidate.mkdir()
            reference_state = reference / "state.json"
            candidate_state = candidate / "state.json"
            write_state(reference_state, state_for(item, "reference", reference))
            candidate_value = state_for(item, "candidate", candidate)
            del candidate_value["candidate_evidence"]
            write_state(candidate_state, candidate_value)

            result = qualify(item, trace, reference_state, candidate_state)

            self.assertEqual(result.status, "NOT_COMPARABLE")
            self.assertIn("candidate-machine-evidence-invalid", result.details)

    def test_cdp_reference_capture_requires_canonical_trust_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            trace = self.make_trace(root)
            item = scenario(trace_digest(trace))
            reference = root / "reference"
            candidate = root / "candidate"
            reference.mkdir()
            candidate.mkdir()
            reference_state = reference / "state.json"
            candidate_state = candidate / "state.json"
            baseline = state_for(item, "reference", reference)
            make_modern_reference(baseline, item)
            write_state(candidate_state, state_for(item, "candidate", candidate))
            write_state(reference_state, baseline)
            result = qualify(item, trace, reference_state, candidate_state)
            self.assertEqual(result.status, "ELIGIBLE", result.details)

            retained_failure = copy.deepcopy(baseline)
            retained_failure["replay_ledger"] = {
                **healthy_reference_replay_ledger(),
                "served": 2,
                "rejected": 1,
            }
            retained_failure["resources"]["network_completions"] = 2
            write_state(reference_state, retained_failure)
            result = qualify(item, trace, reference_state, candidate_state)
            self.assertEqual(result.status, "ELIGIBLE", result.details)
            write_state(reference_state, baseline)

            cases: tuple[tuple[str, object, str], ...] = (
                (
                    "transport-missing",
                    None,
                    "reference-capture-transport-mismatch",
                ),
                (
                    "transport-wrong",
                    "loopback",
                    "reference-capture-transport-mismatch",
                ),
                (
                    "capture-url",
                    "http://127.0.0.1:8765/fixture",
                    "reference-capture-url-mismatch",
                ),
                (
                    "not-ready",
                    False,
                    "reference-capture-not-ready",
                ),
                (
                    "retained-failure",
                    "navigation:timeout",
                    "reference-capture-failure",
                ),
                (
                    "retained-eligibility-reason",
                    ["reference-title-mismatch"],
                    "reference-eligibility-reasons-not-empty",
                ),
                (
                    "ledger-missing",
                    None,
                    "reference-replay-ledger-missing",
                ),
                (
                    "ledger-mode",
                    {**healthy_reference_replay_ledger(), "mode": "response-keyed"},
                    "reference-replay-ledger-mode",
                ),
                (
                    "request-schedule",
                    {**healthy_reference_replay_ledger(), "requests": 4},
                    "reference-replay-ledger-request-schedule-mismatch",
                ),
                (
                    "request-terminal-accounting",
                    {**healthy_reference_replay_ledger(), "requests": 4},
                    "reference-replay-ledger-request-terminal-mismatch",
                ),
                (
                    "schedule-match",
                    {**healthy_reference_replay_ledger(), "scheduled": 2},
                    "reference-replay-ledger-schedule-match-mismatch",
                ),
                (
                    "scheduled-invalid",
                    {**healthy_reference_replay_ledger(), "scheduled": "3"},
                    "reference-replay-ledger-scheduled-invalid",
                ),
                (
                    "matched-closure",
                    {**healthy_reference_replay_ledger(), "served": 2},
                    "reference-replay-ledger-match-serve-mismatch",
                ),
                (
                    "unmatched",
                    {**healthy_reference_replay_ledger(), "unmatched": 1},
                    "reference-replay-ledger-unmatched",
                ),
                (
                    "conflicts",
                    {**healthy_reference_replay_ledger(), "conflicts": 1},
                    "reference-replay-ledger-conflicts",
                ),
                (
                    "invalid",
                    {**healthy_reference_replay_ledger(), "invalid": 1},
                    "reference-replay-ledger-invalid-routes",
                ),
                (
                    "environment-missing",
                    None,
                    "reference-replay-environment-missing",
                ),
                (
                    "environment-version",
                    {
                        **baseline["replay_environment"],  # type: ignore[arg-type]
                        "version": "uncontrolled",
                    },
                    "reference-replay-environment-version",
                ),
                (
                    "environment-clock",
                    {
                        **baseline["replay_environment"],  # type: ignore[arg-type]
                        "clock_contract": "host-clock",
                    },
                    "reference-replay-environment-clock-contract",
                ),
                (
                    "environment-fields",
                    {
                        **baseline["replay_environment"],  # type: ignore[arg-type]
                        "logical_elapsed_ms": item.ticks * item.tick_ms,
                    },
                    "reference-replay-environment-fields",
                ),
                (
                    "environment-rng",
                    {
                        **baseline["replay_environment"],  # type: ignore[arg-type]
                        "rng_version": "host-random",
                    },
                    "reference-replay-environment-rng-contract",
                ),
                (
                    "environment-seed",
                    {
                        **baseline["replay_environment"],  # type: ignore[arg-type]
                        "seed_u64": "42",
                    },
                    "reference-replay-environment-seed-u64",
                ),
                (
                    "environment-intl",
                    {
                        **baseline["replay_environment"],  # type: ignore[arg-type]
                        "intl_surface": "host-icu",
                    },
                    "reference-replay-environment-intl-surface",
                ),
                (
                    "environment-ticks",
                    {
                        **baseline["replay_environment"],  # type: ignore[arg-type]
                        "ticks": item.ticks + 1,
                    },
                    "reference-replay-environment-ticks",
                ),
                (
                    "environment-domain-clock",
                    {
                        **baseline["replay_environment"],  # type: ignore[arg-type]
                        "wall_elapsed_ms": item.ticks * item.tick_ms + 1,
                    },
                    "reference-replay-environment-domain-clock",
                ),
                (
                    "environment-clock-sources",
                    {
                        **baseline["replay_environment"],  # type: ignore[arg-type]
                        "clock_sources": {
                            **baseline["replay_environment"]["clock_sources"],  # type: ignore[index]
                            "performance_now": 1,
                        },
                    },
                    "reference-replay-environment-clock-source-closure",
                ),
                (
                    "occurrence-version",
                    {
                        **healthy_reference_replay_ledger(),
                        "route_selection_version": "first-record",
                    },
                    "reference-replay-ledger-route-selection-version",
                ),
                (
                    "occurrence-closure",
                    {
                        **healthy_reference_replay_ledger(),
                        "occurrence_claims": 0,
                        "reusable_claims": 0,
                    },
                    "reference-replay-ledger-occurrence-closure",
                ),
                (
                    "occurrence-distinct-route-closure",
                    {
                        **healthy_reference_replay_ledger(),
                        "claimed": 2,
                        "claimed_routes": [0, 2],
                    },
                    "reference-replay-ledger-occurrence-closure",
                ),
                (
                    "policy-missing",
                    None,
                    "reference-read-only-policy-missing",
                ),
                (
                    "policy-not-ready",
                    {
                        **baseline["read_only_policy"],  # type: ignore[arg-type]
                        "ready": False,
                    },
                    "reference-read-only-policy-not-ready",
                ),
                (
                    "policy-count-mismatch",
                    {
                        **baseline["read_only_policy"],  # type: ignore[arg-type]
                        "denied_before_network": 1,
                    },
                    "reference-read-only-policy-diagnostics-count-mismatch",
                ),
                (
                    "policy-preflight-version",
                    {
                        **baseline["read_only_policy"],  # type: ignore[arg-type]
                        "preflight_policy": "uncontrolled",
                    },
                    "reference-read-only-policy-preflight-version",
                ),
                (
                    "policy-preflight-denied",
                    {
                        **baseline["read_only_policy"],  # type: ignore[arg-type]
                        "ready": False,
                        "preflight_denied_before_network": 1,
                    },
                    "reference-read-only-policy-preflight-denied",
                ),
                (
                    "policy-overflow",
                    {
                        **baseline["read_only_policy"],  # type: ignore[arg-type]
                        "diagnostics": {
                            **healthy_request_diagnostics(),
                            "overflow": True,
                        },
                    },
                    "reference-read-only-policy-diagnostics-overflow",
                ),
                (
                    "capability-missing",
                    None,
                    "reference-capability-policy-missing",
                ),
                (
                    "capability-mode",
                    {
                        **baseline["offline_capability_policy"],  # type: ignore[arg-type]
                        "worker_realms": "host-worker",
                    },
                    "reference-capability-policy-mode",
                ),
                (
                    "capability-surface",
                    {
                        **baseline["offline_capability_policy"],  # type: ignore[arg-type]
                        "surface_evidence": {
                            **baseline["offline_capability_policy"]["surface_evidence"],  # type: ignore[index]
                            "shared_worker_constructor": "exposed",
                        },
                    },
                    "reference-capability-policy-surface-evidence",
                ),
                (
                    "capability-denied",
                    {
                        **baseline["offline_capability_policy"],  # type: ignore[arg-type]
                        "ready": False,
                        "diagnostics": healthy_request_diagnostics(1),
                    },
                    "reference-capability-policy-denied",
                ),
                (
                    "scheduler-missing",
                    None,
                    "reference-response-scheduler-missing",
                ),
                (
                    "scheduler-ordering",
                    {
                        **baseline["response_scheduler"],  # type: ignore[arg-type]
                        "ordering": "arrival-order",
                    },
                    "reference-response-scheduler-ordering",
                ),
                (
                    "scheduler-version",
                    {
                        **baseline["response_scheduler"],  # type: ignore[arg-type]
                        "version": "admission-generations-v6",
                    },
                    "reference-response-scheduler-version",
                ),
                (
                    "scheduler-terminal-boundary",
                    {
                        **baseline["response_scheduler"],  # type: ignore[arg-type]
                        "terminal_boundary": "matched-only",
                    },
                    "reference-response-scheduler-ordering",
                ),
                (
                    "scheduler-pump-work",
                    {
                        **baseline["response_scheduler"],  # type: ignore[arg-type]
                        "pump_work_units": 2,
                    },
                    "reference-response-scheduler-pump-work-closure",
                ),
                (
                    "scheduler-failure",
                    {
                        **baseline["response_scheduler"],  # type: ignore[arg-type]
                        "ready": False,
                        "failures": 1,
                    },
                    "reference-response-scheduler-failures",
                ),
                (
                    "scheduler-fast-forward",
                    {
                        **baseline["response_scheduler"],  # type: ignore[arg-type]
                        "fast_forwarded_pumps": 2,
                        "semantic_pumps": 1,
                    },
                    "reference-response-scheduler-fast-forward-closure",
                ),
                (
                    "scheduler-ledger-closure",
                    {
                        **baseline["response_scheduler"],  # type: ignore[arg-type]
                        "enqueued": 2,
                        "completed": 2,
                        "max_pending": 2,
                    },
                    "reference-response-scheduler-ledger-closure",
                ),
                (
                    "scheduler-terminal-admission-closure",
                    {
                        **baseline["response_scheduler"],  # type: ignore[arg-type]
                        "retained_admissions": 2,
                        "terminal_admissions": 1,
                    },
                    "reference-response-scheduler-ledger-closure",
                ),
                (
                    "scheduler-count-closure",
                    {
                        **baseline["response_scheduler"],  # type: ignore[arg-type]
                        "exact_order_count": 2,
                    },
                    "reference-response-scheduler-count-closure",
                ),
                (
                    "scheduler-admission-delay-closure",
                    {
                        **baseline["response_scheduler"],  # type: ignore[arg-type]
                        "retained_delay_work_units": 2,
                        "terminal_delay_work_units": 1,
                    },
                    "reference-response-scheduler-admission-delay-closure",
                ),
                (
                    "scheduler-semantic-digest",
                    {
                        **baseline["response_scheduler"],  # type: ignore[arg-type]
                        "semantic_delivery_order_sha256": "invalid",
                    },
                    "reference-response-scheduler-semantic-delivery-order-sha256",
                ),
                (
                    "scheduler-structural",
                    {
                        **baseline["response_scheduler"],  # type: ignore[arg-type]
                        "generations": 4,
                    },
                    "reference-response-scheduler-structural-closure",
                ),
                (
                    "scheduler-request-bound",
                    {
                        **baseline["response_scheduler"],  # type: ignore[arg-type]
                        "enqueued": 16_385,
                        "completed": 16_385,
                        "max_pending": 16_385,
                    },
                    "reference-response-scheduler-enqueued",
                ),
                (
                    "host-missing",
                    None,
                    "reference-host-operations-missing",
                ),
                (
                    "host-timeout",
                    {
                        **baseline["host_operations"],  # type: ignore[arg-type]
                        "timeout_ms": 120_001,
                    },
                    "reference-host-operations-timeout",
                ),
                (
                    "host-orphan",
                    {
                        **baseline["host_operations"],  # type: ignore[arg-type]
                        "ready": False,
                        "completed": 5,
                        "timed_out": 1,
                        "orphaned": 1,
                        "orphan_pending": 1,
                        "terminal_failures": 1,
                        "terminal_label": "fulfill",
                    },
                    "reference-host-operations-not-ready",
                ),
                (
                    "publication-open",
                    {
                        **baseline["publication_boundary"],  # type: ignore[arg-type]
                        "context_closed": False,
                    },
                    "reference-publication-boundary-invalid",
                ),
                (
                    "publication-delta",
                    {
                        **baseline["publication_boundary"],  # type: ignore[arg-type]
                        "ready": False,
                        "teardown_changes": [{
                            "phase": "context-close",
                            "field": "requests",
                            "before": 3,
                            "after": 4,
                        }],
                    },
                    "reference-publication-boundary-invalid",
                ),
                (
                    "publication-late-callback",
                    {
                        **baseline["publication_boundary"],  # type: ignore[arg-type]
                        "ready": False,
                        "late_callbacks": {
                            "closing": 1, "closed": 0,
                            "binding": 0, "handlers": 0,
                            "terminal_failures": 0,
                        },
                    },
                    "reference-publication-boundary-invalid",
                ),
                (
                    "ledger-diagnostics-missing",
                    {
                        key: value
                        for key, value in healthy_reference_replay_ledger().items()
                        if key != "unexpected_requests"
                    },
                    "reference-replay-ledger-diagnostics-missing",
                ),
                (
                    "acquisition-open",
                    {
                        "mode": "exact-get-head-plan-v1",
                        "complete": True,
                        "request_count": 1,
                        "unplannable": 0,
                        "requests": [{"method": "GET", "url": "https://fixture.test/x"}],
                    },
                    "reference-acquisition-plan-not-closed",
                ),
            )
            for label, value, expected in cases:
                with self.subTest(label=label):
                    state = copy.deepcopy(baseline)
                    if label.startswith("transport"):
                        if value is None:
                            state.pop("capture_transport")
                        else:
                            state["capture_transport"] = value
                    elif label == "capture-url":
                        state["capture_url"] = value
                    elif label == "not-ready":
                        state["capture_ready"] = value
                    elif label == "retained-failure":
                        state["failure"] = value
                    elif label == "retained-eligibility-reason":
                        state["eligibility_reasons"] = value
                    elif label == "ledger-missing":
                        state.pop("replay_ledger")
                    elif label.startswith("environment"):
                        if value is None:
                            state.pop("replay_environment")
                        else:
                            state["replay_environment"] = value
                    elif label.startswith("policy"):
                        if value is None:
                            state.pop("read_only_policy")
                        else:
                            state["read_only_policy"] = value
                    elif label.startswith("capability"):
                        if value is None:
                            state.pop("offline_capability_policy")
                        else:
                            state["offline_capability_policy"] = value
                    elif label.startswith("scheduler"):
                        if value is None:
                            state.pop("response_scheduler")
                        else:
                            state["response_scheduler"] = value
                    elif label.startswith("host"):
                        if value is None:
                            state.pop("host_operations")
                        else:
                            state["host_operations"] = value
                    elif label.startswith("publication"):
                        state["publication_boundary"] = value
                    elif label == "acquisition-open":
                        state["acquisition_plan"] = value
                    else:
                        state["replay_ledger"] = value
                    write_state(reference_state, state)
                    result = qualify(
                        item, trace, reference_state, candidate_state
                    )
                    self.assertEqual(result.status, "NOT_COMPARABLE")
                    self.assertIn(expected, result.details)

    def test_legacy_reference_is_rejected_even_on_loopback(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            trace = self.make_trace(root)
            item = scenario(trace_digest(trace))
            reference = root / "reference"
            candidate = root / "candidate"
            reference.mkdir()
            candidate.mkdir()
            reference_state = reference / "state.json"
            candidate_state = candidate / "state.json"
            reference_value = state_for(item, "reference", reference)
            reference_value["capture_url"] = "http://127.0.0.1:8765/fixture"
            for field in (
                "capture_transport", "capture_ready", "replay_ledger",
                "replay_environment", "read_only_policy",
                "offline_capability_policy", "response_scheduler",
                "publication_boundary", "acquisition_plan",
            ):
                reference_value.pop(field, None)
            candidate_value = state_for(item, "candidate", candidate)
            candidate_value["replay_ledger"] = {
                **healthy_replay_ledger(),
                "served": 2,
                "rejected": 1,
            }
            write_state(reference_state, reference_value)
            write_state(candidate_state, candidate_value)
            result = qualify(item, trace, reference_state, candidate_state)
            self.assertEqual(result.status, "NOT_COMPARABLE", result.details)
            self.assertIn("reference-capture-transport-mismatch", result.details)
            self.assertIn("reference-replay-environment-missing", result.details)

            reference_value["capture_url"] = "http://[malformed"
            write_state(reference_state, reference_value)
            result = qualify(item, trace, reference_state, candidate_state)
            self.assertEqual(result.status, "NOT_COMPARABLE")
            self.assertIn("reference-capture-url-mismatch", result.details)

    def test_final_response_keyed_replay_ledger_is_parsed_boundedly(self) -> None:
        earlier = (
            "http-replay-ledger mode=strict records=1 claimed=1 requests=1 "
            "matched=1 served=1 rejected=0 unmatched=0 conflicts=0 invalid=0 "
            "shape-mismatches=0 claimed-routes=0"
        )
        final = (
            "http-replay-ledger mode=response-keyed records=5 claimed=3 "
            "requests=4 matched=4 served=4 rejected=0 unmatched=0 conflicts=0 invalid=0 "
            "shape-mismatches=4 claimed-routes=1-2,4"
        )
        self.assertEqual(
            replay_ledger_from_log(earlier + "\n" + final),
            {
                "mode": "response-keyed",
                "records": 5,
                "claimed": 3,
                "requests": 4,
                "matched": 4,
                "served": 4,
                "rejected": 0,
                "unmatched": 0,
                "conflicts": 0,
                "invalid": 0,
                "shape_mismatches": 4,
                "claimed_routes": [1, 2, 4],
            },
        )
        self.assertIsNone(
            replay_ledger_from_log(
                final + "\nhttp-replay-ledger mode=response-keyed malformed"
            )
        )
        self.assertIsNone(
            replay_ledger_from_log(
                final.replace("claimed-routes=1-2,4", "claimed-routes=1-5")
            )
        )

    def test_candidate_replay_ledger_health_is_required(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            trace = self.make_trace(root)
            item = scenario(trace_digest(trace))
            reference = root / "reference"
            candidate = root / "candidate"
            reference.mkdir()
            candidate.mkdir()
            reference_state = reference / "state.json"
            candidate_state = candidate / "state.json"
            write_state(reference_state, state_for(item, "reference", reference))
            baseline = state_for(item, "candidate", candidate)
            cases: tuple[tuple[str, object, str], ...] = (
                (
                    "missing",
                    None,
                    "candidate-replay-ledger-missing",
                ),
                (
                    "strict-mode",
                    {**healthy_replay_ledger(), "mode": "strict"},
                    "candidate-replay-ledger-mode",
                ),
                (
                    "zero-served",
                    {
                        **healthy_replay_ledger(),
                        "claimed": 0,
                        "matched": 0,
                        "served": 0,
                        "claimed_routes": [],
                    },
                    "candidate-replay-ledger-no-served-response",
                ),
                (
                    "serve-mismatch",
                    {**healthy_replay_ledger(), "served": 2},
                    "candidate-replay-ledger-match-serve-mismatch",
                ),
                (
                    "unclassified-request",
                    {**healthy_replay_ledger(), "requests": 4},
                    "candidate-replay-ledger-request-terminal-mismatch",
                ),
                (
                    "unmatched",
                    {**healthy_replay_ledger(), "unmatched": 1},
                    "candidate-replay-ledger-unmatched",
                ),
                (
                    "conflict",
                    {**healthy_replay_ledger(), "conflicts": 1},
                    "candidate-replay-ledger-conflicts",
                ),
                (
                    "invalid-route",
                    {**healthy_replay_ledger(), "invalid": 1},
                    "candidate-replay-ledger-invalid-routes",
                ),
                (
                    "claimed-route-list",
                    {**healthy_replay_ledger(), "claimed_routes": [0, 3]},
                    "candidate-replay-ledger-claimed-routes-invalid",
                ),
                (
                    "matched-without-claimed-route",
                    {
                        **healthy_replay_ledger(),
                        "claimed": 0,
                        "claimed_routes": [],
                    },
                    "candidate-replay-ledger-occurrence-closure",
                ),
                (
                    "unbounded-count",
                    {**healthy_replay_ledger(), "requests": 1_000_001},
                    "candidate-replay-ledger-invalid",
                ),
            )
            for label, ledger, expected in cases:
                with self.subTest(label=label):
                    candidate_value = copy.deepcopy(baseline)
                    if ledger is None:
                        candidate_value.pop("replay_ledger")
                    else:
                        candidate_value["replay_ledger"] = ledger
                    write_state(candidate_state, candidate_value)
                    result = qualify(
                        item, trace, reference_state, candidate_state
                    )
                    self.assertEqual(result.status, "NOT_COMPARABLE")
                    self.assertIn(expected, result.details)

    def test_candidate_replay_seed_intl_and_requested_origin_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            trace = self.make_trace(root)
            item = scenario(trace_digest(trace))
            reference = root / "reference"
            candidate = root / "candidate"
            reference.mkdir()
            candidate.mkdir()
            reference_state = reference / "state.json"
            candidate_state = candidate / "state.json"
            write_state(reference_state, state_for(item, "reference", reference))
            baseline = state_for(item, "candidate", candidate)
            cases = (
                (
                    "requested-origin",
                    {"requested_origin_ms": TRACE_ORIGIN_MS + 1},
                    "candidate-replay-environment-requested-origin-mismatch",
                ),
                (
                    "seed",
                    {"seed_u64": "42"},
                    "candidate-replay-environment-seed-u64",
                ),
                (
                    "intl",
                    {"intl_surface": "host-icu"},
                    "candidate-replay-environment-intl-surface",
                ),
            )
            for label, mutation, expected in cases:
                with self.subTest(label=label):
                    candidate_value = copy.deepcopy(baseline)
                    environment = candidate_value["replay_environment"]
                    assert isinstance(environment, dict)
                    environment.update(mutation)
                    write_state(candidate_state, candidate_value)
                    result = qualify(item, trace, reference_state, candidate_state)
                    self.assertEqual(result.status, "NOT_COMPARABLE")
                    self.assertIn(expected, result.details)

    def test_state_mismatch_fallback_and_incomplete_resources_are_not_comparable(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            trace = self.make_trace(root)
            item = scenario(trace_digest(trace))
            reference = root / "reference"
            candidate = root / "candidate"
            reference.mkdir()
            candidate.mkdir()
            reference_value = state_for(item, "reference", reference)
            candidate_value = state_for(item, "candidate", candidate)
            candidate_value["title"] = "Different title"
            candidate_value["fallback"] = True
            candidate_value["resources"]["ready"] = False  # type: ignore[index]
            reference_state = reference / "state.json"
            candidate_state = candidate / "state.json"
            write_state(reference_state, reference_value)
            write_state(candidate_state, candidate_value)
            result = qualify(item, trace, reference_state, candidate_state)
            self.assertEqual(result.status, "NOT_COMPARABLE")
            self.assertIn("candidate-fallback", result.details)
            self.assertIn("candidate-resources-incomplete", result.details)
            self.assertIn("state-title-mismatch", result.details)

    def test_missing_artifacts_are_reported_not_silently_skipped(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            trace = self.make_trace(root)
            item = scenario(trace_digest(trace))
            result = qualify(
                item, trace, root / "missing-reference.json", root / "missing-candidate.json"
            )
            self.assertEqual(result.status, "NOT_COMPARABLE")
            self.assertEqual(
                result.details, ("reference-state-missing", "candidate-state-missing")
            )

    def test_trivial_document_requires_recorded_top_bottom_coincidence(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            trace = self.make_trace(root)
            item = scenario(trace_digest(trace), trivial=True)
            reference = root / "reference"
            candidate = root / "candidate"
            reference.mkdir()
            candidate.mkdir()
            reference_value = state_for(item, "reference", reference)
            candidate_value = state_for(item, "candidate", candidate)
            candidate_value["top_bottom_coincident"] = False
            candidate_value["checkpoints"][1]["scroll_y"] = 1  # type: ignore[index]
            reference_state = reference / "state.json"
            candidate_state = candidate / "state.json"
            write_state(reference_state, reference_value)
            write_state(candidate_state, candidate_value)
            result = qualify(item, trace, reference_state, candidate_state)
            self.assertIn("candidate-top-bottom-not-coincident", result.details)
            self.assertIn("candidate-coincidence-not-recorded", result.details)

    def test_candidate_log_records_resource_and_checkpoint_evidence(self) -> None:
        item = scenario("0" * 64, trivial=True)
        log = candidate_evidence_line(item)
        state = candidate_state_from_log(
            item, log, Path("frames"), origin_ms=TRACE_ORIGIN_MS
        )
        self.assertTrue(state["resources"]["ready"])  # type: ignore[index]
        self.assertEqual(state["resources"]["network_completions"], 1)  # type: ignore[index]
        self.assertEqual(state["resources"]["pending"], 0)  # type: ignore[index]
        self.assertEqual(state["replay_ledger"], healthy_replay_ledger())
        self.assertEqual(
            state["replay_environment"]["requested_origin_ms"],  # type: ignore[index]
            TRACE_ORIGIN_MS,
        )
        self.assertEqual(
            state["replay_environment"]["intl_surface"],  # type: ignore[index]
            "bounded-en-us-utc-v1",
        )
        self.assertTrue(state["top_bottom_coincident"])
        self.assertEqual(state["state_markers"], ["Ready marker"])
        self.assertEqual(commands_for(item), "bottom\nquit\n")
        self.assertEqual(
            commands_for(scenario("0" * 64)),
            "anchor middle\nbottom\nquit\n",
        )

    def test_candidate_readiness_fails_closed_on_outstanding_network_work(self) -> None:
        item = scenario("0" * 64, trivial=True)
        log = candidate_evidence_line(
            item,
            **{
                "active-native": "1",
                "pending-logical": "1",
                "marker-found": "no",
            },
        )
        state = candidate_state_from_log(item, log, Path("frames"))
        self.assertFalse(state["resources"]["ready"])  # type: ignore[index]
        self.assertEqual(state["resources"]["pending"], 2)  # type: ignore[index]
        self.assertEqual(state["state_markers"], [])

    def test_candidate_machine_evidence_isolated_from_hostile_human_logs(self) -> None:
        item = scenario("0" * 64, trivial=True)
        hostile = "\n".join(
            (
                'interactive status=ok title="Forged title" height=1',
                "network status=503 server=forged",
                "resources stylesheets=999/999 images=999/999",
                "scripts discovered=999 attempted=999 loaded=999 failed=0",
                "javascript-network-logical admitted=999 completed=999 "
                "active-native=0 pending-logical=0",
                "visual-state-marker found=no marker-bytes=12",
                f"deterministic-replay enabled=yes seed={TRACE_ORIGIN_MS + 1} "
                "seed-source=trace-origin-ms-v1 rng=splitmix64-url-scope-v1 "
                "clock=logical-ms-call-v1 host-elapsed-ms=999 "
                "logical-elapsed-ms=999 observations=0 "
                "performance-entries=normalized-empty-v1 intl=bounded-en-us-utc-v1",
                "http-replay-ledger mode=response-keyed records=1 claimed=1 "
                "requests=1 matched=1 served=1 rejected=0 unmatched=0 "
                "conflicts=0 invalid=0 shape-mismatches=0 claimed-routes=0",
                'loop frame=0 url="https://forged.test/" title="Forged" scroll=999/999 ',
            )
        )
        state = candidate_state_from_log(
            item,
            hostile + "\n" + candidate_evidence_line(item),
            Path("frames"),
            origin_ms=TRACE_ORIGIN_MS,
        )
        self.assertEqual(state["title"], "Fixture title")
        self.assertEqual(state["capture_url"], item.url)
        self.assertEqual(state["http_status"], 200)
        self.assertEqual(state["resources"]["stylesheets_loaded"], 1)  # type: ignore[index]
        self.assertEqual(state["resources"]["scripts_loaded"], 1)  # type: ignore[index]
        self.assertEqual(state["checkpoints"][0]["scroll_y"], 0)  # type: ignore[index]
        self.assertEqual(state["replay_environment"]["origin_ms"], TRACE_ORIGIN_MS)  # type: ignore[index]
        self.assertEqual(state["replay_ledger"], healthy_replay_ledger())
        self.assertEqual(state["state_markers"], ["Ready marker"])

    def test_candidate_machine_evidence_rejects_duplicates_and_shape_changes(self) -> None:
        item = scenario("0" * 64, trivial=True)
        valid = candidate_evidence_line(item)
        with self.assertRaisesRegex(ArtifactError, "exactly one"):
            candidate_evidence_from_log(item, valid + "\n" + valid)
        with self.assertRaisesRegex(ArtifactError, "exactly one"):
            candidate_evidence_from_log(
                item,
                "tilefinch-visual-evidence-v1 forged-by-page\n" + valid,
            )
        tokens = valid.split(" ")
        tokens[2], tokens[3] = tokens[3], tokens[2]
        with self.assertRaisesRegex(ArtifactError, "reordered"):
            candidate_evidence_from_log(item, " ".join(tokens))
        with self.assertRaisesRegex(ArtifactError, "schema is unsupported"):
            candidate_evidence_from_log(
                item, candidate_evidence_line(item, **{"schema": "1"})
            )
        with self.assertRaisesRegex(ArtifactError, "outside its bound"):
            candidate_evidence_from_log(
                item,
                candidate_evidence_line(
                    item, **{"network-completions": "9" * 5000}
                ),
            )
        with self.assertRaisesRegex(ArtifactError, "claimed routes"):
            candidate_evidence_from_log(
                item,
                candidate_evidence_line(
                    item, **{"claimed-routes": "9" * 5000}
                ),
            )
        with self.assertRaisesRegex(ArtifactError, "marker does not match"):
            candidate_evidence_from_log(
                item,
                candidate_evidence_line(
                    item, **{"marker-hex": "Different marker".encode().hex()}
                ),
            )
        with self.assertRaisesRegex(ArtifactError, "occurrence claims"):
            candidate_evidence_from_log(
                item,
                candidate_evidence_line(
                    item,
                    **{
                        "occurrence-claims": "1",
                        "reusable-claims": "3",
                    },
                ),
            )
        with self.assertRaisesRegex(ArtifactError, "exhausted"):
            candidate_evidence_from_log(
                item,
                candidate_evidence_line(item, **{"occurrence-exhausted": "1"}),
            )

    def test_candidate_machine_evidence_rejects_unclassified_selected_failure(
        self,
    ) -> None:
        item = scenario("0" * 64, trivial=True)
        # This is the pre-fix shape of a rank-2 record selected successfully
        # but lost after the full parser: the request grew, yet no terminal
        # matched/unmatched/conflict/invalid bucket did.
        with self.assertRaisesRegex(ArtifactError, "exact terminal accounting"):
            candidate_evidence_from_log(
                item,
                candidate_evidence_line(item, **{"requests": "4"}),
            )

    def test_candidate_machine_evidence_hex_keeps_page_newlines_as_data(self) -> None:
        item = scenario("0" * 64, trivial=True)
        title = "Fixture title\nnetwork status=503"
        body = "Ready marker\nscripts discovered=9 attempted=9 loaded=9"
        state = candidate_state_from_log(
            item,
            candidate_evidence_line(
                item,
                **{
                    "title-hex": title.encode().hex(),
                    "body-preview-hex": body.encode().hex(),
                },
            ),
            Path("frames"),
            origin_ms=TRACE_ORIGIN_MS,
        )
        self.assertEqual(state["title"], title)
        self.assertEqual(state["resources"]["scripts_loaded"], 1)  # type: ignore[index]

    def test_candidate_clock_evidence_uses_dual_domain_source_ceiling(self) -> None:
        item = replace(
            scenario("0" * 64, trivial=True),
            ticks=MAX_REPLAY_TICKS,
            tick_ms=MAX_REPLAY_TICK_MS,
        )
        boundary = candidate_evidence_line(
            item,
            **{
                "host-elapsed-ms": str(MAX_REPLAY_HOST_ELAPSED_MS),
                "wall-elapsed-ms": str(MAX_REPLAY_DOMAIN_ELAPSED_MS),
                "monotonic-elapsed-ms": str(MAX_REPLAY_HOST_ELAPSED_MS),
                "wall-observations": str(MAX_REPLAY_OBSERVATIONS),
                "clock-date-now": str(MAX_REPLAY_OBSERVATIONS),
            },
        )
        evidence = candidate_evidence_from_log(item, boundary)
        self.assertEqual(
            evidence["wall_elapsed_ms"], MAX_REPLAY_DOMAIN_ELAPSED_MS
        )
        stable_sample = candidate_evidence_from_log(
            item,
            candidate_evidence_line(
                item,
                **{
                    "monotonic-samples": "1",
                    "clock-event-timestamp": "1",
                },
            ),
        )
        self.assertEqual(
            stable_sample["monotonic_elapsed_ms"], MAX_REPLAY_HOST_ELAPSED_MS
        )
        cases = (
            (
                "host-elapsed-ms",
                {"host-elapsed-ms": str(MAX_REPLAY_HOST_ELAPSED_MS + 1)},
                "host-elapsed-ms.*bound",
            ),
            (
                "wall-elapsed-ms",
                {
                    "host-elapsed-ms": str(MAX_REPLAY_HOST_ELAPSED_MS),
                    "wall-elapsed-ms": str(
                        MAX_REPLAY_DOMAIN_ELAPSED_MS + 1
                    ),
                    "wall-observations": str(MAX_REPLAY_OBSERVATIONS),
                    "clock-date-now": str(MAX_REPLAY_OBSERVATIONS),
                },
                "wall-elapsed-ms.*bound",
            ),
            (
                "source-observations",
                {
                    "host-elapsed-ms": str(MAX_REPLAY_HOST_ELAPSED_MS),
                    "wall-elapsed-ms": str(MAX_REPLAY_DOMAIN_ELAPSED_MS),
                    "wall-observations": str(MAX_REPLAY_OBSERVATIONS + 1),
                },
                "wall-observations.*bound",
            ),
            (
                "inconsistent",
                {
                    "host-elapsed-ms": str(MAX_REPLAY_HOST_ELAPSED_MS),
                    "wall-elapsed-ms": str(MAX_REPLAY_HOST_ELAPSED_MS + 1),
                },
                "dual-domain clock is inconsistent",
            ),
            (
                "source-partition",
                {"clock-performance-now": "1"},
                "clock source totals are inconsistent",
            ),
            (
                "source-total",
                {
                    "host-elapsed-ms": str(MAX_REPLAY_HOST_ELAPSED_MS),
                    "wall-elapsed-ms": str(MAX_REPLAY_DOMAIN_ELAPSED_MS),
                    "wall-observations": str(MAX_REPLAY_OBSERVATIONS),
                    "clock-date-now": str(MAX_REPLAY_OBSERVATIONS),
                    "monotonic-samples": "1",
                    "clock-event-timestamp": "1",
                },
                "clock source total exceeds",
            ),
        )
        for label, overrides, expected in cases:
            with self.subTest(label=label), self.assertRaisesRegex(
                ArtifactError, expected
            ):
                candidate_evidence_from_log(
                    item, candidate_evidence_line(item, **overrides)
                )

    def test_trace_digest_rejects_symlink_directories(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            trace = self.make_trace(root)
            target = root / "target"
            target.mkdir()
            link = trace / "linked-directory"
            try:
                link.symlink_to(target, target_is_directory=True)
            except (OSError, NotImplementedError) as error:
                self.skipTest(f"directory symlinks unavailable: {error}")
            with self.assertRaisesRegex(ArtifactError, "symlink"):
                trace_digest(trace)

    def test_trace_origin_uses_ecmascript_date_bound(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            trace = self.make_trace(root)
            metadata = trace / "trace.meta"
            original = metadata.read_text(encoding="utf-8")
            metadata.write_text(
                original.replace(str(TRACE_ORIGIN_MS), str(ECMASCRIPT_DATE_MAX_MS)),
                encoding="utf-8",
            )
            self.assertEqual(trace_replay_origin_ms(trace), ECMASCRIPT_DATE_MAX_MS)
            metadata.write_text(
                original.replace(
                    str(TRACE_ORIGIN_MS), str(ECMASCRIPT_DATE_MAX_MS + 1)
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ArtifactError, "bound"):
                trace_replay_origin_ms(trace)

    def test_qualify_only_reconstructs_and_binds_candidate_state(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            item, manifest, output, log_path, candidate_state = (
                self.make_qualify_only_fixture(root)
            )
            proof_root = self.make_reference_proof(root, item)
            verify_candidate_state_binding(
                item,
                root / item.replay_dir,
                candidate_state,
                log_path,
                output / item.scenario / "frames",
            )
            binding = verify_reference_proof_binding(
                item, root / item.replay_dir, proof_root
            )
            self.assertEqual(
                binding.reference_state_path,
                proof_root / "run-a" / item.reference_state,
            )

            completed = self.run_qualify_only(
                root,
                manifest,
                output,
                reference_root=proof_root,
                require_proof=True,
            )

            self.assertEqual(completed.returncode, 0, completed.stderr)
            summary = (output / "summary.tsv").read_text(encoding="utf-8")
            self.assertIn("ELIGIBLE", summary)
            self.assertIn(REFERENCE_PROOF_VERSION, summary)
            self.assertNotIn("candidate-binding-invalid", summary)

    def test_reference_proof_binding_rejects_hostile_retained_artifacts(self) -> None:
        cases = (
            "missing-proof",
            "malformed-proof",
            "duplicate-proof-key",
            "symlink-proof",
            "oversized-proof",
            "missing-state",
            "malformed-state",
            "duplicate-state-key",
            "tampered-state",
            "symlink-state",
            "oversized-state",
            "tampered-frame-a",
            "tampered-frame-b",
            "missing-frame",
            "symlink-frame",
            "oversized-frame",
            "extra-frame",
            "duplicate-frame-entry",
            "replay-environment",
            "synchronized-bad-seed",
            "synchronized-wrong-origin",
            "synchronized-unknown-state-field",
            "synchronized-unknown-ledger-field",
            "synchronized-retained-failure",
            "old-proof-version",
            "old-state-schema",
        )
        for label in cases:
            with self.subTest(label=label), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                trace = self.make_trace(root)
                item = scenario(trace_digest(trace))
                proof_root = self.make_reference_proof(root, item)
                proof_path = proof_root / "determinism-proof.json"
                proof = load_state(proof_path)
                canonical_state = proof_root / "run-a" / item.reference_state
                comparison_state = proof_root / "run-b" / item.reference_state
                canonical_frame = proof_root / "run-a" / item.scenario / "frame-0.png"
                comparison_frame = proof_root / "run-b" / item.scenario / "frame-0.png"
                if label == "missing-proof":
                    proof_path.unlink()
                elif label == "malformed-proof":
                    proof_path.write_text("{", encoding="utf-8")
                elif label == "duplicate-proof-key":
                    raw = proof_path.read_text(encoding="utf-8")
                    proof_path.write_text(
                        '{"version":"duplicate",' + raw.lstrip()[1:],
                        encoding="utf-8",
                    )
                elif label == "symlink-proof":
                    target = root / "proof-target.json"
                    target.write_bytes(proof_path.read_bytes())
                    proof_path.unlink()
                    proof_path.symlink_to(target)
                elif label == "oversized-proof":
                    proof_path.write_bytes(b"x" * (REFERENCE_PROOF_BYTE_LIMIT + 1))
                elif label == "missing-state":
                    canonical_state.unlink()
                elif label == "malformed-state":
                    for state_path in (canonical_state, comparison_state):
                        state_path.write_text("{", encoding="utf-8")
                    state_entry = proof["state"]
                    assert isinstance(state_entry, dict)
                    state_entry["sha256"] = hashlib.sha256(b"{").hexdigest()
                    write_state(proof_path, proof)
                elif label == "duplicate-state-key":
                    raw = canonical_state.read_text(encoding="utf-8")
                    duplicated = '{"scenario":"duplicate",' + raw.lstrip()[1:]
                    for state_path in (canonical_state, comparison_state):
                        state_path.write_text(duplicated, encoding="utf-8")
                    state_entry = proof["state"]
                    assert isinstance(state_entry, dict)
                    state_entry["sha256"] = hashlib.sha256(
                        duplicated.encode("utf-8")
                    ).hexdigest()
                    write_state(proof_path, proof)
                elif label == "tampered-state":
                    value = load_state(canonical_state)
                    value["title"] = "Tampered"
                    write_state(canonical_state, value)
                elif label == "symlink-state":
                    target = root / "state-target.json"
                    target.write_bytes(canonical_state.read_bytes())
                    canonical_state.unlink()
                    canonical_state.symlink_to(target)
                elif label == "oversized-state":
                    canonical_state.write_bytes(b"x" * (REFERENCE_STATE_BYTE_LIMIT + 1))
                elif label == "tampered-frame-a":
                    canonical_frame.write_bytes(b"tampered")
                elif label == "tampered-frame-b":
                    comparison_frame.write_bytes(b"tampered")
                elif label == "missing-frame":
                    canonical_frame.unlink()
                elif label == "symlink-frame":
                    target = root / "frame-target.png"
                    target.write_bytes(canonical_frame.read_bytes())
                    canonical_frame.unlink()
                    canonical_frame.symlink_to(target)
                elif label == "oversized-frame":
                    canonical_frame.write_bytes(b"x" * (REFERENCE_FRAME_BYTE_LIMIT + 1))
                elif label == "extra-frame":
                    (canonical_frame.parent / "extra.png").write_bytes(b"extra")
                elif label == "duplicate-frame-entry":
                    frames = proof["frames"]
                    assert isinstance(frames, list)
                    frames[-1] = copy.deepcopy(frames[0])
                    write_state(proof_path, proof)
                elif label == "replay-environment":
                    environment = proof["replay_environment"]
                    assert isinstance(environment, dict)
                    environment["origin_ms"] = TRACE_ORIGIN_MS + 1
                    write_state(proof_path, proof)
                elif label in {
                    "synchronized-bad-seed",
                    "synchronized-wrong-origin",
                    "synchronized-unknown-state-field",
                    "synchronized-unknown-ledger-field",
                    "synchronized-retained-failure",
                }:
                    values = []
                    for state_path in (canonical_state, comparison_state):
                        value = load_state(state_path)
                        if label == "synchronized-bad-seed":
                            environment = value["replay_environment"]
                            assert isinstance(environment, dict)
                            environment["seed_sha256"] = "0" * 64
                        elif label == "synchronized-wrong-origin":
                            environment = value["replay_environment"]
                            assert isinstance(environment, dict)
                            wrong_origin = TRACE_ORIGIN_MS + 1
                            environment["origin_ms"] = wrong_origin
                            environment["seed_u64"] = str(wrong_origin)
                            environment["seed_sha256"] = replay_seed_sha256(
                                wrong_origin
                            )
                        elif label == "synchronized-unknown-state-field":
                            value["unexamined_extension"] = True
                        elif label == "synchronized-unknown-ledger-field":
                            ledger = value["replay_ledger"]
                            assert isinstance(ledger, dict)
                            ledger["unexamined_extension"] = True
                        else:
                            value["failure"] = "navigation:timeout"
                        write_state(state_path, value)
                        values.append(value)
                    self.assertEqual(
                        canonical_state.read_bytes(), comparison_state.read_bytes()
                    )
                    if label in {
                        "synchronized-bad-seed",
                        "synchronized-wrong-origin",
                    }:
                        proof["replay_environment"] = values[0]["replay_environment"]
                    state_entry = proof["state"]
                    assert isinstance(state_entry, dict)
                    state_entry["sha256"] = hashlib.sha256(
                        canonical_state.read_bytes()
                    ).hexdigest()
                    write_state(proof_path, proof)
                elif label == "old-proof-version":
                    proof["version"] = "two-full-run-byte-equality-v2"
                    write_state(proof_path, proof)
                elif label == "old-state-schema":
                    proof["state_schema"] = 1
                    write_state(proof_path, proof)
                with self.assertRaises(ArtifactError):
                    verify_reference_proof_binding(item, trace, proof_root)

    def test_reference_proof_rejects_synchronized_known_field_mutations(self) -> None:
        cases: tuple[tuple[str, tuple[str, ...], object, str], ...] = (
            (
                "routes",
                ("replay_ledger", "routes"),
                0,
                "reference-replay-ledger-routes-invalid",
            ),
            (
                "shape-comparison",
                ("replay_ledger", "shape_comparison"),
                "collected",
                "reference-replay-ledger-shape-comparison",
            ),
            (
                "claimed-route-ranges",
                ("replay_ledger", "claimed_route_ranges"),
                "0000,0001,0002",
                "reference-replay-ledger-claimed-route-ranges",
            ),
            (
                "unmatched-urls",
                ("replay_ledger", "unmatched_urls"),
                ["GET https://fixture.test/missing"],
                "reference-replay-ledger-unmatched-urls",
            ),
            (
                "empty-diagnostic-digest",
                ("replay_ledger", "unexpected_requests", "multiset_sha256"),
                "0" * 64,
                "reference-replay-ledger-diagnostics-empty-digest-mismatch",
            ),
            (
                "deferred-images",
                ("resources", "deferred_images"),
                -1,
                "reference-resource-deferred-images-invalid",
            ),
            (
                "network-completions",
                ("resources", "network_completions"),
                2,
                "reference-resource-network-ledger-closure",
            ),
            (
                "scheduler-pumps",
                ("response_scheduler", "semantic_pumps"),
                2,
                "reference-response-scheduler-pump-closure",
            ),
            (
                "scheduler-peak",
                ("response_scheduler", "max_pending"),
                0,
                "reference-response-scheduler-peak-closure",
            ),
            (
                "host-operation-ledger",
                ("host_operations", "started"),
                2,
                "reference-host-operations-ledger-closure",
            ),
            (
                "publication-ledger",
                ("publication_boundary", "final_activity"),
                2,
                "reference-publication-ledger-closure",
            ),
            (
                "browser-engine",
                ("browser", "engine"),
                "firefox",
                "reference-browser-engine",
            ),
            (
                "browser-version",
                ("browser", "version"),
                "149.0",
                "reference-browser-version",
            ),
            (
                "browser-platform",
                ("browser", "platform"),
                "",
                "reference-browser-platform",
            ),
        )
        for label, path, replacement, expected in cases:
            with self.subTest(label=label), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                trace = self.make_trace(root)
                item = scenario(trace_digest(trace))
                proof_root = self.make_reference_proof(root, item)
                state_paths = tuple(
                    proof_root / run_name / item.reference_state
                    for run_name in ("run-a", "run-b")
                )
                for state_path in state_paths:
                    state = load_state(state_path)
                    value: object = state
                    for component in path[:-1]:
                        assert isinstance(value, dict)
                        value = value[component]
                    assert isinstance(value, dict)
                    value[path[-1]] = replacement
                    if label == "host-operation-ledger":
                        host_operations = state["host_operations"]
                        assert isinstance(host_operations, dict)
                        host_operations["completed"] = replacement
                    write_state(state_path, state)
                self.assertEqual(state_paths[0].read_bytes(), state_paths[1].read_bytes())
                proof_path = proof_root / "determinism-proof.json"
                proof = load_state(proof_path)
                state_entry = proof["state"]
                assert isinstance(state_entry, dict)
                state_entry["sha256"] = hashlib.sha256(
                    state_paths[0].read_bytes()
                ).hexdigest()
                write_state(proof_path, proof)
                with self.assertRaisesRegex(ArtifactError, expected):
                    verify_reference_proof_binding(item, trace, proof_root)

    def test_reference_proof_png_decode_is_bounded_and_fail_closed(self) -> None:
        def chunk(kind: bytes, payload: bytes) -> bytes:
            checksum = zlib.crc32(kind + payload) & 0xFFFFFFFF
            return (
                struct.pack(">I", len(payload))
                + kind
                + payload
                + struct.pack(">I", checksum)
            )

        def png(width: int, height: int, compressed: bytes) -> bytes:
            header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
            return (
                b"\x89PNG\r\n\x1a\n"
                + chunk(b"IHDR", header)
                + chunk(b"IDAT", compressed)
                + chunk(b"IEND", b"")
            )

        cases = (
            ("malformed-zlib", png(2, 1, b"not-a-zlib-stream"), "invalid compressed"),
            (
                "decompression-bomb",
                png(2, 1, zlib.compress(b"\0" * (1024 * 1024), 9)),
                "decompressed PNG length exceeds",
            ),
            (
                "wrong-geometry-before-zlib",
                png(1_000_000, 1, b"not-a-zlib-stream"),
                "PNG geometry differs",
            ),
        )
        for label, frame_bytes, expected in cases:
            with self.subTest(label=label), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                trace = self.make_trace(root)
                item = scenario(trace_digest(trace))
                proof_root = self.make_reference_proof(root, item)
                frame_name = "frame-0.png"
                for run_name in ("run-a", "run-b"):
                    frame_path = proof_root / run_name / item.scenario / frame_name
                    frame_path.write_bytes(frame_bytes)
                proof_path = proof_root / "determinism-proof.json"
                proof = load_state(proof_path)
                frames = proof["frames"]
                assert isinstance(frames, list)
                entry = next(frame for frame in frames if frame["name"] == frame_name)
                entry["sha256"] = hashlib.sha256(frame_bytes).hexdigest()
                write_state(proof_path, proof)
                with self.assertRaisesRegex(ArtifactError, expected):
                    verify_reference_proof_binding(item, trace, proof_root)

    def test_reference_proof_failure_is_preflighted_in_both_runner_modes(self) -> None:
        for mutation in ("missing", "tampered"):
            for qualify_only in (False, True):
                with self.subTest(
                    mutation=mutation, qualify_only=qualify_only
                ), tempfile.TemporaryDirectory() as directory:
                    root = Path(directory)
                    item, manifest, output, _log_path, candidate_state = (
                        self.make_qualify_only_fixture(root)
                    )
                    proof_root = self.make_reference_proof(root, item)
                    proof_path = proof_root / "determinism-proof.json"
                    if mutation == "missing":
                        proof_path.unlink()
                    else:
                        proof = load_state(proof_path)
                        proof["equivalent"] = False
                        write_state(proof_path, proof)

                    build = root / "build"
                    build.mkdir()
                    launch_marker = root / "candidate-launched"
                    lab = build / "psp-browser-interactive-lab"
                    lab.write_text(
                        "#!/bin/sh\n" + f"touch '{launch_marker}'\n" + "exit 0\n",
                        encoding="utf-8",
                    )
                    lab.chmod(0o700)
                    command = [
                        sys.executable,
                        str(BENCHMARKS / "run-visual-scenarios.py"),
                        str(build),
                        str(root),
                        str(proof_root),
                        str(output),
                        "--manifest",
                        str(manifest),
                        "--require-reference-proof",
                    ]
                    if qualify_only:
                        command.append("--qualify-only")
                    retained_candidate = candidate_state.read_bytes()
                    completed = subprocess.run(command, capture_output=True, text=True)
                    self.assertEqual(completed.returncode, 3, completed.stderr)
                    summary = (output / "summary.tsv").read_text(encoding="utf-8")
                    self.assertIn("NOT_COMPARABLE", summary)
                    self.assertIn("reference-proof-invalid", summary)
                    self.assertIn(f"{REFERENCE_PROOF_VERSION}:invalid", summary)
                    self.assertFalse(launch_marker.exists())
                    self.assertEqual(candidate_state.read_bytes(), retained_candidate)

    def test_unbound_reference_is_diagnostic_only(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _item, manifest, output, _log_path, _candidate_state = (
                self.make_qualify_only_fixture(root)
            )
            completed = self.run_qualify_only(root, manifest, output)
            self.assertEqual(completed.returncode, 3, completed.stderr)
            summary = (output / "summary.tsv").read_text(encoding="utf-8")
            self.assertIn("NOT_COMPARABLE", summary)
            self.assertIn("reference-proof-required", summary)
            self.assertIn("legacy-unbound", summary)

    def test_proof_mode_rejects_ambiguous_multi_scenario_invocation(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            completed = subprocess.run(
                (
                    sys.executable,
                    str(BENCHMARKS / "run-visual-scenarios.py"),
                    str(root / "build"),
                    str(root / "replay"),
                    str(root / "proof"),
                    str(root / "output"),
                    "--manifest",
                    str(BENCHMARKS / "visual-scenarios.tsv"),
                    "--qualify-only",
                    "--require-reference-proof",
                ),
                capture_output=True,
                text=True,
            )
            self.assertEqual(completed.returncode, 2)
            self.assertIn("exactly one selected scenario", completed.stderr)
            self.assertFalse((root / "output").exists())

    def test_qualify_only_rejects_unbound_candidate_artifacts_without_rewrite(
        self,
    ) -> None:
        cases = (
            "missing",
            "symlink",
            "non-regular",
            "oversized",
            "invalid-utf8",
            "duplicate",
            "tampered-record",
            "state-type-mismatch",
        )
        for label in cases:
            with self.subTest(label=label), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                item, manifest, output, log_path, candidate_state = (
                    self.make_qualify_only_fixture(root)
                )
                original_log = log_path.read_text(encoding="utf-8")
                if label == "missing":
                    log_path.unlink()
                elif label == "symlink":
                    target = root / "candidate-log-target"
                    target.write_text(original_log, encoding="utf-8")
                    log_path.unlink()
                    log_path.symlink_to(target)
                elif label == "non-regular":
                    log_path.unlink()
                    log_path.mkdir()
                elif label == "oversized":
                    log_path.write_bytes(b"x" * (CANDIDATE_LOG_BYTE_LIMIT + 1))
                elif label == "invalid-utf8":
                    log_path.write_bytes(b"\xff")
                elif label == "duplicate":
                    log_path.write_text(original_log + original_log, encoding="utf-8")
                elif label == "tampered-record":
                    log_path.write_text(
                        candidate_evidence_line(
                            item,
                            **{"title-hex": "Tampered title".encode().hex()},
                        )
                        + "\n",
                        encoding="utf-8",
                    )
                elif label == "state-type-mismatch":
                    value = load_state(candidate_state)
                    evidence = value["candidate_evidence"]
                    assert isinstance(evidence, dict)
                    evidence["schema"] = True
                    write_state(candidate_state, value)
                retained_before = candidate_state.read_bytes()
                log_before = (
                    log_path.read_bytes()
                    if log_path.is_file() and not log_path.is_symlink()
                    else None
                )

                completed = self.run_qualify_only(root, manifest, output)

                self.assertEqual(completed.returncode, 3, completed.stderr)
                summary = (output / "summary.tsv").read_text(encoding="utf-8")
                self.assertIn("NOT_COMPARABLE", summary)
                self.assertIn("candidate-binding-invalid", summary)
                self.assertEqual(candidate_state.read_bytes(), retained_before)
                if label == "missing":
                    self.assertFalse(log_path.exists())
                elif label == "symlink":
                    self.assertTrue(log_path.is_symlink())
                elif label == "non-regular":
                    self.assertTrue(log_path.is_dir())
                else:
                    self.assertEqual(log_path.read_bytes(), log_before)

    def test_qualify_only_returns_incomplete_status_and_summary(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            trace = self.make_trace(root)
            digest = trace_digest(trace)
            item = scenario(digest)
            manifest = root / "manifest.tsv"
            source = (BENCHMARKS / "visual-scenarios.tsv").read_text().splitlines()[0]
            values = [
                item.scenario,
                item.url,
                item.replay_dir,
                item.trace_sha256,
                "200",
                "Fixture",
                "Ready marker",
                "Fallback shell",
                "Checking browser",
                "2",
                "1",
                "2",
                "1",
                "1",
                "1",
                "top|anchor:middle|bottom",
                item.reference_state,
                "4",
                "1",
                "1",
                "1",
                "1",
                "1",
                "1",
                "1",
                "1",
                "1",
                "1",
                "1",
                "1",
                "0",
            ]
            manifest.write_text(source + "\n" + "\t".join(values) + "\n")
            output = root / "output"
            completed = subprocess.run(
                (
                    sys.executable,
                    str(BENCHMARKS / "run-visual-scenarios.py"),
                    str(root / "build"),
                    str(root),
                    str(root / "references"),
                    str(output),
                    "--manifest",
                    str(manifest),
                    "--qualify-only",
                ),
                capture_output=True,
                text=True,
            )
            self.assertEqual(completed.returncode, 3, completed.stderr)
            summary = (output / "summary.tsv").read_text()
            self.assertIn("NOT_COMPARABLE", summary)
            self.assertIn("reference-state-missing", summary)
            self.assertIn("candidate-state-missing", summary)


if __name__ == "__main__":
    unittest.main()
