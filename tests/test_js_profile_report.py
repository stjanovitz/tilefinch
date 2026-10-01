"""Precision, prefix, and non-overlapping accounting regressions for the report."""
import pathlib
import subprocess
import sys
import tempfile
import unittest

REPORT = pathlib.Path(__file__).resolve().parents[1] / "tools/js_profile_report.py"


class ProfileReportTest(unittest.TestCase):
    def report(self, text):
        with tempfile.TemporaryDirectory() as directory:
            log = pathlib.Path(directory) / "profile.txt"
            log.write_text(text)
            return subprocess.check_output(
                [sys.executable, str(REPORT), "send", str(log)], text=True)

    def test_precision_and_overlapping_tables(self):
        prefix = "engine: tilefinch-js-profile: label=send "
        rows = [prefix + "samples=4 total-ms=2"]
        for kind, micros in (("self", 375), ("inclusive", 1900),
                             ("micro-self", 250), ("micro-inclusive", 1500)):
            rows.append(prefix + f"{kind} rank=0 ms={micros // 1000} samples=1 "
                        f"at=<browser-traversal-lazy>:walk:20:1 fn=18:1 us={micros}")
        rows.append(prefix + "self rank=1 ms=1 samples=1 at=page.js:f:2:1 fn=1:1 us=1125")
        output = self.report("\n".join(rows))
        self.assertIn("printed self buckets: 1.500 ms (75.0% coverage)", output)
        self.assertIn("bootstrap self: 0.375 ms; other JS self: 1.125 ms", output)
        self.assertIn("traversal.js:walk:18:1 | 1.900 | 0.375 | 1.500", output)

    def test_legacy_milliseconds_and_label_filter(self):
        output = self.report("tilefinch-js-profile: label=send samples=1 total-ms=3\n"
            "tilefinch-js-profile: label=send self rank=0 ms=2 samples=1 "
            "at=page.js:f:2:1 fn=1:1\n"
            "tilefinch-js-profile: label=load self rank=0 ms=99 samples=1 "
            "at=page.js:f:2:1 fn=1:1\n")
        self.assertIn("printed self buckets: 2.000 ms (66.7% coverage)", output)

    def test_unframed_samples_are_not_assigned_to_bootstrap(self):
        output = self.report("tilefinch-js-profile: label=send samples=2 total-ms=2 "
            "total-us=2500 unframed-us=1000\n"
            "tilefinch-js-profile: label=send self rank=0 ms=1 samples=1 "
            "at=page.js:f:2:1 fn=1:1 us=1500\n")
        self.assertIn("Sampled runtime intervals: 2.500 ms", output)
        self.assertIn("bootstrap self: 0.000 ms; other JS self: 1.500 ms", output)
        self.assertIn("Samples without a JS frame: 1.000 ms", output)

    def test_unframed_breakdown_keeps_profiler_cost_separate(self):
        output = self.report("tilefinch-js-profile: label=send samples=2 total-ms=3\n"
            "tilefinch-js-profile: label=send unframed pending-us=1000 "
            "compile-us=500 checkpoint-us=200 entry-us=100 tail-us=1200 "
            "profiler-us=9000\n")
        self.assertIn("Sampled runtime intervals: 3.000 ms", output)
        self.assertIn("Explicit compilation without a JS stack: 0.500 ms", output)
        self.assertIn("Script/job return tails: 1.200 ms", output)
        self.assertIn("Sampling overhead (separate from runtime intervals): 9.000 ms", output)


if __name__ == "__main__":
    unittest.main()
