#!/usr/bin/env python3
"""scripts/treadline_lab.py, the loopback lab runner both Treadline gates
share: it serves the game, hands the lab the game URL and its flags, and
logs nothing per request (the menu-layout gate once set log_message on a
functools.partial, which never reached the handler, so every request was
logged).

A stand-in "lab" fetches the URL it is given, so no build is needed. Exit 77
when loopback serving is unavailable."""
import contextlib
import io
from pathlib import Path
import socket
import stat
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import treadline_lab  # noqa: E402

FAKE_LAB = """#!{python}
import sys, urllib.request
args = sys.argv[1:]
url = args[args.index("--url") + 1]
with urllib.request.urlopen(url) as response:
    body = response.read()
open(args[args.index("--output") + 1], "w").write(
    "%d %d %s\\n" % (response.status, len(body), " ".join(args)))
"""


def loopback_available():
    try:
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
        return True
    except OSError:
        return False


class TreadlineLabRunnerTests(unittest.TestCase):
    def test_serves_the_game_quietly(self):
        with tempfile.TemporaryDirectory() as work:
            work = Path(work)
            lab = work / "lab"
            lab.write_text(FAKE_LAB.format(python=sys.executable))
            lab.chmod(lab.stat().st_mode | stat.S_IXUSR)
            commands = work / "game.commands"
            commands.write_text("tick 1 16\n")
            log = io.StringIO()
            with contextlib.redirect_stderr(log):
                run = treadline_lab.run_game_lab(
                    lab, commands, work / "out.txt", query="?daily=20261001")
            self.assertIsNotNone(run)
            self.assertEqual(run.returncode, 0, run.stderr)
            status, size, argv = (work / "out.txt").read_text().split(" ", 2)
            self.assertEqual(status, "200")
            self.assertEqual(
                int(size), (treadline_lab.GAME / "index.html").stat().st_size)
            self.assertIn("/index.html?daily=20261001", argv)
            self.assertIn(" ".join(treadline_lab.LAB_FLAGS), argv)
            self.assertIn(f"--commands {commands}", argv)
            self.assertEqual(log.getvalue(), "", "the server logged requests")


if __name__ == "__main__":
    if not loopback_available():
        print("SKIP: loopback sockets are unavailable")
        sys.exit(77)
    unittest.main()
