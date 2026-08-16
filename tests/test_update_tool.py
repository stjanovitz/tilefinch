#!/usr/bin/env python3

import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

TOOL = Path(__file__).resolve().parents[1] / "tools" / "tilefinch_update_tool.py"
P256_ORDER = int(
    "FFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551", 16
)


class UpdateToolTests(unittest.TestCase):
    def command(self, *arguments):
        subprocess.run(
            [sys.executable, str(TOOL), *map(str, arguments)], check=True,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )

    def key(self, directory, name):
        private = directory / f"{name}.pem"
        public = directory / f"{name}.pub.pem"
        subprocess.run(
            ["openssl", "genpkey", "-algorithm", "EC", "-pkeyopt",
             "ec_paramgen_curve:P-256", "-out", private],
            check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        subprocess.run(
            ["openssl", "pkey", "-in", private, "-pubout", "-out", public],
            check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        return private, public

    def test_offline_package_and_low_s_envelope(self):
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            root_private, root_public = self.key(work, "root")
            release_private, release_public = self.key(work, "release")
            root = work / "root.tfur"
            self.command(
                "root", "--version", 1, "--expires", 2_000_000_000,
                "--root-threshold", 1, "--release-threshold", 1,
                "--root-key", root_public, "--release-key", release_public,
                "--output", root,
            )
            self.assertEqual(root.read_bytes()[:2], b"\0\1")

            slot = work / "slot"
            (slot / "fonts").mkdir(parents=True)
            (slot / "EBOOT.PBP").write_bytes(b"test-eboot")
            (slot / "fonts" / "ui.ttf").write_bytes(b"test-font")
            package = work / "tilefinch.tfup"
            self.command("pack", "--directory", slot, "--output", package)
            self.assertEqual(package.read_bytes()[:8], b"TFUPv1\0\0")

            notes = work / "notes.txt"
            notes.write_text("Security and stability update.", encoding="utf-8")
            manifest = work / "manifest.bin"
            self.command(
                "manifest", "--package", package, "--root-version", 1,
                "--sequence", 2, "--expires", 2_000_000_000,
                "--version", "0.1.1", "--tag", "v0.1.1",
                "--asset", "tilefinch.tfup", "--notes", notes,
                "--output", manifest,
            )
            envelope = work / "tilefinch-update-v1.tfum"
            self.command(
                "envelope", "--manifest", manifest,
                "--release-key", release_private, "--output", envelope,
            )
            encoded = envelope.read_bytes()
            self.assertEqual(encoded[:8], b"TFUMv1\0\0")
            signature = encoded[-64:]
            self.assertGreater(int.from_bytes(signature[:32], "big"), 0)
            self.assertLessEqual(
                int.from_bytes(signature[32:], "big"), P256_ORDER // 2
            )
            self.assertLessEqual(len(encoded), 16 * 1024)
            self.assertLessEqual(len(package.read_bytes()), 32 * 1024 * 1024)

            forbidden = work / "browser-with-model" / "voice-model"
            forbidden.mkdir(parents=True)
            (forbidden / "mdef").write_bytes(b"model")
            with self.assertRaises(subprocess.CalledProcessError):
                self.command(
                    "pack", "--directory", forbidden.parent,
                    "--output", work / "forbidden.tfup",
                )

            component = work / "component"
            (component / "model" / "en-us").mkdir(parents=True)
            (component / "LICENSES").mkdir()
            (component / "model-info.tfv").write_bytes(b"TFVINFO1")
            (component / "model" / "en-us" / "mdef").write_bytes(b"model")
            (component / "LICENSES" / "MODEL.txt").write_bytes(b"notice")
            voice_package = work / "voice.tfvp"
            self.command(
                "pack", "--component", "--directory", component,
                "--output", voice_package,
            )
            self.assertEqual(voice_package.read_bytes()[:8], b"TFVPv1\0\0")
            voice_manifest = work / "voice-manifest.bin"
            self.command(
                "manifest", "--component", "--package", voice_package,
                "--root-version", 1, "--sequence", 1,
                "--expires", 2_000_000_000, "--version", "en-US-1",
                "--tag", "voice-en-us-v1",
                "--asset", "tilefinch-voice-en-us-v1.tfvp",
                "--output", voice_manifest,
            )
            voice_envelope = work / "tilefinch-voice-en-us-v1.tfvm"
            self.command(
                "envelope", "--component", "--manifest", voice_manifest,
                "--release-key", release_private, "--output", voice_envelope,
            )
            self.assertEqual(voice_envelope.read_bytes()[:8], b"TFVMv1\0\0")
            with self.assertRaises(subprocess.CalledProcessError):
                self.command(
                    "envelope", "--component", "--manifest", manifest,
                    "--release-key", release_private,
                    "--output", work / "wrong-kind.tfvm",
                )
            with self.assertRaises(subprocess.CalledProcessError):
                self.command(
                    "envelope", "--manifest", voice_manifest,
                    "--release-key", release_private,
                    "--output", work / "wrong-kind.tfum",
                )

            glyph_package = work / "tilefinch-glyph-ja-v1.tfgf"
            glyph_package.write_bytes(b"TFGFv1\0\0test-glyph-pack")
            glyph_manifest = work / "glyph-manifest.bin"
            self.command(
                "manifest", "--glyph-component",
                "--package", glyph_package, "--root-version", 1,
                "--sequence", 1, "--expires", 2_000_000_000,
                "--version", "ja-1", "--tag", "glyph-ja-v1",
                "--asset", "tilefinch-glyph-ja-v1.tfgf",
                "--output", glyph_manifest,
            )
            self.assertEqual(
                int.from_bytes(glyph_manifest.read_bytes()[26:28], "big"), 3
            )
            glyph_envelope = work / "tilefinch-glyph-ja-v1.tfgm"
            self.command(
                "envelope", "--glyph-component",
                "--manifest", glyph_manifest,
                "--release-key", release_private,
                "--output", glyph_envelope,
            )
            self.assertEqual(glyph_envelope.read_bytes()[:8], b"TFGMv1\0\0")
            with self.assertRaises(subprocess.CalledProcessError):
                self.command(
                    "envelope", "--component", "--manifest", glyph_manifest,
                    "--release-key", release_private,
                    "--output", work / "wrong-kind.tfvm",
                )

            developer_envelope = work / "developer-update.tfum"
            self.command(
                "developer-envelope", "--manifest", manifest,
                "--output", developer_envelope,
            )
            developer = developer_envelope.read_bytes()
            self.assertEqual(developer[:8], b"TFUMv1\0\0")
            self.assertEqual(developer[10], 0)  # no root rotations
            self.assertEqual(developer[-1], 0)  # no signatures
            self.assertEqual(len(developer), 14 + len(manifest.read_bytes()))


if __name__ == "__main__":
    unittest.main()
