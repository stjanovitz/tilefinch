"""Run the interactive lab on Treadline Arena served over loopback.

Shared by scripts/check-treadline-menu-layout.py and
scripts/check-treadline-references.py, so the two gates load the game the
same way: examples/treadline-arena from a quiet loopback HTTP server, with
the game's script allowances (a 16 MiB script heap, 512 KiB per file).
"""
import functools
import http.server
from pathlib import Path
import subprocess
import threading

ROOT = Path(__file__).resolve().parents[1]
GAME = ROOT / "examples/treadline-arena"
LAB_FLAGS = ["--fetch-scripts", "--script-heap-mb", "16", "--script-file-kb", "512"]


class QuietHandler(http.server.SimpleHTTPRequestHandler):
    """Serve files without a log line per request."""

    def log_message(self, *_):
        pass


def run_game_lab(lab, commands, output, query="", timeout=600):
    """Serve the game, run `lab` on its index.html (plus `query`, such as
    "?daily=20261001") with the command file `commands`, writing the final
    frame to `output`. Returns the lab's CompletedProcess (text, captured),
    or None, after printing a SKIP line, when loopback serving is
    unavailable."""
    handler = functools.partial(QuietHandler, directory=str(GAME))
    try:
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    except OSError as error:
        print(f"SKIP: loopback serving unavailable: {error}")
        return None
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        return subprocess.run(
            [str(lab), "--url",
             f"http://127.0.0.1:{server.server_port}/index.html{query}",
             *LAB_FLAGS, "--commands", str(commands), "--output", str(output)],
            text=True, capture_output=True, timeout=timeout)
    finally:
        server.shutdown()
        server.server_close()
