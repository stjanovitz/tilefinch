#!/usr/bin/env python3
"""The PGO size check compares two built shipping trees' .text."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

FAKE_SIZE = """#!/bin/sh
# psp-size -A ELF: the fake ELF holds its .text split over two sections.
read total < "$2"
case "$total" in
    tool_error) echo ".text 123 0"; exit 1 ;;
    no_text) echo ".rodata 5000 0"; exit 0 ;;
    malformed) echo ".text 123 0"; echo ".text.hot unknown 0"; exit 0 ;;
    zero) echo ".text 0 0"; exit 0 ;;
esac
echo "$2  :"
echo "section          size   addr"
echo ".text            $((total - 100))  0"
echo ".text.unlikely   100  0"
echo ".rodata          5000  0"
echo "Total            $((total + 5000))"
"""


def write_tree(path, home, text, profile="", validation="OFF", flags="",
               override="", ratchet="120000"):
    path.mkdir(parents=True, exist_ok=True)
    (path / "psp-browser-script").write_text(f"{text}\n")
    (path / "CMakeCache.txt").write_text(
        f"CMAKE_HOME_DIRECTORY:INTERNAL={home}\n"
        f"TILEFINCH_PSP_VALIDATION_LOG:BOOL={validation}\n"
        "PSP_BROWSER_QUICKJS_PGO_GENERATE:BOOL=OFF\n"
        f"PSP_BROWSER_QUICKJS_PGO_USE:PATH={profile}\n"
        f"PSP_BROWSER_QUICKJS_OPT_FLAGS:STRING={flags}\n"
        f"PSP_BROWSER_PSP_TEXT_LIMIT_OVERRIDE:STRING={override}\n"
        f"TILEFINCH_PSP_TEXT_RATCHET:INTERNAL={ratchet}\n")


def main():
    source_root = Path(__file__).resolve().parents[1]
    checker = Path(sys.argv[1]) if len(sys.argv) > 1 else (
        source_root / "scripts/check-quickjs-pgo-size.sh")
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        sdk = root / "sdk"
        (sdk / "bin").mkdir(parents=True)
        (sdk / "bin/psp-size").write_text(FAKE_SIZE)
        (sdk / "bin/psp-size").chmod(0o755)
        profile = root / "profile"
        profile.mkdir()
        (profile / "PROVENANCE").write_text(
            "fingerprint=0123456789abcdef01234567\n"
            f"source-dir={root / 'checkout'}\ncommit=abc\n")
        env = dict(os.environ, PSPDEV=str(sdk))
        home = str(root / "checkout")
        cases = (
            # name, PGO tree .text, base tree .text, base overrides, status, text
            ("smaller", 90000, 100000, {}, 0, "PASS"),
            ("equal", 100000, 100000, {}, 0, "PASS"),
            ("over limit", 130000, 140000, {}, 1, "over the shipping limit"),
            ("over base", 110000, 100000, {}, 1, "larger than without a profile"),
            ("base profiled", 90000, 100000, {"profile": str(profile)}, 2,
             "must have none"),
            ("base validation", 90000, 100000, {"validation": "ON"}, 2,
             "not a shipping tree"),
            ("base experiment", 90000, 100000, {"flags": "-O3"}, 2,
             "plain shipping tree"),
            ("base other checkout", 90000, 100000,
             {"home": str(root / "other")}, 2, "different checkouts"),
            ("no ratchet", 90000, 100000, {"ratchet": ""}, 2, "reconfigure"),
            ("failed tool", "tool_error", 100000, {}, 2, "measure"),
            ("no text", "no_text", 100000, {}, 2, "measure"),
            ("malformed section", "malformed", 100000, {}, 2, "measure"),
            ("empty text", "zero", 100000, {}, 2, "measure"),
            ("invalid baseline", 90000, "no_text", {}, 2, "measure"),
        )
        for name, with_profile, without, base_options, status, text in cases:
            case = root / name.replace(" ", "-")
            write_tree(case / "pgo", home, with_profile, profile=str(profile),
                       override="16777216")
            base_options = dict(base_options)
            write_tree(case / "base", base_options.pop("home", home), without,
                       **base_options)
            result = subprocess.run(
                ["sh", str(checker), "--trees", str(case / "pgo"),
                 str(case / "base")],
                env=env, text=True, capture_output=True, timeout=10)
            if result.returncode != status or text not in (
                    result.stdout + result.stderr):
                raise AssertionError((name, result.returncode, result.stdout,
                                      result.stderr))
            if status == 0 and (
                    f"With the profile:       {with_profile} "
                    not in result.stdout
                    or "engine build 0123456789abcdef01234567"
                    not in result.stdout
                    or f"in checkout {home}\n" not in result.stdout):
                raise AssertionError((name, "split .text or provenance",
                                      result.stdout))
        result = subprocess.run(
            ["sh", str(checker), str(root / "missing")],
            env=env, text=True, capture_output=True, timeout=10)
        if result.returncode != 2 or "no PROVENANCE" not in result.stderr:
            raise AssertionError(("missing profile", result.returncode,
                                  result.stderr))
    print("QuickJS PGO size check passed")


if __name__ == "__main__":
    main()
