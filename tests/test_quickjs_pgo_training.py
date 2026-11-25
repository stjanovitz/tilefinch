#!/usr/bin/env python3
"""Reject diagnostic engine profiles before launching the emulator, and
record the checkout a profile was trained in."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile


FAKE_RUNNER = """#!/bin/sh
# Stands in for run-ppsspp-input-script.sh: one clean run with counters.
while [ "$#" -gt 0 ]; do
    case "$1" in --build-dir) build=$2; shift 2 ;; *) shift ;; esac
done
run="$build/ppsspp-input-script-latest/run-1"
mkdir -p "$run/pgo"
printf '%s\\n' 'tilefinch-pgo: counters written' \\
    'tilefinch-input-script: outcome=complete' >"$run/tilefinch-validation.txt"
echo counters >"$run/pgo/quickjs.c.gcda"
"""


def check_provenance(trainer, root):
    """PROVENANCE names the instrumented build's checkout, not the cwd."""
    checkout = root / "checkout"
    (checkout / "scripts").mkdir(parents=True)
    (checkout / "tests/fixtures/ppsspp-pgo").mkdir(parents=True)
    (checkout / "tests/input-scripts").mkdir(parents=True)
    fake_trainer = checkout / "scripts/train-quickjs-pgo.sh"
    fake_trainer.write_text(trainer.read_text())
    runner = checkout / "scripts/run-ppsspp-input-script.sh"
    runner.write_text(FAKE_RUNNER)
    runner.chmod(0o755)
    (checkout / "tests/fixtures/ppsspp-pgo/profile.cfg").write_text("")
    (checkout / "tests/input-scripts/chatgpt-ask.txt").write_text("")
    trained_in = root / "trained-in"
    build = root / "build"
    (root / "corpus/chatgpt-ask-send76").mkdir(parents=True)
    build.mkdir()
    (build / "CMakeCache.txt").write_text(
        "PSP_BROWSER_QUICKJS_PGO_GENERATE:BOOL=ON\n"
        "PSP_BROWSER_EXECUTION_CENSUS:BOOL=OFF\n"
        f"CMAKE_HOME_DIRECTORY:INTERNAL={trained_in}\n"
        "TILEFINCH_QUICKJS_PGO_FINGERPRINT:INTERNAL=0123456789abcdef01234567\n")
    out = root / "out"
    result = subprocess.run(
        ["sh", str(fake_trainer), str(build), str(out), str(root / "corpus")],
        cwd=root, env=dict(os.environ, PSPDEV=str(root / "sdk")), text=True,
        capture_output=True, timeout=10)
    if result.returncode != 0:
        raise AssertionError(("training", result.stdout, result.stderr))
    provenance = (out / "PROVENANCE").read_text().splitlines()
    if (provenance[:2] != ["fingerprint=0123456789abcdef01234567",
                           f"source-dir={trained_in}"]
            or not (out / "quickjs.c.gcda").exists()):
        raise AssertionError(("PROVENANCE", provenance))


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
        home = "CMAKE_HOME_DIRECTORY:INTERNAL=" + str(root / "checkout") + "\n"
        for generate, census, cache, expected in (
            ("ON", "ON", home, "Disable PSP_BROWSER_EXECUTION_CENSUS"),
            ("ON", "OFF", "", "records no source directory"),
            ("ON", "OFF", home, "missing trace"),
            ("OFF", "OFF", home, "not an instrumented"),
        ):
            (build / "CMakeCache.txt").write_text(
                "PSP_BROWSER_QUICKJS_PGO_GENERATE:BOOL=" + generate + "\n"
                "PSP_BROWSER_EXECUTION_CENSUS:BOOL=" + census + "\n" + cache)
            result = subprocess.run(
                ["sh", str(trainer), str(build), str(root / "out"), str(corpus)],
                env=env, text=True, capture_output=True, timeout=5)
            if result.returncode != 2 or expected not in result.stderr:
                raise AssertionError((generate, census, result.returncode,
                                      result.stdout, result.stderr))
        if (root / "out").exists() or (build / "pgo-training").exists():
            raise AssertionError("invalid setup began profile collection")
        check_provenance(trainer, root / "provenance")
    print("QuickJS PGO training configuration checks passed")


if __name__ == "__main__":
    main()
