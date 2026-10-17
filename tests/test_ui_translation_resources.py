"""Keep downloaded catalogs out of the source tree while testing exact bytes."""
import hashlib
import re
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import generate_ui_translations as generator


class ResourceTests(unittest.TestCase):
    def test_new_locale_preserves_existing_resource_identity(self):
        manifest = generator.outputs()[ROOT / "src/generated/ui_languages.inc"].decode()
        resources = ROOT / "_resource_identity_probe"
        historical = hashlib.sha256()
        for path, data in sorted(generator.outputs(resources).items()):
            if path.is_relative_to(resources) and int(path.parent.name[1:]) <= 10:
                historical.update(path.relative_to(resources).as_posix().encode() + b"\0" + data)
        # Fixed before v11: every previously published resource remains exact.
        self.assertEqual(historical.hexdigest(),
            "43644306dca44836cd686c6556e914169eea13723bcdec28d755e3d33f503b7f")
        old_languages = generator.CATALOGS[6][1]
        rows = generator.read_rows(7, old_languages)
        for column, (code, label, glyphs) in enumerate(old_languages, 1):
            self.assertIn(f'"translations/ui/v11/{code}.tful"', manifest)
        self.assertIn('"translations/ui/v11/pt.tful"', manifest)
        portuguese = generator.read_rows(8, generator.CATALOGS[7][1])
        self.assertGreaterEqual({row[0] for row in portuguese}, {row[0] for row in rows})
        self.assertEqual(dict(portuguese)["Portuguese"], "Português")
        self.assertIn('"translations/ui/v11/bn.tful"', manifest)
        self.assertIn('"translations/ui/v11/as.tful"', manifest)

    def test_download_labels_have_translations(self):
        rows = generator.read_rows(generator.ACTIVE_VERSION, generator.CATALOGS[-1][1])
        labels = {row[0]: row[1:] for row in rows}
        required = {"Save file?", "Install homebrew?", "Remove homebrew?",
            "Installed homebrew", "Manage apps installed by Tilefinch", "Downloads",
            "The whole app folder will be deleted.", "Saved data inside it will also be deleted.",
            "Downloaded ZIP files are kept.", "X Save   O Cancel", "X Install   O Cancel",
            "X Remove   O Cancel", "PRESS SQUARE AGAIN TO DELETE", "File saved",
            "X DETAILS   SQUARE DELETE   L/R SECTION", "SQUARE DELETE   O CANCEL"}
        self.assertLessEqual(required, labels.keys())
        for key in required - {"Downloads"}:
            self.assertTrue(all(value != key for value in labels[key]), key)
        header = (ROOT / "include/tilefinch/ui_language.h").read_text()
        self.assertLessEqual(len(rows), int(re.search(r"TILEFINCH_UI_TRANSLATION_ROWS (\d+)u", header).group(1)))

    def test_build_resource_list_includes_each_immutable_download(self):
        listed = subprocess.check_output([sys.executable, str(ROOT / "tools/generate_ui_translations.py"),
                                          "--list-resources"], text=True).splitlines()
        expected = [f"translations/ui/v{version}/{code}.tful"
                    for version, languages in generator.CATALOGS for code, _, _ in languages]
        self.assertEqual(listed, expected)
        self.assertEqual(len(set(listed)), len(listed))

    def test_native_player_labels_have_translations(self):
        languages = generator.CATALOGS[-1][1]
        rows = generator.read_rows(generator.ACTIVE_VERSION, languages)
        keys = {row[0] for row in rows}
        self.assertLessEqual({"Audio", "Subtitles", "Chapters", "LIVE", "SEEK",
            "SELECT ROTATE", "X PAUSE", "X PLAY", "X PAUSE  TRI TRACKS",
            "X PLAY  TRI TRACKS", "X PAUSE  TRI MENU  L/R SEEK",
            "X PLAY  TRI MENU  L/R SEEK", "STICK/L/R SEEK  X PAUSE",
            "STICK/L/R SEEK  X PLAY", "X GO   O CANCEL",
            "L/R  X SELECT  O BACK", "Queue", "Play all", "Play from here",
            "Automatic next: On", "Automatic next: Off", "Retry queue",
            "R Skip   Triangle Queue", "X PAUSE  TRI QUEUE  L/R VIDEO",
            "X PLAY  TRI QUEUE  L/R VIDEO", "Loading playlist...",
            "Playlist could not load"}, keys)
        header = (ROOT / "include/tilefinch/ui_language.h").read_text()
        bound = int(re.search(r"TILEFINCH_UI_TRANSLATION_ROWS (\d+)u", header).group(1))
        self.assertLessEqual(len(rows), bound)

    def test_provider_templates_have_translations(self):
        languages = generator.CATALOGS[-1][1]
        keys = {row[0] for row in generator.read_rows(generator.ACTIVE_VERSION, languages)}
        source = (ROOT / "src/youtube_lite.c").read_text() + (ROOT / "src/youtube_library.inc").read_text()
        # Only inspect C literals, not the explanatory template comment.
        tokens = re.findall(r'''"(?:[^"\\]|\\.)*"|'(?:[^'\\]|\\.)*'|/\*[\s\S]*?\*/|//[^\n]*''', source)
        labels = {label for token in tokens if token.startswith('"')
                  for label in re.findall(r"\{\{([^{}]+)\}\}", token)}
        self.assertGreater(len(labels), 40)
        self.assertEqual(labels - keys, set())
        self.assertLessEqual({"Subscribe", "Unsubscribe", "Add to this playlist", "Remove", "Load more", "Remove from Watch Later",
            "Updating...", "Added to playlist.", "Removed from playlist.", "Subscribed.", "Unsubscribed.",
            "Not confirmed. Select again to retry.", "Sign in to use this action.", "Select an action."}, keys)

    def test_source_tree_contains_no_downloads(self):
        self.assertEqual(list((ROOT / "translations").rglob("*.tful")), [])

    def test_export_matches_keys_and_pins_without_editing_sources(self):
        authored = generator.outputs()
        before = {path: path.read_bytes() for path in authored}
        with tempfile.TemporaryDirectory() as directory:
            resources = Path(directory)
            command = [sys.executable, str(ROOT / "tools/generate_ui_translations.py"),
                       "--resource-dir", directory, "--resources-only"]
            subprocess.run(command, check=True)
            subprocess.run(command + ["--check"], check=True)
            expected = generator.outputs(resources)
            files = list(resources.rglob("*.tful"))
            self.assertEqual(len(files), sum(len(languages) for _, languages in generator.CATALOGS))
            manifest = before[ROOT / "src/generated/ui_languages.inc"].decode()
            for path in files:
                data = path.read_bytes()
                self.assertEqual(data, expected[path])
                if path.parent.name == f"v{generator.ACTIVE_VERSION}":
                    self.assertIn('"' + path.relative_to(resources).as_posix() + '"', manifest)
                    self.assertIn(hashlib.sha256(data).hexdigest(), manifest)
        self.assertEqual(before, {path: path.read_bytes() for path in authored})


if __name__ == "__main__":
    unittest.main()
