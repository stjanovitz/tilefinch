#!/usr/bin/env python3
"""Exercise the allocator experiment's disposable builds without compiling."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


TOOL = Path(sys.argv.pop(1)) if len(sys.argv) > 1 else (
    Path(__file__).resolve().parents[1] / "scripts/benchmark_allocator_pgo.sh")

STUB = r'''import json
import os
from pathlib import Path
import sys

name = Path(sys.argv[0]).name
args = sys.argv[1:]
with open(os.environ["CALL_LOG"], "a") as log:
    log.write(json.dumps([name, args]) + "\n")
if name == "uname":
    print("Darwin")
elif name == "cmake":
    build = Path(args[args.index("--build" if "--build" in args else "-B") + 1])
    # Reject old in-checkout build defaults without touching the checkout.
    build.resolve().relative_to(Path(os.environ["TMPDIR"]).resolve())
    if "--build" not in args:
        build.mkdir(parents=True, exist_ok=True)
    elif os.environ.get("FAIL_BUILD"):
        sys.exit(37)
    else:
        for target, output in (
            ("psp-browser-quickjs-allocator-bench", "median-ms=10"),
            ("psp-browser-jsc-spike", "elapsed-ms=10"),
        ):
            program = build / target
            program.write_text("#!/bin/sh\nprintf '%s\\n' '" + output + "'\n")
            program.chmod(0o700)
elif name == "xcrun":
    output = next(arg.split("=", 1)[1] for arg in args
                  if arg.startswith("-output="))
    Path(output).write_bytes(b"test profile")
'''


class AllocatorPgoToolTests(unittest.TestCase):
    def run_tool(self, fail=False):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            binaries = root / "bin"
            binaries.mkdir()
            for name in ("uname", "cmake", "xcrun"):
                program = binaries / name
                program.write_text("#!" + sys.executable + "\n" + STUB)
                program.chmod(0o700)
            scratch = root / "scratch"
            scratch.mkdir()
            log = root / "calls.jsonl"
            env = dict(os.environ, TMPDIR=str(scratch), CALL_LOG=str(log),
                       PATH=str(binaries) + os.pathsep + os.environ["PATH"])
            if fail:
                env["FAIL_BUILD"] = "1"
            else:
                env.pop("FAIL_BUILD", None)
            result = subprocess.run(
                ["bash", str(TOOL), "-DTEST_FORWARDED=ON"],
                env=env, text=True, capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 37 if fail else 0, result.stderr)
            self.assertEqual(list(scratch.glob("tilefinch-allocator-pgo.*")), [],
                             "experiment leaked builds or profiles")
            calls = [json.loads(line) for line in log.read_text().splitlines()]
            configurations = [args for name, args in calls
                              if name == "cmake" and "-B" in args]
            self.assertEqual(len(configurations), 1 if fail else 2)
            for args in configurations:
                self.assertIn("-DTEST_FORWARDED=ON", args)
                Path(args[args.index("-B") + 1]).relative_to(scratch)
            if not fail:
                self.assertIn("ratio=1.000x", result.stdout)
                builds = [Path(args[args.index("-B") + 1])
                          for args in configurations]
                self.assertNotEqual(builds[0], builds[1])
                self.assertEqual(builds[0].parent, builds[1].parent)

    def test_success_removes_builds_and_profiles(self):
        self.run_tool()

    def test_build_failure_removes_builds_and_profiles(self):
        self.run_tool(fail=True)


if __name__ == "__main__":
    unittest.main()
