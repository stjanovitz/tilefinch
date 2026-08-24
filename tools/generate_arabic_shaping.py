#!/usr/bin/env python3
"""Generate Tilefinch's bounded Arabic-family joining/form tables.

Inputs are the version-matched Unicode ArabicShaping.txt and UnicodeData.txt.
Only Arabic-family base characters (U+0600..U+08FF) are emitted; the browser
does not claim Syriac or Mongolian shaping from this narrow first shaper.
"""

from __future__ import annotations

import argparse
from pathlib import Path


FORM_INDEX = {
    "<isolated>": 0,
    "<final>": 1,
    "<initial>": 2,
    "<medial>": 3,
}
JOIN_CODE = {"U": 0, "R": 1, "L": 2, "D": 3, "C": 4, "T": 5}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--arabic-shaping", required=True, type=Path)
    parser.add_argument("--unicode-data", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    joining: dict[int, int] = {}
    for raw in args.arabic_shaping.read_text(encoding="utf-8").splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        fields = [field.strip() for field in line.split(";")]
        codepoint = int(fields[0], 16)
        if 0x0600 <= codepoint <= 0x08FF and fields[2] != "U":
            joining[codepoint] = JOIN_CODE[fields[2]]

    forms: dict[int, list[int]] = {}
    for raw in args.unicode_data.read_text(encoding="utf-8").splitlines():
        fields = raw.split(";")
        codepoint = int(fields[0], 16)
        decomposition = fields[5].split()
        if len(decomposition) != 2 or decomposition[0] not in FORM_INDEX:
            continue
        base = int(decomposition[1], 16)
        if not 0x0600 <= base <= 0x08FF:
            continue
        slots = forms.setdefault(base, [0, 0, 0, 0])
        slots[FORM_INDEX[decomposition[0]]] = codepoint

    entries = sorted(set(joining) | set(forms))
    lines = [
        "/* Generated from Unicode 17.0 ArabicShaping.txt and UnicodeData.txt.",
        "   Regenerate with tools/generate_arabic_shaping.py. */",
        "static const ArabicShapeEntry arabic_shape_entries[] = {",
    ]
    for codepoint in entries:
        isolated, final, initial, medial = forms.get(codepoint, [0, 0, 0, 0])
        lines.append(
            "    {0x%04xu, 0x%04xu, 0x%04xu, 0x%04xu, 0x%04xu, %uu},"
            % (codepoint, isolated, final, initial, medial,
               joining.get(codepoint, 0))
        )
    lines.append("};")
    lines.append("")
    args.output.write_text("\n".join(lines), encoding="utf-8")


if __name__ == "__main__":
    main()
