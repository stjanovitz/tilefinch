#!/usr/bin/env python3
"""Generate Tilefinch's bounded CJK and emoji bitmap fallback.

Input is GNU Unifont's unifont_all-17.0.04.hex.gz.  The checked-in output is
deliberately generated ahead of time: PSP and ordinary host builds never need
Python, network access, or the complete upstream font.
"""

from __future__ import annotations

import argparse
import gzip
import hashlib
from pathlib import Path


UNIFONT_SHA256 = "c31d210962408a00de8e2ebe2f2fc26824d7a4939d4eb15d347761fb2a0b39a6"
CJK_RANGES = (
    (0x3000, 0x30FF),
    (0x31F0, 0x31FF),
    (0x3400, 0x4DBF),
    (0x4E00, 0x9FFF),
    (0xF900, 0xFAFF),
    (0xFF00, 0xFFEF),
)
EMOJI_RANGES = (
    (0x2600, 0x27BF),
    (0x1F300, 0x1F6FF),
    (0x1F900, 0x1F9FF),
    (0x1FA70, 0x1FAFF),
)
BLOCK_GLYPHS = 64


def in_ranges(codepoint: int, ranges: tuple[tuple[int, int], ...]) -> bool:
    return any(first <= codepoint <= last for first, last in ranges)


def common_cjk(codepoint: int) -> bool:
    if not in_ranges(codepoint, CJK_RANGES):
        return False
    character = chr(codepoint)
    for encoding in ("gb2312", "shift_jis"):
        try:
            character.encode(encoding)
            return True
        except UnicodeEncodeError:
            pass
    return False


def normalize_bitmap(raw: bytes) -> tuple[bytes, bool]:
    if len(raw) == 32:
        return raw, False
    if len(raw) == 16:
        expanded = bytearray()
        for row in raw:
            expanded.extend((row, 0))
        return bytes(expanded), True
    raise ValueError(f"expected an 8x16 or 16x16 glyph, got {len(raw)} bytes")


def lzss_compress(data: bytes) -> bytes:
    """Small random-access blocks: flag bit 1=literal, 0=12-bit backref."""
    output = bytearray()
    chains: dict[bytes, list[int]] = {}
    cursor = 0
    while cursor < len(data):
        flag_offset = len(output)
        output.append(0)
        flags = 0
        for bit in range(8):
            if cursor >= len(data):
                break
            best_length = 0
            best_distance = 0
            key = data[cursor:cursor + 3]
            candidates = chains.get(key, ()) if len(key) == 3 else ()
            for candidate in reversed(candidates[-96:]):
                distance = cursor - candidate
                if distance > 0xFFF:
                    continue
                length = 0
                while (length < 18 and cursor + length < len(data)
                       and data[candidate + length] == data[cursor + length]):
                    length += 1
                if length > best_length and length >= 3:
                    best_length = length
                    best_distance = distance
                    if length == 18:
                        break
            consumed = best_length if best_length >= 3 else 1
            if best_length >= 3:
                encoded = ((best_length - 3) << 12) | best_distance
                output.extend((encoded >> 8, encoded & 0xFF))
            else:
                flags |= 1 << bit
                output.append(data[cursor])
            for position in range(cursor, cursor + consumed):
                position_key = data[position:position + 3]
                if len(position_key) == 3:
                    chain = chains.setdefault(position_key, [])
                    chain.append(position)
                    if len(chain) > 128:
                        del chain[:-96]
            cursor += consumed
        output[flag_offset] = flags
    return bytes(output)


def lzss_verify(data: bytes, compressed: bytes) -> None:
    output = bytearray()
    cursor = 0
    while cursor < len(compressed) and len(output) < len(data):
        flags = compressed[cursor]
        cursor += 1
        for bit in range(8):
            if len(output) >= len(data):
                break
            if flags & (1 << bit):
                output.append(compressed[cursor])
                cursor += 1
                continue
            encoded = (compressed[cursor] << 8) | compressed[cursor + 1]
            cursor += 2
            distance = encoded & 0xFFF
            length = (encoded >> 12) + 3
            if distance == 0 or distance > len(output):
                raise ValueError("invalid generated LZSS back-reference")
            for _ in range(length):
                output.append(output[-distance])
    if bytes(output) != data:
        raise ValueError("generated LZSS stream does not round-trip")


def parse_unifont(path: Path) -> tuple[list[tuple[int, bytes, bool]],
                                         list[tuple[int, bytes, bool]]]:
    source = path.read_bytes()
    digest = hashlib.sha256(source).hexdigest()
    if digest != UNIFONT_SHA256:
        raise ValueError(f"unexpected GNU Unifont archive SHA-256: {digest}")
    cjk: list[tuple[int, bytes, bool]] = []
    emoji: list[tuple[int, bytes, bool]] = []
    with gzip.open(path, "rt", encoding="ascii") as stream:
        for line_number, line in enumerate(stream, 1):
            code_text, separator, bitmap_text = line.strip().partition(":")
            if not separator:
                raise ValueError(f"malformed input line {line_number}")
            codepoint = int(code_text, 16)
            destination = cjk if common_cjk(codepoint) else (
                emoji if in_ranges(codepoint, EMOJI_RANGES) else None
            )
            if destination is None:
                continue
            bitmap, narrow = normalize_bitmap(bytes.fromhex(bitmap_text))
            destination.append((codepoint, bitmap, narrow))
    return cjk, emoji


def emit_table(name: str, records: list[tuple[int, bytes, bool]]) -> str:
    codes = [codepoint for codepoint, _, _ in records]
    narrow_codepoints = name == "cjk"
    if narrow_codepoints and any(codepoint > 0xFFFF for codepoint in codes):
        raise ValueError("CJK codepoint table no longer fits uint16_t")
    code_type = "uint16_t" if narrow_codepoints else "uint32_t"
    width_bits = bytearray((len(records) + 7) // 8)
    for index, (_, _, narrow) in enumerate(records):
        if narrow:
            width_bits[index // 8] |= 1 << (index & 7)
    code_lines = []
    for offset in range(0, len(codes), 10):
        code_lines.append("    " + ", ".join(
            f"UINT32_C(0x{codepoint:x})"
            for codepoint in codes[offset:offset + 10]
        ) + ",\n")
    width_lines = []
    for offset in range(0, len(width_bits), 16):
        width_lines.append("    " + ", ".join(
            f"0x{value:02x}" for value in width_bits[offset:offset + 16]
        ) + ",\n")
    compressed = bytearray()
    offsets = [0]
    for offset in range(0, len(records), BLOCK_GLYPHS):
        raw = b"".join(
            bitmap for _, bitmap, _ in records[offset:offset + BLOCK_GLYPHS]
        )
        block = lzss_compress(raw)
        lzss_verify(raw, block)
        compressed.extend(block)
        offsets.append(len(compressed))
    offset_lines = []
    for offset in range(0, len(offsets), 10):
        offset_lines.append("    " + ", ".join(
            f"UINT32_C({value})" for value in offsets[offset:offset + 10]
        ) + ",\n")
    compressed_lines = []
    for offset in range(0, len(compressed), 20):
        compressed_lines.append("    " + ", ".join(
            f"0x{value:02x}" for value in compressed[offset:offset + 20]
        ) + ",\n")
    block_count = (len(records) + BLOCK_GLYPHS - 1) // BLOCK_GLYPHS
    return (
        f"#define BUILTIN_{name.upper()}_COUNT {len(records)}u\n"
        f"static const {code_type} builtin_{name}_codepoints"
        f"[BUILTIN_{name.upper()}_COUNT] = {{\n"
        + "".join(code_lines)
        + "};\n"
        f"static const unsigned char builtin_{name}_narrow_bits"
        f"[(BUILTIN_{name.upper()}_COUNT + 7u) / 8u] = {{\n"
        + "".join(width_lines)
        + "};\n"
        f"#define BUILTIN_{name.upper()}_BLOCK_COUNT {block_count}u\n"
        f"static const uint32_t builtin_{name}_block_offsets"
        f"[BUILTIN_{name.upper()}_BLOCK_COUNT + 1u] = {{\n"
        + "".join(offset_lines)
        + "};\n"
        f"static const unsigned char builtin_{name}_compressed"
        f"[{len(compressed)}u] = {{\n"
        + "".join(compressed_lines)
        + "};\n\n"
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    arguments = parser.parse_args()
    cjk, emoji = parse_unifont(arguments.input)
    if len(cjk) != 10150 or len(emoji) != 1872:
        raise ValueError(
            f"selection drifted: expected 10150/1872, got "
            f"{len(cjk)}/{len(emoji)}"
        )
    generated = (
        "/* Generated by tools/generate_font_fallback.py from GNU Unifont\n"
        " * 17.0.04.  Do not edit by hand.  Glyph data is available under\n"
        " * SIL OFL 1.1; see fonts/LICENSE-Unifont.txt.\n"
        " */\n\n"
        f"#define BUILTIN_BITMAP_BLOCK_GLYPHS {BLOCK_GLYPHS}u\n\n"
        + emit_table("cjk", cjk)
        + emit_table("emoji", emoji)
    )
    arguments.output.write_text(generated, encoding="ascii")


if __name__ == "__main__":
    main()
