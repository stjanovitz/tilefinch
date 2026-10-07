"""A short benchmark must run the selected work, not silently succeed empty."""
from pathlib import Path
import re
import subprocess
import sys
import unittest

BINARY = Path(sys.argv.pop(1)).resolve()


class BenchSelectionTest(unittest.TestCase):
    def run_bench(self, selection):
        return subprocess.run([str(BINARY), "--scale", "0", "--repeat", "1",
                               "--only", selection], capture_output=True,
                              text=True, timeout=10)

    def test_unknown_and_empty_tokens_refuse_before_any_kernel(self):
        for selection in ("not_a_kernel", "int_loop,typo", ",", "int_loop,",
                          ",int_loop", "int_loop,,call", "x" * 513):
            with self.subTest(selection=selection):
                result = self.run_bench(selection)
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertIn('error="selection"', result.stdout)
                self.assertNotIn("kernel=", result.stdout)

    def test_trace_selection_requires_a_trace(self):
        result = self.run_bench("compile_trace")
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn('error="selection"', result.stdout)

    def test_known_subset_runs_only_requested_kernels(self):
        result = self.run_bench("int_loop,binding_move_ref")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(re.findall(r"kernel=(\S+)", result.stdout),
                         ["int_loop", "binding_move_ref"])
        self.assertIn("end failed=0", result.stdout)

    def test_special_selectors_and_duplicate_names(self):
        result = self.run_bench("poll,int_loop,int_loop")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(re.findall(r"kernel=(\S+)", result.stdout),
                         ["int_loop", "int_loop_polled", "poll"])


if __name__ == "__main__":
    unittest.main()
