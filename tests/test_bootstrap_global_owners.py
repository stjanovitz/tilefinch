#!/usr/bin/env python3
"""Keep selected standards globals owned by one authored bootstrap module."""

from pathlib import Path
import re
import sys


def bootstrap_definitions(bootstrap: Path, name: str) -> list[tuple[str, int]]:
    pattern = re.compile(
        rf"(?:(?:globalThis|__tilefinchRealmGlobal)\.{re.escape(name)}\s*=|"
        rf"defineTrustedTypesGlobal\([\"']{re.escape(name)}[\"'])"
    )
    hits: list[tuple[str, int]] = []
    for source in bootstrap.glob("*.js"):
        text = source.read_text()
        hits.extend(
            (source.name, text.count("\n", 0, match.start()) + 1)
            for match in pattern.finditer(text)
        )
    return hits


def check_native_owner(root: Path, bootstrap: Path, name: str,
                       expected_file: str) -> list[str]:
    """A native installer names the global as a string literal and defines
    it on the global object; no bootstrap module may also assign it."""
    failures: list[str] = []
    source = root / "src" / expected_file
    text = source.read_text() if source.is_file() else ""
    if f'"{name}"' not in text or not re.search(
            r"JS_DefinePropertyValueStr\(\s*context,\s*global,", text):
        failures.append(f"{name}: expected a native definition in {expected_file}")
    hits = bootstrap_definitions(bootstrap, name)
    if hits:
        failures.append(f"{name}: native global also assigned in bootstrap {hits}")
    return failures


def main() -> int:
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path.cwd()
    bootstrap = root / "src" / "bootstrap"
    failures: list[str] = []
    for raw in (bootstrap / "global_owners.tsv").read_text().splitlines():
        if not raw or raw.startswith("#"):
            continue
        name, expected_file = raw.split("\t")
        if not expected_file.endswith(".js"):
            failures.extend(check_native_owner(root, bootstrap, name,
                                               expected_file))
            continue
        pattern = re.compile(
            rf"(?:(?:globalThis|__tilefinchRealmGlobal)\.{re.escape(name)}\s*=|"
            rf"defineTrustedTypesGlobal\([\"']{re.escape(name)}[\"'])"
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
                f"{name}: expected one definition in {expected_file}, got {hits}"
            )
    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1
    print("bootstrap-global-owners: all checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
