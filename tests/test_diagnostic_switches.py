#!/usr/bin/env python3
"""Every TILEFINCH_* environment switch read in src/ or the vendored QuickJS
must be listed in docs/DIAGNOSTIC_SWITCHES.md, and every listed switch must
still be read. A switch read only through tilefinch_lab_getenv() is compiled
out of shipping PSP builds, so its row may not claim the device reads it.

A switch read only in files the PSP browser EBOOT does not compile (the host
lab executables) is a "lab binary" switch. The EBOOT's files are taken from
the CMake source lists of psp-browser-script, tilefinch_core and the PSP
libraries the EBOOT links, plus the vendored QuickJS, and everything they
include. src/diagnostic_trace.h counts as outside the EBOOT: its reads sit
under TILEFINCH_NO_TRACE, which every PSP build defines."""
import re
import sys
from pathlib import Path

ROOT = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]
DOC = ROOT / "docs" / "DIAGNOSTIC_SWITCHES.md"
SOURCE_DIRS = ("src", "third_party/quickjs")
LAB_BINARY = "lab binary"
TRACE_HEADER = "src/diagnostic_trace.h"
# The EBOOT is psp-browser-script plus every target it links, transitively.
# Conditional source additions and links count too, so the set
# over-approximates shipping: that can only weaken the lab-binary rule.
EBOOT_TARGET = "psp-browser-script"
CMAKE_COMMAND = re.compile(
    r"\b(set|add_library|add_executable|target_sources|target_link_libraries)"
    r"\(\s*([\w-]+)")
CMAKE_SOURCE = re.compile(r"(?:(?<=\}/)|(?<![\w/]))src/[\w/.-]+\.(?:c|S)\b")
CMAKE_VARIABLE = re.compile(r"\$\{(\w+)\}")
CMAKE_WORD = re.compile(r"[\w-]+")
INCLUDE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.MULTILINE)
# Group 1 is set when the read goes through tilefinch_lab_getenv().
PATTERN = re.compile(r'(tilefinch_lab_)?getenv\("(TILEFINCH_[A-Z0-9_]+)"\)')


def source_files():
    paths = []
    for directory in SOURCE_DIRS:
        paths.extend(path for path in (ROOT / directory).rglob("*")
                     if path.suffix in (".c", ".h", ".inc"))
    return sorted(paths)


def cmake_block(text, start):
    """The text of the parenthesized CMake command opening at start."""
    depth = 0
    for index in range(text.index("(", start), len(text)):
        if text[index] == "(":
            depth += 1
        elif text[index] == ")":
            depth -= 1
            if depth == 0:
                return text[start:index]
    raise ValueError("unbalanced CMake command")


def eboot_files():
    """Repository-relative source files compiled into the PSP browser EBOOT,
    with every quoted include they reach."""
    variables, sources, links = {}, {}, {}
    for cmake in sorted((ROOT / "cmake").glob("*.cmake")) + [ROOT / "CMakeLists.txt"]:
        text = cmake.read_text(encoding="utf-8")
        for match in CMAKE_COMMAND.finditer(text):
            command, name = match[1], match[2]
            body = cmake_block(text, match.start())[match.end() - match.start():]
            files = set(CMAKE_SOURCE.findall(body))
            referenced = set(CMAKE_VARIABLE.findall(body))
            if command == "set":
                entry = variables.setdefault(name, [set(), set()])
            elif command == "target_link_libraries":
                links.setdefault(name, set()).update(CMAKE_WORD.findall(body))
                continue
            else:
                entry = sources.setdefault(name, [set(), set()])
            entry[0].update(files)
            entry[1].update(referenced)

    def expand(entry, seen):
        files = set(entry[0])
        for variable in entry[1] - seen:
            if variable in variables:
                files |= expand(variables[variable], seen | {variable})
        return files

    roots, targets, pending = set(), set(), [EBOOT_TARGET]
    while pending:
        target = pending.pop()
        if target in targets:
            continue
        targets.add(target)
        if target in sources:
            roots |= expand(sources[target], set())
        pending.extend(word for word in links.get(target, ()) if word in sources)
    roots.update(path.relative_to(ROOT).as_posix()
                 for path in (ROOT / "third_party/quickjs").glob("*.c"))
    for required in ("src/psp_script_main.c", "src/budget.c", "src/fetch.c"):
        if required not in roots:
            raise ValueError(f"could not find {required} in the PSP EBOOT "
                             "source lists in cmake/")
    seen = set()
    pending = [ROOT / root for root in roots]
    while pending:
        path = pending.pop()
        if not path.is_file():
            continue
        name = path.resolve().relative_to(ROOT).as_posix()
        if name in seen:
            continue
        seen.add(name)
        text = path.read_text(encoding="utf-8", errors="replace")
        for include in INCLUDE.findall(text):
            for base in (path.parent, ROOT / "src", ROOT / "include",
                         ROOT / "third_party/quickjs"):
                candidate = base / include
                if candidate.is_file():
                    pending.append(candidate)
                    break
    return seen


def lab_binary_switches():
    """Switches every read of which is outside the EBOOT, at least one of them
    in a lab executable's own source."""
    eboot = eboot_files() - {TRACE_HEADER}
    result = set()
    for name, sites in switch_sites().items():
        if any(site in eboot for site in sites):
            continue
        if any(site != TRACE_HEADER for site in sites):
            result.add(name)
    return result


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
    lab_binary = lab_binary_switches()
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
                elif name in lab_binary:
                    columns[4] = f" {LAB_BINARY} "
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
    lab_binary = lab_binary_switches()
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
        device = columns[4].strip()
        if name in lab_binary and device != LAB_BINARY:
            print(f"{name} is read only in lab executables the PSP EBOOT does not "
                  f"compile ({', '.join(sorted(set(sites[name])))}); its Device "
                  f"column must be '{LAB_BINARY}'")
            return 1
        if device == LAB_BINARY and name not in lab_binary:
            print(f"{name} is marked '{LAB_BINARY}' but is read in a file the "
                  "PSP EBOOT compiles")
            return 1
    print(f"diagnostic switch registry: {len(read)} switches documented")
    return 0


if __name__ == "__main__":
    sys.exit(main())
