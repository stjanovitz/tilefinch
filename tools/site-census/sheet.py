#!/usr/bin/env python3
"""Screenshot grid without Pillow: one row per page, one 480x272 cell per
frame (binary P6 PPM). A missing frame is a grey cell.

usage: sheet.py OUT.png ROW1_A,ROW1_B,ROW1_C ROW2_A,... (or import write_grid)
"""
import os
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "benchmarks"))
from reference_frame import read_ppm, write_png_rgb  # noqa: E402

W, H, GAP = 480, 272, 6


def cell(path):
    if not path or not os.path.exists(path):
        return [bytes([200, 200, 200]) * W for _ in range(H)]
    w, h, pixels = read_ppm(Path(path))
    out = []
    for y in range(H):
        r = pixels[y * w * 3:y * w * 3 + min(w, W) * 3] if y < h else b""
        out.append(r + bytes([255, 255, 255]) * (W - len(r) // 3))
    return out


def write_grid(output, rows):
    columns = max(len(r) for r in rows)
    width = W * columns + GAP * (columns + 1)
    sep = bytes([90, 90, 90])
    lines = []
    for row in rows:
        cells = [cell(p) for p in row] + [cell(None)] * (columns - len(row))
        lines += [sep * width] * GAP
        for y in range(H):
            lines.append(sep * GAP + (sep * GAP).join(c[y] for c in cells) + sep * GAP)
    lines += [sep * width] * GAP
    write_png_rgb(Path(output), width, len(lines), b"".join(lines))


if __name__ == "__main__":
    write_grid(sys.argv[1], [row.split(",") for row in sys.argv[2:]])
