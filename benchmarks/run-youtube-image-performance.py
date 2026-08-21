#!/usr/bin/env python3
"""Measure a deterministic thumbnail-heavy results page on the host lab.

The fixture is generated in the requested work directory so the repository
does not carry captured third-party pages.  It intentionally uses the same
bounded response-keyed replay and independently addressed 320x180 images
displayed in 144x82 boxes. JPEG is the default because it matches YouTube's
mqdefault.jpg results resources; PNG remains available as a deliberately
expensive decoder comparison. That makes cold decoding, viewport deferral,
repeat-navigation decoded-cache reuse, scroll latency, and retained memory
directly comparable between revisions without a live CDN in the measurement.
"""

from __future__ import annotations

import argparse
import binascii
import json
import re
import shutil
import statistics
import struct
import subprocess
import zlib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DOCUMENT_URL = "https://results-perf.test/results"
THUMBNAIL_COUNT = 18


def png_chunk(kind: bytes, payload: bytes) -> bytes:
    checksum = binascii.crc32(kind)
    checksum = binascii.crc32(payload, checksum) & 0xFFFFFFFF
    return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", checksum)


def thumbnail_png(index: int) -> bytes:
    width, height = 320, 180
    raw = bytearray()
    for y in range(height):
        raw.append(0)
        for x in range(width):
            raw.extend(
                (
                    (x * 3 + y + index * 29) & 0xFF,
                    (x + y * 2 + index * 47) & 0xFF,
                    (x * 2 + y * 3 + index * 71) & 0xFF,
                )
            )
    return (
        b"\x89PNG\r\n\x1a\n"
        + png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
        + png_chunk(b"IDAT", zlib.compress(bytes(raw), 6))
        + png_chunk(b"IEND", b"")
    )


def jpeg_template(work: Path) -> bytes:
    ffmpeg = shutil.which("ffmpeg")
    if ffmpeg is None:
        raise RuntimeError("JPEG fixture generation requires ffmpeg")
    output = work / "thumbnail-template.jpg"
    completed = subprocess.run(
        (
            ffmpeg, "-hide_banner", "-loglevel", "error",
            "-f", "lavfi", "-i", "testsrc2=size=320x180:rate=1",
            "-frames:v", "1", "-q:v", "5", "-y", str(output),
        ),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if completed.returncode != 0:
        raise RuntimeError(
            "ffmpeg could not generate the JPEG fixture: "
            + completed.stdout.decode("utf-8", errors="replace")
        )
    encoded = output.read_bytes()
    if not encoded.startswith(b"\xff\xd8"):
        raise RuntimeError("ffmpeg produced a non-JPEG thumbnail fixture")
    return encoded


def thumbnail_jpeg(template: bytes, index: int) -> bytes:
    # A short COM marker makes every response body independently cacheable
    # without changing the decoded pixels or requiring 18 encoder processes.
    comment = f"tilefinch-thumbnail-{index:02d}".encode("ascii")
    marker = b"\xff\xfe" + struct.pack(">H", len(comment) + 2) + comment
    return template[:2] + marker + template[2:]


def fnv1a(data: bytes) -> int:
    value = 0xCBF29CE484222325
    for byte in data:
        value ^= byte
        value = (value * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return value


def response_meta(url: str, body: bytes, content_type: str) -> str:
    return "\n".join(
        (
            "psp-http-trace=10",
            "cookie-values=redacted",
            "method=GET",
            f"url={url}",
            f"logical-request-url={url}",
            "success=1",
            "async-delay-pumps=0",
            "external-cancel=0",
            "transport-timeout=0",
            "redirect-origin-tainted=0",
            "error=",
            "request-body-length=0",
            "request-body-hash=cbf29ce484222325",
            "request-content-type=",
            "request-cookie-bytes=0",
            "request-has-cf-clearance=0",
            "request-extra-header-bytes=0",
            "request-extra-header-shape=",
            "request-allow-http-errors=0",
            "request-enforce-cors=0",
            "request-redirect-same-origin-only=0",
            "request-cors-cached-response-validated=0",
            "request-if-none-match=",
            "request-if-modified-since=",
            "request-referer=",
            "request-origin=",
            "request-accept=",
            "request-sec-fetch-dest=",
            "request-sec-fetch-mode=",
            "request-sec-fetch-site=",
            "request-send-client-hints=0",
            "request-client-hint-tokens=",
            "request-client-hint-origin=",
            "request-send-low-client-hints=1",
            "request-sec-fetch-user=0",
            "request-upgrade-insecure=0",
            "request-user-agent=Mozilla/5.0 (iPhone; PlayStation Portable; Tilefinch/0.1) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.4 Mobile/15E148 Safari/604.1",
            "request-diagnostic-mobile-safari=0",
            "request-credentials=0",
            "request-credential-origin=",
            "request-initiator-url=",
            "request-referrer-source=",
            "request-referrer-policy=",
            "status=200",
            f"length={len(body)}",
            f"response-body-hash={fnv1a(body):016x}",
            f"effective-url={url}",
            f"content-type={content_type}",
            "etag=",
            "last-modified=",
            "cf-mitigated=",
            "accept-ch=",
            "critical-ch=",
            "server=fixture-performance",
            "cf-ray=",
            "response-referrer-policy-metadata-valid=1",
            "response-referrer-policy-present=0",
            "response-referrer-policy=",
            "response-security-headers-truncated=0",
            "response-header-count=1",
            "set-cookie-count=0",
            f"response-header-0=content-type: {content_type}",
            "",
        )
    )


def build_fixture(work: Path, image_format: str) -> Path:
    trace = work / "trace"
    trace.mkdir(parents=True, exist_ok=True)
    cards = []
    extension = "jpg" if image_format == "jpeg" else "png"
    content_type = "image/jpeg" if image_format == "jpeg" else "image/png"
    template = jpeg_template(work) if image_format == "jpeg" else b""
    for index in range(THUMBNAIL_COUNT):
        cards.append(
            "<a class=card href='/watch?v=TF%02d'>"
            "<img width=144 height=82 src='https://results-perf.test/"
            "thumb-%02d.%s'><span><b>Bounded result %02d</b>"
            "<small>Fixture channel · %d views</small></span></a>"
            % (index, index, extension, index, 1000 + index)
        )
    html = (
        "<!doctype html><meta name=viewport content='width=device-width'>"
        "<title>Video results</title><style>"
        "*{box-sizing:border-box}html,body{margin:0;background:#fff;color:#111}"
        "header{height:34px;padding:7px 9px;background:#f6f6f6;font-weight:700}"
        "main{padding:5px}.card{display:flex;width:100%;min-height:88px;"
        "gap:7px;padding:3px;color:#111;text-decoration:none;border-bottom:1px solid #ddd}"
        ".card img{display:block;flex:0 0 144px;width:144px;height:82px;object-fit:cover}"
        ".card span{display:block;min-width:0;padding-top:3px}"
        ".card b,.card small{display:block;line-height:16px}"
        ".card small{color:#666;margin-top:5px}</style>"
        "<header>Video search results</header><main>"
        + "".join(cards)
        + "</main>"
    ).encode("utf-8")
    (trace / "0000.body").write_bytes(html)
    (trace / "0000.meta").write_text(
        response_meta(DOCUMENT_URL, html, "text/html; charset=utf-8"),
        encoding="utf-8",
    )
    first_thumbnail_bytes = 0
    for index in range(THUMBNAIL_COUNT):
        sequence = index + 1
        url = f"https://results-perf.test/thumb-{index:02d}.{extension}"
        image_bytes = (thumbnail_jpeg(template, index)
                       if image_format == "jpeg" else thumbnail_png(index))
        if index == 0:
            first_thumbnail_bytes = len(image_bytes)
        (trace / f"{sequence:04d}.body").write_bytes(image_bytes)
        (trace / f"{sequence:04d}.meta").write_text(
            response_meta(url, image_bytes, content_type),
            encoding="utf-8",
        )
    (trace / "trace.meta").write_text(
        "psp-http-trace-clock=1\n"
        "origin-ms=1700000000000\n"
        "capture-complete=yes\n"
        f"record-count={THUMBNAIL_COUNT + 1}\n",
        encoding="utf-8",
    )
    (work / "thumbnail-bytes.txt").write_text(
        f"{first_thumbnail_bytes}\n", encoding="utf-8"
    )
    return trace


def last_line(log: str, prefix: str) -> str:
    matches = [line for line in log.splitlines() if line.startswith(prefix)]
    if not matches:
        raise RuntimeError(f"missing {prefix!r} in lab output")
    return matches[-1]


def integer(line: str, name: str) -> int:
    match = re.search(rf"(?:^| ){re.escape(name)}=([0-9]+)", line)
    if match is None:
        raise RuntimeError(f"missing {name!r} in {line!r}")
    return int(match.group(1))


def run_case(
    lab: Path, trace: Path, work: Path, name: str, repeat: bool, settle: bool
) -> dict[str, int]:
    commands = work / f"{name}.commands"
    command_lines = []
    if repeat:
        command_lines.append("reload")
    if settle:
        command_lines.append("drain 256 0")
    command_lines.extend(
        (
            "mark-steady", "focus-next", "focus-next", "focus-next",
            "page-down", "page-down", "page-down", "top", "status",
        )
    )
    commands.write_text("\n".join(command_lines) + "\n", encoding="utf-8")
    frames = work / f"{name}-frames"
    frames.mkdir(parents=True, exist_ok=True)
    completed = subprocess.run(
        (
            str(lab), "--url", DOCUMENT_URL,
            "--replay-http-response-keyed", str(trace),
            "--commands", str(commands),
            "--loop-output-dir", str(frames), "--no-loop-capture",
            "--output", str(work / f"{name}.ppm"),
            "--limit-mb", "24",
        ),
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
    )
    (work / f"{name}.log").write_text(completed.stdout, encoding="utf-8")
    if completed.returncode != 0:
        raise RuntimeError(
            f"{name} failed with {completed.returncode}:\n{completed.stdout[-4000:]}"
        )
    resources = last_line(completed.stdout, "resources ")
    performance = last_line(completed.stdout, "performance-us ")
    work_us = last_line(completed.stdout, "image-work-us ")
    latency = last_line(completed.stdout, "interaction-latency-summary ")
    memory = last_line(completed.stdout, "memory-categories phase=interactive-stable ")
    stream = last_line(completed.stdout, "stream ")
    images = re.search(
        r" images=([0-9]+)/([0-9]+).* image-cache-hits=([0-9]+) "
        r"image-decoded-cache-hits=([0-9]+).* image-encoded=([0-9]+) "
        r"image-decoded=([0-9]+)",
        resources,
    )
    decode = re.search(r"decode=([0-9]+)/([0-9]+) scale=([0-9]+)/([0-9]+)", work_us)
    if images is None or decode is None:
        raise RuntimeError("could not parse image metrics")
    interaction_lines = [
        line for line in completed.stdout.splitlines()
        if line.startswith("interaction-latency command=")
    ]
    scroll = [integer(line, "total-us") for line in interaction_lines
              if "command=page-down " in line or "command=top " in line]
    focus = [integer(line, "total-us") for line in interaction_lines
             if "command=focus-next " in line]
    return {
        "first_paint_us": integer(stream, "first-paint-us"),
        "layout_us": integer(performance, "layout"),
        "resource_us": integer(performance, "resource"),
        "raster_us": integer(performance, "raster"),
        "network_us": integer(performance, "network"),
        "loaded_images": int(images.group(1)),
        "discovered_images": int(images.group(2)),
        "encoded_cache_hits": int(images.group(3)),
        "decoded_cache_hits": int(images.group(4)),
        "encoded_bytes": int(images.group(5)),
        "decoded_bytes": int(images.group(6)),
        "downsampled_images": integer(resources, "image-downsampled"),
        "decode_us": int(decode.group(1)),
        "scale_us": int(decode.group(3)),
        "interaction_total_us": integer(latency, "total-us"),
        "interaction_max_us": integer(latency, "max-us"),
        "interaction_paint_us": integer(latency, "paint-us"),
        "scroll_max_us": max(scroll, default=0),
        "focus_max_us": max(focus, default=0),
        "peak_bytes": integer(memory, "global-peak"),
    }


def median_rows(rows: list[dict[str, int]]) -> dict[str, int]:
    return {
        key: int(statistics.median(row[key] for row in rows))
        for key in rows[0]
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--lab", type=Path,
        default=ROOT / "build-preset-release" / "psp-browser-interactive-lab",
    )
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--repetitions", type=int, default=5)
    parser.add_argument(
        "--image-format", choices=("jpeg", "png"), default="jpeg",
        help="thumbnail encoding (default: JPEG, matching YouTube results)",
    )
    args = parser.parse_args()
    if args.repetitions < 1:
        parser.error("--repetitions must be positive")
    args.work_dir.mkdir(parents=True, exist_ok=True)
    trace = build_fixture(args.work_dir, args.image_format)
    case_specs = (
        ("cold-immediate", False, False),
        ("cold-settled", False, True),
        ("repeat-immediate", True, False),
        ("repeat-settled", True, True),
    )
    samples: dict[str, list[dict[str, int]]] = {
        name: [] for name, _, _ in case_specs
    }
    for repetition in range(args.repetitions):
        run_dir = args.work_dir / f"run-{repetition + 1}"
        run_dir.mkdir(parents=True, exist_ok=True)
        for name, repeat, settle in case_specs:
            samples[name].append(
                run_case(args.lab, trace, run_dir, name, repeat, settle)
            )
    result = {
        "fixture": {
            "cards": THUMBNAIL_COUNT,
            "image_format": args.image_format,
            "thumbnail_bytes": int(
                (args.work_dir / "thumbnail-bytes.txt").read_text(encoding="utf-8")
            ),
            "repetitions": args.repetitions,
        },
        "median": {name: median_rows(rows) for name, rows in samples.items()},
        "samples": samples,
    }
    (args.work_dir / "result.json").write_text(
        json.dumps(result, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(result["median"], indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
