#!/usr/bin/env python3
"""Focused unit checks for experimental native-PC attribution."""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("report", Path(__file__).resolve().parents[1] /
                                            "tools/native_tier_sample_report.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
MAP = """region code-map begin=16 end=24 opcode=get_field phase=helper-call
region code-map begin=24 end=32 opcode=get_field phase=helper-restore
region code-base=0x1000 bytes=32
"""
SAMPLE = """Call graph:
    10 Thread_1
      10 JS_CallInternal  (in probe) [0x2000]
        7 ???  (in <unknown binary>) [0x1018]
        + 4 tf_region_field  (in probe) [0x3000]
        2 ???  (in <unknown binary>) [0x1010]

Total number in stack:
"""


class ReportTests(unittest.TestCase):
    def test_exclusive_not_inclusive(self):
        result = module.report(MAP, SAMPLE)
        self.assertEqual(result["samples"], 10)
        self.assertEqual(result["mapped_emitted_samples"], 5)
        self.assertEqual(result["exclusive"], {"tf_region_field": 4,
            "emitted:helper-restore": 3, "emitted:helper-call": 2, "JS_CallInternal": 1})

    def test_wrong_process_map_refused(self):
        with self.assertRaisesRegex(ValueError, "unmapped"):
            module.report(MAP.replace("0x1000", "0x4000"), SAMPLE)

    def test_overlapping_map_refused(self):
        with self.assertRaisesRegex(ValueError, "overlap"):
            module.report(MAP.replace("begin=24", "begin=20"), SAMPLE)

    def test_unreconciled_tree_refused(self):
        with self.assertRaisesRegex(ValueError, "child samples"):
            module.report(MAP, SAMPLE.replace("+ 4 tf", "+ 8 tf"))


if __name__ == "__main__":
    unittest.main()
