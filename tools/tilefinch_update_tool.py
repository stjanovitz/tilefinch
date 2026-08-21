#!/usr/bin/env python3
"""Offline Tilefinch browser and optional-component release producer.

This tool intentionally delegates private-key operations to the local OpenSSL
command. Private keys are never parsed, copied into output, or expected in CI.
Every ECDSA signature is normalized to canonical low-S form.
"""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import struct
import subprocess

P256_ORDER = int(
    "FFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551", 16
)
MAX_PACKAGE = 32 * 1024 * 1024
MAX_FILES = 64
ALLOWED_EXACT = {"EBOOT.PBP", "roots.pem", "boot-defaults.cfg"}
ALLOWED_PREFIX = ("fonts/",)
VOICE_ALLOWED_EXACT = {"model-info.tfv"}
VOICE_ALLOWED_PREFIX = ("model/", "LICENSES/")


def run(*args: str, input_bytes: bytes | None = None) -> bytes:
    return subprocess.run(
        args, input=input_bytes, stdout=subprocess.PIPE, check=True
    ).stdout


def public_point(key: Path, public_only: bool = False) -> bytes:
    command = ["openssl", "pkey", "-in", str(key)]
    if public_only:
        command.append("-pubin")
    command += ["-pubout", "-outform", "DER"]
    der = run(*command)
    point = der[-65:]
    if len(point) != 65 or point[0] != 4:
        raise ValueError(f"{key}: expected an uncompressed P-256 public key")
    return point


def key_id(point: bytes) -> bytes:
    return hashlib.sha256(b"tilefinch:p256-key:v1\0" + point).digest()


def der_integer(data: bytes, at: int) -> tuple[int, int]:
    if at >= len(data) or data[at] != 2:
        raise ValueError("invalid ECDSA DER integer")
    length = data[at + 1]
    start = at + 2
    end = start + length
    if end > len(data) or length == 0:
        raise ValueError("truncated ECDSA DER integer")
    return int.from_bytes(data[start:end], "big"), end


def sign(private_key: Path, domain: bytes, payload: bytes) -> bytes:
    der = run(
        "openssl", "dgst", "-sha256", "-sign", str(private_key),
        input_bytes=domain + b"\0" + payload,
    )
    if len(der) < 8 or der[0] != 0x30 or der[1] != len(der) - 2:
        raise ValueError("unsupported ECDSA DER signature")
    r, at = der_integer(der, 2)
    s, at = der_integer(der, at)
    if at != len(der) or not (0 < r < P256_ORDER and 0 < s < P256_ORDER):
        raise ValueError("invalid ECDSA signature values")
    if s > P256_ORDER // 2:
        s = P256_ORDER - s
    return r.to_bytes(32, "big") + s.to_bytes(32, "big")


def safe_name(value: str, maximum: int) -> bytes:
    encoded = value.encode("ascii")
    if not encoded or len(encoded) > maximum or any(
        not (chr(byte).isalnum() or chr(byte) in "._-") for byte in encoded
    ):
        raise ValueError(f"unsafe release name: {value!r}")
    return encoded


def safe_package_path(path: str, component: bool = False) -> bool:
    if (
        not path or path.startswith("/") or "\\" in path or ":" in path
        or len(path.encode("ascii", "ignore")) != len(path)
        or len(path) > 128
    ):
        return False
    components = path.split("/")
    if any(
        not part or part in {".", ".."} or any(
            not (character.isalnum() or character in "._-")
            for character in part
        )
        for part in components
    ):
        return False
    exact = VOICE_ALLOWED_EXACT if component else ALLOWED_EXACT
    prefixes = VOICE_ALLOWED_PREFIX if component else ALLOWED_PREFIX
    return path in exact or path.startswith(prefixes)


def command_root(args: argparse.Namespace) -> None:
    root_points = [public_point(Path(item), True) for item in args.root_key]
    release_points = [
        public_point(Path(item), True) for item in args.release_key
    ]
    all_ids = [key_id(point) for point in root_points + release_points]
    if len(set(all_ids)) != len(all_ids):
        raise ValueError("root metadata contains a duplicate key ID")
    if not (
        0 < args.root_threshold <= len(root_points) <= 6
        and 0 < args.release_threshold <= len(release_points) <= 6
    ):
        raise ValueError("invalid role threshold or key count")
    output = bytearray()
    output += struct.pack(
        ">HIQBBBB", 1, args.version, args.expires,
        args.root_threshold, args.release_threshold,
        len(root_points), len(release_points),
    )
    for point in root_points + release_points:
        output += key_id(point) + point
    Path(args.output).write_bytes(output)


def command_pack(args: argparse.Namespace) -> None:
    root = Path(args.directory).resolve()
    files = sorted(path for path in root.rglob("*") if path.is_file())
    if not files or len(files) > MAX_FILES:
        raise ValueError("TFUP requires between 1 and 64 files")
    records: list[tuple[bytes, bytes, bytes]] = []
    for file in files:
        relative = file.relative_to(root).as_posix()
        if not safe_package_path(relative, args.component):
            raise ValueError(f"path is not in the TFUP allowlist: {relative}")
        payload = file.read_bytes()
        if not payload:
            raise ValueError(f"empty TFUP file: {relative}")
        records.append(
            (relative.encode("ascii"), payload, hashlib.sha256(payload).digest())
        )
    table_length = sum(1 + len(path) + 8 + 32 + 8 for path, _, _ in records)
    payload_offset = 16 + table_length
    table = bytearray()
    payloads = bytearray()
    for path, payload, digest in records:
        table += bytes([len(path)]) + path
        table += struct.pack(">Q", len(payload)) + digest
        table += struct.pack(">Q", payload_offset)
        payload_offset += len(payload)
        payloads += payload
    magic = b"TFVPv1\0\0" if args.component else b"TFUPv1\0\0"
    package = magic + struct.pack(">HHI", 1, len(records), table_length)
    package += table + payloads
    if len(package) > MAX_PACKAGE:
        raise ValueError("TFUP exceeds the 32 MiB signed package ceiling")
    Path(args.output).write_bytes(package)
    print(f"{len(package)} {hashlib.sha256(package).hexdigest()}")


def command_manifest(args: argparse.Namespace) -> None:
    package = Path(args.package).read_bytes()
    if not package or len(package) > MAX_PACKAGE:
        raise ValueError("package is empty or exceeds 32 MiB")
    glyph = getattr(args, "glyph_component", False)
    expected_magic = (b"TFGFv1\0\0" if glyph else
                      b"TFVPv1\0\0" if args.component else b"TFUPv1\0\0")
    if not package.startswith(expected_magic):
        raise ValueError("package kind does not match the selected artifact")
    version = args.version.encode("ascii")
    if not version or len(version) > 31 or any(
        byte < 0x20 or byte > 0x7E for byte in version
    ):
        raise ValueError("display version must be 1-31 printable ASCII bytes")
    tag = safe_name(args.tag, 63)
    asset = safe_name(args.asset, 95)
    notes = Path(args.notes).read_bytes() if args.notes else b""
    notes.decode("utf-8")
    if not args.component and not args.glyph_component:
        if args.decoder_abi is None or not (1 <= args.decoder_abi <= 65535):
            raise ValueError("browser manifests require --decoder-abi 1..65535")
        notes = (
            f"Decoder ABI {args.decoder_abi}; rebuild if different. ".encode(
                "ascii"
            ) + notes
        )
    elif args.decoder_abi is not None:
        raise ValueError("component manifests do not carry a decoder ABI")
    if len(notes) > 512 or any(byte < 0x20 or byte == 0x7F for byte in notes):
        raise ValueError("notes must be at most 512 inert UTF-8 bytes")
    output = bytearray()
    output += struct.pack(
        ">HIQQHHHQ", 1, args.root_version, args.sequence, args.expires,
        args.launcher_protocol, 1, 3 if glyph else 2 if args.component else 1,
        len(package),
    )
    output += hashlib.sha256(package).digest()
    output += bytes([len(version)]) + version
    output += bytes([len(tag)]) + tag
    output += bytes([len(asset)]) + asset
    output += struct.pack(">H", len(notes)) + notes
    Path(args.output).write_bytes(output)


def signature_record(private_key: Path, domain: bytes, payload: bytes) -> bytes:
    point = public_point(private_key)
    return key_id(point) + sign(private_key, domain, payload)


def command_root_update(args: argparse.Namespace) -> None:
    root = Path(args.root).read_bytes()
    if not root or len(root) > 2048:
        raise ValueError("root record is empty or too large")
    old = [
        signature_record(
            Path(key), b"tilefinch:root-metadata:v1", root
        )
        for key in args.old_key
    ]
    new = [
        signature_record(
            Path(key), b"tilefinch:root-metadata:v1", root
        )
        for key in args.new_key
    ]
    if not old or not new or len(old) > 12 or len(new) > 12:
        raise ValueError("root update requires bounded old and new signatures")
    output = struct.pack(">H", len(root)) + root + bytes([len(old)])
    output += b"".join(old) + bytes([len(new)]) + b"".join(new)
    Path(args.output).write_bytes(output)


def command_envelope(args: argparse.Namespace) -> None:
    manifest = Path(args.manifest).read_bytes()
    glyph = getattr(args, "glyph_component", False)
    expected_format = 3 if glyph else 2 if args.component else 1
    if (
        len(manifest) < 28
        or struct.unpack(">H", manifest[26:28])[0] != expected_format
    ):
        raise ValueError("manifest kind does not match the selected artifact")
    rotations = [Path(item).read_bytes() for item in args.root_update]
    domain = (b"tilefinch:glyph-component-manifest:v1" if glyph else
              b"tilefinch:voice-component-manifest:v1" if args.component else
              b"tilefinch:update-manifest:v1")
    signatures = [
        signature_record(
            Path(key), domain, manifest
        )
        for key in args.release_key
    ]
    if (
        not manifest or len(manifest) > 1024 or len(rotations) > 8
        or not signatures or len(signatures) > 12
    ):
        raise ValueError("manifest, root chain, or signature count exceeds limits")
    magic = (b"TFGMv1\0\0" if glyph else
             b"TFVMv1\0\0" if args.component else b"TFUMv1\0\0")
    output = magic + struct.pack(">HB", 1, len(rotations))
    output += b"".join(rotations)
    output += struct.pack(">H", len(manifest)) + manifest
    output += bytes([len(signatures)]) + b"".join(signatures)
    if len(output) > 16 * 1024:
        raise ValueError("TFUM exceeds 16 KiB")
    Path(args.output).write_bytes(output)


def command_developer_envelope(args: argparse.Namespace) -> None:
    """Emit an explicitly unsigned TFUM for the opt-in Developer channel."""
    manifest = Path(args.manifest).read_bytes()
    if not manifest or len(manifest) > 1024:
        raise ValueError("developer manifest is empty or exceeds limits")
    output = b"TFUMv1\0\0" + struct.pack(">HBH", 1, 0, len(manifest))
    output += manifest + b"\0"
    if len(output) > 16 * 1024:
        raise ValueError("developer TFUM exceeds 16 KiB")
    Path(args.output).write_bytes(output)


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser()
    commands = result.add_subparsers(dest="command", required=True)
    root = commands.add_parser("root")
    root.add_argument("--version", type=int, required=True)
    root.add_argument("--expires", type=int, required=True)
    root.add_argument("--root-threshold", type=int, required=True)
    root.add_argument("--release-threshold", type=int, required=True)
    root.add_argument("--root-key", action="append", required=True)
    root.add_argument("--release-key", action="append", required=True)
    root.add_argument("--output", required=True)
    root.set_defaults(function=command_root)

    pack = commands.add_parser("pack")
    pack.add_argument("--directory", required=True)
    pack.add_argument("--output", required=True)
    pack.add_argument("--component", action="store_true")
    pack.set_defaults(function=command_pack)

    manifest = commands.add_parser("manifest")
    manifest.add_argument("--package", required=True)
    manifest.add_argument("--root-version", type=int, required=True)
    manifest.add_argument("--sequence", type=int, required=True)
    manifest.add_argument("--expires", type=int, required=True)
    manifest.add_argument("--launcher-protocol", type=int, default=1)
    manifest.add_argument("--version", required=True)
    manifest.add_argument("--tag", required=True)
    manifest.add_argument("--asset", required=True)
    manifest.add_argument("--notes")
    manifest.add_argument("--decoder-abi", type=int)
    manifest.add_argument("--output", required=True)
    manifest_kind = manifest.add_mutually_exclusive_group()
    manifest_kind.add_argument("--component", action="store_true")
    manifest_kind.add_argument("--glyph-component", action="store_true")
    manifest.set_defaults(function=command_manifest)

    rotation = commands.add_parser("root-update")
    rotation.add_argument("--root", required=True)
    rotation.add_argument("--old-key", action="append", required=True)
    rotation.add_argument("--new-key", action="append", required=True)
    rotation.add_argument("--output", required=True)
    rotation.set_defaults(function=command_root_update)

    envelope = commands.add_parser("envelope")
    envelope.add_argument("--manifest", required=True)
    envelope.add_argument("--root-update", action="append", default=[])
    envelope.add_argument("--release-key", action="append", required=True)
    envelope.add_argument("--output", required=True)
    envelope_kind = envelope.add_mutually_exclusive_group()
    envelope_kind.add_argument("--component", action="store_true")
    envelope_kind.add_argument("--glyph-component", action="store_true")
    envelope.set_defaults(function=command_envelope)

    developer = commands.add_parser("developer-envelope")
    developer.add_argument("--manifest", required=True)
    developer.add_argument("--output", required=True)
    developer.set_defaults(function=command_developer_envelope)
    return result


def main() -> None:
    args = parser().parse_args()
    args.function(args)


if __name__ == "__main__":
    main()
