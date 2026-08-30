#!/usr/bin/env python3
"""Compose local-only, pixel-exact 480x272 browser comparisons."""

import argparse
import json
from pathlib import Path
from typing import Optional

from PIL import Image, ImageDraw, ImageFont


WIDTH = 480
HEIGHT = 272
POSITIONS = ("top", "middle", "bottom")


def clear_png_outputs(directory: Path) -> None:
    for path in directory.glob("*.png"):
        path.unlink()


def find_candidate(directory: Path, domain: str) -> Optional[Path]:
    matches = []
    for suffix in ("png", "ppm"):
        matches.extend(directory.glob(f"*-{domain}.{suffix}"))
    return sorted(matches)[0] if matches else None


def label(image: Image.Image, text: str, right: bool = False) -> None:
    draw = ImageDraw.Draw(image)
    font = ImageFont.load_default()
    box = draw.textbbox((0, 0), text, font=font)
    width = box[2] - box[0]
    x = image.width - width - 4 if right else 4
    # Labels sit inside the captured pixels; the compositor adds no gutters,
    # headers, scaling, or interpolation around either 480x272 source.
    draw.text((x + 1, 3), text, fill=(255, 255, 255), font=font)
    draw.text((x, 2), text, fill=(0, 0, 0), font=font)


def checked_frame(path: Path, description: str) -> Image.Image:
    image = Image.open(path).convert("RGB")
    if image.size != (WIDTH, HEIGHT):
        raise ValueError(
            f"{description}: expected {WIDTH}x{HEIGHT}; got {image.size}"
        )
    return image


def save_sheets(rows: list[Image.Image], output: Path, prefix: str,
                rows_per_sheet: int) -> int:
    for start in range(0, len(rows), rows_per_sheet):
        group = rows[start:start + rows_per_sheet]
        sheet = Image.new("RGB", (group[0].width, HEIGHT * len(group)))
        for index, row in enumerate(group):
            sheet.paste(row, (0, HEIGHT * index))
        number = start // rows_per_sheet + 1
        name = f"{prefix}-sheet-{number:02d}.png" if prefix \
            else f"sheet-{number:02d}.png"
        sheet.save(output / name)
    return (len(rows) + rows_per_sheet - 1) // rows_per_sheet


def compose_default(references: Path, candidates: Path, output: Path,
                    rows_per_sheet: int) -> None:
    clear_png_outputs(output)
    rows: list[tuple[str, Image.Image]] = []
    for metadata_path in sorted(references.glob("[0-9][0-9]-*.json")):
        metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
        domain = metadata["domain"]
        reference_path = metadata_path.with_suffix(".png")
        candidate_path = find_candidate(candidates, domain)
        if not reference_path.exists() or candidate_path is None:
            continue
        reference = checked_frame(reference_path, f"{domain} reference")
        candidate = checked_frame(candidate_path, f"{domain} candidate")
        label(reference, f"Mobile browser - {domain}")
        label(candidate, "Tilefinch", right=True)
        row = Image.new("RGB", (WIDTH * 2, HEIGHT))
        row.paste(reference, (0, 0))
        row.paste(candidate, (WIDTH, 0))
        row.save(output / f"{metadata_path.stem}.png")
        rows.append((domain, row))

    sheet_count = save_sheets(
        [row for _, row in rows], output, "", rows_per_sheet,
    ) if rows else 0
    print(
        f"composed {len(rows)} exact 960x272 rows into {sheet_count} sheets"
    )


def compose_full_audit(references: Path, candidates: Path, output: Path,
                       rows_per_sheet: int) -> None:
    raw_output = output / "raw"
    reader_output = output / "reader"
    raw_output.mkdir(parents=True, exist_ok=True)
    reader_output.mkdir(parents=True, exist_ok=True)
    # A refreshed invalid or missing capture must not leave a prior strip in
    # place where it could be mistaken for current evidence.
    clear_png_outputs(raw_output)
    clear_png_outputs(reader_output)
    raw_by_position: dict[str, list[Image.Image]] = {
        position: [] for position in POSITIONS
    }
    reader_by_position: dict[str, list[Image.Image]] = {
        position: [] for position in POSITIONS
    }
    raw_strips = 0
    reader_strips = 0
    summary_path = candidates / "audit-summary.json"
    try:
        summary = json.loads(summary_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        summary = {}
    candidate_audit = {
        item.get("name"): item
        for item in summary.get("results", [])
        if isinstance(item, dict) and isinstance(item.get("name"), str)
    }

    for metadata_path in sorted(references.glob("[0-9][0-9]-*.json")):
        metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
        if (metadata.get("status", 0) <= 0
                or metadata.get("devicePixelRatio") != 1
                or metadata.get("audit", {}).get("mode") != "full"):
            continue
        domain = metadata["domain"]
        stem = metadata_path.stem
        candidate_record = candidate_audit.get(stem, {})
        raw_rows = []
        reader_rows = []
        for position in POSITIONS:
            reference_path = references / f"{stem}-{position}.png"
            raw_path = candidates / "raw" / f"{stem}-{position}.ppm"
            reader_path = candidates / "reader" / f"{stem}-{position}.ppm"
            raw_capture = candidate_record.get("modes", {}).get(
                "raw", {}
            ).get(position, {})
            reader_capture = candidate_record.get("modes", {}).get(
                "reader", {}
            ).get(position, {})
            if (reference_path.exists() and raw_path.exists()
                    and raw_capture.get("status") == 0
                    and raw_capture.get("valid") is True):
                reference = checked_frame(
                    reference_path, f"{domain} {position} reference",
                )
                raw = checked_frame(raw_path, f"{domain} {position} raw")
                label(reference, f"Chrome raw | {position} | {domain}")
                label(raw, f"Tilefinch raw | {position}", right=True)
                row = Image.new("RGB", (WIDTH * 2, HEIGHT))
                row.paste(reference, (0, 0))
                row.paste(raw, (WIDTH, 0))
                row.save(raw_output / f"{stem}-{position}.png")
                raw_rows.append(row)
                raw_by_position[position].append(row)
            if (reader_path.exists()
                    and reader_capture.get("status") == 0
                    and reader_capture.get("valid") is True):
                reader = checked_frame(
                    reader_path, f"{domain} {position} Reader",
                )
                label(reader, f"Tilefinch Reader | {position} | {domain}")
                reader.save(reader_output / f"{stem}-{position}.png")
                reader_rows.append(reader)
                reader_by_position[position].append(reader)
        if raw_rows:
            strip = Image.new("RGB", (WIDTH * 2, HEIGHT * len(raw_rows)))
            for index, row in enumerate(raw_rows):
                strip.paste(row, (0, HEIGHT * index))
            strip.save(raw_output / f"{stem}-strip.png")
            raw_strips += 1
        if reader_rows:
            strip = Image.new("RGB", (WIDTH, HEIGHT * len(reader_rows)))
            for index, row in enumerate(reader_rows):
                strip.paste(row, (0, HEIGHT * index))
            strip.save(reader_output / f"{stem}-strip.png")
            reader_strips += 1

    raw_sheets = 0
    reader_sheets = 0
    for position in POSITIONS:
        raw_rows = raw_by_position[position]
        reader_rows = reader_by_position[position]
        if raw_rows:
            raw_sheets += save_sheets(
                raw_rows, raw_output, position, rows_per_sheet,
            )
        if reader_rows:
            reader_sheets += save_sheets(
                reader_rows, reader_output, position, rows_per_sheet,
            )
    print(
        f"composed full audit: {raw_strips} raw Chrome/Tilefinch strips "
        f"({raw_sheets} position sheets), {reader_strips} Tilefinch Reader "
        f"strips ({reader_sheets} position sheets)"
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--references", type=Path, required=True)
    parser.add_argument("--candidates", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--rows-per-sheet", type=int, default=5)
    parser.add_argument(
        "--full-audit", action="store_true",
        help=("compose top/middle/bottom raw Chrome/Tilefinch strips and "
              "separate Tilefinch Reader strips"),
    )
    args = parser.parse_args()
    if args.rows_per_sheet < 1:
        parser.error("--rows-per-sheet must be positive")
    args.output.mkdir(parents=True, exist_ok=True)
    if args.full_audit:
        compose_full_audit(
            args.references, args.candidates, args.output,
            args.rows_per_sheet,
        )
    else:
        compose_default(
            args.references, args.candidates, args.output,
            args.rows_per_sheet,
        )


if __name__ == "__main__":
    main()
