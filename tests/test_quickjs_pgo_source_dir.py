#!/usr/bin/env python3
"""Configure refuses a QuickJS profile trained in another checkout."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def check(cmake, module, profile, source_dir):
    result = subprocess.run(
        [cmake, f"-DPROFILE_DIR={profile}", f"-DSOURCE_DIR={source_dir}",
         "-P", str(module)],
        text=True, capture_output=True, timeout=10)
    # CMake wraps long messages; compare with the whitespace collapsed.
    return result.returncode, " ".join((result.stdout + result.stderr).split())


def main():
    source_root = Path(__file__).resolve().parents[1]
    cmake = sys.argv[1] if len(sys.argv) > 1 else "cmake"
    module = Path(sys.argv[2]) if len(sys.argv) > 2 else (
        source_root / "cmake/CheckQuickjsPgoSourceDir.cmake")
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory).resolve()
        checkout = root / "checkout"
        other = root / "other"
        link = root / "link"
        checkout.mkdir()
        other.mkdir()
        os.symlink(checkout, link)
        profile = root / "profile"
        profile.mkdir()
        header = "fingerprint=0123456789abcdef01234567\n"
        cases = (
            # name, recorded source-dir, configured source dir, status, texts
            ("matching", checkout, checkout, 0, ()),
            ("trained through the link", link, link, 0, ()),
            ("other checkout", other, checkout, 1, (
                f"was trained in the checkout {other}; this build is "
                f"configured from {checkout}.",
                "Train a profile in this checkout with "
                "scripts/train-quickjs-pgo.sh, or build from "
                f"{other}.")),
            # GCC hashes the path the compiler is given, not the folder.
            ("symlinked same folder", checkout, link, 1, (
                f"was trained in {checkout}; this build is configured from "
                f"{link}, the same folder by another path.",
                f"Configure from {checkout}, or train a profile from "
                f"{link}")),
            ("no source-dir", None, checkout, 0, (
                "CMake Warning",
                "does not record the checkout it was trained in",
                f"this one, {checkout}.")),
        )
        for name, trained, configured, status, texts in cases:
            provenance = header + "commit=abc\n"
            if trained is not None:
                provenance = header + f"source-dir={trained}\ncommit=abc\n"
            (profile / "PROVENANCE").write_text(provenance)
            code, output = check(cmake, module, profile, configured)
            if (code != 0) != (status != 0) or any(
                    text not in output for text in texts):
                raise AssertionError((name, code, output))
            if status == 0 and trained is not None and "CMake" in output:
                raise AssertionError((name, "unexpected diagnostic", output))
    print("QuickJS PGO source-dir checks passed")


if __name__ == "__main__":
    main()
