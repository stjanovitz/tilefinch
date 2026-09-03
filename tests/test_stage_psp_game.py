#!/usr/bin/env python3
import os
import pathlib
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "stage-psp-game.sh"


class StagePspGameTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temporary.name)
        self.source = self.root / "source"
        self.stage = self.root / "served"
        self.boot = self.root / "boot.cfg"
        self.source.mkdir()
        self.boot.write_text("trace=none\nurl=https://old.invalid/\n", encoding="utf-8")
        (self.source / "index.html").write_text(
            '<script src="game.js"></script>\n', encoding="utf-8")
        (self.source / "game.js").write_text("const revision = 1;\n", encoding="utf-8")

    def tearDown(self):
        self.temporary.cleanup()

    def stage_once(self, check=True):
        return subprocess.run(
            [str(SCRIPT), "--source", str(self.source),
             "--stage-root", str(self.stage),
             "--base-url", "http://192.0.2.1:8770",
             "--boot-config", str(self.boot), "--name", "game",
             "--query", "qualification=long-soak"],
            check=check, text=True, capture_output=True)

    def current_url(self):
        return next(line[4:] for line in self.boot.read_text(
            encoding="utf-8").splitlines() if line.startswith("url="))

    def test_content_change_gets_fresh_document_and_subresource_path(self):
        first = self.stage_once()
        first_url = self.current_url()
        first_directory = pathlib.Path(first_url.split("/index.html", 1)[0].split(
            ":8770/", 1)[1])
        self.assertIn("stage-digest=", first.stdout)
        self.assertIn("stage=", first_url)
        self.assertIn("file_kb=384", self.boot.read_text(encoding="utf-8"))

        same = self.stage_once()
        self.assertEqual(first_url, self.current_url())
        self.assertEqual(first.stdout, same.stdout)

        (self.source / "game.js").write_text(
            "const revision = 2;\n", encoding="utf-8")
        self.stage_once()
        second_url = self.current_url()
        second_directory = pathlib.Path(second_url.split(
            "/index.html", 1)[0].split(":8770/", 1)[1])
        self.assertNotEqual(first_url, second_url)
        self.assertEqual("const revision = 1;\n", (self.stage / first_directory
            / "game.js").read_text(encoding="utf-8"))
        self.assertEqual("const revision = 2;\n", (self.stage / second_directory
            / "game.js").read_text(encoding="utf-8"))

    def test_existing_stage_is_immutable(self):
        self.stage_once()
        url = self.current_url()
        directory = pathlib.Path(url.split("/index.html", 1)[0].split(
            ":8770/", 1)[1])
        (self.stage / directory / "game.js").write_text(
            "tampered\n", encoding="utf-8")
        failed = self.stage_once(check=False)
        self.assertNotEqual(0, failed.returncode)
        self.assertIn("corrupt immutable game stage", failed.stderr)

    def test_managed_refresh_rehashes_edits_but_respects_a_new_task(self):
        self.stage_once()
        first_url = self.current_url()
        managed = pathlib.Path(str(self.boot) + ".tilefinch-game-stage")
        self.assertTrue(managed.is_file())

        (self.source / "game.js").write_text(
            "const revision = 3;\n", encoding="utf-8")
        refreshed = subprocess.run(
            [str(SCRIPT), "--refresh-managed", str(self.boot)],
            check=True, text=True, capture_output=True)
        second_url = self.current_url()
        self.assertNotEqual(first_url, second_url)
        self.assertIn("stage-digest=", refreshed.stdout)

        self.boot.write_text(
            "url=https://manual.test/another-task\n", encoding="utf-8")
        (self.source / "game.js").write_text(
            "const revision = 4;\n", encoding="utf-8")
        ignored = subprocess.run(
            [str(SCRIPT), "--refresh-managed", str(self.boot)],
            check=True, text=True, capture_output=True)
        self.assertEqual("", ignored.stdout)
        self.assertEqual(
            "url=https://manual.test/another-task\n",
            self.boot.read_text(encoding="utf-8"))

    def test_game_profile_script_limit_replaces_a_stale_override(self):
        self.boot.write_text(
            "file_kb=256\ntrace=none\nurl=https://old.invalid/\n",
            encoding="utf-8")
        self.stage_once()
        lines = self.boot.read_text(encoding="utf-8").splitlines()
        self.assertEqual(1, lines.count("file_kb=384"))
        self.assertNotIn("file_kb=256", lines)

    def test_absolute_symlink_is_refused_without_publishing(self):
        secret = self.root / "secret.js"
        secret.write_text("private\n", encoding="utf-8")
        (self.source / "escape.js").symlink_to(secret)

        failed = self.stage_once(check=False)

        self.assertNotEqual(0, failed.returncode)
        self.assertIn("non-regular entry", failed.stderr)
        self.assertFalse(self.stage.exists())
        self.assertEqual(
            "trace=none\nurl=https://old.invalid/\n",
            self.boot.read_text(encoding="utf-8"))

    def test_parent_relative_symlink_is_refused_without_publishing(self):
        secret = self.root / "secret.js"
        secret.write_text("private\n", encoding="utf-8")
        (self.source / "escape.js").symlink_to("../secret.js")

        failed = self.stage_once(check=False)

        self.assertNotEqual(0, failed.returncode)
        self.assertIn("non-regular entry", failed.stderr)
        self.assertFalse(self.stage.exists())
        self.assertEqual(
            "trace=none\nurl=https://old.invalid/\n",
            self.boot.read_text(encoding="utf-8"))

    def test_nested_digest_marker_symlink_is_refused_without_publishing(self):
        secret = self.root / "secret.txt"
        secret.write_text("private staging-user data\n", encoding="utf-8")
        assets = self.source / "assets"
        assets.mkdir()
        (assets / ".tilefinch-stage-digest").symlink_to(secret)

        failed = self.stage_once(check=False)

        self.assertNotEqual(0, failed.returncode)
        self.assertIn("non-regular entry", failed.stderr)
        self.assertFalse(self.stage.exists())
        self.assertEqual(
            "trace=none\nurl=https://old.invalid/\n",
            self.boot.read_text(encoding="utf-8"))

    def test_root_digest_marker_symlink_cannot_overwrite_target(self):
        victim = self.root / "developer-file.txt"
        original = b"developer-owned contents remain intact\n"
        victim.write_bytes(original)
        (self.source / ".tilefinch-stage-digest").symlink_to(victim)

        failed = self.stage_once(check=False)

        self.assertNotEqual(0, failed.returncode)
        self.assertIn("unsafe root stage marker", failed.stderr)
        self.assertEqual(original, victim.read_bytes())
        self.assertFalse(self.stage.exists())
        self.assertEqual(
            "trace=none\nurl=https://old.invalid/\n",
            self.boot.read_text(encoding="utf-8"))

    def test_newline_filename_is_hashed_without_aliasing(self):
        unusual = self.source / "line\nbreak.txt"
        unusual.write_text("first\n", encoding="utf-8")
        self.stage_once()
        first_url = self.current_url()
        first_directory = pathlib.Path(first_url.split(
            "/index.html", 1)[0].split(":8770/", 1)[1])

        unusual.write_text("second\n", encoding="utf-8")
        self.stage_once()
        second_url = self.current_url()
        second_directory = pathlib.Path(second_url.split(
            "/index.html", 1)[0].split(":8770/", 1)[1])

        self.assertNotEqual(first_directory, second_directory)
        self.assertEqual(
            "second\n", (self.stage / second_directory
                          / "line\nbreak.txt").read_text(encoding="utf-8"))

    def test_hardlinked_resource_is_refused_without_publishing(self):
        outside = self.root / "outside.js"
        outside.write_text("mutable elsewhere\n", encoding="utf-8")
        os.link(outside, self.source / "shared.js")

        failed = self.stage_once(check=False)

        self.assertNotEqual(0, failed.returncode)
        self.assertIn("hardlinked entry", failed.stderr)
        self.assertFalse(self.stage.exists())
        self.assertEqual(
            "trace=none\nurl=https://old.invalid/\n",
            self.boot.read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
