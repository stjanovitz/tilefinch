#!/usr/bin/env python3
"""Acquire an exact GET/HEAD plan into a new, atomically published trace.

The input diagnostic and its network authority are digest pinned by a small
explicit contract.  This tool never discovers URLs, follows redirects, sends
credentials, mutates the source trace, or publishes a partial output trace.
"""

from __future__ import annotations

import argparse
import ctypes
import email.utils
import errno
import hashlib
import json
import os
import re
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any, Iterable
from urllib.parse import urlsplit


PLAN_MODE = "exact-get-head-plan-v1"
ALLOWLIST_DOMAIN = b"tilefinch-exact-get-head-allowlist-v1\0"
TRACE_DIGEST_DOMAIN = b"tilefinch-http-trace-v1\0"
SHA256 = re.compile(r"[0-9a-f]{64}\Z")
TRACE_RECORD = re.compile(r"([0-9]{4})\.(meta|body)\Z")
HEADER_NAME = re.compile(r"[!#$%&'*+.^_`|~0-9A-Za-z-]+\Z")
COOKIE_NAME = re.compile(r"[!#$%&'*+\-.^_`|~0-9A-Za-z]+\Z")
MAX_JSON_BYTES = 1024 * 1024
MAX_METADATA_BYTES = 1024 * 1024
MAX_URL_BYTES = 2047
MAX_RECORDS = 4096
HARD_MAX_RESPONSE_BYTES = 64 * 1024 * 1024
HARD_MAX_TRACE_BYTES = 512 * 1024 * 1024
OUTPUT_CAPTURE_LIMIT = 64 * 1024
ECMASCRIPT_DATE_MAX_MS = 8_640_000_000_000_000
REFERENCE_INSPECTOR = Path(__file__).with_name("capture-reference.js")
REFERENCE_CLOSURE = Path(__file__).with_name("verify-trace-acquisition.js")
REFERENCE_ROUTE_SELECTION = "ranked-occurrence-v2"
DIAGNOSTIC_MULTISET_DOMAIN = b"tilefinch-diagnostic-multiset-v1\0"
MAX_DIAGNOSTIC_ENTRIES = 512
MAX_DIAGNOSTIC_COUNTER_KEYS = 64
MAX_DIAGNOSTIC_EVENTS = 1_000_000

CURRENT_TRACE_VERSION = "11"
CURRENT_REQUIRED_KEYS = frozenset(
    {
        "psp-http-trace",
        "cookie-values",
        "method",
        "url",
        "logical-request-url",
        "success",
        "async-delay-pumps",
        "external-cancel",
        "transport-timeout",
        "redirect-origin-tainted",
        "error",
        "request-body-length",
        "request-body-hash",
        "request-content-type",
        "request-cookie-bytes",
        "request-has-cf-clearance",
        "request-extra-header-bytes",
        "request-extra-header-shape",
        "request-allow-http-errors",
        "request-enforce-cors",
        "request-redirect-same-origin-only",
        "request-cors-cached-response-validated",
        "request-if-none-match",
        "request-if-modified-since",
        "request-referer",
        "request-origin",
        "request-accept",
        "request-sec-fetch-dest",
        "request-sec-fetch-mode",
        "request-sec-fetch-site",
        "request-send-client-hints",
        "request-client-hint-tokens",
        "request-client-hint-origin",
        "request-send-low-client-hints",
        "request-sec-fetch-user",
        "request-upgrade-insecure",
        "request-user-agent",
        "request-diagnostic-mobile-safari",
        "request-credentials",
        "request-credential-origin",
        "request-initiator-url",
        "request-referrer-source",
        "request-referrer-policy",
        "status",
        "length",
        "response-body-hash",
        "effective-url",
        "content-type",
        "etag",
        "last-modified",
        "cf-mitigated",
        "accept-ch",
        "critical-ch",
        "server",
        "cf-ray",
        "response-referrer-policy-metadata-valid",
        "response-referrer-policy-present",
        "response-referrer-policy",
        "response-security-headers-truncated",
        "response-security-metadata",
        "response-security-allow-origin",
        "response-header-count",
        "set-cookie-count",
    }
)


class AcquisitionError(RuntimeError):
    pass


def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise AcquisitionError(f"duplicate JSON key {key!r}")
        result[key] = value
    return result


def _regular_file(path: Path, label: str, maximum: int | None = None) -> int:
    try:
        info = path.lstat()
    except OSError as error:
        raise AcquisitionError(f"{label} is unavailable: {error}") from error
    if stat.S_ISLNK(info.st_mode) or not stat.S_ISREG(info.st_mode):
        raise AcquisitionError(f"{label} must be a regular non-symlink file")
    if maximum is not None and info.st_size > maximum:
        raise AcquisitionError(f"{label} exceeds {maximum} bytes")
    return info.st_size


def _load_json(path: Path, label: str) -> dict[str, Any]:
    _regular_file(path, label, MAX_JSON_BYTES)
    try:
        value = json.loads(
            path.read_text(encoding="utf-8"), object_pairs_hook=_unique_object
        )
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise AcquisitionError(f"cannot parse {label}: {error}") from error
    if not isinstance(value, dict):
        raise AcquisitionError(f"{label} must contain one JSON object")
    return value


def _file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _integer(value: Any, label: str, minimum: int, maximum: int) -> int:
    if type(value) is not int or value < minimum or value > maximum:
        raise AcquisitionError(f"{label} must be an integer in {minimum}..{maximum}")
    return value


def _digest(value: Any, label: str) -> str:
    if not isinstance(value, str) or SHA256.fullmatch(value) is None:
        raise AcquisitionError(f"{label} must be a lowercase SHA-256")
    return value


def _validate_contract(path: Path) -> dict[str, Any]:
    contract = _load_json(path, "acquisition contract")
    required = {
        "schema",
        "name",
        "diagnostic_sha256",
        "allowlist_sha256",
        "source_trace_sha256",
        "source_record_count",
        "source_origin_ms",
        "request_count",
        "max_response_bytes",
        "timeout_ms",
        "max_trace_bytes",
        "allowed_methods",
        "allowed_schemes",
        "allowed_origins",
    }
    if set(contract) != required:
        missing = sorted(required - set(contract))
        extra = sorted(set(contract) - required)
        raise AcquisitionError(
            f"acquisition contract keys differ (missing={missing}, extra={extra})"
        )
    if contract["schema"] != 1:
        raise AcquisitionError("acquisition contract schema must be 1")
    if not isinstance(contract["name"], str) or not contract["name"]:
        raise AcquisitionError("acquisition contract name is invalid")
    for key in ("diagnostic_sha256", "allowlist_sha256", "source_trace_sha256"):
        _digest(contract[key], key)
    _integer(contract["source_record_count"], "source_record_count", 1, MAX_RECORDS)
    _integer(
        contract["source_origin_ms"],
        "source_origin_ms",
        1,
        ECMASCRIPT_DATE_MAX_MS,
    )
    _integer(contract["request_count"], "request_count", 1, MAX_RECORDS)
    _integer(
        contract["max_response_bytes"],
        "max_response_bytes",
        1,
        HARD_MAX_RESPONSE_BYTES,
    )
    _integer(contract["timeout_ms"], "timeout_ms", 1, 120000)
    _integer(
        contract["max_trace_bytes"],
        "max_trace_bytes",
        1,
        HARD_MAX_TRACE_BYTES,
    )
    if contract["allowed_methods"] != ["GET", "HEAD"]:
        raise AcquisitionError("allowed_methods must be exactly GET, HEAD")
    if contract["allowed_schemes"] != ["https"]:
        raise AcquisitionError("allowed_schemes must be exactly https")
    origins = contract["allowed_origins"]
    if (
        not isinstance(origins, list)
        or not origins
        or origins != sorted(set(origins))
        or any(not isinstance(origin, str) for origin in origins)
    ):
        raise AcquisitionError("allowed_origins must be a sorted unique string list")
    return contract


def _url_authority(value: Any, label: str) -> tuple[str, str]:
    if not isinstance(value, str) or not value or len(value.encode("utf-8")) > MAX_URL_BYTES:
        raise AcquisitionError(f"{label} is missing or exceeds the URL bound")
    if not value.isascii() or any(ord(byte) <= 0x20 or ord(byte) == 0x7F for byte in value):
        raise AcquisitionError(f"{label} must be printable ASCII without whitespace")
    try:
        parsed = urlsplit(value)
        port = parsed.port
    except ValueError as error:
        raise AcquisitionError(f"{label} is malformed: {error}") from error
    if parsed.scheme != "https" or not parsed.hostname:
        raise AcquisitionError(f"{label} must be an absolute HTTPS URL")
    if parsed.username is not None or parsed.password is not None or parsed.fragment:
        raise AcquisitionError(f"{label} cannot contain credentials or a fragment")
    if "\\" in value:
        raise AcquisitionError(f"{label} cannot contain a backslash")
    host = parsed.hostname.lower()
    if port is None or port == 443:
        origin = f"https://{host}"
    else:
        origin = f"https://{host}:{port}"
    return value, origin


def _allowlist_digest(requests: Iterable[dict[str, Any]]) -> str:
    rows = sorted((request["method"], request["url"]) for request in requests)
    digest = hashlib.sha256()
    digest.update(ALLOWLIST_DOMAIN)
    digest.update(str(len(rows)).encode("ascii"))
    digest.update(b"\0")
    for method, url in rows:
        digest.update(method.encode("ascii"))
        digest.update(b"\0")
        digest.update(url.encode("utf-8"))
        digest.update(b"\n")
    return digest.hexdigest()


def _diagnostic_multiset_digest(entries: list[dict[str, Any]]) -> str:
    total = 0
    for entry in entries:
        event = json.dumps(
            [
                entry["classification"],
                entry["method"],
                entry["resource_type"],
                entry["url"],
            ],
            ensure_ascii=False,
            separators=(",", ":"),
        ).encode("utf-8")
        total = (total + int.from_bytes(hashlib.sha256(event).digest(), "big")) % (
            1 << 256
        )
    value = (
        DIAGNOSTIC_MULTISET_DOMAIN
        + str(len(entries)).encode("ascii")
        + b"\0"
        + f"{total:064x}".encode("ascii")
    )
    return hashlib.sha256(value).hexdigest()


def _bounded_diagnostic_counter(values: Iterable[str]) -> dict[str, int]:
    counter: dict[str, int] = {}
    for value in values:
        if value in counter:
            counter[value] += 1
        elif len(counter) < MAX_DIAGNOSTIC_COUNTER_KEYS:
            counter[value] = 1
        else:
            counter["__other__"] = counter.get("__other__", 0) + 1
    return counter


def _validate_diagnostic_counter(
    value: Any, expected: dict[str, int], label: str
) -> None:
    if not isinstance(value, dict) or len(value) > MAX_DIAGNOSTIC_COUNTER_KEYS + 1:
        raise AcquisitionError(f"{label} is invalid")
    for key, count in value.items():
        if (
            not isinstance(key, str)
            or not key
            or len(key) > 256
            or type(count) is not int
            or count < 1
            or count > MAX_DIAGNOSTIC_EVENTS
        ):
            raise AcquisitionError(f"{label} is invalid")
    if value != expected:
        raise AcquisitionError(f"{label} does not exactly match retained entries")


def _validate_diagnostic_plan_binding(
    diagnostic: dict[str, Any],
    requests: list[dict[str, Any]],
    contract: dict[str, Any],
) -> None:
    if diagnostic.get("capture_transport") != "cdp-response-keyed":
        raise AcquisitionError("reference diagnostic capture_transport is not cdp-response-keyed")
    if diagnostic.get("trace_sha256") != contract["source_trace_sha256"]:
        raise AcquisitionError("reference diagnostic trace_sha256 does not bind the source trace")
    environment = diagnostic.get("replay_environment")
    if (
        not isinstance(environment, dict)
        or environment.get("origin_ms") != contract["source_origin_ms"]
    ):
        raise AcquisitionError("reference diagnostic origin_ms does not bind the source trace")
    ledger = diagnostic.get("replay_ledger")
    if not isinstance(ledger, dict):
        raise AcquisitionError("reference diagnostic has no replay_ledger")
    if ledger.get("mode") != "cdp-response-keyed":
        raise AcquisitionError("reference diagnostic replay_ledger mode is not cdp-response-keyed")
    if ledger.get("route_selection_version") != REFERENCE_ROUTE_SELECTION:
        raise AcquisitionError(
            "reference diagnostic route_selection_version is not ranked-occurrence-v2"
        )

    count_limits = {
        "records": MAX_RECORDS,
        "claimed": MAX_RECORDS,
        "requests": MAX_DIAGNOSTIC_EVENTS,
        "matched": MAX_DIAGNOSTIC_EVENTS,
        "served": MAX_DIAGNOSTIC_EVENTS,
        "rejected": MAX_DIAGNOSTIC_EVENTS,
        "unmatched": MAX_DIAGNOSTIC_EVENTS,
        "conflicts": MAX_DIAGNOSTIC_EVENTS,
        "invalid": MAX_DIAGNOSTIC_EVENTS,
        "occurrence_claims": MAX_DIAGNOSTIC_EVENTS,
        "reusable_claims": MAX_DIAGNOSTIC_EVENTS,
        "occurrence_exhausted": MAX_DIAGNOSTIC_EVENTS,
    }
    counts = {
        name: _integer(ledger.get(name), f"replay_ledger.{name}", 0, maximum)
        for name, maximum in count_limits.items()
    }
    if counts["records"] != contract["source_record_count"]:
        raise AcquisitionError("replay_ledger.records does not bind the source trace")
    claimed_routes = ledger.get("claimed_routes")
    if (
        not isinstance(claimed_routes, list)
        or len(claimed_routes) != counts["claimed"]
        or any(
            type(route) is not int or route < 0 or route >= counts["records"]
            for route in claimed_routes
        )
        or claimed_routes != sorted(set(claimed_routes))
    ):
        raise AcquisitionError("replay_ledger.claimed_routes is inconsistent")
    if (
        counts["claimed"] > counts["matched"]
        or counts["matched"] > counts["requests"]
        or counts["matched"] != counts["served"] + counts["rejected"]
        or counts["requests"]
        != counts["matched"]
        + counts["unmatched"]
        + counts["conflicts"]
        + counts["invalid"]
    ):
        raise AcquisitionError("replay_ledger terminal counts are inconsistent")
    occurrence = counts["occurrence_claims"]
    reusable = counts["reusable_claims"]
    if (
        occurrence + reusable != counts["matched"]
        or counts["occurrence_exhausted"] != 0
        or (counts["matched"] > 0 and counts["claimed"] == 0)
        or occurrence + (1 if reusable > 0 else 0) > counts["claimed"]
        or counts["claimed"] > occurrence + reusable
    ):
        raise AcquisitionError("replay_ledger route claims are inconsistent")
    if counts["conflicts"] != 0 or counts["invalid"] != 0:
        raise AcquisitionError(
            "acquisition requires exact unmatched evidence without conflicts or invalid routes"
        )

    summary = ledger.get("unexpected_requests")
    summary_keys = {
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
    if not isinstance(summary, dict) or set(summary) != summary_keys:
        raise AcquisitionError("replay_ledger.unexpected_requests has an unsupported shape")
    entries = summary.get("entries")
    if not isinstance(entries, list) or len(entries) > MAX_DIAGNOSTIC_ENTRIES:
        raise AcquisitionError("unexpected request entries are invalid")
    total = _integer(
        summary.get("total"),
        "replay_ledger.unexpected_requests.total",
        1,
        MAX_DIAGNOSTIC_EVENTS,
    )
    retained = _integer(
        summary.get("retained"),
        "replay_ledger.unexpected_requests.retained",
        0,
        MAX_DIAGNOSTIC_ENTRIES,
    )
    truncated = _integer(
        summary.get("truncated"),
        "replay_ledger.unexpected_requests.truncated",
        0,
        MAX_DIAGNOSTIC_EVENTS,
    )
    if (
        summary.get("overflow") is not False
        or total != retained
        or retained != len(entries)
        or truncated != 0
        or counts["unmatched"] != total
    ):
        raise AcquisitionError(
            "unexpected request evidence is incomplete or inconsistent with the replay ledger"
        )

    validated_entries: list[dict[str, Any]] = []
    origins: list[str] = []
    entry_keys = {
        "classification",
        "method",
        "resource_type",
        "url",
        "url_sha256",
        "url_truncated",
    }
    for index, entry in enumerate(entries):
        label = f"replay_ledger.unexpected_requests.entries[{index}]"
        if not isinstance(entry, dict) or set(entry) != entry_keys:
            raise AcquisitionError(f"{label} has an unsupported shape")
        if entry.get("classification") != "unmatched":
            raise AcquisitionError(f"{label} is not an exact unmatched request")
        method = entry.get("method")
        if method not in ("GET", "HEAD"):
            raise AcquisitionError(f"{label}.method is not read-only GET/HEAD")
        resource_type = entry.get("resource_type")
        if (
            not isinstance(resource_type, str)
            or not resource_type
            or len(resource_type) > 64
            or not resource_type.isascii()
            or resource_type != resource_type.lower()
        ):
            raise AcquisitionError(f"{label}.resource_type is not canonical")
        if entry.get("url_truncated") is not False:
            raise AcquisitionError(f"{label}.url is truncated")
        url, origin = _url_authority(entry.get("url"), f"{label}.url")
        if entry.get("url_sha256") != hashlib.sha256(url.encode("utf-8")).hexdigest():
            raise AcquisitionError(f"{label}.url_sha256 does not match its URL")
        validated_entries.append(entry)
        origins.append(origin)

    _digest(
        summary.get("multiset_sha256"),
        "replay_ledger.unexpected_requests.multiset_sha256",
    )
    if summary["multiset_sha256"] != _diagnostic_multiset_digest(validated_entries):
        raise AcquisitionError("unexpected request multiset SHA-256 is inconsistent")
    _validate_diagnostic_counter(
        summary.get("by_method"),
        _bounded_diagnostic_counter(entry["method"] for entry in validated_entries),
        "replay_ledger.unexpected_requests.by_method",
    )
    _validate_diagnostic_counter(
        summary.get("by_resource_type"),
        _bounded_diagnostic_counter(
            entry["resource_type"] for entry in validated_entries
        ),
        "replay_ledger.unexpected_requests.by_resource_type",
    )
    _validate_diagnostic_counter(
        summary.get("by_origin"),
        _bounded_diagnostic_counter(origins),
        "replay_ledger.unexpected_requests.by_origin",
    )

    projected: dict[tuple[str, str], dict[str, Any]] = {}
    for entry in validated_entries:
        key = (entry["method"], entry["url"])
        if key not in projected:
            projected[key] = {
                "method": entry["method"],
                "url": entry["url"],
                "url_sha256": entry["url_sha256"],
                "resource_types": set(),
                "occurrences": 0,
            }
        projected[key]["resource_types"].add(entry["resource_type"])
        projected[key]["occurrences"] += 1
    expected_requests = []
    for key in sorted(projected):
        request = projected[key]
        expected_requests.append(
            {
                "method": request["method"],
                "url": request["url"],
                "url_sha256": request["url_sha256"],
                "resource_types": sorted(request["resource_types"]),
                "occurrences": request["occurrences"],
            }
        )
    if requests != expected_requests:
        raise AcquisitionError(
            "acquisition_plan is not the exact canonical projection of unmatched evidence"
        )


def _validate_plan(
    diagnostic_path: Path, contract: dict[str, Any]
) -> list[dict[str, Any]]:
    actual = _file_sha256(diagnostic_path)
    if actual != contract["diagnostic_sha256"]:
        raise AcquisitionError(
            f"diagnostic SHA-256 mismatch: expected {contract['diagnostic_sha256']}, got {actual}"
        )
    diagnostic = _load_json(diagnostic_path, "reference diagnostic")
    plan = diagnostic.get("acquisition_plan")
    if not isinstance(plan, dict):
        raise AcquisitionError("reference diagnostic has no acquisition_plan")
    if set(plan) != {"mode", "complete", "request_count", "unplannable", "requests"}:
        raise AcquisitionError("acquisition_plan has an unsupported shape")
    requests = plan.get("requests")
    if (
        plan.get("mode") != PLAN_MODE
        or plan.get("complete") is not True
        or plan.get("unplannable") != 0
        or not isinstance(requests, list)
        or plan.get("request_count") != len(requests)
        or len(requests) != contract["request_count"]
    ):
        raise AcquisitionError("acquisition_plan is incomplete or has the wrong count")

    normalized: list[dict[str, Any]] = []
    seen: set[tuple[str, str]] = set()
    origins: set[str] = set()
    for index, request in enumerate(requests):
        label = f"acquisition_plan.requests[{index}]"
        if not isinstance(request, dict) or set(request) != {
            "method",
            "url",
            "url_sha256",
            "resource_types",
            "occurrences",
        }:
            raise AcquisitionError(f"{label} has an unsupported shape")
        method = request["method"]
        if method not in contract["allowed_methods"]:
            raise AcquisitionError(f"{label}.method is not authorized")
        url, origin = _url_authority(request["url"], f"{label}.url")
        url_sha = hashlib.sha256(url.encode("utf-8")).hexdigest()
        if request["url_sha256"] != url_sha:
            raise AcquisitionError(f"{label}.url_sha256 does not match its URL")
        resource_types = request["resource_types"]
        if (
            not isinstance(resource_types, list)
            or not resource_types
            or resource_types != sorted(set(resource_types))
            or any(not isinstance(item, str) or not item for item in resource_types)
        ):
            raise AcquisitionError(f"{label}.resource_types is invalid")
        _integer(request["occurrences"], f"{label}.occurrences", 1, MAX_RECORDS)
        key = (method, url)
        if key in seen:
            raise AcquisitionError(f"{label} duplicates an authorized request")
        seen.add(key)
        origins.add(origin)
        normalized.append(request)
    if [(item["method"], item["url"]) for item in normalized] != sorted(seen):
        raise AcquisitionError("acquisition requests are not in canonical order")
    if sorted(origins) != contract["allowed_origins"]:
        raise AcquisitionError("plan origins do not exactly match the contract")
    actual_allowlist = _allowlist_digest(normalized)
    if actual_allowlist != contract["allowlist_sha256"]:
        raise AcquisitionError(
            "allowlist SHA-256 mismatch: expected "
            f"{contract['allowlist_sha256']}, got {actual_allowlist}"
        )
    _validate_diagnostic_plan_binding(diagnostic, normalized, contract)
    return normalized


def _metadata(path: Path) -> tuple[dict[str, str], bytes]:
    _regular_file(path, str(path), MAX_METADATA_BYTES)
    try:
        raw = path.read_bytes()
        text = raw.decode("utf-8")
    except (OSError, UnicodeError) as error:
        raise AcquisitionError(f"cannot read metadata {path}: {error}") from error
    result: dict[str, str] = {}
    for line_number, line in enumerate(text.splitlines(), 1):
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            raise AcquisitionError(f"{path}:{line_number}: expected key=value")
        key, value = line.split("=", 1)
        if not key or key in result:
            raise AcquisitionError(f"{path}:{line_number}: duplicate or empty key")
        result[key] = value
    return result, raw


def _decimal(meta: dict[str, str], key: str, maximum: int) -> int:
    value = meta.get(key, "")
    if not value.isdigit():
        raise AcquisitionError(f"missing or invalid {key}")
    parsed = int(value, 10)
    if parsed > maximum:
        raise AcquisitionError(f"{key} exceeds {maximum}")
    return parsed


def _trace_files(root: Path) -> list[tuple[Path, str, int]]:
    try:
        root_info = root.lstat()
    except OSError as error:
        raise AcquisitionError(f"trace directory is unavailable: {error}") from error
    if stat.S_ISLNK(root_info.st_mode) or not stat.S_ISDIR(root_info.st_mode):
        raise AcquisitionError("trace root must be a non-symlink directory")
    files: list[tuple[Path, str, int]] = []

    def visit(directory: Path, prefix: str) -> None:
        try:
            children = sorted(directory.iterdir(), key=lambda item: item.name)
        except OSError as error:
            raise AcquisitionError(f"cannot enumerate trace: {error}") from error
        for child in children:
            relative = f"{prefix}/{child.name}" if prefix else child.name
            info = child.lstat()
            if stat.S_ISLNK(info.st_mode):
                raise AcquisitionError(f"trace contains symlink {relative}")
            if stat.S_ISDIR(info.st_mode):
                visit(child, relative)
            elif stat.S_ISREG(info.st_mode):
                files.append((child, relative, info.st_size))
            else:
                raise AcquisitionError(f"trace contains non-regular entry {relative}")

    visit(root, "")
    return files


def trace_digest(root: Path, maximum_bytes: int) -> tuple[str, int]:
    files = _trace_files(root)
    total = sum(size for _path, _relative, size in files)
    if total > maximum_bytes:
        raise AcquisitionError(f"trace exceeds {maximum_bytes} bytes")
    digest = hashlib.sha256()
    digest.update(TRACE_DIGEST_DOMAIN)
    for path, relative, size in files:
        encoded = relative.encode("utf-8")
        if len(encoded) > 0xFFFFFFFF:
            raise AcquisitionError("trace relative path is too long")
        digest.update(struct.pack(">I", len(encoded)))
        digest.update(encoded)
        digest.update(struct.pack(">Q", size))
        with path.open("rb") as source:
            for chunk in iter(lambda: source.read(1024 * 1024), b""):
                digest.update(chunk)
    return digest.hexdigest(), total


def _trace_inventory(root: Path) -> tuple[dict[str, str], bytes, set[tuple[str, str]]]:
    info, raw_info = _metadata(root / "trace.meta")
    if info.get("psp-http-trace-clock") != "1" or info.get("capture-complete") != "yes":
        raise AcquisitionError("trace.meta lacks finalized clock metadata")
    origin = _decimal(info, "origin-ms", ECMASCRIPT_DATE_MAX_MS)
    if origin == 0:
        raise AcquisitionError("trace origin-ms must be positive")
    count = _decimal(info, "record-count", MAX_RECORDS)
    if count == 0:
        raise AcquisitionError("trace record-count must be positive")
    expected = {"trace.meta"}
    keys: set[tuple[str, str]] = set()
    for sequence in range(count):
        stem = f"{sequence:04d}"
        expected.update({f"{stem}.meta", f"{stem}.body"})
        meta, _raw = _metadata(root / f"{stem}.meta")
        method = meta.get("method", "")
        url = meta.get("url", "")
        if not method or not url:
            raise AcquisitionError(f"trace record {stem} has no method/URL")
        keys.add((method, url))
        _regular_file(root / f"{stem}.body", f"trace body {stem}")
    actual = {relative for _path, relative, _size in _trace_files(root)}
    if actual != expected:
        raise AcquisitionError(
            f"trace inventory differs (missing={sorted(expected - actual)}, "
            f"extra={sorted(actual - expected)})"
        )
    return info, raw_info, keys


def _fnv1a64(body: bytes) -> str:
    value = 0xCBF29CE484222325
    for byte in body:
        value ^= byte
        value = (value * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return f"{value:016x}"


def _cookie_date(value: str, label: str) -> None:
    try:
        parsed = email.utils.parsedate_to_datetime(value)
    except (TypeError, ValueError) as error:
        raise AcquisitionError(f"{label} is not a valid HTTP date") from error
    if parsed is None or parsed.tzinfo is None:
        raise AcquisitionError(f"{label} is not a timezone-qualified HTTP date")


def _validate_response_cookies(
    meta: dict[str, str], url: str, headers: list[tuple[str, str]]
) -> None:
    count = _decimal(meta, "set-cookie-count", 16)
    parsed_url = urlsplit(url)
    response_dates = [value for name, value in headers if name == "date"]
    for index in range(count):
        cookie_key = f"set-cookie-{index}"
        url_key = f"set-cookie-url-{index}"
        if cookie_key not in meta or meta.get(url_key) != url:
            raise AcquisitionError("response cookie indices or source URL are invalid")
        serialized = meta[cookie_key]
        if len(serialized.encode("utf-8")) >= 4096 or ";" in serialized.split("=", 1)[0]:
            raise AcquisitionError("response cookie exceeds its syntax bound")
        parts = [part.strip() for part in serialized.split(";")]
        if not parts or "=" not in parts[0]:
            raise AcquisitionError("response cookie lacks a name/value")
        name, value = parts[0].split("=", 1)
        if COOKIE_NAME.fullmatch(name) is None:
            raise AcquisitionError("response cookie name is invalid")
        if value.startswith('"') or value.endswith('"'):
            if len(value) < 2 or not (value.startswith('"') and value.endswith('"')):
                raise AcquisitionError("redacted quoted cookie value is malformed")
            redacted = value[1:-1]
        else:
            redacted = value
        if any(character != "x" for character in redacted):
            raise AcquisitionError("response cookie value is not deterministically redacted")
        attributes: dict[str, str | None] = {}
        for part in parts[1:]:
            if not part:
                raise AcquisitionError("response cookie has an empty attribute")
            if "=" in part:
                attribute, attribute_value = part.split("=", 1)
                attribute_value = attribute_value.strip()
            else:
                attribute, attribute_value = part, None
            lower = attribute.strip().lower()
            if lower in attributes:
                raise AcquisitionError("response cookie repeats an attribute")
            if lower not in {
                "domain",
                "path",
                "secure",
                "httponly",
                "samesite",
                "max-age",
                "expires",
            }:
                raise AcquisitionError(f"unsupported response cookie attribute {attribute!r}")
            if lower in {"secure", "httponly"} and attribute_value is not None:
                raise AcquisitionError(f"response cookie {attribute} must be a flag")
            if lower not in {"secure", "httponly"} and not attribute_value:
                raise AcquisitionError(f"response cookie {attribute} requires a value")
            attributes[lower] = attribute_value
        domain = attributes.get("domain")
        if domain is not None:
            canonical = domain.lstrip(".").lower()
            host = (parsed_url.hostname or "").lower()
            if not canonical or not (host == canonical or host.endswith("." + canonical)):
                raise AcquisitionError("response cookie Domain does not match its URL")
        path = attributes.get("path")
        if path is not None and not path.startswith("/"):
            raise AcquisitionError("response cookie Path must be absolute")
        same_site = attributes.get("samesite")
        if same_site is not None and same_site.lower() not in {"strict", "lax", "none"}:
            raise AcquisitionError("response cookie SameSite value is invalid")
        secure = "secure" in attributes
        if same_site is not None and same_site.lower() == "none" and not secure:
            raise AcquisitionError("SameSite=None response cookie is not Secure")
        if name.startswith("__Secure-") and not secure:
            raise AcquisitionError("__Secure- response cookie is not Secure")
        if name.startswith("__Host-") and (
            not secure or domain is not None or attributes.get("path") != "/"
        ):
            raise AcquisitionError("__Host- response cookie scope is invalid")
        max_age = attributes.get("max-age")
        if max_age is not None and re.fullmatch(r"-?[0-9]+", max_age) is None:
            raise AcquisitionError("response cookie Max-Age is invalid")
        expires = attributes.get("expires")
        if expires is not None:
            _cookie_date(expires, "response cookie Expires")
            if max_age is None:
                if len(response_dates) != 1:
                    raise AcquisitionError(
                        "an acquired Expires-only cookie requires exactly one response Date"
                    )
                _cookie_date(response_dates[0], "response Date")
    cookie_keys = {
        key
        for key in meta
        if re.fullmatch(r"set-cookie-(?:url-)?[0-9]+", key)
    }
    expected = {
        key
        for index in range(count)
        for key in (f"set-cookie-{index}", f"set-cookie-url-{index}")
    }
    if cookie_keys != expected:
        raise AcquisitionError("response cookie field count or sequence is inconsistent")


def _validate_acquired_record(
    root: Path, request: dict[str, Any], maximum_bytes: int
) -> None:
    info, _raw_info, _keys = _trace_inventory(root)
    if info.get("record-count") != "1":
        raise AcquisitionError("isolated recorder did not publish exactly one record")
    meta, _raw = _metadata(root / "0000.meta")
    missing = sorted(CURRENT_REQUIRED_KEYS - set(meta))
    if missing:
        raise AcquisitionError(
            f"acquired v{CURRENT_TRACE_VERSION} record is missing fields: {missing}"
        )
    method = request["method"]
    url = request["url"]
    exact = {
        "psp-http-trace": CURRENT_TRACE_VERSION,
        "cookie-values": "redacted",
        "method": method,
        "url": url,
        "logical-request-url": url,
        "success": "1",
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
        "request-diagnostic-mobile-safari": "0",
        "request-credentials": "0",
        "request-credential-origin": "",
        "request-initiator-url": "",
        "request-referrer-source": "",
        "request-referrer-policy": "",
        "effective-url": url,
    }
    for key, expected in exact.items():
        if meta.get(key) != expected:
            raise AcquisitionError(
                f"acquired record {key} differs: expected {expected!r}, got {meta.get(key)!r}"
            )
    if not meta.get("request-user-agent"):
        raise AcquisitionError("acquired record has no deterministic User-Agent")
    status_code = _decimal(meta, "status", 599)
    if status_code < 200:
        raise AcquisitionError("acquired record has no terminal HTTP status")
    length = _decimal(meta, "length", maximum_bytes)
    body_path = root / "0000.body"
    if _regular_file(body_path, "acquired body", maximum_bytes) != length:
        raise AcquisitionError("acquired response length does not match its body")
    body = body_path.read_bytes()
    if meta.get("response-body-hash") != _fnv1a64(body):
        raise AcquisitionError("acquired response body hash is invalid")
    if method == "HEAD" and length != 0:
        raise AcquisitionError("HEAD acquisition retained a response body")
    _decimal(meta, "async-delay-pumps", 1_000_000)
    header_count = _decimal(meta, "response-header-count", 512)
    response_headers: list[tuple[str, str]] = []
    for index in range(header_count):
        line = meta.get(f"response-header-{index}")
        if line is None or ":" not in line:
            raise AcquisitionError("acquired response headers are not contiguous")
        name, value = line.split(":", 1)
        if HEADER_NAME.fullmatch(name.strip()) is None or "\r" in value or "\n" in value:
            raise AcquisitionError("acquired response header is unsafe")
        if name.strip().lower() in ("set-cookie", "set-cookie2"):
            raise AcquisitionError("response cookies escaped structured redaction")
        response_headers.append((name.strip().lower(), value.strip()))
    indexed = [
        key for key in meta if re.fullmatch(r"response-header-[0-9]+", key)
    ]
    if len(indexed) != header_count:
        raise AcquisitionError("acquired response-header-count is inconsistent")
    _validate_response_cookies(meta, url, response_headers)
    policy_valid = meta.get("response-referrer-policy-metadata-valid")
    policy_present = meta.get("response-referrer-policy-present")
    policy_value = meta.get("response-referrer-policy", "")
    if policy_valid != "1" or policy_present not in ("0", "1"):
        raise AcquisitionError("acquired referrer-policy metadata is invalid")
    if (policy_present == "0") != (policy_value == ""):
        raise AcquisitionError("acquired referrer-policy shape is inconsistent")


def _copy_regular(source: Path, target: Path) -> None:
    _regular_file(source, str(source))
    flags = os.O_RDONLY
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    source_fd = os.open(source, flags)
    try:
        target_fd = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        try:
            while True:
                chunk = os.read(source_fd, 1024 * 1024)
                if not chunk:
                    break
                view = memoryview(chunk)
                while view:
                    written = os.write(target_fd, view)
                    if written <= 0:
                        raise AcquisitionError(f"short write while copying {target}")
                    view = view[written:]
            os.fsync(target_fd)
        finally:
            os.close(target_fd)
    finally:
        os.close(source_fd)


def _fsync_directory(path: Path) -> None:
    descriptor = os.open(path, os.O_RDONLY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def _fsync_file(path: Path) -> None:
    flags = os.O_RDONLY
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    descriptor = os.open(path, flags)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def _publish_new_directory(staging: Path, output: Path) -> None:
    """Atomically rename staging without replacing an output created by a race."""
    library = ctypes.CDLL(None, use_errno=True)
    source = os.fsencode(staging)
    target = os.fsencode(output)
    result: int
    if sys.platform == "darwin" and hasattr(library, "renamex_np"):
        rename_exclusive = 0x00000004
        function = library.renamex_np
        function.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint]
        function.restype = ctypes.c_int
        result = function(source, target, rename_exclusive)
    elif sys.platform.startswith("linux") and hasattr(library, "renameat2"):
        at_fdcwd = -100
        rename_no_replace = 1
        function = library.renameat2
        function.argtypes = [
            ctypes.c_int,
            ctypes.c_char_p,
            ctypes.c_int,
            ctypes.c_char_p,
            ctypes.c_uint,
        ]
        function.restype = ctypes.c_int
        result = function(
            at_fdcwd, source, at_fdcwd, target, rename_no_replace
        )
    else:
        raise AcquisitionError(
            "atomic no-replace directory publication is unavailable on this host"
        )
    if result != 0:
        number = ctypes.get_errno()
        if number in (errno.EEXIST, errno.ENOTEMPTY):
            raise AcquisitionError("output trace appeared during publication")
        raise AcquisitionError(
            f"atomic trace publication failed: {os.strerror(number)}"
        )


def _rewrite_record_count(raw: bytes, previous: int, current: int) -> bytes:
    try:
        text = raw.decode("utf-8")
    except UnicodeError as error:
        raise AcquisitionError(f"trace.meta is not UTF-8: {error}") from error
    lines = text.splitlines(keepends=True)
    found = 0
    rewritten: list[str] = []
    for line in lines:
        body = line.rstrip("\r\n")
        ending = line[len(body) :]
        if body.startswith("record-count="):
            found += 1
            if body != f"record-count={previous}":
                raise AcquisitionError("trace.meta record-count changed during staging")
            line = f"record-count={current}{ending}"
        rewritten.append(line)
    if found != 1:
        raise AcquisitionError("trace.meta must contain one record-count")
    return "".join(rewritten).encode("utf-8")


def _safe_environment() -> dict[str, str]:
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
        environment.pop(name, None)
    return environment


def _bounded_command(
    command: list[str], label: str, timeout: float,
    standard_input: bytes | None = None,
) -> bytes:
    try:
        completed = subprocess.run(
            command,
            check=False,
            env=_safe_environment(),
            input=standard_input,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise AcquisitionError(f"{label} failed to run: {error}") from error
    stdout = completed.stdout[:OUTPUT_CAPTURE_LIMIT]
    stderr = completed.stderr[:OUTPUT_CAPTURE_LIMIT]
    if (
        completed.returncode != 0
        or len(completed.stdout) > OUTPUT_CAPTURE_LIMIT
        or len(completed.stderr) > OUTPUT_CAPTURE_LIMIT
    ):
        detail = stderr.decode("utf-8", "replace").strip()
        if not detail:
            detail = stdout.decode("utf-8", "replace").strip()
        raise AcquisitionError(
            f"{label} exited {completed.returncode}: {detail[:1000]}"
        )
    return stdout


def _node_executable() -> Path:
    resolved = shutil.which("node", path=_safe_environment().get("PATH"))
    if resolved is None:
        raise AcquisitionError("Node.js is required for authoritative trace inspection")
    executable = Path(resolved).resolve(strict=True)
    _regular_file(executable, "Node.js executable")
    if not os.access(executable, os.X_OK):
        raise AcquisitionError("Node.js executable is not executable")
    return executable


def _inspect_reference_trace(node: Path, trace: Path) -> dict[str, Any]:
    _regular_file(REFERENCE_INSPECTOR, "reference trace inspector", MAX_METADATA_BYTES)
    raw = _bounded_command(
        [str(node), str(REFERENCE_INSPECTOR), "--inspect-trace", str(trace)],
        "authoritative reference trace inspector",
        # Local subprocess safety cap only; a cold Node.js start on a loaded
        # host can take tens of seconds.  Matches the 120 s reference-host
        # operation limit in visual_scenario.py.
        120,
    )
    try:
        inspected = json.loads(
            raw.decode("utf-8"), object_pairs_hook=_unique_object
        )
    except (UnicodeError, json.JSONDecodeError) as error:
        raise AcquisitionError(
            f"authoritative reference inspector returned invalid JSON: {error}"
        ) from error
    required = {
        "trace_dir",
        "trace_sha256",
        "records",
        "routes",
        "ambiguous_routes",
        "occurrence_routes",
        "route_selection_version",
        "response_cookies",
        "clock_origin_ms",
        "urls",
    }
    if not isinstance(inspected, dict) or set(inspected) != required:
        raise AcquisitionError("authoritative reference inspection shape changed")
    for key in (
        "records",
        "routes",
        "ambiguous_routes",
        "occurrence_routes",
        "response_cookies",
        "clock_origin_ms",
    ):
        maximum = (
            ECMASCRIPT_DATE_MAX_MS if key == "clock_origin_ms" else 2**63 - 1
        )
        _integer(inspected[key], f"reference inspection {key}", 0, maximum)
    if inspected["route_selection_version"] != REFERENCE_ROUTE_SELECTION:
        raise AcquisitionError("authoritative route-selection version changed")
    if (
        not isinstance(inspected["urls"], list)
        or inspected["urls"] != sorted(set(inspected["urls"]))
        or any(not isinstance(url, str) for url in inspected["urls"])
    ):
        raise AcquisitionError("authoritative reference route URL list is invalid")
    _digest(inspected["trace_sha256"], "reference inspection trace_sha256")
    if Path(inspected["trace_dir"]).resolve(strict=True) != trace.resolve(strict=True):
        raise AcquisitionError("authoritative reference inspector opened another trace")
    return inspected


def _require_inspection_authority(
    inspected: dict[str, Any], expected_digest: str,
    expected_count: int, expected_origin: int,
) -> None:
    if (
        inspected["trace_sha256"] != expected_digest
        or inspected["records"] != expected_count
        or inspected["clock_origin_ms"] != expected_origin
        or inspected["ambiguous_routes"] > inspected["routes"]
        or inspected["occurrence_routes"] < inspected["ambiguous_routes"]
    ):
        raise AcquisitionError(
            "authoritative reference inspection differs from count/origin/digest authority"
        )


def _run_native_inventory(
    executable: Path, trace: Path, expected_count: int
) -> None:
    executable = executable.resolve(strict=True)
    _regular_file(executable, "native response-keyed inventory tool")
    if not os.access(executable, os.X_OK):
        raise AcquisitionError("native response-keyed inventory tool is not executable")
    output = _bounded_command(
        [
            str(executable),
            "--response-keyed",
            str(trace),
            "--expect-records",
            str(expected_count),
        ],
        "native response-keyed inventory",
        30,
    )
    expected = (
        f"trace-inventory-ok mode=response-keyed records={expected_count}\n"
    ).encode("ascii")
    if output != expected:
        raise AcquisitionError("native response-keyed inventory output is invalid")


def _run_route_closure(
    node: Path, source: Path, merged: Path,
    source_digest: str, merged_digest: str,
    source_count: int, merged_count: int, origin_ms: int,
    requests: list[dict[str, Any]],
) -> dict[str, Any]:
    _regular_file(REFERENCE_CLOSURE, "trace acquisition closure", MAX_METADATA_BYTES)
    plan = json.dumps(
        {
            "requests": [
                {"method": request["method"], "url": request["url"]}
                for request in requests
            ]
        },
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")
    raw = _bounded_command(
        [
            str(node),
            str(REFERENCE_CLOSURE),
            "--source",
            str(source),
            "--merged",
            str(merged),
            "--source-sha256",
            source_digest,
            "--merged-sha256",
            merged_digest,
            "--source-records",
            str(source_count),
            "--merged-records",
            str(merged_count),
            "--origin-ms",
            str(origin_ms),
        ],
        "authoritative response-route closure",
        60,
        plan,
    )
    try:
        closed = json.loads(raw.decode("utf-8"), object_pairs_hook=_unique_object)
    except (UnicodeError, json.JSONDecodeError) as error:
        raise AcquisitionError(
            f"authoritative response-route closure returned invalid JSON: {error}"
        ) from error
    required = {
        "schema",
        "verified",
        "source_records",
        "merged_records",
        "acquired_routes",
        "source_ambiguous_routes",
        "merged_ambiguous_routes",
        "source_trace_sha256",
        "merged_trace_sha256",
        "origin_ms",
    }
    if (
        not isinstance(closed, dict)
        or set(closed) != required
        or closed["schema"] != 1
        or closed["verified"] is not True
        or closed["source_records"] != source_count
        or closed["merged_records"] != merged_count
        or closed["acquired_routes"] != len(requests)
        or closed["source_ambiguous_routes"]
            != closed["merged_ambiguous_routes"]
        or closed["source_trace_sha256"] != source_digest
        or closed["merged_trace_sha256"] != merged_digest
        or closed["origin_ms"] != origin_ms
    ):
        raise AcquisitionError("authoritative response-route closure is inconsistent")
    return closed


def _run_recorder(
    recorder: Path,
    request: dict[str, Any],
    output: Path,
    maximum_bytes: int,
    timeout_ms: int,
) -> None:
    command = [
        str(recorder),
        "--method",
        request["method"],
        "--url",
        request["url"],
        "--output",
        str(output),
        "--max-bytes",
        str(maximum_bytes),
        "--timeout-ms",
        str(timeout_ms),
    ]
    try:
        completed = subprocess.run(
            command,
            check=False,
            env=_safe_environment(),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout_ms / 1000 + 5,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise AcquisitionError(f"isolated recorder failed to run: {error}") from error
    stdout = completed.stdout[:OUTPUT_CAPTURE_LIMIT]
    stderr = completed.stderr[:OUTPUT_CAPTURE_LIMIT]
    if (
        completed.returncode != 0
        or len(completed.stdout) > OUTPUT_CAPTURE_LIMIT
        or len(completed.stderr) > OUTPUT_CAPTURE_LIMIT
    ):
        detail = stderr.decode("utf-8", "replace").strip()
        if not detail:
            detail = stdout.decode("utf-8", "replace").strip()
        raise AcquisitionError(
            f"isolated recorder exited {completed.returncode}: {detail[:1000]}"
        )


def _publish(
    source: Path,
    output: Path,
    recorder: Path,
    native_inventory: Path,
    node: Path,
    source_inspection: dict[str, Any],
    requests: list[dict[str, Any]],
    contract: dict[str, Any],
) -> dict[str, Any]:
    source = source.resolve(strict=True)
    if output.name in ("", ".", ".."):
        raise AcquisitionError("output trace needs a new directory name")
    output_parent = output.parent.resolve(strict=True)
    output = output_parent / output.name
    if output.exists() or output.is_symlink():
        raise AcquisitionError("output trace already exists")
    if source == output:
        raise AcquisitionError("output trace cannot replace the source trace")
    if source in output.parents:
        raise AcquisitionError("output trace cannot be created inside its source")
    _regular_file(recorder, "trace recorder")
    if not os.access(recorder, os.X_OK):
        raise AcquisitionError("trace recorder is not executable")

    info, source_info_raw, existing_keys = _trace_inventory(source)
    source_count = int(info["record-count"], 10)
    source_origin = int(info["origin-ms"], 10)
    if source_count != contract["source_record_count"]:
        raise AcquisitionError("source trace record count differs from the contract")
    if source_origin != contract["source_origin_ms"]:
        raise AcquisitionError("source trace origin differs from the contract")
    source_sha, source_bytes = trace_digest(source, contract["max_trace_bytes"])
    if source_sha != contract["source_trace_sha256"]:
        raise AcquisitionError(
            f"source trace SHA-256 mismatch: expected {contract['source_trace_sha256']}, got {source_sha}"
        )
    if source_count + len(requests) > MAX_RECORDS:
        raise AcquisitionError("merged trace would exceed the record limit")
    # Per-record size is enforced by _validate_acquired_record and the final
    # trace_digest enforces the merged byte limit; the fail-fast check here
    # only needs the source plus one worst-case response to fit.
    if source_bytes + contract["max_response_bytes"] > contract["max_trace_bytes"]:
        raise AcquisitionError("merged trace could not fit another response within its byte limit")
    for request in requests:
        if (request["method"], request["url"]) in existing_keys:
            raise AcquisitionError("acquisition plan contains an already retained route")

    staging = Path(
        tempfile.mkdtemp(prefix=f".{output.name}.acquire-", dir=output_parent)
    )
    isolated: Path | None = None
    published = False
    try:
        os.chmod(staging, 0o700)
        for source_file, relative, _size in _trace_files(source):
            _copy_regular(source_file, staging / relative)
        staged_source_sha, _staged_bytes = trace_digest(
            staging, contract["max_trace_bytes"]
        )
        if staged_source_sha != source_sha:
            raise AcquisitionError("staged source trace differs from its authority digest")

        for offset, request in enumerate(requests):
            isolated = Path(
                tempfile.mkdtemp(prefix=f".{output.name}.record-", dir=output_parent)
            )
            isolated.rmdir()  # Recorder requires a path which does not exist.
            _run_recorder(
                recorder,
                request,
                isolated,
                contract["max_response_bytes"],
                contract["timeout_ms"],
            )
            _validate_acquired_record(
                isolated, request, contract["max_response_bytes"]
            )
            sequence = source_count + offset
            os.replace(isolated / "0000.meta", staging / f"{sequence:04d}.meta")
            os.replace(isolated / "0000.body", staging / f"{sequence:04d}.body")
            _fsync_file(staging / f"{sequence:04d}.meta")
            _fsync_file(staging / f"{sequence:04d}.body")
            (isolated / "trace.meta").unlink()
            isolated.rmdir()
            isolated = None

        merged_count = source_count + len(requests)
        next_info = staging / "trace.meta.next"
        rewritten = _rewrite_record_count(
            source_info_raw, source_count, merged_count
        )
        descriptor = os.open(
            next_info, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600
        )
        try:
            view = memoryview(rewritten)
            while view:
                written = os.write(descriptor, view)
                if written <= 0:
                    raise AcquisitionError("short write while publishing trace.meta")
                view = view[written:]
            os.fsync(descriptor)
        finally:
            os.close(descriptor)
        os.replace(next_info, staging / "trace.meta")
        _fsync_directory(staging)

        final_info, _final_raw, final_keys = _trace_inventory(staging)
        final_origin = int(final_info["origin-ms"], 10)
        final_count = int(final_info["record-count"], 10)
        if final_origin != source_origin or final_count != merged_count:
            raise AcquisitionError("final trace changed its origin or record count")
        final_sha, final_bytes = trace_digest(
            staging, contract["max_trace_bytes"]
        )
        final_inspection = _inspect_reference_trace(node, staging)
        _require_inspection_authority(
            final_inspection, final_sha, merged_count, source_origin
        )
        closure = _run_route_closure(
            node,
            source,
            staging,
            source_sha,
            final_sha,
            source_count,
            merged_count,
            source_origin,
            requests,
        )
        if (
            closure["source_ambiguous_routes"]
            != source_inspection["ambiguous_routes"]
        ):
            raise AcquisitionError(
                "source route ambiguity changed between inspection and closure"
            )
        _run_native_inventory(native_inventory, staging, merged_count)
        source_sha_after, _source_bytes_after = trace_digest(
            source, contract["max_trace_bytes"]
        )
        if source_sha_after != source_sha:
            raise AcquisitionError("source trace changed during acquisition")

        # Every authoritative inspector above receives the private staging
        # directory by pathname.  Re-establish the complete Python authority
        # after the last child exits so a buggy or mutating helper cannot make
        # publication diverge from the digest returned to the caller.
        publish_info, _publish_raw, publish_keys = _trace_inventory(staging)
        publish_sha, publish_bytes = trace_digest(
            staging, contract["max_trace_bytes"]
        )
        if (
            int(publish_info["origin-ms"], 10) != final_origin
            or int(publish_info["record-count"], 10) != final_count
            or publish_keys != final_keys
            or publish_sha != final_sha
            or publish_bytes != final_bytes
        ):
            raise AcquisitionError(
                "final staged trace changed after external validation"
            )
        _publish_new_directory(staging, output)
        _fsync_directory(output_parent)
        published = True
        return {
            "schema": 1,
            "contract": contract["name"],
            "output_trace": str(output),
            "trace_sha256": final_sha,
            "record_count": merged_count,
            "origin_ms": source_origin,
            "acquired_requests": len(requests),
            "trace_bytes": final_bytes,
            "routes": final_inspection["routes"],
            "ambiguous_routes": final_inspection["ambiguous_routes"],
            "route_selection_version": final_inspection[
                "route_selection_version"
            ],
            "native_inventory_validated": True,
        }
    finally:
        if isolated is not None and isolated.exists():
            shutil.rmtree(isolated)
        if not published and staging.exists():
            shutil.rmtree(staging)


def _arguments(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--contract", type=Path, required=True)
    parser.add_argument("--diagnostic", type=Path, required=True)
    parser.add_argument("--source-trace", type=Path, required=True)
    parser.add_argument("--output-trace", type=Path)
    parser.add_argument("--recorder", type=Path)
    parser.add_argument(
        "--native-inventory",
        type=Path,
        help="host native response-keyed inventory executable",
    )
    parser.add_argument(
        "--validate-only",
        action="store_true",
        help="validate all pinned input authority without running the recorder",
    )
    result = parser.parse_args(argv)
    if result.validate_only:
        if result.output_trace is not None or result.recorder is not None:
            parser.error("--validate-only cannot publish an output or run a recorder")
    elif (
        result.output_trace is None
        or result.recorder is None
        or result.native_inventory is None
    ):
        parser.error(
            "acquisition requires --output-trace, --recorder, and --native-inventory"
        )
    return result


def main(argv: list[str]) -> int:
    try:
        options = _arguments(argv)
        contract = _validate_contract(options.contract)
        requests = _validate_plan(options.diagnostic, contract)
        source = options.source_trace.resolve(strict=True)
        info, _raw, existing = _trace_inventory(source)
        source_sha, source_bytes = trace_digest(source, contract["max_trace_bytes"])
        if source_sha != contract["source_trace_sha256"]:
            raise AcquisitionError("source trace digest differs from the contract")
        if int(info["record-count"], 10) != contract["source_record_count"]:
            raise AcquisitionError("source trace record count differs from the contract")
        if int(info["origin-ms"], 10) != contract["source_origin_ms"]:
            raise AcquisitionError("source trace origin differs from the contract")
        if any((item["method"], item["url"]) in existing for item in requests):
            raise AcquisitionError("plan includes a route already present in source")
        node = _node_executable()
        source_inspection = _inspect_reference_trace(node, source)
        _require_inspection_authority(
            source_inspection,
            source_sha,
            contract["source_record_count"],
            contract["source_origin_ms"],
        )
        native_validated = False
        if options.native_inventory is not None:
            _run_native_inventory(
                options.native_inventory, source, contract["source_record_count"]
            )
            native_validated = True
        if options.validate_only:
            summary = {
                "schema": 1,
                "validated": True,
                "contract": contract["name"],
                "diagnostic_sha256": contract["diagnostic_sha256"],
                "allowlist_sha256": contract["allowlist_sha256"],
                "source_trace_sha256": source_sha,
                "source_record_count": int(info["record-count"], 10),
                "source_origin_ms": int(info["origin-ms"], 10),
                "source_trace_bytes": source_bytes,
                "request_count": len(requests),
                "source_routes": source_inspection["routes"],
                "source_ambiguous_routes": source_inspection[
                    "ambiguous_routes"
                ],
                "route_selection_version": source_inspection[
                    "route_selection_version"
                ],
                "native_inventory_validated": native_validated,
            }
        else:
            summary = _publish(
                source,
                options.output_trace,
                options.recorder.resolve(strict=True),
                options.native_inventory.resolve(strict=True),
                node,
                source_inspection,
                requests,
                contract,
            )
        print(json.dumps(summary, sort_keys=True, separators=(",", ":")))
        return 0
    except (AcquisitionError, OSError) as error:
        print(f"trace-acquisition-error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
