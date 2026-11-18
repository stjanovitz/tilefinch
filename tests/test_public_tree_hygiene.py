#!/usr/bin/env python3
"""Keep private investigations and development journals out of public sources,
keep docs/ARCHITECTURE.md a page of rules rather than mechanism, and keep it
and the engineering pages that hold its mechanism in the present tense."""

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

# docs/ARCHITECTURE.md gives each rule, its reason and a link; mechanism
# (thresholds, helper functions, diagnostics, site examples, history) lives
# in docs/engineering/ or in code comments. These ceilings hold it there:
# the page and its largest section measured about 8,000 and 480 words when
# they were set, so they leave room for a new rule but not for a mechanism.
ARCHITECTURE_PAGE = "docs/ARCHITECTURE.md"
ARCHITECTURE_PAGE_WORD_CEILING = 8500
ARCHITECTURE_SECTION_WORD_CEILING = 550
ARCHITECTURE_HEADING = re.compile(r"^#{1,6}\s")
# The engineering pages that hold the mechanism ARCHITECTURE.md summarizes.
# They get the domain and history checks below, not the word ceilings.
ENGINEERING_RULE_PAGES = (
    "docs/engineering/JS_RUNTIME.md",
    "docs/engineering/SCRIPT_MEMORY.md",
    "docs/engineering/STYLE_INVALIDATION.md",
    "docs/engineering/PAGE_NAVIGATIONS.md",
    "docs/engineering/UI_DURING_PAGE_WORK.md",
)
# A site's domain name. Standards bodies and the upstream source hosts of
# vendored code may be linked as references, and a library whose name looks
# like a domain (Socket.io, ASP.NET) may be named.
ARCHITECTURE_DOMAIN = re.compile(
    r"\b(?:[a-z0-9](?:[a-z0-9-]*[a-z0-9])?\.)+"
    r"(?:com|org|net|io|gov|edu|co|uk|de|jp|tv|me|app|dev|ai|info|news)\b",
    re.IGNORECASE)
ARCHITECTURE_DOMAIN_ALLOWED = re.compile(
    r"(?:^|\.)(?:whatwg\.org|w3\.org|ietf\.org|rfc-editor\.org|"
    r"tc39\.es|unicode\.org|"
    r"github\.com|githubusercontent\.com|gitlab\.com|codeberg\.org|"
    r"sourceforge\.net|bellard\.org)$|"
    r"^(?:socket\.io|asp\.net)$", re.IGNORECASE)
# Words that narrate change instead of describing the present system.
# "Previously", "no longer", "any more" and "used to" are left out: each has
# an ordinary present-tense use ("the previously healthy slot", "a permission
# that is no longer granted", "the table is used to pick a slot").
ARCHITECTURE_HISTORY = re.compile(
    r"\b(?:now|nowadays|formerly|originally|as before|until recently)\b",
    re.IGNORECASE)
ARCHITECTURE_HINT = (
    "docs/ARCHITECTURE.md states each rule, the reason for it and a link; "
    "put mechanism detail (thresholds, helper functions, diagnostics, "
    "examples, measurements) in an engineering page under docs/engineering/ "
    "or in a code comment, and link to it from the rule.")
ENGINEERING_RULE_HINT = (
    "engineering pages describe the present system: name no site unless it "
    "is needed, and put history (what changed, and when) in the commit "
    "message rather than in the page.")


def architecture_page_violations(text: str) -> list[str]:
    """Word ceilings, site domains and history words in ARCHITECTURE.md."""
    return page_rule_violations(text, word_ceilings=True)


def engineering_page_violations(text: str) -> list[str]:
    """Site domains and history words in an engineering rule page."""
    return page_rule_violations(text, word_ceilings=False)


def page_rule_violations(text: str, word_ceilings: bool) -> list[str]:
    found = []
    total = 0
    sections = [["(opening)", 1, 0]]
    fence = False
    for number, line in enumerate(text.splitlines(), 1):
        words = len(line.split())
        total += words
        if line.lstrip().startswith(("```", "~~~")):
            fence = not fence
        elif not fence and ARCHITECTURE_HEADING.match(line):
            sections.append([line.lstrip("#").strip(), number, 0])
            continue
        sections[-1][2] += words
        if fence:
            continue
        prose = re.sub(r"`[^`]*`", "", line)
        for match in ARCHITECTURE_DOMAIN.finditer(prose):
            if not ARCHITECTURE_DOMAIN_ALLOWED.search(match[0]):
                found.append(f"line {number}: site domain name "
                             f"'{match[0]}'")
        for match in ARCHITECTURE_HISTORY.finditer(prose):
            found.append(f"line {number}: history word '{match[0]}'")
    if not word_ceilings:
        return found
    if total > ARCHITECTURE_PAGE_WORD_CEILING:
        found.append(f"page has {total} words, ceiling "
                     f"{ARCHITECTURE_PAGE_WORD_CEILING}")
    for title, number, words in sections:
        if words > ARCHITECTURE_SECTION_WORD_CEILING:
            found.append(f"line {number}: section '{title}' has {words} "
                         f"words, ceiling {ARCHITECTURE_SECTION_WORD_CEILING}")
    return found



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
    architecture = root / ARCHITECTURE_PAGE
    if architecture.is_file():
        violations = architecture_page_violations(
            architecture.read_text(encoding="utf-8"))
        if violations:
            print(f"public-tree hygiene: {ARCHITECTURE_PAGE}:", file=sys.stderr)
            for violation in violations:
                print(f"  {violation}", file=sys.stderr)
            print(ARCHITECTURE_HINT, file=sys.stderr)
            return 1
    engineering_failed = False
    for name in ENGINEERING_RULE_PAGES:
        page = root / name
        if not page.is_file():
            continue
        violations = engineering_page_violations(
            page.read_text(encoding="utf-8"))
        if violations:
            print(f"public-tree hygiene: {name}:", file=sys.stderr)
            for violation in violations:
                print(f"  {violation}", file=sys.stderr)
            engineering_failed = True
    if engineering_failed:
        print(ENGINEERING_RULE_HINT, file=sys.stderr)
        return 1
    print("PASS: public tree contains no private investigation files or logs, "
          "and ARCHITECTURE.md and its engineering pages stay within their "
          "rules")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
