#!/usr/bin/env python3
"""Keep private investigations and development journals out of public sources."""

from pathlib import Path
import re
import subprocess
import sys


PRIVATE_EXACT_PATHS = {
    "docs/engineering/BROWSING_JOURNEYS.md",
    "docs/engineering/PERFORMANCE_LEDGER.md",
    "docs/engineering/MEMORY_EXPERIMENTS.md",
}
IGNORE_RULES = {
    "/.private-investigations/",
    "/docs/engineering/BROWSING_JOURNEYS.md",
    "/docs/engineering/PERFORMANCE_LEDGER.md",
    "/docs/engineering/MEMORY_EXPERIMENTS.md",
    "/docs/engineering/RELEASE_*_CHECKS.md",
}
PUBLIC_SUMMARIES = (
    "docs/engineering/NATIVE_TIER_INVESTIGATION.md",
)
PRIVATE_REFERENCE = re.compile(
    r"(?:/Users/[^/\s]+/|/home/[^/\s]+/|/private/tmp/|i/work/|"
    r"\b(?:claude|codex)/[\w-]+)"
)


def tracked_paths(root: Path) -> list[str]:
    result = subprocess.run(
        ["git", "-C", str(root), "ls-files", "-z", "--", "docs/engineering",
         ".private-investigations"],
        check=True,
        capture_output=True,
    )
    return [path for path in result.stdout.decode().split("\0") if path]


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
    private_references = []
    for name in PUBLIC_SUMMARIES:
        path = root / name
        if not path.exists():
            continue
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            if PRIVATE_REFERENCE.search(line):
                private_references.append(f"{name}:{number}")
    if private_references:
        print("public-tree hygiene: private references in public summaries:",
              file=sys.stderr)
        for reference in private_references:
            print(f"  {reference}", file=sys.stderr)
        return 1
    print("PASS: public tree contains no private investigation files or logs")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
