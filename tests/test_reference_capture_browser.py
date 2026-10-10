#!/usr/bin/env python3
"""Offline browser regression: redirects, lazy images, capture identity.

No server or network is required. Playwright is optional in public clones;
maintainers run this lane with their installed browser before capture changes.
"""
import json
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "benchmarks"))
from visual_scenario import trace_digest, load_manifest, validate_state
from reference_frame import read_png

NODE = shutil.which("node")
PLAYWRIGHT = NODE and subprocess.run(
    [NODE, "-e", "require('playwright')"], capture_output=True).returncode == 0
UA = "Mozilla/5.0 (PlayStation Portable; Tilefinch/0.1)"
spec = importlib.util.spec_from_file_location("capture_census", ROOT / "benchmarks/capture-census.py")
census = importlib.util.module_from_spec(spec)
spec.loader.exec_module(census)


class CaptureReceiptTests(unittest.TestCase):
    @staticmethod
    def complete_state():
        return {"capture_ready": True,
                "browser_request_audit": {"ready": True, "observed": 1, "intercepted": 1,
                    "pending": 0, "overflow": False, "failures": []},
                "target_interception": {"ready": True, "pending_commands": 0,
                    "failures": 0, "overflow": False},
                "full_document": {"version": "bounded-viewport-sweep-v2", "complete": True,
                    "full_screenshot": False, "full_screenshot_status": "omitted-mobile-emulation",
                    "viewport_css": {"width": 480, "height": 272},
                    "viewports": [{"fraction": p / 100, "frame": f"view-{p}.png", "ready": True,
                        "incomplete": 0, "truncated": False, "document_width": 480,
                        "document_height": 272, "maximum": 0, "scroll_y": 0, "requested": 0,
                        "input_profile": {point: {"coarse": True, "touch": 1, "width": 480, "height": 272}
                                          for point in ("before", "after")}}
                        for p in (0, 25, 50, 75, 100)],
                    "final_extent": {"width": 480, "height": 272, "maximum": 0,
                        "incomplete": 0, "ready": True},
                    },
                "checkpoints": [{"frame": "middle.png"}]}

    def test_malformed_state_and_receipts_are_not_resumable(self):
        for value in (None, [], {"capture_ready": True, "browser_request_audit": []},
                      {"capture_ready": True, "browser_request_audit": {"ready": True}, "full_document": []}):
            self.assertFalse(census.complete_state(value))
            self.assertFalse(census.receipt_valid(value, {}, Path("/tmp")))

    def test_whole_attempt_timeout_is_terminal(self):
        with tempfile.TemporaryFile() as log:
            self.assertEqual(census.run_bounded([sys.executable, "-c", "import time;time.sleep(60)"],
                                               log, log, .1), 124)
    def test_resume_requires_state_and_every_mandatory_artifact(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            attempt = root / "attempt-1"
            output = attempt / "fixture"
            output.mkdir(parents=True)
            state = self.complete_state()
            (output / "reference-state.json").write_text(json.dumps(state))
            paths = [attempt / "stdout.log", attempt / "stderr.log"]
            paths.extend(output / name for name in ("view-0.png", "view-25.png",
                "view-50.png", "view-75.png", "view-100.png", "top.png", "bottom.png", "middle.png"))
            for path in paths:
                path.write_bytes(b"receipt fixture")
            paths.append(output / "reference-state.json")
            audit = output / "capture-audit.json"
            audit.write_text(json.dumps({"state_sha256": census.file_digest(output / "reference-state.json")}))
            paths.append(audit)
            identity = {"trace": "pinned"}
            receipt = {"identity": identity, "status": "complete", "exit": 0,
                       "attempt": str(attempt), "output": str(output), "scenario": "fixture",
                       "artifacts": {str(path.relative_to(root)): census.file_digest(path) for path in paths}}
            self.assertTrue(census.receipt_valid(receipt, identity, root))
            self.assertFalse(census.receipt_valid(receipt, {"trace": "changed"}, root))
            saved = receipt["artifacts"].pop("attempt-1/fixture/middle.png")
            self.assertFalse(census.receipt_valid(receipt, identity, root))
            receipt["artifacts"]["attempt-1/fixture/middle.png"] = saved
            (output / "middle.png").write_bytes(b"corrupt")
            self.assertFalse(census.receipt_valid(receipt, identity, root))

    def test_resume_refuses_missing_or_mixed_geometry_evidence(self):
        self.assertTrue(census.complete_state(self.complete_state()))
        for field in ("viewports", "final_extent", "viewport_css", "version"):
            state = self.complete_state()
            del state["full_document"][field]
            self.assertFalse(census.complete_state(state))
        state = self.complete_state()
        state["full_document"]["viewports"][2]["document_height"] = 500
        self.assertFalse(census.complete_state(state))
        state = self.complete_state()
        state["full_document"]["viewports"][1]["fraction"] = .75
        self.assertFalse(census.complete_state(state))
        state = self.complete_state()
        state["checkpoints"] = []
        self.assertFalse(census.complete_state(state))
        state = self.complete_state()
        state["full_document"]["viewports"][2]["input_profile"]["after"]["coarse"] = False
        self.assertFalse(census.complete_state(state))
        state = self.complete_state()
        state["full_document"]["full_screenshot"] = True
        self.assertFalse(census.complete_state(state))

    def test_geometry_retry_requires_preserved_screenshots(self):
        state = {"checkpoints": [], "full_document": {"geometry_attempts": [{
            "directory": "geometry-attempt-1", "views": [
                {"frame": f"view-{p}.png"} for p in (0, 25, 50, 75, 100)]}]}}
        paths = census.required_artifacts(state, Path("attempt"), Path("attempt/site"))
        self.assertEqual(sum("geometry-attempt-1" in str(path) for path in paths), 5)
        state["full_document"]["geometry_attempts"][0]["directory"] = "../outside"
        with self.assertRaises(ValueError):
            census.required_artifacts(state, Path("attempt"), Path("attempt/site"))

    def test_receipt_cannot_escape_output_root(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.assertFalse(census.receipt_valid({"identity": {}, "status": "complete", "exit": 0,
                "attempt": str(root.parent), "output": str(root.parent / "fixture"),
                "scenario": "fixture", "artifacts": {"../outside": "digest"}}, {}, root))


def record(trace, number, url, body, content_type, status=200, headers=()):
    stem = trace / f"{number:04d}"
    stem.with_suffix(".body").write_bytes(body)
    fields = ["method=GET", f"url={url}", "success=1", f"status={status}",
              f"length={len(body)}", f"content-type={content_type}",
              f"request-user-agent={UA}", "set-cookie-count=0",
              f"response-header-count={len(headers)}"]
    fields.extend(f"response-header-{index}={header}" for index, header in enumerate(headers))
    stem.with_suffix(".meta").write_text("\n".join(fields) + "\n")


@unittest.skipUnless(PLAYWRIGHT, "Node/Playwright is unavailable")
class BrowserCaptureTests(unittest.TestCase):
    def capture(self, directory, *, missing=False, full=True, iframe=False, viewport_units=False, corrupt_frame_image=False, hidden_frame=False, pointer_media=False):
        trace = directory / "trace"
        trace.mkdir()
        # Native lazy loading requires the scroll sweep; the hidden broken
        # image must not falsely fail visual qualification.
        html = b'''<!doctype html><meta name="viewport" content="width=device-width">
<title>Capture fixture</title><style>body{margin:0}img{width:32px;height:32px}</style>
<h1>Fixture ready</h1><img src="/redirect.svg"><div style="height:800px"></div>
<img loading="lazy" src="/lazy.svg"><img hidden src="/hidden.svg">'''
        if viewport_units:
            html += (b'<div style="height:100vh;background:#00ff00"></div>'
                     b'<div style="height:16px;background:#0000ff"></div>')
        if pointer_media:
            html += (b'<style>#pointer{height:16px;background:#123456}'
                     b'@media(pointer:coarse){#pointer{background:#ff00ff}}</style>'
                     b'<div id="pointer"></div>')
        svg = b'<svg xmlns="http://www.w3.org/2000/svg" width="32" height="32"><rect width="32" height="32" fill="red"/></svg>'
        records = [("https://capture.test/", html, "text/html", 200, ()),
                   ("https://capture.test/redirect.svg", b"", "text/plain", 302,
                    ("Location: https://images.test/final.svg",)),
                   ("https://capture.test/lazy.svg", svg, "image/svg+xml", 200, ()),
                   ("https://capture.test/hidden.svg", svg, "image/svg+xml", 200, ())]
        if iframe:
            # Different registrable domains force Chromium's separate frame
            # target; the nested redirect must still remain fully offline.
            frame_style = b' style="display:none"' if hidden_frame else b''
            records[0] = ("https://capture.test/", html + b'<iframe' + frame_style + b' src="https://frame.example/frame"></iframe>',
                          "text/html", 200, ())
            records.extend([
                ("https://frame.example/frame", b'<img width="32" height="32" src="/redirect.svg">', "text/html", 200, ()),
                ("https://frame.example/redirect.svg", b"", "text/plain", 302,
                 ("Location: https://nested.example/final.svg",)),
                ("https://nested.example/final.svg", b"not an image" if corrupt_frame_image else svg,
                 "image/svg+xml", 200, ())])
        if not missing:
            records.append(("https://images.test/final.svg", svg, "image/svg+xml", 200, ()))
        for index, values in enumerate(records):
            record(trace, index, *values)
        (trace / "trace.meta").write_text(
            f"psp-http-trace-clock=1\norigin-ms=1000\ncapture-complete=yes\nrecord-count={len(records)}\n")
        header = (ROOT / "benchmarks/fidelity-scenarios.tsv").read_text().splitlines()[0].split("\t")
        row = dict.fromkeys(header, "-")
        row.update(scenario="capture-fixture", url="https://capture.test/", replay_dir="trace",
                   trace_sha256=trace_digest(trace), expected_http="200", required_title="Capture fixture",
                   required_state_marker="Fixture ready", device_width="480", device_height="272",
                   css_width="480", css_height="272", scale_numerator="1", scale_denominator="1",
                   checkpoints="top|selector:h1|bottom", reference_state="reference-state.json", limit_mb="32",
                   ticks="1", tick_ms="33", max_download_kb="4096", script_timeout_ms="1000",
                   script_heap_mb="8", script_total_mb="8", script_file_kb="512", script_count="16",
                   min_stylesheets_loaded="0", min_images_loaded="1", min_scripts_loaded="0",
                   min_network_completions="1", max_pending="0")
        manifest = directory / "manifest.tsv"
        manifest.write_text("\t".join(header) + "\n" + "\t".join(row[key] for key in header) + "\n")
        command = [NODE, str(ROOT / "benchmarks/capture-reference.js"), "--scenario", "capture-fixture",
                   "--manifest", str(manifest), "--trace-root", str(directory),
                   "--output-root", str(directory / "output"), "--settle-ms", "0"]
        if full:
            command.append("--full-document")
        if os.environ.get("PLAYWRIGHT_CHROMIUM_EXECUTABLE_PATH"):
            command.extend(["--executable", os.environ["PLAYWRIGHT_CHROMIUM_EXECUTABLE_PATH"]])
        completed = subprocess.run(command, capture_output=True, text=True, timeout=90)
        output = directory / "output/capture-fixture"
        state_file = output / ("reference-state.json" if completed.returncode == 0 else "reference-diagnostic.json")
        self.assertTrue(state_file.exists(), completed.stdout + completed.stderr)
        state = json.loads(state_file.read_text())
        if not full and completed.returncode == 0:
            reasons, _ = validate_state(load_manifest(manifest)[0], state, state_file, "reference")
            self.assertEqual(reasons, [], str(reasons))
        audit = json.loads((output / "capture-audit.json").read_text())
        return completed, {**state, **audit}, output

    def test_redirect_and_lazy_images_are_replayed_and_verified(self):
        with tempfile.TemporaryDirectory() as temp:
            result, state, output = self.capture(Path(temp))
            self.assertEqual(result.returncode, 0, json.dumps(state, indent=2))
            self.assertEqual(state["browser"]["user_agent"], UA)
            self.assertTrue(state["browser_request_audit"]["ready"])
            self.assertTrue(state["full_document"]["complete"])
            self.assertEqual(len(state["full_document"]["viewports"]), 5)
            self.assertFalse((output / "full-page.png").exists())
            self.assertEqual(state["full_document"]["full_screenshot_status"], "omitted-mobile-emulation")
            self.assertEqual(state["full_document"]["viewports"][-1]["incomplete"], 0)
            self.assertEqual(state["replay_ledger"]["requests"], 5)

    def test_corrupt_painted_child_image_refuses_complete_capture(self):
        for full in (False, True):
            with self.subTest(full=full), tempfile.TemporaryDirectory() as temp:
                result, state, _ = self.capture(Path(temp), iframe=True, full=full, corrupt_frame_image=True)
                self.assertEqual(result.returncode, 3, json.dumps(state, indent=2))
                self.assertTrue(state["browser_request_audit"]["ready"])
                self.assertIn("reference-paintable-images-incomplete", state["eligibility_reasons"])
                evidence = [checkpoint["visual_evidence"] for checkpoint in state["checkpoints"]]
                evidence.extend(state["full_document"]["viewports"])
                children = [child for entry in evidence for child in entry["child_frames"]]
                self.assertTrue(any(child["paintable"] and child["visual_evidence"]["incomplete"] == 1
                                    for child in children))

    def test_hidden_child_corruption_is_not_a_paintable_image_failure(self):
        with tempfile.TemporaryDirectory() as temp:
            result, state, _ = self.capture(Path(temp), iframe=True, hidden_frame=True, corrupt_frame_image=True)
            self.assertEqual(result.returncode, 0, json.dumps(state, indent=2))
            self.assertTrue(all(not child["paintable"] for checkpoint in state["checkpoints"]
                                for child in checkpoint["visual_evidence"]["child_frames"]))

    def test_missing_redirect_target_is_an_exact_acquisition_request(self):
        with tempfile.TemporaryDirectory() as temp:
            result, state, _ = self.capture(Path(temp), missing=True)
            self.assertEqual(result.returncode, 3, result.stderr)
            self.assertIn("reference-paintable-images-incomplete", state["eligibility_reasons"])
            urls = [entry["url"] for entry in state["acquisition_plan"]["requests"]]
            self.assertIn("https://images.test/final.svg", urls)

    def test_viewport_sweep_preserves_viewport_unit_geometry(self):
        with tempfile.TemporaryDirectory() as temp:
            result, state, output = self.capture(Path(temp), viewport_units=True)
            self.assertEqual(result.returncode, 0, json.dumps(state, indent=2))
            full = state["full_document"]
            self.assertEqual(full["final_extent"]["height"], full["viewports"][0]["document_height"])
            width, height, pixels = read_png(output / "view-100.png")
            self.assertEqual((width, height), (480, 272))
            # Bottom viewport contains 256 rows of the 272px 100vh block
            # followed by its 16px blue sibling, never a resized 100vh box.
            green_rows = sum(pixels[(y * width + width // 2) * 3:
                                    (y * width + width // 2) * 3 + 3] == b'\x00\xff\x00'
                             for y in range(height))
            self.assertEqual(green_rows, 256)
            self.assertFalse(full["full_screenshot"])

    def test_viewport_sweep_preserves_coarse_pointer_pixels(self):
        with tempfile.TemporaryDirectory() as temp:
            result, state, output = self.capture(Path(temp), pointer_media=True)
            self.assertEqual(result.returncode, 0, json.dumps(state, indent=2))
            width, height, pixels = read_png(output / "view-100.png")
            coarse_rows = sum(pixels[(y * width + width // 2) * 3:
                                     (y * width + width // 2) * 3 + 3] == b'\xff\x00\xff'
                              for y in range(height))
            self.assertEqual(coarse_rows, 16,
                             "viewport capture changed mobile pointer media while painting")
            self.assertFalse(state["full_document"]["full_screenshot"])
            self.assertFalse((output / "full-page.png").exists())
            self.assertTrue(census.complete_state(json.loads((output / "reference-state.json").read_text())))

    def test_missing_redirect_target_is_not_false_success_in_ordinary_capture(self):
        with tempfile.TemporaryDirectory() as temp:
            result, state, _ = self.capture(Path(temp), missing=True, full=False)
            self.assertEqual(result.returncode, 3, result.stderr)
            self.assertFalse(state["capture_ready"])
            self.assertIn("reference-paintable-images-incomplete", state["eligibility_reasons"])

    def test_ordinary_capture_also_intercepts_redirect_targets(self):
        with tempfile.TemporaryDirectory() as temp:
            result, state, _ = self.capture(Path(temp), full=False)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue(state["browser_request_audit"]["ready"])
            self.assertEqual(state["response_scheduler"]["browser_ordinal_semantics"],
                             "raw-chromium-fetch-callback-v1")

    def test_cross_origin_frame_redirects_are_intercepted(self):
        with tempfile.TemporaryDirectory() as temp:
            result, state, _ = self.capture(Path(temp), iframe=True)
            self.assertEqual(result.returncode, 0, json.dumps(state, indent=2))
            self.assertTrue(state["browser_request_audit"]["ready"])
            self.assertEqual(state["replay_ledger"]["requests"], 8)
            self.assertGreaterEqual(state["target_interception"]["targets"], 2)
            self.assertTrue(state["target_interception"]["ready"])


if __name__ == "__main__":
    unittest.main()
