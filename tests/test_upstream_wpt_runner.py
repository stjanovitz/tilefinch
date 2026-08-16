#!/usr/bin/env python3

import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "benchmarks"))
import run_upstream_wpt as runner  # noqa: E402
import check_wpt_dom_interaction as dom_gate  # noqa: E402


class UpstreamWptRunnerTests(unittest.TestCase):
    @staticmethod
    def dom_gate_payload():
        results = [
            {
                "name": f"case-{index}",
                "status": "PASS",
                "pass_count": 0,
                "fail_count": 0,
                "timeout_count": 0,
                "notrun_count": 0,
                "skip_count": 0,
            }
            for index in range(dom_gate.EXPECTED_FILES)
        ]
        results[0]["pass_count"] = dom_gate.EXPECTED_PASSING_ASSERTIONS
        return {
            "schema": 2,
            "wpt_revision": dom_gate.EXPECTED_REVISION,
            "manifest": "/tmp/dom-interaction.tsv",
            "totals": {
                "PASS": dom_gate.EXPECTED_FILES,
                "FAIL": 0,
                "SKIP": 0,
                "HARNESS-ERROR": 0,
            },
            "results": results,
        }

    def test_dom_interaction_gate_accepts_all_green_contract(self):
        self.assertEqual(dom_gate.validate(self.dom_gate_payload()), [])

    def test_dom_interaction_gate_rejects_engine_regression(self):
        payload = self.dom_gate_payload()
        payload["results"][1]["status"] = "FAIL"
        payload["results"][1]["fail_count"] = 1
        payload["totals"]["PASS"] -= 1
        payload["totals"]["FAIL"] = 1
        errors = dom_gate.validate(payload)
        self.assertTrue(any("non-passing assertion" in error for error in errors))
        self.assertTrue(any("expected PASS" in error for error in errors))

    def test_dom_interaction_gate_rejects_new_harness_error(self):
        payload = self.dom_gate_payload()
        payload["results"][1]["status"] = "HARNESS-ERROR"
        payload["totals"]["PASS"] -= 1
        payload["totals"]["HARNESS-ERROR"] = 1
        errors = dom_gate.validate(payload)
        self.assertTrue(any("expected PASS" in error for error in errors))

    def test_selected_manifest_is_bounded_and_balanced(self):
        cases = runner.read_manifest(runner.DEFAULT_MANIFEST)
        self.assertEqual(len(cases), 89)
        self.assertEqual({case.suite for case in cases}, {"html", "css"})
        self.assertEqual(
            {case.kind for case in cases}, {"testharness", "reftest"}
        )
        self.assertEqual(
            sum(case.kind == "testharness" for case in cases), 68
        )
        self.assertEqual(sum(case.kind == "reftest" for case in cases), 21)
        self.assertEqual(
            sum(
                case.suite == "html" and case.kind == "testharness"
                for case in cases
            ),
            28,
        )
        self.assertEqual(
            sum(
                case.suite == "css" and case.kind == "testharness"
                for case in cases
            ),
            40,
        )
        self.assertGreaterEqual(len({case.panel for case in cases}), 10)

    def test_selected_manifest_matches_static_sparse_file_list(self):
        cases = runner.read_manifest(runner.DEFAULT_MANIFEST)
        paths_file = ROOT / "benchmarks" / "wpt" / "paths.txt"
        paths = {
            line.strip()
            for line in paths_file.read_text(encoding="utf-8").splitlines()
            if line.strip() and not line.lstrip().startswith("#")
        }
        required = {"resources/testharness.js"}
        for case in cases:
            required.add(case.test)
            if case.reference:
                required.add(case.reference)
            self.assertNotIn(".sub.", case.test)
            self.assertNotIn("testdriver", case.test)
        self.assertTrue(required.issubset(paths))

    def test_dom_interaction_manifest_is_static_and_panel_balanced(self):
        manifest = ROOT / "benchmarks" / "wpt" / "dom-interaction.tsv"
        paths_file = (
            ROOT / "benchmarks" / "wpt" / "dom-interaction-paths.txt"
        )
        cases = runner.read_manifest(manifest)
        paths = {
            line.strip()
            for line in paths_file.read_text(encoding="utf-8").splitlines()
            if line.strip() and not line.lstrip().startswith("#")
        }
        self.assertEqual(len(cases), 65)
        self.assertEqual(
            {
                panel: sum(case.panel == panel for case in cases)
                for panel in {case.panel for case in cases}
            },
            {
                "dom-core": 15,
                "dom-events": 15,
                "dom-observer": 10,
                "cssom-view": 15,
                "html-interaction": 10,
            },
        )
        self.assertEqual({case.kind for case in cases}, {"testharness"})
        self.assertEqual({case.suite for case in cases}, {"html", "css"})
        viewport_cases = [
            case for case in cases if case.viewport_css_height is not None
        ]
        self.assertEqual(
            [
                (
                    case.name,
                    case.viewport_css_width,
                    case.viewport_css_height,
                )
                for case in viewport_cases
            ],
            [("cssom-view-elements-from-point-simple", 480, 600)],
        )
        for case in cases:
            self.assertIn(case.test, paths)
            self.assertNotIn(".sub.", case.test)
            self.assertNotIn("testdriver", case.test)

    def test_secondary_sites_manifest_covers_stylesheet_checkpoint_rules(self):
        manifest = ROOT / "benchmarks" / "wpt" / "secondary-sites.tsv"
        paths_file = ROOT / "benchmarks" / "wpt" / "secondary-sites-paths.txt"
        cases = runner.read_manifest(manifest)
        paths = {
            line.strip()
            for line in paths_file.read_text(encoding="utf-8").splitlines()
            if line.strip() and not line.lstrip().startswith("#")
        }
        self.assertEqual(len(cases), 3)
        self.assertEqual(
            {case.panel for case in cases}, {"secondary-stylesheets"}
        )
        self.assertEqual({case.kind for case in cases}, {"reftest"})
        self.assertEqual(
            {case.name for case in cases},
            {
                "secondary-stylesheet-media",
                "secondary-stylesheet-base",
                "secondary-stylesheet-type-case",
            },
        )
        for case in cases:
            self.assertIn(case.test, paths)
            self.assertIn(case.reference, paths)
            self.assertNotIn(".sub.", case.test)
            self.assertNotIn("testdriver", case.test)

    def test_ppm_parser_preserves_whitespace_valued_first_pixel(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "frame.ppm"
            pixels = bytes((0x20, 0x0A, 0xFF))
            path.write_bytes(b"P6\n1 1\n255\n" + pixels)
            width, height, maximum, parsed = runner.parse_ppm(path)
            self.assertEqual((width, height, maximum), (1, 1, 255))
            self.assertEqual(parsed, pixels)

    def test_ppm_comparison_reports_pixel_and_channel_delta(self):
        with tempfile.TemporaryDirectory() as directory:
            left = Path(directory) / "left.ppm"
            right = Path(directory) / "right.ppm"
            left.write_bytes(b"P6\n2 1\n255\n" + bytes((0, 0, 0, 10, 20, 30)))
            right.write_bytes(
                b"P6\n2 1\n255\n" + bytes((0, 0, 0, 10, 35, 30))
            )
            self.assertEqual(runner.compare_ppm(left, right), (1, 15))

    def test_manifest_path_cannot_escape_wpt_root(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(ValueError):
                runner.safe_wpt_path(Path(directory), "../outside.html")

    def test_case_names_are_unique_for_targeted_iteration(self):
        cases = runner.read_manifest(runner.DEFAULT_MANIFEST)
        names = [case.name for case in cases]
        self.assertEqual(len(names), len(set(names)))

    @mock.patch.object(runner.subprocess, "run")
    def test_wpt_heap_cap_is_passed_separately_from_total_limit(self, run):
        run.return_value = mock.Mock(returncode=0, stdout="")
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "case.log"
            runner.run_lab(
                Path("/bounded-lab"),
                "http://127.0.0.1/test.html",
                Path(directory) / "case.ppm",
                log,
                16,
                10,
                24,
                6,
                480,
                272,
                30.0,
            )
        command = run.call_args.args[0]
        self.assertEqual(command[command.index("--limit-mb") + 1], "24")
        self.assertEqual(
            command[command.index("--script-heap-mb") + 1], "6"
        )
        self.assertEqual(
            command[command.index("--viewport-css-width") + 1], "480"
        )
        self.assertEqual(
            command[command.index("--viewport-css-height") + 1], "272"
        )


if __name__ == "__main__":
    unittest.main()
