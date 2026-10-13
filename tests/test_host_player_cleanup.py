#!/usr/bin/env python3
"""Exercise local playback without a network, account, or audible output."""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

lab = Path(sys.argv[1]).resolve()
root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="tilefinch-player-cleanup-") as directory:
    work = Path(directory)
    commands = work / "commands.txt"
    commands.write_text("media-play\nmedia-run 2 33\nmedia-seek 1\n"
                        "media-run 2 33\nmedia-seek 0\nmedia-close\nquit\n")
    environment = dict(os.environ, SDL_AUDIODRIVER="dummy")
    result = subprocess.run([
        str(lab), "--fixture", str(root / "benchmarks/fixtures/youtube-video-lab.html"),
        "--media-file", str(root / "tests/fixtures/psp-media/main-640x360.mp4"),
        "--no-javascript", "--psp-profile", "realistic", "--commands", str(commands),
        "--no-loop-capture", "--loop-output-dir", str(work), "--output", os.devnull,
    ], env=environment, capture_output=True, text=True, timeout=30)
    output = result.stdout + result.stderr
    assert result.returncode == 0, output
    assert re.search(r"teardown=0\b", output), output
    assert "interaction-latency command=media-seek" in output, output
    assert "interaction-latency command=media-close" in output, output
print("local playback, seeks and title-cache teardown passed")
