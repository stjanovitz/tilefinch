#!/usr/bin/env python3
"""Keep selected standards globals owned by one authored bootstrap module."""

from pathlib import Path
import re
import sys


def main() -> int:
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path.cwd()
    bootstrap = root / "src" / "bootstrap"
    failures: list[str] = []
    for raw in (bootstrap / "global_owners.tsv").read_text().splitlines():
        if not raw or raw.startswith("#"):
            continue
        name, expected_file = raw.split("\t")
        pattern = re.compile(
            rf"globalThis\.{re.escape(name)}\s*="
        )
        hits: list[tuple[str, int]] = []
        for source in bootstrap.glob("*.js"):
            text = source.read_text()
            hits.extend(
                (source.name, text.count("\n", 0, match.start()) + 1)
                for match in pattern.finditer(text)
            )
        if len(hits) != 1 or hits[0][0] != expected_file:
            failures.append(
                f"{name}: expected one assignment in {expected_file}, got {hits}"
            )
    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1
    print("bootstrap-global-owners: all checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
