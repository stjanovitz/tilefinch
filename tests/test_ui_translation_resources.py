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
            "translations/ui/v16/es.tful": "ba9f69592d730fa57c5130a7d7dd642d5865eb9cb5dc399d317aaa322c27f415",
            "translations/ui/v16/fr.tful": "2fcb0401904d54c74648545fe058884ddec2d9ea73beae3040d45595a5048f99",
            "translations/ui/v16/de.tful": "793d1186487878f60b91227a93401c1c60a4dd71311955bbc8ea37c7531529da",
            "translations/ui/v16/ja.tful": "1ef05b73cc12771dd55512c2e69b490e569884e6b74c432d7fb40eb01dc3bc7e",
            "translations/ui/v16/ru.tful": "e40be7ee2d989cdc7865aa427e25b8914e482cc730c0e5a4545c0b6c224afbc7",
            "translations/ui/v16/uk.tful": "d25d963a4e6fe9132d1317388049053296c0926c771759712f681616adcaa4b8",
            "translations/ui/v16/zh-hans.tful": "a2a7a55ee3c71355ab3259f67f49c44f50b66d4575235539c4ae4f38f0cd1e50",
            "translations/ui/v16/ko.tful": "f378ec430a66e1a36ee2922a7d432c9eb3a6f1ebce1eccb75226309e3a43ec2a",
            "translations/ui/v16/hi.tful": "9fda2ccb7ef1f04f8744f87c6449a00079781bde2652fd063047bde3004fe0d5",
            "translations/ui/v16/ar.tful": "36edc3d11b99be1bb8aee53040efc8e8d9f67d3d06acd575232e33b97adfa6e9",
            "translations/ui/v16/pt.tful": "43704abf269667b59ecd129a6728a0cf480c7429c0ed577af71ebd089564d954",
            "translations/ui/v16/bn.tful": "3abd4a97e1ab4ac4cd9e6227afcfacc014ea746f44d57bb6a3a759ecd9666641",
            "translations/ui/v16/as.tful": "1b14d3af11c10fc4d6ad845395da38adaa0f85c257f7fc84ea368438a2ed57eb",
            "translations/ui/v17/es.tful": "6c45c52fae6251e77a3c2104de9923a5326c68246ca485538a2fb4481e489019",
            "translations/ui/v17/fr.tful": "9fdb1c0a282867f2f44ee0123875ba2e5d26b6cd075f6168adc9acca70b762d6",
            "translations/ui/v17/de.tful": "3381d8c6d8b5a8bfd1eb2f4f88438c4f3697813e14ba5116c70061846ab2667a",
            "translations/ui/v17/ja.tful": "cdeabd3a5ff2e0f1e9eda6ba0f7d16b1bc6c74b48651ad15b5aa6a6238faac76",
            "translations/ui/v17/ru.tful": "d482a3250cb3d0f4139f2e99754d24a6d9ab751cdbdcd9980ab4ed405c666050",
            "translations/ui/v17/uk.tful": "22364f9b68535581f9446b56633d801c1339f23b45fc072f8d04e0239679ae14",
            "translations/ui/v17/zh-hans.tful": "2471a39dc2fa5b515275f23334a5c67e3a4900344f8b11afe1f9e17cc1d2237f",
            "translations/ui/v17/ko.tful": "5eb1a45bd74244d2785276eecc8fa1650613e10c16fc46ababad603634bcd7a9",
            "translations/ui/v17/hi.tful": "f4245bf0f1f5177e3072f35ac5df89e12778a0b63fb6ac3eeecb229b8d9317a9",
            "translations/ui/v17/ar.tful": "53b421c9ad09a33b2f0a783f70189848ee58c2f4d3e576e62671a9d592596ebc",
            "translations/ui/v17/pt.tful": "d345414e6b472071bb60e69eb9df288f5ffbd41997d379d10c6d3062d811f592",
            "translations/ui/v17/bn.tful": "aea1a6de553a79526fcf860221653c515d3db2b139d410d84438533d1e1c7b23",
            "translations/ui/v17/as.tful": "17ac666499a2f692fb24ce40577d660528162fbfb048703e4c2fa3b014aff8c5",
        }
        published.update({
            "translations/ui/v18/es.tful": "db0aa30d8e2a640c0229059b696d186c55f25fe6f30ae60d46c5234784fca09f",
            "translations/ui/v18/fr.tful": "ca2d080f59b42f36a803953d9f2ad2c8f05b162f107ba7aad09eca47b963bc95",
            "translations/ui/v18/de.tful": "c9e87f72229cc3c06d8ca43692785f0540d8086da61980f39807e9f1dc70f3ff",
            "translations/ui/v18/ja.tful": "331a383019d86806f717a60dd613e21d0e69704f06ddd5a5da8de7dae83b0c88",
            "translations/ui/v18/ru.tful": "97b1e75087d9a4231ed5a6ea17ef22c6ce8e94bd0748e8563c72616c780ff3ef",
            "translations/ui/v18/uk.tful": "bdd491a560080aae3c827d38f7b4abd513becd29e78e700d6244bf932b5101cc",
            "translations/ui/v18/zh-hans.tful": "f060e579c651c262cf675c19458a1138f1ecedf7ee43d9c25f82df8408ccee5d",
            "translations/ui/v18/ko.tful": "c91fad65e23c028c6086e11af50b0478c0b634d66ed2cf2306ff4e0fa49aba15",
            "translations/ui/v18/hi.tful": "cd79185685ee9f89be5e1f6452f620259df8a3d6709c33a628efb415969256c8",
            "translations/ui/v18/ar.tful": "811462d3eadacf27b7fe0da0376a93adcfae37292c57ddd726bff7806a9aa96c",
            "translations/ui/v18/pt.tful": "3d409ae70bc3d03d106645390a54c19675e24a3f699d84bf0a83082e9fceb8f2",
            "translations/ui/v18/bn.tful": "4d66b64da1220c1e534754f9679585eb87cfbe1b7ef5a8f9609940d5d561c719",
            "translations/ui/v18/as.tful": "067622f4e0fa0579eeb19fbaa4aa20a66880a97110a300f4cbdb85455d87227c",
            "translations/ui/v18/he.tful": "9cdd605c3aba182f90fe7e788214a9e3eecdbf4b95b2056993a9c0d0b818fbb7",
        })
        # v19, pinned by 0.1.51 (src/generated/ui_languages.inc at a3c1f099).
        published.update({
            "translations/ui/v19/es.tful": "1cc133970d705fafdd5590e8e73a51aa5a8b9bc2dd57801d9a7e0c1a6d5088fb",
            "translations/ui/v19/fr.tful": "76c3f8d42e441b0b37c3476bb8377b7435c5505991b66084a057cd458787cb5f",
            "translations/ui/v19/de.tful": "9df2f4e73c563c5b599a625d5d7ee291c2a16b5f4ff6f08c565d4eb8d3113333",
            "translations/ui/v19/ja.tful": "43a62fa24a98ace4ecf5a74c5307b02588cdf088985d3414e0f59cbe71cfd74e",
            "translations/ui/v19/ru.tful": "7e3b99233253b82b1088b6c9a7bb2ebd7fef28d438da63ed37069e77fd7e788b",
            "translations/ui/v19/uk.tful": "c7626cf53e51a83df44a6f258dd51a8f828d325878e2002b0b9ad92928138cf8",
            "translations/ui/v19/zh-hans.tful": "8b982b6a7e869820eff3393075215529e30d0e4a57fa2e1c429ca3e94a76a5e5",
            "translations/ui/v19/ko.tful": "05f7e2e9da2640c3035d6906b8322e428b19ea5ca60ec2a5db024a4b64b7ba0f",
            "translations/ui/v19/hi.tful": "0fec6daff42c2306d4d0ab9a40aeabe7988e1024d526b3fa22887a553fcd5248",
            "translations/ui/v19/ar.tful": "e01b1b39c5947ba729708638567d797804d27555855ab8a2f23ff5c5f3fa0d95",
            "translations/ui/v19/pt.tful": "24ef743ee01b7034eca384404cf594d9e97f786f3535eef15116ae9eab6f0274",
            "translations/ui/v19/bn.tful": "fa120fccf2713151b701e55a5b71a2fc6bf10fbf1b58c9616854e19d1d7e174a",
            "translations/ui/v19/as.tful": "ef017d328b24fa20c0dbaa2959b80ea7c137f3b547d0abe56546067dead2bea6",
            "translations/ui/v19/he.tful": "fd9130a521cdd9f16ad1691a8dfce21f77982c958cd8772436563d26a224e53c",
        })
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
            self.assertIn(f'"translations/ui/v20/{code}.tful"', manifest)
        self.assertIn('"translations/ui/v20/pt.tful"', manifest)
        portuguese = generator.read_rows(8, generator.CATALOGS[7][1])
        self.assertGreaterEqual({row[0] for row in portuguese}, {row[0] for row in rows})
        self.assertEqual(dict(portuguese)["Portuguese"], "Português")
        self.assertIn('"translations/ui/v20/bn.tful"', manifest)
        self.assertIn('"translations/ui/v20/as.tful"', manifest)

    def test_v16_stays_frozen_under_the_narrow_bound(self):
        # v16 is pinned by the release candidate, whose reader has 16-bit
        # value offsets: its sources, sequences and files never change, and
        # every file before v17 fits 64 KiB - 1.
        self.assertEqual(hashlib.sha256((ROOT / "translations/ui/v16/source.tsv")
                                        .read_bytes()).hexdigest(),
                         "cd5141d73bc0ab708b83bc6ed6af5fc61dfe572ba9cb9020bf79876a7192d153")
        for name, digest in (("hi.sequences", "c764fd269b0f8ed2ca8d8c3a7779df2105f2d287f18091c3d8d148527f20cb09"),
                             ("bengali-assamese.sequences", "6b6b9b9838821bf6545b5404b0b5e412dfaf0c13398b3c76cbb39544347970aa")):
            self.assertEqual(hashlib.sha256((ROOT / "translations/ui/v16" / name)
                                            .read_bytes()).hexdigest(), digest, name)
        generated = outputs(PROBE)
        for path, data in generated.items():
            if path.is_relative_to(PROBE) and int(path.parent.name[1:]) < generator.WIDE_VERSION:
                self.assertLessEqual(len(data), 64 * 1024 - 1, path)
        self.assertEqual(generator.byte_limit(16), 64 * 1024 - 1)
        self.assertEqual(generator.byte_limit(generator.WIDE_VERSION), 128 * 1024)
        header = (ROOT / "include/tilefinch/ui_language.h").read_text()
        self.assertIn("#define TILEFINCH_UI_TRANSLATION_BYTES (128u * 1024u)", header)

    def test_v17_stays_frozen(self):
        # v17 is published and pinned by released browsers; Hebrew and the
        # rows after it went into v18 instead of new columns and rows here.
        for name, digest in (("source.tsv", "dbdab76c36c9ef4ba8202ff7e4ef9b2ec71c3ab0b9632eb059401c2d5c91cc39"),
                             ("hi.sequences", "c764fd269b0f8ed2ca8d8c3a7779df2105f2d287f18091c3d8d148527f20cb09"),
                             ("bengali-assamese.sequences", "6b6b9b9838821bf6545b5404b0b5e412dfaf0c13398b3c76cbb39544347970aa")):
            self.assertEqual(hashlib.sha256((ROOT / "translations/ui/v17" / name)
                                            .read_bytes()).hexdigest(), digest, name)

    def test_v18_catalog(self):
        # v18 holds every language, Hebrew last, and keeps every v17 key
        # it does not retire.
        # Hebrew reads right to left with Arabic and needs no shaping: no
        # niqqud or cantillation marks, no presentation forms.
        version, languages = generator.CATALOGS[-3]
        self.assertEqual(version, 18)
        self.assertEqual(languages, generator.CATALOGS[-4][1] + [("he", "Hebrew", "HEBREW")])
        self.assertEqual(generator.RIGHT_TO_LEFT, {"ar", "he"})
        rows = generator.read_rows(18, languages)
        v17 = generator.read_rows(17, generator.CATALOGS[-4][1])
        self.assertLessEqual({row[0] for row in v17} - generator.RETIRED_KEYS[18],
                             {row[0] for row in rows})
        hebrew = [(row[0], row[-1]) for row in rows]
        self.assertEqual(dict(hebrew)["Hebrew"], "עברית")
        for key, value in hebrew:
            self.assertRegex(value, "[\u05d0-\u05ea]|^[^A-Za-z]*$|^(JavaScript|Memory Stick|Shorts)$", key)
            self.assertFalse(any(0x0591 <= ord(c) <= 0x05c7 or 0xfb1d <= ord(c) <= 0xfb4f
                                 for c in value), key)
        manifest = outputs()[ROOT / "src/generated/ui_languages.inc"].decode()
        self.assertIn('BROWSER_GLYPH_LANGUAGE_HEBREW', manifest)
        self.assertRegex(manifest, r'\{"he", "Hebrew", BROWSER_GLYPH_LANGUAGE_HEBREW, "[0-9a-f]{64}", 20, 1, '
                                   r'"translations/ui/v20/he.tful", true\},')
        self.assertRegex(manifest, r'\{"ar", "Arabic", .*, true\},')
        self.assertEqual(manifest.count(", true},"), 2)

    def test_v19_adds_labels_without_rewriting_v18(self):
        self.assertEqual(generator.CATALOGS[-2][0], 19)
        self.assertEqual(generator.CATALOGS[-2][1], generator.CATALOGS[-3][1])
        previous = generator.read_rows(18, generator.CATALOGS[-3][1])
        current = generator.read_rows(19, generator.CATALOGS[-2][1])
        self.assertEqual(len(current), len(previous) + 2)
        old = {row[0]: row for row in previous}
        self.assertEqual({row[0]: row for row in current if row[0] in old}, old)
        self.assertEqual(hashlib.sha256((ROOT / "translations/ui/v18/source.tsv")
                                        .read_bytes()).hexdigest(),
                         "b942dbf79450906c7d626c19570f5bafef660f19034e9e01bd1d1a3f0d46d6d9")

    def test_v20_adds_labels_without_rewriting_v19(self):
        # v19 is pinned by 0.1.51; the Basic-by-default notice, Try full
        # page and frame offer rows went into v20.
        self.assertEqual(generator.ACTIVE_VERSION, 20)
        self.assertEqual(generator.CATALOGS[-1][1], generator.CATALOGS[-2][1])
        previous = generator.read_rows(19, generator.CATALOGS[-2][1])
        current = generator.read_rows(20, generator.CATALOGS[-1][1])
        old = {row[0]: row for row in previous}
        self.assertEqual({row[0]: row for row in current if row[0] in old}, old)
        self.assertEqual({row[0] for row in current} - set(old), {
            "BASIC VIEW: SCRIPTS RAN OUT OF MEMORY LAST TIME",
            "X Try full page  O Keep Basic view", "Try full page",
            "LOADING FULL PAGE...",
            # The frame offer and its answers' statuses.
            "A frame has a large script", "Script size:",
            "It may take a minute or more.", "Start it?",
            "Scripts can run out of PSP memory.", "X Start   O Not now",
            "STARTING THE FRAME'S SCRIPT", "FRAME SCRIPT NOT STARTED"})
        self.assertEqual(hashlib.sha256((ROOT / "translations/ui/v19/source.tsv")
                                        .read_bytes()).hexdigest(),
                         "4ea6c18fa757e36b351a3553d91c01ba3c034627ac2bd41a2945d4552e8838e5")

    def test_active_clusters_are_in_the_required_glyph_revision(self):
        # A pack revision is built from the sequences of the catalog that
        # introduced it (MINIMUM_GLYPH_SEQUENCES). A later catalog that keeps
        # that revision must not need a cluster outside those sequences;
        # otherwise reword the row or raise the revision and rebuild the pack.
        for glyphs, name in (("DEVANAGARI", "hi.sequences"),
                             ("BENGALI_ASSAMESE", "bengali-assamese.sequences")):
            # The newest catalog with this script's labels.
            active = max(version for version, languages in generator.CATALOGS
                         if any(script == glyphs for _, _, script in languages))
            required = generator.minimum_glyph_sequence(glyphs, active)
            built_from = max(first for (script, first), sequence
                             in generator.MINIMUM_GLYPH_SEQUENCES.items()
                             if script == glyphs and sequence == required)
            def entries(version):
                text = (ROOT / f"translations/ui/v{version}" / name).read_text()
                return {line for line in text.splitlines()
                        if line and not line.startswith("#")}
            missing = sorted(entries(active) - entries(built_from))
            self.assertEqual([], missing, (glyphs, required, built_from))

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

    def test_chrome_labels_have_translations(self):
        # The page's bottom bar, the Collections screen and the Settings
        # screens draw these keys; every language translates them.
        rows = generator.read_rows(generator.ACTIVE_VERSION, generator.CATALOGS[-1][1])
        # Lookups fold ASCII case: COLLECTIONS reads the Collections row.
        labels = {generator.folded_key(row[0]): row[1:] for row in rows}
        required = {"Open", "Back", "Bar", "Search", "Reload", "Edit", "Enter", "Cancel",
            "COLLECTIONS", "BOOKMARKS", "HISTORY", "DOWNLOADS",
            "Saved items", "SHOTS", "NOTHING HERE YET", "NO DOWNLOADS YET",
            "NOTHING SAVED YET", "NO BOOKMARKS YET", "NO HISTORY YET",
            "NO SCREENSHOT LIST AVAILABLE", "L/R SECTION   O BACK",
            "PRESS SQUARE AGAIN TO UNINSTALL", "N/A", "Always", "Never", "Smart",
            "Website fonts", "Save YouTube login",
            "X Edit URL   O Back", "X Apply   O Back", "Copy .tfth files into data/themes",
            "Showing first 12 valid theme files", "Themes", "Midnight", "Cobalt", "Slate",
            "Ember", "Daylight", "CONTINUE", "NOTHING OPEN YET", "CURRENT PAGE", "Untitled",
            "Page", "Profile", "STARTING BROWSER", "HOLD L WHILE STARTING", "Opening video",
            "Chinese (S)", "Chinese (T)", "Italian", "Sans", "Serif", "Standard",
            "Save YouTube login?", "Voice input", "Update channel"}
        # Words that read the same in the language itself.
        shared = {("COLLECTIONS", "fr"), ("DOWNLOADS", "de"), ("Smart", "de"),
                  ("Cobalt", "fr"), ("Page", "fr"), ("Sans", "es"), ("Sans", "fr"),
                  ("Serif", "es"), ("Serif", "fr")}
        codes = [code for code, _, _ in generator.CATALOGS[-1][1]]
        for key in required:
            values = labels.get(generator.folded_key(key))
            self.assertIsNotNone(values, key)
            for code, value in zip(codes, values):
                if (key, code) not in shared:
                    self.assertNotEqual(value.casefold(), key.casefold(), (key, code))

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
