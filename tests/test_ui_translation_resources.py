"""Keep downloaded catalogs out of the source tree while testing exact bytes."""
import hashlib
import re
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import generate_ui_translations as generator

PROBE = ROOT / "_resource_identity_probe"
_OUTPUTS = {}


def outputs(resources=None):
    """generator.outputs() of the real catalogs, built once per run. It is a
    pure function of the sources, and building it is most of this suite's
    time; a resource directory other than PROBE is PROBE's map rebased."""
    key = None if resources is None else PROBE
    if key not in _OUTPUTS:
        _OUTPUTS[key] = generator.outputs(key)
    if resources is None or resources == PROBE:
        return _OUTPUTS[key]
    return {resources / path.relative_to(PROBE) if path.is_relative_to(PROBE)
            else path: data for path, data in _OUTPUTS[key].items()}


class ResourceTests(unittest.TestCase):
    def test_key_table_prepared_once_per_catalog(self):
        rows = [["Back", "Volver", "Retour"], ["Save", "Guardar", "Enregistrer"]]
        languages = (("es", "Spanish", "EMBEDDED"), ("fr", "French", "EMBEDDED"))
        with mock.patch.object(generator, "CATALOGS", ((14, languages),)), \
                mock.patch.object(generator, "read_rows", return_value=rows), \
                mock.patch.object(generator, "key_hash", wraps=generator.key_hash) as hashed:
            output = generator.outputs(ROOT / "_resource_identity_probe")
        self.assertEqual(hashed.call_count, len(rows))
        self.assertEqual(sum(path.suffix == ".tful" for path in output), len(languages))

    def test_published_resources_remain_immutable(self):
        resources = PROBE
        published = {
            "translations/ui/v12/es.tful": "ccebd012a6ee6461359f268879b5074953d47372b81620760520aef962b6b2c3",
            "translations/ui/v12/fr.tful": "201207ac1648e6b33835d953d56240ce36c97bfa01404fac934133a136d50348",
            "translations/ui/v12/de.tful": "9a2fd56ed2977e18aeb4e36dc2516049a3b426f104bddeb02bbf2632157b3b85",
            "translations/ui/v12/ja.tful": "e953599ca0656a440827121b93e66c3287fe79719d446a96eb6241c8de6e356d",
            "translations/ui/v12/ru.tful": "90000a901c8a6e8b099a8035ef823f76fe35b0800842d020980265cb199c8f94",
            "translations/ui/v12/uk.tful": "ae530b69f9273ea8a3f8df7f49aa6fc484931b2b1b714e72158db87b8bf6d0e2",
            "translations/ui/v12/zh-hans.tful": "aefe41554e8073bf9e0cc8e4b167b0cb11f372ae0535cb660dafec1beffb94b0",
            "translations/ui/v12/ko.tful": "33eb4fd570c4d13430f0b1d09ef3f8bafb4e62471d07809357a7a4cae37f5f4d",
            "translations/ui/v12/hi.tful": "f268cc60c6108e5e45086a89fe8ce232ac2e1f430f6d46c91fa1936d7aec19b7",
            "translations/ui/v12/ar.tful": "e8631ddcab36163a7ca3e0402f0fa4505e9bac312638ff3a966e80280d9635ca",
            "translations/ui/v12/pt.tful": "73e1ebd6ee1d83780324df7c425becc3c16aae20592550ec514c13d5ffd8e1d1",
            "translations/ui/v12/bn.tful": "844408ff42aae65bf8ed29cf31ecc8a1bd69c3072976dbdc5a0f143836e5875e",
            "translations/ui/v12/as.tful": "5f8cc75142e272b0690b2df97102b65f854b78ad5bd8874938016e85fbd73595",
            "translations/ui/v13/es.tful": "ab95ac10a8fec90770ef7a6d1061e3d246b5669277d795896574e68644c28641",
            "translations/ui/v13/fr.tful": "65ef4c7bc794e04f8061272b2c8054c024f46ccc878de02af238ba71e993fce6",
            "translations/ui/v13/de.tful": "bd25907a640d28bec46fa1ceffc43cf60003270d189f30a1d9fefa7eb14ccb89",
            "translations/ui/v13/ja.tful": "64a9d4d14b4ca90ff4599d9eb6d37de9a7c043f6c97170c7c5db34e281577bff",
            "translations/ui/v13/ru.tful": "658248f6fc15326e3e11a5e3efd367b1fad63b2d47c1c18ad36af2989b4e7dc9",
            "translations/ui/v13/uk.tful": "ce2d950d89e59fe51ecdf31b25e5432d3e1a3b1add81e5ea6270894fe1975448",
            "translations/ui/v13/zh-hans.tful": "c9c3f1cddecd4231d5f3eefacbdc23ae0a2d6af0ecf770c05214ffad7fa25423",
            "translations/ui/v13/ko.tful": "83adef8afc6491e0a4f47a02548089d755fe1a4187e60d934d8a17ed0b8a0448",
            "translations/ui/v13/hi.tful": "ae358e8033e33b37409abbb6557217e5c4a5665eb38ec09d40b21fbfa33d6036",
            "translations/ui/v13/ar.tful": "3d38d10c5838e69d10f4df37fb5b9ae03219a3dc10ffcb412031973931afa02d",
            "translations/ui/v13/pt.tful": "ebb951179c03a034acf7c1aa94920b82c49b9ca8d78fe326042ccc9e17629414",
            "translations/ui/v13/bn.tful": "e34b3ecf507f8c7374f8e4e14a2184559fdf19332612accb5684c1762f659922",
            "translations/ui/v13/as.tful": "bf17531deb72b832673386a4e411eef473bf4c829f8ef5e7878e89d42a074072",
            "translations/ui/v14/es.tful": "19da47ea39a7a777d112b4aaf54ecb83a7b4cc24214aac173c93acfe24aab5ec",
            "translations/ui/v14/fr.tful": "9a2939fa86aacbae306e8368b50c126ee357b7a24d592665f4af1350e0d7b941",
            "translations/ui/v14/de.tful": "f0da61f4fdad2fd686605c24d29ac88289c372850d714e1016fb97ee87cc5462",
            "translations/ui/v14/ja.tful": "6d782ea35f187a3b7ee0e8317eddf38bf9857091ae96b36629e8acdf7f8aca3a",
            "translations/ui/v14/ru.tful": "5083ee2485fe80759ed0dd7d48455b33cb392f9ed6ad1dee2bea0f7aba8851f1",
            "translations/ui/v14/uk.tful": "fbe2d1f9d4ccbc884fd3ccb1760c4208482cb9eb99336a125e7d46f679ff5ee0",
            "translations/ui/v14/zh-hans.tful": "d24d6aaf6e73cb2b5366a01d5eed5f91fe20cafdcb63a8eb25221bbda0c07681",
            "translations/ui/v14/ko.tful": "fa9367e14473207bae1e0d16e151bdb5a4d751205d57925e37e1a41e65429d9f",
            "translations/ui/v14/hi.tful": "8b7a52de5dfddd7d9e8e1479e4bba8728fd42b62c8e3a56bb81ab3a6a16c0694",
            "translations/ui/v14/ar.tful": "067a1912ada538cfd288a356457ec94402c76606fc94bab18e162b6f283255dc",
            "translations/ui/v14/pt.tful": "010d2a63272fda0c08a44fe409c2996915a46f7950acf2af7c7e70ce3e966397",
            "translations/ui/v14/bn.tful": "894e231ac2713c1c684602e4724457a6f5ca07d4bb994a32baec4d5c452b566a",
            "translations/ui/v14/as.tful": "18ff37c5fa1fd221a138fa2e37ea9416dbbe4aeb9f177a45ccb80ca236cfdf12",
            "translations/ui/v15/es.tful": "7facd6eae79dbce53e03398a9e146b3a7b745ab57a3bd747d773657bb897d611",
            "translations/ui/v15/fr.tful": "15f0e399ac9e6fde28ade43b20b0c58f85f915b9c7730b083e19242cd214d609",
            "translations/ui/v15/de.tful": "a24edb29fe7d2fb0f5413ac95879993fc87e839c8d2d65bc9a3e5b535c81499b",
            "translations/ui/v15/ja.tful": "00b57c30f653853c64c20c458fecc3243f028ca131747553000f534c87422054",
            "translations/ui/v15/ru.tful": "d16d1cfe63e2933ba78db192ba7430f69c8a9ab51e88d6a3cf793ddba5554258",
            "translations/ui/v15/uk.tful": "7dbcbadf5c3c7e6a9e2931ebd49a422f486a2b24a739f6e1427280e5bf79affa",
            "translations/ui/v15/zh-hans.tful": "b6cee0583a5a3fec687453a9fd859b2bf91b0ad6f804ca838fc96f5e919b43e4",
            "translations/ui/v15/ko.tful": "78cf6e881df95380c75463b1679895f3e140f74803876f387e8fd23f240f0f09",
            "translations/ui/v15/hi.tful": "fbec1227ebecdab3c00589fec9716c676c4afb33c5921ec3d63a414145b6f8cc",
            "translations/ui/v15/ar.tful": "6769bba07635cb006c7bacbf906cbcea02fa23e68acd4ed9aa20de0237171409",
            "translations/ui/v15/pt.tful": "352adf59c77eed42869d200edf678c88dcc9e6a30cf567a8285bb0a18904fcfa",
            "translations/ui/v15/bn.tful": "e1d66d88a3e569f8dd13261e6edca66687d643b5cd434f614db5b33c507e4da2",
            "translations/ui/v15/as.tful": "d334d991dd2f0c3316522a9e1aaa1ebd88a331ca183c884916fb3d6747bd43a5",
        }
        generated = outputs(resources)
        for key, digest in published.items():
            self.assertEqual(hashlib.sha256(generated[resources / key]).hexdigest(),
                             digest, key)

    def test_new_locale_preserves_existing_resource_identity(self):
        manifest = outputs()[ROOT / "src/generated/ui_languages.inc"].decode()
        resources = PROBE
        historical = hashlib.sha256()
        for path, data in sorted(outputs(resources).items()):
            if path.is_relative_to(resources) and int(path.parent.name[1:]) <= 11:
                historical.update(path.relative_to(resources).as_posix().encode() + b"\0" + data)
        # Fixed before v12: every previously published resource remains exact.
        self.assertEqual(historical.hexdigest(),
            "0fbf0634d2a69b5672c4aee19f9436e14065b075ae54eccd8d361a03ca036a18")
        old_languages = generator.CATALOGS[6][1]
        rows = generator.read_rows(7, old_languages)
        for column, (code, label, glyphs) in enumerate(old_languages, 1):
            self.assertIn(f'"translations/ui/v16/{code}.tful"', manifest)
        self.assertIn('"translations/ui/v16/pt.tful"', manifest)
        portuguese = generator.read_rows(8, generator.CATALOGS[7][1])
        self.assertGreaterEqual({row[0] for row in portuguese}, {row[0] for row in rows})
        self.assertEqual(dict(portuguese)["Portuguese"], "Português")
        self.assertIn('"translations/ui/v16/bn.tful"', manifest)
        self.assertIn('"translations/ui/v16/as.tful"', manifest)

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
        source = "".join((ROOT / name).read_text() for name in (
            "src/youtube_lite.c", "src/youtube_library.inc", "src/youtube_related.inc",
            "src/youtube_channel.inc", "src/youtube_navigation.inc"))
        # Only inspect C literals, not the explanatory template comment.
        tokens = re.findall(r'''"(?:[^"\\]|\\.)*"|'(?:[^'\\]|\\.)*'|/\*[\s\S]*?\*/|//[^\n]*''', source)
        labels = {label for token in tokens if token.startswith('"')
                  for label in re.findall(r"\{\{([^{}]+)\}\}", token)}
        self.assertGreater(len(labels), 40)
        self.assertEqual(labels - keys, set())
        self.assertLessEqual({"Subscribe", "Unsubscribe", "Add to this playlist", "Remove", "Load more", "Remove from Watch Later",
            "Updating...", "Added to playlist.", "Removed from playlist.", "Subscribed.", "Unsubscribed.",
            "Not confirmed. Select again to retry.", "Sign in to use this action.", "Select an action."}, keys)

    def test_hashed_catalog_refuses_colliding_keys(self):
        # FNV-1a of the lowercased key, as style_fnv1a(..., true) computes.
        self.assertEqual(generator.key_hash("queued"), 0x58300254)
        self.assertEqual(generator.key_hash("QUEUED"), 0x58300254)
        self.assertEqual(generator.key_hash("Untranslated probe 3466157"), 0x58300254)
        rows = [["queued", "a"], ["Untranslated probe 3466157", "b"]]
        with self.assertRaisesRegex(ValueError, "collision"):
            generator.hashed_catalog("es", rows, 1)
        with self.assertRaisesRegex(ValueError, "too long"):
            generator.hashed_catalog("es", [["x" * 256, "a"]], 1)
        data = generator.hashed_catalog("es", [["Back", "Volver"], ["Settings", "Ajustes"]], 1)
        header = b"TFUI2\tes\n"
        self.assertTrue(data.startswith(header + b"\x00\x02"))
        hashes = [int.from_bytes(data[len(header) + 2 + 4 * i:][:4], "big") for i in range(2)]
        self.assertEqual(hashes, sorted(generator.key_hash(k) for k in ("Back", "Settings")))
        self.assertNotIn(b"Settings", data)

    def test_non_ascii_keys_hash_their_utf8_bytes(self):
        # style_fnv1a folds only ASCII letters, and the length byte counts
        # the key's UTF-8 bytes: "□" alone is three.
        self.assertEqual(generator.key_hash("□ CAPTIONS"),
                         generator.key_hash("□ captions"))
        self.assertNotEqual(generator.key_hash("É"), generator.key_hash("é"))
        data = generator.hashed_catalog("es", [["□ CAPTIONS", "□ SUBTÍTULOS"]], 1)
        header = b"TFUI2\tes\n"
        self.assertEqual(data[len(header) + 6], len("□ CAPTIONS".encode("utf-8")))

    def test_active_catalog_has_no_english_keys(self):
        rows = generator.read_rows(generator.ACTIVE_VERSION, generator.CATALOGS[-1][1])
        data = generator.hashed_catalog("es", rows, 1)
        self.assertTrue(data.startswith(b"TFUI2\tes\n"))
        self.assertNotIn(b"Manage apps installed by Tilefinch", data)

    def test_source_tree_contains_no_downloads(self):
        self.assertEqual(list((ROOT / "translations").rglob("*.tful")), [])

    def test_export_matches_keys_and_pins_without_editing_sources(self):
        authored = outputs()
        before = {path: path.read_bytes() for path in authored}
        with tempfile.TemporaryDirectory() as directory:
            resources = Path(directory)
            command = [sys.executable, str(ROOT / "tools/generate_ui_translations.py"),
                       "--resource-dir", directory, "--resources-only"]
            subprocess.run(command, check=True)
            subprocess.run(command + ["--check"], check=True)
            expected = outputs(resources)
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
