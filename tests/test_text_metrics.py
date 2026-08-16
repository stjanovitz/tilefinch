#!/usr/bin/env python3

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
COMPARE = ROOT / "benchmarks" / "compare-text-metrics.py"
LAB = Path(sys.argv[1]).resolve() if len(sys.argv) >= 2 else None


def candidate_artifact() -> dict[str, object]:
    return {
        "schema": "tilefinch-text-metrics-v1",
        "source": "tilefinch",
        "viewport": {"width": 480, "height": 272},
        "document_height": 300,
        "eligible_runs": 2,
        "truncated": False,
        "run_count": 2,
        "runs": [
            {
                "index": 0, "text": "alpha", "x_26_6": 640,
                "y": 20, "advance_26_6": 1920, "coverage_width": 30,
                "line_height": 18, "baseline": 34, "font_size": 16,
                "font_family": 0, "font_weight": 400,
                "font_italic": False, "letter_spacing": 0,
                "face_backend": "stb-trusted",
            },
            {
                "index": 1, "text": "beta", "x_26_6": 2880,
                "y": 20, "advance_26_6": 1600, "coverage_width": 25,
                "line_height": 18, "baseline": 34, "font_size": 16,
                "font_family": 0, "font_weight": 400,
                "font_italic": False, "letter_spacing": 0,
                "face_backend": "stb-trusted",
            },
        ],
    }


def reference_artifact() -> dict[str, object]:
    return {
        "schema": "tilefinch-text-metrics-v1",
        "source": "chromium",
        "viewport": {"width": 480, "height": 272},
        "document_height": 300,
        "eligible_runs": 2,
        "truncated": False,
        "run_count": 2,
        "runs": [
            {
                "index": 0, "text": "alpha", "x_26_6": 640,
                "y_26_6": 1280, "advance_26_6": 1888,
                "rect_height_26_6": 1152, "font_size_26_6": 1024,
                "line_height_26_6": 1152, "letter_spacing_26_6": 0,
                "word_spacing_26_6": 0, "font_family": "Arial",
                "font_weight": "400", "font_style": "normal",
            },
            {
                "index": 1, "text": "beta", "x_26_6": 2880,
                "y_26_6": 1280, "advance_26_6": 1600,
                "rect_height_26_6": 1152, "font_size_26_6": 1024,
                "line_height_26_6": 1152, "letter_spacing_26_6": 0,
                "word_spacing_26_6": 0, "font_family": "Arial",
                "font_weight": "400", "font_style": "normal",
            },
        ],
    }


class TextMetricsComparatorTest(unittest.TestCase):
    def test_comparator_reports_bounded_run_geometry(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            candidate = root / "candidate.json"
            reference = root / "reference.json"
            candidate.write_text(json.dumps(candidate_artifact()), encoding="utf-8")
            reference.write_text(json.dumps(reference_artifact()), encoding="utf-8")
            completed = subprocess.run(
                [sys.executable, str(COMPARE), str(candidate), str(reference),
                 "--min-match-coverage", "1", "--max-mean-advance-error-px", "0.3",
                 "--min-line-break-agreement", "1"],
                capture_output=True, text=True,
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)
            report = json.loads(completed.stdout)
            self.assertEqual(report["matched_runs"], 2)
            self.assertEqual(report["advance_absolute_error_px"]["max"], 0.5)
            self.assertEqual(report["line_break_agreement"], 1.0)

    def test_comparator_fails_closed_on_invalid_source(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            candidate = root / "candidate.json"
            reference = root / "reference.json"
            invalid = candidate_artifact()
            invalid["source"] = "chromium"
            candidate.write_text(json.dumps(invalid), encoding="utf-8")
            reference.write_text(json.dumps(reference_artifact()), encoding="utf-8")
            completed = subprocess.run(
                [sys.executable, str(COMPARE), str(candidate), str(reference)],
                capture_output=True, text=True,
            )
            self.assertEqual(completed.returncode, 2)
            self.assertIn("expected source tilefinch", completed.stderr)


@unittest.skipUnless(LAB is not None, "interactive lab path not supplied")
class NativeTextMetricsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.lab = LAB

    def test_interactive_lab_emits_final_text_run_metrics(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixture = root / "fixture.html"
            metrics = root / "metrics.json"
            frame = root / "frame.ppm"
            fixture.write_text(
                "<!doctype html><style>body{font:10pt sans-serif;margin:0}"
                "span{letter-spacing:1px}</style><span>alpha</span> <span>beta</span>",
                encoding="utf-8",
            )
            completed = subprocess.run(
                [str(self.lab), "--fixture", str(fixture),
                 "--no-external-resources", "--limit-mb", "8",
                 "--dump-text-metrics", str(metrics), "--output", str(frame)],
                capture_output=True, text=True,
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)
            artifact = json.loads(metrics.read_text(encoding="utf-8"))
            self.assertEqual(artifact["schema"], "tilefinch-text-metrics-v1")
            self.assertEqual(artifact["source"], "tilefinch")
            by_text = {run["text"]: run for run in artifact["runs"]}
            self.assertIn("alpha", by_text)
            self.assertIn("beta", by_text)
            self.assertGreater(by_text["alpha"]["advance_26_6"], 0)
            self.assertEqual(by_text["alpha"]["font_size_26_6"], 853)
            self.assertEqual(by_text["alpha"]["letter_spacing"], 1)
            self.assertFalse(artifact["truncated"])


if __name__ == "__main__":
    # Keep unittest from interpreting the executable path as a test name.
    unittest.main(argv=[sys.argv[0]])
