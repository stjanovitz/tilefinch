#!/usr/bin/env python3
"""Every TILEFINCH_* environment switch read in src/ or the vendored QuickJS
must be listed in docs/DIAGNOSTIC_SWITCHES.md, and every listed switch must
still be read. A switch read only through tilefinch_lab_getenv() is compiled
out of shipping PSP builds, so its row may not claim the device reads it."""
import re
import sys
from pathlib import Path

ROOT = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]
DOC = ROOT / "docs" / "DIAGNOSTIC_SWITCHES.md"
SOURCE_DIRS = ("src", "third_party/quickjs")
# Group 1 is set when the read goes through tilefinch_lab_getenv().
PATTERN = re.compile(r'(tilefinch_lab_)?getenv\("(TILEFINCH_[A-Z0-9_]+)"\)')


def source_files():
    paths = []
    for directory in SOURCE_DIRS:
        paths.extend(path for path in (ROOT / directory).rglob("*")
                     if path.suffix in (".c", ".h", ".inc"))
    return sorted(paths)


def switch_reads():
    """Map each switch to its (path, through tilefinch_lab_getenv) reads."""
    reads = {}
    for path in source_files():
        text = path.read_text(encoding="utf-8", errors="replace")
        for match in PATTERN.finditer(text):
            reads.setdefault(match[2], []).append(
                (path.relative_to(ROOT).as_posix(), match[1] is not None))
    return reads


def read_switches():
    return set(switch_reads())


def switch_sites():
    return {name: [path for path, _ in reads] for name, reads in switch_reads().items()}


def lab_only_switches():
    return {name for name, reads in switch_reads().items() if all(lab for _, lab in reads)}


def refresh_sites():
    sites = switch_sites()
    lab_only = lab_only_switches()
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
                elif name in lab_only and columns[4].strip() == "always":
                    columns[4] = " host / PSP validation "
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
    if "--update-switch" in sys.argv:
        index = sys.argv.index("--update-switch")
        name, scope, description = sys.argv[index + 1:index + 4]
        if name not in switch_sites() or name not in documented_switches():
            raise ValueError("switch must already be source-backed and documented")
        rows = []
        for line in DOC.read_text(encoding="utf-8").splitlines():
            if line.startswith(f"| `{name}` |"):
                columns = line.split("|")
                columns[4], columns[5] = f" {scope} ", f" {description} "
                line = "|".join(columns)
            rows.append(line)
        DOC.write_text("\n".join(rows) + "\n", encoding="utf-8")
    if "--register-switch" in sys.argv:
        index = sys.argv.index("--register-switch")
        name, scope, description = sys.argv[index + 1:index + 4]
        if not re.fullmatch(r"TILEFINCH_[A-Z0-9_]+", name):
            raise ValueError("invalid diagnostic switch name")
        sites = switch_sites()
        if name not in sites or name in documented_switches():
            raise ValueError("switch must be a new source-backed read")
        text = DOC.read_text(encoding="utf-8")
        row = f"| `{name}` | {len(sites[name])} | `{sites[name][0]}` | {scope} | {description} |\n"
        lines = text.splitlines(keepends=True)
        last = max(i for i, line in enumerate(lines) if line.startswith("| `TILEFINCH_"))
        lines.insert(last + 1, row)
        DOC.write_text("".join(lines), encoding="utf-8")
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
    lab_only = lab_only_switches()
    for line in DOC.read_text(encoding="utf-8").splitlines():
        if not line.startswith("| `TILEFINCH_"):
            continue
        columns = line.split("|")
        name = columns[1].strip(" `")
        if columns[2].strip() != str(len(sites[name])) or columns[3].strip(" `") != sites[name][0]:
            print(f"stale sites for {name}: run {Path(__file__).name} {ROOT} --refresh-sites")
            return 1
        if name in lab_only and columns[4].strip().startswith("always"):
            print(f"{name} is read only through tilefinch_lab_getenv, which shipping "
                  "builds compile out; its Device column cannot be 'always'")
            return 1
    print(f"diagnostic switch registry: {len(read)} switches documented")
    return 0


if __name__ == "__main__":
    sys.exit(main())
