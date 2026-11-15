#!/usr/bin/env python3
"""Keep private investigations and development journals out of public sources."""

from pathlib import Path
import re
import subprocess
import sys


PRIVATE_EXACT_PATHS = {
    "docs/STATE_MACHINES.md",
    "docs/engineering/BROWSING_JOURNEYS.md",
    "docs/engineering/PERFORMANCE_LEDGER.md",
    "docs/engineering/MEMORY_EXPERIMENTS.md",
}
IGNORE_RULES = {
    "/docs/STATE_MACHINES.md",
    "/.private-investigations/",
    "/docs/engineering/BROWSING_JOURNEYS.md",
    "/docs/engineering/PERFORMANCE_LEDGER.md",
    "/docs/engineering/MEMORY_EXPERIMENTS.md",
    "/docs/engineering/RELEASE_*_CHECKS.md",
}
# Public prose and replay scripts must not point into a maintainer's machine
# or development workflow: every tracked Markdown file (docs, READMEs, the
# changelog) and every keyed input script under tests/input-scripts/.
PUBLIC_REFERENCE_SUFFIX = ".md"
PUBLIC_REFERENCE_SCRIPT_DIR = "tests/input-scripts/"
# The private ledgers live only in maintainers' ignored
# .private-investigations/ directory. Public files may name them only by that
# path, so no comment or link points at a document a clone cannot contain.
PRIVATE_LEDGER_NAME = re.compile(
    r"(?<!\.private-investigations/)"
    r"\b(?:BROWSING_JOURNEYS|PERFORMANCE_LEDGER|MEMORY_EXPERIMENTS|STATE_MACHINES)\b"
)
# Files that must spell the old public names: the ignore rules, the release
# rule that forbids them, and this test.
LEDGER_NAME_ALLOWED = {
    ".gitignore",
    "docs/RELEASE_PROCESS.md",
    "tests/test_public_tree_hygiene.py",
}
# Local user paths, scratch paths, the private work area and its merge tools,
# agent branch names, and "main checkout" (the maintainers' name for their
# development clone).
PRIVATE_REFERENCE = re.compile(
    r"(?:/Users/[^/\s]+/|/home/[^/\s]+/|/private/tmp/|i/work/|merge-tools/|"
    r"\b(?:claude|codex)/[\w-]+|(?i:\bmain checkout\b))"
)


def tracked_paths(root: Path) -> list[str]:
    result = subprocess.run(
        ["git", "-C", str(root), "ls-files", "-z", "--", "docs/engineering",
         ".private-investigations", *sorted(PRIVATE_EXACT_PATHS)],
        check=True,
        capture_output=True,
    )
    return [path for path in result.stdout.decode().split("\0") if path]


def all_tracked_files(root: Path) -> list[str]:
    try:
        result = subprocess.run(
            ["git", "-C", str(root), "ls-files", "-z"],
            check=True,
            capture_output=True,
        )
        return [path for path in result.stdout.decode().split("\0") if path]
    except (OSError, subprocess.CalledProcessError):
        # A source archive: every file present is published.
        return [
            str(path.relative_to(root))
            for path in root.rglob("*")
            if path.is_file()
            and not any(part.startswith((".git", "build", "_deps"))
                        for part in path.relative_to(root).parts)
        ]


def private_ledger_references(root: Path) -> list[str]:
    found = []
    for name in all_tracked_files(root):
        if name in LEDGER_NAME_ALLOWED:
            continue
        path = root / name
        try:
            text = path.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        if "PERFORMANCE_LEDGER" not in text \
                and "MEMORY_EXPERIMENTS" not in text \
                and "BROWSING_JOURNEYS" not in text \
                and "STATE_MACHINES" not in text:
            continue
        for number, line in enumerate(text.splitlines(), 1):
            if PRIVATE_LEDGER_NAME.search(line):
                found.append(f"{name}:{number}")
    return found


def is_public_reference_file(name: str) -> bool:
    if name.endswith(PUBLIC_REFERENCE_SUFFIX):
        return True
    return (name.startswith(PUBLIC_REFERENCE_SCRIPT_DIR)
            and name.endswith(".txt")
            and "/" not in name[len(PUBLIC_REFERENCE_SCRIPT_DIR):])


def private_references(root: Path) -> list[str]:
    found = []
    for name in all_tracked_files(root):
        if not is_public_reference_file(name):
            continue
        try:
            text = (root / name).read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        for number, line in enumerate(text.splitlines(), 1):
            if PRIVATE_REFERENCE.search(line):
                found.append(f"{name}:{number}")
    return found


def main() -> int:
    root = Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
    ignore_lines = {
        line.strip()
        for line in (root / ".gitignore").read_text(encoding="utf-8").splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    }
    missing_rules = sorted(IGNORE_RULES - ignore_lines)
    if missing_rules:
        print("public-tree hygiene: missing ignore rules:", file=sys.stderr)
        for rule in missing_rules:
            print(f"  {rule}", file=sys.stderr)
        return 1

    try:
        tracked = tracked_paths(root)
    except (OSError, subprocess.CalledProcessError):
        # A source archive has no index to inspect. Reject private log names
        # physically present in that archive instead.
        tracked = [
            str(path.relative_to(root))
            for path in (root / "docs" / "engineering").glob("*.md")
        ]
        tracked.extend(
            str(path.relative_to(root))
            for path in (root / ".private-investigations").rglob("*")
            if path.is_file()
        )
        tracked.extend(path for path in PRIVATE_EXACT_PATHS
                       if (root / path).is_file())
    rejected = []
    for path in tracked:
        private = (
            path.startswith(".private-investigations/")
            or path in PRIVATE_EXACT_PATHS
            or (path.startswith("docs/engineering/RELEASE_")
                and path.endswith("_CHECKS.md"))
        )
        # A deletion in the working tree describes the prospective commit,
        # even before it is staged. Do not mistake its old index entry for a
        # public file that will survive the change.
        if private and (root / path).exists():
            rejected.append(path)
    if rejected:
        print("public-tree hygiene: private files are tracked:",
              file=sys.stderr)
        for path in sorted(rejected):
            print(f"  {path}", file=sys.stderr)
        return 1
    references = private_references(root)
    if references:
        print("public-tree hygiene: private references in public docs or "
              "input scripts:", file=sys.stderr)
        for reference in references:
            print(f"  {reference}", file=sys.stderr)
        return 1
    ledger_references = private_ledger_references(root)
    if ledger_references:
        print("public-tree hygiene: private ledgers named outside "
              ".private-investigations/:", file=sys.stderr)
        for reference in ledger_references:
            print(f"  {reference}", file=sys.stderr)
        return 1
    print("PASS: public tree contains no private investigation files or logs")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
