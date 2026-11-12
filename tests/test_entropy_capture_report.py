#!/usr/bin/env python3
"""The validation entropy capture end to end on the host.

tests/psp_entropy_capture_driver.c builds src/psp_entropy.c with its capture
hooks over a modelled timer and records warm-up batches, a foreground
attempt and extra runs through the PSP recorder's sink and encoder. This
checks that the file holds every sample the clock produced, in measured
order (the pool's estimator sorts its own copy), with the pool's verdicts;
that extra runs leave the pool alone; that tools/entropy_capture_report.py
finds the two-clock beat in the deterministic model and nothing in the
jittered one; and that src/psp_entropy.c built as shipping carries no
capture code.

    test_entropy_capture_report.py DRIVER SHIPPING_OBJECT
"""

from __future__ import annotations

import json
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REPORT = ROOT / "tools" / "entropy_capture_report.py"
sys.path.insert(0, str(ROOT / "tools"))
import entropy_capture_report  # noqa: E402

DRIVER = None
SHIPPING_OBJECT = None


class EntropyCaptureTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.temporary = tempfile.TemporaryDirectory(
            prefix="tilefinch-entropy-capture-")
        cls.work = Path(cls.temporary.name)
        cls.runs = {}
        for mode in ("beat", "jitter"):
            capture = cls.work / f"{mode}.tfer"
            expected = cls.work / f"{mode}.expected"
            result = subprocess.run(
                [str(DRIVER), mode, str(capture), str(expected)],
                check=True, capture_output=True, text=True, timeout=60)
            raw = expected.read_bytes()
            cls.runs[mode] = {
                "line": result.stdout,
                "capture": capture,
                "expected": list(struct.unpack(f"<{len(raw) // 4}I", raw)),
            }
            json_path = cls.work / f"{mode}.json"
            subprocess.run([sys.executable, str(REPORT), str(capture),
                            "--json", str(json_path)], check=True,
                           capture_output=True, text=True, timeout=120)
            cls.runs[mode]["report"] = json.loads(
                json_path.read_text(encoding="utf-8"))

    @classmethod
    def tearDownClass(cls) -> None:
        cls.temporary.cleanup()

    def test_every_sample_is_kept_in_measured_order(self):
        for mode, run in self.runs.items():
            header, batches = entropy_capture_report.read_capture(
                run["capture"])
            self.assertEqual(header["dropped"], 0, mode)
            self.assertEqual((header["work_batch"], header["sleep_batch"],
                              header["work_touches"], header["sleep_us"],
                              header["clock_hz"]), (256, 64, 32, 1, 1000000))
            recorded = [value for batch in batches
                        for value in batch["samples"]]
            # A first-difference message: unittest's list diff of tens of
            # thousands of samples takes minutes.
            expected = run["expected"]
            first = next((at for at, (got, want)
                          in enumerate(zip(recorded, expected))
                          if got != want), None)
            self.assertTrue(len(recorded) == len(expected) and first is None,
                            f"{mode}: {len(recorded)} recorded vs "
                            f"{len(expected)} measured samples, first "
                            f"difference at {first}")
            # Sorted batches would hide a beat; the model's are not sorted.
            work = [batch for batch in batches if batch["source"] == "work"
                    and batch["phase"] != "extra"]
            self.assertTrue(any(batch["samples"] != sorted(batch["samples"])
                                for batch in work), mode)

    def test_records_carry_phase_source_and_the_pool_verdict(self):
        for mode, run in self.runs.items():
            _, batches = entropy_capture_report.read_capture(run["capture"])
            phases = [batch["phase"] for batch in batches]
            self.assertEqual(phases[:3], ["prepare"] * 3, mode)
            fill = [batch for batch in batches if batch["phase"] == "fill"]
            extra = [batch for batch in batches if batch["phase"] == "extra"]
            self.assertTrue(fill, mode)
            self.assertEqual(len(extra), 6, mode)
            self.assertEqual(phases, ["prepare"] * 3 + ["fill"] * len(fill)
                             + ["extra"] * 6, mode)
            pool = [batch for batch in batches if batch["phase"] != "extra"]
            # Warm-up and attempt each start on a work batch and alternate.
            for group in (pool[:3], fill):
                for at, batch in enumerate(group):
                    self.assertEqual(batch["source"],
                                     "work" if at % 2 == 0 else "sleep")
                    self.assertEqual(batch["count"],
                                     256 if at % 2 == 0 else 64)
            credited = 0
            for batch in pool:
                credited += batch["credited"]
                self.assertEqual(batch["pool_bits"], credited, mode)
            for batch in extra:
                self.assertEqual(batch["millibits"], 0xFFFFFFFF)
                self.assertEqual(batch["credited"], 0xFFFFFFFF)
                self.assertEqual(batch["pool_bits"], credited)
            self.assertEqual([batch["count"] for batch in extra],
                             [8192, 1024] * 3)
            self.assertEqual([batch["index"] for batch in batches],
                             list(range(len(batches))))
            self.assertIn("pool-unchanged-by-extras=1", run["line"])
            self.assertIn(f"pool-credited={credited} ", run["line"])
        # The beat earns nothing from the shipped estimator; jitter seeds.
        self.assertIn("pool-credited=0 ", self.runs["beat"]["line"])
        self.assertIn("released-credited=1", self.runs["jitter"]["line"])

    def test_the_report_finds_the_two_clock_beat(self):
        result = self.runs["beat"]["report"]
        for name in ("work/pool", "work/extra", "sleep/extra"):
            stream = result["streams"][name]
            self.assertTrue(stream["structure_beyond_lag_8"], name)
            self.assertTrue(stream["near_periodic_lags"], name)
            self.assertGreater(stream["strongest_long_lags"][0]["lag"], 8)
            self.assertLess(stream["min_bits"], 0.05, name)
        self.assertIn("work/extra", result["structure_beyond_lag_8"])

    def test_the_report_finds_nothing_in_independent_jitter(self):
        result = self.runs["jitter"]["report"]
        self.assertEqual(result["structure_beyond_lag_8"], [])
        for name, stream in result["streams"].items():
            self.assertEqual(stream["short_lags_flagged"], [], name)
            self.assertGreater(stream["min_bits"], 0.5, name)

    def test_the_report_refuses_a_damaged_capture(self):
        data = bytearray(self.runs["beat"]["capture"].read_bytes())
        damaged = self.work / "damaged.tfer"
        damaged.write_bytes(bytes(data[:-3]))
        result = subprocess.run([sys.executable, str(REPORT), str(damaged)],
                                capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 1)
        self.assertIn("runs past the end", result.stderr)

    def test_shipping_entropy_code_has_no_capture(self):
        nm = shutil.which("nm")
        if nm is None:
            self.skipTest("nm is required")
        symbols = subprocess.run([nm, str(SHIPPING_OBJECT)], check=True,
                                 capture_output=True, text=True).stdout
        self.assertIn("psp_entropy_fill", symbols)
        self.assertNotIn("capture", symbols)
        driver = subprocess.run([nm, str(DRIVER)], check=True,
                                capture_output=True, text=True).stdout
        self.assertIn("psp_entropy_capture_run", driver)


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("usage: test_entropy_capture_report.py DRIVER SHIPPING_OBJECT",
              file=sys.stderr)
        sys.exit(2)
    SHIPPING_OBJECT = Path(sys.argv.pop(2))
    DRIVER = Path(sys.argv.pop(1))
    unittest.main()
