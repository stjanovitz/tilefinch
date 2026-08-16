#!/usr/bin/env python3
"""Normalize a PNG, JPEG, or PPM visual reference to canonical RGB PNG.

JPEG decoding uses Pillow or an explicitly discovered host image converter.
The normalized output is then decoded again by the dependency-free acceptance
decoder, so a converter cannot leave an unsupported or mislabeled artifact.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from reference_frame import (
    declared_raster_format,
    read_png,
    read_ppm,
    validate_declared_format,
    write_png_rgb,
)


def decode_jpeg(source: Path, output: Path) -> str:
    try:
        from PIL import Image  # type: ignore[import-not-found]

        with Image.open(source) as image:
            image.convert("RGB").save(output, format="PNG")
        return "pillow"
    except ImportError:
        pass

    candidates: list[tuple[str, list[str]]] = []
    if shutil.which("magick"):
        candidates.append(("magick", ["magick", str(source), str(output)]))
    if shutil.which("convert"):
        candidates.append(("imagemagick-convert", ["convert", str(source), str(output)]))
    if shutil.which("sips"):
        candidates.append(
            ("sips", ["sips", "-s", "format", "png", str(source), "--out", str(output)])
        )
    if shutil.which("ffmpeg"):
        candidates.append(
            (
                "ffmpeg",
                ["ffmpeg", "-v", "error", "-y", "-i", str(source), "-frames:v", "1", str(output)],
            )
        )
    errors: list[str] = []
    for name, command in candidates:
        completed = subprocess.run(
            command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True
        )
        if completed.returncode == 0 and output.is_file():
            return name
        detail = completed.stderr.strip().replace("\n", " ")
        errors.append(f"{name}: {detail or f'exit {completed.returncode}'}")
    suffix = f" ({'; '.join(errors)})" if errors else ""
    raise ValueError(
        "JPEG normalization needs Pillow, ImageMagick, sips, or ffmpeg" + suffix
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--width", type=int, default=480)
    parser.add_argument("--height", type=int, default=272)
    parser.add_argument(
        "--accept-mislabeled-input",
        action="store_true",
        help="explicitly acknowledge that the source suffix disagrees with its bytes",
    )
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args()

    temporary: Path | None = None
    try:
        if args.output.suffix.lower() != ".png":
            raise ValueError("normalized output must use a .png suffix")
        if args.source.resolve() == args.output.resolve():
            raise ValueError("source and normalized output must be different files")
        if args.output.exists() and not args.force:
            raise ValueError(f"{args.output}: output exists; pass --force to replace it")
        actual = validate_declared_format(
            args.source,
            accepted=("png", "jpeg", "ppm"),
            allow_mismatch=args.accept_mislabeled_input,
        )
        declared = declared_raster_format(args.source) or "none"
        args.output.parent.mkdir(parents=True, exist_ok=True)
        descriptor, temporary_name = tempfile.mkstemp(
            prefix=f".{args.output.name}.", suffix=".png", dir=args.output.parent
        )
        os.close(descriptor)
        temporary = Path(temporary_name)
        temporary.unlink()

        if actual == "png":
            width, height, pixels = read_png(
                args.source, allow_mismatch=args.accept_mislabeled_input
            )
            write_png_rgb(temporary, width, height, pixels)
            decoder = "builtin-png"
        elif actual == "ppm":
            width, height, pixels = read_ppm(
                args.source, allow_mismatch=args.accept_mislabeled_input
            )
            write_png_rgb(temporary, width, height, pixels)
            decoder = "builtin-ppm"
        else:
            decoder = decode_jpeg(args.source, temporary)

        width, height, pixels = read_png(temporary)
        if (width, height) != (args.width, args.height):
            raise ValueError(
                f"normalized frame is {width}x{height}, expected "
                f"{args.width}x{args.height}"
            )
        # Re-encode decoder output so every accepted artifact has one simple,
        # non-interlaced RGB representation regardless of the host backend.
        write_png_rgb(temporary, width, height, pixels)
        os.replace(temporary, args.output)
        temporary = None
        print(
            f"source_format={actual} declared_format={declared} decoder={decoder} "
            f"width={width} height={height} output={args.output}"
        )
        return 0
    except (OSError, ValueError) as error:
        print(f"reference normalization failed: {error}", file=sys.stderr)
        return 2
    finally:
        if temporary is not None:
            try:
                temporary.unlink()
            except FileNotFoundError:
                pass


if __name__ == "__main__":
    raise SystemExit(main())
