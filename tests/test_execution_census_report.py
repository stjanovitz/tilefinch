#!/usr/bin/env python3
"""Synthetic attribution records: no captured page or challenge data."""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location(
    "census", Path(__file__).resolve().parents[1] / "tools/execution_census_report.py")
census = importlib.util.module_from_spec(spec)
spec.loader.exec_module(census)


class CensusReportTests(unittest.TestCase):
    def records(self):
        return "\n".join([
            "tilefinch-execution-census: label=start samples=2 failed=0 gaps-us=0 outside-us=10 native-overflow-us=0 anchor=1000",
            "tilefinch-execution-bin: label=start phase=js zone=get_field cpu-us=10",
            "tilefinch-execution-native: label=start address=1100 cpu-us=0",
            "tilefinch-script-split: label=start at-us=100 js=20 compile=5",
            "tilefinch-execution-census: label=end samples=6 failed=1 gaps-us=30 outside-us=20 native-overflow-us=5 anchor=1000",
            "tilefinch-execution-bin: label=end phase=js zone=get_field cpu-us=50",
            "tilefinch-execution-bin: label=end phase=js zone=native cpu-us=20",
            "tilefinch-execution-bin: label=end phase=compile zone=allocate cpu-us=10",
            "tilefinch-execution-native: label=end address=1100 cpu-us=15",
            "tilefinch-script-split: label=end at-us=300 js=100 compile=25",
            # Observer's later wall-clock mark must not extend the owner window.
            "tilefinch-script-split: label=end at-us=310 js=100 compile=25",
        ])

    def test_exclusive_cpu_and_wall_denominators(self):
        result = census.report(self.records(), "start", "end")
        self.assertEqual(result["elapsed_us"], 200)
        self.assertEqual(result["sampled_cpu_us"], 70)
        self.assertEqual(result["cpu_phases_us"], {"js": 60, "compile": 10})
        self.assertEqual(result["js_groups_us"]["property and element access"], 40)
        self.assertEqual(result["script_wall_us"]["js"], 80)
        self.assertEqual(result["quality"]["samples"], 4)
        self.assertEqual(result["quality"]["native-overflow-us"], 5)
        self.assertEqual(result["native_functions"], [{"function": "0x1100", "cpu_us": 15}])

    def test_reset_and_missing_marks_refused(self):
        with self.assertRaisesRegex(ValueError, "reset"):
            census.report(self.records().replace("samples=6", "samples=1"), "start", "end")
        with self.assertRaisesRegex(ValueError, "missing"):
            census.report(self.records(), "missing", "end")

    def test_distinct_families(self):
        self.assertEqual(census.group("call0"), "calls, returns and closures")
        self.assertEqual(census.group("get_loc0"), "bindings, constants and stack")
        self.assertEqual(census.group("if_false8"), "branches and comparisons")
        self.assertEqual(census.group("release"), "allocation and release")
        self.assertEqual(census.group("add"), "other VM operations")
        self.assertEqual(census.group("get-own-lookup"), "property and element access")
        self.assertEqual(census.group("call-frame-cleanup"), "calls, returns and closures")

    def test_call_frame_bins_are_parts_of_cpu_not_additional_time(self):
        text = self.records() + "\n" + "\n".join([
            "tilefinch-execution-bin: label=end phase=js zone=call-frame-setup cpu-us=5",
            "tilefinch-execution-bin: label=end phase=js zone=call-frame-arguments cpu-us=3",
            "tilefinch-execution-bin: label=end phase=js zone=call-frame-initialize cpu-us=2",
            "tilefinch-execution-bin: label=end phase=js zone=call-frame-cleanup cpu-us=10",
            "tilefinch-execution-path: label=end path=call-frame-setup count=100",
            "tilefinch-execution-path: label=end path=call-frame-simple count=70",
            "tilefinch-execution-job: id=1 begin-us=120 end-us=290 samples=1",
            "tilefinch-execution-job-phase: id=1 phase=js cpu-us=10",
            "tilefinch-execution-job-zone: id=1 zone=call-frame-cleanup cpu-us=10",
        ])
        result = census.report(text, "start", "end")
        self.assertEqual(result["sampled_cpu_us"], 90)
        self.assertEqual(result["cpu_phases_us"]["js"], 80)
        self.assertEqual(result["js_groups_us"]["calls, returns and closures"], 20)
        self.assertEqual(result["path_counts"]["call-frame-simple"], 70)
        self.assertEqual(sum(result["bytecode_frame_cpu_us"].values()), 20)
        self.assertEqual(result["long_jobs"][0]["bytecode_frame_cpu_us"],
                         {"call-frame-cleanup": 10})

    def test_function_job_mix_and_counts(self):
        text = self.records() + "\n" + "\n".join([
            "tilefinch-execution-function: label=start id=1 cpu-us=2 file=sample.js name=run line=3 column=1 bytes=40",
            "tilefinch-execution-function: label=end id=1 cpu-us=32 file=sample.js name=run line=3 column=1 bytes=40",
            "tilefinch-execution-function-zone: label=end function=1 zone=get-own-lookup cpu-us=30",
            "tilefinch-execution-path: label=start path=get-own-lookup count=5",
            "tilefinch-execution-path: label=end path=get-own-lookup count=500",
            "tilefinch-execution-job: id=1 begin-us=120 end-us=290 samples=3",
            "tilefinch-execution-job-phase: id=1 phase=js cpu-us=30",
            "tilefinch-execution-job-function: id=1 function=1 cpu-us=30",
            "tilefinch-execution-job-zone: id=1 zone=get-own-lookup cpu-us=30",
            "tilefinch-execution-job-function-zone: id=1 function=1 zone=get-own-lookup cpu-us=30",
            "tilefinch-execution-job-native: id=1 address=1100 cpu-us=10",
            "tilefinch-execution-function-refs: label=end function=1 retain=100 release=95 final=2",
            "tilefinch-execution-job-refs: id=1 retain=50 release=48 final=1",
        ])
        result = census.report(text, "start", "end")
        self.assertEqual(result["source_functions"][0]["cpu_us"], 30)
        self.assertEqual(result["source_functions"][0]["name"], "run")
        self.assertEqual(result["source_functions"][0]["mix_unassigned_cpu_us"], 0)
        self.assertEqual(result["path_counts"]["get-own-lookup"], 495)
        self.assertEqual(result["long_jobs"][0]["wall_us"], 170)
        self.assertEqual(result["long_jobs"][0]["reference_counts"]["retain"], 50)
        self.assertEqual(result["reference_counts"][0]["count"], 100)
        self.assertEqual(result["js_groups_us"]["property and element access"], 40)
        self.assertEqual(result["long_jobs"][0]["functions"][0]["groups_us"], {"property and element access": 30})
        with self.assertRaisesRegex(ValueError, "identity reused"):
            census.report(text + "\ntilefinch-execution-job: id=1 begin-us=120 end-us=290 samples=3", "start", "end")

    def test_missing_joint_samples_are_not_silently_assigned(self):
        rows = census.function_rows({1: 100}, {(1, "family-property"): 40}, {})
        self.assertEqual(rows[0]["mix_unassigned_cpu_us"], 60)

    def test_native_bodies_stay_bound_to_their_callers_without_double_counting(self):
        text = self.records() + "\n" + "\n".join([
            "tilefinch-execution-function: label=start id=1 cpu-us=5 file=sample.js name=render line=3 column=1 bytes=40",
            "tilefinch-execution-function: label=end id=1 cpu-us=35 file=sample.js name=render line=3 column=1 bytes=40",
            "tilefinch-execution-function-zone: label=start function=1 zone=family-native cpu-us=5",
            "tilefinch-execution-function-zone: label=end function=1 zone=family-native cpu-us=25",
            "tilefinch-execution-function-native: label=start function=1 address=1100 cpu-us=5",
            "tilefinch-execution-function-native: label=end function=1 address=1100 cpu-us=15",
            "tilefinch-execution-function-native: label=end function=1 address=1200 cpu-us=5",
            "tilefinch-execution-function: label=end id=2 cpu-us=10 file=sample.js name=observe line=4 column=1 bytes=20",
            "tilefinch-execution-function-zone: label=end function=2 zone=family-native cpu-us=10",
            "tilefinch-execution-function-native: label=end function=2 address=1100 cpu-us=10",
            "tilefinch-execution-job: id=1 begin-us=120 end-us=290 samples=3",
            "tilefinch-execution-job-phase: id=1 phase=js cpu-us=30",
            "tilefinch-execution-job-function: id=1 function=1 cpu-us=30",
            "tilefinch-execution-job-function-zone: id=1 function=1 zone=family-native cpu-us=20",
            "tilefinch-execution-job-function-native: id=1 function=1 address=1100 cpu-us=10",
        ])
        result = census.report(text, "start", "end")
        self.assertEqual(result["sampled_cpu_us"], 70)
        render, observe = result["source_functions"]
        self.assertEqual(render["native_functions"], [
            {"function": "0x1100", "cpu_us": 10},
            {"function": "0x1200", "cpu_us": 5}])
        self.assertEqual(render["native_unassigned_cpu_us"], 5)
        self.assertEqual(observe["native_functions"], [{"function": "0x1100", "cpu_us": 10}])
        self.assertEqual(observe["native_unassigned_cpu_us"], 0)
        job = result["long_jobs"][0]
        self.assertEqual(job["functions"][0]["native_functions"], [{"function": "0x1100", "cpu_us": 10}])
        self.assertEqual(job["functions"][0]["native_unassigned_cpu_us"], 10)
        # Older logs have no joint native table: show that gap, never infer it.
        old = census.function_rows({1: 20}, {(1, "family-native"): 20}, {})[0]
        self.assertEqual(old["native_functions"], [])
        self.assertEqual(old["native_unassigned_cpu_us"], 20)
        # The family and native joins have independent bounded tables. A lost
        # family row must not hide a native sample retained by the other table.
        lost = census.function_rows({1: 20}, {}, {}, {(1, 0x1100): 10})[0]
        self.assertEqual(lost["native_without_family_cpu_us"], 10)
        self.assertEqual(lost["mix_unassigned_cpu_us"], 20)


if __name__ == "__main__":
    unittest.main()
