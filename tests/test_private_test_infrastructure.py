#!/usr/bin/env python3
"""Check optional private suites using an isolated, synthetic CMake project."""

import json
import contextlib
import io
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import test_public_tree_hygiene as hygiene


ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which("cmake") and shutil.which("ctest"),
                     "CMake and CTest are required")
class PrivateTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="tf-private-tests-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "source"
        self.build = self.root / "build"
        self.source.mkdir()
        module = (ROOT / "cmake/TilefinchPrivateTests.cmake").as_posix()
        (self.source / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.20)\n'
            'project(PrivateTestInfrastructure C)\n'
            'enable_testing()\n'
            'function(tilefinch_add_test_binary target)\n'
            '  add_executable(${target} ${ARGN})\n'
            '  set_property(GLOBAL APPEND PROPERTY TILEFINCH_TEST_BINARIES ${target})\n'
            'endfunction()\n'
            f'include("{module}")\n'
            'get_property(binaries GLOBAL PROPERTY TILEFINCH_TEST_BINARIES)\n'
            'add_custom_target(tilefinch-test-binaries DEPENDS ${binaries})\n',
            encoding="utf-8")

    def configure(self, *arguments, success=True):
        result = subprocess.run(
            ["cmake", "-S", str(self.source), "-B", str(self.build), *arguments],
            capture_output=True, text=True)
        if success:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0)
        return result.stdout + result.stderr

    def install_suite(self):
        suite = self.source / ".private-investigations/tests"
        suite.mkdir(parents=True)
        (suite / "probe.c").write_text("int main(void) { return 0; }\n",
                                       encoding="utf-8")
        (suite / "CMakeLists.txt").write_text(
            'tilefinch_add_test_binary(private-probe probe.c)\n'
            'tilefinch_add_private_test(private-probe COMMAND private-probe '
            'LABELS unit TIMEOUT 7)\n', encoding="utf-8")
        return suite

    def registered_tests(self):
        result = subprocess.run(
            ["ctest", "--test-dir", str(self.build), "--show-only=json-v1"],
            capture_output=True, text=True, check=True)
        return json.loads(result.stdout)["tests"]

    def test_public_clone_needs_no_private_directory(self):
        self.configure()
        self.assertEqual(self.registered_tests(), [])

    def test_explicit_enable_requires_sources(self):
        output = self.configure("-DTILEFINCH_ENABLE_PRIVATE_TESTS=ON", success=False)
        self.assertIn(".private-investigations/tests", output)

    def test_enable_build_register_and_disable(self):
        self.install_suite()
        self.configure()
        self.assertEqual(self.registered_tests(), [])  # Local files alone never opt in.
        self.configure("-DTILEFINCH_ENABLE_PRIVATE_TESTS=ON")
        subprocess.run(["cmake", "--build", str(self.build), "--target",
                        "tilefinch-test-binaries"], check=True, capture_output=True)
        tests = self.registered_tests()
        self.assertEqual([test["name"] for test in tests], ["private-probe"])
        properties = {item["name"]: item["value"]
                      for item in tests[0]["properties"]}
        self.assertIn("private", properties["LABELS"])
        self.assertEqual(properties["TIMEOUT"], 7)
        subprocess.run(["ctest", "--test-dir", str(self.build), "-L", "private",
                        "--output-on-failure"], check=True, capture_output=True)
        self.configure("-DTILEFINCH_ENABLE_PRIVATE_TESTS=OFF")
        self.assertEqual(self.registered_tests(), [])

    def test_external_directory_and_device_refusal(self):
        suite = self.install_suite()
        external = self.root / "external-tests"
        suite.rename(external)
        self.configure("-DTILEFINCH_ENABLE_PRIVATE_TESTS=ON",
                       f"-DTILEFINCH_PRIVATE_TESTS_DIR={external}")
        self.assertEqual(len(self.registered_tests()), 1)
        output = self.configure("-DPSP=ON", success=False)
        self.assertIn("host-only", output)

    def test_private_failure_fails_ctest(self):
        suite = self.install_suite()
        (suite / "probe.c").write_text("int main(void) { return 1; }\n",
                                       encoding="utf-8")
        self.configure("-DTILEFINCH_ENABLE_PRIVATE_TESTS=ON")
        subprocess.run(["cmake", "--build", str(self.build), "--target",
                        "tilefinch-test-binaries"], check=True, capture_output=True)
        result = subprocess.run(
            ["ctest", "--test-dir", str(self.build), "-L", "private",
             "--output-on-failure"], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("private-probe", result.stdout)


class PublicTreeHygiene(unittest.TestCase):
    def test_private_files_may_exist_locally_but_must_not_be_tracked(self):
        with tempfile.TemporaryDirectory(prefix="tf-private-hygiene-") as directory:
            root = Path(directory)
            (root / ".gitignore").write_text(
                "\n".join(sorted(hygiene.IGNORE_RULES)) + "\n", encoding="utf-8")
            private = root / ".private-investigations/tests/synthetic.txt"
            private.parent.mkdir(parents=True)
            private.write_text("synthetic fixture\n", encoding="utf-8")
            with patch.object(sys, "argv", ["hygiene", str(root)]), \
                    contextlib.redirect_stdout(io.StringIO()), \
                    contextlib.redirect_stderr(io.StringIO()):
                with patch.object(hygiene, "tracked_paths", return_value=[]):
                    self.assertEqual(hygiene.main(), 0)
                with patch.object(hygiene, "tracked_paths", return_value=[
                        ".private-investigations/tests/synthetic.txt"]):
                    self.assertEqual(hygiene.main(), 1)


if __name__ == "__main__":
    unittest.main()
