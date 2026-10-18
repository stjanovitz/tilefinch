#!/usr/bin/env python3
"""Derive bounded entity records and equivalent cold interface dispatch.

The pinned upstream tables remain the oracle. Fail closed on an unknown
record, index, or constructor body instead of truncating data or guessing.
"""
import argparse
from pathlib import Path
import re

ENTRY = re.compile(r'\{(0x[0-9a-f]+),\s*(NULL|"(?:\\.|[^"\\])*"),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+)\}')


def entity_records(source):
    text = (source / 'html/tokenizer/res.h').read_text()
    body = text.split('[] =', 1)[1].split(';', 1)[0]
    matches = list(ENTRY.finditer(body))
    if len(matches) != 9854 or re.sub(r'[\s{},]', '', ENTRY.sub('', body)):
        raise ValueError('unexpected pinned entity table')
    rows = [m.groups() for m in matches]
    for key, value, length, left, right, next_ in rows:
        if int(key, 16) > 255 or int(length) > 6:
            raise ValueError('entity byte/length exceeds representation')
        if any(int(index) >= len(rows) for index in (left, right, next_)):
            raise ValueError('entity index outside table')
        if (value == 'NULL') != (int(length) == 0):
            raise ValueError('invalid entity value')
    return rows


def entities(source):
    rows = entity_records(source)
    return ('/* Generated from Lexbor 3.0.0; do not edit.\n'
            ' * Entity data Copyright (C) 2018-2026 Alexander Borisov, Apache-2.0. */\n'
            'static const lxb_html_entity_entry_t lxb_html_tokenizer_res_entities_sbst[] = {\n'
            + ''.join(f'    {{{key}, {length}, {left}, {right}, {next_}, {value}}},\n'
                      for key, value, length, left, right, next_ in rows)
            + '};\n')


def normalized(text):
    return re.sub(r'\s+', '', re.sub(r'/\*.*?\*/', '', text, flags=re.S))


def interfaces(source):
    text = (source / 'html/interface_res.h').read_text()
    base = (source / 'html/interfaces/element.c').read_text()
    # Only exact generic initializer/teardown bodies qualify. Specialized
    # objects (template/style/etc.) keep their original dispatch and storage.
    replacements = {}
    proofs = []
    for path in sorted((source / 'html/interfaces').glob('*_element.c')):
        original = path.read_text()
        stem = path.stem
        type_ = f'lxb_html_{stem}_t'
        for operation in ('create', 'destroy'):
            name = f'lxb_html_{stem}_interface_{operation}'
            pattern = rf'{name}\([^)]*\)\s*\{{(.*?)\n\}}'
            candidate = re.search(pattern, original, re.S)
            reference = re.search(rf'lxb_html_element_interface_{operation}\([^)]*\)\s*\{{(.*?)\n\}}', base, re.S)
            if not candidate or not reference:
                continue
            body = candidate.group(1)
            # The parameter name differs in the trivial destructor bodies.
            parameter = re.search(rf'{name}\([^)]*\*([a-z_]+)\)', original)
            if operation == 'destroy' and parameter:
                body = re.sub(rf'\b{parameter.group(1)}\b', 'element', body)
            body = body.replace(type_, 'lxb_html_element_t')
            if normalized(body) != normalized(reference.group(1)):
                continue
            if operation == 'create':
                header = path.with_suffix('.h').read_text()
                structure = re.search(rf'struct lxb_html_{stem}\s*\{{(.*?)\}};', header, re.S)
                if not structure or normalized(structure.group(1)) != 'lxb_html_element_telement;':
                    continue
            wrapper = name + '_wrapper'
            if wrapper not in text:
                continue
            replacements[wrapper] = f'lxb_html_element_interface_{operation}_wrapper'
            if operation == 'create':
                proofs.append(f'typedef char tilefinch_size_{stem}[(sizeof({type_}) == sizeof(lxb_html_element_t)) ? 1 : -1];\n')
    if len(replacements) < 100:
        raise ValueError('unexpected interface initializer coverage')
    # Replace only dispatch table entries, not declarations, wrappers, or the
    # externally callable functions. Their ABI and identities remain intact.
    for old, new in replacements.items():
        text = re.sub(rf'(\(lxb_dom_interface_(?:constructor|destructor)_f\) ){old}\b', rf'\g<1>{new}', text)
    return '/* Generated cold dispatch sharing; upstream copyright follows. */\n' + text + '\n' + ''.join(proofs)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    for name, data in (('tilefinch_lexbor_entities.inc', entities(args.source)),
                       ('tilefinch_lexbor_interfaces.inc', interfaces(args.source))):
        path = args.output / name
        if not path.exists() or path.read_text() != data:
            path.write_text(data)


if __name__ == '__main__':
    main()
