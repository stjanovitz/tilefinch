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
    def test_provider_templates_have_translations(self):
        languages = generator.CATALOGS[-1][1]
        keys = {row[0] for row in generator.read_rows(generator.ACTIVE_VERSION, languages)}
        source = (ROOT / "src/youtube_lite.c").read_text()
        # Only inspect C literals, not the explanatory template comment.
        tokens = re.findall(r'''"(?:[^"\\]|\\.)*"|'(?:[^'\\]|\\.)*'|/\*[\s\S]*?\*/|//[^\n]*''', source)
        labels = {label for token in tokens if token.startswith('"')
                  for label in re.findall(r"\{\{([^{}]+)\}\}", token)}
        self.assertGreater(len(labels), 40)
        self.assertEqual(labels - keys, set())

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
