#!/usr/bin/env python3
"""The compact representation must reject truncation and retain the oracle."""
import importlib.util
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = Path(sys.argv.pop(1))
spec = importlib.util.spec_from_file_location('tables', ROOT / 'tools/compact_lexbor_tables.py')
tables = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tables)


class TextSource:
    def __init__(self, text):
        self.text = text

    def __truediv__(self, unused):
        return self

    def read_text(self):
        return self.text


class TableTests(unittest.TestCase):
    def test_all_records_and_determinism(self):
        self.assertEqual(len(tables.entity_records(SOURCE)), 9854)
        self.assertEqual(tables.entities(SOURCE), tables.entities(SOURCE))
        self.assertEqual(tables.interfaces(SOURCE), tables.interfaces(SOURCE))

    def test_refuse_out_of_range_and_unknown_records(self):
        original = (SOURCE / 'html/tokenizer/res.h').read_text()
        for replacement in ('{0x100, NULL, 0, 0, 0, 0}',
                            '{0x00, NULL, 0, 65536, 0, 0}',
                            '{0x00, NULL, 7, 0, 0, 0}',
                            '{0x00, NULL, 0, 0, 0, 0, 1}'):
            with self.subTest(replacement=replacement):
                changed = original.replace('{0x00, NULL, 0, 0, 0, 0}', replacement, 1)
                self.assertNotEqual(changed, original)
                with self.assertRaises(ValueError):
                    tables.entities(TextSource(changed))

    def test_shared_dispatch_does_not_replace_specialized_storage(self):
        generated = tables.interfaces(SOURCE)
        self.assertNotIn('(lxb_dom_interface_constructor_f) lxb_html_div_element_interface_create_wrapper,', generated)
        self.assertIn('(lxb_dom_interface_constructor_f) lxb_html_template_element_interface_create_wrapper,', generated)
        self.assertIn('sizeof(lxb_html_div_element_t) == sizeof(lxb_html_element_t)', generated)
        # Original wrappers/public functions are retained; only table entries change.
        self.assertTrue('return lxb_html_div_element_interface_create(' in generated,
                        'public constructor wrapper must remain available')


if __name__ == '__main__':
    unittest.main()
