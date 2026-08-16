#!/usr/bin/env python3

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BENCHMARKS = ROOT / "benchmarks"
sys.path.insert(0, str(BENCHMARKS))

from reference_frame import raster_format, write_png_rgb  # noqa: E402


class ReferenceFrameToolsTest(unittest.TestCase):
    def run_tool(self, name: str, *arguments: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(BENCHMARKS / name), *arguments],
            capture_output=True,
            text=True,
        )

    def write_frames(
        self,
        root: Path,
        width: int,
        height: int,
        candidate_pixels: bytes,
        reference_pixels: bytes,
    ) -> tuple[Path, Path]:
        candidate = root / "candidate.ppm"
        candidate.write_bytes(
            f"P6\n{width} {height}\n255\n".encode("ascii") + candidate_pixels
        )
        reference = root / "reference.png"
        write_png_rgb(reference, width, height, reference_pixels)
        return candidate, reference

    def test_compare_accepts_canonical_ppm_and_png(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            pixels = bytes((0, 16, 32, 255, 240, 224))
            candidate = root / "candidate.ppm"
            candidate.write_bytes(b"P6\n2 1\n255\n" + pixels)
            reference = root / "reference.png"
            write_png_rgb(reference, 2, 1, pixels)
            result = self.run_tool(
                "compare-reference-frame.py",
                str(candidate),
                str(reference),
                "--width",
                "2",
                "--height",
                "1",
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("2\t1\t0.0000\t0.0000", result.stdout)

    def test_compare_rejects_mislabeled_reference_before_decode(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            candidate = root / "candidate.ppm"
            candidate.write_bytes(b"P6\n1 1\n255\n\0\0\0")
            reference = root / "reference.png"
            reference.write_bytes(b"\xff\xd8\xff\xe0not-a-complete-jpeg")
            result = self.run_tool(
                "compare-reference-frame.py",
                str(candidate),
                str(reference),
                "--width",
                "1",
                "--height",
                "1",
            )
            self.assertEqual(result.returncode, 2)
            self.assertIn(
                "extension declares PNG but contents are JPEG", result.stderr
            )
            self.assertIn("normalize or rename", result.stderr)

    def test_compare_reports_rgb565_structural_and_foreground_metrics(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            # The raw samples differ, but both round-trip to the same RGB565
            # value. This is the primary PSP-aware comparison domain.
            candidate, reference = self.write_frames(
                root,
                3,
                3,
                bytes((8, 4, 8)) * 9,
                bytes((15, 7, 15)) * 9,
            )
            result = self.run_tool(
                "compare-reference-frame.py",
                str(candidate),
                str(reference),
                "--width",
                "3",
                "--height",
                "3",
                "--output-format",
                "json",
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            report = json.loads(result.stdout)
            self.assertGreater(report["metrics"]["mae_rgb"], 0.0)
            self.assertEqual(report["metrics"]["mae_rgb565"], 0.0)
            self.assertEqual(report["metrics"]["luma_ssim"], 1.0)
            self.assertEqual(report["metrics"]["luma_ms_ssim"], 1.0)
            self.assertEqual(report["metrics"]["edge_f1"], 1.0)
            self.assertTrue(report["metrics"]["candidate_blank"])
            self.assertEqual(report["qualification"]["status"], "diagnostic")
            self.assertEqual(report["qualification"]["thresholds"], {})

    def test_explicit_threshold_fails_qualification_but_diagnostic_does_not(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            candidate, reference = self.write_frames(
                root, 4, 4, bytes((0, 0, 0)) * 16, bytes((255, 255, 255)) * 16
            )
            diagnostic = self.run_tool(
                "compare-reference-frame.py",
                str(candidate),
                str(reference),
                "--width",
                "4",
                "--height",
                "4",
            )
            self.assertEqual(diagnostic.returncode, 0, diagnostic.stderr)
            qualified = self.run_tool(
                "compare-reference-frame.py",
                str(candidate),
                str(reference),
                "--width",
                "4",
                "--height",
                "4",
                "--max-mae-rgb565",
                "1",
                "--output-format",
                "json",
            )
            self.assertEqual(qualified.returncode, 1)
            report = json.loads(qualified.stdout)
            self.assertEqual(report["qualification"]["status"], "fail")
            self.assertEqual(
                report["qualification"]["thresholds"]["max_mae_rgb565"], 1.0
            )
            self.assertIn("RGB565 MAE", qualified.stderr)

    def test_blank_frame_check_is_opt_in(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            candidate, reference = self.write_frames(
                root, 4, 4, bytes((255, 255, 255)) * 16, bytes((255, 255, 255)) * 16
            )
            result = self.run_tool(
                "compare-reference-frame.py",
                str(candidate),
                str(reference),
                "--width",
                "4",
                "--height",
                "4",
                "--require-nonblank",
            )
            self.assertEqual(result.returncode, 1)
            self.assertIn("candidate frame is blank", result.stderr)

    def test_edge_f1_tolerates_one_pixel_alignment_difference(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            width = height = 7

            def line(column: int) -> bytes:
                pixels = bytearray(bytes((255, 255, 255)) * width * height)
                for y in range(height):
                    at = (y * width + column) * 3
                    pixels[at : at + 3] = b"\0\0\0"
                return bytes(pixels)

            candidate, reference = self.write_frames(
                root, width, height, line(2), line(3)
            )
            exact = self.run_tool(
                "compare-reference-frame.py",
                str(candidate),
                str(reference),
                "--width",
                str(width),
                "--height",
                str(height),
                "--edge-tolerance",
                "0",
                "--output-format",
                "json",
            )
            tolerant = self.run_tool(
                "compare-reference-frame.py",
                str(candidate),
                str(reference),
                "--width",
                str(width),
                "--height",
                str(height),
                "--edge-tolerance",
                "1",
                "--output-format",
                "json",
            )
            self.assertEqual(exact.returncode, 0, exact.stderr)
            self.assertEqual(tolerant.returncode, 0, tolerant.stderr)
            exact_f1 = json.loads(exact.stdout)["metrics"]["edge_f1"]
            tolerant_f1 = json.loads(tolerant.stdout)["metrics"]["edge_f1"]
            self.assertLess(exact_f1, tolerant_f1)
            self.assertEqual(tolerant_f1, 1.0)

    def test_geometry_anchors_are_aggregated_and_can_qualify(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            pixels = bytes((255, 255, 255)) * 16
            candidate, reference = self.write_frames(root, 4, 4, pixels, pixels)
            geometry = root / "geometry.json"
            geometry.write_text(
                json.dumps(
                    {
                        "anchors": [
                            {
                                "name": "header",
                                "candidate": {"x": 4, "y": 2, "width": 100},
                                "reference": {"x": 0, "y": 1, "width": 98},
                            },
                            {
                                "name": "main",
                                "candidate": {"height": 20},
                                "reference": {"height": 20},
                            },
                        ]
                    }
                ),
                encoding="utf-8",
            )
            result = self.run_tool(
                "compare-reference-frame.py",
                str(candidate),
                str(reference),
                "--width",
                "4",
                "--height",
                "4",
                "--geometry-anchors",
                str(geometry),
                "--max-geometry-error-px",
                "3",
                "--output-format",
                "json",
            )
            self.assertEqual(result.returncode, 1)
            report = json.loads(result.stdout)
            self.assertEqual(report["geometry"]["metrics"]["anchor_count"], 2)
            self.assertEqual(report["geometry"]["metrics"]["component_count"], 4)
            self.assertEqual(report["geometry"]["metrics"]["max_error_px"], 4.0)
            self.assertIn("geometry maximum error", result.stderr)

    def test_normalization_requires_explicit_mismatch_acknowledgement(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            mislabeled = root / "reference.jpg"
            write_png_rgb(mislabeled, 2, 1, b"\x10\x20\x30" * 2)
            normalized = root / "normalized.png"
            rejected = self.run_tool(
                "normalize-reference-frame.py",
                str(mislabeled),
                str(normalized),
                "--width",
                "2",
                "--height",
                "1",
            )
            self.assertEqual(rejected.returncode, 2)
            self.assertFalse(normalized.exists())
            accepted = self.run_tool(
                "normalize-reference-frame.py",
                str(mislabeled),
                str(normalized),
                "--width",
                "2",
                "--height",
                "1",
                "--accept-mislabeled-input",
            )
            self.assertEqual(accepted.returncode, 0, accepted.stderr)
            self.assertEqual(raster_format(normalized), "png")
            self.assertIn("source_format=png declared_format=jpeg", accepted.stdout)


if __name__ == "__main__":
    unittest.main()
