#!/usr/bin/env python3
"""Shipping-font memory evidence must not silently use desktop fallback faces."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

lab = Path(sys.argv[1]).resolve()
root = Path(__file__).resolve().parents[1]
names = ["DejaVuSans-Latin.ttf", "DejaVuSerif-Latin.ttf",
         "DejaVuSans-Oblique-Latin.ttf", "DejaVuSans-Bold-Latin.ttf",
         "DejaVuSerif-Bold-Latin.ttf", "TilefinchSans-Regular.ttf",
         "TilefinchSans-Bold.ttf"]
expected = sum((root / "fonts" / name).stat().st_size for name in names)

with tempfile.TemporaryDirectory(prefix="tilefinch-shipping-fonts-") as tmp:
    work = Path(tmp)
    fixture = work / "page.html"
    fixture.write_text("<!doctype html><style>body{margin:12px}"
                       "p{font-size:18px} .serif{font-family:serif}</style>"
                       "<p>Readable text 0123456789</p>"
                       "<p class=serif>Reader text and spacing</p>"
                       "<a href='#end'>Next section</a><div style='height:400px'></div>"
                       "<p id=end>End of article</p>")
    outputs = []
    for shipping in (True, False):
        lane = work / ("shipping" if shipping else "desktop")
        lane.mkdir()
        commands = lane / "commands.txt"
        commands.write_text("focus-next\nrender " + str(lane / "focus.ppm")
                            + "\nscroll-by 180\npage-up\nquit\n")
        args = [str(lab), "--fixture", str(fixture), "--no-javascript",
                "--psp-profile", "realistic", "--no-loop-capture",
                "--commands", str(commands), "--output", str(lane / "initial.ppm")]
        if shipping:
            args.append("--psp-fonts")
        result = subprocess.run(args, capture_output=True, text=True, timeout=30)
        output = result.stdout + result.stderr
        assert result.returncode == 0, output
        receipt = re.search(r"runtime-fonts mode=(\w+) loaded-bytes=(\d+)", output)
        assert receipt, output
        assert receipt[1] == ("shipping" if shipping else "desktop"), output
        if shipping:
            assert int(receipt[2]) == expected, (receipt[0], expected)
        assert re.search(r"interactive teardown=0\b", output), output
        assert "interaction-latency command=focus-next" in output, output
        outputs.append((lane, output))
    for name in ("initial.ppm", "focus.ppm"):
        assert (outputs[0][0] / name).read_bytes() == (outputs[1][0] / name).read_bytes(), name

print("shipping-font admission, Latin pixels, focus and teardown: PASS")
