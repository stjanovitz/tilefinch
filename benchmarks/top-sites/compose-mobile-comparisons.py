#!/usr/bin/env python3
"""Compose local-only, pixel-exact 480x272 browser comparisons."""

import argparse
import json
from pathlib import Path
from typing import Optional

from PIL import Image, ImageDraw, ImageFont


WIDTH = 480
HEIGHT = 272


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


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--references", type=Path, required=True)
    parser.add_argument("--candidates", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--rows-per-sheet", type=int, default=5)
    args = parser.parse_args()
    if args.rows_per_sheet < 1:
        parser.error("--rows-per-sheet must be positive")
    args.output.mkdir(parents=True, exist_ok=True)

    rows: list[tuple[str, Image.Image]] = []
    for metadata_path in sorted(args.references.glob("[0-9][0-9]-*.json")):
        metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
        domain = metadata["domain"]
        reference_path = metadata_path.with_suffix(".png")
        candidate_path = find_candidate(args.candidates, domain)
        if not reference_path.exists() or candidate_path is None:
            continue
        reference = Image.open(reference_path).convert("RGB")
        candidate = Image.open(candidate_path).convert("RGB")
        if reference.size != (WIDTH, HEIGHT) or candidate.size != (WIDTH, HEIGHT):
            raise ValueError(
                f"{domain}: expected two {WIDTH}x{HEIGHT} frames; "
                f"got {reference.size} and {candidate.size}"
            )
        label(reference, f"Mobile browser - {domain}")
        label(candidate, "Tilefinch", right=True)
        row = Image.new("RGB", (WIDTH * 2, HEIGHT))
        row.paste(reference, (0, 0))
        row.paste(candidate, (WIDTH, 0))
        row.save(args.output / f"{metadata_path.stem}.png")
        rows.append((domain, row))

    for start in range(0, len(rows), args.rows_per_sheet):
        group = rows[start:start + args.rows_per_sheet]
        sheet = Image.new("RGB", (WIDTH * 2, HEIGHT * len(group)))
        for index, (_, row) in enumerate(group):
            sheet.paste(row, (0, HEIGHT * index))
        number = start // args.rows_per_sheet + 1
        sheet.save(args.output / f"sheet-{number:02d}.png")

    print(
        f"composed {len(rows)} exact 960x272 rows into "
        f"{(len(rows) + args.rows_per_sheet - 1) // args.rows_per_sheet} sheets"
    )


if __name__ == "__main__":
    main()
