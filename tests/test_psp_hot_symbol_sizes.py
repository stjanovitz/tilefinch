"""Keep the real PSP hot-function gate authoritative across GCC clones."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest


class HotSymbolTests(unittest.TestCase):
    def run_gate(self, rows, limit):
        cmake = shutil.which("cmake")
        self.assertIsNotNone(cmake)
        root = pathlib.Path(__file__).resolve().parents[1]
        source = (root / "cmake/CheckPspHotSymbolSizes.cmake").read_text()
        start = source.index("function(hot_symbol_size symbol out)")
        end = source.index("# The code one kind of frame", start)
        with tempfile.TemporaryDirectory(prefix="tf-hot-symbol-") as directory:
            script = pathlib.Path(directory) / "gate.cmake"
            script.write_text(
                "set(nm_output [=[\n" + rows + "\n]=])\n"
                "set(PSP_ELF test-image)\n" + source[start:end]
                + f"\ncheck_hot_symbol(sample {limit})\n"
            )
            return subprocess.run(
                [cmake, "-P", str(script)], capture_output=True, text=True,
                timeout=10,
            )

    def test_base_symbol(self):
        result = self.run_gate("00000000 00000040 t sample", 64)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("64/64", result.stdout)

    def test_clone_without_base(self):
        result = self.run_gate("00000000 00000040 t sample.constprop.0", 64)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_all_variants_are_charged(self):
        rows = "00000000 00000040 t sample\n00000040 00000020 t sample.isra.2"
        self.assertEqual(self.run_gate(rows, 96).returncode, 0)
        result = self.run_gate(rows, 95)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("grew to 96 bytes", result.stderr)

    def test_missing_or_prefix_collision_is_refused(self):
        result = self.run_gate("00000000 00000040 t sample_other", 64)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("could not find sample", result.stderr)


if __name__ == "__main__":
    unittest.main()
