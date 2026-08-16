#!/usr/bin/env python3
"""Compile exact PocketSphinx dictionary-to-senone mappings for Tilefinch.

The output contains only lookup results that PocketSphinx 5.1.1 normally
derives from the large CD-phone tree at startup.  It does not alter the
dictionary, language model, acoustic scores, search beams, or decoder passes.
"""

from __future__ import annotations

import argparse
import pathlib
import struct
import sys
from dataclasses import dataclass


BAD_SSID = 0xFFFF
MAGIC = b"TFD2P\r\n\0"
VERSION = 1
WORD_INTERNAL = 0
WORD_BEGIN = 1
WORD_END = 2
WORD_SINGLE = 3
WORD_POSITIONS = 4
FNV_OFFSET = 1_469_598_103_934_665_603
FNV_PRIME = 1_099_511_628_211
MASK64 = (1 << 64) - 1


@dataclass(frozen=True)
class Phone:
    ssid: int
    tmat: int
    info: bytes


@dataclass
class Model:
    byte_order: str
    n_ci: int
    n_phone: int
    n_emit: int
    n_sseq: int
    silence: int
    names: list[str]
    tree: list[tuple[int, int, int]]
    phones: list[Phone]
    sseq: list[int]

    def phone_id(self, base: int, left: int, right: int, position: int) -> int:
        if left < 0 or right < 0:
            return base
        left = self.silence if self.phones[left].info[0] else left
        right = self.silence if self.phones[right].info[0] else right
        context = (position, base, left, right)
        start = 0
        count = WORD_POSITIONS
        for value in context:
            for index in range(start, start + count):
                node_context, child_count, child = self.tree[index]
                if node_context == value:
                    break
            else:
                return -1
            if child_count == 0:
                return child
            start = child
            count = child_count
        return -1

    def nearest(self, base: int, left: int, right: int, position: int) -> int:
        if left < 0 or right < 0:
            return base
        found = self.phone_id(base, left, right, position)
        if found >= 0:
            return found
        for alternate in range(WORD_POSITIONS):
            if alternate != position:
                found = self.phone_id(base, left, right, alternate)
                if found >= 0:
                    return found
        new_left = left
        new_right = right
        if self.silence >= 0:
            if self.phones[left].info[0] or position in (WORD_BEGIN, WORD_SINGLE):
                new_left = self.silence
            if self.phones[right].info[0] or position in (WORD_END, WORD_SINGLE):
                new_right = self.silence
        if (new_left, new_right) != (left, right):
            found = self.phone_id(base, new_left, new_right, position)
            if found >= 0:
                return found
            for alternate in range(WORD_POSITIONS):
                if alternate != position:
                    found = self.phone_id(
                        base, new_left, new_right, alternate
                    )
                    if found >= 0:
                        return found
        return base

    def ssid_for(self, phone_id: int) -> int:
        return self.phones[phone_id].ssid


def read_i32(stream, order: str) -> int:
    data = stream.read(4)
    if len(data) != 4:
        raise ValueError("truncated 32-bit model field")
    return struct.unpack(order + "i", data)[0]


def read_sendump_geometry(path: pathlib.Path) -> tuple[int, int]:
    """Return (row_count, row_bytes) from a PocketSphinx sendump."""
    with path.open("rb") as stream:
        marker = stream.read(4)
        if len(marker) != 4:
            raise ValueError(f"{path}: truncated sendump")
        order = ""
        title_bytes = 0
        for candidate in ("<", ">"):
            value = struct.unpack(candidate + "i", marker)[0]
            if 0 < value < 1000:
                order = candidate
                title_bytes = value
                break
        if not order:
            raise ValueError(f"{path}: invalid sendump byte order")
        if len(stream.read(title_bytes)) != title_bytes:
            raise ValueError(f"{path}: truncated sendump title")
        header_bytes = read_i32(stream, order)
        if not 0 < header_bytes < 1000:
            raise ValueError(f"{path}: invalid sendump header size")
        if len(stream.read(header_bytes)) != header_bytes:
            raise ValueError(f"{path}: truncated sendump header")

        feature_count = 1
        cluster_count = 0
        cluster_bits = 8
        while True:
            field_bytes = read_i32(stream, order)
            if field_bytes == 0:
                break
            if not 0 < field_bytes < 1000:
                raise ValueError(f"{path}: invalid sendump field size")
            field = stream.read(field_bytes)
            if len(field) != field_bytes:
                raise ValueError(f"{path}: truncated sendump field")
            text = field.rstrip(b"\0").decode("ascii", errors="strict")
            name, separator, value = text.partition(" ")
            if not separator:
                continue
            if name == "feature_count":
                feature_count = int(value)
            elif name == "cluster_count":
                cluster_count = int(value)
            elif name == "cluster_bits":
                cluster_bits = int(value)

        if cluster_count != 0 or cluster_bits != 8:
            raise ValueError(
                f"{path}: expected an unclustered eight-bit fixed sendump"
            )
        density_count = read_i32(stream, order)
        row_bytes = read_i32(stream, order)
        if feature_count <= 0 or density_count <= 0 or row_bytes <= 0:
            raise ValueError(f"{path}: invalid sendump geometry")
        return feature_count * density_count, row_bytes


def read_model(path: pathlib.Path) -> Model:
    with path.open("rb") as stream:
        marker = stream.read(4)
        if marker == b"BMDF":
            order = "<"
        elif marker == b"FDMB":
            order = ">"
        else:
            raise ValueError(f"{path}: invalid binary mdef marker")
        version = read_i32(stream, order)
        if version > 1:
            raise ValueError(f"{path}: unsupported mdef version {version}")
        descriptor_bytes = read_i32(stream, order)
        if descriptor_bytes < 0:
            raise ValueError(f"{path}: invalid descriptor size")
        stream.seek(descriptor_bytes, 1)
        (
            n_ci,
            n_phone,
            n_emit,
            _n_ci_sen,
            _n_sen,
            _n_tmat,
            n_sseq,
            _n_context,
            n_tree,
            silence,
        ) = (read_i32(stream, order) for _ in range(10))
        if not (0 < n_ci <= 255 and n_phone >= n_ci and n_emit == 3):
            raise ValueError(f"{path}: expected a homogeneous three-state model")

        names = []
        for _ in range(n_ci):
            chars = bytearray()
            while True:
                char = stream.read(1)
                if not char:
                    raise ValueError(f"{path}: truncated CI-phone names")
                if char == b"\0":
                    break
                chars += char
            names.append(chars.decode("ascii"))
        stream.seek((-stream.tell()) & 3, 1)

        tree = []
        tree_record = struct.Struct(order + "hhi")
        for _ in range(n_tree):
            data = stream.read(tree_record.size)
            if len(data) != tree_record.size:
                raise ValueError(f"{path}: truncated CD tree")
            tree.append(tree_record.unpack(data))

        phones = []
        phone_record = struct.Struct(order + "ii4s")
        for _ in range(n_phone):
            data = stream.read(phone_record.size)
            if len(data) != phone_record.size:
                raise ValueError(f"{path}: truncated phone table")
            ssid, tmat, info = phone_record.unpack(data)
            phones.append(Phone(ssid, tmat, info))

        sequence_values = read_i32(stream, order)
        expected_values = n_sseq * n_emit
        if sequence_values != expected_values:
            raise ValueError(
                f"{path}: senone sequence count {sequence_values}, "
                f"expected {expected_values}"
            )
        data = stream.read(sequence_values * 2)
        if len(data) != sequence_values * 2:
            raise ValueError(f"{path}: truncated senone sequences")
        sseq = list(struct.unpack(order + f"{sequence_values}H", data))

    if silence < 0:
        try:
            silence = names.index("SIL")
        except ValueError as error:
            raise ValueError(f"{path}: silence phone is absent") from error
    return Model(
        order,
        n_ci,
        n_phone,
        n_emit,
        n_sseq,
        silence,
        names,
        tree,
        phones,
        sseq,
    )


def read_dictionary_file(
    path: pathlib.Path,
    phone_ids: dict[str, int],
    words: list[tuple[str, tuple[int, ...]]],
    known_words: set[str],
) -> None:
    with path.open("r", encoding="utf-8") as stream:
        for number, line in enumerate(stream, 1):
            if line.startswith(("##", ";;")):
                continue
            fields = line.split()
            if not fields:
                continue
            if len(fields) < 2:
                continue
            spelling = fields[0]
            if spelling in known_words:
                continue
            try:
                pronunciation = tuple(phone_ids[phone] for phone in fields[1:])
            except KeyError as error:
                raise ValueError(
                    f"{path}:{number}: unknown phone {error.args[0]}"
                ) from error
            words.append((spelling, pronunciation))
            known_words.add(spelling)


def read_dictionary(
    primary: pathlib.Path, filler: pathlib.Path, model: Model
) -> list[tuple[str, tuple[int, ...]]]:
    phone_ids = {name: index for index, name in enumerate(model.names)}
    words: list[tuple[str, tuple[int, ...]]] = []
    known_words: set[str] = set()
    read_dictionary_file(primary, phone_ids, words, known_words)
    forbidden = {"<s>", "</s>", "<sil>"} & known_words
    if forbidden:
        raise ValueError(
            f"{primary}: sentence/silence words belong in the filler dictionary"
        )
    read_dictionary_file(filler, phone_ids, words, known_words)
    for spelling in ("<s>", "</s>", "<sil>"):
        if spelling not in known_words:
            words.append((spelling, (model.silence,)))
            known_words.add(spelling)
    return words


def fingerprint(words: list[tuple[str, tuple[int, ...]]]) -> int:
    value = FNV_OFFSET
    for spelling, pronunciation in words:
        for byte in spelling.encode("utf-8"):
            value = ((value ^ byte) * FNV_PRIME) & MASK64
        value = ((value ^ 0xFF) * FNV_PRIME) & MASK64
        value = ((value ^ len(pronunciation)) * FNV_PRIME) & MASK64
        for phone in pronunciation:
            value = ((value ^ phone) * FNV_PRIME) & MASK64
    return value


def cube_index(n_ci: int, base: int, right: int, left: int) -> int:
    return (base * n_ci + right) * n_ci + left


def compile_mappings(
    model: Model, words: list[tuple[str, tuple[int, ...]]]
) -> tuple[list[int], list[tuple[list[int], list[int]]], list[int], list[int]]:
    cube_size = model.n_ci**3
    left_diphone = [BAD_SSID] * cube_size
    right_diphone = [BAD_SSID] * cube_size
    seen_left: set[tuple[int, int]] = set()
    seen_right: set[tuple[int, int]] = set()
    seen_single: set[int] = set()

    for _spelling, pronunciation in words:
        if len(pronunciation) >= 2:
            base, right = pronunciation[0], pronunciation[1]
            if (base, right) not in seen_left:
                seen_left.add((base, right))
                for left in range(model.n_ci):
                    phone_id = model.nearest(
                        base, left, right, WORD_BEGIN
                    )
                    left_diphone[
                        cube_index(model.n_ci, base, right, left)
                    ] = model.ssid_for(phone_id)
            left, base = pronunciation[-2], pronunciation[-1]
            if (base, left) not in seen_right:
                seen_right.add((base, left))
                for right in range(model.n_ci):
                    phone_id = model.nearest(
                        base, left, right, WORD_END
                    )
                    right_diphone[
                        cube_index(model.n_ci, base, left, right)
                    ] = model.ssid_for(phone_id)
        elif len(pronunciation) == 1:
            base = pronunciation[0]
            if base in seen_single:
                continue
            seen_single.add(base)
            for left in range(model.n_ci):
                for right in range(model.n_ci):
                    phone_id = model.nearest(
                        base, left, right, WORD_SINGLE
                    )
                    ssid = model.ssid_for(phone_id)
                    if right == model.silence:
                        left_diphone[
                            cube_index(model.n_ci, base, right, left)
                        ] = ssid
                    if left == model.silence:
                        right_diphone[
                            cube_index(model.n_ci, base, left, right)
                        ] = ssid

    compressed_right: list[tuple[list[int], list[int]]] = []
    for base in range(model.n_ci):
        for left in range(model.n_ci):
            unique: list[int] = []
            indexes: dict[int, int] = {}
            context_map = []
            for right in range(model.n_ci):
                ssid = right_diphone[
                    cube_index(model.n_ci, base, left, right)
                ]
                if ssid not in indexes:
                    indexes[ssid] = len(unique)
                    unique.append(ssid)
                context_map.append(indexes[ssid])
            if unique == [BAD_SSID]:
                compressed_right.append(([], []))
            else:
                compressed_right.append((unique, context_map))

    offsets = [0]
    internal = []
    for _spelling, pronunciation in words:
        for position, base in enumerate(pronunciation):
            if 0 < position < len(pronunciation) - 1:
                phone_id = model.nearest(
                    base,
                    pronunciation[position - 1],
                    pronunciation[position + 1],
                    WORD_INTERNAL,
                )
                internal.append(model.ssid_for(phone_id))
            else:
                internal.append(BAD_SSID)
        offsets.append(len(internal))
    return left_diphone, compressed_right, offsets, internal


def write_output(
    path: pathlib.Path,
    model: Model,
    words: list[tuple[str, tuple[int, ...]]],
    left_diphone: list[int],
    compressed_right: list[tuple[list[int], list[int]]],
    offsets: list[int],
    internal: list[int],
) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    with temporary.open("wb") as stream:
        stream.write(MAGIC)
        stream.write(
            struct.pack(
                "<IIIIQ",
                VERSION,
                model.n_ci,
                len(words),
                len(internal),
                fingerprint(words),
            )
        )
        stream.write(struct.pack(f"<{len(left_diphone)}H", *left_diphone))
        for unique, context_map in compressed_right:
            stream.write(struct.pack("<B", len(unique)))
            if unique:
                stream.write(struct.pack(f"<{len(unique)}H", *unique))
                stream.write(
                    struct.pack(f"<{len(context_map)}h", *context_map)
                )
        stream.write(struct.pack(f"<{len(offsets)}i", *offsets))
        stream.write(struct.pack(f"<{len(internal)}H", *internal))
    temporary.replace(path)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--mdef", required=True, type=pathlib.Path)
    parser.add_argument("--dict", required=True, type=pathlib.Path)
    parser.add_argument("--filler", required=True, type=pathlib.Path)
    parser.add_argument("--sendump", required=True, type=pathlib.Path)
    parser.add_argument("--expected-sendump-rows", required=True, type=int)
    parser.add_argument(
        "--expected-sendump-row-bytes", required=True, type=int
    )
    parser.add_argument("--output", required=True, type=pathlib.Path)
    arguments = parser.parse_args()
    try:
        model = read_model(arguments.mdef)
        sendump_rows, sendump_row_bytes = read_sendump_geometry(
            arguments.sendump
        )
        expected_geometry = (
            arguments.expected_sendump_rows,
            arguments.expected_sendump_row_bytes,
        )
        if (sendump_rows, sendump_row_bytes) != expected_geometry:
            raise ValueError(
                f"{arguments.sendump}: sendump geometry "
                f"{sendump_rows}x{sendump_row_bytes}, expected "
                f"{expected_geometry[0]}x{expected_geometry[1]}"
            )
        words = read_dictionary(arguments.dict, arguments.filler, model)
        mappings = compile_mappings(model, words)
        arguments.output.parent.mkdir(parents=True, exist_ok=True)
        write_output(arguments.output, model, words, *mappings)
    except (OSError, ValueError, struct.error) as error:
        print(f"compile_voice_model.py: {error}", file=sys.stderr)
        return 1
    print(
        f"compiled {len(words)} words / {sum(len(p) for _, p in words)} "
        f"phones -> {arguments.output.stat().st_size} bytes"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
