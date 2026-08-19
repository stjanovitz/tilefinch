#!/usr/bin/env python3
"""Format-level tests for tools/decode_diagnostic_qr.py."""

from __future__ import annotations

import importlib.util
import struct
import sys
import zlib
from pathlib import Path


sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "decode_diagnostic_qr", ROOT / "tools" / "decode_diagnostic_qr.py"
)
assert SPEC is not None and SPEC.loader is not None
DECODER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(DECODER)


def base45_encode(data: bytes) -> str:
    output: list[str] = []
    at = 0
    while at + 1 < len(data):
        value = data[at] * 256 + data[at + 1]
        output.extend(
            (
                DECODER.BASE45[value % 45],
                DECODER.BASE45[(value // 45) % 45],
                DECODER.BASE45[value // (45 * 45)],
            )
        )
        at += 2
    if at < len(data):
        value = data[at]
        output.extend((DECODER.BASE45[value % 45], DECODER.BASE45[value // 45]))
    return "".join(output)


def make_bundle() -> tuple[bytes, str, bytes]:
    name = b"tilefinch-last-error.txt"
    log = b"tilefinch-device-error-v2\nstage=navigation\ndetail=test\n"
    version = b"test"
    raw = bytearray(b"TFDG\x01\x01\0\0\0\0\0\0")
    raw.extend(struct.pack(">QQII", 7, 123456789, 2, 0x06060110))
    raw.append(len(version))
    raw.extend(version)
    raw.append(len(name))
    raw.extend(struct.pack(">II", len(log), zlib.crc32(log) & 0xFFFFFFFF))
    raw.extend(name)
    raw.extend(log)
    report_crc = zlib.crc32(raw) & 0xFFFFFFFF
    raw[8:12] = struct.pack(">I", report_crc)
    return bytes(raw), f"{report_crc:08X}", log


def make_pages(raw: bytes, report_id: str) -> list[str]:
    compressed = zlib.compress(raw, 1)
    compressed_crc = zlib.crc32(compressed) & 0xFFFFFFFF
    chunks = [compressed[at : at + 31] for at in range(0, len(compressed), 31)]
    return [
        (
            f"TFD1:{report_id}:{index:02X}:{len(chunks):02X}:"
            f"{compressed_crc:08X}:{zlib.crc32(chunk) & 0xFFFFFFFF:08X}:"
            f"{base45_encode(chunk)}"
        )
        for index, chunk in enumerate(chunks, 1)
    ]


def make_v2_part(
    report_id: str, part: int, total_parts: int,
    name: bytes, original: bytes, offset: int, size: int,
) -> tuple[bytes, list[str]]:
    version = b"test"
    data = original[offset : offset + size]
    raw = bytearray(b"TFDG\x02\x01\0\0")
    raw.extend(struct.pack(">IQQIIII", int(report_id, 16), 7, 123456789,
                           2, 0x06060110, part, total_parts))
    raw.append(len(version))
    raw.extend(version)
    segmented = int(offset != 0 or size != len(original))
    raw.extend((len(name), segmented, 0, 0))
    raw.extend(struct.pack(">IIII", len(original), offset, size,
                           zlib.crc32(data) & 0xFFFFFFFF))
    raw.extend(name)
    raw.extend(data)
    compressed = zlib.compress(raw, 1)
    compressed_crc = zlib.crc32(compressed) & 0xFFFFFFFF
    chunks = [compressed[at : at + 31] for at in range(0, len(compressed), 31)]
    pages = [
        (
            f"TFD2:{report_id}:{part + 1:08X}:{total_parts:08X}:"
            f"{index:02X}:{len(chunks):02X}:{compressed_crc:08X}:"
            f"{zlib.crc32(chunk) & 0xFFFFFFFF:08X}:{base45_encode(chunk)}"
        )
        for index, chunk in enumerate(chunks, 1)
    ]
    return bytes(raw), pages


def main() -> None:
    raw, report_id, expected_log = make_bundle()
    pages = make_pages(raw, report_id)
    rebuilt, rebuilt_id = DECODER.reassemble(list(reversed(pages)))
    assert rebuilt == raw
    assert rebuilt_id == report_id
    metadata, files = DECODER.parse_bundle(rebuilt, rebuilt_id)
    assert metadata["app_version"] == "test"
    assert metadata["release_sequence"] == 7
    assert metadata["psp_model"] == 2
    assert files == [
        ("tilefinch-last-error.txt", expected_log, len(expected_log), 0)
    ]

    name = b"tilefinch-validation.txt"
    original = bytes(range(256)) * 400
    split = 55_000
    v2_report = "89ABCDEF"
    raw0, pages0 = make_v2_part(
        v2_report, 0, 2, name, original, 0, split
    )
    raw1, pages1 = make_v2_part(
        v2_report, 1, 2, name, original, split, len(original) - split
    )
    part_metadata, part_files = DECODER.parse_bundle(raw0, v2_report)
    assert part_metadata["format"] == "TFDG/2"
    assert part_metadata["part_index"] == 0 and part_metadata["part_count"] == 2
    assert part_files == [
        ("tilefinch-validation.txt", original[:split], len(original), 0)
    ]
    complete_metadata, complete_files = DECODER.recover_pages(
        list(reversed(pages1)) + list(reversed(pages0))
    )
    assert complete_metadata["report_id"] == v2_report
    assert complete_files == [
        ("tilefinch-validation.txt", original, len(original), 0)
    ]
    try:
        DECODER.recover_pages(pages0)
    except DECODER.DiagnosticError as error:
        assert "part 2" in str(error)
    else:
        raise AssertionError("incomplete multipart report was accepted")

    missing = pages[:-1]
    try:
        DECODER.reassemble(missing)
    except DECODER.DiagnosticError as error:
        assert "missing QR page" in str(error)
    else:
        raise AssertionError("missing page was accepted")

    damaged = pages.copy()
    damaged[0] = damaged[0][:-1] + ("0" if damaged[0][-1] != "0" else "1")
    try:
        DECODER.reassemble(damaged)
    except DECODER.DiagnosticError as error:
        assert "checksum" in str(error) or "Base45" in str(error)
    else:
        raise AssertionError("damaged page was accepted")

    print("diagnostic QR decoder tests passed")


if __name__ == "__main__":
    main()
