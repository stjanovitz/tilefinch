#!/usr/bin/env python3
"""Small, dependency-free helpers for acceptance reference frames."""

from __future__ import annotations

import struct
import zlib
from pathlib import Path


PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
JPEG_SIGNATURE = b"\xff\xd8\xff"


def raster_format(path: Path) -> str:
    """Return the format identified by file contents, never by its suffix."""

    with path.open("rb") as source:
        prefix = source.read(16)
    if prefix.startswith(PNG_SIGNATURE):
        return "png"
    if prefix.startswith(JPEG_SIGNATURE):
        return "jpeg"
    if prefix.startswith(b"P6") and (
        len(prefix) == 2 or prefix[2:3].isspace() or prefix[2:3] == b"#"
    ):
        return "ppm"
    return "unknown"


def declared_raster_format(path: Path) -> str | None:
    suffix = path.suffix.lower()
    if suffix == ".png":
        return "png"
    if suffix in (".jpg", ".jpeg"):
        return "jpeg"
    if suffix == ".ppm":
        return "ppm"
    return None


def validate_declared_format(
    path: Path, *, accepted: tuple[str, ...], allow_mismatch: bool = False
) -> str:
    actual = raster_format(path)
    declared = declared_raster_format(path)
    if actual == "unknown":
        raise ValueError(f"{path}: unrecognized raster file contents")
    if declared is not None and declared != actual and not allow_mismatch:
        raise ValueError(
            f"{path}: extension declares {declared.upper()} but contents are "
            f"{actual.upper()}; normalize or rename the reference explicitly"
        )
    if actual not in accepted:
        expected = "/".join(item.upper() for item in accepted)
        raise ValueError(
            f"{path}: canonical reference must be {expected}, detected "
            f"{actual.upper()}; run benchmarks/normalize-reference-frame.py"
        )
    return actual


def read_ppm(path: Path, *, allow_mismatch: bool = False) -> tuple[int, int, bytes]:
    validate_declared_format(
        path, accepted=("ppm",), allow_mismatch=allow_mismatch
    )
    data = path.read_bytes()
    at = 2

    def token() -> bytes:
        nonlocal at
        while at < len(data):
            if data[at] == ord("#"):
                end = data.find(b"\n", at)
                if end < 0:
                    raise ValueError(f"{path}: unterminated PPM comment")
                at = end + 1
            elif chr(data[at]).isspace():
                at += 1
            else:
                break
        start = at
        while at < len(data) and not chr(data[at]).isspace():
            at += 1
        if start == at:
            raise ValueError(f"{path}: truncated PPM header")
        return data[start:at]

    width, height, maximum = (int(token()) for _ in range(3))
    if maximum != 255 or width <= 0 or height <= 0:
        raise ValueError(f"{path}: unsupported PPM geometry or sample range")
    if at >= len(data) or not chr(data[at]).isspace():
        raise ValueError(f"{path}: missing PPM pixel separator")
    at += 1
    pixels = data[at:]
    if len(pixels) != width * height * 3:
        raise ValueError(f"{path}: PPM pixel length does not match header")
    return width, height, pixels


def paeth(left: int, above: int, upper_left: int) -> int:
    estimate = left + above - upper_left
    left_distance = abs(estimate - left)
    above_distance = abs(estimate - above)
    upper_left_distance = abs(estimate - upper_left)
    if left_distance <= above_distance and left_distance <= upper_left_distance:
        return left
    if above_distance <= upper_left_distance:
        return above
    return upper_left


def read_png_bytes(
    data: bytes,
    *,
    label: str = "PNG",
    expected_width: int | None = None,
    expected_height: int | None = None,
    required_color: int | None = None,
    max_compressed_bytes: int | None = None,
    max_decompressed_bytes: int | None = None,
) -> tuple[int, int, bytes]:
    """Decode one bounded PNG byte string without unbounded zlib expansion.

    Callers that use a PNG as trust evidence can bind and hash ``data`` first,
    then require its expected IHDR geometry before this function admits any
    compressed payload to zlib.  The decompressor is capped at exactly one byte
    beyond the IHDR-derived raster length so a small compressed stream cannot
    allocate an attacker-selected amount of memory.
    """

    if not data.startswith(PNG_SIGNATURE):
        raise ValueError(f"{label}: invalid PNG signature")
    if expected_width is not None and expected_width <= 0:
        raise ValueError(f"{label}: invalid expected PNG width")
    if expected_height is not None and expected_height <= 0:
        raise ValueError(f"{label}: invalid expected PNG height")
    if max_compressed_bytes is not None and max_compressed_bytes < 0:
        raise ValueError(f"{label}: invalid compressed PNG byte bound")
    if max_decompressed_bytes is not None and max_decompressed_bytes < 0:
        raise ValueError(f"{label}: invalid decompressed PNG byte bound")

    at = len(PNG_SIGNATURE)
    header: tuple[int, int, int, int, int] | None = None
    compressed = bytearray()
    saw_data = False
    ended_data = False
    saw_end = False
    while at < len(data):
        if at + 12 > len(data):
            raise ValueError(f"{label}: truncated PNG chunk")
        length = struct.unpack(">I", data[at : at + 4])[0]
        kind = data[at + 4 : at + 8]
        payload_start = at + 8
        payload_end = payload_start + length
        if payload_end + 4 > len(data):
            raise ValueError(f"{label}: truncated PNG chunk")
        payload = data[payload_start:payload_end]
        expected_crc = struct.unpack(">I", data[payload_end : payload_end + 4])[0]
        if zlib.crc32(kind + payload) & 0xFFFFFFFF != expected_crc:
            raise ValueError(f"{label}: corrupt PNG chunk")
        if kind == b"IHDR":
            if header is not None or at != len(PNG_SIGNATURE) or length != 13:
                raise ValueError(f"{label}: invalid PNG header")
            width, height, depth, color, compression, filtering, interlace = (
                struct.unpack(">IIBBBBB", payload)
            )
            if compression != 0 or filtering != 0 or interlace != 0:
                raise ValueError(
                    f"{label}: interlaced or nonstandard PNG unsupported"
                )
            channels = {0: 1, 2: 3, 4: 2, 6: 4}.get(color)
            if depth != 8 or channels is None or width <= 0 or height <= 0:
                raise ValueError(
                    f"{label}: only 8-bit gray/RGB/RGBA PNG is supported"
                )
            if required_color is not None and color != required_color:
                raise ValueError(f"{label}: PNG color type differs from its contract")
            if (
                expected_width is not None
                and width != expected_width
                or expected_height is not None
                and height != expected_height
            ):
                raise ValueError(f"{label}: PNG geometry differs from its contract")
            header = (width, height, depth, color, interlace)
        elif kind == b"IDAT":
            if header is None or ended_data:
                raise ValueError(f"{label}: invalid PNG data chunk ordering")
            if (
                max_compressed_bytes is not None
                and len(compressed) > max_compressed_bytes - len(payload)
            ):
                raise ValueError(f"{label}: compressed PNG data exceeds its bound")
            compressed.extend(payload)
            saw_data = True
        elif kind == b"IEND":
            if header is None or not saw_data or length != 0:
                raise ValueError(f"{label}: invalid PNG end chunk")
            saw_end = True
            at = payload_end + 4
            break
        elif header is None:
            raise ValueError(f"{label}: PNG header must be the first chunk")
        elif saw_data:
            ended_data = True
        at = payload_end + 4
    if header is None or not saw_end or at != len(data):
        raise ValueError(f"{label}: incomplete PNG or trailing data")
    width, height, depth, color, _ = header
    channels = {0: 1, 2: 3, 4: 2, 6: 4}.get(color)
    assert depth == 8 and channels is not None
    row_bytes = width * channels
    raw_bytes = height * (row_bytes + 1)
    if max_decompressed_bytes is not None and raw_bytes > max_decompressed_bytes:
        raise ValueError(f"{label}: decompressed PNG data exceeds its bound")
    try:
        decompressor = zlib.decompressobj()
        raw = decompressor.decompress(bytes(compressed), raw_bytes + 1)
        if len(raw) > raw_bytes or decompressor.unconsumed_tail:
            raise ValueError(f"{label}: decompressed PNG length exceeds its header")
        raw += decompressor.flush(raw_bytes + 1 - len(raw))
    except zlib.error as error:
        raise ValueError(f"{label}: invalid compressed PNG data") from error
    if (
        len(raw) != raw_bytes
        or not decompressor.eof
        or decompressor.unused_data
        or decompressor.unconsumed_tail
    ):
        raise ValueError(f"{label}: decompressed PNG length is invalid")
    previous = bytearray(row_bytes)
    rgb = bytearray(width * height * 3)
    source_at = 0
    output_at = 0
    for _ in range(height):
        filter_kind = raw[source_at]
        source_at += 1
        encoded = raw[source_at : source_at + row_bytes]
        source_at += row_bytes
        row = bytearray(row_bytes)
        for index, value in enumerate(encoded):
            left = row[index - channels] if index >= channels else 0
            above = previous[index]
            upper_left = previous[index - channels] if index >= channels else 0
            if filter_kind == 0:
                prediction = 0
            elif filter_kind == 1:
                prediction = left
            elif filter_kind == 2:
                prediction = above
            elif filter_kind == 3:
                prediction = (left + above) // 2
            elif filter_kind == 4:
                prediction = paeth(left, above, upper_left)
            else:
                raise ValueError(f"{label}: unknown PNG row filter")
            row[index] = (value + prediction) & 0xFF
        for x in range(width):
            offset = x * channels
            if color == 0:
                red = green = blue = row[offset]
                alpha = 255
            elif color == 2:
                red, green, blue = row[offset : offset + 3]
                alpha = 255
            elif color == 4:
                red = green = blue = row[offset]
                alpha = row[offset + 1]
            else:
                red, green, blue, alpha = row[offset : offset + 4]
            inverse = 255 - alpha
            rgb[output_at] = (red * alpha + 255 * inverse + 127) // 255
            rgb[output_at + 1] = (green * alpha + 255 * inverse + 127) // 255
            rgb[output_at + 2] = (blue * alpha + 255 * inverse + 127) // 255
            output_at += 3
        previous = row
    return width, height, bytes(rgb)


def read_png(
    path: Path,
    *,
    allow_mismatch: bool = False,
    expected_width: int | None = None,
    expected_height: int | None = None,
    required_color: int | None = None,
    max_compressed_bytes: int | None = None,
    max_decompressed_bytes: int | None = None,
) -> tuple[int, int, bytes]:
    validate_declared_format(
        path, accepted=("png",), allow_mismatch=allow_mismatch
    )
    return read_png_bytes(
        path.read_bytes(),
        label=str(path),
        expected_width=expected_width,
        expected_height=expected_height,
        required_color=required_color,
        max_compressed_bytes=max_compressed_bytes,
        max_decompressed_bytes=max_decompressed_bytes,
    )


def write_png_rgb(path: Path, width: int, height: int, pixels: bytes) -> None:
    if width <= 0 or height <= 0 or len(pixels) != width * height * 3:
        raise ValueError("invalid RGB frame geometry")

    def chunk(kind: bytes, payload: bytes) -> bytes:
        checksum = zlib.crc32(kind + payload) & 0xFFFFFFFF
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", checksum)

    rows = bytearray()
    row_bytes = width * 3
    for y in range(height):
        rows.append(0)
        start = y * row_bytes
        rows.extend(pixels[start : start + row_bytes])
    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    path.write_bytes(
        PNG_SIGNATURE
        + chunk(b"IHDR", header)
        + chunk(b"IDAT", zlib.compress(bytes(rows), 9))
        + chunk(b"IEND", b"")
    )
