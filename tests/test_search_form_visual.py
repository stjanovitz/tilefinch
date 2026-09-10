#!/usr/bin/env python3
"""Hermetic pixel regression for the script-free search form and results.

Uses explicitly pinned repository font files, not machine-installed fonts. These
goldens pin a reviewed engine render, not a Chrome-fidelity score. They must
never be silently refreshed by a test run.
"""
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "benchmarks"))
from reference_frame import read_png, read_ppm, write_png_rgb


def render(lab, output, fixture, scrolls):
    output.mkdir(parents=True, exist_ok=True)
    # Samples may be clamped to a new page height after a layout change.
    # Never let a previous run's sample satisfy a missing current capture.
    for path in output.glob("*.ppm"):
        path.unlink()
    scroll_args = [arg for y in scrolls for arg in ("--save-scroll", str(y))]
    run = subprocess.run(
        [str(lab), "--fixture", str(ROOT / "tests/fixtures" / fixture),
         "--output-dir", str(output), "--skip-js",
         "--sans-font", str(ROOT / "fonts/TilefinchSans-Regular.ttf"),
         "--sans-bold-font", str(ROOT / "fonts/TilefinchSans-Bold.ttf"),
         "--sans-italic-font", str(ROOT / "fonts/DejaVuSans-Oblique-Latin.ttf"),
         "--serif-font", str(ROOT / "fonts/DejaVuSerif-Latin.ttf"),
         "--serif-bold-font", str(ROOT / "fonts/DejaVuSerif-Bold-Latin.ttf"),
         "--metric-sans-font", str(ROOT / "fonts/DejaVuSans-Latin.ttf"),
         "--metric-sans-bold-font", str(ROOT / "fonts/DejaVuSans-Bold-Latin.ttf"),
         "--limit-mb", "8", "--viewport-width", "480", "--viewport-height", "272",
         "--viewport-css-width", "480", "--viewport-css-height", "272",
         *scroll_args],
        capture_output=True, text=True, timeout=20, check=True)
    (output / "render.log").write_text(run.stdout + run.stderr)
    assert "benchmark status=ok" in run.stdout, "render did not complete"


def compare(output, cases):
    failures = []
    for name, filename in cases:
        width, height, pixels = read_ppm(output / filename)
        candidate = output / (name + ".png")
        write_png_rgb(candidate, width, height, pixels)
        expected = read_png(ROOT / "tests/visual/search-form" / (name + ".png"))
        assert (width, height) == expected[:2] == (480, 272)
        changed = sum(pixels[i:i+3] != expected[2][i:i+3]
                      for i in range(0, len(pixels), 3))
        if changed:
            failures.append(f"{name}: {changed} changed pixels; inspect {candidate}")
    if failures:
        raise AssertionError("\n".join(failures))


def main():
    lab, output = (Path(arg).resolve() for arg in sys.argv[1:])
    render(lab, output, "search-form-visual.html", [320])
    compare(output, (("search", "frame_00_y0000.ppm"),
                     ("results", "sample_00_y00320.ppm")))
    scrolled = output / "scrolled"
    render(lab, scrolled, "search-results-scroll-visual.html", [240, 816, 99999, 0])
    bottom = list(scrolled.glob("sample_02_y*.ppm"))
    assert len(bottom) == 1, "expected one clamped end-of-results capture"
    assert int(bottom[0].stem.rsplit("y", 1)[1]) > 1088, "results unexpectedly collapsed"
    compare(scrolled, (("results-middle", "sample_00_y00240.ppm"),
                       ("results-lower", "sample_01_y00816.ppm"),
                       ("results-end", bottom[0].name)))
    assert read_ppm(scrolled / "frame_00_y0000.ppm") == read_ppm(
        scrolled / "sample_03_y00000.ppm"), "scroll-back changed top-of-results pixels"
    print("search form, middle/lower results, pagination and scroll-back: "
          "pixel-identical at 480x272")


if __name__ == "__main__":
    main()
