#!/usr/bin/env python3
"""Reject diagnostic engine profiles before launching the emulator."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    source_root = Path(__file__).resolve().parents[1]
    profile = (source_root / "tests/fixtures/ppsspp-pgo/profile.cfg").read_bytes()
    body, footer = profile.rsplit(b"END\t", 1)
    checksum = 2166136261
    for byte in body:
        checksum = ((checksum ^ byte) * 16777619) & 0xffffffff
    if footer != f"{len(body)}\t{checksum:08X}\n".encode() or b"HEAVY\t1\n" not in body:
        raise AssertionError("training must explicitly allow heavy-page execution")
    trainer = Path(sys.argv[1]) if len(sys.argv) > 1 else (
        source_root / "scripts/train-quickjs-pgo.sh")
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        build = root / "build"
        corpus = root / "corpus"
        build.mkdir()
        corpus.mkdir()
        env = dict(os.environ, PSPDEV=str(root / "sdk"))
        for generate, census, expected in (
            ("ON", "ON", "Disable PSP_BROWSER_EXECUTION_CENSUS"),
            ("ON", "OFF", "missing trace"),
            ("OFF", "OFF", "not an instrumented"),
        ):
            (build / "CMakeCache.txt").write_text(
                "PSP_BROWSER_QUICKJS_PGO_GENERATE:BOOL=" + generate + "\n"
                "PSP_BROWSER_EXECUTION_CENSUS:BOOL=" + census + "\n")
            result = subprocess.run(
                ["sh", str(trainer), str(build), str(root / "out"), str(corpus)],
                env=env, text=True, capture_output=True, timeout=5)
            if result.returncode != 2 or expected not in result.stderr:
                raise AssertionError((generate, census, result.returncode,
                                      result.stdout, result.stderr))
        if (root / "out").exists() or (build / "pgo-training").exists():
            raise AssertionError("invalid setup began profile collection")
    print("QuickJS PGO training configuration checks passed")


if __name__ == "__main__":
    main()
