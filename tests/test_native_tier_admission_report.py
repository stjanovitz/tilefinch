#!/usr/bin/env python3
"""Fail closed on incomplete native-tier census accounting."""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("report", Path(__file__).resolve().parents[1] /
                                            "tools/native_tier_admission_report.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
ADMISSION = """region admission-limit bytecode=100 plan-bytes=20 code-limit=4096
region admission opcode=1 name=get_loc0 kind=value
region admission opcode=2 name=get_field kind=property
region admission opcode=3 name=add kind=unsupported
"""
CENSUS = """tf-tier-total functions=2 overflow=0
tf-tier slot=0 bytes=99 static=5 vars=1 args=0 stack=2 counts=1:20,2:30,3:50,
tf-tier slot=1 bytes=101 static=7 vars=2 args=0 stack=2 counts=1:100,
"""


class AdmissionTests(unittest.TestCase):
    def test_partition_and_ranking(self):
        result = module.report(ADMISSION, CENSUS)
        self.assertEqual(result["instructions"], 200)
        self.assertEqual(result["supported_before_guards"], 50)
        self.assertEqual(result["supported_percent"], 25)
        self.assertEqual(result["oversized_body_instructions"], 100)
        self.assertEqual(result["unsupported_opcodes"], {"add": 50})
        self.assertEqual(result["top_body_coverage"]["1"], 50)

    def test_incomplete_and_overflow_refused(self):
        for text in (CENSUS.replace("functions=2", "functions=3"),
                     CENSUS.replace("overflow=0", "overflow=1"),
                     CENSUS.replace("3:50,", "3:50,garbage")):
            with self.assertRaises(ValueError):
                module.report(ADMISSION, text)

    def test_duplicate_and_unknown_opcode_refused(self):
        for text in (CENSUS.replace("3:50,", "3:50,3:1,"),
                     CENSUS.replace("3:50,", "4:50,"),
                     CENSUS.replace("slot=1", "slot=0")):
            with self.assertRaises(ValueError):
                module.report(ADMISSION, text)

    def test_duplicate_map_refused(self):
        with self.assertRaises(ValueError):
            module.report(ADMISSION + "region admission opcode=3 name=other kind=value\n", CENSUS)

    def test_region_histogram_reconciles(self):
        text = "\n".join(f"tf-current-region model={m} total=20 admitted=10 runs=4 overflow=0 max-frames=2 reasons=unsupported:10,admitted:10, histogram=1:2,4:2," for m in (0, 1))
        result = module.region_report(text)["1"]
        self.assertEqual(result["mean_length"], 2.5)
        self.assertEqual(result["length_percentiles"], {"50": 1, "95": 4, "99": 4})
        self.assertEqual(result["instruction_share_in_runs_at_least"]["8"], 0)
        for bad in (text.replace("overflow=0", "overflow=1"),
                    text.replace("1:2,4:2,", "1:1,4:2,"),
                    text.replace("unsupported:10", "unsupported:11"),
                    text.replace("1:2,4:2,", "1:1,1:1,4:2,"),
                    text.splitlines()[0]):
            with self.assertRaises(ValueError):
                module.region_report(bad)

    def test_emitted_footprints_include_page_and_plan_cost(self):
        text = """tf-native-size-summary bodies=3 overflow=0 page=4096 plan=200
tf-native-size slot=0 bytes=10 code=100 accepted=1 entries=5 total=80 guarded=40
tf-native-size slot=1 bytes=20 code=4100 accepted=1 entries=8 total=30 guarded=20
tf-native-size slot=2 bytes=400 code=0 accepted=0 entries=0 total=10 guarded=0
"""
        region = {"instructions": 120, "admitted": 60}
        result = module.footprint_report(text, region, 100, 8192)
        self.assertEqual(result["emitted_code_bytes"], 4200)
        self.assertEqual(result["separate_page_bytes"], 12288)
        self.assertEqual(result["ideal_packed_code_bytes_lower_bound"], 8192)
        self.assertEqual(result["all_plan_bytes"], 400)
        self.assertEqual(result["ranked_selections"]["1"]["guarded"], 40)
        self.assertEqual(result["ranked_selections"]["4"]["bodies"], 2)
        self.assertEqual(result["ranked_selections"]["4"]["instruction_percent"], 50)
        for bad in (text.replace("overflow=0", "overflow=1"),
                    text.replace("page=4096", "page=4095"),
                    text.replace("slot=1", "slot=0"),
                    text.replace("code=100", "code=101"),
                    text.replace("code=0", "code=4"),
                    text.replace("guarded=40", "guarded=41"),
                    text.replace("total=80", "total=81"),
                    text.replace("entries=5", "entries=11"),
                    text.replace("bytes=20", "bytes=101"),
                    text.replace("bodies=3", "bodies=4")):
            with self.assertRaises(ValueError):
                module.footprint_report(bad, region, 100, 8192)


if __name__ == "__main__":
    unittest.main()
