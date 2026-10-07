"""Exercise the local-only, authenticated app-package fixture writer."""
import pathlib
import subprocess
import sys
import tempfile
import unittest

TOOL = sys.argv.pop(1)


class OfflineAppFixture(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temp.name)
        self.source = self.root / "source"
        self.source.mkdir()
        (self.source / "index.html").write_text(
            '<!doctype html><title>Fixture</title>'
            '<link rel="stylesheet" href="game.css">'
            '<script defer src="game.js"></script><button>Play</button>'
        )
        (self.source / "manifest.webmanifest").write_text(
            '{"name":"Fixture","start_url":"./index.html",'
            '"display":"fullscreen"}'
        )
        (self.source / "game.css").write_text("body{color:blue}")
        (self.source / "game.js").write_text("globalThis.fixtureReady=true;")
        self.output = self.root / "offline"

    def tearDown(self):
        self.temp.cleanup()

    def run_tool(self, *resources):
        return subprocess.run(
            [TOOL, "--web-app", str(self.output),
             "https://fixture.test/index.html?qualification=input",
             str(self.source), *resources],
            capture_output=True, text=True, timeout=10
        )

    def test_roundtrip_and_no_overwrite(self):
        result = self.run_tool("game.css", "game.js")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("offline/app?id=1", result.stdout)
        files = {p.name: p.read_bytes() for p in self.output.iterdir()}
        self.assertIn("library.bin", files)
        self.assertIn("00000001.app.pack", files)
        self.assertIn(b"fixtureReady", files["00000001.app.pack"])
        self.assertNotEqual(self.run_tool("game.css").returncode, 0)
        self.assertEqual(files, {p.name: p.read_bytes() for p in self.output.iterdir()})

    def test_append_adds_a_second_saved_row(self):
        self.assertNotEqual(subprocess.run(
            [TOOL, "--web-app-append", str(self.output),
             "https://fixture.test/index.html", str(self.source), "game.js"],
            capture_output=True, text=True, timeout=10).returncode, 0)
        self.assertEqual(self.run_tool("game.css", "game.js").returncode, 0)
        before = {p.name: p.read_bytes() for p in self.output.iterdir()}
        second = subprocess.run(
            [TOOL, "--web-app-append", str(self.output),
             "https://other.test/index.html", str(self.source), "game.js"],
            capture_output=True, text=True, timeout=10)
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertIn("offline/app?id=2", second.stdout)
        after = {p.name: p.read_bytes() for p in self.output.iterdir()}
        self.assertIn("00000002.app.pack", after)
        for name in ("00000001.app.pack", "00000001.app.html"):
            self.assertEqual(before[name], after[name])

    def test_symlink_and_parent_inventory_refused(self):
        (self.source / "linked.js").symlink_to(self.source / "game.js")
        self.assertNotEqual(self.run_tool("linked.js").returncode, 0)
        self.assertFalse((self.output / "library.bin").exists())
        self.output.rmdir()
        self.assertNotEqual(self.run_tool("../source/game.js").returncode, 0)
        self.assertFalse((self.output / "library.bin").exists())


if __name__ == "__main__":
    unittest.main()
