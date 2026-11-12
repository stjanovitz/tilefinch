#!/usr/bin/env python3
"""Makes a response-keyed replay trace that serves one browser update.

The PSP under test may have no working Wi-Fi, but its update session, client,
signature and hash checks, installer, journal and launcher should still run
for real. This writes the two responses that update would fetch, from local
files and without any network: the signed metadata (TFUM) at a test URL and
the package (TFUP) beside it, where a signed-URL override resolves it
(package_relative_to_metadata in src/update_client.c). It also writes
update-route.cfg, the sidecar that arms the validation seam in
src/psp_update_e2e.c (format in src/psp_update_route.h).

Run the trace from host0: with trace_keyed=1, validation_update_auto=1 and
validation_update_url set to --metadata-url. The update session then installs
into the inactive slot of --install-root, a separate test install whose
launcher must embed the root that signed the TFUM; nothing here signs.

Keep the output outside the repository; it is a run artifact.
"""

import argparse
import hashlib
import struct
import sys
from pathlib import Path
from urllib.parse import urlsplit

DEFAULT_METADATA_URL = (
    "https://updates.tilefinch.test/route/tilefinch-update-v1.tfum")
DEFAULT_ORIGIN_MS = 1794156092000
MANIFEST_HEADER = ">HIQQHHHQ"
MANIFEST_HEADER_BYTES = struct.calcsize(MANIFEST_HEADER)
BROWSER_ARTIFACT = 1


def record_meta(url, body, content_type):
    """A minimal (version 1) response record: the replay matches it on the
    method and URL, and checks the body length."""
    lines = [
        "psp-http-trace=1",
        "method=GET",
        f"url={url}",
        "success=1",
        "async-delay-pumps=0",
        "external-cancel=0",
        "transport-timeout=0",
        "error=",
        "status=200",
        f"length={len(body)}",
        f"effective-url={url}",
        f"content-type={content_type}",
        "server=fixture",
        "set-cookie-count=0",
        "response-header-count=1",
        f"response-header-0=content-type: {content_type}",
    ]
    return "\n".join(lines) + "\n"


def read_manifest(envelope, package):
    """The signed manifest's fields, found in the envelope by the package
    digest it must carry. Only used to choose URLs and catch a mismatched
    pair before anything is copied; the device verifies the signature."""
    digest = hashlib.sha256(package).digest()
    at = envelope.find(digest)
    if at < MANIFEST_HEADER_BYTES or envelope.find(digest, at + 1) != -1:
        raise SystemExit("the metadata does not name this package's SHA-256 "
                         "exactly once; was it made from this package?")
    start = at - MANIFEST_HEADER_BYTES
    (version, _root_version, sequence, expires, _protocol, _platform,
     artifact, size) = struct.unpack_from(MANIFEST_HEADER, envelope, start)
    if version != 1 or artifact != BROWSER_ARTIFACT or size != len(package):
        raise SystemExit("the metadata is not a browser manifest for this "
                         f"package (version {version}, artifact {artifact}, "
                         f"size {size} vs {len(package)})")
    cursor = at + len(digest)
    strings = []
    for _ in range(3):
        length = envelope[cursor]
        strings.append(envelope[cursor + 1:cursor + 1 + length].decode("ascii"))
        cursor += 1 + length
    display, tag, asset = strings
    return {"sequence": sequence, "expires": expires, "version": display,
            "tag": tag, "asset": asset}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--metadata", required=True, type=Path,
                        help="signed TFUM (tilefinch_update_tool.py envelope)")
    parser.add_argument("--package", required=True, type=Path,
                        help="the TFUP that metadata authorizes")
    parser.add_argument("--out", required=True, type=Path,
                        help="trace directory to create")
    parser.add_argument("--metadata-url", default=DEFAULT_METADATA_URL)
    parser.add_argument("--install-root", required=True,
                        help="ms0:/PSP/GAME/<test install>, never TILEFINCH")
    parser.add_argument("--running-slot", required=True, choices=("a", "b"),
                        help="the slot the session acts as (the journal's "
                             "active slot, unless testing a refusal)")
    parser.add_argument("--origin-ms", type=int, default=DEFAULT_ORIGIN_MS)
    parser.add_argument("--journal-only", action="store_true",
                        help="the route only reports the install's journal "
                             "and files, then exits; it writes nothing on "
                             "the Memory Stick")
    arguments = parser.parse_args()

    out = arguments.out
    if out.exists():
        raise SystemExit(f"{out} exists; remove it or choose another name")
    url = urlsplit(arguments.metadata_url)
    if (url.scheme != "https" or not url.hostname or url.query
            or url.fragment or "@" in url.netloc
            or not url.path.endswith(".tfum")):
        raise SystemExit("--metadata-url must be a plain https URL of a .tfum")
    if (not arguments.install_root.startswith("ms0:/PSP/GAME/")
            or arguments.install_root.rstrip("/").upper()
            == "MS0:/PSP/GAME/TILEFINCH"):
        raise SystemExit("--install-root must be a separate test install "
                         "under ms0:/PSP/GAME/")
    envelope = arguments.metadata.read_bytes()
    package = arguments.package.read_bytes()
    if not package.startswith(b"TFUPv1\0\0"):
        raise SystemExit("--package is not a browser TFUP")
    manifest = read_manifest(envelope, package)
    base = arguments.metadata_url.rsplit("/", 1)[0]
    package_url = f"{base}/{manifest['asset']}"

    out.mkdir(parents=True)
    records = [
        (arguments.metadata_url, envelope),
        (package_url, package),
    ]
    for sequence, (record_url, body) in enumerate(records):
        name = f"{sequence:04d}"
        (out / f"{name}.body").write_bytes(body)
        (out / f"{name}.meta").write_text(
            record_meta(record_url, body, "application/octet-stream"),
            encoding="utf-8")
    (out / "trace.meta").write_text(
        f"psp-http-trace-clock=1\norigin-ms={arguments.origin_ms}\n"
        f"capture-complete=yes\nrecord-count={len(records)}\n",
        encoding="utf-8")
    (out / "update-route.cfg").write_text(
        f"install_root={arguments.install_root.rstrip('/')}\n"
        f"running_slot={arguments.running_slot}\n"
        + ("journal_only=1\n" if arguments.journal_only else ""),
        encoding="utf-8")
    print(f"trace {out}: {manifest['version']} sequence "
          f"{manifest['sequence']} tag {manifest['tag']} expires "
          f"{manifest['expires']}; metadata {len(envelope)} B at "
          f"{arguments.metadata_url}; package {len(package)} B at "
          f"{package_url}; "
          + ("reports only on " if arguments.journal_only
             else "installs into ")
          + f"{arguments.install_root} as slot {arguments.running_slot}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
