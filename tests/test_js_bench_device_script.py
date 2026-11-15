"""scripts/js-bench-device.sh stages a device bench only for a trace the
caller names (second argument or JS_BENCH_TRACE); without one it prints its
usage and leaves the build directory untouched."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "scripts" / "js-bench-device.sh"


class JsBenchDeviceScriptTest(unittest.TestCase):
    def setUp(self):
        self._directory = tempfile.TemporaryDirectory(
            prefix="tilefinch-js-bench-device-")
        self.dir = Path(self._directory.name)
        self.build = self.dir / "build"
        self.build.mkdir()
        (self.build / "EBOOT.PBP").write_bytes(b"\0PBP")
        self.trace = self.dir / "trace"
        self.trace.mkdir()
        (self.trace / "trace.meta").write_text("synthetic\n")

    def tearDown(self):
        self._directory.cleanup()

    def run_script(self, *arguments, trace_env=None):
        env = {key: value for key, value in os.environ.items()
               if key != "JS_BENCH_TRACE"}
        if trace_env is not None:
            env["JS_BENCH_TRACE"] = trace_env
        return subprocess.run(["sh", str(SCRIPT), *arguments], env=env,
                              capture_output=True, text=True, timeout=30)

    def test_missing_trace_prints_usage_and_stages_nothing(self):
        result = self.run_script(str(self.build))
        self.assertEqual(result.returncode, 2)
        self.assertIn("usage:", result.stderr)
        self.assertIn("TRACE_DIR", result.stderr)
        self.assertFalse((self.build / "boot.cfg").exists())
        self.assertFalse((self.build / "replay-trace").exists())

    def test_trace_argument_is_staged(self):
        result = self.run_script(str(self.build), str(self.trace), "2")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((self.build / "replay-trace" / "trace.meta").is_file())
        boot = (self.build / "boot.cfg").read_text().splitlines()
        self.assertIn("trace=replay-trace", boot)
        self.assertIn("validation_js_bench=2", boot)

    def test_trace_environment_variable_is_staged(self):
        result = self.run_script(str(self.build), trace_env=str(self.trace))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((self.build / "replay-trace" / "trace.meta").is_file())
        self.assertIn("validation_js_bench=1",
                      (self.build / "boot.cfg").read_text().splitlines())


if __name__ == "__main__":
    unittest.main()
