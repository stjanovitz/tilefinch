#!/usr/bin/env python3
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    "cursor_cadence", ROOT / "tools/check_cursor_cadence.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

LOG = """tilefinch-ui-cadence: phase=controlled-exit cursor-samples=52 cursor-presents=52 cursor-coalesced=0 cursor-average=16000us cursor-max=17000us
tilefinch-cursor-cadence: phase=controlled-exit intervals=50 average=16667us max=17000us segments=2
"""


class CursorCadenceTests(unittest.TestCase):
    def test_normal_clock(self):
        self.assertIn("QUALIFIED", module.check(LOG, 333))

    def test_pressure_is_not_shipping_qualification(self):
        log = LOG.replace("average=16667us", "average=33380us")
        self.assertIn("PRESSURE", module.check(log, 111))
        with self.assertRaises(ValueError):
            module.check(log, 333)

    def test_latency_is_separate_from_cadence(self):
        for field in ("cursor-average=16000us", "cursor-max=17000us"):
            with self.assertRaises(ValueError):
                module.check(LOG.replace(field, field.split("=")[0] + "=40000us"), 333)

    def test_pressure_keeps_correctness_checks(self):
        for old, new in (("cursor-presents=52", "cursor-presents=51"),
                         ("cursor-coalesced=0", "cursor-coalesced=1"),
                         ("intervals=50", "intervals=0"),
                         (" segments=2", "")):
            with self.assertRaises(ValueError):
                module.check(LOG.replace(old, new), 111)

    def test_golden_includes_validation_keyboard_mode(self):
        golden = (ROOT / "tests/input-scripts/cursor-latency.device-golden.txt").read_text()
        self.assertEqual(golden.splitlines()[0],
                         "tilefinch-input-script: text-entry=danzeff")


if __name__ == "__main__":
    unittest.main()
