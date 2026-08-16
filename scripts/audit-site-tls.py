#!/usr/bin/env python3
"""Audit live site chains with Mbed TLS and Tilefinch's compact CA bundle.

This is an explicit network qualification, not a hermetic CTest.  The macOS
curl binary uses SecureTransport and can silently consult the system trust
store even when a small PEM bundle is supplied; that is not representative of
the PSP.  Point this script at Mbed TLS 3.6.x's native ``ssl_client2`` program
so certificate construction and verification match the shipping TLS library.
"""

from __future__ import annotations

import argparse
import concurrent.futures
from pathlib import Path
import re
import subprocess
from urllib.parse import urlparse


UPDATE_HOSTS = {
    "api.github.com",
    "github.com",
    "release-assets.githubusercontent.com",
}


def top_site_hosts(path: Path, limit: int) -> set[str]:
    hosts: set[str] = set()
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#"):
            continue
        fields = line.split("\t" if "\t" in line else ",")
        if fields[0].lower() == "rank":
            continue
        if len(fields) >= 2 and fields[0].isdigit() and int(fields[0]) <= limit:
            hosts.add(fields[1].strip().lower())
    return hosts


def trace_hosts(roots: list[Path]) -> set[str]:
    hosts: set[str] = set()
    for root in roots:
        if not root.exists():
            continue
        for metadata in root.glob("**/*.meta"):
            for line in metadata.read_text(errors="ignore").splitlines():
                if not line.startswith(("url=https://", "effective-url=https://")):
                    continue
                host = urlparse(line.split("=", 1)[1]).hostname
                if host and ".invalid" not in host:
                    hosts.add(host.lower())
    return hosts


def verify_host(
    client: Path, ca_file: Path, host: str, timeout_seconds: float
) -> tuple[str, str, str]:
    def run(max_version: str):
        command = [
            str(client),
            f"server_name={host}",
            "server_port=443",
            # The program always performs one HTTP exchange after the
            # handshake. Ask for a normally small resource so application
            # bytes do not dwarf the certificate transcript.
            "request_page=/robots.txt",
            f"ca_file={ca_file}",
            "auth_mode=required",
            "min_version=tls12",
            f"max_version={max_version}",
            f"read_timeout={int(timeout_seconds * 1000)}",
            "debug_level=1",
        ]
        try:
            result = subprocess.run(
                command,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                errors="replace",
                timeout=timeout_seconds + 4.0,
                check=False,
            )
        except subprocess.TimeoutExpired:
            return None
        return result.stdout

    transcript = run("tls13")
    if transcript is None:
        return host, "unreachable", "process timeout"
    if (
        "[ Protocol is " not in transcript
        and "Verify requested for" in transcript
        and "-0x2700" not in transcript
        and "Certificate verification failed" not in transcript
    ):
        # The shipping transport retries a pre-HTTP TLS handshake failure once
        # with TLS 1.2. Reproduce that compatibility path so a TLS 1.3
        # negotiation quirk is not misreported as a trust-store failure.
        fallback = run("tls12")
        if fallback is None:
            return host, "unreachable", "TLS 1.2 fallback timed out"
        transcript = fallback
    if "[ Protocol is " in transcript:
        # Application reads may still fail after a completely verified TLS
        # handshake.  This audit deliberately classifies only the certificate.
        return host, "verified", ""
    if "-0x2700" in transcript or "Certificate verification failed" in transcript:
        if "does not match with the expected CN" in transcript:
            return host, "hostname", "certificate does not cover this apex host"
        issuers = re.findall(r"^issuer name\s+:\s*(.+)$", transcript, re.MULTILINE)
        flags = list(
            dict.fromkeys(
                re.findall(r"^\s+!\s+(.+)$", transcript, re.MULTILINE)
            )
        )
        reason = "; ".join(flags) if flags else "certificate policy or trust failure"
        if issuers:
            reason += f"; leaf issuer: {issuers[0]}"
        return host, "certificate", reason
    errors = [
        line.strip()
        for line in transcript.splitlines()
        if "Last error" in line or " failed" in line.lower()
    ]
    detail = errors[-1] if errors else "TLS client exited without a diagnostic"
    if "Verify requested for" in transcript:
        # The chain was accepted, but the TLS handshake or subsequent
        # protocol exchange was not. Keep this distinct from both missing
        # trust and DNS/connectivity failures.
        return host, "protocol", detail
    return host, "unreachable", detail


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--mbedtls-client", type=Path, required=True)
    parser.add_argument("--ca-file", type=Path, default=Path("certs/roots.pem"))
    parser.add_argument(
        "--top-sites",
        type=Path,
        default=Path("benchmarks/top-sites/2026-07-23-cloudflare-radar-us.tsv"),
    )
    parser.add_argument("--limit", type=int, default=50)
    parser.add_argument("--trace-root", type=Path, action="append", default=[])
    parser.add_argument("--host", action="append", default=[])
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--timeout", type=float, default=8.0)
    args = parser.parse_args()

    if not args.mbedtls_client.is_file():
        parser.error(f"Mbed TLS client not found: {args.mbedtls_client}")
    hosts = UPDATE_HOSTS | top_site_hosts(args.top_sites, args.limit)
    hosts |= trace_hosts(args.trace_root)
    hosts.update(host.lower() for host in args.host)

    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        results = sorted(
            pool.map(
                lambda host: verify_host(
                    args.mbedtls_client, args.ca_file, host, args.timeout
                ),
                hosts,
            )
        )

    counts: dict[str, int] = {}
    for host, status, detail in results:
        counts[status] = counts.get(status, 0) + 1
        if status != "verified":
            print(f"{status}\t{host}\t{detail}")
    summary = " ".join(f"{key}={counts[key]}" for key in sorted(counts))
    print(f"site-tls-audit: total={len(results)} {summary}")

    # Infrastructure apexes without HTTPS and apex certificates that correctly
    # do not cover the bare DNS service name are census facts, not missing CAs.
    # Any other certificate failure is actionable and fails the qualification.
    return 1 if counts.get("certificate", 0) else 0


if __name__ == "__main__":
    raise SystemExit(main())
