#!/usr/bin/env python3
"""Compare a Tilefinch PPM frame with a canonical Chrome PNG reference.

The comparator intentionally uses only the Python standard library.  Its
default mode is diagnostic and exits successfully after producing metrics,
even when the frames are very different.  Supplying a qualification threshold
or ``--qualify`` turns metric misses into exit status 1.  Invalid inputs remain
exit status 2.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
import zlib
from pathlib import Path
from typing import Any

from reference_frame import read_png, read_ppm


QUALIFY_MAX_MAE_RGB565 = 12.0
QUALIFY_MIN_LUMA_MS_SSIM = 0.88
QUALIFY_MIN_EDGE_F1 = 0.82

GEOMETRY_COMPONENTS = ("x", "y", "width", "height")


def compare(candidate: bytes, reference: bytes) -> tuple[float, float, float, float]:
    """Return the legacy raw-RGB MAE, RMSE, mismatch and material delta metrics."""

    metrics = rgb_error_metrics(candidate, reference)
    return (
        metrics["mae"],
        metrics["rmse"],
        metrics["mismatch_pct"],
        metrics["material_pct"],
    )


def rgb_error_metrics(candidate: bytes, reference: bytes) -> dict[str, float]:
    if len(candidate) != len(reference) or len(candidate) % 3 != 0:
        raise ValueError("RGB frames have different or invalid lengths")
    absolute = 0
    squared = 0
    changed_pixels = 0
    materially_changed = 0
    for pixel in range(0, len(candidate), 3):
        changed = False
        material = False
        for channel in range(3):
            delta = abs(candidate[pixel + channel] - reference[pixel + channel])
            absolute += delta
            squared += delta * delta
            changed |= delta != 0
            material |= delta > 8
        changed_pixels += changed
        materially_changed += material
    samples = len(candidate)
    pixels = samples // 3
    return {
        "mae": absolute / samples,
        "rmse": math.sqrt(squared / samples),
        "mismatch_pct": changed_pixels * 100.0 / pixels,
        "material_pct": materially_changed * 100.0 / pixels,
    }


def quantize_rgb565(pixels: bytes) -> bytes:
    """Round-trip RGB888 through RGB565, expanding bits back to all of 0..255."""

    quantized = bytearray(len(pixels))
    for at in range(0, len(pixels), 3):
        red = pixels[at] >> 3
        green = pixels[at + 1] >> 2
        blue = pixels[at + 2] >> 3
        quantized[at] = (red << 3) | (red >> 2)
        quantized[at + 1] = (green << 2) | (green >> 4)
        quantized[at + 2] = (blue << 3) | (blue >> 2)
    return bytes(quantized)


def rgb_to_luma(pixels: bytes) -> list[int]:
    # Integer Rec. 709 coefficients, rounded and normalized to sum to 256.
    return [
        (54 * pixels[at] + 183 * pixels[at + 1] + 19 * pixels[at + 2] + 128)
        >> 8
        for at in range(0, len(pixels), 3)
    ]


def local_luma_ssim(
    candidate: list[int], reference: list[int], width: int, height: int
) -> float:
    """Compute mean local luminance SSIM over dependency-free 8x8 windows."""

    if len(candidate) != width * height or len(reference) != width * height:
        raise ValueError("luminance frame length does not match geometry")
    c1 = (0.01 * 255.0) ** 2
    c2 = (0.03 * 255.0) ** 2
    weighted_score = 0.0
    total_weight = 0
    block = 8
    for top in range(0, height, block):
        bottom = min(top + block, height)
        for left in range(0, width, block):
            right = min(left + block, width)
            count = (right - left) * (bottom - top)
            candidate_sum = 0
            reference_sum = 0
            candidate_square_sum = 0
            reference_square_sum = 0
            product_sum = 0
            for y in range(top, bottom):
                row = y * width
                for x in range(left, right):
                    first = candidate[row + x]
                    second = reference[row + x]
                    candidate_sum += first
                    reference_sum += second
                    candidate_square_sum += first * first
                    reference_square_sum += second * second
                    product_sum += first * second
            candidate_mean = candidate_sum / count
            reference_mean = reference_sum / count
            candidate_variance = max(
                0.0, candidate_square_sum / count - candidate_mean**2
            )
            reference_variance = max(
                0.0, reference_square_sum / count - reference_mean**2
            )
            covariance = product_sum / count - candidate_mean * reference_mean
            numerator = (2.0 * candidate_mean * reference_mean + c1) * (
                2.0 * covariance + c2
            )
            denominator = (
                candidate_mean**2 + reference_mean**2 + c1
            ) * (candidate_variance + reference_variance + c2)
            score = numerator / denominator if denominator else 1.0
            weighted_score += max(-1.0, min(1.0, score)) * count
            total_weight += count
    return weighted_score / total_weight


def downsample_luma(
    pixels: list[int], width: int, height: int
) -> tuple[list[int], int, int]:
    next_width = (width + 1) // 2
    next_height = (height + 1) // 2
    output = [0] * (next_width * next_height)
    for output_y in range(next_height):
        source_y = output_y * 2
        for output_x in range(next_width):
            source_x = output_x * 2
            total = 0
            count = 0
            for y in range(source_y, min(source_y + 2, height)):
                row = y * width
                for x in range(source_x, min(source_x + 2, width)):
                    total += pixels[row + x]
                    count += 1
            output[output_y * next_width + output_x] = (total + count // 2) // count
    return output, next_width, next_height


def multi_scale_luma_ssim(
    candidate: list[int], reference: list[int], width: int, height: int
) -> float:
    """Combine local luminance SSIM at native, half and quarter resolution."""

    weights = (0.5, 0.3, 0.2)
    scores: list[float] = []
    first = candidate
    second = reference
    current_width = width
    current_height = height
    for scale in range(len(weights)):
        scores.append(local_luma_ssim(first, second, current_width, current_height))
        if scale == len(weights) - 1 or (current_width == 1 and current_height == 1):
            break
        first, next_width, next_height = downsample_luma(
            first, current_width, current_height
        )
        second, second_width, second_height = downsample_luma(
            second, current_width, current_height
        )
        if (next_width, next_height) != (second_width, second_height):
            raise ValueError("downsampled luminance geometry differs")
        current_width, current_height = next_width, next_height

    # A weighted geometric mean makes a badly mismatched scale visible instead
    # of allowing good scores at the other scales to hide it. Negative SSIM has
    # no useful geometric interpretation and is treated as complete mismatch.
    used_weights = weights[: len(scores)]
    weight_total = sum(used_weights)
    if any(score <= 0.0 for score in scores):
        return 0.0
    logarithm = sum(
        weight * math.log(min(1.0, score))
        for weight, score in zip(used_weights, scores)
    )
    return math.exp(logarithm / weight_total)


def sobel_edges(
    luma: list[int], width: int, height: int, threshold: int
) -> bytearray:
    edges = bytearray(width * height)
    if width < 3 or height < 3:
        return edges
    for y in range(1, height - 1):
        above = (y - 1) * width
        current = y * width
        below = (y + 1) * width
        for x in range(1, width - 1):
            horizontal = (
                -luma[above + x - 1]
                + luma[above + x + 1]
                - 2 * luma[current + x - 1]
                + 2 * luma[current + x + 1]
                - luma[below + x - 1]
                + luma[below + x + 1]
            )
            vertical = (
                -luma[above + x - 1]
                - 2 * luma[above + x]
                - luma[above + x + 1]
                + luma[below + x - 1]
                + 2 * luma[below + x]
                + luma[below + x + 1]
            )
            if abs(horizontal) + abs(vertical) >= threshold:
                edges[current + x] = 1
    return edges


def edge_has_neighbor(
    edges: bytearray, width: int, height: int, x: int, y: int, tolerance: int
) -> bool:
    for neighbor_y in range(max(0, y - tolerance), min(height, y + tolerance + 1)):
        row = neighbor_y * width
        for neighbor_x in range(max(0, x - tolerance), min(width, x + tolerance + 1)):
            if edges[row + neighbor_x]:
                return True
    return False


def edge_metrics(
    candidate: list[int],
    reference: list[int],
    width: int,
    height: int,
    threshold: int,
    tolerance: int,
) -> dict[str, float | int]:
    candidate_edges = sobel_edges(candidate, width, height, threshold)
    reference_edges = sobel_edges(reference, width, height, threshold)
    candidate_count = sum(candidate_edges)
    reference_count = sum(reference_edges)
    candidate_matches = 0
    reference_matches = 0
    for y in range(height):
        row = y * width
        for x in range(width):
            at = row + x
            if candidate_edges[at] and edge_has_neighbor(
                reference_edges, width, height, x, y, tolerance
            ):
                candidate_matches += 1
            if reference_edges[at] and edge_has_neighbor(
                candidate_edges, width, height, x, y, tolerance
            ):
                reference_matches += 1
    if candidate_count == 0 and reference_count == 0:
        precision = recall = f1 = 1.0
    else:
        precision = candidate_matches / candidate_count if candidate_count else 0.0
        recall = reference_matches / reference_count if reference_count else 0.0
        f1 = (
            2.0 * precision * recall / (precision + recall)
            if precision + recall
            else 0.0
        )
    return {
        "precision": precision,
        "recall": recall,
        "f1": f1,
        "candidate_count": candidate_count,
        "reference_count": reference_count,
    }


def foreground_metrics(
    pixels: bytes,
    luma: list[int],
    threshold: int,
    blank_foreground_pct: float,
    blank_luma_stddev: float,
) -> dict[str, Any]:
    # Find a coarse dominant color, then average that bucket's original samples
    # so anti-aliased near-whites do not arbitrarily become the background.
    histogram: dict[int, int] = {}
    for at in range(0, len(pixels), 3):
        bucket = (
            (pixels[at] >> 4) << 8
            | (pixels[at + 1] >> 4) << 4
            | (pixels[at + 2] >> 4)
        )
        histogram[bucket] = histogram.get(bucket, 0) + 1
    dominant = min(histogram, key=lambda item: (-histogram[item], item))
    sums = [0, 0, 0]
    background_count = 0
    for at in range(0, len(pixels), 3):
        bucket = (
            (pixels[at] >> 4) << 8
            | (pixels[at + 1] >> 4) << 4
            | (pixels[at + 2] >> 4)
        )
        if bucket == dominant:
            sums[0] += pixels[at]
            sums[1] += pixels[at + 1]
            sums[2] += pixels[at + 2]
            background_count += 1
    background = tuple((value + background_count // 2) // background_count for value in sums)
    foreground = 0
    for at in range(0, len(pixels), 3):
        if max(
            abs(pixels[at] - background[0]),
            abs(pixels[at + 1] - background[1]),
            abs(pixels[at + 2] - background[2]),
        ) > threshold:
            foreground += 1
    pixel_count = len(pixels) // 3
    foreground_pct = foreground * 100.0 / pixel_count
    mean_luma = sum(luma) / len(luma)
    variance = sum((value - mean_luma) ** 2 for value in luma) / len(luma)
    luma_stddev = math.sqrt(variance)
    return {
        "background_rgb": list(background),
        "foreground_pct": foreground_pct,
        "luma_stddev": luma_stddev,
        "blank": foreground_pct <= blank_foreground_pct
        and luma_stddev <= blank_luma_stddev,
    }


def finite_number(value: object, description: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(f"{description} must be a number")
    numeric = float(value)
    if not math.isfinite(numeric):
        raise ValueError(f"{description} must be finite")
    return numeric


def read_geometry_anchors(path: Path) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as error:
        raise ValueError(f"{path}: invalid geometry JSON: {error.msg}") from error
    if not isinstance(document, dict) or not isinstance(document.get("anchors"), list):
        raise ValueError(f"{path}: geometry JSON must contain an anchors array")
    anchors = document["anchors"]
    if not anchors:
        raise ValueError(f"{path}: geometry anchors array is empty")
    seen: set[str] = set()
    component_deltas: list[float] = []
    anchor_details: list[dict[str, Any]] = []
    for index, anchor in enumerate(anchors):
        prefix = f"{path}: anchors[{index}]"
        if not isinstance(anchor, dict):
            raise ValueError(f"{prefix} must be an object")
        name = anchor.get("name")
        if not isinstance(name, str) or not name:
            raise ValueError(f"{prefix}.name must be a non-empty string")
        if name in seen:
            raise ValueError(f"{prefix}.name duplicates {name!r}")
        seen.add(name)
        candidate = anchor.get("candidate")
        reference = anchor.get("reference")
        if not isinstance(candidate, dict) or not isinstance(reference, dict):
            raise ValueError(f"{prefix} must contain candidate and reference objects")
        present = [
            component
            for component in GEOMETRY_COMPONENTS
            if component in candidate or component in reference
        ]
        if not present:
            raise ValueError(
                f"{prefix} must compare at least one of {', '.join(GEOMETRY_COMPONENTS)}"
            )
        deltas: dict[str, float] = {}
        for component in present:
            if component not in candidate or component not in reference:
                raise ValueError(
                    f"{prefix}.{component} must exist in both candidate and reference"
                )
            first = finite_number(candidate[component], f"{prefix}.candidate.{component}")
            second = finite_number(reference[component], f"{prefix}.reference.{component}")
            delta = abs(first - second)
            deltas[component] = delta
            component_deltas.append(delta)
        anchor_details.append(
            {
                "name": name,
                "components": len(deltas),
                "mean_error_px": sum(deltas.values()) / len(deltas),
                "max_error_px": max(deltas.values()),
                "deltas_px": deltas,
            }
        )
    squared = sum(delta * delta for delta in component_deltas)
    metrics: dict[str, Any] = {
        "anchor_count": len(anchor_details),
        "component_count": len(component_deltas),
        "mean_error_px": sum(component_deltas) / len(component_deltas),
        "rmse_px": math.sqrt(squared / len(component_deltas)),
        "max_error_px": max(component_deltas),
    }
    return metrics, anchor_details


def percentage(value: float | None, name: str) -> None:
    if value is not None and not 0.0 <= value <= 100.0:
        raise ValueError(f"{name} must be between 0 and 100")


def nonnegative(value: float | None, name: str) -> None:
    if value is not None and value < 0.0:
        raise ValueError(f"{name} must be nonnegative")


def similarity(value: float | None, name: str) -> None:
    if value is not None and not 0.0 <= value <= 1.0:
        raise ValueError(f"{name} must be between 0 and 1")


def validate_options(args: argparse.Namespace) -> None:
    if args.width <= 0 or args.height <= 0:
        raise ValueError("expected width and height must be positive")
    if not 1 <= args.edge_threshold <= 2040:
        raise ValueError("edge threshold must be between 1 and 2040")
    if not 0 <= args.edge_tolerance <= 4:
        raise ValueError("edge tolerance must be between 0 and 4")
    if not 0 <= args.foreground_threshold <= 255:
        raise ValueError("foreground threshold must be between 0 and 255")
    percentage(args.blank_foreground_pct, "blank foreground percentage")
    nonnegative(args.blank_luma_stddev, "blank luma standard deviation")
    nonnegative(args.max_mae_rgb565, "maximum RGB565 MAE")
    nonnegative(args.max_rmse_rgb565, "maximum RGB565 RMSE")
    percentage(
        args.max_material_rgb565_pct, "maximum RGB565 material delta percentage"
    )
    nonnegative(args.max_mae_rgb, "maximum raw RGB MAE")
    similarity(args.min_luma_ssim, "minimum luminance SSIM")
    similarity(args.min_luma_ms_ssim, "minimum multi-scale luminance SSIM")
    similarity(args.min_edge_f1, "minimum edge F1")
    percentage(args.min_candidate_foreground_pct, "minimum candidate foreground")
    percentage(args.max_foreground_delta_pct, "maximum foreground delta")
    nonnegative(args.max_geometry_mean_error_px, "maximum geometry mean error")
    nonnegative(args.max_geometry_error_px, "maximum geometry error")
    if (
        args.geometry_anchors is None
        and (
            args.max_geometry_mean_error_px is not None
            or args.max_geometry_error_px is not None
        )
    ):
        raise ValueError("geometry thresholds require --geometry-anchors")


def qualification_failures(
    args: argparse.Namespace, report: dict[str, Any]
) -> tuple[bool, list[str], dict[str, Any]]:
    thresholds = {
        "max_mae_rgb565": args.max_mae_rgb565,
        "max_rmse_rgb565": args.max_rmse_rgb565,
        "max_material_rgb565_pct": args.max_material_rgb565_pct,
        "max_mae_rgb": args.max_mae_rgb,
        "min_luma_ssim": args.min_luma_ssim,
        "min_luma_ms_ssim": args.min_luma_ms_ssim,
        "min_edge_f1": args.min_edge_f1,
        "min_candidate_foreground_pct": args.min_candidate_foreground_pct,
        "max_foreground_delta_pct": args.max_foreground_delta_pct,
        "max_geometry_mean_error_px": args.max_geometry_mean_error_px,
        "max_geometry_error_px": args.max_geometry_error_px,
    }
    require_nonblank = args.require_nonblank
    if args.qualify:
        if thresholds["max_mae_rgb565"] is None:
            thresholds["max_mae_rgb565"] = QUALIFY_MAX_MAE_RGB565
        if thresholds["min_luma_ms_ssim"] is None:
            thresholds["min_luma_ms_ssim"] = QUALIFY_MIN_LUMA_MS_SSIM
        if thresholds["min_edge_f1"] is None:
            thresholds["min_edge_f1"] = QUALIFY_MIN_EDGE_F1
        require_nonblank = True
    active = args.qualify or require_nonblank or args.require_reference_nonblank or any(
        value is not None for value in thresholds.values()
    )
    applied = {
        "profile": "provisional-psp-aware" if args.qualify else None,
        "thresholds": {
            name: value for name, value in thresholds.items() if value is not None
        },
        "require_nonblank": require_nonblank,
        "require_reference_nonblank": args.require_reference_nonblank,
    }
    if not active:
        return False, [], applied

    metrics = report["metrics"]
    failures: list[str] = []

    def maximum(metric: str, threshold: str, label: str) -> None:
        limit = thresholds[threshold]
        if limit is not None and metrics[metric] > limit:
            failures.append(f"{label} {metrics[metric]:.4f} exceeds {limit:.4f}")

    def minimum(metric: str, threshold: str, label: str) -> None:
        limit = thresholds[threshold]
        if limit is not None and metrics[metric] < limit:
            failures.append(f"{label} {metrics[metric]:.4f} is below {limit:.4f}")

    maximum("mae_rgb565", "max_mae_rgb565", "RGB565 MAE")
    maximum("rmse_rgb565", "max_rmse_rgb565", "RGB565 RMSE")
    maximum(
        "pixel_delta_gt_8_rgb565_pct",
        "max_material_rgb565_pct",
        "RGB565 material delta percentage",
    )
    maximum("mae_rgb", "max_mae_rgb", "raw RGB MAE")
    minimum("luma_ssim", "min_luma_ssim", "luminance SSIM")
    minimum("luma_ms_ssim", "min_luma_ms_ssim", "multi-scale luminance SSIM")
    minimum("edge_f1", "min_edge_f1", "edge F1")
    minimum(
        "candidate_foreground_pct",
        "min_candidate_foreground_pct",
        "candidate foreground percentage",
    )
    maximum(
        "foreground_delta_pct",
        "max_foreground_delta_pct",
        "foreground coverage delta",
    )
    if require_nonblank and metrics["candidate_blank"]:
        failures.append("candidate frame is blank")
    if args.require_reference_nonblank and metrics["reference_blank"]:
        failures.append("reference frame is blank")
    geometry = report["geometry"]
    if geometry is not None:
        mean_limit = thresholds["max_geometry_mean_error_px"]
        max_limit = thresholds["max_geometry_error_px"]
        if mean_limit is not None and geometry["metrics"]["mean_error_px"] > mean_limit:
            failures.append(
                "geometry mean error "
                f"{geometry['metrics']['mean_error_px']:.4f}px exceeds {mean_limit:.4f}px"
            )
        if max_limit is not None and geometry["metrics"]["max_error_px"] > max_limit:
            failures.append(
                "geometry maximum error "
                f"{geometry['metrics']['max_error_px']:.4f}px exceeds {max_limit:.4f}px"
            )
    return True, failures, applied


TSV_COLUMNS = (
    "width",
    "height",
    # Preserve the original columns and their order for existing consumers.
    "mae_rgb",
    "rmse_rgb",
    "pixel_mismatch_pct",
    "pixel_delta_gt_8_pct",
    "mae_rgb565",
    "rmse_rgb565",
    "pixel_mismatch_rgb565_pct",
    "pixel_delta_gt_8_rgb565_pct",
    "luma_ssim",
    "luma_ms_ssim",
    "edge_precision",
    "edge_recall",
    "edge_f1",
    "candidate_edge_count",
    "reference_edge_count",
    "candidate_foreground_pct",
    "reference_foreground_pct",
    "foreground_delta_pct",
    "candidate_luma_stddev",
    "reference_luma_stddev",
    "candidate_blank",
    "reference_blank",
    "geometry_anchor_count",
    "geometry_component_count",
    "geometry_mean_error_px",
    "geometry_rmse_px",
    "geometry_max_error_px",
    "qualification",
)


def tsv_value(value: Any) -> str:
    if value is None:
        return "NA"
    if isinstance(value, bool):
        return "1" if value else "0"
    if isinstance(value, float):
        return f"{value:.4f}"
    return str(value)


def emit_tsv(report: dict[str, Any]) -> None:
    metrics = report["metrics"]
    geometry = report["geometry"]
    geometry_metrics = geometry["metrics"] if geometry is not None else {}
    row: dict[str, Any] = {
        "width": report["width"],
        "height": report["height"],
        **metrics,
        "geometry_anchor_count": geometry_metrics.get("anchor_count", 0),
        "geometry_component_count": geometry_metrics.get("component_count", 0),
        "geometry_mean_error_px": geometry_metrics.get("mean_error_px"),
        "geometry_rmse_px": geometry_metrics.get("rmse_px"),
        "geometry_max_error_px": geometry_metrics.get("max_error_px"),
        "qualification": report["qualification"]["status"],
    }
    print("\t".join(TSV_COLUMNS))
    print("\t".join(tsv_value(row[column]) for column in TSV_COLUMNS))


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    parser.add_argument("candidate", type=Path, help="Tilefinch P6 PPM frame")
    parser.add_argument("reference", type=Path, help="canonical Chrome PNG frame")
    parser.add_argument("--width", type=int, default=480)
    parser.add_argument("--height", type=int, default=272)
    parser.add_argument("--output-format", choices=("tsv", "json"), default="tsv")
    parser.add_argument(
        "--geometry-anchors",
        type=Path,
        help="JSON file containing matched candidate/reference geometry anchors",
    )
    parser.add_argument("--edge-threshold", type=int, default=160)
    parser.add_argument("--edge-tolerance", type=int, default=1)
    parser.add_argument("--foreground-threshold", type=int, default=16)
    parser.add_argument("--blank-foreground-pct", type=float, default=0.25)
    parser.add_argument("--blank-luma-stddev", type=float, default=2.0)

    qualification = parser.add_argument_group("qualification thresholds")
    qualification.add_argument(
        "--qualify",
        action="store_true",
        help=(
            "apply the provisional PSP-aware MAE, MS-SSIM, edge-F1, and "
            "nonblank qualification profile"
        ),
    )
    qualification.add_argument("--max-mae-rgb565", type=float)
    qualification.add_argument("--max-rmse-rgb565", type=float)
    qualification.add_argument("--max-material-rgb565-pct", type=float)
    qualification.add_argument("--max-mae-rgb", type=float)
    qualification.add_argument("--min-luma-ssim", type=float)
    qualification.add_argument("--min-luma-ms-ssim", type=float)
    qualification.add_argument("--min-edge-f1", type=float)
    qualification.add_argument("--min-candidate-foreground-pct", type=float)
    qualification.add_argument("--max-foreground-delta-pct", type=float)
    qualification.add_argument("--require-nonblank", action="store_true")
    qualification.add_argument("--require-reference-nonblank", action="store_true")
    qualification.add_argument("--max-geometry-mean-error-px", type=float)
    qualification.add_argument("--max-geometry-error-px", type=float)
    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    try:
        validate_options(args)
        candidate_width, candidate_height, candidate = read_ppm(args.candidate)
        reference_width, reference_height, reference = read_png(args.reference)
        expected = (args.width, args.height)
        if (candidate_width, candidate_height) != expected:
            raise ValueError(f"candidate is not {args.width}x{args.height}")
        if (reference_width, reference_height) != expected:
            raise ValueError(f"reference is not {args.width}x{args.height}")

        raw = rgb_error_metrics(candidate, reference)
        candidate_rgb565 = quantize_rgb565(candidate)
        reference_rgb565 = quantize_rgb565(reference)
        primary = rgb_error_metrics(candidate_rgb565, reference_rgb565)
        candidate_luma = rgb_to_luma(candidate_rgb565)
        reference_luma = rgb_to_luma(reference_rgb565)
        edges = edge_metrics(
            candidate_luma,
            reference_luma,
            args.width,
            args.height,
            args.edge_threshold,
            args.edge_tolerance,
        )
        candidate_foreground = foreground_metrics(
            candidate_rgb565,
            candidate_luma,
            args.foreground_threshold,
            args.blank_foreground_pct,
            args.blank_luma_stddev,
        )
        reference_foreground = foreground_metrics(
            reference_rgb565,
            reference_luma,
            args.foreground_threshold,
            args.blank_foreground_pct,
            args.blank_luma_stddev,
        )
        metrics: dict[str, Any] = {
            "mae_rgb": raw["mae"],
            "rmse_rgb": raw["rmse"],
            "pixel_mismatch_pct": raw["mismatch_pct"],
            "pixel_delta_gt_8_pct": raw["material_pct"],
            "mae_rgb565": primary["mae"],
            "rmse_rgb565": primary["rmse"],
            "pixel_mismatch_rgb565_pct": primary["mismatch_pct"],
            "pixel_delta_gt_8_rgb565_pct": primary["material_pct"],
            "luma_ssim": local_luma_ssim(
                candidate_luma, reference_luma, args.width, args.height
            ),
            "luma_ms_ssim": multi_scale_luma_ssim(
                candidate_luma, reference_luma, args.width, args.height
            ),
            "edge_precision": edges["precision"],
            "edge_recall": edges["recall"],
            "edge_f1": edges["f1"],
            "candidate_edge_count": edges["candidate_count"],
            "reference_edge_count": edges["reference_count"],
            "candidate_foreground_pct": candidate_foreground["foreground_pct"],
            "reference_foreground_pct": reference_foreground["foreground_pct"],
            "foreground_delta_pct": abs(
                candidate_foreground["foreground_pct"]
                - reference_foreground["foreground_pct"]
            ),
            "candidate_luma_stddev": candidate_foreground["luma_stddev"],
            "reference_luma_stddev": reference_foreground["luma_stddev"],
            "candidate_blank": candidate_foreground["blank"],
            "reference_blank": reference_foreground["blank"],
        }
        geometry = None
        if args.geometry_anchors is not None:
            geometry_metrics, anchor_details = read_geometry_anchors(
                args.geometry_anchors
            )
            geometry = {
                "path": str(args.geometry_anchors),
                "metrics": geometry_metrics,
                "anchors": anchor_details,
            }
        report: dict[str, Any] = {
            "schema_version": 1,
            "candidate": str(args.candidate),
            "reference": str(args.reference),
            "width": args.width,
            "height": args.height,
            "settings": {
                "rgb_normalization": "rgb565-roundtrip",
                "edge_threshold": args.edge_threshold,
                "edge_tolerance_px": args.edge_tolerance,
                "foreground_threshold": args.foreground_threshold,
                "blank_foreground_pct": args.blank_foreground_pct,
                "blank_luma_stddev": args.blank_luma_stddev,
            },
            "background": {
                "candidate_rgb": candidate_foreground["background_rgb"],
                "reference_rgb": reference_foreground["background_rgb"],
            },
            "metrics": metrics,
            "geometry": geometry,
            "qualification": {},
        }
        active, failures, applied = qualification_failures(args, report)
        report["qualification"] = {
            "status": "diagnostic" if not active else "fail" if failures else "pass",
            "passed": None if not active else not failures,
            "failures": failures,
            **applied,
        }
    except (OSError, ValueError, zlib.error) as error:
        print(f"visual comparison failed: {error}", file=sys.stderr)
        return 2

    if args.output_format == "json":
        print(json.dumps(report, indent=2, sort_keys=True))
    else:
        emit_tsv(report)
    if failures:
        for failure in failures:
            print(f"visual qualification failed: {failure}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
