#!/usr/bin/env python3
"""Page audio's probe and MP3 stream over real HTTP on loopback.

The PSP opens page audio with a 1 KiB range probe and then reads the
station's own response through the MP3 source (psp_media_open.c,
psp_media_hls.c). This serves the shapes a radio directory's stations
answer, on 127.0.0.1 only, and runs tests/test_media_mp3_stream.c against
each:

  file      a finite MP3 that answers ranges: routed seekable, with its length
  file200   the same file, ignoring Range: routed by its whole body
  big200    a file larger than the probe's sniff window, ignoring Range:
            the probe overflows and routes to MP3 as live, and the stream
            ends at the file's declared length instead of replaying
  big404    a 404 with an audio type and a body past the sniff window: the
            scheduled probe does not route it to MP3
  endless   HTTP/1.0 200, audio/mpeg, no length, never ends (a SHOUTcast or
            Icecast mount): the probe overflows its window and routes to MP3;
            the stream reads thousands of frames in constant memory
  icy       the same body after an "ICY 200 OK" status line, as SHOUTcast v1
            servers answer
  html      a 200 text/html error page: not audio

The endless probes also run through a FetchScheduler, the PSP's path, which
copies the request and configures its own transfer.

Each stream request must not ask for in-band metadata (Icy-MetaData), which
would interleave title blocks with the audio.
"""
import http.server
import socket
import socketserver
import subprocess
import sys
import threading
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = (ROOT / "tests" / "fixtures" / "media-mp3"
           / "cbr-44100-stereo.mp3").read_bytes()
FIXTURE_FRAMES = 40
# Larger than MEDIA_HTTP_PROBE_SNIFF_BYTES (32 KiB).
BIG_REPEAT = 12
BIG = FIXTURE * BIG_REPEAT
ICY_REQUESTS = []
HTTP_REQUESTS = []


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def log_message(self, *args):
        pass

    def endless(self):
        try:
            while True:
                self.wfile.write(FIXTURE)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_GET(self):
        HTTP_REQUESTS.append((self.path, dict(self.headers.items())))
        if self.path.startswith("/big"):
            self.send_response(404 if self.path.startswith("/big404")
                               else 200)
            self.send_header("Content-Type", "audio/mpeg")
            self.send_header("Content-Length", str(len(BIG)))
            self.end_headers()
            try:
                self.wfile.write(BIG)
            except (BrokenPipeError, ConnectionResetError):
                pass
        elif self.path.startswith("/file200"):
            self.send_response(200)
            self.send_header("Content-Type", "audio/mpeg")
            self.send_header("Content-Length", str(len(FIXTURE)))
            self.end_headers()
            self.wfile.write(FIXTURE)
        elif self.path.startswith("/file"):
            spec = self.headers.get("Range", "")
            if spec.startswith("bytes="):
                first, _, last = spec[6:].partition("-")
                first = int(first)
                last = min(int(last) if last else len(FIXTURE) - 1,
                           len(FIXTURE) - 1)
                body = FIXTURE[first:last + 1]
                self.send_response(206)
                self.send_header("Content-Range",
                                 f"bytes {first}-{last}/{len(FIXTURE)}")
            else:
                body = FIXTURE
                self.send_response(200)
            self.send_header("Content-Type", "audio/mpeg")
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif self.path.startswith("/endless"):
            self.send_response(200)
            self.send_header("Content-Type", "audio/mpeg")
            self.send_header("icy-br", "32")
            self.end_headers()
            self.endless()
        elif self.path.startswith("/html"):
            body = b"<!doctype html><title>Offline</title><p>Offline"
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            self.send_error(404)


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def icy_server(listener):
    """A SHOUTcast v1 server: "ICY 200 OK" and an endless body."""
    def serve(connection):
        with connection:
            request = b""
            while b"\r\n\r\n" not in request:
                chunk = connection.recv(4096)
                if not chunk:
                    return
                request += chunk
            ICY_REQUESTS.append(request.decode("latin-1"))
            try:
                connection.sendall(b"ICY 200 OK\r\nicy-name: Loopback FM\r\n"
                                   b"icy-br: 32\r\ncontent-type: audio/mpeg"
                                   b"\r\n\r\n")
                while True:
                    connection.sendall(FIXTURE)
            except OSError:
                pass

    while True:
        try:
            connection, _ = listener.accept()
        except OSError:
            return
        threading.Thread(target=serve, args=(connection,), daemon=True).start()


def run(binary, *args):
    completed = subprocess.run([binary, *args], capture_output=True,
                               text=True, timeout=60)
    if completed.returncode != 0:
        raise AssertionError(f"{args}: exit {completed.returncode}\n"
                             f"{completed.stdout}{completed.stderr}")
    fields = {}
    for item in completed.stdout.split():
        key, _, value = item.partition("=")
        fields[key] = value
    return fields


def main():
    binary = sys.argv[1]
    server = Server(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", 0))
    listener.listen(8)
    threading.Thread(target=icy_server, args=(listener,), daemon=True).start()
    base = f"http://127.0.0.1:{server.server_address[1]}"
    icy = f"http://127.0.0.1:{listener.getsockname()[1]}/stream?fromyp=true"
    document = f"{base}/station.html"
    failures = []

    def expect(name, condition, fields):
        if not condition:
            failures.append(f"{name}: {fields}")

    probe = run(binary, "probe", f"{base}/file.mp3", document)
    expect("file probe", probe["route"] == "mp3" and probe["seekable"] == "1"
           and probe["length"] == str(len(FIXTURE)), probe)
    probe = run(binary, "probe", f"{base}/file200.mp3", document)
    expect("file200 probe", probe["route"] == "mp3"
           and probe["seekable"] == "0"
           and probe["length"] == str(len(FIXTURE)), probe)
    for mode in ("probe", "probe-scheduled"):
        for name, url in (("endless", f"{base}/endless"), ("icy", icy)):
            probe = run(binary, mode, url, document)
            expect(f"{name} {mode}", probe["route"] == "mp3"
                   and probe["length"] == "0" and probe["limit"] == "1",
                   probe)
    probe = run(binary, "probe", f"{base}/html", document)
    expect("html probe", probe["route"] != "mp3", probe)
    for mode in ("probe", "probe-scheduled"):
        probe = run(binary, mode, f"{base}/big200.mp3", document)
        expect(f"big200 {mode}", probe["route"] == "mp3"
               and probe["length"] == "0" and probe["limit"] == "1", probe)
    # The PSP's scheduled probe keeps the status of a response that
    # overflowed (the direct transport reports none, so it cannot tell).
    probe = run(binary, "probe-scheduled", f"{base}/big404.mp3", document)
    expect("big404 probe-scheduled", probe["route"] != "mp3"
           and probe["http"] == "404", probe)

    stream = run(binary, "stream", f"{base}/file.mp3", "1000")
    expect("file stream", stream["frames"] == str(FIXTURE_FRAMES)
           and stream["rate"] == "44100" and stream["channels"] == "2"
           and stream["broken"] == "0", stream)
    stream = run(binary, "stream", f"{base}/big200.mp3", "100000")
    expect("big200 stream",
           stream["frames"] == str(FIXTURE_FRAMES * BIG_REPEAT)
           and stream["live"] == "1" and stream["ended"] == "1"
           and stream["failed"] == "0" and stream["broken"] == "0", stream)
    # About two minutes of audio at 32 kbit/s, far past the 128 KiB ring.
    for name, url in (("endless", f"{base}/endless"), ("icy", icy)):
        stream = run(binary, "stream", url, "5000")
        expect(f"{name} stream", stream["frames"] == "5000"
               and stream["rate"] == "44100" and stream["live"] == "1"
               and stream["sync_losses"] == "0" and stream["broken"] == "0"
               and int(stream["received"]) > 128 * 1024
               and stream["peak"] == stream["first"], stream)

    sent = [headers for _, headers in HTTP_REQUESTS] + ICY_REQUESTS
    expect("no Icy-MetaData", not any(
        "icy-metadata" in str(request).lower() for request in sent), sent)
    expect("icy server reached", len(ICY_REQUESTS) >= 2, ICY_REQUESTS)
    server.shutdown()
    listener.close()
    if failures:
        print("media-mp3-stream-tests failed:\n  " + "\n  ".join(failures))
        return 1
    print("media-mp3-stream-tests: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
