#!/usr/bin/env python3
"""Contract tests for the host build's content-keyed verification memo."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class BootstrapVerificationCacheTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="tilefinch-bootstrap-cache-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        for relative in ("src/bootstrap", "src/generated", "cmake", "build"):
            (self.root / relative).mkdir(parents=True)
        for name in ("CheckBootstrapGenerated.cmake", "VerifyBootstrapManifest.cmake"):
            shutil.copyfile(ROOT / "cmake" / name, self.root / "cmake" / name)
        self.source = self.root / "src/bootstrap/a.js"
        (self.root / "src/bootstrap/sources.def").write_text("fixture\n")
        self.generator = self.root / "generator"
        self.generator.write_text(
            "#!/usr/bin/env python3\n"
            "import pathlib, sys\n"
            "root=pathlib.Path(__file__).parent\n"
            "count=root/'calls'\n"
            "count.write_text(str(int(count.read_text())+1) if count.exists() else '1')\n"
            "source=(root/'src/bootstrap/a.js').read_text()\n"
            "ok=(pathlib.Path(sys.argv[3]).read_text()=='embedded:'+source and "
            "pathlib.Path(sys.argv[4]).read_text()=='bytecode:'+source)\n"
            "sys.exit(0 if ok else 1)\n")
        self.generator.chmod(0o755)
        self.publish("original\n")

    def manifest(self):
        paths = sorted((self.root / "src/bootstrap").glob("*.js"))
        paths += [self.root / "src/bootstrap/sources.def",
                  self.root / "src/generated/js_bootstrap.c",
                  self.root / "src/generated/js_bootstrap_bytecode.c"]
        text = "".join(f"{hashlib.sha256(p.read_bytes()).hexdigest()}  "
                       f"{p.relative_to(self.root)}\n" for p in paths)
        (self.root / "src/bootstrap/generated.sha256").write_text(text)

    def publish(self, source):
        self.source.write_text(source)
        (self.root / "src/generated/js_bootstrap.c").write_text("embedded:" + source)
        (self.root / "src/generated/js_bootstrap_bytecode.c").write_text("bytecode:" + source)
        self.manifest()

    def command(self):
        return ["cmake", f"-DTILEFINCH_ROOT={self.root}",
                f"-DTILEFINCH_BOOTSTRAP_MANIFEST={self.root}/src/bootstrap/generated.sha256",
                f"-DTILEFINCH_BOOTSTRAP_GENERATOR={self.generator}",
                f"-DTILEFINCH_BOOTSTRAP_SCRATCH={self.root}/build",
                "-P", str(self.root / "cmake/CheckBootstrapGenerated.cmake")]

    def run_check(self, success=True):
        result = subprocess.run(self.command(), capture_output=True, text=True)
        self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)
        return result.stdout

    def calls(self):
        return int((self.root / "calls").read_text())

    def test_reuses_success_but_checks_contents_not_timestamps(self):
        self.assertIn("Recorded content-verified", self.run_check())
        self.assertIn("Reusing content-verified", self.run_check())
        self.assertEqual(self.calls(), 1)
        original = self.source.stat()
        self.source.write_text("tampered\n")  # Same length, preserved mtime.
        os.utime(self.source, ns=(original.st_atime_ns, original.st_mtime_ns))
        self.run_check(False)
        self.assertEqual(self.calls(), 1)
        self.source.write_text("original\n")
        self.run_check()
        self.assertEqual(self.calls(), 1)
        self.publish("modified\n")
        self.run_check()
        self.assertEqual(self.calls(), 2)

    def test_changed_artifact_and_manifest_still_require_real_comparison(self):
        self.run_check()
        artifact = self.root / "src/generated/js_bootstrap_bytecode.c"
        artifact.write_text("incorrect bytecode\n")
        self.manifest()
        self.run_check(False)
        self.run_check(False)
        self.assertEqual(self.calls(), 3)  # Failure never becomes a cached success.

    def test_generator_and_verification_implementation_invalidate_memo(self):
        self.run_check()
        for path in (self.generator, self.root / "cmake/CheckBootstrapGenerated.cmake",
                     self.root / "cmake/VerifyBootstrapManifest.cmake"):
            with path.open("a") as stream:
                stream.write("\n# Changed implementation identity.\n")
            self.assertIn("Recorded content-verified", self.run_check())
        self.assertEqual(self.calls(), 4)

    def test_new_unlisted_source_is_not_hidden_by_cached_success(self):
        self.run_check()
        (self.root / "src/bootstrap/new.js").write_text("new\n")
        self.run_check(False)
        self.assertEqual(self.calls(), 1)

    def test_concurrent_checks_publish_one_success(self):
        processes = [subprocess.Popen(self.command(), stdout=subprocess.PIPE,
                                      stderr=subprocess.PIPE, text=True) for _ in range(2)]
        results = [process.communicate(timeout=30) for process in processes]
        for process, (stdout, stderr) in zip(processes, results):
            self.assertEqual(process.returncode, 0, stdout + stderr)
        self.assertEqual(self.calls(), 1)


if __name__ == "__main__":
    unittest.main()
