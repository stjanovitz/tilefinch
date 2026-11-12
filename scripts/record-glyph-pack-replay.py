#!/usr/bin/env python3
"""Records a language pack's download as a response-keyed replay trace.

The real PSP's Wi-Fi may be unavailable, but the pack install should still run
on it for real: same menu or offer route, update client, redirects, signature
and hash checks, Memory Stick staging and activation. Only the bytes on the
wire are replaced. This script makes those bytes, on the Mac, by asking GitHub
the way the update client does: one curl per hop with the updater's
User-Agent, Accept and identity encoding, never following a redirect itself,
so the recording keeps each 302 with its real headers. The seam that replays
it is scheduled_replay_runs_hops in src/fetch/scheduler.inc and
src/psp_app/psp_app_validation_glyph.c.

Set-Cookie values are redacted as a capture redacts them. The update client
sends no credentials and the replay lane ignores response cookies.

The trace also holds one small generated Cyrillic page, so the font going live
can be seen, and glyph-replay.cfg, the sidecar that arms the validation seam.
With --base-trace the pack records are added to a copy of an existing keyed
trace, so the install can run on a heavy replayed page instead.

Keep the output under build-logs/. It is not committed.
"""

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

OWNER = "stjanovitz"
REPOSITORY = "tilefinch-models"
USER_AGENT = "Tilefinch-Updater/1"
ACCEPT = "application/octet-stream"
PAGE_URL = "https://tilefinch.test/cyrillic"
HEADER_SNAPSHOT_LIMIT = 8192
REDIRECTS = (301, 302, 303, 307, 308)
HOP_LIMIT = 5

PAGE = """<!DOCTYPE html>
<html lang="ru"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width">
<title>Проверка кириллицы</title></head>
<body>
<h1>Съешь же ещё этих мягких французских булок</h1>
<p>Широкая электрификация южных губерний даст мощный толчок подъёму сельского
хозяйства. Эта страница нужна, чтобы увидеть, как шрифт начинает работать сразу
после установки языкового пакета: квадраты сменяются буквами без перезагрузки.</p>
<p>Українська: Чуєш їх, доцю, га? Кумедна ж ти, прощайся без ґольфів!</p>
<p>Latin text stays readable before and after the install.</p>
</body></html>
"""


def fnv1a(data):
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return value


def redact_cookie(line):
    head, separator, attributes = line.partition(";")
    name, equals, value = head.partition("=")
    return f"{name}{equals}{'x' * len(value)}{separator}{attributes}"


def parse_headers(raw):
    """The last header block (curl writes one per response) as ordered pairs."""
    blocks = raw.decode("latin-1").replace("\r\n", "\n").strip().split("\n\n")
    lines = blocks[-1].split("\n")
    status = int(lines[0].split()[1])
    headers = []
    for line in lines[1:]:
        name, _, value = line.partition(":")
        headers.append((name.strip().lower(), value.strip()))
    return status, headers


def header(headers, name):
    for key, value in headers:
        if key == name:
            return value
    return ""


class Trace:
    def __init__(self, directory, first_sequence):
        self.directory = directory
        self.sequence = first_sequence

    def _write(self, meta, body):
        name = f"{self.sequence:04d}"
        (self.directory / f"{name}.body").write_bytes(body)
        (self.directory / f"{name}.meta").write_text(meta, encoding="utf-8")
        self.sequence += 1

    def add_hop(self, url, status, headers, body):
        """A version 10 record in the hop's exact request shape: the replay's
        request match compares every field that is not blank here."""
        plain = [(k, v) for k, v in headers if k != "set-cookie"]
        cookies = [v for k, v in headers if k == "set-cookie"]
        snapshot = sum(len(k) + len(v) + 3 for k, v in plain)
        if snapshot >= HEADER_SNAPSHOT_LIMIT:
            raise SystemExit(f"headers of {url} exceed the replay's snapshot")
        policy = header(headers, "referrer-policy").lower()
        lines = [
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
            f"request-accept={ACCEPT}",
            "request-sec-fetch-dest=",
            "request-sec-fetch-mode=",
            "request-sec-fetch-site=",
            "request-send-client-hints=0",
            "request-client-hint-tokens=",
            "request-client-hint-origin=",
            "request-send-low-client-hints=0",
            "request-sec-fetch-user=0",
            "request-upgrade-insecure=0",
            f"request-user-agent={USER_AGENT}",
            "request-diagnostic-mobile-safari=0",
            "request-credentials=0",
            "request-credential-origin=",
            "request-initiator-url=",
            "request-referrer-source=",
            "request-referrer-policy=",
            f"status={status}",
            f"length={len(body)}",
            f"response-body-hash={fnv1a(body):016x}",
            f"effective-url={url}",
            f"content-type={header(headers, 'content-type')}",
            f"etag={header(headers, 'etag')}",
            f"last-modified={header(headers, 'last-modified')}",
            "cf-mitigated=",
            "accept-ch=",
            "critical-ch=",
            f"server={header(headers, 'server')}",
            "cf-ray=",
            "response-referrer-policy-metadata-valid=1",
            f"response-referrer-policy-present={1 if policy else 0}",
            f"response-referrer-policy={policy}",
            "response-security-headers-truncated=0",
            f"response-header-count={len(plain)}",
            f"set-cookie-count={len(cookies)}",
        ]
        for index, cookie in enumerate(cookies):
            lines.append(f"set-cookie-{index}={redact_cookie(cookie)}")
            lines.append(f"set-cookie-url-{index}={url}")
        for index, (name, value) in enumerate(plain):
            lines.append(f"response-header-{index}={name}: {value}")
        self._write("\n".join(lines) + "\n", body)

    def add_page(self, url, body):
        """A minimal (version 1) record: a generated page, not a recording."""
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
            "content-type=text/html; charset=utf-8",
            "server=fixture",
            "set-cookie-count=0",
            "response-header-count=1",
            "response-header-0=content-type: text/html; charset=utf-8",
        ]
        self._write("\n".join(lines) + "\n", body)


def fetch_hop(url, scratch):
    head = scratch / "hop.headers"
    body = scratch / "hop.body"
    subprocess.run(
        ["curl", "--silent", "--show-error", "--fail-with-body",
         "--max-time", "120", "--dump-header", str(head),
         "--output", str(body), "--user-agent", USER_AGENT,
         "--header", f"Accept: {ACCEPT}",
         "--header", "Accept-Encoding: identity", url],
        check=True)
    status, headers = parse_headers(head.read_bytes())
    data = body.read_bytes()
    head.unlink()
    body.unlink()
    return status, headers, data


def record_chain(trace, url, scratch):
    """Records every hop of one request and returns the final body."""
    for hop in range(HOP_LIMIT):
        status, headers, body = fetch_hop(url, scratch)
        trace.add_hop(url, status, headers, body)
        target = header(headers, "location")
        print(f"hop {hop}: {status} {len(body)} B {url[:90]}")
        if status not in REDIRECTS:
            if status != 200:
                raise SystemExit(f"{url}: HTTP {status}")
            return body
        if not target.startswith("https://"):
            raise SystemExit(f"{url}: unexpected redirect to {target[:90]}")
        url = target
    raise SystemExit("too many redirects")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", required=True, type=Path,
                        help="trace directory to create (under build-logs/)")
    parser.add_argument("--asset", default="tilefinch-glyph-cyrillic-v1",
                        help="release asset stem")
    parser.add_argument("--install-root", default="ms0:/PSP/GAME/TILEFINCH",
                        help="where the run installs the pack")
    parser.add_argument("--base-trace", type=Path,
                        help="an existing keyed trace to add the records to")
    arguments = parser.parse_args()
    out = arguments.out
    if out.exists():
        raise SystemExit(f"{out} exists; remove it or choose another name")
    if arguments.base_trace is not None:
        shutil.copytree(arguments.base_trace, out)
        info = (out / "trace.meta").read_text(encoding="utf-8")
        match = re.search(r"^record-count=(\d+)$", info, re.MULTILINE)
        if match is None:
            raise SystemExit("base trace has no record-count")
        first = int(match.group(1))
    else:
        out.mkdir(parents=True)
        info = ("psp-http-trace-clock=1\norigin-ms=1794156092000\n"
                "capture-complete=yes\nrecord-count=0\n")
        first = 0
    trace = Trace(out, first)
    scratch = out / "scratch"
    scratch.mkdir()
    base = f"https://github.com/{OWNER}/{REPOSITORY}/releases"
    metadata_asset = f"{arguments.asset}.tfgm"
    manifest = record_chain(
        trace, f"{base}/latest/download/{metadata_asset}", scratch)
    # The signed manifest names the package. Reading its plain strings here
    # only picks the URL to record; the client verifies them on the device.
    tags = re.findall(rb"components-v[0-9]+", manifest)
    if not tags:
        raise SystemExit("manifest names no components-vN tag")
    tag = tags[0].decode("ascii")
    package_asset = f"{arguments.asset}.tfgf"
    if package_asset.encode("ascii") not in manifest:
        raise SystemExit(f"manifest does not name {package_asset}")
    package = record_chain(
        trace, f"{base}/download/{tag}/{package_asset}", scratch)
    trace.add_page(PAGE_URL, PAGE.encode("utf-8"))
    scratch.rmdir()
    info = re.sub(r"^record-count=\d+$",
                  f"record-count={trace.sequence}", info, flags=re.MULTILINE)
    (out / "trace.meta").write_text(info, encoding="utf-8")
    (out / "glyph-replay.cfg").write_text(
        f"install_root={arguments.install_root}\n", encoding="utf-8")
    assets = out.parent / f"{out.name}-assets"
    assets.mkdir(exist_ok=True)
    (assets / metadata_asset).write_bytes(manifest)
    (assets / package_asset).write_bytes(package)
    print(f"recorded {trace.sequence - first} records in {out}: "
          f"manifest {len(manifest)} B, package {len(package)} B, tag {tag}")


if __name__ == "__main__":
    sys.exit(main())
