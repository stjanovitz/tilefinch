"""Protect join controls, normalization and bounded offline Indic inventories."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import indic_sequences as indic
import devanagari_sequences as legacy_hindi
import generate_ui_translations as catalogs


class IndicTests(unittest.TestCase):
    def test_complete_hindi_clusters_keep_join_controls(self):
        self.assertEqual(list(indic.clusters("क्‍षि क्‌षि कि", "hi")),
                         ["क्‍षि", "क्‌षि", "कि"])

    def test_hindi_update_keeps_existing_coverage(self):
        rows = catalogs.read_rows(7, catalogs.CATALOGS[6][1])
        labels = [row[9] for row in rows]
        previous = set(legacy_hindi.inventory(labels))
        updated = set(indic.inventory(labels, "hi"))
        self.assertLessEqual(previous, updated)
        self.assertIn(tuple(map(ord, "क्‍षि")), updated)
        self.assertIn(tuple(map(ord, "क्‌षि")), updated)
        self.assertLessEqual(len(updated), 4096)

    def test_bengali_and_assamese_cluster_inventory(self):
        labels = ["বাংলা অসমীয়া ৰাষ্ট্ৰীয় ৱেব", "ক্ষি", "ক্‍ষি", "ক্‌ষি", "ৰ্কি", "ড়ো"]
        for language in ("bn", "as"):
            entries = indic.inventory(labels, language)
            for cluster in ("কি", "ক্ষি", "ক্‍ষি", "ক্‌ষি", "ৰ্কি", "ড়ো"):
                self.assertIn(tuple(map(ord, cluster)), entries)
            self.assertLessEqual(len(entries), 4096)
            self.assertTrue(all(2 <= len(entry) <= 8 for entry in entries))
            self.assertEqual(entries, sorted(set(entries)))
            self.assertEqual(indic.encoded(labels, language), indic.encoded(labels, language))

    def test_does_not_join_across_scripts_or_word_boundaries(self):
        self.assertEqual(list(indic.clusters("কি English ক্‍ षि", "bn")), ["কি", "ক্‍"])
        self.assertEqual(list(indic.clusters("ক‍ষ", "bn")), ["ক", "ষ"])

    def test_refuse_not_truncate_oversized_clusters_or_inputs(self):
        with self.assertRaises(ValueError):
            list(indic.clusters("ক" + "ি" * 8, "bn"))
        with self.assertRaises(ValueError):
            indic.inventory(["a" * 32769], "hi")
        with self.assertRaises(ValueError):
            indic.inventory([], "th")


if __name__ == "__main__":
    unittest.main()
