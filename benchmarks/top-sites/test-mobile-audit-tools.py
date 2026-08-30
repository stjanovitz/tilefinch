#!/usr/bin/env python3
"""Offline smoke tests for the local-only top-sites visual audit tools."""

import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

from PIL import Image


HERE = Path(__file__).resolve().parent


def load(name: str, filename: str):
    spec = importlib.util.spec_from_file_location(name, HERE / filename)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


RENDER = load("tilefinch_mobile_render", "render-tilefinch-mobile.py")
COMPOSE = load("tilefinch_mobile_compose", "compose-mobile-comparisons.py")


class MobileAuditToolsTest(unittest.TestCase):
    def test_scroll_status_parser(self):
        with tempfile.TemporaryDirectory() as temporary:
            log = Path(temporary) / "capture.log"
            log.write_text(
                "noise\ninteractive status=ok title=\"Example\" height=900 "
                "scroll-y=628 links=3 controls=1 ticks=2 callbacks=0 "
                "pending=0 relayouts=1\n",
                encoding="utf-8",
            )
            self.assertEqual(RENDER.extract_scroll_y(log), 628)
            self.assertEqual(
                RENDER.extract_scroll_status(log),
                {"height": 900, "scrollY": 628, "maximumScrollY": 628,
                 "links": 3, "controls": 1, "ticks": 2,
                 "callbacks": 0, "pendingTasks": 0, "relayouts": 1},
            )
            log.write_text("interactive status=failed\n", encoding="utf-8")
            self.assertIsNone(RENDER.extract_scroll_y(log))

    def test_full_render_plan_uses_product_reader_and_own_midpoints(self):
        commands = []

        def fake_capture(command, frame, log, timeout):
            del timeout
            commands.append(command)
            reader = "--reader-mode" in command
            height = 1072 if reader else 872
            if "--scroll-bottom" in command:
                scroll_y = height - 272
            elif "--scroll-y" in command:
                scroll_y = int(command[command.index("--scroll-y") + 1])
            else:
                scroll_y = 0
            frame.parent.mkdir(parents=True, exist_ok=True)
            frame.write_bytes(b"frame")
            log.write_text(
                f"interactive status=ok title=\"Example\" height={height} "
                f"scroll-y={scroll_y} links=3 controls=1 ticks=2 "
                "callbacks=0 pending=0 relayouts=1\n",
                encoding="utf-8",
            )
            return 0

        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            with mock.patch.object(RENDER, "run_capture", fake_capture):
                result = RENDER.render_full_audit(
                    Path("lab"), {"url": "https://example.test/"},
                    "01-example.test", output, 60,
                )
        self.assertEqual(result["modes"]["raw"]["middle"]["scrollY"], 300)
        self.assertEqual(result["modes"]["reader"]["middle"]["scrollY"], 400)
        self.assertTrue(result["modes"]["raw"]["bottom"]["valid"])
        self.assertTrue(result["modes"]["reader"]["middle"]["valid"])
        self.assertEqual(sum("--reader-mode" in item for item in commands), 3)
        self.assertEqual(
            sum("--hide-cookie-banners" in item for item in commands), 3,
        )
        self.assertFalse(any(
            "--reader-mode" in item and "--hide-cookie-banners" in item
            for item in commands
        ))
        self.assertEqual(
            sum("--probe-usability" in item for item in commands), 1,
        )

    def test_invalid_candidate_position_is_diagnostic_only(self):
        def fake_capture(command, frame, log, timeout):
            del timeout
            frame.parent.mkdir(parents=True, exist_ok=True)
            frame.write_bytes(b"frame")
            position = 500 if "--scroll-bottom" in command else 0
            log.write_text(
                "interactive status=ok title=\"Example\" height=1000 "
                f"scroll-y={position} links=3 controls=1 ticks=2 "
                "callbacks=0 pending=0 relayouts=1\n"
                "usability-probe focus=ready initial=authored "
                "activation=not-run action=link url=\"\" "
                "handlers=0/0 network=0/0\n",
                encoding="utf-8",
            )
            return 0

        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            with mock.patch.object(RENDER, "run_capture", fake_capture):
                result = RENDER.render_full_audit(
                    Path("lab"), {"url": "https://example.test/"},
                    "01-example.test", output, 60,
                )
            for mode in ("raw", "reader"):
                bottom = result["modes"][mode]["bottom"]
                self.assertFalse(bottom["valid"])
                self.assertEqual(bottom["reason"], "position-mismatch")
                self.assertEqual(bottom["usability"], {})
                self.assertFalse(
                    (output / mode / "01-example.test-bottom.ppm").exists()
                )
                self.assertTrue(
                    (output / mode
                     / "01-example.test-bottom-invalid.ppm").exists()
                )
                self.assertEqual(
                    result["modes"][mode]["middle"]["status"],
                    "skipped-no-bottom",
                )

    def test_middle_capture_converges_against_its_own_extent(self):
        def fake_capture(command, frame, log, timeout):
            del timeout
            frame.parent.mkdir(parents=True, exist_ok=True)
            if "--scroll-bottom" in command:
                height = 1000
                scroll_y = 728
            elif "--scroll-y" in command:
                height = 1200
                scroll_y = int(command[command.index("--scroll-y") + 1])
            else:
                height = 1000
                scroll_y = 0
            frame.write_bytes(b"frame")
            log.write_text(
                f"interactive status=ok title=\"Example\" height={height} "
                f"scroll-y={scroll_y} links=3 controls=1 ticks=2 "
                "callbacks=0 pending=0 relayouts=1\n",
                encoding="utf-8",
            )
            return 0

        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            with mock.patch.object(RENDER, "run_capture", fake_capture):
                result = RENDER.render_full_audit(
                    Path("lab"), {"url": "https://example.test/"},
                    "01-example.test", output, 60,
                )
            for mode in ("raw", "reader"):
                middle = result["modes"][mode]["middle"]
                self.assertTrue(middle["valid"])
                self.assertEqual(middle["actualScrollY"], 464)
                self.assertEqual(middle["attemptCount"], 2)
                self.assertFalse(middle["attempts"][0]["valid"])
                self.assertTrue(middle["attempts"][1]["valid"])
                self.assertTrue(
                    (output / mode / "01-example.test-middle.ppm").exists()
                )

    def test_default_and_full_composition(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            references = root / "references"
            candidates = root / "candidates"
            ordinary = root / "ordinary"
            full = root / "full"
            references.mkdir()
            candidates.mkdir()
            ordinary.mkdir()
            full.mkdir()
            stem = "01-example.test"
            metadata_path = references / f"{stem}.json"
            metadata = {
                "domain": "example.test", "url": "https://example.test/",
                "status": 200, "devicePixelRatio": 1,
            }
            metadata_path.write_text(
                json.dumps(metadata),
                encoding="utf-8",
            )
            Image.new("RGB", (480, 272), (10, 20, 30)).save(
                references / f"{stem}.png"
            )
            Image.new("RGB", (480, 272), (30, 20, 10)).save(
                candidates / f"{stem}.ppm"
            )
            COMPOSE.compose_default(references, candidates, ordinary, 5)
            self.assertTrue((ordinary / f"{stem}.png").is_file())
            self.assertTrue((ordinary / "sheet-01.png").is_file())
            self.assertFalse((ordinary / "sheet-sheet-01.png").exists())

            metadata["audit"] = {"mode": "full"}
            metadata_path.write_text(json.dumps(metadata), encoding="utf-8")
            (candidates / "raw").mkdir()
            (candidates / "reader").mkdir()
            for index, position in enumerate(COMPOSE.POSITIONS):
                Image.new("RGB", (480, 272), (index * 20, 40, 60)).save(
                    references / f"{stem}-{position}.png"
                )
                Image.new("RGB", (480, 272), (60, index * 20, 40)).save(
                    candidates / "raw" / f"{stem}-{position}.ppm"
                )
                Image.new("RGB", (480, 272), (40, 60, index * 20)).save(
                    candidates / "reader" / f"{stem}-{position}.ppm"
                )
            captures = {
                position: {"status": 0, "valid": True}
                for position in COMPOSE.POSITIONS
            }
            (candidates / "audit-summary.json").write_text(
                json.dumps({
                    "mode": "full",
                    "results": [{
                        "name": stem,
                        "modes": {"raw": captures, "reader": captures},
                    }],
                }),
                encoding="utf-8",
            )
            COMPOSE.compose_full_audit(references, candidates, full, 5)
            with Image.open(full / "raw" / f"{stem}-strip.png") as image:
                self.assertEqual(image.size, (960, 816))
            with Image.open(full / "reader" / f"{stem}-strip.png") as image:
                self.assertEqual(image.size, (480, 816))
            self.assertTrue((full / "raw" / "middle-sheet-01.png").is_file())
            self.assertTrue((full / "reader" / "bottom-sheet-01.png").is_file())

            # A later invalid refresh must remove rather than retain the old
            # per-site artifacts in a reused comparison directory.
            summary = json.loads(
                (candidates / "audit-summary.json").read_text(
                    encoding="utf-8"
                )
            )
            summary["results"][0]["modes"]["raw"]["middle"]["valid"] = False
            summary["results"][0]["modes"]["reader"]["middle"]["valid"] = False
            (candidates / "audit-summary.json").write_text(
                json.dumps(summary), encoding="utf-8"
            )
            COMPOSE.compose_full_audit(references, candidates, full, 5)
            with Image.open(full / "raw" / f"{stem}-strip.png") as image:
                self.assertEqual(image.size, (960, 544))
            with Image.open(full / "reader" / f"{stem}-strip.png") as image:
                self.assertEqual(image.size, (480, 544))
            self.assertFalse(
                (full / "raw" / f"{stem}-middle.png").exists()
            )
            self.assertFalse(
                (full / "reader" / f"{stem}-middle.png").exists()
            )

    def test_usability_signals_include_pixels_reader_and_activation(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            frame = root / "frame.ppm"
            log = root / "frame.log"
            image = Image.new("RGB", (480, 272), (255, 255, 255))
            for x in range(80):
                for y in range(40):
                    image.putpixel((x, y), (10, 20, 30))
            image.save(frame)
            log.write_text(
                "interactive status=ok title=\"Article\" height=900 "
                "scroll-y=0 links=4 controls=1 ticks=100 callbacks=2 "
                "pending=0 relayouts=3\n"
                "reader-mode kind=article high-confidence=yes entries=0 "
                "visited=50 bounded=no adapter=reader-content-shape "
                "available=yes\n"
                "reader-recovery available=yes kind=article "
                "high-confidence=no entries=0 visited=50 bounded=no "
                "applied=yes\n"
                "document-memory nodes=60 elements=20 text-nodes=30 "
                "attributes=4 attribute-bytes=20 body-text=800\n"
                "layout-retained bytes=100 command-size=32 commands=12/16 "
                "node-box-size=24 node-boxes=20/24 links=4/8 controls=1/4\n"
                "layout-presentation visually-blank=no\n"
                "javascript-error=\"\" source=\"\"\n"
                "usability-probe focus=ready initial=none "
                "activation=not-run action=link url=\"\" "
                "handlers=0/0 network=0/0\n",
                encoding="utf-8",
            )
            signals = RENDER.extract_usability_signals(log, frame)
        self.assertEqual(signals["classification"], "useful")
        self.assertTrue(signals["useful"])
        self.assertFalse(signals["blank"])
        self.assertEqual(signals["readerKind"], "article")
        self.assertTrue(signals["readerHighConfidence"])
        self.assertTrue(signals["readerAvailable"])
        self.assertEqual(signals["readerRecoveryKind"], "article")
        self.assertTrue(signals["readerRecoveryAvailable"])
        self.assertFalse(signals["readerRecoveryHighConfidence"])
        self.assertFalse(signals["readerRecoveryBoundedOut"])
        self.assertTrue(signals["readerRecoveryApplied"])
        self.assertTrue(signals["probeFocusReady"])
        self.assertFalse(signals["probeActivationAttempted"])
        self.assertIsNone(signals["probeActivationAccepted"])
        self.assertEqual(signals["probeAction"], "link")
        self.assertTrue(signals["probeSideEffectsStable"])
        self.assertGreater(signals["foregroundPixelShare"], 0.02)

        summary = RENDER.summarize_usability([{
            "usability": {"raw": signals, "reader": signals},
        }])
        self.assertEqual(summary["rawClassifications"], {"useful": 1})
        self.assertEqual(summary["readerKinds"], {"article": 1})
        self.assertEqual(summary["readerAvailability"],
                         {"reported": 1, "available": 1})
        self.assertEqual(summary["rawProbe"]["activationAttempted"], 0)
        self.assertEqual(summary["rawProbe"]["activationAccepted"], 0)
        self.assertEqual(summary["rawReaderRecovery"],
                         {"reported": 1, "applied": 1})

    def test_authoritative_blank_layout_overrides_hidden_body_text(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            frame = root / "frame.ppm"
            log = root / "frame.log"
            Image.new("RGB", (480, 272), (250, 250, 250)).save(frame)
            log.write_text(
                "interactive status=ok title=\"Hidden shell\" height=272 "
                "scroll-y=0 links=0 controls=0 ticks=100 callbacks=1 "
                "pending=0 relayouts=1\n"
                "document-memory nodes=40 elements=15 text-nodes=20 "
                "attributes=3 attribute-bytes=12 body-text=16000\n"
                "layout-retained bytes=64 command-size=32 commands=1/2 "
                "node-box-size=24 node-boxes=1/2 links=0/0 controls=0/0\n"
                "layout-presentation visually-blank=yes\n",
                encoding="utf-8",
            )
            signals = RENDER.extract_usability_signals(log, frame)
        self.assertTrue(signals["blank"])
        self.assertTrue(signals["layoutVisuallyBlank"])
        self.assertEqual(signals["classification"], "blank")
        self.assertEqual(signals["bodyTextBytes"], 16000)

    def test_pointer_focus_probe_is_retained_as_diagnostic_shape(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            frame = root / "frame.ppm"
            log = root / "frame.log"
            Image.new("RGB", (480, 272), (240, 240, 240)).save(frame)
            log.write_text(
                "interactive status=ok title=\"Pointer shell\" height=272 "
                "scroll-y=0 links=0 controls=0 ticks=2 callbacks=0 "
                "pending=0 relayouts=1\n"
                "usability-probe focus=missing initial=pointer "
                "activation=not-run action=pointer url=\"\" "
                "handlers=0/0 network=0/0\n",
                encoding="utf-8",
            )
            signals = RENDER.extract_usability_signals(log, frame)
        self.assertEqual(signals["probeInitial"], "pointer")
        self.assertFalse(signals["probeFocusReady"])
        self.assertEqual(signals["probeAction"], "pointer")


if __name__ == "__main__":
    unittest.main()
