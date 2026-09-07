#!/usr/bin/env python3
"""Every TILEFINCH_* environment switch read in src/ must be listed in
docs/DIAGNOSTIC_SWITCHES.md, and every listed switch must still be read."""
import re
import sys
from pathlib import Path

ROOT = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]
DOC = ROOT / "docs" / "DIAGNOSTIC_SWITCHES.md"
PATTERN = re.compile(r'getenv\("(TILEFINCH_[A-Z0-9_]+)"\)')


def read_switches():
    found = set()
    for path in (ROOT / "src").rglob("*"):
        if path.suffix not in (".c", ".h", ".inc"):
            continue
        found.update(PATTERN.findall(path.read_text(encoding="utf-8", errors="replace")))
    return found


def switch_sites():
    sites = {}
    for path in sorted((ROOT / "src").rglob("*")):
        if path.suffix not in (".c", ".h", ".inc"):
            continue
        for match in PATTERN.finditer(path.read_text(encoding="utf-8", errors="replace")):
            sites.setdefault(match[1], []).append(path.relative_to(ROOT).as_posix())
    return sites


def refresh_sites():
    sites = switch_sites()
    rows = []
    for line in DOC.read_text(encoding="utf-8").splitlines():
        if line.startswith("| `TILEFINCH_"):
            columns = line.split("|")
            name = columns[1].strip(" `")
            if name in sites:
                columns[2] = f" {len(sites[name])} "
                columns[3] = f" `{sites[name][0]}` "
                if set(sites[name]) == {"src/diagnostic_trace.h"}:
                    columns[4] = " host "
                line = "|".join(columns)
        rows.append(line)
    DOC.write_text("\n".join(rows) + "\n", encoding="utf-8")


def documented_switches():
    names = set()
    for line in DOC.read_text(encoding="utf-8").splitlines():
        if line.startswith("| `TILEFINCH_"):
            names.add(line.split("`")[1])
    return names


def main():
    if "--refresh-sites" in sys.argv[2:]:
        refresh_sites()
    read = read_switches()
    documented = documented_switches()
    undocumented = sorted(read - documented)
    stale = sorted(documented - read)
    for name in undocumented:
        print(f"undocumented switch: {name} (add a row to {DOC.relative_to(ROOT)})")
    for name in stale:
        print(f"stale switch: {name} is documented but no longer read")
    if undocumented or stale:
        return 1
    sites = switch_sites()
    for line in DOC.read_text(encoding="utf-8").splitlines():
        if not line.startswith("| `TILEFINCH_"):
            continue
        columns = line.split("|")
        name = columns[1].strip(" `")
        if columns[2].strip() != str(len(sites[name])) or columns[3].strip(" `") != sites[name][0]:
            print(f"stale sites for {name}: run {Path(__file__).name} {ROOT} --refresh-sites")
            return 1
    print(f"diagnostic switch registry: {len(read)} switches documented")
    return 0


if __name__ == "__main__":
    sys.exit(main())
