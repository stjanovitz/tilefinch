#!/usr/bin/env python3
"""The recorded update route on the host: trace maker, replay, signed client
and installer together (scripts/make-update-replay.py,
tests/update_replay_probe.c).

Throwaway P-256 keys sign a fake browser slot; the trace maker writes the
keyed trace a PSP validation build replays over host0:; the probe replays it
through the shipping client and installer into a temporary A/B install.
This is the download-and-install half of the device route without a PSP.
Skips (77) when openssl is unavailable.
"""

from __future__ import annotations

import hashlib
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools" / "tilefinch_update_tool.py"
MAKER = ROOT / "scripts" / "make-update-replay.py"
URL = "https://updates.tilefinch.test/route/tilefinch-update-v1.tfum"
PROBE = None

SLOT_FILES = {
    "EBOOT.PBP": b"\0PBP\0\0\1\0" + bytes(range(256)) * 300,
    "boot-defaults.cfg": b"url=tilefinch://home\n",
    "roots.pem": b"-----BEGIN CERTIFICATE-----\nfixture\n",
}


def run(*arguments: str | Path, check: bool = True):
    return subprocess.run([str(item) for item in arguments], check=check,
                          capture_output=True, text=True)


class UpdateReplayRouteTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.temporary = tempfile.TemporaryDirectory(
            prefix="tilefinch-update-route-")
        work = Path(cls.temporary.name)
        cls.work = work
        for name in ("root", "release"):
            run("openssl", "genpkey", "-algorithm", "EC", "-pkeyopt",
                "ec_paramgen_curve:P-256", "-out", work / f"{name}.pem")
            run("openssl", "pkey", "-in", work / f"{name}.pem", "-pubout",
                "-out", work / f"{name}-public.pem")
        run(sys.executable, TOOL, "root", "--version", "1", "--expires",
            "2000000000", "--root-threshold", "1", "--release-threshold", "1",
            "--root-key", work / "root-public.pem", "--release-key",
            work / "release-public.pem", "--output", work / "root-v1.tfur")
        slot = work / "candidate-slot"
        slot.mkdir()
        for name, body in SLOT_FILES.items():
            (slot / name).write_bytes(body)
        cls.package = work / "tilefinch-update-v1.tfup"
        run(sys.executable, TOOL, "pack", "--directory", slot, "--output",
            cls.package)
        (work / "notes.txt").write_text("Recorded route fixture.",
                                        encoding="ascii")
        cls.metadata = work / "tilefinch-update-v1.tfum"
        run(sys.executable, TOOL, "manifest", "--package", cls.package,
            "--root-version", "1", "--sequence", "49", "--expires",
            "2000000000", "--version", "0.1.48-route", "--tag",
            "v0.1.48-route", "--asset", "tilefinch-update-v1.tfup",
            "--notes", work / "notes.txt", "--decoder-abi", "5", "--output",
            work / "manifest.bin")
        run(sys.executable, TOOL, "envelope", "--manifest",
            work / "manifest.bin", "--release-key", work / "release.pem",
            "--output", cls.metadata)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.temporary.cleanup()

    def make_trace(self, name: str, package: Path | None = None,
                   check: bool = True):
        return run(sys.executable, MAKER, "--metadata", self.metadata,
                   "--package", package or self.package, "--out",
                   self.work / name, "--metadata-url", URL,
                   "--install-root", "ms0:/PSP/GAME/TFUPTEST",
                   "--running-slot", "a", check=check)

    def make_install(self, name: str) -> Path:
        install = self.work / name
        (install / "slot-a").mkdir(parents=True)
        (install / "data").mkdir()
        (install / "slot-a" / "EBOOT.PBP").write_bytes(b"\0PBP\0\0\1\0old")
        return install

    def probe(self, trace: str, install: Path):
        return run(PROBE, self.work / trace, self.work / "root-v1.tfur", URL,
                   install, "a", "48", check=False)

    def test_trace_installs_the_signed_candidate_as_a_pending_trial(self):
        made = self.make_trace("trace-ok")
        self.assertIn("sequence 49", made.stdout)
        cfg = (self.work / "trace-ok" / "update-route.cfg").read_text(
            encoding="utf-8")
        self.assertEqual(cfg, "install_root=ms0:/PSP/GAME/TFUPTEST\n"
                              "running_slot=a\n")
        install = self.make_install("install-ok")
        result = self.probe("trace-ok", install)
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        self.assertIn("client signature-verified sequence=49", result.stdout)
        self.assertIn("journal after generation=", result.stdout)
        self.assertIn("pending=2 previous=0 trial=1 installed=48 "
                      "candidate=49", result.stdout)
        for name, body in SLOT_FILES.items():
            self.assertEqual((install / "slot-b" / name).read_bytes(), body)
        # The running slot is never touched.
        self.assertEqual((install / "slot-a" / "EBOOT.PBP").read_bytes(),
                         b"\0PBP\0\0\1\0old")

    def test_a_tampered_package_record_fails_the_signed_hash(self):
        self.make_trace("trace-tampered")
        body = self.work / "trace-tampered" / "0001.body"
        data = bytearray(body.read_bytes())
        data[-1] ^= 0x01
        body.write_bytes(bytes(data))
        install = self.make_install("install-tampered")
        result = self.probe("trace-tampered", install)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((install / "slot-b").exists())

    def test_the_maker_refuses_a_package_the_metadata_does_not_sign(self):
        other = self.work / "other.tfup"
        data = bytearray(self.package.read_bytes())
        data[-1] ^= 0x01
        other.write_bytes(bytes(data))
        result = self.make_trace("trace-mismatch", package=other, check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("does not name this package", result.stderr)
        self.assertFalse((self.work / "trace-mismatch").exists())

    def test_the_maker_refuses_the_main_install(self):
        result = run(sys.executable, MAKER, "--metadata", self.metadata,
                     "--package", self.package, "--out",
                     self.work / "trace-main", "--install-root",
                     "ms0:/PSP/GAME/TILEFINCH", "--running-slot", "a",
                     check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.work / "trace-main").exists())

    def test_a_journal_only_trace_says_so(self):
        result = run(sys.executable, MAKER, "--metadata", self.metadata,
                     "--package", self.package, "--out",
                     self.work / "trace-journal", "--metadata-url", URL,
                     "--install-root", "ms0:/PSP/GAME/TFUPTEST",
                     "--running-slot", "b", "--journal-only")
        self.assertIn("reports only on ms0:/PSP/GAME/TFUPTEST", result.stdout)
        cfg = (self.work / "trace-journal" / "update-route.cfg").read_text(
            encoding="utf-8")
        self.assertEqual(cfg, "install_root=ms0:/PSP/GAME/TFUPTEST\n"
                              "running_slot=b\njournal_only=1\n")

    def test_the_trace_digest_matches_the_signed_package(self):
        self.make_trace("trace-digest")
        body = (self.work / "trace-digest" / "0001.body").read_bytes()
        self.assertEqual(hashlib.sha256(body).digest(),
                         hashlib.sha256(self.package.read_bytes()).digest())
        meta = (self.work / "trace-digest" / "0001.meta").read_text(
            encoding="utf-8")
        self.assertIn("url=https://updates.tilefinch.test/route/"
                      "tilefinch-update-v1.tfup\n", meta)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("usage: test_update_replay_route.py PROBE", file=sys.stderr)
        sys.exit(2)
    PROBE = Path(sys.argv.pop(1))
    if shutil.which("openssl") is None:
        print("openssl unavailable; skipping", file=sys.stderr)
        sys.exit(77)
    unittest.main()
