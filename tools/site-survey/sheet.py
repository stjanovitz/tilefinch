#!/usr/bin/env python3
"""Comparison sheet without Pillow: two columns (Tilefinch | Chrome), one row
per checkpoint. Inputs may be binary P6 PPM or 8-bit gray/RGB/RGBA non-interlaced PNG.
usage: sheet.py OUT.png LABEL_LEFT LABEL_RIGHT left1,right1 left2,right2 ...
(a '-' entry leaves that cell grey)."""
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "benchmarks"))
from reference_frame import read_png, read_ppm, write_png_rgb  # noqa: E402

W, H, GAP = 480, 272, 6


def load(path):
    """(width, height, rows of RGB bytes), or None for '-'. PNG alpha is
    composited over white."""
    if path == "-":
        return None
    path = Path(path)
    w, h, pixels = read_png(path) if path.suffix == ".png" else read_ppm(path)
    return w, h, [pixels[y * w * 3:(y + 1) * w * 3] for y in range(h)]


def fit(img):
    """Crop/pad to W x H; upscale 2x when the input is half size."""
    if img is None:
        return [bytes([200, 200, 200]) * W for _ in range(H)]
    w, h, rows = img
    if w * 2 == W and h * 2 == H:
        out = []
        for r in rows:
            wide = bytearray()
            for i in range(0, len(r), 3):
                wide += r[i:i + 3] * 2
            out += [bytes(wide), bytes(wide)]
        rows, w, h = out, W, H
    out = []
    for y in range(H):
        r = rows[y] if y < h else b""
        r = r[:W * 3]
        out.append(r + bytes([255, 255, 255]) * (W - len(r) // 3))
    return out


def main():
    out, pairs = sys.argv[1], sys.argv[4:]
    width = W * 2 + GAP * 3
    sep = bytes([90, 90, 90])
    rows = []
    for pair in pairs:
        left, right = pair.split(",")
        a, b = fit(load(left)), fit(load(right))
        rows += [sep * width] * GAP
        for y in range(H):
            rows.append(sep * GAP + a[y] + sep * GAP + b[y] + sep * GAP)
    rows += [sep * width] * GAP
    write_png_rgb(Path(out), width, len(rows), b"".join(rows))


if __name__ == "__main__":
    main()
