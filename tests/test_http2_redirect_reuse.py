#!/usr/bin/env python3
"""Aborted HTTP/2 streams must leave their connection usable.

redirect case: the transport stops reading a redirect at its headers and
follows Location on the same easy handle. With the macOS system libcurl
(8.7.1) that abort broke the shared HTTP/2 connection: the next stream
decoded shifted HPACK fields ("Invalid HTTP header field was received ...
[:status], [302]") and every later request on it failed.

error case: a 404 the transport rejects (the request does not allow HTTP
errors) did the same with that libcurl: later streams decoded header names
from the wrong HPACK table entry ("content-length: <another field's
value>", a misnamed or foreign Set-Cookie).

This runs the case's probe (http2_redirect_probe or http2_error_reuse_probe)
against a loopback HTTP/2 server and checks both the results and that the
later streams really shared a connection with the aborted one.

usage: test_http2_redirect_reuse.py PROBE NODE [redirect|error]
"""

from __future__ import annotations

from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
SERVER = ROOT / "tests" / "http2_redirect_server.js"
SKIP = 77


def make_certificates(directory: Path) -> None:
    config = directory / "openssl.cnf"
    config.write_text(
        "[req]\ndistinguished_name=dn\n[dn]\n"
        "[ca]\nbasicConstraints=critical,CA:TRUE\n"
        "keyUsage=critical,keyCertSign,cRLSign\n"
        "[leaf]\nbasicConstraints=critical,CA:FALSE\n"
        "keyUsage=critical,digitalSignature,keyEncipherment\n"
        "extendedKeyUsage=serverAuth\n"
        "subjectAltName=DNS:localhost,IP:127.0.0.1\n",
        encoding="ascii")
    quiet = {"check": True, "stdout": subprocess.DEVNULL,
             "stderr": subprocess.DEVNULL}
    subprocess.run(
        ["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
         "-keyout", str(directory / "ca.key"),
         "-out", str(directory / "ca.pem"), "-days", "1",
         "-subj", "/CN=Tilefinch test CA", "-config", str(config),
         "-extensions", "ca"], **quiet)
    subprocess.run(
        ["openssl", "req", "-newkey", "rsa:2048", "-nodes",
         "-keyout", str(directory / "leaf.key"),
         "-out", str(directory / "leaf.csr"), "-subj", "/CN=localhost",
         "-config", str(config)], **quiet)
    subprocess.run(
        ["openssl", "x509", "-req", "-in", str(directory / "leaf.csr"),
         "-CA", str(directory / "ca.pem"),
         "-CAkey", str(directory / "ca.key"), "-CAcreateserial",
         "-out", str(directory / "leaf.pem"), "-days", "1",
         "-extfile", str(config), "-extensions", "leaf"], **quiet)


def redirect_reused(streams: list[list[str]]) -> bool:
    targets = [fields for fields in streams if fields[4] == "/target"]
    # Each /target follows an aborted or drained redirect stream; it shows
    # the stream's effect only when it shares a connection with earlier
    # streams. The last redirect's body is over the drain bound, so its
    # transfer is aborted and libcurl closes that connection: its /target
    # may open a new one.
    return len(targets) == 4 and all(int(fields[2]) > 1
                                     for fields in targets[:3])


def error_reused(streams: list[list[str]]) -> bool:
    # The scheduler's five requests (two of them rejected 404s) must share
    # one connection: the probe's checks of /fields then show any HPACK
    # desynchronization. The synchronous transport's five that follow need
    # not reuse a connection; they check its rejection of the 404s.
    return len(streams) == 10 and all(fields[1] == streams[0][1]
                                      for fields in streams[:5])


CHECKS = {"redirect": redirect_reused, "error": error_reused}


def main() -> int:
    if len(sys.argv) not in (3, 4) or (
            len(sys.argv) == 4 and sys.argv[3] not in CHECKS):
        print(__doc__)
        return 2
    probe, node = sys.argv[1], sys.argv[2]
    check = CHECKS[sys.argv[3] if len(sys.argv) == 4 else "redirect"]
    if not node or shutil.which(node) is None:
        print("SKIP: node was not found when CMake configured this tree")
        return SKIP
    if shutil.which("openssl") is None:
        print("SKIP: openssl unavailable")
        return SKIP
    with tempfile.TemporaryDirectory() as name:
        directory = Path(name)
        make_certificates(directory)
        port_file = directory / "port"
        server = subprocess.Popen(
            [node, str(SERVER), str(directory / "leaf.pem"),
             str(directory / "leaf.key"), str(port_file)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            for _ in range(500):
                if port_file.exists() and port_file.read_text():
                    break
                if server.poll() is not None:
                    print(server.communicate()[1])
                    return 1
                time.sleep(0.01)
            else:
                print("server did not publish its port")
                return 1
            port = int(port_file.read_text(encoding="ascii"))
            try:
                run = subprocess.run(
                    [probe, f"https://localhost:{port}",
                     str(directory / "ca.pem")],
                    capture_output=True, text=True, timeout=45)
            except subprocess.TimeoutExpired as hung:
                # Under ctest's 60 s, so a hung probe still reports.
                print(f"FAIL: probe did not finish in 45 s\n"
                      f"{hung.stdout or ''}{hung.stderr or ''}")
                return 1
        finally:
            server.terminate()
            try:
                log = server.communicate(timeout=5)[0]
            except subprocess.TimeoutExpired:
                server.kill()
                log = server.communicate(timeout=5)[0]
    print(run.stdout + run.stderr)
    if run.returncode == SKIP:
        print("SKIP: the transport did not negotiate HTTP/2")
        return SKIP
    if run.returncode != 0:
        print(f"FAIL: probe exited {run.returncode}")
        return 1
    streams = [line.split() for line in log.splitlines()
               if line.startswith("stream ")]
    if not check(streams):
        print("FAIL: later streams did not reuse a connection:\n" + log)
        return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
