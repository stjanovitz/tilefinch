#!/usr/bin/env python3
"""Stage the deterministic, separately signed Tilefinch voice component."""

from __future__ import annotations

import argparse
import shutil
from pathlib import Path


MODEL_INFO = b"TFVIv1\0\0\x00\x01\x00\x01"


def copy_file(source: Path, destination: Path) -> None:
    if not source.is_file():
        raise SystemExit(f"missing voice component input: {source}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--extra-map", required=True, type=Path)
    parser.add_argument("--small-map", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    root = args.source.resolve()
    output = args.output.resolve()
    if output.exists():
        shutil.rmtree(output)
    (output / "model").mkdir(parents=True)
    for relative in (
        "en-us/README",
        "en-us/feat.params",
        "en-us/mdef",
        "en-us/means",
        "en-us/noisedict",
        "en-us/sendump",
        "en-us/transition_matrices",
        "en-us/variances",
        "search/search.dict",
        "search/search.lm.bin",
        "extra-wide/search.dict",
        "extra-wide/search.lm.bin",
    ):
        copy_file(root / "psp-assets/voice-model" / relative,
                  output / "model" / relative)
    copy_file(args.extra_map, output / "model/extra-wide/search.dict.tilefinch")
    copy_file(args.small_map, output / "model/search/search.dict.tilefinch")
    copy_file(root / "psp-assets/voice-model/en-us/README",
              output / "LICENSES/ALPHA_CEPHEI_LICENSE.txt")
    copy_file(root / "third_party/notices/cmudict/LICENSE",
              output / "LICENSES/CMUDICT_LICENSE.txt")
    copy_file(root / "third_party/notices/cmudict/NOTICE.md",
              output / "LICENSES/CMUDICT_NOTICE.md")
    (output / "model-info.tfv").write_bytes(MODEL_INFO)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
