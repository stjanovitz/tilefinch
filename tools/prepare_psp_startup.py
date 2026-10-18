#!/usr/bin/env python3
"""Pair the first-install PRX with its PBP, without changing update file lists.

The marker is a bundle identity, not executable authentication. Like the root
launcher, a manually installed module is trusted as part of the install archive.
Journalled updates never use this first-install shortcut.
"""
import argparse
import hashlib
from pathlib import Path
import struct

MAGIC = b'TFBOOT01'
LIMIT = 16 * 1024 * 1024


def prepare(pbp: Path, module: Path, header: Path) -> None:
    if not 60 <= pbp.stat().st_size <= LIMIT or not 20 <= module.stat().st_size <= LIMIT:
        raise ValueError('startup executable exceeds bounds')
    package = pbp.read_bytes()
    body = module.read_bytes()
    if package[:8] != b'\0PBP\0\0\1\0':
        raise ValueError('not a PBP')
    offsets = struct.unpack_from('<8I', package, 8)
    if offsets != tuple(sorted(offsets)) or offsets[0] < 40 or offsets[-1] > len(package):
        raise ValueError('invalid PBP offsets')
    begin, end = offsets[-2:]
    if body[:7] != b'\x7fELF\x01\x01\x01' or body[16:20] != b'\xa0\xff\x08\0':
        raise ValueError('startup module is not relocatable Allegrex ELF')
    if package[begin:end] != body:
        raise ValueError('PBP and module differ')
    digest = hashlib.sha256(body).digest()
    marker = MAGIC + digest
    if package[end:] not in (b'', marker):
        raise ValueError('unexpected PBP auxiliary payload')
    if len(package) + (0 if package[end:] else len(marker)) > LIMIT:
        raise ValueError('marked package exceeds bounds')
    if not package[end:]:
        package += marker
        pbp.write_bytes(package)
    text = ('/* Generated bundle identity; not a persistent reuse receipt. */\n'
            '#define TILEFINCH_PREPACKAGED_MODULE "startup-' + digest.hex() + '.prx"\n'
            f'#define TILEFINCH_PREPACKAGED_PBP_BYTES {len(package)}u\n'
            f'#define TILEFINCH_PREPACKAGED_MODULE_BYTES {len(body)}u\n'
            '#define TILEFINCH_PREPACKAGED_DIGEST {' + ','.join(f'0x{x:02x}' for x in digest) + '}\n')
    header.parent.mkdir(parents=True, exist_ok=True)
    if not header.exists() or header.read_text() != text:
        header.write_text(text)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pbp', type=Path, required=True)
    parser.add_argument('--module', type=Path, required=True)
    parser.add_argument('--header', type=Path, required=True)
    args = parser.parse_args()
    prepare(args.pbp, args.module, args.header)
