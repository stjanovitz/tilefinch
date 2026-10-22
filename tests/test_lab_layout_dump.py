#!/usr/bin/env python3
"""The lab's --dump-layout bold column reports the face paint uses.

A text command selects the bold face at the shared boundary
(draw_uses_bold_face, TILEFINCH_FONT_BOLD_FACE_WEIGHT), so semibold text
is painted with the regular face and must not be reported as bold.

Usage: test_lab_layout_dump.py <psp-browser-lab>
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

FIXTURE = (
    "<!doctype html><style>body{margin:0;font:16px sans-serif}</style>"
    "<p style=font-weight:400>Regular</p>"
    "<p style=font-weight:600>Semibold</p>"
    "<p style=font-weight:650>Boundary</p>"
    "<p style=font-weight:700>Bold</p>"
)
EXPECTED = {"Regular": "0", "Semibold": "0", "Boundary": "1", "Bold": "1"}


def main() -> int:
    lab = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        fixture = root / "weights.html"
        dump = root / "layout.tsv"
        fixture.write_text(FIXTURE, encoding="utf-8")
        completed = subprocess.run(
            [str(lab), "--fixture", str(fixture), "--skip-js", "--no-render",
             "--limit-mb", "8", "--dump-layout", str(dump)],
            capture_output=True, text=True, timeout=20)
        if completed.returncode != 0:
            print(completed.stdout + completed.stderr, file=sys.stderr)
            return 1
        bold = {}
        for line in dump.read_text(encoding="utf-8").splitlines():
            fields = line.split("\t")
            if len(fields) >= 10 and fields[0] == "text":
                bold[fields[9]] = fields[8]
    if {text: bold.get(text) for text in EXPECTED} != EXPECTED:
        print(f"bold column: expected {EXPECTED}, got "
              f"{ {text: bold.get(text) for text in EXPECTED} }",
              file=sys.stderr)
        return 1
    print("layout dump bold column follows the bold-face boundary")
    return 0


if __name__ == "__main__":
    sys.exit(main())
