"""Parsing, delta, and --check-equal behavior of tools/work_vector_report.py."""
import pathlib
import subprocess
import sys
import tempfile
import unittest

REPORT = pathlib.Path(__file__).resolve().parents[1] / "tools/work_vector_report.py"
sys.path.insert(0, str(REPORT.parent))
import work_vector_report  # noqa: E402

RUN_A = """noise before
engine: tilefinch-work: label=loaded js.work_units=100 js.bytecode_ops=n/a style.resolutions=7
tilefinch-input-script: mark=loaded step=2 screen=page
tilefinch-work: label=scrolled js.work_units=150 js.bytecode_ops=n/a style.resolutions=9
tilefinch-work: label=loaded js.work_units=20 js.bytecode_ops=n/a style.resolutions=1
"""
RUN_B = RUN_A.replace("label=scrolled js.work_units=150",
                      "label=scrolled js.work_units=180")


class WorkVectorReportTest(unittest.TestCase):
    def run_tool(self, *texts, flags=()):
        with tempfile.TemporaryDirectory() as directory:
            paths = []
            for index, text in enumerate(texts):
                path = pathlib.Path(directory) / f"run{index}.log"
                path.write_text(text)
                paths.append(str(path))
            return subprocess.run(
                [sys.executable, str(REPORT), *flags, *paths], text=True,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def test_parse_prefixes_repeats_and_text_values(self):
        records = work_vector_report.parse(RUN_A)
        self.assertEqual([label for label, _ in records],
                         ["loaded", "scrolled", "loaded#2"])
        self.assertEqual(records[0][1], {"js.work_units": 100,
                                         "js.bytecode_ops": "n/a",
                                         "style.resolutions": 7})

    def test_single_run_values_and_steps(self):
        result = self.run_tool(RUN_A)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("[scrolled]\n  js.work_units = 150", result.stdout)
        steps = self.run_tool(RUN_A, flags=("--steps", "--fields", "js.work"))
        self.assertIn("js.work_units = 150 (+50)", steps.stdout)
        self.assertIn("counter reset, new document?", steps.stdout)
        self.assertNotIn("style.resolutions", steps.stdout)

    def test_two_runs_print_deltas(self):
        result = self.run_tool(RUN_A, RUN_B)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("js.work_units = 180  +30 (+20.0%)", result.stdout)
        self.assertIn("style.resolutions = 9  0", result.stdout)

    def test_check_equal(self):
        same = self.run_tool(RUN_A, RUN_A, flags=("--check-equal",))
        self.assertEqual(same.returncode, 0, same.stderr)
        self.assertIn("identical: 3 records", same.stdout)
        differ = self.run_tool(RUN_A, RUN_B, flags=("--check-equal",))
        self.assertEqual(differ.returncode, 1)
        self.assertIn("scrolled: js.work_units 150 -> 180", differ.stderr)
        # A missing record or field is a difference too.
        missing = self.run_tool(RUN_A, RUN_A.replace(" style.resolutions=1", ""),
                                flags=("--check-equal",))
        self.assertEqual(missing.returncode, 1)
        self.assertIn("style.resolutions only in one run", missing.stderr)
        # --fields narrows the comparison.
        narrowed = self.run_tool(RUN_A, RUN_B,
                                 flags=("--check-equal", "--fields", "style."))
        self.assertEqual(narrowed.returncode, 0, narrowed.stderr)
        ignored = self.run_tool(RUN_A, RUN_B,
                                flags=("--check-equal", "--ignore", "js.work"))
        self.assertEqual(ignored.returncode, 0, ignored.stderr)

    def test_no_records_is_an_error(self):
        result = self.run_tool("nothing here\n")
        self.assertEqual(result.returncode, 2)


if __name__ == "__main__":
    unittest.main()
