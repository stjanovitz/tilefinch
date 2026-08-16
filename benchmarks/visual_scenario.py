#!/usr/bin/env python3
"""Manifest and artifact validation for deterministic visual scenarios.

This module deliberately decides only whether two captures are comparable.  It
does not calculate a visual score.  A fallback page, a different response
state, an incomplete resource set, or a malformed frame must never become a
misleading pixel-difference result.
"""

from __future__ import annotations

import csv
import hashlib
import json
import os
import re
import stat
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Any, Iterable

from reference_frame import read_png_bytes, read_ppm


SCHEMA_VERSION = 2
PENDING_DIGEST = "PENDING"
SHA256 = re.compile(r"[0-9a-f]{64}\Z")
SAFE_NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.-]*\Z")
SAFE_ANCHOR = re.compile(r"[A-Za-z0-9_.:-]+\Z")
REPLAY_LEDGER_PREFIX = "http-replay-ledger "
REPLAY_LEDGER_RECORD_LIMIT = 4096
REPLAY_LEDGER_COUNT_LIMIT = 1_000_000
REPLAY_LEDGER_LINE_LIMIT = 32_768
# Cross-engine visual replay envelope; keep capture-reference.js and the
# interactive lab CLI validation synchronized with these exact ceilings.
MAX_REPLAY_TICKS = 1_000
MAX_REPLAY_TICK_MS = 60_000
MAX_REPLAY_HOST_ELAPSED_MS = MAX_REPLAY_TICKS * MAX_REPLAY_TICK_MS
MAX_REPLAY_OBSERVATIONS = 1_000_000
MAX_REPLAY_DOMAIN_ELAPSED_MS = (
    MAX_REPLAY_HOST_ELAPSED_MS + MAX_REPLAY_OBSERVATIONS
)
REFERENCE_CAPTURE_TRANSPORT = "cdp-response-keyed"
REFERENCE_REPLAY_ENVIRONMENT = "deterministic-hermetic-v3"
REFERENCE_CLOCK_VERSION = "playwright-clock-paused-v2"
CANDIDATE_CLOCK_VERSION = "native-script-runtime-v2"
REPLAY_CLOCK_CONTRACT = "dual-domain-ms-call-v2"
REPLAY_CLOCK_SCOPE = "top-level-realm-v1"
REPLAY_CLOCK_OBSERVATION_SOURCES = (
    "date_now",
    "date_function",
    "date_constructor",
    "performance_now",
    "performance_mark",
    "performance_measure",
    "animation_timeline",
    "idle_deadline_time_remaining",
    "animation_frame",
    "event_timestamp",
    "intersection_observer",
    "idle_callback_start",
)
REPLAY_CLOCK_WALL_SOURCES = REPLAY_CLOCK_OBSERVATION_SOURCES[:3]
REPLAY_CLOCK_MONOTONIC_OBSERVATION_SOURCES = REPLAY_CLOCK_OBSERVATION_SOURCES[3:8]
REPLAY_CLOCK_MONOTONIC_SAMPLE_SOURCES = REPLAY_CLOCK_OBSERVATION_SOURCES[8:]
REPLAY_ENVIRONMENT_COMMON_FIELDS = {
    "version",
    "origin_ms",
    "clock_version",
    "clock_contract",
    "clock_scope",
    "rng_version",
    "seed_source",
    "intl_surface",
    "seed_u64",
    "seed_sha256",
    "ticks",
    "tick_ms",
    "host_elapsed_ms",
    "wall_elapsed_ms",
    "monotonic_elapsed_ms",
    "wall_observations",
    "monotonic_observations",
    "monotonic_samples",
    "clock_sources",
    "performance_entries",
    "document_timeline",
    "animation_frame",
}
REPLAY_RNG_CONTRACT = "splitmix64-url-scope-v1"
REPLAY_SEED_SOURCE = "trace-origin-ms-v1"
REPLAY_INTL_CONTRACT = "bounded-en-us-utc-v1"
REFERENCE_READ_ONLY_POLICY = "get-head-only-v3"
REFERENCE_READ_ONLY_PREFLIGHT_POLICY = "cors-preflight-before-network-v1"
REFERENCE_CAPABILITY_POLICY = "offline-capabilities-v2"
REFERENCE_CAPABILITY_SURFACE_EVIDENCE = {
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
}
REFERENCE_RESPONSE_SCHEDULER = "admission-generations-v7"
REFERENCE_RESPONSE_SCHEDULER_TERMINAL_BOUNDARY = "all-http-terminal-v1"
REFERENCE_RESPONSE_SCHEDULER_FIELDS = {
    "version",
    "terminal_boundary",
    "ordering",
    "admission_ordering",
    "admission_probe",
    "order_digest_version",
    "browser_ordinal_semantics",
    "request_limit",
    "retained_delay_limit_pumps",
    "semantic_pump_limit",
    "pump_work_limit",
    "drive_step_limit",
    "pump_work_units",
    "scheduled_delay_work_units",
    "retained_delay_work_units",
    "terminal_delay_work_units",
    "retained_admissions",
    "terminal_admissions",
    "batches",
    "generations",
    "semantic_pumps",
    "fast_forwarded_pumps",
    "admission_probe_stable_passes",
    "admission_probe_limit",
    "admission_probe_timeout_ms",
    "enqueued",
    "completed",
    "pending",
    "max_pending",
    "overflow",
    "failures",
    "terminal_failures",
    "rejected_after_terminal",
    "exact_order_count",
    "semantic_delivery_count",
    "raw_callback_count",
    "ready",
    "order_sha256",
    "semantic_delivery_order_sha256",
    "raw_callback_arrival_sha256",
}
REFERENCE_SCHEDULER_REQUEST_LIMIT = 16_384
REFERENCE_SCHEDULER_WORK_LIMIT = 16_384_000_000
REFERENCE_SCHEDULER_DRIVE_STEP_LIMIT = REFERENCE_SCHEDULER_REQUEST_LIMIT * 2
REFERENCE_HOST_OPERATIONS = "bounded-host-operations-v1"
REFERENCE_HOST_OPERATION_TIMEOUT_LIMIT_MS = 120_000
REFERENCE_PUBLICATION_BOUNDARY = "close-then-publish-v1"
REFERENCE_ROUTE_SELECTION = "ranked-occurrence-v2"
REFERENCE_DIAGNOSTIC_ENTRY_LIMIT = 128
REFERENCE_DIAGNOSTIC_COUNTER_LIMIT = 65
REFERENCE_DIAGNOSTIC_URL_LIMIT = 2048
REFERENCE_POLICY_DENIAL_LIMIT = 4096
TRACE_META_LIMIT = 1024 * 1024
ECMASCRIPT_DATE_MAX_MS = 8_640_000_000_000_000
TRACE_RECORD = re.compile(r"([0-9]{4})\.meta\Z")
CANDIDATE_EVIDENCE_PREFIX = "tilefinch-visual-evidence"
CANDIDATE_EVIDENCE_VERSION = "tilefinch-visual-evidence-v2"
CANDIDATE_EVIDENCE_LINE_LIMIT = 32_768
CANDIDATE_EVIDENCE_MAX_FRAMES = 64
CANDIDATE_LOG_BYTE_LIMIT = 4 * 1024 * 1024
REFERENCE_STATE_BYTE_LIMIT = 4 * 1024 * 1024
REFERENCE_PROOF_BYTE_LIMIT = 1024 * 1024
REFERENCE_FRAME_BYTE_LIMIT = 16 * 1024 * 1024
REFERENCE_PROOF_VERSION = "two-full-run-byte-equality-v3"
REFERENCE_PROOF_CANONICAL_RUN = "run-a"
REFERENCE_PROOF_COMPARISON_RUN = "run-b"
REFERENCE_BROWSER_VERSION = re.compile(r"[0-9]+(?:\.[0-9]+){3}\Z")
EMPTY_DIAGNOSTIC_MULTISET_SHA256 = hashlib.sha256(
    b"tilefinch-diagnostic-multiset-v1\0" + b"0\0" + b"0" * 64
).hexdigest()
CANDIDATE_EVIDENCE_FIELDS = (
    "schema",
    "title-hex",
    "url-hex",
    "body-preview-hex",
    "http-status",
    "marker-hex",
    "marker-found",
    "marker-bytes",
    "stylesheets-loaded",
    "images-loaded",
    "scripts-loaded",
    "network-completions",
    "active-native",
    "pending-logical",
    "seed",
    "seed-source",
    "rng",
    "clock",
    "clock-scope",
    "host-elapsed-ms",
    "wall-elapsed-ms",
    "monotonic-elapsed-ms",
    "wall-observations",
    "monotonic-observations",
    "monotonic-samples",
    "clock-date-now",
    "clock-date-function",
    "clock-date-constructor",
    "clock-performance-now",
    "clock-performance-mark",
    "clock-performance-measure",
    "clock-animation-timeline",
    "clock-idle-deadline-time-remaining",
    "clock-animation-frame",
    "clock-event-timestamp",
    "clock-intersection-observer",
    "clock-idle-callback-start",
    "performance-entries",
    "intl",
    "frames",
    "scrolls",
    "replay-mode",
    "route-selection",
    "occurrence-claims",
    "reusable-claims",
    "occurrence-exhausted",
    "records",
    "claimed",
    "requests",
    "matched",
    "served",
    "rejected",
    "unmatched",
    "conflicts",
    "invalid",
    "shape-mismatches",
    "claimed-routes",
)
REPLAY_LEDGER_PATTERN = re.compile(
    r"http-replay-ledger mode=(?P<mode>[a-z][a-z-]{0,31}) "
    r"records=(?P<records>[0-9]{1,4}) claimed=(?P<claimed>[0-9]{1,4}) "
    r"requests=(?P<requests>[0-9]{1,7}) matched=(?P<matched>[0-9]{1,7}) "
    r"served=(?P<served>[0-9]{1,7}) rejected=(?P<rejected>[0-9]{1,7}) "
    r"unmatched=(?P<unmatched>[0-9]{1,7}) "
    r"conflicts=(?P<conflicts>[0-9]{1,7}) invalid=(?P<invalid>[0-9]{1,7}) "
    r"shape-mismatches=(?P<shape_mismatches>[0-9]{1,7}) "
    r"claimed-routes=(?P<claimed_routes>none|[0-9]{1,4}(?:-[0-9]{1,4})?"
    r"(?:,[0-9]{1,4}(?:-[0-9]{1,4})?)*)\Z"
)

MANIFEST_FIELDS = (
    "scenario",
    "url",
    "replay_dir",
    "trace_sha256",
    "expected_http",
    "required_title",
    "required_state_marker",
    "fallback_markers",
    "interstitial_markers",
    "device_width",
    "device_height",
    "css_width",
    "css_height",
    "scale_numerator",
    "scale_denominator",
    "checkpoints",
    "reference_state",
    "limit_mb",
    "ticks",
    "tick_ms",
    "max_download_kb",
    "script_timeout_ms",
    "script_heap_mb",
    "script_total_mb",
    "script_file_kb",
    "script_count",
    "min_stylesheets_loaded",
    "min_images_loaded",
    "min_scripts_loaded",
    "min_network_completions",
    "max_pending",
)

RESOURCE_FIELDS = (
    "stylesheets_loaded",
    "images_loaded",
    "scripts_loaded",
    "network_completions",
    "pending",
)

# Schema 2 reference evidence is a security boundary, not an extensible API.
# Freeze every stable object published by capture-reference.js so two equally
# corrupted runs cannot mint a proof for fields the qualifier never examined.
# response_scheduler is deliberately not duplicated in the stable-subobject
# sets here: its exact v7 fields and closure are validated as one versioned
# unit in _validate_reference_environment below.
REFERENCE_STATE_FIELDS = {
    "schema",
    "scenario",
    "trace_sha256",
    "url",
    "capture_url",
    "capture_transport",
    "http_status",
    "title",
    "state_markers",
    "fallback",
    "interstitial",
    "top_bottom_coincident",
    "viewport",
    "resources",
    "replay_ledger",
    "replay_environment",
    "read_only_policy",
    "offline_capability_policy",
    "response_scheduler",
    "host_operations",
    "publication_boundary",
    "acquisition_plan",
    "browser",
    "checkpoints",
    "capture_ready",
    "failure",
    "eligibility_reasons",
}
REFERENCE_RESOURCE_FIELDS = {
    "ready",
    "stylesheets_loaded",
    "images_loaded",
    "scripts_loaded",
    "network_completions",
    "deferred_images",
    "pending",
}
REFERENCE_REPLAY_LEDGER_FIELDS = {
    "mode",
    "records",
    "routes",
    "claimed",
    "requests",
    "scheduled",
    "matched",
    "served",
    "rejected",
    "unmatched",
    "conflicts",
    "invalid",
    "route_selection_version",
    "occurrence_claims",
    "reusable_claims",
    "occurrence_exhausted",
    "shape_mismatches",
    "shape_comparison",
    "claimed_routes",
    "claimed_route_ranges",
    "unmatched_urls",
    "unexpected_requests",
}
REFERENCE_DIAGNOSTIC_FIELDS = {
    "total",
    "retained",
    "truncated",
    "overflow",
    "multiset_sha256",
    "by_method",
    "by_resource_type",
    "by_origin",
    "entries",
}
REFERENCE_DIAGNOSTIC_ENTRY_FIELDS = {
    "classification",
    "method",
    "resource_type",
    "url",
    "url_sha256",
    "url_truncated",
}
REFERENCE_CAPABILITY_POLICY_FIELDS = {
    "version",
    "ready",
    "worker_realms",
    "shared_worker_realms",
    "worklet_realms",
    "shadow_realms",
    "service_workers",
    "delayed_fetch",
    "randomized_webcrypto",
    "surface_evidence",
    "diagnostics",
}
REFERENCE_PUBLICATION_FIELDS = {
    "version",
    "ready",
    "context_closed",
    "browser_closed",
    "final_activity",
    "late_callbacks",
    "teardown_changes",
}
REFERENCE_ACQUISITION_FIELDS = {
    "mode",
    "complete",
    "request_count",
    "unplannable",
    "requests",
}
REFERENCE_BROWSER_FIELDS = {
    "engine",
    "version",
    "user_agent",
    "platform",
    "locale",
    "timezone",
}
REFERENCE_CHECKPOINT_FIELDS = {
    "name",
    "kind",
    "target",
    "scroll_y",
    "frame",
    "format",
    "width",
    "height",
}

REPLAY_LEDGER_COUNT_FIELDS = (
    "records",
    "claimed",
    "requests",
    "matched",
    "served",
    "rejected",
    "unmatched",
    "conflicts",
    "invalid",
    "shape_mismatches",
)


class ManifestError(ValueError):
    """A checked-in scenario contract is invalid."""


class ArtifactError(ValueError):
    """An artifact exists but cannot be trusted as scenario evidence."""


@dataclass(frozen=True)
class Checkpoint:
    name: str
    kind: str
    target: str | None = None


@dataclass(frozen=True)
class Scenario:
    scenario: str
    url: str
    replay_dir: str
    trace_sha256: str
    expected_http: int
    required_title: str
    required_state_marker: str
    fallback_markers: tuple[str, ...]
    interstitial_markers: tuple[str, ...]
    device_width: int
    device_height: int
    css_width: int
    css_height: int
    scale_numerator: int
    scale_denominator: int
    checkpoints: tuple[Checkpoint, ...]
    reference_state: str
    limit_mb: int
    ticks: int
    tick_ms: int
    max_download_kb: int
    script_timeout_ms: int
    script_heap_mb: int
    script_total_mb: int
    script_file_kb: int
    script_count: int
    min_stylesheets_loaded: int
    min_images_loaded: int
    min_scripts_loaded: int
    min_network_completions: int
    max_pending: int

    @property
    def viewport(self) -> dict[str, Any]:
        return {
            "device": {"width": self.device_width, "height": self.device_height},
            "css": {"width": self.css_width, "height": self.css_height},
            "scale": {
                "numerator": self.scale_numerator,
                "denominator": self.scale_denominator,
            },
        }

    @property
    def resource_minimums(self) -> dict[str, int]:
        return {
            "stylesheets_loaded": self.min_stylesheets_loaded,
            "images_loaded": self.min_images_loaded,
            "scripts_loaded": self.min_scripts_loaded,
            "network_completions": self.min_network_completions,
        }

    @property
    def trivial_viewport(self) -> bool:
        return len(self.checkpoints) == 2


@dataclass(frozen=True)
class Eligibility:
    scenario: str
    status: str
    details: tuple[str, ...]
    trace_sha256: str
    checkpoint_count: int
    reference_proof: str = "legacy-unbound"


@dataclass(frozen=True)
class ReferenceProofBinding:
    reference_state_path: Path
    proof_root: Path
    contract: str


def _positive(
    row: dict[str, str],
    name: str,
    *,
    allow_zero: bool = False,
    minimum: int | None = None,
    maximum: int | None = None,
) -> int:
    value = row.get(name, "")
    try:
        parsed = int(value, 10)
    except ValueError as error:
        raise ManifestError(f"{name} must be an integer, got {value!r}") from error
    lower_bound = minimum if minimum is not None else (0 if allow_zero else 1)
    if parsed < lower_bound:
        raise ManifestError(f"{name} must be >= {lower_bound}, got {parsed}")
    if maximum is not None and parsed > maximum:
        raise ManifestError(f"{name} must be <= {maximum}, got {parsed}")
    return parsed


def _markers(value: str) -> tuple[str, ...]:
    if value == "-":
        return ()
    markers = tuple(item.strip() for item in value.split("||"))
    if not markers or any(not item for item in markers):
        raise ManifestError("marker lists use non-empty values separated by ||")
    return markers


def _safe_relative(value: str, name: str) -> str:
    path = PurePosixPath(value)
    if value in ("", "-") or path.is_absolute() or ".." in path.parts:
        raise ManifestError(f"{name} must be a safe relative path")
    return value


def parse_checkpoints(value: str) -> tuple[Checkpoint, ...]:
    raw = value.split("|")
    if raw == ["top", "-", "bottom"]:
        return (Checkpoint("top", "top"), Checkpoint("bottom", "bottom"))
    if len(raw) < 3 or raw[0] != "top" or raw[-1] != "bottom":
        raise ManifestError(
            "checkpoints must start with top, end with bottom, and include "
            "at least one semantic anchor"
        )
    checkpoints: list[Checkpoint] = [Checkpoint("top", "top")]
    seen = {"top", "bottom"}
    for item in raw[1:-1]:
        kind, separator, target = item.partition(":")
        if kind not in ("anchor", "selector", "text") or not separator or not target:
            raise ManifestError(f"unsupported semantic checkpoint {item!r}")
        if kind == "anchor" and not SAFE_ANCHOR.fullmatch(target):
            raise ManifestError(f"invalid fragment anchor {target!r}")
        name = f"{kind}-{len(checkpoints)}"
        if name in seen:
            raise ManifestError(f"duplicate checkpoint {name!r}")
        seen.add(name)
        checkpoints.append(Checkpoint(name, kind, target))
    checkpoints.append(Checkpoint("bottom", "bottom"))
    return tuple(checkpoints)


def parse_scenario(row: dict[str, str], line: int) -> Scenario:
    name = row.get("scenario", "")
    if not SAFE_NAME.fullmatch(name):
        raise ManifestError(f"line {line}: invalid scenario name {name!r}")
    digest = row.get("trace_sha256", "")
    if digest != PENDING_DIGEST and not SHA256.fullmatch(digest):
        raise ManifestError(
            f"line {line}: trace_sha256 must be 64 lowercase hex digits or PENDING"
        )
    url = row.get("url", "")
    if not (url.startswith("https://") or url.startswith("http://")):
        raise ManifestError(f"line {line}: URL must be absolute HTTP(S)")
    title = row.get("required_title", "")
    marker = row.get("required_state_marker", "")
    if not title or not marker or title == "-" or marker == "-":
        raise ManifestError(f"line {line}: title and state marker are required")
    try:
        return Scenario(
            scenario=name,
            url=url,
            replay_dir=_safe_relative(row.get("replay_dir", ""), "replay_dir"),
            trace_sha256=digest,
            expected_http=_positive(
                row, "expected_http", minimum=100, maximum=599
            ),
            required_title=title,
            required_state_marker=marker,
            fallback_markers=_markers(row.get("fallback_markers", "-")),
            interstitial_markers=_markers(row.get("interstitial_markers", "-")),
            device_width=_positive(row, "device_width"),
            device_height=_positive(row, "device_height"),
            css_width=_positive(row, "css_width"),
            css_height=_positive(row, "css_height"),
            scale_numerator=_positive(row, "scale_numerator"),
            scale_denominator=_positive(row, "scale_denominator"),
            checkpoints=parse_checkpoints(row.get("checkpoints", "")),
            reference_state=_safe_relative(
                row.get("reference_state", ""), "reference_state"
            ),
            limit_mb=_positive(row, "limit_mb", minimum=4, maximum=512),
            ticks=_positive(
                row, "ticks", allow_zero=True, maximum=MAX_REPLAY_TICKS
            ),
            tick_ms=_positive(row, "tick_ms", maximum=MAX_REPLAY_TICK_MS),
            max_download_kb=_positive(row, "max_download_kb", maximum=65_536),
            script_timeout_ms=_positive(
                row, "script_timeout_ms", maximum=300_000
            ),
            script_heap_mb=_positive(row, "script_heap_mb", maximum=256),
            script_total_mb=_positive(row, "script_total_mb", maximum=128),
            script_file_kb=_positive(row, "script_file_kb", maximum=8_192),
            script_count=_positive(row, "script_count", maximum=256),
            min_stylesheets_loaded=_positive(
                row, "min_stylesheets_loaded", allow_zero=True
            ),
            min_images_loaded=_positive(row, "min_images_loaded", allow_zero=True),
            min_scripts_loaded=_positive(
                row, "min_scripts_loaded", allow_zero=True
            ),
            min_network_completions=_positive(
                row, "min_network_completions", allow_zero=True
            ),
            max_pending=_positive(row, "max_pending", allow_zero=True),
        )
    except ManifestError as error:
        raise ManifestError(f"line {line}: {error}") from error


def load_manifest(path: Path) -> list[Scenario]:
    try:
        with path.open(newline="", encoding="utf-8") as source:
            reader = csv.DictReader(
                (line for line in source if not line.startswith("#")), delimiter="\t"
            )
            if reader.fieldnames is None:
                raise ManifestError("manifest has no header")
            missing = [name for name in MANIFEST_FIELDS if name not in reader.fieldnames]
            optional = ("blocked_origins", "engine_scripts")
            extra = [
                name for name in reader.fieldnames
                if name not in MANIFEST_FIELDS and name not in optional
            ]
            if missing or extra:
                raise ManifestError(
                    f"manifest columns differ: missing={missing} extra={extra}"
                )
            scenarios = [parse_scenario(row, index + 2) for index, row in enumerate(reader)]
    except OSError as error:
        raise ManifestError(str(error)) from error
    if not scenarios:
        raise ManifestError("manifest has no scenario rows")
    names = [scenario.scenario for scenario in scenarios]
    if len(set(names)) != len(names):
        raise ManifestError("scenario names must be unique")
    return scenarios


def trace_digest(directory: Path) -> str:
    """Hash a replay directory including names, lengths, and exact file bytes."""

    if directory.is_symlink():
        raise ArtifactError(f"{directory}: replay trace directory is a symlink")
    root = directory.resolve(strict=True)
    if not root.is_dir():
        raise ArtifactError(f"{directory}: replay trace is not a directory")
    files: list[Path] = []
    for current, directories, names in os.walk(root, followlinks=False):
        directories.sort()
        for name in directories:
            child = Path(current, name)
            try:
                mode = child.lstat().st_mode
            except OSError as error:
                raise ArtifactError(f"{child}: cannot inspect replay trace entry") from error
            if stat.S_ISLNK(mode) or not stat.S_ISDIR(mode):
                raise ArtifactError(
                    f"{child}: replay trace contains a symlink or non-directory entry"
                )
        for name in sorted(names):
            path = Path(current, name)
            try:
                mode = path.lstat().st_mode
            except OSError as error:
                raise ArtifactError(f"{path}: cannot inspect replay trace entry") from error
            if stat.S_ISLNK(mode) or not stat.S_ISREG(mode):
                raise ArtifactError(f"{path}: replay trace contains a non-regular file")
            files.append(path)
    if not files or not (root / "trace.meta").is_file():
        raise ArtifactError(f"{directory}: replay trace is incomplete")
    digest = hashlib.sha256()
    digest.update(b"tilefinch-http-trace-v1\0")
    for path in files:
        relative = path.relative_to(root).as_posix().encode("utf-8")
        size = path.stat().st_size
        digest.update(len(relative).to_bytes(4, "big"))
        digest.update(relative)
        digest.update(size.to_bytes(8, "big"))
        with path.open("rb") as source:
            while chunk := source.read(1024 * 1024):
                digest.update(chunk)
    return digest.hexdigest()


def trace_replay_origin_ms(directory: Path) -> int:
    """Return the exact stable replay seed from a complete retained trace."""

    root = directory.resolve(strict=True)
    metadata_path = root / "trace.meta"
    if metadata_path.is_symlink() or not metadata_path.is_file():
        raise ArtifactError(f"{directory}: replay trace metadata is missing")
    if metadata_path.stat().st_size > TRACE_META_LIMIT:
        raise ArtifactError(f"{metadata_path}: trace metadata exceeds its bound")
    metadata: dict[str, str] = {}
    for number, line in enumerate(
        metadata_path.read_text(encoding="utf-8").splitlines(), 1
    ):
        if not line or line.startswith("#"):
            continue
        key, separator, value = line.partition("=")
        if not separator or not key or key in metadata:
            raise ArtifactError(f"{metadata_path}:{number}: invalid or duplicate metadata")
        metadata[key] = value
    if metadata.get("capture-complete") != "yes":
        raise ArtifactError(f"{metadata_path}: capture-complete must be exactly yes")
    if metadata.get("psp-http-trace-clock") != "1":
        raise ArtifactError(f"{metadata_path}: replay clock version is missing")
    count_text = metadata.get("record-count", "")
    origin_text = metadata.get("origin-ms", "")
    if not re.fullmatch(r"[1-9][0-9]*", count_text):
        raise ArtifactError(f"{metadata_path}: record-count is invalid")
    if not re.fullmatch(r"[1-9][0-9]*", origin_text):
        raise ArtifactError(f"{metadata_path}: origin-ms is invalid")
    count = int(count_text, 10)
    origin = int(origin_text, 10)
    if count > REPLAY_LEDGER_RECORD_LIMIT or origin > ECMASCRIPT_DATE_MAX_MS:
        raise ArtifactError(f"{metadata_path}: retained trace bound exceeded")
    records = sorted(
        name for name in os.listdir(root) if TRACE_RECORD.fullmatch(name)
    )
    expected = [f"{index:04d}.meta" for index in range(count)]
    if records != expected:
        raise ArtifactError(
            f"{metadata_path}: record-count does not match the exact retained sequence"
        )
    return origin


def replay_seed_sha256(origin_ms: int) -> str:
    material = (
        f"{REPLAY_RNG_CONTRACT}\0{REPLAY_SEED_SOURCE}\0{origin_ms}"
    ).encode("utf-8")
    return hashlib.sha256(material).hexdigest()


def _read_bounded_regular(path: Path, byte_limit: int, label: str) -> bytes:
    """Read one stable regular file without following its final path component."""

    try:
        path_status = path.lstat()
    except FileNotFoundError as error:
        raise ArtifactError(f"{path}: {label} is missing") from error
    except OSError as error:
        raise ArtifactError(f"{path}: {label} cannot be inspected") from error
    if stat.S_ISLNK(path_status.st_mode):
        raise ArtifactError(f"{path}: {label} must not be a symlink")
    if not stat.S_ISREG(path_status.st_mode):
        raise ArtifactError(f"{path}: {label} must be a regular file")
    if path_status.st_size > byte_limit:
        raise ArtifactError(f"{path}: {label} exceeds its byte bound")

    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0)
    flags |= getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(path, flags)
    except OSError as error:
        raise ArtifactError(f"{path}: {label} cannot be opened safely") from error
    try:
        opened_status = os.fstat(descriptor)
        if (
            not stat.S_ISREG(opened_status.st_mode)
            or opened_status.st_dev != path_status.st_dev
            or opened_status.st_ino != path_status.st_ino
        ):
            raise ArtifactError(f"{path}: {label} changed before it was opened")
        if opened_status.st_size > byte_limit:
            raise ArtifactError(f"{path}: {label} exceeds its byte bound")

        chunks: list[bytes] = []
        remaining = byte_limit + 1
        while remaining:
            chunk = os.read(descriptor, min(64 * 1024, remaining))
            if not chunk:
                break
            chunks.append(chunk)
            remaining -= len(chunk)
        raw = b"".join(chunks)
        final_status = os.fstat(descriptor)
        try:
            final_path_status = path.lstat()
        except OSError as error:
            raise ArtifactError(f"{path}: {label} changed while read") from error
        stable_identity = (
            final_path_status.st_dev == opened_status.st_dev
            and final_path_status.st_ino == opened_status.st_ino
            and not stat.S_ISLNK(final_path_status.st_mode)
            and stat.S_ISREG(final_path_status.st_mode)
        )
        stable_contents = (
            final_status.st_size == opened_status.st_size == len(raw)
            and final_status.st_mtime_ns == opened_status.st_mtime_ns
            and final_status.st_ctime_ns == opened_status.st_ctime_ns
        )
        if not stable_identity or not stable_contents:
            raise ArtifactError(f"{path}: {label} changed while read")
    finally:
        os.close(descriptor)
    if len(raw) > byte_limit:
        raise ArtifactError(f"{path}: {label} exceeds its byte bound")
    return raw


def _reject_duplicate_json_pairs(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    value: dict[str, Any] = {}
    for key, item in pairs:
        if key in value:
            raise ArtifactError(f"duplicate JSON key {key!r}")
        value[key] = item
    return value


def _decode_json_object(raw: bytes, path: Path, label: str) -> dict[str, Any]:
    try:
        text = raw.decode("utf-8", errors="strict")
        value = json.loads(text, object_pairs_hook=_reject_duplicate_json_pairs)
    except (UnicodeDecodeError, json.JSONDecodeError, ArtifactError) as error:
        raise ArtifactError(f"{path}: invalid {label} JSON: {error}") from error
    if not isinstance(value, dict):
        raise ArtifactError(f"{path}: {label} root must be an object")
    return value


def _load_bounded_json(path: Path, byte_limit: int, label: str) -> dict[str, Any]:
    raw = _read_bounded_regular(path, byte_limit, label)
    return _decode_json_object(raw, path, label)


def load_state(path: Path) -> dict[str, Any]:
    try:
        value = _load_bounded_json(path, REFERENCE_STATE_BYTE_LIMIT, "state")
    except ArtifactError:
        raise
    if not isinstance(value, dict):
        raise ArtifactError(f"{path}: state root must be an object")
    return value


def write_state(path: Path, state: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(state, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def _state_scalar(state: dict[str, Any], name: str, kind: type) -> Any:
    value = state.get(name)
    if not isinstance(value, kind) or (kind is int and isinstance(value, bool)):
        raise ArtifactError(f"state field {name!r} must be {kind.__name__}")
    return value


def _validate_resources(scenario: Scenario, state: dict[str, Any], side: str) -> list[str]:
    resources = state.get("resources")
    if not isinstance(resources, dict):
        return [f"{side}-resource-state-missing"]
    reasons: list[str] = []
    if resources.get("ready") is not True:
        reasons.append(f"{side}-resources-incomplete")
    for name in RESOURCE_FIELDS:
        value = resources.get(name)
        if not isinstance(value, int) or isinstance(value, bool) or value < 0:
            reasons.append(f"{side}-{name}-missing")
    for name, minimum in scenario.resource_minimums.items():
        value = resources.get(name)
        if isinstance(value, int) and not isinstance(value, bool) and value < minimum:
            reasons.append(f"{side}-{name}-below-{minimum}")
    pending = resources.get("pending")
    if isinstance(pending, int) and not isinstance(pending, bool):
        if pending > scenario.max_pending:
            reasons.append(f"{side}-pending-above-{scenario.max_pending}")
    return reasons


def _valid_replay_ledger_counts(ledger: dict[str, Any]) -> bool:
    for name in REPLAY_LEDGER_COUNT_FIELDS:
        value = ledger.get(name)
        maximum = (
            REPLAY_LEDGER_RECORD_LIMIT
            if name in ("records", "claimed")
            else REPLAY_LEDGER_COUNT_LIMIT
        )
        if (
            not isinstance(value, int)
            or isinstance(value, bool)
            or value < 0
            or value > maximum
        ):
            return False
    records = ledger["records"]
    claimed = ledger["claimed"]
    requests = ledger["requests"]
    matched = ledger["matched"]
    served = ledger["served"]
    rejected = ledger["rejected"]
    shape_mismatches = ledger["shape_mismatches"]
    return (
        claimed <= records
        and claimed <= matched
        and matched <= requests
        and served + rejected <= matched
        and shape_mismatches <= requests
    )


def _valid_claimed_routes(ledger: dict[str, Any]) -> bool:
    routes = ledger.get("claimed_routes")
    if not isinstance(routes, list) or len(routes) > REPLAY_LEDGER_RECORD_LIMIT:
        return False
    previous = -1
    for route in routes:
        if (
            not isinstance(route, int)
            or isinstance(route, bool)
            or route <= previous
            or route < 0
            or route >= ledger["records"]
        ):
            return False
        previous = route
    return len(routes) == ledger["claimed"]


def _compact_route_ranges(routes: list[int]) -> str:
    ranges: list[str] = []
    index = 0
    while index < len(routes):
        start = routes[index]
        end = start
        while index + 1 < len(routes) and routes[index + 1] == end + 1:
            index += 1
            end = routes[index]
        ranges.append(
            f"{start:04d}" if start == end else f"{start:04d}-{end:04d}"
        )
        index += 1
    return ",".join(ranges)


def _validate_replay_ledger(
    state: dict[str, Any], side: str, expected_mode: str
) -> list[str]:
    ledger = state.get("replay_ledger")
    if ledger is None:
        return [f"{side}-replay-ledger-missing"]
    if not isinstance(ledger, dict) or not _valid_replay_ledger_counts(ledger):
        return [f"{side}-replay-ledger-invalid"]
    reasons: list[str] = []
    if ledger.get("mode") != expected_mode:
        reasons.append(f"{side}-replay-ledger-mode")
    if not _valid_claimed_routes(ledger):
        reasons.append(f"{side}-replay-ledger-claimed-routes-invalid")
    if ledger["matched"] == 0 or ledger["served"] == 0:
        reasons.append(f"{side}-replay-ledger-no-served-response")
    if ledger["matched"] != ledger["served"] + ledger["rejected"]:
        reasons.append(f"{side}-replay-ledger-match-serve-mismatch")
    if ledger["requests"] != (
        ledger["matched"]
        + ledger["unmatched"]
        + ledger["conflicts"]
        + ledger["invalid"]
    ):
        reasons.append(f"{side}-replay-ledger-request-terminal-mismatch")
    if side == "reference":
        routes = ledger.get("routes")
        if (
            not _bounded_nonnegative(routes, REPLAY_LEDGER_RECORD_LIMIT)
            or routes == 0
            or routes > ledger["records"]
        ):
            reasons.append("reference-replay-ledger-routes-invalid")
        if (
            ledger.get("shape_mismatches") != 0
            or ledger.get("shape_comparison") != "not-collected"
        ):
            reasons.append("reference-replay-ledger-shape-comparison")
        claimed_routes = ledger.get("claimed_routes")
        if isinstance(claimed_routes, list) and _valid_claimed_routes(ledger):
            if ledger.get("claimed_route_ranges") != _compact_route_ranges(
                claimed_routes
            ):
                reasons.append("reference-replay-ledger-claimed-route-ranges")
        unmatched_urls = ledger.get("unmatched_urls")
        unexpected = ledger.get("unexpected_requests")
        expected_unmatched_urls = (
            [
                f"{entry.get('method')} {entry.get('url')}"
                for entry in unexpected.get("entries", [])
                if isinstance(entry, dict)
                and entry.get("classification") == "unmatched"
            ][:16]
            if isinstance(unexpected, dict)
            and isinstance(unexpected.get("entries"), list)
            else None
        )
        if (
            not isinstance(unmatched_urls, list)
            or any(not isinstance(value, str) for value in unmatched_urls)
            or unmatched_urls != expected_unmatched_urls
        ):
            reasons.append("reference-replay-ledger-unmatched-urls")
        scheduled = ledger.get("scheduled")
        if not _bounded_nonnegative(scheduled, REPLAY_LEDGER_COUNT_LIMIT):
            reasons.append("reference-replay-ledger-scheduled-invalid")
        else:
            if ledger["requests"] != scheduled:
                reasons.append("reference-replay-ledger-request-schedule-mismatch")
            if scheduled != ledger["matched"]:
                reasons.append("reference-replay-ledger-schedule-match-mismatch")
        occurrence_claims = ledger.get("occurrence_claims")
        reusable_claims = ledger.get("reusable_claims")
        occurrence_exhausted = ledger.get("occurrence_exhausted")
        if ledger.get("route_selection_version") != REFERENCE_ROUTE_SELECTION:
            reasons.append("reference-replay-ledger-route-selection-version")
        if not all(
            _bounded_nonnegative(value, REPLAY_LEDGER_COUNT_LIMIT)
            for value in (occurrence_claims, reusable_claims, occurrence_exhausted)
        ):
            reasons.append("reference-replay-ledger-occurrence-counts-invalid")
        elif (
            occurrence_claims + reusable_claims != ledger["matched"]
            or occurrence_exhausted != 0
            or (ledger["matched"] > 0 and ledger["claimed"] == 0)
            or occurrence_claims + (1 if reusable_claims > 0 else 0)
            > ledger["claimed"]
            or ledger["claimed"] > occurrence_claims + reusable_claims
        ):
            reasons.append("reference-replay-ledger-occurrence-closure")
        if (
            _bounded_nonnegative(routes, REPLAY_LEDGER_RECORD_LIMIT)
            and _bounded_nonnegative(
                occurrence_claims, REPLAY_LEDGER_COUNT_LIMIT
            )
        ):
            # Every reusable route contributes exactly one uniquely claimed
            # record.  All occurrence claims may share one sequence route, so
            # this is the strongest route-count lower bound the retained
            # aggregate can prove without reconstructing route identities.
            minimum_routes = ledger["claimed"] - occurrence_claims
            if occurrence_claims > 0:
                minimum_routes += 1
            if (
                routes < minimum_routes
                and "reference-replay-ledger-routes-invalid" not in reasons
            ):
                reasons.append("reference-replay-ledger-routes-invalid")
    elif side == "candidate":
        occurrence_claims = ledger.get("occurrence_claims")
        reusable_claims = ledger.get("reusable_claims")
        occurrence_exhausted = ledger.get("occurrence_exhausted")
        if ledger.get("route_selection_version") != REFERENCE_ROUTE_SELECTION:
            reasons.append("candidate-replay-ledger-route-selection-version")
        if not all(
            _bounded_nonnegative(value, REPLAY_LEDGER_COUNT_LIMIT)
            for value in (occurrence_claims, reusable_claims, occurrence_exhausted)
        ):
            reasons.append("candidate-replay-ledger-occurrence-counts-invalid")
        elif (
            occurrence_claims + reusable_claims != ledger["matched"]
            or occurrence_exhausted != 0
            or (ledger["matched"] > 0 and ledger["claimed"] == 0)
            or occurrence_claims + (1 if reusable_claims > 0 else 0)
            > ledger["claimed"]
            or ledger["claimed"] > occurrence_claims + reusable_claims
        ):
            reasons.append("candidate-replay-ledger-occurrence-closure")
    if ledger["unmatched"] != 0:
        reasons.append(f"{side}-replay-ledger-unmatched")
    if ledger["conflicts"] != 0:
        reasons.append(f"{side}-replay-ledger-conflicts")
    if ledger["invalid"] != 0:
        reasons.append(f"{side}-replay-ledger-invalid-routes")
    return reasons


def _bounded_nonnegative(value: Any, maximum: int) -> bool:
    return (
        isinstance(value, int)
        and not isinstance(value, bool)
        and 0 <= value <= maximum
    )


def _valid_diagnostic_counts(value: Any, total: int) -> bool:
    if not isinstance(value, dict) or len(value) > REFERENCE_DIAGNOSTIC_COUNTER_LIMIT:
        return False
    count = 0
    for key, item in value.items():
        if (
            not isinstance(key, str)
            or not key
            or len(key) > 256
            or not _bounded_nonnegative(item, REPLAY_LEDGER_COUNT_LIMIT)
        ):
            return False
        count += item
    return count == total


def _validate_request_diagnostics(
    value: Any,
    expected_total: int,
    *,
    policy: bool = False,
    capability: bool = False,
) -> list[str]:
    prefix = (
        "reference-capability-policy"
        if capability
        else ("reference-read-only-policy" if policy else "reference-replay-ledger")
    )
    if not isinstance(value, dict):
        return [f"{prefix}-diagnostics-missing"]
    total = value.get("total")
    retained = value.get("retained")
    truncated = value.get("truncated")
    overflow = value.get("overflow")
    if not all(
        _bounded_nonnegative(item, REPLAY_LEDGER_COUNT_LIMIT)
        for item in (total, retained, truncated)
    ):
        return [f"{prefix}-diagnostics-invalid"]
    reasons: list[str] = []
    if total != expected_total:
        reasons.append(f"{prefix}-diagnostics-count-mismatch")
    if retained > REFERENCE_DIAGNOSTIC_ENTRY_LIMIT or retained > total:
        reasons.append(f"{prefix}-diagnostics-retained-invalid")
    if truncated != total - retained:
        reasons.append(f"{prefix}-diagnostics-truncation-mismatch")
    if overflow is not False:
        reasons.append(f"{prefix}-diagnostics-overflow")
    multiset_sha256 = value.get("multiset_sha256")
    if not isinstance(multiset_sha256, str) or not SHA256.fullmatch(
        multiset_sha256
    ):
        reasons.append(f"{prefix}-diagnostics-digest-invalid")
    elif total == 0 and multiset_sha256 != EMPTY_DIAGNOSTIC_MULTISET_SHA256:
        reasons.append(f"{prefix}-diagnostics-empty-digest-mismatch")
    for field in ("by_method", "by_resource_type", "by_origin"):
        if not _valid_diagnostic_counts(value.get(field), total):
            reasons.append(f"{prefix}-diagnostics-{field.replace('_', '-')}-invalid")
    entries = value.get("entries")
    if not isinstance(entries, list) or len(entries) != retained:
        reasons.append(f"{prefix}-diagnostics-entries-invalid")
        return reasons
    expected_classification = (
        "capability-denied" if capability else ("policy-denied" if policy else None)
    )
    retained_entries: list[tuple[str, str, str, str, str]] = []
    for entry in entries:
        if not isinstance(entry, dict):
            reasons.append(f"{prefix}-diagnostics-entry-invalid")
            break
        classification = entry.get("classification")
        method = entry.get("method")
        resource_type = entry.get("resource_type")
        url = entry.get("url")
        url_digest = entry.get("url_sha256")
        url_truncated = entry.get("url_truncated")
        url_hash_matches = False
        if isinstance(url, str) and isinstance(url_digest, str):
            try:
                url_hash_matches = (
                    hashlib.sha256(url.encode("utf-8")).hexdigest() == url_digest
                )
            except UnicodeEncodeError:
                url_hash_matches = False
        if (
            not isinstance(classification, str)
            or not classification
            or len(classification) > 64
            or (expected_classification and classification != expected_classification)
            or not isinstance(method, str)
            or not method
            or len(method) > 64
            or method != method.upper()
            or not isinstance(resource_type, str)
            or not resource_type
            or len(resource_type) > 64
            or resource_type != resource_type.lower()
            or not isinstance(url, str)
            or len(url) > REFERENCE_DIAGNOSTIC_URL_LIMIT
            or not isinstance(url_digest, str)
            or not SHA256.fullmatch(url_digest)
            or not isinstance(url_truncated, bool)
            or url_truncated
            and len(url) != REFERENCE_DIAGNOSTIC_URL_LIMIT
            or not url_truncated
            and not url_hash_matches
        ):
            reasons.append(f"{prefix}-diagnostics-entry-invalid")
            break
        retained_entries.append(
            (classification, method, resource_type, url_digest, url)
        )
    if retained_entries != sorted(retained_entries):
        reasons.append(f"{prefix}-diagnostics-entry-order")
    return reasons


def _validate_replay_environment(
    scenario: Scenario,
    state: dict[str, Any],
    side: str,
    expected_origin_ms: int | None = None,
) -> list[str]:
    reasons: list[str] = []
    environment = state.get("replay_environment")
    if not isinstance(environment, dict):
        return [f"{side}-replay-environment-missing"]
    expected_fields = REPLAY_ENVIRONMENT_COMMON_FIELDS | (
        {"requested_origin_ms"} if side == "candidate" else set()
    )
    if set(environment) != expected_fields:
        reasons.append(f"{side}-replay-environment-fields")
    if environment.get("version") != REFERENCE_REPLAY_ENVIRONMENT:
        reasons.append(f"{side}-replay-environment-version")
    origin = environment.get("origin_ms")
    if not _bounded_nonnegative(origin, ECMASCRIPT_DATE_MAX_MS) or not origin:
        reasons.append(f"{side}-replay-environment-origin-invalid")
        origin = None
    elif expected_origin_ms is not None and origin != expected_origin_ms:
        reasons.append(f"{side}-replay-environment-origin-mismatch")
    expected_clock_version = (
        REFERENCE_CLOCK_VERSION if side == "reference" else CANDIDATE_CLOCK_VERSION
    )
    if environment.get("clock_version") != expected_clock_version:
        reasons.append(f"{side}-replay-environment-clock-version")
    if environment.get("clock_contract") != REPLAY_CLOCK_CONTRACT:
        reasons.append(f"{side}-replay-environment-clock-contract")
    if environment.get("clock_scope") != REPLAY_CLOCK_SCOPE:
        reasons.append(f"{side}-replay-environment-clock-scope")
    if environment.get("rng_version") != REPLAY_RNG_CONTRACT:
        reasons.append(f"{side}-replay-environment-rng-contract")
    if environment.get("seed_source") != REPLAY_SEED_SOURCE:
        reasons.append(f"{side}-replay-environment-seed-source")
    if environment.get("intl_surface") != REPLAY_INTL_CONTRACT:
        reasons.append(f"{side}-replay-environment-intl-surface")
    if origin is not None:
        if environment.get("seed_u64") != str(origin):
            reasons.append(f"{side}-replay-environment-seed-u64")
        if environment.get("seed_sha256") != replay_seed_sha256(origin):
            reasons.append(f"{side}-replay-environment-seed-digest")
    if environment.get("ticks") != scenario.ticks:
        reasons.append(f"{side}-replay-environment-ticks")
    if environment.get("tick_ms") != scenario.tick_ms:
        reasons.append(f"{side}-replay-environment-tick-ms")
    expected_elapsed = scenario.ticks * scenario.tick_ms
    host_elapsed = environment.get("host_elapsed_ms")
    if (
        expected_elapsed > MAX_REPLAY_HOST_ELAPSED_MS
        or not _bounded_nonnegative(host_elapsed, MAX_REPLAY_HOST_ELAPSED_MS)
        or host_elapsed != expected_elapsed
    ):
        reasons.append(f"{side}-replay-environment-host-elapsed")

    wall_elapsed = environment.get("wall_elapsed_ms")
    monotonic_elapsed = environment.get("monotonic_elapsed_ms")
    wall_observations = environment.get("wall_observations")
    monotonic_observations = environment.get("monotonic_observations")
    monotonic_samples = environment.get("monotonic_samples")
    sources = environment.get("clock_sources")
    if (
        not isinstance(sources, dict)
        or set(sources) != set(REPLAY_CLOCK_OBSERVATION_SOURCES)
        or any(
            not _bounded_nonnegative(sources.get(name), MAX_REPLAY_OBSERVATIONS)
            for name in REPLAY_CLOCK_OBSERVATION_SOURCES
        )
    ):
        reasons.append(f"{side}-replay-environment-clock-sources")
    else:
        wall_sum = sum(sources[name] for name in REPLAY_CLOCK_WALL_SOURCES)
        monotonic_sum = sum(
            sources[name] for name in REPLAY_CLOCK_MONOTONIC_OBSERVATION_SOURCES
        )
        sample_sum = sum(
            sources[name] for name in REPLAY_CLOCK_MONOTONIC_SAMPLE_SOURCES
        )
        source_total = wall_sum + monotonic_sum + sample_sum
        if (
            source_total > MAX_REPLAY_OBSERVATIONS
            or not _bounded_nonnegative(
                wall_observations, MAX_REPLAY_OBSERVATIONS
            )
            or not _bounded_nonnegative(
                monotonic_observations, MAX_REPLAY_OBSERVATIONS
            )
            or not _bounded_nonnegative(monotonic_samples, MAX_REPLAY_OBSERVATIONS)
            or wall_observations != wall_sum
            or monotonic_observations != monotonic_sum
            or monotonic_samples != sample_sum
        ):
            reasons.append(f"{side}-replay-environment-clock-source-closure")
    if (
        not _bounded_nonnegative(wall_elapsed, MAX_REPLAY_DOMAIN_ELAPSED_MS)
        or not _bounded_nonnegative(
            monotonic_elapsed, MAX_REPLAY_DOMAIN_ELAPSED_MS
        )
        or not isinstance(host_elapsed, int)
        or isinstance(host_elapsed, bool)
        or not isinstance(wall_observations, int)
        or isinstance(wall_observations, bool)
        or not isinstance(monotonic_observations, int)
        or isinstance(monotonic_observations, bool)
        or wall_elapsed != host_elapsed + wall_observations
        or monotonic_elapsed != host_elapsed + monotonic_observations
    ):
        reasons.append(f"{side}-replay-environment-domain-clock")
    if environment.get("performance_entries") != "normalized-empty-v1":
        reasons.append(f"{side}-replay-environment-performance-entries")
    if environment.get("document_timeline") != REPLAY_CLOCK_CONTRACT:
        reasons.append(f"{side}-replay-environment-document-timeline")
    if environment.get("animation_frame") != REPLAY_CLOCK_CONTRACT:
        reasons.append(f"{side}-replay-environment-animation-frame")
    if side == "candidate":
        requested = environment.get("requested_origin_ms")
        if requested != origin:
            reasons.append("candidate-replay-environment-requested-origin-mismatch")
        if expected_origin_ms is not None and requested != expected_origin_ms:
            reasons.append("candidate-replay-environment-requested-origin-trace-mismatch")
    return reasons


def validate_reference_replay_contract(
    scenario: Scenario, state: dict[str, Any], expected_origin_ms: int
) -> list[str]:
    """Validate the cross-engine seed/clock contract before candidate launch."""

    return _validate_replay_environment(
        scenario, state, "reference", expected_origin_ms
    )


def _validate_reference_environment(
    scenario: Scenario, state: dict[str, Any]
) -> list[str]:
    reasons = _validate_replay_environment(scenario, state, "reference")

    policy = state.get("read_only_policy")
    if not isinstance(policy, dict):
        reasons.append("reference-read-only-policy-missing")
        return reasons
    if set(policy) != {
        "version",
        "preflight_policy",
        "ready",
        "denied_before_network",
        "preflight_denied_before_network",
        "diagnostics",
    }:
        reasons.append("reference-read-only-policy-fields")
    if policy.get("version") != REFERENCE_READ_ONLY_POLICY:
        reasons.append("reference-read-only-policy-version")
    if policy.get("preflight_policy") != REFERENCE_READ_ONLY_PREFLIGHT_POLICY:
        reasons.append("reference-read-only-policy-preflight-version")
    if policy.get("ready") is not True:
        reasons.append("reference-read-only-policy-not-ready")
    denied = policy.get("denied_before_network")
    preflight_denied = policy.get("preflight_denied_before_network")
    if not _bounded_nonnegative(denied, REFERENCE_POLICY_DENIAL_LIMIT):
        reasons.append("reference-read-only-policy-denied-count-invalid")
    else:
        reasons.extend(
            _validate_request_diagnostics(policy.get("diagnostics"), denied, policy=True)
        )
    if (
        not _bounded_nonnegative(
            preflight_denied, REFERENCE_POLICY_DENIAL_LIMIT
        )
        or preflight_denied != 0
    ):
        reasons.append("reference-read-only-policy-preflight-denied")

    capability = state.get("offline_capability_policy")
    if not isinstance(capability, dict):
        reasons.append("reference-capability-policy-missing")
    else:
        if capability.get("version") != REFERENCE_CAPABILITY_POLICY:
            reasons.append("reference-capability-policy-version")
        if capability.get("ready") is not True:
            reasons.append("reference-capability-policy-not-ready")
        expected_modes = {
            "worker_realms": "api-unavailable-before-page-script",
            "shared_worker_realms": "api-unavailable-before-page-script",
            "worklet_realms": "api-unavailable-before-module-load",
            "shadow_realms": "api-unavailable-before-page-script",
            "service_workers": "api-unavailable-and-browser-context-blocked",
            "delayed_fetch": "blocked-before-network",
            "randomized_webcrypto": "blocked-before-operation",
        }
        if any(capability.get(key) != value for key, value in expected_modes.items()):
            reasons.append("reference-capability-policy-mode")
        if capability.get("surface_evidence") != REFERENCE_CAPABILITY_SURFACE_EVIDENCE:
            reasons.append("reference-capability-policy-surface-evidence")
        diagnostics = capability.get("diagnostics")
        capability_total = diagnostics.get("total") if isinstance(diagnostics, dict) else None
        if not _bounded_nonnegative(capability_total, REFERENCE_POLICY_DENIAL_LIMIT):
            reasons.append("reference-capability-policy-denied-count-invalid")
        else:
            if capability_total != 0:
                reasons.append("reference-capability-policy-denied")
            reasons.extend(
                _validate_request_diagnostics(
                    diagnostics, capability_total, capability=True
                )
            )

    scheduler = state.get("response_scheduler")
    if not isinstance(scheduler, dict):
        reasons.append("reference-response-scheduler-missing")
    else:
        if set(scheduler) != REFERENCE_RESPONSE_SCHEDULER_FIELDS:
            reasons.append("reference-response-scheduler-fields")
        if scheduler.get("version") != REFERENCE_RESPONSE_SCHEDULER:
            reasons.append("reference-response-scheduler-version")
        if scheduler.get("ready") is not True or scheduler.get("overflow") is not False:
            reasons.append("reference-response-scheduler-not-ready")
        if (
            scheduler.get("ordering")
            != "semantic-pump,admission-generation,record-id,exact-request-identity"
            or scheduler.get("terminal_boundary")
            != REFERENCE_RESPONSE_SCHEDULER_TERMINAL_BOUNDARY
            or scheduler.get("admission_ordering")
            != (
                "exact-route-key,global-route-occurrence,resource-type,"
                "playwright-callback-ordinal"
            )
            or scheduler.get("admission_probe")
            != "bounded-playwright-context-roundtrip-quiescence"
            or scheduler.get("order_digest_version")
            != "exact-request-identity-v3"
            or scheduler.get("browser_ordinal_semantics")
            != "raw-playwright-route-callback-v1"
            or scheduler.get("request_limit")
            != REFERENCE_SCHEDULER_REQUEST_LIMIT
            or scheduler.get("retained_delay_limit_pumps")
            != MAX_REPLAY_OBSERVATIONS
            or scheduler.get("semantic_pump_limit")
            != REFERENCE_SCHEDULER_WORK_LIMIT
            or scheduler.get("pump_work_limit")
            != REFERENCE_SCHEDULER_WORK_LIMIT
            or scheduler.get("drive_step_limit")
            != REFERENCE_SCHEDULER_DRIVE_STEP_LIMIT
            or scheduler.get("admission_probe_stable_passes") != 2
            or scheduler.get("admission_probe_limit") != 64
            or scheduler.get("admission_probe_timeout_ms") != 2000
        ):
            reasons.append("reference-response-scheduler-ordering")
        for name in (
            "batches", "generations", "enqueued",
            "completed", "pending", "max_pending", "failures",
            "terminal_failures", "rejected_after_terminal",
            "retained_admissions", "terminal_admissions",
            "exact_order_count", "semantic_delivery_count",
            "raw_callback_count",
        ):
            if not _bounded_nonnegative(
                scheduler.get(name), REFERENCE_SCHEDULER_REQUEST_LIMIT
            ):
                reasons.append(f"reference-response-scheduler-{name.replace('_', '-')}")
        for name in ("semantic_pumps", "fast_forwarded_pumps"):
            if not _bounded_nonnegative(
                scheduler.get(name), REFERENCE_SCHEDULER_WORK_LIMIT
            ):
                reasons.append(f"reference-response-scheduler-{name.replace('_', '-')}")
        for name in (
            "pump_work_units",
            "scheduled_delay_work_units",
            "retained_delay_work_units",
            "terminal_delay_work_units",
        ):
            if not _bounded_nonnegative(
                scheduler.get(name), REFERENCE_SCHEDULER_WORK_LIMIT
            ):
                reasons.append(
                    f"reference-response-scheduler-{name.replace('_', '-')}"
                )
        if (
            isinstance(scheduler.get("enqueued"), int)
            and scheduler.get("completed") != scheduler.get("enqueued")
        ) or scheduler.get("pending") != 0:
            reasons.append("reference-response-scheduler-incomplete")
        if scheduler.get("failures") != 0:
            reasons.append("reference-response-scheduler-failures")
        if (
            scheduler.get("terminal_failures") != 0
            or scheduler.get("rejected_after_terminal") != 0
        ):
            reasons.append("reference-response-scheduler-terminal-failures")
        enqueued = scheduler.get("enqueued")
        batches = scheduler.get("batches")
        generations = scheduler.get("generations")
        max_pending = scheduler.get("max_pending")
        ledger = state.get("replay_ledger")
        if (
            isinstance(enqueued, int)
            and not isinstance(enqueued, bool)
            and (
                not isinstance(batches, int)
                or isinstance(batches, bool)
                or not isinstance(generations, int)
                or isinstance(generations, bool)
                or not isinstance(max_pending, int)
                or isinstance(max_pending, bool)
                or batches > enqueued
                or generations > enqueued
                or max_pending > enqueued
                or (enqueued > 0 and (batches == 0 or generations == 0))
                or (enqueued == 0 and (batches != 0 or generations != 0))
            )
        ):
            reasons.append("reference-response-scheduler-structural-closure")
        completed = scheduler.get("completed")
        retained_admissions = scheduler.get("retained_admissions")
        terminal_admissions = scheduler.get("terminal_admissions")
        exact_order_count = scheduler.get("exact_order_count")
        semantic_delivery_count = scheduler.get("semantic_delivery_count")
        raw_callback_count = scheduler.get("raw_callback_count")
        if not all(
            isinstance(value, int) and not isinstance(value, bool)
            for value in (
                enqueued,
                completed,
                retained_admissions,
                terminal_admissions,
                exact_order_count,
                semantic_delivery_count,
                raw_callback_count,
            )
        ) or (
            retained_admissions + terminal_admissions != enqueued
            or exact_order_count != completed
            or semantic_delivery_count != completed
            or raw_callback_count != enqueued
        ):
            reasons.append("reference-response-scheduler-count-closure")
        if (
            not isinstance(ledger, dict)
            or not isinstance(ledger.get("scheduled"), int)
            or isinstance(ledger.get("scheduled"), bool)
            or not isinstance(ledger.get("matched"), int)
            or isinstance(ledger.get("matched"), bool)
            or ledger.get("scheduled") < ledger.get("matched")
            or completed != ledger.get("scheduled")
            or retained_admissions != ledger.get("matched")
            or terminal_admissions
            != ledger.get("scheduled") - ledger.get("matched")
        ):
            reasons.append("reference-response-scheduler-ledger-closure")
        pump_work_units = scheduler.get("pump_work_units")
        scheduled_delay_work_units = scheduler.get("scheduled_delay_work_units")
        retained_delay_work_units = scheduler.get("retained_delay_work_units")
        terminal_delay_work_units = scheduler.get("terminal_delay_work_units")
        if not all(
            isinstance(value, int) and not isinstance(value, bool)
            for value in (
                pump_work_units,
                scheduled_delay_work_units,
                retained_delay_work_units,
                terminal_delay_work_units,
            )
        ) or (
            pump_work_units != scheduled_delay_work_units
            or scheduled_delay_work_units
            != retained_delay_work_units + terminal_delay_work_units
        ):
            reasons.append("reference-response-scheduler-pump-work-closure")
        if not all(
            isinstance(value, int) and not isinstance(value, bool)
            for value in (
                retained_admissions,
                terminal_admissions,
                retained_delay_work_units,
                terminal_delay_work_units,
            )
        ) or (
            retained_delay_work_units < retained_admissions
            or terminal_delay_work_units < terminal_admissions
            or retained_delay_work_units
            > retained_admissions * MAX_REPLAY_OBSERVATIONS
            or terminal_delay_work_units
            > terminal_admissions * MAX_REPLAY_OBSERVATIONS
        ):
            reasons.append("reference-response-scheduler-admission-delay-closure")
        if (
            isinstance(scheduler.get("fast_forwarded_pumps"), int)
            and isinstance(scheduler.get("semantic_pumps"), int)
            and scheduler.get("fast_forwarded_pumps")
            > scheduler.get("semantic_pumps")
        ):
            reasons.append("reference-response-scheduler-fast-forward-closure")
        semantic_pumps = scheduler.get("semantic_pumps")
        fast_forwarded_pumps = scheduler.get("fast_forwarded_pumps")
        if not all(
            isinstance(value, int) and not isinstance(value, bool)
            for value in (
                semantic_pumps,
                fast_forwarded_pumps,
                batches,
                pump_work_units,
            )
        ) or (
            semantic_pumps != fast_forwarded_pumps + batches
            or semantic_pumps > pump_work_units
        ):
            reasons.append("reference-response-scheduler-pump-closure")
        if (
            not isinstance(max_pending, int)
            or isinstance(max_pending, bool)
            or not isinstance(enqueued, int)
            or isinstance(enqueued, bool)
            or (enqueued == 0 and max_pending != 0)
            or (enqueued > 0 and max_pending == 0)
        ):
            reasons.append("reference-response-scheduler-peak-closure")
        for name in (
            "order_sha256",
            "semantic_delivery_order_sha256",
            "raw_callback_arrival_sha256",
        ):
            if not isinstance(scheduler.get(name), str) or not SHA256.fullmatch(
                scheduler.get(name, "")
            ):
                reasons.append(
                    f"reference-response-scheduler-{name.replace('_', '-')}"
                )

    host_operations = state.get("host_operations")
    if not isinstance(host_operations, dict):
        reasons.append("reference-host-operations-missing")
    else:
        expected_host_fields = {
            "version",
            "timeout_ms",
            "started",
            "completed",
            "rejected",
            "timed_out",
            "pending",
            "orphaned",
            "orphan_pending",
            "late_completions",
            "terminal_failures",
            "rejected_after_terminal",
            "terminal_label",
            "closed",
            "ready",
        }
        if set(host_operations) != expected_host_fields:
            reasons.append("reference-host-operations-fields")
        if host_operations.get("version") != REFERENCE_HOST_OPERATIONS:
            reasons.append("reference-host-operations-version")
        timeout_ms = host_operations.get("timeout_ms")
        if (
            not _bounded_nonnegative(
                timeout_ms, REFERENCE_HOST_OPERATION_TIMEOUT_LIMIT_MS
            )
            or timeout_ms == 0
        ):
            reasons.append("reference-host-operations-timeout")
        for name in (
            "started",
            "completed",
            "rejected",
            "timed_out",
            "pending",
            "orphaned",
            "orphan_pending",
            "late_completions",
            "terminal_failures",
            "rejected_after_terminal",
        ):
            if not _bounded_nonnegative(
                host_operations.get(name), REFERENCE_SCHEDULER_WORK_LIMIT
            ):
                reasons.append(f"reference-host-operations-{name.replace('_', '-')}")
        if (
            host_operations.get("ready") is not True
            or host_operations.get("closed") is not True
            or host_operations.get("completed") != host_operations.get("started")
            or any(
                host_operations.get(name) != 0
                for name in (
                    "rejected",
                    "timed_out",
                    "pending",
                    "orphaned",
                    "orphan_pending",
                    "late_completions",
                    "terminal_failures",
                    "rejected_after_terminal",
                )
            )
            or host_operations.get("terminal_label") != ""
        ):
            reasons.append("reference-host-operations-not-ready")
        ledger = state.get("replay_ledger")
        if (
            isinstance(ledger, dict)
            and _bounded_nonnegative(
                ledger.get("scheduled"), REPLAY_LEDGER_COUNT_LIMIT
            )
            and (
                not isinstance(host_operations.get("started"), int)
                or isinstance(host_operations.get("started"), bool)
                or host_operations["started"] < ledger["scheduled"]
            )
        ):
            reasons.append("reference-host-operations-ledger-closure")

    publication = state.get("publication_boundary")
    if (
        not isinstance(publication, dict)
        or publication.get("version") != REFERENCE_PUBLICATION_BOUNDARY
        or publication.get("ready") is not True
        or publication.get("context_closed") is not True
        or publication.get("browser_closed") is not True
        or publication.get("teardown_changes") != []
        or publication.get("late_callbacks")
        != {
            "closing": 0,
            "closed": 0,
            "binding": 0,
            "handlers": 0,
            "terminal_failures": 0,
        }
        or not _bounded_nonnegative(
            publication.get("final_activity"), REPLAY_LEDGER_COUNT_LIMIT
        )
    ):
        reasons.append("reference-publication-boundary-invalid")
    ledger = state.get("replay_ledger")
    if (
        isinstance(publication, dict)
        and isinstance(ledger, dict)
        and _bounded_nonnegative(ledger.get("requests"), REPLAY_LEDGER_COUNT_LIMIT)
        and (
            not isinstance(publication.get("final_activity"), int)
            or isinstance(publication.get("final_activity"), bool)
            or publication["final_activity"] < ledger["requests"]
        )
    ):
        reasons.append("reference-publication-ledger-closure")
    return reasons


def _validate_reference_state_schema(state: dict[str, Any]) -> list[str]:
    """Reject schema-2 reference objects with missing or unexamined fields."""

    reasons: list[str] = []
    if set(state) != REFERENCE_STATE_FIELDS:
        reasons.append("reference-state-fields")

    exact_objects = (
        ("resources", REFERENCE_RESOURCE_FIELDS, "reference-resource-fields"),
        (
            "replay_ledger",
            REFERENCE_REPLAY_LEDGER_FIELDS,
            "reference-replay-ledger-fields",
        ),
        (
            "offline_capability_policy",
            REFERENCE_CAPABILITY_POLICY_FIELDS,
            "reference-capability-policy-fields",
        ),
        (
            "publication_boundary",
            REFERENCE_PUBLICATION_FIELDS,
            "reference-publication-boundary-fields",
        ),
        (
            "acquisition_plan",
            REFERENCE_ACQUISITION_FIELDS,
            "reference-acquisition-plan-fields",
        ),
        ("browser", REFERENCE_BROWSER_FIELDS, "reference-browser-fields"),
    )
    for name, fields, reason in exact_objects:
        value = state.get(name)
        if not isinstance(value, dict) or set(value) != fields:
            reasons.append(reason)

    diagnostics = (
        state.get("replay_ledger", {}).get("unexpected_requests")
        if isinstance(state.get("replay_ledger"), dict)
        else None,
        state.get("read_only_policy", {}).get("diagnostics")
        if isinstance(state.get("read_only_policy"), dict)
        else None,
        state.get("offline_capability_policy", {}).get("diagnostics")
        if isinstance(state.get("offline_capability_policy"), dict)
        else None,
    )
    for value in diagnostics:
        if not isinstance(value, dict) or set(value) != REFERENCE_DIAGNOSTIC_FIELDS:
            reasons.append("reference-diagnostics-fields")
            continue
        entries = value.get("entries")
        if not isinstance(entries, list) or any(
            not isinstance(entry, dict)
            or set(entry) != REFERENCE_DIAGNOSTIC_ENTRY_FIELDS
            for entry in entries
        ):
            reasons.append("reference-diagnostics-entry-fields")

    checkpoints = state.get("checkpoints")
    if not isinstance(checkpoints, list) or any(
        not isinstance(checkpoint, dict)
        or set(checkpoint) != REFERENCE_CHECKPOINT_FIELDS
        for checkpoint in checkpoints
    ):
        reasons.append("reference-checkpoint-fields")
    return reasons


def _validate_reference_capture(
    scenario: Scenario, state: dict[str, Any], capture_url: str
) -> list[str]:
    reasons = _validate_reference_state_schema(state)
    if state.get("capture_transport") != REFERENCE_CAPTURE_TRANSPORT:
        reasons.append("reference-capture-transport-mismatch")
    if capture_url != scenario.url:
        reasons.append("reference-capture-url-mismatch")
    if state.get("capture_ready") is not True:
        reasons.append("reference-capture-not-ready")
    if state.get("failure") is not None:
        reasons.append("reference-capture-failure")
    if state.get("eligibility_reasons") != []:
        reasons.append("reference-eligibility-reasons-not-empty")
    if state.get("state_markers") != [scenario.required_state_marker]:
        reasons.append("reference-state-markers-not-exact")
    checkpoints = state.get("checkpoints")
    coincident = (
        scenario.trivial_viewport
        and isinstance(checkpoints, list)
        and len(checkpoints) == 2
        and all(
            isinstance(checkpoint, dict) and checkpoint.get("scroll_y") == 0
            for checkpoint in checkpoints
        )
    )
    if state.get("top_bottom_coincident") is not coincident:
        reasons.append("reference-top-bottom-coincidence-closure")
    reasons.extend(
        _validate_replay_ledger(state, "reference", REFERENCE_CAPTURE_TRANSPORT)
    )
    ledger = state.get("replay_ledger")
    if isinstance(ledger, dict) and _valid_replay_ledger_counts(ledger):
        # Matched rejections are retained trace outcomes, not unexpected
        # requests. Route-policy denials still fail the terminal ledger
        # closure because they are not matched records.
        expected_unexpected = sum(
            ledger[name] for name in ("unmatched", "conflicts", "invalid")
        )
        reasons.extend(
            _validate_request_diagnostics(
                ledger.get("unexpected_requests"), expected_unexpected, policy=False
            )
        )
    resources = state.get("resources")
    scheduler = state.get("response_scheduler")
    if isinstance(resources, dict):
        for name in (
            "stylesheets_loaded",
            "images_loaded",
            "scripts_loaded",
            "network_completions",
            "deferred_images",
            "pending",
        ):
            if not _bounded_nonnegative(
                resources.get(name), REPLAY_LEDGER_COUNT_LIMIT
            ):
                reasons.append(
                    f"reference-resource-{name.replace('_', '-')}-invalid"
                )
        if (
            isinstance(ledger, dict)
            and resources.get("network_completions") != ledger.get("served")
        ):
            reasons.append("reference-resource-network-ledger-closure")
        if (
            isinstance(scheduler, dict)
            and resources.get("pending") != scheduler.get("pending")
        ):
            reasons.append("reference-resource-scheduler-pending-closure")
        if (
            isinstance(resources.get("scripts_loaded"), int)
            and not isinstance(resources.get("scripts_loaded"), bool)
            and isinstance(resources.get("network_completions"), int)
            and not isinstance(resources.get("network_completions"), bool)
            and resources["scripts_loaded"] > resources["network_completions"]
        ):
            reasons.append("reference-resource-script-network-closure")
    browser = state.get("browser")
    if isinstance(browser, dict):
        version = browser.get("version")
        user_agent = browser.get("user_agent")
        platform = browser.get("platform")
        if browser.get("engine") != "chromium":
            reasons.append("reference-browser-engine")
        if (
            not isinstance(version, str)
            or not REFERENCE_BROWSER_VERSION.fullmatch(version)
        ):
            reasons.append("reference-browser-version")
        if (
            not isinstance(user_agent, str)
            or not 0 < len(user_agent) <= 2048
            or any(ord(character) < 0x20 or ord(character) > 0x7E for character in user_agent)
            or not isinstance(version, str)
            or f"HeadlessChrome/{version}" not in user_agent
        ):
            reasons.append("reference-browser-user-agent")
        if (
            not isinstance(platform, str)
            or not 0 < len(platform) <= 128
            or any(ord(character) < 0x20 or ord(character) > 0x7E for character in platform)
        ):
            reasons.append("reference-browser-platform")
        if browser.get("locale") != "en-US" or browser.get("timezone") != "UTC":
            reasons.append("reference-browser-locale-timezone")
    reasons.extend(_validate_reference_environment(scenario, state))
    acquisition = state.get("acquisition_plan")
    if (
        not isinstance(acquisition, dict)
        or acquisition.get("mode") != "exact-get-head-plan-v1"
        or acquisition.get("complete") is not True
        or acquisition.get("request_count") != 0
        or acquisition.get("unplannable") != 0
        or acquisition.get("requests") != []
    ):
        reasons.append("reference-acquisition-plan-not-closed")
    return reasons


def _frame_info(
    path: Path,
    side: str,
    expected_width: int | None = None,
    expected_height: int | None = None,
) -> tuple[int, int, str]:
    try:
        if side == "reference":
            raw = _read_bounded_regular(
                path, REFERENCE_FRAME_BYTE_LIMIT, "reference frame"
            )
            width, height, _ = read_png_bytes(
                raw,
                label=str(path),
                expected_width=expected_width,
                expected_height=expected_height,
                required_color=2,
                max_compressed_bytes=REFERENCE_FRAME_BYTE_LIMIT,
                max_decompressed_bytes=REFERENCE_FRAME_BYTE_LIMIT,
            )
            return width, height, "png-rgb8"
        width, height, _ = read_ppm(path)
        return width, height, "ppm-p6-rgb8"
    except (OSError, ValueError) as error:
        raise ArtifactError(str(error)) from error


def _safe_artifact(root: Path, relative: Any, label: str) -> Path:
    if not isinstance(relative, str):
        raise ArtifactError(f"{label} frame path must be a string")
    path = PurePosixPath(relative)
    if not relative or path.is_absolute() or ".." in path.parts:
        raise ArtifactError(f"{label} frame path must be relative")
    artifact = root / Path(*path.parts)
    try:
        resolved_root = root.resolve(strict=True)
        resolved = artifact.resolve(strict=True)
        resolved.relative_to(resolved_root)
    except (FileNotFoundError, OSError, ValueError) as error:
        raise ArtifactError(f"{artifact}: frame is missing or escapes its state root") from error
    if artifact.is_symlink():
        raise ArtifactError(f"{artifact}: frame must be a regular artifact, not a symlink")
    return artifact


def validate_state(
    scenario: Scenario,
    state: dict[str, Any],
    state_path: Path,
    side: str,
    *,
    prevalidated_reference_frames: dict[str, tuple[int, int, str]] | None = None,
) -> tuple[list[str], list[str]]:
    reasons: list[str] = []
    formats: list[str] = []
    try:
        schema = _state_scalar(state, "schema", int)
        name = _state_scalar(state, "scenario", str)
        digest = _state_scalar(state, "trace_sha256", str)
        url = _state_scalar(state, "url", str)
        capture_url = _state_scalar(state, "capture_url", str)
        status = _state_scalar(state, "http_status", int)
        title = _state_scalar(state, "title", str)
        markers = _state_scalar(state, "state_markers", list)
        fallback = _state_scalar(state, "fallback", bool)
        interstitial = _state_scalar(state, "interstitial", bool)
        viewport = _state_scalar(state, "viewport", dict)
        checkpoints = _state_scalar(state, "checkpoints", list)
    except ArtifactError as error:
        return [f"{side}-state-invalid:{error}"], formats
    if schema != SCHEMA_VERSION:
        reasons.append(f"{side}-schema-mismatch")
    if name != scenario.scenario:
        reasons.append(f"{side}-scenario-mismatch")
    if digest != scenario.trace_sha256:
        reasons.append(f"{side}-trace-mismatch")
    if url != scenario.url:
        reasons.append(f"{side}-url-mismatch")
    if side == "reference":
        reasons.extend(_validate_reference_capture(scenario, state, capture_url))
    elif capture_url != scenario.url:
        reasons.append("candidate-capture-url-mismatch")
    if status != scenario.expected_http:
        reasons.append(f"{side}-http-mismatch")
    if scenario.required_title not in title:
        reasons.append(f"{side}-title-mismatch")
    if not all(isinstance(marker, str) for marker in markers):
        reasons.append(f"{side}-state-markers-invalid")
    elif scenario.required_state_marker not in markers:
        reasons.append(f"{side}-state-marker-missing")
    if fallback:
        reasons.append(f"{side}-fallback")
    if interstitial:
        reasons.append(f"{side}-interstitial")
    if viewport != scenario.viewport:
        reasons.append(f"{side}-viewport-mismatch")
    reasons.extend(_validate_resources(scenario, state, side))
    if side == "candidate":
        evidence = state.get("candidate_evidence")
        if (
            not isinstance(evidence, dict)
            or set(evidence) != {"version", "schema", "encoding", "record_sha256"}
            or evidence.get("version") != CANDIDATE_EVIDENCE_VERSION
            or evidence.get("schema") != SCHEMA_VERSION
            or evidence.get("encoding") != "fixed-order-hex-utf8-v1"
            or not isinstance(evidence.get("record_sha256"), str)
            or not SHA256.fullmatch(evidence.get("record_sha256", ""))
        ):
            reasons.append("candidate-machine-evidence-invalid")
        reasons.extend(_validate_replay_ledger(state, side, "response-keyed"))
        reasons.extend(_validate_replay_environment(scenario, state, side))

    if len(checkpoints) != len(scenario.checkpoints):
        reasons.append(f"{side}-checkpoint-count-mismatch")
        return reasons, formats
    for expected, actual in zip(scenario.checkpoints, checkpoints):
        if not isinstance(actual, dict):
            reasons.append(f"{side}-{expected.name}-checkpoint-invalid")
            continue
        if actual.get("name") != expected.name or actual.get("kind") != expected.kind:
            reasons.append(f"{side}-{expected.name}-checkpoint-mismatch")
        if actual.get("target") != expected.target:
            reasons.append(f"{side}-{expected.name}-target-mismatch")
        scroll_y = actual.get("scroll_y")
        if not isinstance(scroll_y, int) or isinstance(scroll_y, bool) or scroll_y < 0:
            reasons.append(f"{side}-{expected.name}-scroll-missing")
        try:
            frame = _safe_artifact(state_path.parent, actual.get("frame"), side)
            if side == "reference" and prevalidated_reference_frames is not None:
                frame_info = prevalidated_reference_frames.get(frame.name)
                if frame_info is None:
                    raise ArtifactError(
                        f"{frame}: frame was not bound and validated by the proof"
                    )
                width, height, frame_format = frame_info
            else:
                width, height, frame_format = _frame_info(
                    frame,
                    side,
                    scenario.device_width if side == "reference" else None,
                    scenario.device_height if side == "reference" else None,
                )
            formats.append(frame_format)
            if actual.get("format") != frame_format:
                reasons.append(f"{side}-{expected.name}-format-mismatch")
            if actual.get("width") != width or actual.get("height") != height:
                reasons.append(f"{side}-{expected.name}-decoded-geometry-mismatch")
            if (width, height) != (scenario.device_width, scenario.device_height):
                reasons.append(
                    f"{side}-{expected.name}-geometry-{width}x{height}"
                )
        except ArtifactError as error:
            reasons.append(f"{side}-{expected.name}-frame-invalid:{error}")
    if scenario.trivial_viewport and len(checkpoints) == 2:
        if checkpoints[0].get("scroll_y") != 0 or checkpoints[1].get("scroll_y") != 0:
            reasons.append(f"{side}-top-bottom-not-coincident")
        if state.get("top_bottom_coincident") is not True:
            reasons.append(f"{side}-coincidence-not-recorded")
    return reasons, formats


def qualify(
    scenario: Scenario,
    replay_directory: Path,
    reference_state_path: Path,
    candidate_state_path: Path,
) -> Eligibility:
    reasons: list[str] = []
    trace_origin_ms: int | None = None
    if scenario.trace_sha256 == PENDING_DIGEST:
        reasons.append("trace-digest-pending")
    else:
        try:
            actual_digest = trace_digest(replay_directory)
            if actual_digest != scenario.trace_sha256:
                reasons.append("replay-trace-mismatch")
            trace_origin_ms = trace_replay_origin_ms(replay_directory)
        except (OSError, ArtifactError) as error:
            reasons.append(f"replay-trace-unavailable:{error}")

    states: list[tuple[str, Path, dict[str, Any]]] = []
    for side, path in (
        ("reference", reference_state_path),
        ("candidate", candidate_state_path),
    ):
        if not path.is_file():
            reasons.append(f"{side}-state-missing")
            continue
        try:
            states.append((side, path, load_state(path)))
        except ArtifactError as error:
            reasons.append(f"{side}-state-invalid:{error}")
    validated: dict[str, dict[str, Any]] = {}
    for side, path, state in states:
        side_reasons, _ = validate_state(scenario, state, path, side)
        reasons.extend(side_reasons)
        validated[side] = state
    if trace_origin_ms is not None:
        for side in ("reference", "candidate"):
            if side in validated:
                reasons.extend(
                    _validate_replay_environment(
                        scenario, validated[side], side, trace_origin_ms
                    )
                )
    if "reference" in validated and "candidate" in validated:
        reference = validated["reference"]
        candidate = validated["candidate"]
        for field in ("trace_sha256", "url", "http_status", "title", "state_markers"):
            if reference.get(field) != candidate.get(field):
                reasons.append(f"state-{field.replace('_', '-')}-mismatch")
    unique = tuple(dict.fromkeys(reasons))
    return Eligibility(
        scenario=scenario.scenario,
        status="ELIGIBLE" if not unique else "NOT_COMPARABLE",
        details=unique or ("ok",),
        trace_sha256=scenario.trace_sha256,
        checkpoint_count=len(scenario.checkpoints),
    )


def runtime_body(log: str) -> str:
    start = log.find("javascript-state ")
    if start < 0:
        return ""
    end = log.find("\njavascript-section-retention ", start)
    return log[start : len(log) if end < 0 else end]


def log_number(pattern: str, log: str, default: int = 0) -> int:
    match = re.search(pattern, log, re.MULTILINE)
    return int(match.group(1)) if match else default


def _claimed_routes(value: str, records: int) -> list[int] | None:
    if value == "none":
        return []
    routes: list[int] = []
    previous = -1
    for item in value.split(","):
        first_text, separator, last_text = item.partition("-")
        if (
            not re.fullmatch(r"0|[1-9][0-9]{0,3}", first_text)
            or (separator and not re.fullmatch(r"0|[1-9][0-9]{0,3}", last_text))
        ):
            return None
        try:
            first = int(first_text, 10)
            last = int(last_text, 10) if separator else first
        except ValueError:
            return None
        if (
            first > last
            or first <= previous
            or last >= records
            or last - first + 1 > REPLAY_LEDGER_RECORD_LIMIT - len(routes)
        ):
            return None
        routes.extend(range(first, last + 1))
        previous = last
    return routes


def replay_ledger_from_log(log: str) -> dict[str, Any] | None:
    """Parse the final bounded native replay ledger from a candidate log."""

    lines = [
        line
        for line in log.splitlines()
        if line.startswith(REPLAY_LEDGER_PREFIX)
    ]
    if not lines or len(lines[-1]) > REPLAY_LEDGER_LINE_LIMIT:
        return None
    match = REPLAY_LEDGER_PATTERN.fullmatch(lines[-1])
    if match is None:
        return None
    ledger: dict[str, Any] = {"mode": match.group("mode")}
    for name in REPLAY_LEDGER_COUNT_FIELDS:
        ledger[name] = int(match.group(name), 10)
    if not _valid_replay_ledger_counts(ledger):
        return None
    routes = _claimed_routes(match.group("claimed_routes"), ledger["records"])
    if routes is None or len(routes) != ledger["claimed"]:
        return None
    ledger["claimed_routes"] = routes
    return ledger


def _candidate_evidence_integer(
    values: dict[str, str], name: str, maximum: int, *, positive: bool = False
) -> int:
    raw = values[name]
    if not re.fullmatch(r"0|[1-9][0-9]*", raw):
        raise ArtifactError(f"candidate evidence {name} is not canonical")
    if len(raw) > len(str(maximum)):
        raise ArtifactError(f"candidate evidence {name} is outside its bound")
    try:
        value = int(raw, 10)
    except ValueError as error:
        raise ArtifactError(
            f"candidate evidence {name} is outside its bound"
        ) from error
    if value > maximum or (positive and value == 0):
        raise ArtifactError(f"candidate evidence {name} is outside its bound")
    return value


def _candidate_evidence_text(
    values: dict[str, str], name: str, maximum_bytes: int, *, replace: bool = False
) -> str:
    encoded = values[name]
    if encoded == "-":
        raw = b""
    else:
        if len(encoded) % 2 or not re.fullmatch(r"[0-9a-f]+", encoded):
            raise ArtifactError(f"candidate evidence {name} is not lowercase hexadecimal")
        raw = bytes.fromhex(encoded)
    if len(raw) > maximum_bytes:
        raise ArtifactError(f"candidate evidence {name} exceeds its byte bound")
    try:
        value = raw.decode("utf-8", errors="replace" if replace else "strict")
    except UnicodeDecodeError as error:
        raise ArtifactError(f"candidate evidence {name} is not UTF-8") from error
    if "\0" in value:
        raise ArtifactError(f"candidate evidence {name} contains NUL")
    return value


def candidate_evidence_from_log(
    scenario: Scenario, log: str
) -> dict[str, Any]:
    """Parse the sole fixed-order native visual evidence record.

    Human diagnostics deliberately remain verbose and may contain arbitrary page
    text. They are never consulted here. Any page-injected lookalike creates a
    duplicate record and fails closed instead of becoming qualification data.
    """

    records = [
        line
        for line in log.splitlines()
        if line.startswith(CANDIDATE_EVIDENCE_PREFIX)
    ]
    if len(records) != 1:
        raise ArtifactError(
            f"candidate evidence requires exactly one record, found {len(records)}"
        )
    line = records[0]
    if not line.isascii() or len(line.encode("ascii")) > CANDIDATE_EVIDENCE_LINE_LIMIT:
        raise ArtifactError("candidate evidence record exceeds its ASCII bound")
    tokens = line.split(" ")
    if not tokens or tokens[0] != CANDIDATE_EVIDENCE_VERSION:
        raise ArtifactError("candidate evidence version is unsupported")
    pairs: list[tuple[str, str]] = []
    for token in tokens[1:]:
        key, separator, value = token.partition("=")
        if not separator or not key or not value:
            raise ArtifactError("candidate evidence contains a malformed field")
        pairs.append((key, value))
    if tuple(key for key, _ in pairs) != CANDIDATE_EVIDENCE_FIELDS:
        raise ArtifactError(
            "candidate evidence fields are missing, duplicated, unknown, or reordered"
        )
    values = dict(pairs)
    if values["schema"] != str(SCHEMA_VERSION):
        raise ArtifactError("candidate evidence schema is unsupported")

    title = _candidate_evidence_text(values, "title-hex", 4096)
    capture_url = _candidate_evidence_text(values, "url-hex", 2047)
    body_preview = _candidate_evidence_text(
        values, "body-preview-hex", 512, replace=True
    )
    marker = _candidate_evidence_text(values, "marker-hex", 4096)
    if marker != scenario.required_state_marker:
        raise ArtifactError("candidate evidence marker does not match the manifest")
    if values["marker-found"] not in ("yes", "no"):
        raise ArtifactError("candidate evidence marker-found is invalid")
    marker_bytes = _candidate_evidence_integer(
        values, "marker-bytes", 4096, positive=True
    )
    if marker_bytes != len(marker.encode("utf-8")):
        raise ArtifactError("candidate evidence marker byte length is inconsistent")

    expected_constants = {
        "seed-source": REPLAY_SEED_SOURCE,
        "rng": REPLAY_RNG_CONTRACT,
        "clock": REPLAY_CLOCK_CONTRACT,
        "clock-scope": REPLAY_CLOCK_SCOPE,
        "performance-entries": "normalized-empty-v1",
        "intl": REPLAY_INTL_CONTRACT,
        "replay-mode": "response-keyed",
        "route-selection": REFERENCE_ROUTE_SELECTION,
    }
    for name, expected in expected_constants.items():
        if values[name] != expected:
            raise ArtifactError(f"candidate evidence {name} contract mismatch")

    status = _candidate_evidence_integer(values, "http-status", 599)
    stylesheets = _candidate_evidence_integer(
        values, "stylesheets-loaded", REPLAY_LEDGER_COUNT_LIMIT
    )
    images = _candidate_evidence_integer(
        values, "images-loaded", REPLAY_LEDGER_COUNT_LIMIT
    )
    scripts = _candidate_evidence_integer(
        values, "scripts-loaded", REPLAY_LEDGER_COUNT_LIMIT
    )
    completions = _candidate_evidence_integer(
        values, "network-completions", REPLAY_LEDGER_COUNT_LIMIT
    )
    active_native = _candidate_evidence_integer(
        values, "active-native", REPLAY_LEDGER_COUNT_LIMIT
    )
    pending_logical = _candidate_evidence_integer(
        values, "pending-logical", REPLAY_LEDGER_COUNT_LIMIT
    )
    seed = _candidate_evidence_integer(
        values, "seed", ECMASCRIPT_DATE_MAX_MS, positive=True
    )
    host_elapsed = _candidate_evidence_integer(
        values, "host-elapsed-ms", MAX_REPLAY_HOST_ELAPSED_MS
    )
    wall_elapsed = _candidate_evidence_integer(
        values, "wall-elapsed-ms", MAX_REPLAY_DOMAIN_ELAPSED_MS
    )
    monotonic_elapsed = _candidate_evidence_integer(
        values, "monotonic-elapsed-ms", MAX_REPLAY_DOMAIN_ELAPSED_MS
    )
    wall_observations = _candidate_evidence_integer(
        values, "wall-observations", MAX_REPLAY_OBSERVATIONS
    )
    monotonic_observations = _candidate_evidence_integer(
        values, "monotonic-observations", MAX_REPLAY_OBSERVATIONS
    )
    monotonic_samples = _candidate_evidence_integer(
        values, "monotonic-samples", MAX_REPLAY_OBSERVATIONS
    )
    clock_sources = {
        name: _candidate_evidence_integer(
            values, f"clock-{name.replace('_', '-')}", MAX_REPLAY_OBSERVATIONS
        )
        for name in REPLAY_CLOCK_OBSERVATION_SOURCES
    }
    wall_sum = sum(clock_sources[name] for name in REPLAY_CLOCK_WALL_SOURCES)
    monotonic_sum = sum(
        clock_sources[name] for name in REPLAY_CLOCK_MONOTONIC_OBSERVATION_SOURCES
    )
    sample_sum = sum(
        clock_sources[name] for name in REPLAY_CLOCK_MONOTONIC_SAMPLE_SOURCES
    )
    if wall_sum + monotonic_sum + sample_sum > MAX_REPLAY_OBSERVATIONS:
        raise ArtifactError("candidate evidence clock source total exceeds its bound")
    if (
        wall_observations != wall_sum
        or monotonic_observations != monotonic_sum
        or monotonic_samples != sample_sum
    ):
        raise ArtifactError("candidate evidence clock source totals are inconsistent")
    if (
        wall_elapsed != host_elapsed + wall_observations
        or monotonic_elapsed != host_elapsed + monotonic_observations
    ):
        raise ArtifactError("candidate evidence dual-domain clock is inconsistent")
    frame_count = _candidate_evidence_integer(
        values, "frames", CANDIDATE_EVIDENCE_MAX_FRAMES, positive=True
    )
    scroll_tokens = values["scrolls"].split(",")
    if len(scroll_tokens) != frame_count:
        raise ArtifactError("candidate evidence frame and scroll counts differ")
    scrolls = []
    for raw in scroll_tokens:
        if not re.fullmatch(r"0|[1-9][0-9]*", raw):
            raise ArtifactError("candidate evidence scroll offset is not canonical")
        value = int(raw, 10)
        if value > 2_147_483_647:
            raise ArtifactError("candidate evidence scroll offset exceeds its bound")
        scrolls.append(value)
    if frame_count != len(scenario.checkpoints):
        raise ArtifactError("candidate evidence frame count differs from the manifest")

    ledger: dict[str, Any] = {"mode": values["replay-mode"]}
    for name in REPLAY_LEDGER_COUNT_FIELDS:
        maximum = (
            REPLAY_LEDGER_RECORD_LIMIT
            if name in ("records", "claimed")
            else REPLAY_LEDGER_COUNT_LIMIT
        )
        ledger[name] = _candidate_evidence_integer(values, name.replace("_", "-"), maximum)
    if not _valid_replay_ledger_counts(ledger):
        raise ArtifactError("candidate evidence replay ledger is inconsistent")
    if ledger["requests"] != (
        ledger["matched"]
        + ledger["unmatched"]
        + ledger["conflicts"]
        + ledger["invalid"]
    ):
        raise ArtifactError(
            "candidate evidence requests do not have exact terminal accounting"
        )
    occurrence_claims = _candidate_evidence_integer(
        values, "occurrence-claims", REPLAY_LEDGER_COUNT_LIMIT
    )
    reusable_claims = _candidate_evidence_integer(
        values, "reusable-claims", REPLAY_LEDGER_COUNT_LIMIT
    )
    occurrence_exhausted = _candidate_evidence_integer(
        values, "occurrence-exhausted", REPLAY_LEDGER_COUNT_LIMIT
    )
    if occurrence_claims + reusable_claims != ledger["matched"]:
        raise ArtifactError("candidate evidence occurrence claims do not match requests")
    if occurrence_exhausted != 0:
        raise ArtifactError("candidate evidence occurrence route was exhausted")
    ledger.update(
        {
            "route_selection_version": values["route-selection"],
            "occurrence_claims": occurrence_claims,
            "reusable_claims": reusable_claims,
            "occurrence_exhausted": occurrence_exhausted,
        }
    )
    routes = _claimed_routes(values["claimed-routes"], ledger["records"])
    if routes is None or len(routes) != ledger["claimed"]:
        raise ArtifactError("candidate evidence claimed routes are inconsistent")
    ledger["claimed_routes"] = routes

    return {
        "record_sha256": hashlib.sha256(line.encode("ascii")).hexdigest(),
        "title": title,
        "capture_url": capture_url,
        "body_preview": body_preview,
        "http_status": status,
        "marker_found": values["marker-found"] == "yes",
        "stylesheets_loaded": stylesheets,
        "images_loaded": images,
        "scripts_loaded": scripts,
        "network_completions": completions,
        "active_native": active_native,
        "pending_logical": pending_logical,
        "seed": seed,
        "host_elapsed_ms": host_elapsed,
        "wall_elapsed_ms": wall_elapsed,
        "monotonic_elapsed_ms": monotonic_elapsed,
        "wall_observations": wall_observations,
        "monotonic_observations": monotonic_observations,
        "monotonic_samples": monotonic_samples,
        "clock_sources": clock_sources,
        "scrolls": scrolls,
        "replay_ledger": ledger,
    }


def candidate_state_from_log(
    scenario: Scenario,
    log: str,
    frame_directory: Path,
    origin_ms: int | None = None,
) -> dict[str, Any]:
    """Build the candidate evidence document from one lab run."""

    evidence = candidate_evidence_from_log(scenario, log)
    body = evidence["body_preview"]
    fallback_hits = [marker for marker in scenario.fallback_markers if marker in body]
    interstitial_hits = [
        marker for marker in scenario.interstitial_markers if marker in body
    ]
    markers = [
        marker
        for marker, found in (
            (scenario.required_state_marker, evidence["marker_found"]),
            *((marker, True) for marker in fallback_hits),
            *((marker, True) for marker in interstitial_hits),
        )
        if found
    ]
    stylesheets_loaded = evidence["stylesheets_loaded"]
    images_loaded = evidence["images_loaded"]
    scripts_loaded = evidence["scripts_loaded"]
    network_completions = evidence["network_completions"]
    active_native = evidence["active_native"]
    pending_logical = evidence["pending_logical"]
    pending = active_native + pending_logical
    resources = {
        "stylesheets_loaded": stylesheets_loaded,
        "images_loaded": images_loaded,
        "scripts_loaded": scripts_loaded,
        "network_completions": network_completions,
        "pending": pending,
        "active_native": active_native,
        "pending_logical": pending_logical,
        "pending_source": "javascript-network-logical",
    }
    resources["ready"] = (
        all(
            resources[name] >= minimum
            for name, minimum in scenario.resource_minimums.items()
        )
        and pending <= scenario.max_pending
    )

    checkpoints: list[dict[str, Any]] = []
    for index, expected in enumerate(scenario.checkpoints):
        checkpoints.append(
            {
                "name": expected.name,
                "kind": expected.kind,
                "target": expected.target,
                "scroll_y": evidence["scrolls"][index],
                "frame": str(
                    Path(frame_directory.name, f"frame-{index:04d}.ppm")
                ),
                "format": "ppm-p6-rgb8",
                "width": scenario.device_width,
                "height": scenario.device_height,
            }
        )
    observed_origin = evidence["seed"]
    replay_environment = {
        "version": REFERENCE_REPLAY_ENVIRONMENT,
        "origin_ms": observed_origin,
        "clock_version": CANDIDATE_CLOCK_VERSION,
        "clock_contract": REPLAY_CLOCK_CONTRACT,
        "clock_scope": REPLAY_CLOCK_SCOPE,
        "rng_version": REPLAY_RNG_CONTRACT,
        "seed_source": REPLAY_SEED_SOURCE,
        "intl_surface": REPLAY_INTL_CONTRACT,
        "seed_u64": str(observed_origin) if observed_origin is not None else "",
        "seed_sha256": (
            replay_seed_sha256(observed_origin)
            if observed_origin is not None
            else ""
        ),
        "ticks": scenario.ticks,
        "tick_ms": scenario.tick_ms,
        "host_elapsed_ms": evidence["host_elapsed_ms"],
        "wall_elapsed_ms": evidence["wall_elapsed_ms"],
        "monotonic_elapsed_ms": evidence["monotonic_elapsed_ms"],
        "wall_observations": evidence["wall_observations"],
        "monotonic_observations": evidence["monotonic_observations"],
        "monotonic_samples": evidence["monotonic_samples"],
        "clock_sources": evidence["clock_sources"],
        "performance_entries": "normalized-empty-v1",
        "document_timeline": REPLAY_CLOCK_CONTRACT,
        "animation_frame": REPLAY_CLOCK_CONTRACT,
        "requested_origin_ms": origin_ms,
    }
    return {
        "schema": SCHEMA_VERSION,
        "scenario": scenario.scenario,
        "trace_sha256": scenario.trace_sha256,
        "url": scenario.url,
        "capture_url": evidence["capture_url"],
        "http_status": evidence["http_status"],
        "title": evidence["title"],
        "state_markers": markers,
        "fallback": bool(fallback_hits),
        "interstitial": bool(interstitial_hits),
        "top_bottom_coincident": scenario.trivial_viewport
        and len(evidence["scrolls"]) >= 2
        and evidence["scrolls"][0] == 0
        and evidence["scrolls"][1] == 0,
        "viewport": scenario.viewport,
        "resources": resources,
        "replay_ledger": evidence["replay_ledger"],
        "replay_environment": replay_environment,
        "candidate_evidence": {
            "version": CANDIDATE_EVIDENCE_VERSION,
            "schema": SCHEMA_VERSION,
            "encoding": "fixed-order-hex-utf8-v1",
            "record_sha256": evidence["record_sha256"],
        },
        "checkpoints": checkpoints,
    }


def _read_candidate_log(path: Path) -> str:
    """Read one stable, bounded regular log without following a symlink."""

    try:
        path_status = path.lstat()
    except FileNotFoundError as error:
        raise ArtifactError(f"{path}: candidate log is missing") from error
    except OSError as error:
        raise ArtifactError(f"{path}: candidate log cannot be inspected") from error
    if stat.S_ISLNK(path_status.st_mode):
        raise ArtifactError(f"{path}: candidate log must not be a symlink")
    if not stat.S_ISREG(path_status.st_mode):
        raise ArtifactError(f"{path}: candidate log must be a regular file")
    if path_status.st_size > CANDIDATE_LOG_BYTE_LIMIT:
        raise ArtifactError(f"{path}: candidate log exceeds its byte bound")

    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0)
    flags |= getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(path, flags)
    except OSError as error:
        raise ArtifactError(f"{path}: candidate log cannot be opened safely") from error
    try:
        opened_status = os.fstat(descriptor)
        if (
            not stat.S_ISREG(opened_status.st_mode)
            or opened_status.st_dev != path_status.st_dev
            or opened_status.st_ino != path_status.st_ino
        ):
            raise ArtifactError(f"{path}: candidate log changed before it was opened")
        if opened_status.st_size > CANDIDATE_LOG_BYTE_LIMIT:
            raise ArtifactError(f"{path}: candidate log exceeds its byte bound")

        chunks: list[bytes] = []
        remaining = CANDIDATE_LOG_BYTE_LIMIT + 1
        while remaining:
            chunk = os.read(descriptor, min(64 * 1024, remaining))
            if not chunk:
                break
            chunks.append(chunk)
            remaining -= len(chunk)
        raw = b"".join(chunks)
        final_status = os.fstat(descriptor)
        try:
            final_path_status = path.lstat()
        except OSError as error:
            raise ArtifactError(f"{path}: candidate log changed while read") from error
        stable_identity = (
            final_path_status.st_dev == opened_status.st_dev
            and final_path_status.st_ino == opened_status.st_ino
            and not stat.S_ISLNK(final_path_status.st_mode)
            and stat.S_ISREG(final_path_status.st_mode)
        )
        stable_contents = (
            final_status.st_size == opened_status.st_size == len(raw)
            and final_status.st_mtime_ns == opened_status.st_mtime_ns
            and final_status.st_ctime_ns == opened_status.st_ctime_ns
        )
        if not stable_identity or not stable_contents:
            raise ArtifactError(f"{path}: candidate log changed while read")
    finally:
        os.close(descriptor)
    if len(raw) > CANDIDATE_LOG_BYTE_LIMIT:
        raise ArtifactError(f"{path}: candidate log exceeds its byte bound")
    try:
        return raw.decode("utf-8", errors="strict")
    except UnicodeDecodeError as error:
        raise ArtifactError(f"{path}: candidate log is not UTF-8") from error


def _strict_json_equal(left: Any, right: Any) -> bool:
    """Compare JSON-shaped values without Python's bool/int coercion."""

    if type(left) is not type(right):
        return False
    if isinstance(left, dict):
        return left.keys() == right.keys() and all(
            _strict_json_equal(left[key], right[key]) for key in left
        )
    if isinstance(left, list):
        return len(left) == len(right) and all(
            _strict_json_equal(a, b) for a, b in zip(left, right)
        )
    return left == right


def _checked_directory(path: Path, label: str) -> Path:
    try:
        status = path.lstat()
    except FileNotFoundError as error:
        raise ArtifactError(f"{path}: {label} is missing") from error
    except OSError as error:
        raise ArtifactError(f"{path}: {label} cannot be inspected") from error
    if stat.S_ISLNK(status.st_mode) or not stat.S_ISDIR(status.st_mode):
        raise ArtifactError(f"{path}: {label} must be a non-symlink directory")
    return path


def _checked_relative_artifact(root: Path, relative: str, label: str) -> Path:
    path = PurePosixPath(relative)
    if (
        not relative
        or path.is_absolute()
        or "." in path.parts
        or ".." in path.parts
    ):
        raise ArtifactError(f"{label} path must be a normalized relative path")
    current = _checked_directory(root, f"{label} root")
    for part in path.parts[:-1]:
        current = _checked_directory(current / part, f"{label} parent")
    return current / path.parts[-1]


def _proof_root_for_reference(reference_root: Path) -> Path:
    """Resolve the explicit proof contract's root without guessing other layouts."""

    _checked_directory(reference_root, "reference proof input")
    direct = reference_root / "determinism-proof.json"
    adjacent = reference_root.parent / "determinism-proof.json"
    try:
        direct_exists = direct.lstat() is not None
    except FileNotFoundError:
        direct_exists = False
    except OSError as error:
        raise ArtifactError(f"{direct}: proof cannot be inspected") from error
    try:
        adjacent_exists = adjacent.lstat() is not None
    except FileNotFoundError:
        adjacent_exists = False
    except OSError as error:
        raise ArtifactError(f"{adjacent}: proof cannot be inspected") from error

    if direct_exists and reference_root.name == REFERENCE_PROOF_CANONICAL_RUN and adjacent_exists:
        raise ArtifactError("reference proof location is ambiguous")
    if direct_exists:
        return reference_root
    if reference_root.name == REFERENCE_PROOF_CANONICAL_RUN and adjacent_exists:
        return reference_root.parent
    raise ArtifactError(
        "determinism-proof.json is missing from the proof root or canonical run parent"
    )


def _proof_digest(value: Any, label: str) -> str:
    if not isinstance(value, str) or not SHA256.fullmatch(value):
        raise ArtifactError(f"reference proof {label} is not a SHA-256 digest")
    return value


def _proof_frame_entries(value: Any, expected_count: int) -> tuple[tuple[str, str], ...]:
    if not isinstance(value, list) or len(value) != expected_count:
        raise ArtifactError("reference proof frame count differs from the manifest")
    entries: list[tuple[str, str]] = []
    for item in value:
        if not isinstance(item, dict) or set(item) != {"name", "sha256"}:
            raise ArtifactError("reference proof frame entry is malformed")
        name = item.get("name")
        if (
            not isinstance(name, str)
            or not SAFE_NAME.fullmatch(name)
            or not name.endswith(".png")
        ):
            raise ArtifactError("reference proof frame name is unsafe")
        entries.append((name, _proof_digest(item.get("sha256"), f"frame {name}")))
    if entries != sorted(entries) or len({name for name, _ in entries}) != len(entries):
        raise ArtifactError("reference proof frames must be uniquely sorted by name")
    return tuple(entries)


def _state_checkpoint_frames(
    state: dict[str, Any], expected_count: int, label: str
) -> tuple[str, ...]:
    checkpoints = state.get("checkpoints")
    if not isinstance(checkpoints, list) or len(checkpoints) != expected_count:
        raise ArtifactError(f"{label} checkpoint count differs from the manifest")
    frames: list[str] = []
    for checkpoint in checkpoints:
        if not isinstance(checkpoint, dict):
            raise ArtifactError(f"{label} checkpoint is malformed")
        frame = checkpoint.get("frame")
        if (
            not isinstance(frame, str)
            or not SAFE_NAME.fullmatch(frame)
            or not frame.endswith(".png")
        ):
            raise ArtifactError(f"{label} checkpoint frame name is unsafe")
        frames.append(frame)
    if len(set(frames)) != len(frames):
        raise ArtifactError(f"{label} checkpoint frame names are duplicated")
    return tuple(sorted(frames))


def _directory_png_names(directory: Path, label: str) -> tuple[str, ...]:
    _checked_directory(directory, label)
    try:
        names = sorted(path.name for path in directory.iterdir() if path.name.endswith(".png"))
    except OSError as error:
        raise ArtifactError(f"{directory}: {label} cannot be enumerated") from error
    for name in names:
        path = directory / name
        try:
            status = path.lstat()
        except OSError as error:
            raise ArtifactError(f"{path}: proof frame cannot be inspected") from error
        if stat.S_ISLNK(status.st_mode) or not stat.S_ISREG(status.st_mode):
            raise ArtifactError(f"{path}: proof frame must be a regular non-symlink file")
    return tuple(names)


def verify_reference_proof_binding(
    scenario: Scenario,
    replay_directory: Path,
    reference_root: Path,
) -> ReferenceProofBinding:
    """Bind a canonical retained reference to its exact two-run equality proof."""

    if scenario.trace_sha256 == PENDING_DIGEST:
        raise ArtifactError("reference proof binding requires a finalized trace digest")
    proof_root = _proof_root_for_reference(reference_root)
    _checked_directory(proof_root, "reference proof root")
    proof_path = proof_root / "determinism-proof.json"
    proof = _load_bounded_json(
        proof_path, REFERENCE_PROOF_BYTE_LIMIT, "determinism proof"
    )
    expected_keys = {
        "version",
        "equivalent",
        "capture_ready",
        "scenario",
        "trace_sha256",
        "canonical_run",
        "comparison_run",
        "state_schema",
        "replay_environment",
        "state",
        "frames",
    }
    if set(proof) != expected_keys:
        raise ArtifactError("reference proof fields are missing, unknown, or duplicated")
    if proof.get("version") != REFERENCE_PROOF_VERSION:
        raise ArtifactError("reference proof version is unsupported")
    if proof.get("equivalent") is not True:
        raise ArtifactError("reference proof does not claim two-run equality")
    if proof.get("capture_ready") is not True:
        raise ArtifactError("reference proof is not capture-ready")
    if proof.get("scenario") != scenario.scenario:
        raise ArtifactError("reference proof scenario differs from the manifest")
    proof_trace_digest = _proof_digest(proof.get("trace_sha256"), "trace")
    if proof_trace_digest != scenario.trace_sha256:
        raise ArtifactError("reference proof trace digest differs from the manifest")
    actual_trace_digest = trace_digest(replay_directory)
    if actual_trace_digest != proof_trace_digest:
        raise ArtifactError("reference proof trace digest differs from retained replay")
    expected_origin_ms = trace_replay_origin_ms(replay_directory)
    if proof.get("canonical_run") != REFERENCE_PROOF_CANONICAL_RUN:
        raise ArtifactError("reference proof canonical run is unsupported")
    if proof.get("comparison_run") != REFERENCE_PROOF_COMPARISON_RUN:
        raise ArtifactError("reference proof comparison run is unsupported")
    if proof.get("state_schema") != SCHEMA_VERSION:
        raise ArtifactError("reference proof state schema is unsupported")
    proof_environment = proof.get("replay_environment")
    proof_environment_reasons = _validate_replay_environment(
        scenario,
        {"replay_environment": proof_environment},
        "reference",
        expected_origin_ms,
    )
    if proof_environment_reasons:
        raise ArtifactError(
            "reference proof replay environment is malformed: "
            + proof_environment_reasons[0]
        )

    state_entry = proof.get("state")
    if not isinstance(state_entry, dict) or set(state_entry) != {"name", "sha256"}:
        raise ArtifactError("reference proof state entry is malformed")
    expected_state_name = Path(scenario.reference_state).name
    if expected_state_name != "reference-state.json" or state_entry.get("name") != expected_state_name:
        raise ArtifactError("reference proof state filename differs from the manifest")
    state_digest = _proof_digest(state_entry.get("sha256"), "state")
    frame_entries = _proof_frame_entries(proof.get("frames"), len(scenario.checkpoints))
    proof_frame_names = tuple(name for name, _ in frame_entries)

    run_states: list[dict[str, Any]] = []
    state_paths: list[Path] = []
    for run_name in (REFERENCE_PROOF_CANONICAL_RUN, REFERENCE_PROOF_COMPARISON_RUN):
        run_root = _checked_directory(proof_root / run_name, f"reference {run_name}")
        state_path = _checked_relative_artifact(
            run_root, scenario.reference_state, f"reference {run_name} state"
        )
        state_raw = _read_bounded_regular(
            state_path, REFERENCE_STATE_BYTE_LIMIT, f"reference {run_name} state"
        )
        if hashlib.sha256(state_raw).hexdigest() != state_digest:
            raise ArtifactError(f"reference {run_name} state digest differs from proof")
        state = _decode_json_object(state_raw, state_path, f"reference {run_name} state")
        if state.get("schema") != SCHEMA_VERSION:
            raise ArtifactError(f"reference {run_name} state schema differs from proof")
        if state.get("scenario") != scenario.scenario:
            raise ArtifactError(f"reference {run_name} state scenario differs from proof")
        if state.get("trace_sha256") != proof_trace_digest:
            raise ArtifactError(f"reference {run_name} state trace digest differs from proof")
        if state.get("capture_ready") is not True:
            raise ArtifactError(f"reference {run_name} state is not capture-ready")
        if not _strict_json_equal(state.get("replay_environment"), proof_environment):
            raise ArtifactError(
                f"reference {run_name} replay environment differs from proof"
            )
        state_frame_names = _state_checkpoint_frames(
            state, len(scenario.checkpoints), f"reference {run_name} state"
        )
        scenario_directory = state_path.parent
        directory_frame_names = _directory_png_names(
            scenario_directory, f"reference {run_name} scenario directory"
        )
        if state_frame_names != proof_frame_names or directory_frame_names != proof_frame_names:
            raise ArtifactError(
                f"reference {run_name} frame set differs from state or proof"
            )
        validated_frames: dict[str, tuple[int, int, str]] = {}
        for frame_name, expected_digest in frame_entries:
            frame_path = scenario_directory / frame_name
            frame_raw = _read_bounded_regular(
                frame_path, REFERENCE_FRAME_BYTE_LIMIT, f"reference {run_name} frame"
            )
            if hashlib.sha256(frame_raw).hexdigest() != expected_digest:
                raise ArtifactError(
                    f"reference {run_name} frame {frame_name} digest differs from proof"
                )
            try:
                width, height, _pixels = read_png_bytes(
                    frame_raw,
                    label=str(frame_path),
                    expected_width=scenario.device_width,
                    expected_height=scenario.device_height,
                    required_color=2,
                    max_compressed_bytes=REFERENCE_FRAME_BYTE_LIMIT,
                    max_decompressed_bytes=REFERENCE_FRAME_BYTE_LIMIT,
                )
            except ValueError as error:
                raise ArtifactError(str(error)) from error
            validated_frames[frame_name] = (width, height, "png-rgb8")
        state_reasons, _formats = validate_state(
            scenario,
            state,
            state_path,
            "reference",
            prevalidated_reference_frames=validated_frames,
        )
        if state_reasons:
            raise ArtifactError(
                f"reference {run_name} state is not valid schema-2 evidence: "
                + state_reasons[0]
            )
        state_paths.append(state_path)
        run_states.append(state)

    if not _strict_json_equal(run_states[0], run_states[1]):
        raise ArtifactError("reference states are not semantically equal across full runs")
    canonical_input = proof_root / REFERENCE_PROOF_CANONICAL_RUN
    if reference_root.name == REFERENCE_PROOF_CANONICAL_RUN:
        try:
            if reference_root.resolve(strict=True) != canonical_input.resolve(strict=True):
                raise ArtifactError("reference canonical run does not belong to proof root")
        except OSError as error:
            raise ArtifactError("reference canonical run cannot be resolved") from error
    return ReferenceProofBinding(
        reference_state_path=state_paths[0],
        proof_root=proof_root,
        contract=REFERENCE_PROOF_VERSION,
    )


def verify_candidate_state_binding(
    scenario: Scenario,
    replay_directory: Path,
    candidate_state_path: Path,
    candidate_log_path: Path,
    frame_directory: Path,
) -> None:
    """Bind retained candidate state to its exact trace and native evidence."""

    if scenario.trace_sha256 == PENDING_DIGEST:
        raise ArtifactError("candidate binding requires a finalized trace digest")
    actual_digest = trace_digest(replay_directory)
    if actual_digest != scenario.trace_sha256:
        raise ArtifactError("candidate binding replay digest does not match the manifest")
    origin_ms = trace_replay_origin_ms(replay_directory)
    retained_state = load_state(candidate_state_path)
    log = _read_candidate_log(candidate_log_path)
    reconstructed_state = candidate_state_from_log(
        scenario, log, frame_directory, origin_ms=origin_ms
    )
    if not _strict_json_equal(retained_state, reconstructed_state):
        raise ArtifactError(
            "retained candidate state does not match its native evidence log"
        )


def commands_for(scenario: Scenario) -> str:
    """The initial loop frame is top; subsequent commands capture anchors/bottom."""

    commands: list[str] = []
    for checkpoint in scenario.checkpoints[1:]:
        if checkpoint.kind == "anchor":
            commands.append(f"anchor {checkpoint.target}")
        elif checkpoint.kind == "selector":
            target = checkpoint.target or ""
            raise ArtifactError(
                f"candidate navigation cannot resolve selector {target!r}"
            )
        elif checkpoint.kind == "text":
            raise ArtifactError(
                f"candidate navigation cannot resolve text {checkpoint.target!r}"
            )
        elif checkpoint.kind == "bottom":
            commands.append("bottom")
    commands.append("quit")
    return "\n".join(commands) + "\n"


def summary_header() -> tuple[str, ...]:
    return (
        "scenario",
        "status",
        "detail",
        "reference_proof",
        "trace_sha256",
        "device_viewport",
        "css_viewport",
        "scale",
        "checkpoints",
    )


def summary_row(scenario: Scenario, result: Eligibility) -> tuple[str, ...]:
    return (
        scenario.scenario,
        result.status,
        ";".join(result.details),
        result.reference_proof,
        result.trace_sha256,
        f"{scenario.device_width}x{scenario.device_height}",
        f"{scenario.css_width}x{scenario.css_height}",
        f"{scenario.scale_numerator}/{scenario.scale_denominator}",
        str(result.checkpoint_count),
    )


def write_summary(path: Path, rows: Iterable[tuple[str, ...]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as destination:
        writer = csv.writer(destination, delimiter="\t", lineterminator="\n")
        writer.writerow(summary_header())
        writer.writerows(rows)
