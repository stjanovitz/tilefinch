#!/usr/bin/env python3
"""Compare bounded Tilefinch and Chromium text-run geometry artifacts."""

from __future__ import annotations

import argparse
import bisect
import json
import math
import sys
from collections import defaultdict
from pathlib import Path
from typing import Any


SCHEMA = "tilefinch-text-metrics-v1"
MAX_ARTIFACT_BYTES = 8 * 1024 * 1024
MAX_RUNS = 4096
MAX_TEXT_LENGTH = 1024


class MetricsError(ValueError):
    pass


def load_artifact(path: Path, expected_source: str) -> dict[str, Any]:
    try:
        size = path.stat().st_size
    except OSError as error:
        raise MetricsError(f"cannot stat {path}: {error}") from error
    if size > MAX_ARTIFACT_BYTES:
        raise MetricsError(f"{path}: artifact exceeds {MAX_ARTIFACT_BYTES} bytes")
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise MetricsError(f"cannot read {path}: {error}") from error
    if not isinstance(value, dict) or value.get("schema") != SCHEMA:
        raise MetricsError(f"{path}: unsupported text-metrics schema")
    if value.get("source") != expected_source:
        raise MetricsError(f"{path}: expected source {expected_source}")
    runs = value.get("runs")
    if not isinstance(runs, list) or len(runs) > MAX_RUNS:
        raise MetricsError(f"{path}: runs must be an array of at most {MAX_RUNS}")
    if value.get("run_count") != len(runs):
        raise MetricsError(f"{path}: run_count does not match runs")
    for index, run in enumerate(runs):
        if not isinstance(run, dict):
            raise MetricsError(f"{path}: run {index} is not an object")
        text = run.get("text")
        if not isinstance(text, str) or not text or len(text) > MAX_TEXT_LENGTH:
            raise MetricsError(f"{path}: run {index} has invalid text")
        for field in ("x_26_6", "advance_26_6"):
            number = run.get(field)
            if not isinstance(number, int) or isinstance(number, bool):
                raise MetricsError(f"{path}: run {index} has invalid {field}")
        y_field = "y" if expected_source == "tilefinch" else "y_26_6"
        number = run.get(y_field)
        if not isinstance(number, int) or isinstance(number, bool):
            raise MetricsError(f"{path}: run {index} has invalid {y_field}")
    return value


def percentile(values: list[float], fraction: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    position = max(0, math.ceil(fraction * len(ordered)) - 1)
    return ordered[position]


def distribution(values: list[float]) -> dict[str, float | int | None]:
    if not values:
        return {"count": 0, "mean": None, "p50": None, "p95": None, "max": None}
    return {
        "count": len(values),
        "mean": sum(values) / len(values),
        "p50": percentile(values, 0.50),
        "p95": percentile(values, 0.95),
        "max": max(values),
    }


def monotone_matches(
    candidate_runs: list[dict[str, Any]], reference_runs: list[dict[str, Any]]
) -> list[tuple[int, int]]:
    candidate_positions: dict[str, list[int]] = defaultdict(list)
    reference_positions: dict[str, list[int]] = defaultdict(list)
    for index, run in enumerate(candidate_runs):
        candidate_positions[run["text"]].append(index)
    for index, run in enumerate(reference_runs):
        reference_positions[run["text"]].append(index)

    # Unique tokens are unambiguous anchors. Select their longest monotone
    # subsequence so an inserted/removed subtree cannot pair a repeated label
    # with a distant occurrence and manufacture a huge geometry outlier.
    unique_pairs = sorted(
        (candidate_at[0], reference_positions[text][0])
        for text, candidate_at in candidate_positions.items()
        if len(candidate_at) == 1 and len(reference_positions.get(text, ())) == 1
    )
    tails: list[int] = []
    tail_pair_indices: list[int] = []
    previous = [-1] * len(unique_pairs)
    for pair_index, (_, reference_index) in enumerate(unique_pairs):
        at = bisect.bisect_left(tails, reference_index)
        if at == len(tails):
            tails.append(reference_index)
            tail_pair_indices.append(pair_index)
        else:
            tails[at] = reference_index
            tail_pair_indices[at] = pair_index
        if at > 0:
            previous[pair_index] = tail_pair_indices[at - 1]
    anchors: list[tuple[int, int]] = []
    if tail_pair_indices:
        at = tail_pair_indices[-1]
        while at >= 0:
            anchors.append(unique_pairs[at])
            at = previous[at]
        anchors.reverse()

    bounded_anchors = [(-1, -1), *anchors,
                       (len(candidate_runs), len(reference_runs))]
    matches: list[tuple[int, int]] = []
    for anchor_at in range(len(bounded_anchors) - 1):
        candidate_start, reference_start = bounded_anchors[anchor_at]
        candidate_end, reference_end = bounded_anchors[anchor_at + 1]
        last_reference = reference_start
        for candidate_index in range(candidate_start + 1, candidate_end):
            choices = reference_positions.get(
                candidate_runs[candidate_index]["text"], []
            )
            at = bisect.bisect_right(choices, last_reference)
            if at >= len(choices) or choices[at] >= reference_end:
                continue
            reference_index = choices[at]
            matches.append((candidate_index, reference_index))
            last_reference = reference_index
        if candidate_end < len(candidate_runs):
            matches.append((candidate_end, reference_end))
    return matches


def compare(candidate: dict[str, Any], reference: dict[str, Any]) -> dict[str, Any]:
    candidate_runs = candidate["runs"]
    reference_runs = reference["runs"]
    matches = monotone_matches(candidate_runs, reference_runs)
    advance_errors: list[float] = []
    x_errors: list[float] = []
    y_errors: list[float] = []
    font_size_errors: list[float] = []
    line_height_errors: list[float] = []
    details: list[dict[str, Any]] = []
    for candidate_index, reference_index in matches:
        left = candidate_runs[candidate_index]
        right = reference_runs[reference_index]
        advance_error = None
        if left["advance_26_6"] >= 0 and right["advance_26_6"] >= 0:
            advance_error = (left["advance_26_6"] - right["advance_26_6"]) / 64.0
            advance_errors.append(abs(advance_error))
        x_error = (left["x_26_6"] - right["x_26_6"]) / 64.0
        y_error = left["y"] - right["y_26_6"] / 64.0
        x_errors.append(abs(x_error))
        y_errors.append(abs(y_error))
        reference_font_size = right.get("font_size_26_6")
        if isinstance(reference_font_size, int):
            candidate_font_size = left.get("font_size_26_6")
            if not isinstance(candidate_font_size, int):
                candidate_font_size = left.get("font_size", 0) * 64
            font_size_errors.append(abs(candidate_font_size
                                        - reference_font_size) / 64.0)
        reference_rect_height = right.get("rect_height_26_6")
        if isinstance(reference_rect_height, int):
            line_height_errors.append(abs(left.get("line_height", 0)
                                          - reference_rect_height / 64.0))
        if len(details) < 256:
            details.append({
                "candidate_index": candidate_index,
                "reference_index": reference_index,
                "text": left["text"],
                "advance_error_px": advance_error,
                "x_error_px": x_error,
                "y_error_px": y_error,
                "candidate_font_family": left.get("font_family"),
                "candidate_face_backend": left.get("face_backend"),
                "reference_font_family": right.get("font_family"),
            })

    line_pairs = 0
    line_agreements = 0
    for (left_candidate, left_reference), (right_candidate, right_reference) in zip(
        matches, matches[1:]
    ):
        if right_candidate != left_candidate + 1 or right_reference != left_reference + 1:
            continue
        candidate_same = candidate_runs[left_candidate]["y"] == candidate_runs[right_candidate]["y"]
        reference_same = abs(
            reference_runs[left_reference]["y_26_6"]
            - reference_runs[right_reference]["y_26_6"]
        ) <= 32
        line_pairs += 1
        line_agreements += candidate_same == reference_same

    candidate_count = len(candidate_runs)
    reference_count = len(reference_runs)
    matched = len(matches)
    coverage_denominator = max(candidate_count, reference_count, 1)
    candidate_height = candidate.get("document_height")
    reference_height = reference.get("document_height")
    height_error = None
    if isinstance(candidate_height, int) and isinstance(reference_height, int):
        height_error = candidate_height - reference_height
    return {
        "schema": "tilefinch-text-metrics-comparison-v1",
        "candidate_runs": candidate_count,
        "reference_runs": reference_count,
        "matched_runs": matched,
        "match_coverage": matched / coverage_denominator,
        "document_height_error_px": height_error,
        "advance_absolute_error_px": distribution(advance_errors),
        "x_absolute_error_px": distribution(x_errors),
        "y_absolute_error_px": distribution(y_errors),
        "font_size_absolute_error_px": distribution(font_size_errors),
        "line_height_absolute_error_px": distribution(line_height_errors),
        "line_relation_pairs": line_pairs,
        "line_break_agreement": line_agreements / line_pairs if line_pairs else None,
        "candidate_truncated": bool(candidate.get("truncated")),
        "reference_truncated": bool(reference.get("truncated")),
        "matches": details,
    }


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("candidate", type=Path)
    parser.add_argument("reference", type=Path)
    parser.add_argument("--output-format", choices=("json", "tsv"), default="json")
    parser.add_argument("--min-match-coverage", type=float)
    parser.add_argument("--max-mean-advance-error-px", type=float)
    parser.add_argument("--min-line-break-agreement", type=float)
    return parser.parse_args()


def validate_fraction(value: float | None, label: str) -> None:
    if value is not None and (not math.isfinite(value) or value < 0 or value > 1):
        raise MetricsError(f"{label} must be in 0..1")


def main() -> int:
    args = parse_arguments()
    try:
        validate_fraction(args.min_match_coverage, "minimum match coverage")
        validate_fraction(args.min_line_break_agreement, "minimum line-break agreement")
        if args.max_mean_advance_error_px is not None and (
            not math.isfinite(args.max_mean_advance_error_px)
            or args.max_mean_advance_error_px < 0
        ):
            raise MetricsError("maximum mean advance error must be nonnegative")
        report = compare(
            load_artifact(args.candidate, "tilefinch"),
            load_artifact(args.reference, "chromium"),
        )
    except MetricsError as error:
        print(f"text-metrics-error={error}", file=sys.stderr)
        return 2
    failures: list[str] = []
    if args.min_match_coverage is not None and report["match_coverage"] < args.min_match_coverage:
        failures.append("match coverage below minimum")
    mean_advance = report["advance_absolute_error_px"]["mean"]
    if (args.max_mean_advance_error_px is not None
            and (mean_advance is None or mean_advance > args.max_mean_advance_error_px)):
        failures.append("mean advance error above maximum")
    line_agreement = report["line_break_agreement"]
    if (args.min_line_break_agreement is not None
            and (line_agreement is None or line_agreement < args.min_line_break_agreement)):
        failures.append("line-break agreement below minimum")
    report["qualification"] = {
        "active": any(value is not None for value in (
            args.min_match_coverage, args.max_mean_advance_error_px,
            args.min_line_break_agreement,
        )),
        "passed": not failures,
        "failures": failures,
    }
    if args.output_format == "json":
        print(json.dumps(report, indent=2, sort_keys=True))
    else:
        print("candidate_runs\treference_runs\tmatched_runs\tmatch_coverage\t"
              "mean_advance_error_px\tp95_advance_error_px\tline_break_agreement")
        print(
            f"{report['candidate_runs']}\t{report['reference_runs']}\t"
            f"{report['matched_runs']}\t{report['match_coverage']:.6f}\t"
            f"{mean_advance if mean_advance is not None else ''}\t"
            f"{report['advance_absolute_error_px']['p95'] or ''}\t"
            f"{line_agreement if line_agreement is not None else ''}"
        )
    for failure in failures:
        print(f"text metrics qualification failed: {failure}", file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
