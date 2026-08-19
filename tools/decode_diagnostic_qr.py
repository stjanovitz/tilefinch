#!/usr/bin/env python3
"""Reassemble Tilefinch diagnostic QR text and recover its original logs."""

from __future__ import annotations

import argparse
import binascii
import re
import struct
import sys
import zlib
from pathlib import Path


BASE45 = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:"
BASE45_VALUES = {character: value for value, character in enumerate(BASE45)}
PAGE_V1_PATTERN = re.compile(
    r"TFD1:([0-9A-F]{8}):([0-9A-F]{2}):([0-9A-F]{2}):"
    r"([0-9A-F]{8}):([0-9A-F]{8}):([0-9A-Z $%*+./:-]+)"
)
PAGE_V2_PATTERN = re.compile(
    r"TFD2:([0-9A-F]{8}):([0-9A-F]{8}):([0-9A-F]{8}):"
    r"([0-9A-F]{2}):([0-9A-F]{2}):([0-9A-F]{8}):"
    r"([0-9A-F]{8}):([0-9A-Z $%*+./:-]+)"
)
RAW_LIMIT = 128 * 1024
PAGE_LIMIT = 64


class DiagnosticError(ValueError):
    pass


def base45_decode(text: str) -> bytes:
    output = bytearray()
    at = 0
    while at + 2 < len(text):
        try:
            value = (
                BASE45_VALUES[text[at]]
                + 45 * BASE45_VALUES[text[at + 1]]
                + 45 * 45 * BASE45_VALUES[text[at + 2]]
            )
        except KeyError as error:
            raise DiagnosticError("page contains a non-Base45 character") from error
        if value > 0xFFFF:
            raise DiagnosticError("page contains an invalid Base45 triplet")
        output.extend((value >> 8, value & 0xFF))
        at += 3
    remaining = len(text) - at
    if remaining == 1:
        raise DiagnosticError("page ends with an incomplete Base45 pair")
    if remaining == 2:
        try:
            value = BASE45_VALUES[text[at]] + 45 * BASE45_VALUES[text[at + 1]]
        except KeyError as error:
            raise DiagnosticError("page contains a non-Base45 character") from error
        if value > 0xFF:
            raise DiagnosticError("page contains an invalid Base45 pair")
        output.append(value)
    return bytes(output)


def extract_page(value: str) -> str:
    upper = value.upper()
    match = PAGE_V2_PATTERN.search(upper)
    if match is None:
        match = PAGE_V1_PATTERN.search(upper)
    if match is None:
        raise DiagnosticError("input does not contain a Tilefinch diagnostic page")
    return match.group(0)


def read_input(value: str) -> str:
    path = Path(value)
    if path.is_file():
        return extract_page(path.read_text(encoding="utf-8", errors="replace"))
    return extract_page(value)


def reassemble(page_texts: list[str]) -> tuple[bytes, str]:
    pages: dict[int, bytes] = {}
    report_id = None
    page_total = None
    compressed_crc = None
    for text in page_texts:
        match = PAGE_V1_PATTERN.fullmatch(text)
        if match is None:
            raise DiagnosticError("malformed TFD1 page")
        current_report = match.group(1)
        page = int(match.group(2), 16)
        total = int(match.group(3), 16)
        current_compressed_crc = int(match.group(4), 16)
        chunk_crc = int(match.group(5), 16)
        chunk = base45_decode(match.group(6))
        if (total == 0 or total > PAGE_LIMIT or page == 0 or page > total
                or len(chunk) > 960):
            raise DiagnosticError("page numbering is outside Tilefinch's bounds")
        if (zlib.crc32(chunk) & 0xFFFFFFFF) != chunk_crc:
            raise DiagnosticError(f"page {page} failed its chunk checksum")
        if report_id is None:
            report_id = current_report
            page_total = total
            compressed_crc = current_compressed_crc
        elif (
            report_id != current_report
            or page_total != total
            or compressed_crc != current_compressed_crc
        ):
            raise DiagnosticError("pages belong to different diagnostic reports")
        previous = pages.get(page)
        if previous is not None and previous != chunk:
            raise DiagnosticError(f"page {page} was supplied with conflicting data")
        pages[page] = chunk
    if report_id is None or page_total is None or compressed_crc is None:
        raise DiagnosticError("no diagnostic pages supplied")
    missing = [str(page) for page in range(1, page_total + 1) if page not in pages]
    if missing:
        raise DiagnosticError("missing QR page(s): " + ", ".join(missing))
    compressed = b"".join(pages[page] for page in range(1, page_total + 1))
    if (zlib.crc32(compressed) & 0xFFFFFFFF) != compressed_crc:
        raise DiagnosticError("the complete compressed report failed its checksum")
    decompressor = zlib.decompressobj()
    raw = decompressor.decompress(compressed, RAW_LIMIT + 1)
    if len(raw) > RAW_LIMIT or decompressor.unconsumed_tail:
        raise DiagnosticError("diagnostic expands beyond the 128 KiB format bound")
    raw += decompressor.flush()
    if len(raw) > RAW_LIMIT or not decompressor.eof or decompressor.unused_data:
        raise DiagnosticError("diagnostic compressed stream is malformed")
    return raw, report_id


def reassemble_v2_parts(page_texts: list[str]) -> tuple[list[bytes], str]:
    groups: dict[tuple[int, int], dict[str, object]] = {}
    report_id: str | None = None
    part_total: int | None = None
    for text in page_texts:
        match = PAGE_V2_PATTERN.fullmatch(text)
        if match is None:
            raise DiagnosticError("mixed or malformed TFD2 diagnostic page")
        current_report = match.group(1)
        part = int(match.group(2), 16)
        total_parts = int(match.group(3), 16)
        page = int(match.group(4), 16)
        total_pages = int(match.group(5), 16)
        compressed_crc = int(match.group(6), 16)
        chunk_crc = int(match.group(7), 16)
        chunk = base45_decode(match.group(8))
        if (
            part == 0
            or total_parts == 0
            or part > total_parts
            or page == 0
            or total_pages == 0
            or total_pages > PAGE_LIMIT
            or page > total_pages
            or len(chunk) > 960
        ):
            raise DiagnosticError("part or page numbering is outside Tilefinch's bounds")
        if (zlib.crc32(chunk) & 0xFFFFFFFF) != chunk_crc:
            raise DiagnosticError(f"part {part}, page {page} failed its checksum")
        if report_id is None:
            report_id, part_total = current_report, total_parts
        elif report_id != current_report or part_total != total_parts:
            raise DiagnosticError("pages belong to different diagnostic reports")
        key = (part, compressed_crc)
        group = groups.setdefault(
            key, {"total": total_pages, "pages": {}}
        )
        if group["total"] != total_pages:
            raise DiagnosticError("a report part has conflicting page totals")
        pages = group["pages"]
        assert isinstance(pages, dict)
        previous = pages.get(page)
        if previous is not None and previous != chunk:
            raise DiagnosticError("a QR page was supplied with conflicting data")
        pages[page] = chunk
    if report_id is None or part_total is None:
        raise DiagnosticError("no diagnostic pages supplied")
    raws: list[bytes] = []
    for part in range(1, part_total + 1):
        matching = [(key, value) for key, value in groups.items() if key[0] == part]
        if len(matching) != 1:
            raise DiagnosticError(f"missing or conflicting report part {part}")
        (key, group), = matching
        compressed_crc = key[1]
        total_pages = group["total"]
        pages = group["pages"]
        assert isinstance(total_pages, int) and isinstance(pages, dict)
        missing = [str(page) for page in range(1, total_pages + 1)
                   if page not in pages]
        if missing:
            raise DiagnosticError(
                f"part {part} missing QR page(s): " + ", ".join(missing)
            )
        compressed = b"".join(pages[page] for page in range(1, total_pages + 1))
        if (zlib.crc32(compressed) & 0xFFFFFFFF) != compressed_crc:
            raise DiagnosticError(f"part {part} failed its compressed checksum")
        decompressor = zlib.decompressobj()
        raw = decompressor.decompress(compressed, RAW_LIMIT + 1)
        if len(raw) > RAW_LIMIT or decompressor.unconsumed_tail:
            raise DiagnosticError(f"part {part} expands beyond the format bound")
        raw += decompressor.flush()
        if (
            len(raw) > RAW_LIMIT
            or not decompressor.eof
            or decompressor.unused_data
            or decompressor.unconsumed_tail
        ):
            raise DiagnosticError(f"part {part} compressed stream is malformed")
        raws.append(raw)
    return raws, report_id


def take(raw: bytes, offset: int, length: int) -> tuple[bytes, int]:
    if length < 0 or offset < 0 or offset > len(raw) - length:
        raise DiagnosticError("diagnostic bundle is truncated")
    return raw[offset : offset + length], offset + length


def parse_bundle(
    raw: bytes, expected_report_id: str
) -> tuple[dict[str, object], list[tuple[str, bytes, int, int]]]:
    if len(raw) < 37 or raw[:4] != b"TFDG" or raw[4] not in (1, 2):
        raise DiagnosticError("unsupported diagnostic bundle format")
    format_version = raw[4]
    file_count = raw[5]
    if (
        file_count == 0
        or file_count > 5
        or raw[6:8] != b"\0\0"
    ):
        raise DiagnosticError("diagnostic bundle header is invalid")
    stored_crc = struct.unpack_from(">I", raw, 8)[0]
    if f"{stored_crc:08X}" != expected_report_id:
        raise DiagnosticError("diagnostic report identifier failed verification")
    if format_version == 1:
        check = bytearray(raw)
        check[8:12] = b"\0\0\0\0"
        if stored_crc != (zlib.crc32(check) & 0xFFFFFFFF):
            raise DiagnosticError("diagnostic report identifier failed verification")
    release_sequence, created_time, model, firmware = struct.unpack_from(">QQII", raw, 12)
    if format_version == 1:
        part_index, part_count = 0, 1
        version_length = raw[36]
        version_bytes, offset = take(raw, 37, version_length)
    else:
        if len(raw) < 45 or file_count != 1:
            raise DiagnosticError("diagnostic part header is invalid")
        part_index, part_count = struct.unpack_from(">II", raw, 36)
        if part_count == 0 or part_index >= part_count:
            raise DiagnosticError("diagnostic part index is invalid")
        version_length = raw[44]
        version_bytes, offset = take(raw, 45, version_length)
    try:
        version = version_bytes.decode("utf-8")
    except UnicodeDecodeError as error:
        raise DiagnosticError("app version is not valid UTF-8") from error
    files: list[tuple[str, bytes, int, int]] = []
    seen: set[str] = set()
    for _ in range(file_count):
        if format_version == 1:
            header, offset = take(raw, offset, 9)
            name_length = header[0]
            size, checksum = struct.unpack_from(">II", header, 1)
            original_size, data_offset = size, 0
        else:
            header, offset = take(raw, offset, 20)
            name_length, flags = header[0], header[1]
            if flags & ~1 or header[2:4] != b"\0\0":
                raise DiagnosticError("diagnostic log entry flags are invalid")
            original_size, data_offset, size, checksum = struct.unpack_from(
                ">IIII", header, 4
            )
            if data_offset > original_size or size > original_size - data_offset:
                raise DiagnosticError("diagnostic log subset is outside the original file")
            is_segment = data_offset != 0 or size != original_size
            if bool(flags & 1) != is_segment:
                raise DiagnosticError("diagnostic log segment marker is inconsistent")
        name_bytes, offset = take(raw, offset, name_length)
        data, offset = take(raw, offset, size)
        try:
            name = name_bytes.decode("utf-8")
        except UnicodeDecodeError as error:
            raise DiagnosticError("log name is not valid UTF-8") from error
        if (
            not name
            or name in seen
            or Path(name).name != name
            or "/" in name
            or "\\" in name
        ):
            raise DiagnosticError("diagnostic contains an unsafe or duplicate log name")
        if (zlib.crc32(data) & 0xFFFFFFFF) != checksum:
            raise DiagnosticError(f"{name} failed its checksum")
        seen.add(name)
        files.append((name, data, original_size, data_offset))
    if offset != len(raw):
        raise DiagnosticError("diagnostic bundle has trailing bytes")
    metadata: dict[str, object] = {
        "format": f"TFDG/{format_version}",
        "report_id": expected_report_id,
        "app_version": version,
        "release_sequence": release_sequence,
        "created_unix_time": created_time,
        "psp_model": model,
        "psp_firmware": f"0x{firmware:08X}",
        "part_index": part_index,
        "part_count": part_count,
    }
    return metadata, files


def recover_pages(
    page_texts: list[str],
) -> tuple[dict[str, object], list[tuple[str, bytes, int, int]]]:
    if not page_texts:
        raise DiagnosticError("no diagnostic pages supplied")
    if all(text.startswith("TFD1:") for text in page_texts):
        raw, report_id = reassemble(page_texts)
        return parse_bundle(raw, report_id)
    if not all(text.startswith("TFD2:") for text in page_texts):
        raise DiagnosticError("TFD1 and TFD2 reports cannot be mixed")
    raws, report_id = reassemble_v2_parts(page_texts)
    first_metadata: dict[str, object] | None = None
    segments: dict[str, tuple[int, list[tuple[int, bytes]]]] = {}
    for expected_part, raw in enumerate(raws):
        metadata, files = parse_bundle(raw, report_id)
        if metadata["part_index"] != expected_part or metadata["part_count"] != len(raws):
            raise DiagnosticError("bundle part metadata disagrees with its QR envelope")
        if first_metadata is None:
            first_metadata = metadata
        else:
            for key in (
                "app_version", "release_sequence", "created_unix_time",
                "psp_model", "psp_firmware", "part_count",
            ):
                if metadata[key] != first_metadata[key]:
                    raise DiagnosticError("report metadata changes between parts")
        for name, data, original_size, offset in files:
            known = segments.get(name)
            if known is None:
                segments[name] = (original_size, [(offset, data)])
            else:
                if known[0] != original_size:
                    raise DiagnosticError(f"{name} changes size between parts")
                known[1].append((offset, data))
    assert first_metadata is not None
    complete: list[tuple[str, bytes, int, int]] = []
    for name, (original_size, pieces) in segments.items():
        pieces.sort(key=lambda value: value[0])
        expected_offset = 0
        output = bytearray()
        for offset, data in pieces:
            if offset != expected_offset:
                raise DiagnosticError(f"{name} has a missing or overlapping segment")
            output.extend(data)
            expected_offset += len(data)
        if expected_offset != original_size:
            raise DiagnosticError(f"{name} is incomplete")
        complete.append((name, bytes(output), original_size, 0))
    return first_metadata, complete


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Recover Tilefinch logs from QR-decoder text."
    )
    parser.add_argument(
        "inputs", nargs="+", help="TFD1/TFD2 text or files containing decoded QR text"
    )
    parser.add_argument(
        "--output", type=Path, default=Path("tilefinch-diagnostic"),
        help="directory for recovered logs (default: tilefinch-diagnostic)",
    )
    arguments = parser.parse_args()
    try:
        metadata, files = recover_pages([read_input(value) for value in arguments.inputs])
        arguments.output.mkdir(parents=True, exist_ok=True)
        for name, data, _original_size, _offset in files:
            (arguments.output / name).write_bytes(data)
    except (DiagnosticError, OSError, binascii.Error) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    print(f"Recovered report {metadata['report_id']} from Tilefinch {metadata['app_version']}")
    print(
        f"Release sequence {metadata['release_sequence']}; "
        f"PSP model {metadata['psp_model']}; firmware {metadata['psp_firmware']}"
    )
    for name, data, original_size, offset in files:
        if offset == 0 and len(data) == original_size:
            print(f"  {name}: {len(data)} bytes")
        else:
            print(
                f"  {name}: {len(data)} recent bytes "
                f"(offset {offset} of {original_size})"
            )
    print(f"Logs written to {arguments.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
